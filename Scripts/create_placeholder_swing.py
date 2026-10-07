"""Generate real, in-place Manny sword slashes using editor animation APIs.

Run through UnrealEditor-Cmd -ExecutePythonScript=<this file>. No external assets
or Python modules are required. The sword must extend along hand_r local +X.
Gameplay remains responsible for damage at 0.18 / 0.25 seconds; metadata records
the matching pose contact time, without creating gameplay damage notifies.
"""
import json
import math
import os
import unreal


EPSILON = 1.0e-8
# Factory animations begin at 30 fps. The controller requires a compatible
# multiple/factor when changing frame rate; 300 also gives exact .18/.25 keys.
FPS = 300
DESTINATION = "/Game/Mistworld/Animations"
IDLE_PATH = "/Game/Characters/Mannequins/Anims/Unarmed/MM_Idle"
FIST_PATH = "/Game/Characters/Mannequins/Anims/Unarmed/Attack/MM_Attack_01"
CHARACTER_CLASS = "/Game/TopDown/Blueprints/BP_TopDownCharacter.BP_TopDownCharacter_C"


def add(a, b):
    return tuple(x + y for x, y in zip(a, b))


def subtract(a, b):
    return tuple(x - y for x, y in zip(a, b))


def multiply(v, scalar):
    return tuple(x * scalar for x in v)


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


def length(v):
    return math.sqrt(dot(v, v))


def normalized(v):
    magnitude = length(v)
    if magnitude < EPSILON:
        raise ValueError("Cannot normalize a zero vector/quaternion")
    return multiply(v, 1.0 / magnitude)


def lerp(a, b, alpha):
    return add(multiply(a, 1.0 - alpha), multiply(b, alpha))


def quat_multiply(a, b):
    # Hamilton product: a * b applies b first, then a, matching FQuat.
    av, bv = a[:3], b[:3]
    xyz = add(add(multiply(bv, a[3]), multiply(av, b[3])), cross(av, bv))
    return normalized(xyz + (a[3] * b[3] - dot(av, bv),))


def quat_inverse(q):
    q = normalized(q)
    return (-q[0], -q[1], -q[2], q[3])


def quat_rotate(q, vector):
    # FQuat::RotateVector equivalent, avoiding invalid pure-vector normalize.
    q = normalized(q)
    tangent = multiply(cross(q[:3], vector), 2.0)
    return add(vector, add(multiply(tangent, q[3]), cross(q[:3], tangent)))


def quat_between(source, destination):
    source, destination = normalized(source), normalized(destination)
    cosine = max(-1.0, min(1.0, dot(source, destination)))
    if cosine > 1.0 - 1.0e-7:
        return (0.0, 0.0, 0.0, 1.0)
    if cosine < -1.0 + 1.0e-7:
        axis = cross(source, (1.0, 0.0, 0.0))
        if length(axis) < 1.0e-5:
            axis = cross(source, (0.0, 1.0, 0.0))
        return normalized(axis) + (0.0,)
    return normalized(cross(source, destination) + (1.0 + cosine,))


def quat_slerp(a, b, alpha):
    a, b = normalized(a), normalized(b)
    cosine = dot(a, b)
    if cosine < 0.0:
        b, cosine = multiply(b, -1.0), -cosine
    if cosine > 0.9995:
        return normalized(lerp(a, b, alpha))
    angle = math.acos(max(-1.0, min(1.0, cosine)))
    denominator = math.sin(angle)
    return normalized(add(multiply(a, math.sin((1.0 - alpha) * angle) / denominator),
                          multiply(b, math.sin(alpha * angle) / denominator)))


def vector_tuple(vector):
    return (vector.x, vector.y, vector.z)


def quat_tuple(quat):
    return normalized((quat.x, quat.y, quat.z, quat.w))


def ue_vector(vector):
    # The reflected SetBoneTrackKeys overload accepts FVector / FQuat arrays.
    return unreal.Vector(vector[0], vector[1], vector[2])


def ue_quat(quat):
    return unreal.Quat(quat[0], quat[1], quat[2], quat[3])


def sample_pose(sequence, time):
    options = unreal.AnimPoseEvaluationOptions()
    options.set_editor_property("evaluation_type", unreal.AnimDataEvalType.RAW)
    options.set_editor_property("should_retarget", False)
    options.set_editor_property("extract_root_motion", False)
    return unreal.AnimPoseExtensions.get_anim_pose_at_time(sequence, time, options)


def read_pose(pose, bone, space):
    transform = unreal.AnimPoseExtensions.get_bone_pose(pose, bone, space)
    return {"position": vector_tuple(transform.translation),
            "rotation": quat_tuple(transform.rotation),
            "scale": vector_tuple(transform.scale3d)}


def solve_two_bone(shoulder, wrist, pole_direction, upper_length, lower_length):
    offset = subtract(wrist, shoulder)
    distance = length(offset)
    axis = normalized(offset)
    # Avoid singular perfectly straight/folded elbows without changing lengths.
    reach = max(abs(upper_length - lower_length) + 0.001,
                min(upper_length + lower_length - 0.001, distance))
    wrist = add(shoulder, multiply(axis, reach))
    along = (upper_length ** 2 + reach ** 2 - lower_length ** 2) / (2.0 * reach)
    bend_height = math.sqrt(max(0.0, upper_length ** 2 - along ** 2))
    bend = subtract(pole_direction, multiply(axis, dot(pole_direction, axis)))
    if length(bend) < 1.0e-5:
        bend = cross(axis, (0.0, 0.0, 1.0))
    if length(bend) < 1.0e-5:
        bend = cross(axis, (0.0, 1.0, 0.0))
    elbow = add(add(shoulder, multiply(axis, along)),
                multiply(normalized(bend), bend_height))
    return elbow, wrist


def timeline_state(time, keyframes):
    for left, right in zip(keyframes, keyframes[1:]):
        if time <= right["time"] + 1.0e-9:
            alpha = max(0.0, min(1.0, (time - left["time"]) / (right["time"] - left["time"])))
            # Wind-up and reset ease into their poses; the blade passes contact
            # briskly, without a slow-down exactly at the damage frame.
            if left["phase"] in ("idle", "follow"):
                alpha = alpha * alpha * (3.0 - 2.0 * alpha)
            return {"hand": lerp(left["hand"], right["hand"], alpha),
                    "pole": lerp(left["pole"], right["pole"], alpha),
                    "rotation": quat_slerp(left["rotation"], right["rotation"], alpha)}
    return keyframes[-1]


def inspect_default_slot():
    result = []
    blueprint = unreal.load_asset("/Game/Characters/Mannequins/Anims/Unarmed/ABP_Unarmed")
    for graph in unreal.BlueprintEditorLibrary.list_graphs(blueprint):
        editor = unreal.BlueprintGraphEditor.get_graph_editor(graph)
        for node in editor.list_all_nodes():
            if node.get_class().get_name() == "AnimGraphNode_Slot":
                result.append({"graph": graph.get_name(),
                               "slot": str(node.get_editor_property("node").get_editor_property("slot_name")),
                               "connections": [{"pin": str(pin.get_pin_name()),
                                                "links": [{"node": link.get_owning_node().get_name(),
                                                           "pin": str(link.get_pin_name())}
                                                          for link in pin.list_connected_pins()]}
                                               for pin in node.list_all_pins()]})
    return result


def generate():
    idle = unreal.load_asset(IDLE_PATH)
    fist = unreal.load_asset(FIST_PATH)
    character = unreal.get_default_object(unreal.load_class(None, CHARACTER_CLASS))
    mesh_component = character.get_editor_property("mesh")
    mesh = mesh_component.get_editor_property("skeletal_mesh_asset")
    skeleton = idle.get_editor_property("skeleton")
    idle_pose = sample_pose(idle, 0.0)
    fist_pose = sample_pose(fist, 0.0)
    bone_names = [str(name) for name in unreal.AnimPoseExtensions.get_bone_names(idle_pose)
                  if not str(name).startswith("VB ")]
    local = {name: read_pose(idle_pose, name, unreal.AnimPoseSpaces.LOCAL) for name in bone_names}
    world = {name: read_pose(idle_pose, name, unreal.AnimPoseSpaces.WORLD) for name in bone_names}
    parents = {name: str(mesh_component.get_parent_bone(name)) for name in bone_names}
    if parents["lowerarm_r"] != "upperarm_r" or parents["hand_r"] != "lowerarm_r":
        raise RuntimeError("Unexpected mannequin right-arm hierarchy: " + str(parents))

    shoulder = world["upperarm_r"]["position"]
    idle_elbow = world["lowerarm_r"]["position"]
    idle_hand = world["hand_r"]["position"]
    idle_upper_direction = subtract(idle_elbow, shoulder)
    idle_lower_direction = subtract(idle_hand, idle_elbow)
    upper_length, lower_length = length(idle_upper_direction), length(idle_lower_direction)
    idle_axis = normalized(subtract(idle_hand, shoulder))
    idle_pole = normalized(subtract(idle_upper_direction,
                                    multiply(idle_axis, dot(idle_upper_direction, idle_axis))))
    idle_hand_quat = world["hand_r"]["rotation"]
    idle_blade_axis = quat_rotate(idle_hand_quat, (1.0, 0.0, 0.0))

    def hand_orientation(axis):
        return quat_multiply(quat_between(idle_blade_axis, axis), idle_hand_quat)

    # A closed fist from the shipped mannequin clip visibly grips the sword.
    # Preserve idle translations/scales so only finger flexion is inherited.
    finger_names = [name for name in bone_names if name.endswith("_r")
                    and any(name.startswith(prefix) for prefix in
                            ("thumb_", "index_", "middle_", "ring_", "pinky_"))]
    for name in finger_names:
        local[name]["rotation"] = read_pose(fist_pose, name, unreal.AnimPoseSpaces.LOCAL)["rotation"]

    report = {"mesh": mesh.get_path_name(), "skeleton": skeleton.get_path_name(),
              "sword_axis": "+X in hand_r local space", "frame_rate": FPS,
              "upper_arm_length": upper_length, "forearm_length": lower_length,
              "slots": inspect_default_slot(), "clips": []}

    for name, duration, contact, windup, follow in [
            ("A_Sword_Auto", 0.60, 0.18, 0.08, 0.30),
            ("A_Sword_Cleave", 0.80, 0.25, 0.12, 0.40)]:
        keyframes = [
            {"time": 0.0, "phase": "idle", "hand": idle_hand,
             "pole": idle_pole, "rotation": idle_hand_quat},
            {"time": windup, "phase": "windup", "hand": (-30.0, -15.0, 170.0),
             "pole": (-1.0, -0.45, 0.2), "rotation": hand_orientation((0.0, -0.12, 1.0))},
            {"time": contact, "phase": "contact", "hand": (-10.0, 42.0, 125.0),
             "pole": (-1.0, -0.3, 0.1), "rotation": hand_orientation((0.0, 1.0, -0.20))},
            {"time": follow, "phase": "follow", "hand": (18.0, 25.0, 112.0),
             "pole": (-1.0, -0.45, -0.1), "rotation": hand_orientation((1.0, 0.25, -0.20))},
            {"time": duration, "phase": "idle", "hand": idle_hand,
             "pole": idle_pole, "rotation": idle_hand_quat},
        ]
        frames = int(round(duration * FPS))
        tracks = {bone: {"positions": [], "rotations": [], "scales": []} for bone in bone_names}
        fk_errors = []
        for frame in range(frames + 1):
            state = timeline_state(frame / FPS, keyframes)
            elbow, wrist = solve_two_bone(shoulder, state["hand"], state["pole"], upper_length, lower_length)
            upper_quat = quat_multiply(quat_between(idle_upper_direction, subtract(elbow, shoulder)),
                                      world["upperarm_r"]["rotation"])
            lower_quat = quat_multiply(quat_between(idle_lower_direction, subtract(wrist, elbow)),
                                      world["lowerarm_r"]["rotation"])
            rotations = {
                "upperarm_r": quat_multiply(quat_inverse(world[parents["upperarm_r"]]["rotation"]), upper_quat),
                "lowerarm_r": quat_multiply(quat_inverse(upper_quat), lower_quat),
                "hand_r": quat_multiply(quat_inverse(lower_quat), state["rotation"]),
            }
            actual_elbow = add(shoulder, quat_rotate(upper_quat, local["lowerarm_r"]["position"]))
            actual_hand = add(actual_elbow, quat_rotate(lower_quat, local["hand_r"]["position"]))
            fk_errors.append(length(subtract(actual_hand, wrist)))
            for bone in bone_names:
                rotation = rotations.get(bone, local[bone]["rotation"])
                track = tracks[bone]
                if track["rotations"] and dot(track["rotations"][-1], rotation) < 0.0:
                    rotation = multiply(rotation, -1.0)
                track["positions"].append(local[bone]["position"])
                track["rotations"].append(rotation)
                track["scales"].append(local[bone]["scale"])

        if max(fk_errors) > 0.01:
            raise RuntimeError("Right-arm forward-kinematics check failed: " + str(max(fk_errors)))
        path = DESTINATION + "/" + name
        sequence = unreal.load_asset(path) if unreal.EditorAssetLibrary.does_asset_exist(path) else None
        if not sequence:
            factory = unreal.AnimSequenceFactory()
            factory.set_editor_property("target_skeleton", skeleton)
            factory.set_editor_property("preview_skeletal_mesh", mesh)
            sequence = unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, DESTINATION, unreal.AnimSequence, factory)
        if not sequence or sequence.get_editor_property("skeleton") != skeleton:
            raise RuntimeError("Could not create compatible sword animation: " + path)

        sequence.set_editor_property("enable_root_motion", False)
        controller = sequence.get_editor_property("controller")
        controller.open_bracket("Generate in-place placeholder sword slash", False)
        try:
            # Durations are integral at the factory's 30fps compression rate.
            controller.remove_all_bone_tracks(False)
            controller.set_frame_rate(unreal.FrameRate(FPS, 1), False)
            controller.set_number_of_frames(unreal.FrameNumber(frames), False)
            for bone, track in tracks.items():
                if not controller.add_bone_curve(bone, False):
                    raise RuntimeError("Could not add bone track " + bone)
                if not controller.set_bone_track_keys(
                        bone, [ue_vector(value) for value in track["positions"]],
                        [ue_quat(value) for value in track["rotations"]],
                        [ue_vector(value) for value in track["scales"]], False):
                    raise RuntimeError("Could not write bone track " + bone)
            # AnimSequenceFactory already notified initial population. The
            # controller's track/length events refresh this populated model;
            # NotifyPopulated itself is not a reflected Python API in 5.8.
        finally:
            controller.close_bracket(False)
        unreal.EditorAssetLibrary.set_metadata_tag(sequence, "Mistworld.ContactTime", str(contact))
        unreal.EditorAssetLibrary.set_metadata_tag(sequence, "Mistworld.WeaponAxis", "hand_r local +X")
        if not unreal.EditorAssetLibrary.save_loaded_asset(sequence, False):
            raise RuntimeError("Could not save " + path)

        samples = []
        for time in [0.0, windup, contact, follow, duration]:
            pose = sample_pose(sequence, time)
            hand = read_pose(pose, "hand_r", unreal.AnimPoseSpaces.WORLD)
            samples.append({"time": time, "hand_position": hand["position"],
                            "blade_direction": quat_rotate(hand["rotation"], (1.0, 0.0, 0.0)),
                            "root": read_pose(pose, "root", unreal.AnimPoseSpaces.WORLD)})
        root_positions = [sample["root"]["position"] for sample in samples]
        if any(length(subtract(position, root_positions[0])) > 0.001 for position in root_positions):
            raise RuntimeError("Generated sword clip unexpectedly moves root: " + name)
        report["clips"].append({"path": path, "duration": unreal.AnimationLibrary.get_sequence_length(sequence),
                                "contact_time": contact, "tracks": len(bone_names),
                                "max_fk_error_cm": max(fk_errors), "samples": samples})
        unreal.log("MIST_SWORD_ANIMATION_CREATED " + path)

    report_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "placeholder-swing-report.json")
    with open(report_path, "w", encoding="utf-8") as output:
        json.dump(report, output, indent=2)
    unreal.log("MIST_SWORD_GENERATION_SUCCESS " + report_path)


if __name__ == "__main__":
    generate()
