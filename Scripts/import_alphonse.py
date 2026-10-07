"""Import the native Alphonse rig, clips and flat-colour greatsword into Mistworld.

Run inside Unreal's editor Python, after the C++ anim instance has been built:
  UnrealEditor-Cmd.exe Mistworld.uproject -run=pythonscript \
    -script=".../import_alphonse.py .../export-manifest.json" -unattended \
    -ExecCmds="Interchange.FeatureFlags.Import.FBX 0" -NullRHI

The manifest is produced by the Blender export. This script is limited to the
Alphonse asset folder and the existing player Blueprint's visual component;
capsule, movement, input and combat defaults are checked and preserved.
"""

import json
import math
import re
import sys
from pathlib import Path

import unreal


ASSET_DIR = "/Game/Mistworld/Characters/Alphonse"
ANIM_DIR = ASSET_DIR + "/Animations"
MATERIAL_DIR = ASSET_DIR + "/Materials"
MESH_NAME = "SK_Alphonse_Blockout"
SWORD_NAME = "SM_Alphonse_Greatsword"
CHARACTER_BP = "/Game/TopDown/Blueprints/BP_TopDownCharacter"
NATIVE_ANIM_CLASS = "/Script/Mistworld.MistAlphonseAnimInstance"
REQUIRED_CLIPS = (
    "Idle", "Run", "Auto", "Slash", "Quake", "SpinDash", "AbyssDash",
    "Throw", "Warcry", "Guard", "AbyssGuard", "Flash", "Death",
)


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def phase(name):
    unreal.log("MIST_ALPHONSE_PHASE=" + name)


def vector(values):
    return unreal.Vector(*[float(value) for value in values])


def rotator(values):
    return unreal.Rotator(pitch=float(values[0]), yaw=float(values[1]), roll=float(values[2]))


def vector_list(value):
    return [float(value.x), float(value.y), float(value.z)]


def transform_dict(value):
    return {
        "translation_cm": vector_list(value.translation),
        "rotation_quat": [float(value.rotation.x), float(value.rotation.y),
                          float(value.rotation.z), float(value.rotation.w)],
        "scale": vector_list(value.scale3d),
    }


def canonical_name(value):
    return re.sub(r"[^a-z0-9]", "", str(value).lower())


def safe_asset_name(value):
    return re.sub(r"[^A-Za-z0-9_]", "_", str(value)).strip("_")


def property_set(obj, name, value):
    """Fail on unsupported required settings rather than silently accepting defaults."""
    obj.set_editor_property(name, value)


def save(asset):
    require(unreal.EditorAssetLibrary.save_loaded_asset(asset, only_if_is_dirty=False),
            "Failed to save " + asset.get_path_name())


def load_if_exists(path):
    return unreal.load_asset(path) if unreal.EditorAssetLibrary.does_asset_exist(path) else None


def find_manifest():
    for argument in sys.argv[1:]:
        candidate = Path(argument.strip('"'))
        if candidate.suffix.lower() == ".json" and candidate.is_file():
            return candidate.resolve()
    candidates = (
        Path(__file__).with_name("rig-manifest.json"),
        Path(__file__).parents[1] / "SourceArt" / "Alphonse" / "rig-manifest.json",
        Path(__file__).parents[3] / "outputs" / "Alphonse-Game-Rig" / "rig-manifest.json",
    )
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    raise RuntimeError("Pass the Blender rig-manifest.json as the script argument.")


def source_file(value, manifest_path):
    candidate = Path(value)
    if not candidate.is_absolute():
        candidate = manifest_path.parent / candidate
    candidate = candidate.resolve()
    require(candidate.is_file(), "FBX input does not exist: " + str(candidate))
    require(candidate.suffix.lower() == ".fbx", "Expected FBX input: " + str(candidate))
    return str(candidate)


def import_settings(kind, manifest, skeleton=None):
    options = unreal.FbxImportUI()
    property_set(options, "automated_import_should_detect_type", False)
    property_set(options, "original_import_type", kind)
    property_set(options, "mesh_type_to_import", kind)
    property_set(options, "override_full_name", True)
    property_set(options, "import_materials", False)
    property_set(options, "import_textures", False)
    is_static = kind == unreal.FBXImportType.FBXIT_STATIC_MESH
    is_animation = kind == unreal.FBXImportType.FBXIT_ANIMATION
    property_set(options, "import_as_skeletal", not is_static)
    property_set(options, "import_mesh", not is_animation)
    property_set(options, "import_animations", is_animation)
    property_set(options, "create_physics_asset", False)
    if skeleton is not None:
        property_set(options, "skeleton", skeleton)
    data = options.get_editor_property(
        "static_mesh_import_data" if is_static else
        "anim_sequence_import_data" if is_animation else "skeletal_mesh_import_data")
    property_set(data, "convert_scene", True)
    property_set(data, "convert_scene_unit", True)
    # Character native forward is -Y; the weapon is already modelled along +X.
    # Applying the character's forward conversion to the static weapon turns
    # its blade onto -Y, so retain its native static axes by default.
    property_set(data, "force_front_x_axis", bool(manifest.get(
        "sword_force_front_x_axis" if is_static else "force_front_x_axis", not is_static)))
    property_set(data, "import_uniform_scale", float(manifest.get("import_uniform_scale", 1.0)))
    property_set(data, "import_translation", unreal.Vector(0.0, 0.0, 0.0))
    property_set(data, "import_rotation", unreal.Rotator(0.0, 0.0, 0.0))
    if is_animation:
        property_set(data, "animation_length", unreal.FBXAnimationLengthImportType.FBXALIT_EXPORTED_TIME)
        property_set(data, "use_default_sample_rate", False)
        property_set(data, "custom_sample_rate", int(manifest.get("fps", 30)))
        # Reconstruct local animation transforms from converted globals. The
        # preserve-local path omits FBX joint-axis post conversion and therefore
        # disagrees with the mesh reference basis for this Blender skeleton.
        property_set(data, "preserve_local_transform", False)
        property_set(data, "import_custom_attribute", False)
        property_set(data, "import_bone_tracks", True)
    else:
        property_set(data, "normal_import_method", unreal.FBXNormalImportMethod.FBXNIM_IMPORT_NORMALS)
        # The flat-colour blockout has no UV/normal-map contract. Built-in
        # tangent generation preserves exported faceted normals without asking
        # MikkTSpace to infer tangent frames from missing texture coordinates.
        property_set(data, "normal_generation_method", unreal.FBXNormalGenerationMethod.BUILT_IN)
        property_set(data, "reorder_material_to_fbx_order", True)
        if is_static:
            property_set(data, "combine_meshes", True)
            property_set(data, "auto_generate_collision", False)
            property_set(data, "transform_vertex_to_absolute", True)
        else:
            property_set(data, "update_skeleton_reference_pose", False)
            property_set(data, "use_t0_as_ref_pose", False)
            property_set(data, "preserve_smoothing_groups", True)
            property_set(data, "import_morph_targets", False)
            property_set(data, "import_meshes_in_bone_hierarchy", True)
    return options


def import_one(filename, name, destination, options, expected_class):
    target = destination + "/" + name
    previous = load_if_exists(target)
    if previous is not None and isinstance(previous, expected_class):
        # UFbxFactory dispatches existing assets through atomic reimport. That
        # path reads the existing asset's import data and ignores a task's new
        # transform options, despite replace_existing_settings=True. Update
        # the owned asset import data before requesting replacement.
        imported_data = previous.get_editor_property("asset_import_data")
        data_name = "static_mesh_import_data" if isinstance(previous, unreal.StaticMesh) else \
            "anim_sequence_import_data" if isinstance(previous, unreal.AnimSequence) else \
            "skeletal_mesh_import_data"
        requested_data = options.get_editor_property(data_name)
        for key in ("convert_scene", "convert_scene_unit", "force_front_x_axis",
                    "import_uniform_scale", "import_translation", "import_rotation"):
            imported_data.set_editor_property(key, requested_data.get_editor_property(key))
        if isinstance(previous, unreal.AnimSequence):
            for key in ("animation_length", "use_default_sample_rate", "custom_sample_rate",
                        "preserve_local_transform", "import_custom_attribute", "import_bone_tracks"):
                imported_data.set_editor_property(key, requested_data.get_editor_property(key))
        else:
            for key in ("normal_import_method", "normal_generation_method"):
                imported_data.set_editor_property(key, requested_data.get_editor_property(key))
    task = unreal.AssetImportTask()
    for key, value in {
        "filename": filename, "destination_path": destination,
        "destination_name": name, "automated": True, "replace_existing": True,
        "replace_existing_settings": True, "save": False, "options": options,
        "factory": unreal.FbxFactory(),
    }.items():
        property_set(task, key, value)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    asset = unreal.load_asset(target)
    if not isinstance(asset, expected_class):
        imported = [unreal.load_asset(path) for path in task.get_editor_property("imported_object_paths")]
        candidates = [candidate for candidate in imported if isinstance(candidate, expected_class)]
        require(len(candidates) == 1,
                "Expected one " + str(expected_class) + " for " + filename + "; got " + str(imported))
        asset = candidates[0]
        require(unreal.EditorAssetLibrary.rename_asset(asset.get_path_name(), target),
                "Could not normalize imported asset name to " + target)
        asset = unreal.load_asset(target)
    require(isinstance(asset, expected_class), "Missing imported asset " + target)
    return asset


def material_specs(manifest):
    specs = manifest.get("materials", {})
    if isinstance(specs, list):
        specs = {entry["name"]: entry for entry in specs}
    require(bool(specs), "Manifest must supply flat-colour material values.")
    return specs


def make_materials(manifest):
    lookup, report = {}, []
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    library = unreal.MaterialEditingLibrary
    for slot_name, spec in material_specs(manifest).items():
        name = "M_Alphonse_" + safe_asset_name(slot_name)
        path = MATERIAL_DIR + "/" + name
        mat = load_if_exists(path)
        reused = mat is not None
        phase("material:" + slot_name + (":reuse" if reused else ":create"))
        if mat is None:
            mat = asset_tools.create_asset(name, MATERIAL_DIR, unreal.Material, unreal.MaterialFactoryNew())
        require(isinstance(mat, unreal.Material), "Expected Material at " + path)
        # Persist the skeletal shader permutation before saving the asset.
        # Automatic editor usage discovery otherwise fixes previews only in
        # memory and leaves a fresh PIE launch falling back to DefaultMaterial.
        usage_changed = not bool(mat.get_editor_property("used_with_skeletal_mesh"))
        if usage_changed:
            mat.set_editor_property("used_with_skeletal_mesh", True)
        color = list(spec.get("base_color", spec.get("color", [])))
        require(len(color) >= 3 and all(math.isfinite(float(v)) for v in color),
                "Invalid material colour for " + slot_name)
        if reused:
            # Deleting expressions from an editor-loaded/preview-rooted
            # Material asserts in UE 5.8. Existing owned materials have this
            # exact three-constant graph; verify and reuse them without graph
            # mutation. A missing skeletal usage flag is repaired and saved.
            base = library.get_material_property_input_node(mat, unreal.MaterialProperty.MP_BASE_COLOR)
            require(isinstance(base, unreal.MaterialExpressionConstant3Vector),
                    "Existing material has an unexpected base-colour graph: " + path)
            current = base.get_editor_property("constant")
            require(max(abs(float(a) - float(b)) for a, b in zip(
                (current.r, current.g, current.b), color[:3])) <= 1.0e-6,
                "Existing material base colour differs from manifest: " + path)
        else:
            base = library.create_material_expression(mat, unreal.MaterialExpressionConstant3Vector, -400, -150)
            base.set_editor_property("constant", unreal.LinearColor(float(color[0]), float(color[1]),
                                                                    float(color[2]), 1.0))
            library.connect_material_property(base, "", unreal.MaterialProperty.MP_BASE_COLOR)
        values = {}
        for key, prop, y, default in (
            ("roughness", unreal.MaterialProperty.MP_ROUGHNESS, 0, 0.72),
            ("metallic", unreal.MaterialProperty.MP_METALLIC, 150, 0.0),
        ):
            value = float(spec.get(key, default))
            require(0.0 <= value <= 1.0, "Invalid " + key + " for " + slot_name)
            if reused:
                scalar = library.get_material_property_input_node(mat, prop)
                require(isinstance(scalar, unreal.MaterialExpressionConstant)
                        and abs(float(scalar.get_editor_property("r")) - value) <= 1.0e-6,
                        "Existing material " + key + " differs from manifest: " + path)
            else:
                scalar = library.create_material_expression(mat, unreal.MaterialExpressionConstant, -400, y)
                scalar.set_editor_property("r", value)
                library.connect_material_property(scalar, "", prop)
            values[key] = value
        if reused:
            require(bool(mat.get_editor_property("two_sided")) == bool(spec.get("two_sided", False)),
                    "Existing material culling differs from manifest: " + path)
        else:
            mat.set_editor_property("two_sided", bool(spec.get("two_sided", False)))
        if not reused or usage_changed:
            library.recompile_material(mat)
            save(mat)
        require(bool(mat.get_editor_property("used_with_skeletal_mesh")),
                "Material has no persisted skeletal usage: " + path)
        lookup[canonical_name(slot_name)] = mat
        for alias in spec.get("aliases", []):
            lookup[canonical_name(alias)] = mat
        report.append({"slot": slot_name, "asset": path, "linear_base_color": color[:3],
                       "reused_verified_material": reused,
                       "used_with_skeletal_mesh": True, **values})
    return lookup, report


def apply_material_slots(asset, lookup):
    attribute = "materials" if isinstance(asset, unreal.SkeletalMesh) else "static_materials"
    slots = list(asset.get_editor_property(attribute))
    require(slots, "Imported mesh has no material slots: " + asset.get_path_name())
    report = []
    for index, slot in enumerate(slots):
        imported_name = str(slot.get_editor_property("imported_material_slot_name"))
        slot_name = str(slot.get_editor_property("material_slot_name"))
        material = lookup.get(canonical_name(imported_name)) or lookup.get(canonical_name(slot_name))
        require(material is not None, "No manifest material for " + repr(imported_name)
                + " / " + repr(slot_name) + " on " + asset.get_path_name())
        slot.set_editor_property("material_interface", material)
        slots[index] = slot
        report.append({"index": index, "slot_name": slot_name, "imported_name": imported_name,
                       "material": material.get_path_name()})
    asset.set_editor_property(attribute, slots)
    save(asset)
    return report


def asset_bounds(asset):
    bounds = asset.get_bounds()
    return {"origin_cm": vector_list(bounds.origin), "half_extent_cm": vector_list(bounds.box_extent),
            "dimensions_cm": [float(v) * 2.0 for v in vector_list(bounds.box_extent)]}


def check_imported_bones(mesh, idle, manifest):
    pose = unreal.AnimPoseExtensions.get_anim_pose_at_time(idle, 0.0, unreal.AnimPoseEvaluationOptions())
    names = [str(name) for name in unreal.AnimPoseExtensions.get_bone_names(pose)]
    specifications = manifest.get("bones", {})
    require(set(specifications) <= set(names),
            "Imported rig lost bones: " + ", ".join(sorted(set(specifications) - set(names))))
    result = []
    for name, spec in specifications.items():
        reference = unreal.AnimPoseExtensions.get_ref_bone_pose(pose, name, unreal.AnimPoseSpaces.WORLD)
        position = vector_list(reference.translation)
        parent = spec.get("parent")
        entry = {"name": name, "reference": transform_dict(reference), "expected_parent": parent}
        if parent:
            parent_reference = unreal.AnimPoseExtensions.get_ref_bone_pose(pose, parent, unreal.AnimPoseSpaces.WORLD)
            imported_distance = math.dist(position, vector_list(parent_reference.translation))
            native_distance = math.dist(spec["head"], specifications[parent]["head"]) * 100.0
            require(abs(imported_distance - native_distance) <= 0.15,
                    "FBX scale or hierarchy changed for " + name + ": "
                    + str(imported_distance) + "cm instead of " + str(native_distance))
            entry["parent_head_distance_cm"] = imported_distance
        result.append(entry)
    bounds = asset_bounds(mesh)
    expected_height = float(manifest.get("expected_height_cm", 200.0))
    require(abs(bounds["dimensions_cm"][2] - expected_height) <= 1.0,
            "Imported Alphonse height is " + str(bounds["dimensions_cm"][2])
            + "cm, expected " + str(expected_height))
    return {"imported_bone_count": len(names), "expected_native_bone_count": len(specifications),
            "additional_bones": sorted(set(names) - set(specifications)), "reference_bones": result}


def import_sword_calibrated(manifest, manifest_path):
    """Keep the static blade's +X contract despite FBX scene-axis conversion.

    The pivot is the hand grip and the long blade extends towards the positive
    dominant axis. Reimport only if the FBX conversion rotated that axis. This
    modifies import rotation, never the hand animation or capsule transform.
    """
    filename = source_file(manifest["sword_fbx"], manifest_path)
    options = import_settings(unreal.FBXImportType.FBXIT_STATIC_MESH, manifest)
    sword = import_one(filename, SWORD_NAME, ASSET_DIR, options, unreal.StaticMesh)
    before = asset_bounds(sword)
    dimensions = before["dimensions_cm"]
    axis = max(range(3), key=lambda index: dimensions[index])
    require(dimensions[axis] > max(v for index, v in enumerate(dimensions) if index != axis) * 2.0,
            "Sword bounds cannot identify a dominant blade axis.")
    sign = 1.0 if before["origin_cm"][axis] >= 0.0 else -1.0
    rotation = [0.0, 0.0, 0.0]
    if axis == 0 and sign < 0.0:
        rotation[1] = 180.0
    elif axis == 1:
        rotation[1] = -90.0 if sign > 0.0 else 90.0
    elif axis == 2:
        rotation[0] = -90.0 if sign > 0.0 else 90.0
    if any(rotation):
        options.get_editor_property("static_mesh_import_data").set_editor_property(
            "import_rotation", rotator(rotation))
        sword = import_one(filename, SWORD_NAME, ASSET_DIR, options, unreal.StaticMesh)
    after = asset_bounds(sword)
    require(after["dimensions_cm"][0] > max(after["dimensions_cm"][1:]) * 2.0
            and after["origin_cm"][0] > 0.0,
            "Sword import rotation failed to align blade along local +X: " + str(after))
    return sword, {"before": before, "applied_import_rotation_deg": rotation,
                   "after": after, "blade_axis": "+X", "pivot": "grip"}


def import_animations(manifest, manifest_path, skeleton):
    entries = manifest.get("animations", {})
    if isinstance(entries, list):
        entries = {entry["name"]: entry for entry in entries}
    normalized = {}
    for key, spec in entries.items():
        short = str(key).removeprefix("A_Alphonse_")
        normalized[short] = {"fbx": spec} if isinstance(spec, str) else dict(spec)
    require(set(REQUIRED_CLIPS) <= set(normalized), "Missing clips: "
            + ", ".join(sorted(set(REQUIRED_CLIPS) - set(normalized))))
    sequences, report = {}, []
    fps = int(manifest.get("fps", 30))
    for short_name in REQUIRED_CLIPS:
        phase("animation:" + short_name)
        spec = normalized[short_name]
        name = "A_Alphonse_" + short_name
        options = import_settings(unreal.FBXImportType.FBXIT_ANIMATION, manifest, skeleton)
        options.set_editor_property("override_animation_name", name)
        sequence = import_one(source_file(spec["fbx"], manifest_path), name, ANIM_DIR,
                              options, unreal.AnimSequence)
        require(sequence.get_editor_property("skeleton") == skeleton,
                "Clip imported against another skeleton: " + name)
        sequence.set_editor_property("enable_root_motion", False)
        sequence.set_editor_property("force_root_lock", False)
        length = float(unreal.AnimationLibrary.get_sequence_length(sequence))
        require(length > 0.0 and math.isfinite(length), "Empty clip: " + name)
        expected_duration = spec.get("duration", spec.get("duration_seconds"))
        if expected_duration is not None:
            require(abs(length - float(expected_duration)) <= 1.01 / fps,
                    "Unexpected clip length for " + name + ": " + str(length))
        root_samples, root_delta, bone_names = check_root_constant(sequence, manifest)
        sequences[short_name] = sequence
        save(sequence)
        report.append({"name": name, "asset": sequence.get_path_name(), "duration_seconds": length,
                       "loop": bool(spec.get("loop", short_name in ("Idle", "Run"))),
                       "root_translation_delta_cm": root_delta,
                       "root_samples": root_samples, "bone_count": len(bone_names)})
    return sequences, report


def check_root_constant(sequence, manifest):
    length = float(unreal.AnimationLibrary.get_sequence_length(sequence))
    options = unreal.AnimPoseEvaluationOptions()
    root_name = manifest.get("root_name", "root")
    samples, bone_names = [], []
    for factor in (0.0, 0.125, 0.25, 0.375, 0.5, 0.625, 0.75, 0.875, 1.0):
        pose = unreal.AnimPoseExtensions.get_anim_pose_at_time(sequence, length * factor, options)
        if not bone_names:
            bone_names = [str(name) for name in unreal.AnimPoseExtensions.get_bone_names(pose)]
            require(root_name in bone_names, "Root bone " + root_name + " missing in " + sequence.get_name())
        root = unreal.AnimPoseExtensions.get_bone_pose(pose, root_name, unreal.AnimPoseSpaces.WORLD)
        samples.append({"time": length * factor, **transform_dict(root)})
    initial = samples[0]
    delta = max(math.dist(initial["translation_cm"], item["translation_cm"]) for item in samples)
    rotation_delta = max(min(math.dist(initial["rotation_quat"], item["rotation_quat"]),
                             math.dist(initial["rotation_quat"], [-v for v in item["rotation_quat"]]))
                         for item in samples)
    scale_delta = max(math.dist(initial["scale"], item["scale"]) for item in samples)
    require(delta <= 0.05 and rotation_delta <= 0.001 and scale_delta <= 0.001,
            "Root animation would fight actor movement in " + sequence.get_name()
            + ": translation=" + str(delta) + "cm quaternion=" + str(rotation_delta))
    return samples, delta, bone_names


def create_weapon_socket(mesh, idle, manifest):
    settings = manifest.get("weapon_socket", {})
    bone_name = str(settings.get("bone", "weapon_r"))
    pose = unreal.AnimPoseExtensions.get_anim_pose_at_time(idle, 0.0, unreal.AnimPoseEvaluationOptions())
    bone_names = [str(name) for name in unreal.AnimPoseExtensions.get_bone_names(pose)]
    require(bone_name in bone_names, "Weapon attachment bone missing: " + bone_name)
    bone = unreal.AnimPoseExtensions.get_bone_pose(pose, bone_name, unreal.AnimPoseSpaces.WORLD)
    bone_scale = vector_list(bone.scale3d)
    require(all(math.isfinite(value) and value > 1.0e-6 for value in bone_scale),
            "Cannot compensate an invalid weapon bone scale: " + str(bone_scale))
    socket_scale = [1.0 / value for value in bone_scale]
    existing = None
    for index in range(mesh.num_sockets()):
        socket = mesh.get_socket_by_index(index)
        if str(socket.get_editor_property("socket_name")) == "MistWeaponGrip":
            existing = socket
            break
    socket = existing or unreal.new_object(unreal.SkeletalMeshSocket, outer=mesh, name="MistWeaponGrip")
    socket.set_socket_parent(mesh, unreal.Name(bone_name))
    translation = settings.get("translation_cm", settings.get("translation", [0.0, 0.0, 0.0]))
    rotation = settings.get("rotation_deg", settings.get("rotation", [0.0, 0.0, 0.0]))
    authored_rotation = rotator(rotation).quaternion()
    # FbxMainImport.cpp ConvertScene sets JointOrientationMatrix from FBX XYZ
    # Euler(-90,-90,0) for ForceFrontXAxis. FFbxDataConverter::ConvertRotToQuat
    # maps(qx,qy,qz,qw) to(qx,-qy,qz,-qw), giving UE joint postrotation
    # (.5,-.5,.5,.5). Its inverse keeps this separately imported +X blade in
    # the original Blender weapon_r frame after corrected animation import.
    joint_inverse = unreal.Quat(-0.5, 0.5, -0.5, 0.5) if bool(
        manifest.get("force_front_x_axis", True)) else unreal.Quat(0, 0, 0, 1)
    socket_rotation = joint_inverse * authored_rotation
    local = unreal.Transform(location=vector(translation), rotation=socket_rotation.rotator(),
                             scale=vector(socket_scale))
    socket.set_socket_local_transform(local)
    if existing is None:
        # SocketName is intentionally read-only to Python. AddSocket assigns
        # a unique default Socket name; the editor subsystem's supported
        # RenameSocket API then changes that name transactionally.
        mesh.add_socket(socket, False)
        old_name = socket.get_editor_property("socket_name")
        require(str(old_name) != "None", "SkeletalMesh.add_socket did not assign a socket name")
        subsystem = unreal.get_editor_subsystem(unreal.SkeletalMeshEditorSubsystem)
        require(subsystem.rename_socket(mesh, old_name, unreal.Name("MistWeaponGrip")),
                "Failed to rename the added weapon socket")
    require(str(socket.get_editor_property("socket_name")) == "MistWeaponGrip"
            and str(socket.get_editor_property("bone_name")) == bone_name,
            "Weapon socket did not retain the required name and bone parent")
    save(mesh)
    composed_scale = [a * b for a, b in zip(bone_scale, vector_list(
        socket.get_socket_local_transform().scale3d))]
    require(max(abs(value - 1.0) for value in composed_scale) <= 1.0e-6,
            "Weapon socket does not cancel FBX unit bone scaling: " + str(composed_scale))
    axis = unreal.MathLibrary.quat_rotate_vector(bone.rotation * local.rotation, unreal.Vector(1, 0, 0))
    require(math.dist(vector_list(axis), [0.750018, 0.0, 0.661417]) < 0.002,
            "Corrected FBX socket does not preserve native idle blade direction: " + str(vector_list(axis)))
    return {"name": "MistWeaponGrip", "bone": bone_name,
            "local_transform": transform_dict(socket.get_socket_local_transform()),
            "idle_bone_transform": transform_dict(bone), "idle_blade_axis_mesh": vector_list(axis),
            "unit_scale_compensation": socket_scale, "composed_weapon_scale": composed_scale,
            "joint_axis_compensation_quat": [float(joint_inverse.x), float(joint_inverse.y),
                                             float(joint_inverse.z), float(joint_inverse.w)]}


def character_invariants(defaults):
    capsule = defaults.get_editor_property("capsule_component")
    movement = defaults.get_editor_property("character_movement")
    return {
        "capsule_radius": float(capsule.get_editor_property("capsule_radius")),
        "capsule_half_height": float(capsule.get_editor_property("capsule_half_height")),
        "collision_profile": str(capsule.get_collision_profile_name()),
        "max_walk_speed": float(movement.get_editor_property("max_walk_speed")),
        "orient_rotation_to_movement": bool(movement.get_editor_property("orient_rotation_to_movement")),
    }


def set_character_visuals(mesh, manifest):
    blueprint = unreal.load_asset(CHARACTER_BP)
    require(isinstance(blueprint, unreal.Blueprint), "Player Blueprint missing: " + CHARACTER_BP)
    anim_class = unreal.load_class(None, NATIVE_ANIM_CLASS)
    require(anim_class is not None, "Build the MistAlphonseAnimInstance C++ class before assigning the mesh.")
    generated_class = unreal.load_class(None, CHARACTER_BP + ".BP_TopDownCharacter_C")
    defaults = unreal.get_default_object(generated_class)
    before = character_invariants(defaults)
    component = defaults.get_editor_property("mesh")
    components = [component]
    # Native inherited components normally live on the Blueprint CDO. Include
    # any editable per-Blueprint component template as well, so recompiling
    # cannot restore the mannequin from an older template.
    subsystem = unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)
    for handle in subsystem.k2_gather_subobject_data_for_blueprint(blueprint):
        data = unreal.SubobjectDataBlueprintFunctionLibrary.get_data(handle)
        candidate = unreal.SubobjectDataBlueprintFunctionLibrary.get_object_for_blueprint(data, blueprint)
        if (isinstance(candidate, unreal.SkeletalMeshComponent)
                and candidate.get_name() == component.get_name()
                and candidate.get_path_name().startswith(CHARACTER_BP + ".")
                and candidate not in components):
            components.append(candidate)
    settings = manifest.get("mesh_transform", {})
    translation = settings.get("translation", [0.0, 0.0, -96.0])
    rotation = settings.get("rotation", [0.0, 0.0, 0.0])
    scale = settings.get("scale", [1.0, 1.0, 1.0])
    for component in components:
        component.set_editor_property("skeletal_mesh_asset", mesh)
        component.set_editor_property("relative_location", vector(translation))
        component.set_editor_property("relative_rotation", rotator(rotation))
        component.set_editor_property("relative_scale3d", vector(scale))
        component.set_editor_property("animation_mode", unreal.AnimationMode.ANIMATION_BLUEPRINT)
        component.set_editor_property("anim_class", anim_class)
    compiled = unreal.BlueprintEditorLibrary.compile_blueprint(blueprint)
    # UE 5.8 returns bool (BlueprintEditorLibrary.cpp594), while older bindings
    # returned None. Check an explicit false/error and the resulting defaults
    # instead of treating a void result as a compilation failure.
    require(compiled is not False
            and blueprint.get_editor_property("status") != unreal.BlueprintStatus.BS_ERROR,
            "Player Blueprint compilation failed.")
    generated_class = unreal.load_class(None, CHARACTER_BP + ".BP_TopDownCharacter_C")
    defaults = unreal.get_default_object(generated_class)
    component = defaults.get_editor_property("mesh")
    require(component.get_editor_property("skeletal_mesh_asset") == mesh,
            "Blueprint compilation reset the imported skeletal mesh.")
    require(component.get_editor_property("anim_class") == anim_class,
            "Blueprint compilation reset the native animation class.")
    after = character_invariants(defaults)
    require(before == after, "Visual update changed player collision/movement defaults.")
    save(blueprint)
    return {"blueprint": CHARACTER_BP, "skeletal_mesh": mesh.get_path_name(),
            "anim_class": NATIVE_ANIM_CLASS, "relative_location": translation,
            "relative_rotation": rotation, "relative_scale": scale,
            "preserved_character_defaults": after}


def main():
    phase("begin")
    manifest_path = find_manifest()
    manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    report_path = Path(manifest.get("unreal_report", manifest_path.parent / "Unreal-Import-Report.json"))
    for directory in (ASSET_DIR, ANIM_DIR, MATERIAL_DIR):
        unreal.EditorAssetLibrary.make_directory(directory)
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    if world is not None:
        unreal.SystemLibrary.execute_console_command(world, "Interchange.FeatureFlags.Import.FBX 0")
    expected_skeleton = ASSET_DIR + "/SKEL_Alphonse_Blockout"
    existing_skeleton = load_if_exists(expected_skeleton)
    require(existing_skeleton is None or isinstance(existing_skeleton, unreal.Skeleton),
            "Expected a Skeleton at " + expected_skeleton)
    phase("skeletal_mesh_import")
    mesh = import_one(source_file(manifest["mesh_fbx"], manifest_path), MESH_NAME, ASSET_DIR,
                      import_settings(unreal.FBXImportType.FBXIT_SKELETAL_MESH, manifest, existing_skeleton),
                      unreal.SkeletalMesh)
    skeleton = mesh.get_editor_property("skeleton")
    require(isinstance(skeleton, unreal.Skeleton), "Skeletal mesh did not produce a Skeleton.")
    if skeleton.get_path_name().split(".")[0] != expected_skeleton:
        require(not unreal.EditorAssetLibrary.does_asset_exist(expected_skeleton),
                "Unexpected existing custom skeleton; avoid replacing a rig with incompatible bones.")
        require(unreal.EditorAssetLibrary.rename_asset(skeleton.get_path_name(), expected_skeleton),
                "Failed to name the Alphonse skeleton.")
        skeleton = unreal.load_asset(expected_skeleton)
    phase("materials")
    lookup, materials = make_materials(manifest)
    phase("mesh_material_assignment")
    mesh_slots = apply_material_slots(mesh, lookup)
    phase("sword_import")
    sword, sword_calibration = import_sword_calibrated(manifest, manifest_path)
    sword_slots = apply_material_slots(sword, lookup)
    phase("animations")
    sequences, clips = import_animations(manifest, manifest_path, skeleton)
    phase("bone_validation")
    bones = check_imported_bones(mesh, sequences["Idle"], manifest)
    phase("socket")
    socket = create_weapon_socket(mesh, sequences["Idle"], manifest)
    phase("blueprint")
    character = set_character_visuals(mesh, manifest)
    phase("report")
    save(skeleton)
    report = {
        "status": "success", "manifest": str(manifest_path), "mesh": mesh.get_path_name(),
        "skeleton": skeleton.get_path_name(), "sword": sword.get_path_name(),
        "mesh_bounds": asset_bounds(mesh), "sword_bounds": asset_bounds(sword),
        "sword_axis_calibration": sword_calibration,
        "materials": materials, "mesh_material_slots": mesh_slots, "sword_material_slots": sword_slots,
        "bones": bones, "clips": clips, "weapon_socket": socket, "character": character,
        "legacy_fbx_import": True, "collision_and_movement_preserved": True,
    }
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    unreal.log("MIST_ALPHONSE_IMPORT_SUCCESS=" + str(report_path))


if __name__ == "__main__":
    main()
