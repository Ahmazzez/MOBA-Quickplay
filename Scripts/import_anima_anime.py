"""Import Anima's anime replacement, textures and eight presentation clips.

Run in Unreal editor Python with legacy FBX import. Only writes the dedicated
/Game/Mistworld/Characters/AnimaAnime folder; no Blueprint or gameplay edits.
The manifest preserves the existing Anima schema and adds optional per-material
base_color_texture, opacity_texture, alpha_mode, opacity_mask_clip_value,
two_sided and emissive_strength fields. Texture paths are relative to SourceArt/AnimaAnime.
"""

import hashlib
import importlib.util
import json
import math
from pathlib import Path

import unreal


PROJECT = Path(unreal.Paths.project_dir())
SOURCE = PROJECT / "SourceArt" / "AnimaAnime"
DEST = "/Game/Mistworld/Characters/AnimaAnime"
OWNER = "Mistworld_AnimaAnime_v1"
REQUIRED_CLIPS = ("Idle", "Run", "Attack", "Thrust", "Dash", "Guard", "Ultimate", "Death")
spec = importlib.util.spec_from_file_location(
    "mist_anima_anime_fbx_helpers", PROJECT / "Scripts" / "import_alphonse.py")
helpers = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helpers)


def source_file(filename, suffixes):
    path = (SOURCE / filename).resolve()
    helpers.require(path.is_relative_to(SOURCE.resolve()), "Source file escapes AnimaAnime: " + filename)
    helpers.require(path.is_file() and path.suffix.lower() in suffixes,
                    "Missing or invalid AnimaAnime source file: " + str(path))
    return path


def owned_asset(path, expected_type):
    asset = helpers.load_if_exists(path)
    if asset is not None:
        helpers.require(isinstance(asset, expected_type), "Unexpected asset type at " + path)
        helpers.require(unreal.EditorAssetLibrary.get_metadata_tag(asset, "MistworldOwner") == OWNER,
                        "Refusing to replace unrelated asset " + path)
    return asset


def save_owned(asset):
    unreal.EditorAssetLibrary.set_metadata_tag(asset, "MistworldOwner", OWNER)
    helpers.save(asset)


def connect_property(expression, output, prop):
    helpers.require(unreal.MaterialEditingLibrary.connect_material_property(expression, output, prop),
                    "Failed to connect Anima material input: " + str(prop))


def input_expression(mat, prop, expression_type, x, y, exclude=None):
    library = unreal.MaterialEditingLibrary
    current = library.get_material_property_input_node(mat, prop)
    if isinstance(current, expression_type) and current != exclude:
        return current
    return library.create_material_expression(mat, expression_type, x, y)


def import_texture(filename, color=True):
    source = source_file(filename, {".png", ".tga", ".jpg", ".jpeg", ".bmp", ".tif", ".tiff"})
    # Include the relative path and role to avoid collisions between packed
    # source textures and separate opacity images with matching file stems.
    key = str(source.relative_to(SOURCE.resolve())).replace("\\", "/")
    digest = hashlib.sha256(key.encode("utf-8")).hexdigest()[:8]
    name = "T_Anima_" + helpers.safe_asset_name(source.stem) + "_" + digest
    if not color:
        name += "_Opacity"
    target = DEST + "/Textures/" + name
    owned_asset(target, unreal.Texture2D)
    task = unreal.AssetImportTask()
    for prop, value in {
        "filename": str(source), "destination_path": DEST + "/Textures",
        "destination_name": name, "automated": True, "replace_existing": True,
        "replace_existing_settings": True, "save": False,
    }.items():
        task.set_editor_property(prop, value)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    asset = unreal.load_asset(target)
    helpers.require(isinstance(asset, unreal.Texture2D), "Texture import failed: " + filename)
    asset.set_editor_property("srgb", color)
    asset.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_DEFAULT
                              if color else unreal.TextureCompressionSettings.TC_MASKS)
    asset.set_editor_property("compression_no_alpha", False)
    save_owned(asset)
    return asset


def material(style, textures):
    slot_name = style["name"]
    name = "M_" + helpers.safe_asset_name(slot_name)
    path = DEST + "/Materials/" + name
    mat = owned_asset(path, unreal.Material)
    library = unreal.MaterialEditingLibrary
    definition = hashlib.sha256(json.dumps(style, sort_keys=True).encode("utf-8")).hexdigest()
    base_texture = None
    if style.get("base_color_texture"):
        base_texture = textures[(style["base_color_texture"], True)]
    opacity_texture = None
    if style.get("opacity_texture"):
        opacity_texture = textures[(style["opacity_texture"], False)]
    alpha_mode = str(style.get("alpha_mode", "OPAQUE")).upper()
    helpers.require(alpha_mode in ("OPAQUE", "MASKED", "BLEND"), "Unknown alpha mode for " + slot_name)
    color = list(style.get("color", [1.0, 1.0, 1.0, 1.0]))
    if len(color) == 3:
        color.append(1.0)
    helpers.require(len(color) == 4 and all(math.isfinite(float(value)) for value in color),
                    "Invalid base color for " + slot_name)
    if mat is None:
        mat = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            name, DEST + "/Materials", unreal.Material, unreal.MaterialFactoryNew())
        helpers.require(isinstance(mat, unreal.Material), "Material creation failed: " + slot_name)
    if unreal.EditorAssetLibrary.get_metadata_tag(mat, "MistworldMaterialDefinition") != definition:
        # Update owned expressions in place. If a material changes from a flat
        # color to a texture, reconnect a new node and retain the unused node:
        # deleting loaded expressions can assert in UE 5.8.
        if base_texture is not None:
            base = input_expression(mat, unreal.MaterialProperty.MP_BASE_COLOR,
                                    unreal.MaterialExpressionTextureSample, -500, 0)
            base.set_editor_property("texture", base_texture)
            base.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)
            connect_property(base, "RGB", unreal.MaterialProperty.MP_BASE_COLOR)
        else:
            base = input_expression(mat, unreal.MaterialProperty.MP_BASE_COLOR,
                                    unreal.MaterialExpressionVectorParameter, -500, 0)
            base.set_editor_property("parameter_name", "BaseColor")
            base.set_editor_property("default_value", unreal.LinearColor(*[float(value) for value in color]))
            connect_property(base, "", unreal.MaterialProperty.MP_BASE_COLOR)
        emission = float(style.get("emissive_strength", 0.0))
        helpers.require(0.0 <= emission <= 0.3, "Anima emissive strength must stay within 0..0.3: " + slot_name)
        if emission > 0.0:
            glow = input_expression(mat, unreal.MaterialProperty.MP_EMISSIVE_COLOR,
                                    unreal.MaterialExpressionMultiply, -200, -450)
            strength = library.create_material_expression(mat, unreal.MaterialExpressionConstant, -500, -600)
            strength.set_editor_property("r", emission)
            helpers.require(library.connect_material_expressions(base, "RGB" if base_texture is not None else "", glow, "A"),
                            "Failed to connect Anima emissive color")
            helpers.require(library.connect_material_expressions(strength, "", glow, "B"),
                            "Failed to connect Anima emissive strength")
            connect_property(glow, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
        elif library.get_material_property_input_node(mat, unreal.MaterialProperty.MP_EMISSIVE_COLOR) is not None:
            black = input_expression(mat, unreal.MaterialProperty.MP_EMISSIVE_COLOR,
                                     unreal.MaterialExpressionConstant, -200, -450)
            black.set_editor_property("r", 0.0)
            connect_property(black, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
        if alpha_mode != "OPAQUE":
            prop = unreal.MaterialProperty.MP_OPACITY_MASK if alpha_mode == "MASKED" else unreal.MaterialProperty.MP_OPACITY
            opacity = base
            output = "A" if base_texture is not None else ""
            if opacity_texture is not None:
                opacity = input_expression(mat, prop, unreal.MaterialExpressionTextureSample, -500, -250, exclude=base)
                opacity.set_editor_property("texture", opacity_texture)
                opacity.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_MASKS)
                output = "R"
            elif base_texture is None:
                opacity = input_expression(mat, prop, unreal.MaterialExpressionConstant, -500, -250)
                opacity.set_editor_property("r", float(color[3]))
            connect_property(opacity, output, prop)
        for prop, key, default, y in (
            (unreal.MaterialProperty.MP_METALLIC, "metallic", 0.0, 200),
            (unreal.MaterialProperty.MP_ROUGHNESS, "roughness", 0.7, 350),
        ):
            value = float(style.get(key, default))
            helpers.require(0.0 <= value <= 1.0, "Invalid " + key + " for " + slot_name)
            node = input_expression(mat, prop, unreal.MaterialExpressionConstant, -500, y)
            node.set_editor_property("r", value)
            connect_property(node, "", prop)
        unreal.EditorAssetLibrary.set_metadata_tag(mat, "MistworldMaterialDefinition", definition)
    modes = {"OPAQUE": unreal.BlendMode.BLEND_OPAQUE, "MASKED": unreal.BlendMode.BLEND_MASKED,
             "BLEND": unreal.BlendMode.BLEND_TRANSLUCENT}
    mat.set_editor_property("blend_mode", modes[alpha_mode])
    mat.set_editor_property("two_sided", bool(style.get("two_sided", alpha_mode != "OPAQUE")))
    threshold = float(style.get("opacity_mask_clip_value", 0.333))
    helpers.require(0.0 <= threshold <= 1.0, "Invalid opacity mask threshold for " + slot_name)
    mat.set_editor_property("opacity_mask_clip_value", threshold)
    mat.set_editor_property("used_with_skeletal_mesh", True)
    library.recompile_material(mat)
    save_owned(mat)
    return mat


def validate_manifest(manifest):
    helpers.require(abs(float(manifest["height_cm"]) - 170.0) < 0.01, "Unexpected Anima normalized height")
    helpers.require(set(manifest["clips"]) == set(REQUIRED_CLIPS), "Manifest must contain the eight Anima clips")
    fps = int(manifest.get("fps", 24))
    helpers.require(fps > 0, "Invalid animation frame rate")
    source_file(manifest["mesh"], {".fbx"})
    names = set()
    for style in manifest["materials"]:
        name = str(style["name"])
        canonical = helpers.canonical_name(name)
        helpers.require(canonical and canonical not in names, "Duplicate or empty Anima material name: " + name)
        names.add(canonical)
        for key in ("base_color_texture", "opacity_texture"):
            if style.get(key):
                source_file(style[key], {".png", ".tga", ".jpg", ".jpeg", ".bmp", ".tif", ".tiff"})
    helpers.require(names, "Manifest has no materials")
    for name in REQUIRED_CLIPS:
        source_file(manifest["clips"][name]["file"], {".fbx"})


def main():
    manifest = json.loads((SOURCE / "anima-manifest.json").read_text(encoding="utf-8-sig"))
    validate_manifest(manifest)
    for folder in (DEST, DEST + "/Materials", DEST + "/Textures", DEST + "/Animations"):
        unreal.EditorAssetLibrary.make_directory(folder)
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    if world is not None:
        unreal.SystemLibrary.execute_console_command(world, "Interchange.FeatureFlags.Import.FBX 0")
    owned_asset(DEST + "/SK_Anima", unreal.SkeletalMesh)
    existing_skeleton = owned_asset(DEST + "/SK_Anima_Skeleton", unreal.Skeleton)
    mesh = helpers.import_one(str(source_file(manifest["mesh"], {".fbx"})), "SK_Anima", DEST,
        helpers.import_settings(unreal.FBXImportType.FBXIT_SKELETAL_MESH, manifest, existing_skeleton), unreal.SkeletalMesh)
    skeleton = mesh.get_editor_property("skeleton")
    helpers.require(isinstance(skeleton, unreal.Skeleton), "Missing imported Anima skeleton")
    save_owned(mesh)
    save_owned(skeleton)
    textures = {}
    for style in manifest["materials"]:
        for key, color in (("base_color_texture", True), ("opacity_texture", False)):
            filename = style.get(key)
            if filename and (filename, color) not in textures:
                textures[(filename, color)] = import_texture(filename, color)
    lookup = {helpers.canonical_name(style["name"]): material(style, textures) for style in manifest["materials"]}
    slots = helpers.apply_material_slots(mesh, lookup)
    helpers.require(len(slots) == len(manifest["materials"]), "Imported material count differs from exported model")
    helpers.require(all(slot.get_editor_property("material_interface") is not None
                        for slot in mesh.get_editor_property("materials")), "Anima materials were not saved")
    sequences, clips = {}, {}
    for name in REQUIRED_CLIPS:
        entry = manifest["clips"][name]
        asset_name = "A_Anima_" + name
        owned_asset(DEST + "/Animations/" + asset_name, unreal.AnimSequence)
        options = helpers.import_settings(unreal.FBXImportType.FBXIT_ANIMATION, manifest, skeleton)
        options.set_editor_property("override_animation_name", asset_name)
        sequence = helpers.import_one(str(source_file(entry["file"], {".fbx"})), asset_name,
                                      DEST + "/Animations", options, unreal.AnimSequence)
        helpers.require(sequence.get_editor_property("skeleton") == skeleton, "Wrong Anima clip skeleton: " + name)
        sequence.set_editor_property("enable_root_motion", False)
        sequence.set_editor_property("force_root_lock", False)
        duration = float(sequence.get_play_length())
        helpers.require(math.isfinite(duration) and duration > 0.1, "Empty Anima clip: " + name)
        if "duration" in entry:
            helpers.require(abs(duration - float(entry["duration"])) <= 1.01 / int(manifest.get("fps", 24)),
                            "Wrong imported duration for " + name)
        save_owned(sequence)
        sequences[name] = sequence
        clips[name] = {"asset": sequence.get_path_name(), "duration": duration}
    pose = unreal.AnimPoseExtensions.get_anim_pose_at_time(sequences["Idle"], 0.0, unreal.AnimPoseEvaluationOptions())
    bones = [str(name) for name in unreal.AnimPoseExtensions.get_bone_names(pose)]
    required_bones = manifest.get("required_bones", [])
    helpers.require(set(required_bones) <= set(bones), "Imported Anima rig is missing required bones")
    if manifest.get("root_name"):
        for name in REQUIRED_CLIPS:
            _, delta, _ = helpers.check_root_constant(sequences[name], manifest)
            clips[name]["root_translation_delta_cm"] = delta
    bounds = mesh.get_bounds()
    height = float(bounds.box_extent.z) * 2.0
    helpers.require(abs(height - 170.0) < 4.0, "Anima imported at wrong scale: " + str(height))
    report = {
        "success": True, "source": manifest["source"], "license": manifest["license"],
        "mesh": mesh.get_path_name(), "skeleton": skeleton.get_path_name(), "height_cm": height,
        "clips": clips, "materials": slots, "textures": [asset.get_path_name() for asset in textures.values()],
        "bone_names": bones, "skeletal_material_usage": True, "mesh_only_no_gameplay_changes": True,
        "legacy_assets_preserved": "/Game/Mistworld/Characters/Anima",
    }
    report_path = PROJECT / "Saved" / "AnimaAnimeImport.json"
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    unreal.log("MIST_ANIMA_ANIME_IMPORT_SUCCESS=" + str(report_path))


if __name__ == "__main__":
    main()
