"""Import the CC0 Pok stone golem with four prototype clips and elemental tint materials.

Only writes the owned /Game/Mistworld/Enemies/Golem folder. Run with Unreal
closed after compiling, using legacy FBX import and Scripts/fix_crypt_labels.py.
"""
import importlib.util
import json
from pathlib import Path
import unreal

HERE = Path(__file__).resolve().parent
PROJECT = Path(unreal.Paths.project_dir())
SOURCE = PROJECT / "SourceArt" / "Golem"
MANIFEST = SOURCE / "golem-manifest.json"
DEST = "/Game/Mistworld/Enemies/Golem"
OWNER = "Mistworld_Golem_v1"

spec = importlib.util.spec_from_file_location("mist_fbx_helpers", PROJECT / "Scripts" / "import_alphonse.py")
helpers = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helpers)


def texture(filename, normal=False):
    if not filename:
        return None
    source = SOURCE / filename
    helpers.require(source.is_file(), "Missing golem texture " + str(source))
    name = "T_Golem_" + helpers.safe_asset_name(source.stem)
    task = unreal.AssetImportTask()
    for key, value in {"filename": str(source), "destination_path": DEST + "/Textures",
                       "destination_name": name, "automated": True, "replace_existing": True,
                       "save": True}.items():
        task.set_editor_property(key, value)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    asset = unreal.load_asset(DEST + "/Textures/" + name)
    helpers.require(isinstance(asset, unreal.Texture2D), "Texture import failed " + filename)
    asset.set_editor_property("srgb", False)
    if normal:
        asset.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_NORMALMAP)
        asset.set_editor_property("flip_green_channel", True)
    helpers.save(asset)
    return asset


def material(name, role, color, normal_file=None, ao_file=None):
    name = helpers.safe_asset_name(name)
    path = DEST + "/Materials/M_Golem_" + name
    mat = helpers.load_if_exists(path)
    library = unreal.MaterialEditingLibrary
    if mat is None:
        mat = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            "M_Golem_" + name, DEST + "/Materials", unreal.Material, unreal.MaterialFactoryNew())
        helpers.require(mat is not None, "Failed to create " + path)
        if role in ("stone", "secondary", "carved", "glow"):
            base = library.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -400, 0)
            base.set_editor_property("parameter_name", "GlowColor" if role == "glow" else "RockColor")
            base.set_editor_property("default_value", unreal.LinearColor(*color[:3], 1.0))
            if role == "secondary":
                factor = library.create_material_expression(mat, unreal.MaterialExpressionConstant, -400, 150)
                factor.set_editor_property("r", 0.62)
                mult = library.create_material_expression(mat, unreal.MaterialExpressionMultiply, -150, 0)
                library.connect_material_expressions(base, "", mult, "A")
                library.connect_material_expressions(factor, "", mult, "B")
                base = mult
            if role == "glow":
                library.connect_material_property(base, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
        else:
            base = library.create_material_expression(mat, unreal.MaterialExpressionConstant3Vector, -400, 0)
            base.set_editor_property("constant", unreal.LinearColor(*color[:3], 1.0))
        library.connect_material_property(base, "", unreal.MaterialProperty.MP_BASE_COLOR)
        normal = texture(normal_file, True)
        if normal:
            sample = library.create_material_expression(mat, unreal.MaterialExpressionTextureSample, -400, -300)
            sample.set_editor_property("texture", normal)
            sample.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL)
            library.connect_material_property(sample, "RGB", unreal.MaterialProperty.MP_NORMAL)
        ambient = texture(ao_file)
        ao = None
        if ambient:
            ao = library.create_material_expression(mat, unreal.MaterialExpressionTextureSample, -400, -450)
            ao.set_editor_property("texture", ambient)
            ao.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
            library.connect_material_property(ao, "R", unreal.MaterialProperty.MP_AMBIENT_OCCLUSION)
        if role == "carved":
            glow = library.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -400, -650)
            glow.set_editor_property("parameter_name", "GlowColor")
            glow.set_editor_property("default_value", unreal.LinearColor(0.35, 0.68, 1.0, 1.0))
            factor = library.create_material_expression(mat, unreal.MaterialExpressionConstant, -400, -800)
            factor.set_editor_property("r", 0.08)
            mult = library.create_material_expression(mat, unreal.MaterialExpressionMultiply, -150, -650)
            library.connect_material_expressions(glow, "", mult, "A")
            library.connect_material_expressions(factor, "", mult, "B")
            library.connect_material_property(mult, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
        for prop, value, y in ((unreal.MaterialProperty.MP_ROUGHNESS, 0.85, 240),
                               (unreal.MaterialProperty.MP_METALLIC, 0.0, 360)):
            node = library.create_material_expression(mat, unreal.MaterialExpressionConstant, -300, y)
            node.set_editor_property("r", value)
            library.connect_material_property(node, "", prop)
        unreal.EditorAssetLibrary.set_metadata_tag(mat, "MistworldOwner", OWNER)
    else:
        helpers.require(unreal.EditorAssetLibrary.get_metadata_tag(mat, "MistworldOwner") == OWNER,
                        "Refusing to change unrelated material " + path)
    mat.set_editor_property("used_with_skeletal_mesh", True)
    library.recompile_material(mat)
    helpers.save(mat)
    return mat


def main():
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    helpers.require(manifest["height_cm"] == 200, "The runtime expects the normalized 2m golem")
    mesh = helpers.import_one(str(SOURCE / manifest["mesh"]), "SK_StoneGolem", DEST,
        helpers.import_settings(unreal.FBXImportType.FBXIT_SKELETAL_MESH, manifest), unreal.SkeletalMesh)
    skeleton = mesh.get_editor_property("skeleton")
    helpers.require(isinstance(skeleton, unreal.Skeleton), "Missing imported golem skeleton")
    # Preserve bright eyes, dark pupils and ivory teeth while changing the stone and magic details.
    styles = {
        "Goleling_Main": ("stone", (0.19, 0.22, 0.28, 1.0)),
        "Goleling_Secondary": ("secondary", (0.19, 0.22, 0.28, 1.0)),
        "Eye_White": ("fixed", (0.78, 0.88, 0.98, 1.0)),
        "Gold": ("glow", (0.35, 0.68, 1.0, 1.0)),
        "Eye_Black": ("fixed", (0.009, 0.011, 0.015, 1.0)),
        "Goleling_Teeth": ("fixed", (0.68, 0.59, 0.43, 1.0)),
    }
    if manifest.get("model") == "Pok":
        styles = {}
        for spec in manifest["materials"]:
            name = spec["name"]
            role = "carved" if "carved" in name.lower() else "secondary" if "poky" in name.lower() else "stone"
            textures = spec.get("textures", {})
            styles[name] = (role, (0.19, 0.22, 0.28, 1.0), textures.get("normal"), textures.get("ao"))
    lookup = {key: material(key, *value) for key, value in styles.items()}
    slots = list(mesh.get_editor_property("materials"))
    for index, slot in enumerate(slots):
        imported_name = str(slot.get_editor_property("imported_material_slot_name"))
        name = str(slot.get_editor_property("material_slot_name"))
        key = imported_name if imported_name in lookup else name
        helpers.require(key in lookup, "Unexpected imported golem material " + key)
        slot.set_editor_property("material_interface", lookup[key])
        slots[index] = slot
    mesh.set_editor_property("materials", slots)
    helpers.save(mesh)
    helpers.require(all(slot.get_editor_property("material_interface") is not None
                        for slot in mesh.get_editor_property("materials")), "Material slot assignment did not persist")
    helpers.save(skeleton)
    clips = {}
    for name in ("Idle", "Walk", "Attack", "Death"):
        source = manifest["clips"][name]
        sequence = helpers.import_one(str(SOURCE / source["file"]), "A_Golem_" + name,
            DEST + "/Animations", helpers.import_settings(unreal.FBXImportType.FBXIT_ANIMATION, manifest, skeleton),
            unreal.AnimSequence)
        sequence.set_editor_property("enable_root_motion", False)
        helpers.save(sequence)
        clips[name] = {"asset": sequence.get_path_name(), "duration": sequence.get_play_length()}
    bounds = mesh.get_bounds()
    height = bounds.box_extent.z * 2.0
    helpers.require(abs(height - 200.0) < 3.0, "Wrong imported golem height " + str(height))
    report = {"success": True, "source": manifest["source"], "license": manifest["license"],
              "mesh": mesh.get_path_name(), "height_cm": height, "clips": clips,
              "materials": list(styles), "skeletal_material_usage": True}
    (PROJECT / "Saved" / "StoneGolemImport.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    unreal.log("MIST_GOLEM_IMPORT_SUCCESS")


if __name__ == "__main__":
    main()
