"""Migrate only the three generated crypt signs; preserve all map geometry/custom edits.

Run with Unreal closed, after compiling MistWorldLabel:
  UnrealEditor-Cmd.exe Mistworld.uproject -unattended -NullRHI -nosound \
      -ExecutePythonScript=Scripts/fix_crypt_labels.py
"""
import json
from pathlib import Path
import unreal

MAP = "/Game/Mistworld/Maps/Lvl_CryptSlice"
OWNER = "Mistworld_CryptSlice_Generated_v1"
REPORT = Path(unreal.Paths.project_saved_dir()) / "CryptLabelFix.json"
CHANGES = {
    "Crypt_EntranceInstructions": ((-1500.0, 0.0, 245.0), "CRYPT CHECKPOINT<br>L: loadout / B: replay boss"),
    "Crypt_EncounterInstructions": ((800.0, -1120.0, 245.0), None),
    "Crypt_RewardInstructions": ((2000.0, 1100.0, 245.0), "REWARD CHECKPOINT<br>C: claim loot / B: replay boss"),
}
result = {"success": False, "map": MAP, "changed_labels": []}
try:
    level = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if level.is_in_play_in_editor():
        raise RuntimeError("Stop Play before repairing world labels.")
    dirty = list(unreal.EditorLoadingAndSavingUtils.get_dirty_map_packages()) + list(unreal.EditorLoadingAndSavingUtils.get_dirty_content_packages())
    if dirty:
        raise RuntimeError("Save existing changes before this migration.")
    cls = unreal.load_class(None, "/Script/Mistworld.MistWorldLabel")
    if not cls:
        raise RuntimeError("Compile MistWorldLabel before running this migration.")
    if not unreal.EditorLevelLibrary.load_level(MAP):
        raise RuntimeError("Could not load the crypt map.")
    subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    if OWNER not in [str(tag) for tag in world.get_world_settings().get_editor_property("tags")]:
        raise RuntimeError("The crypt map is not marked as this project's generated slice.")
    by_name = {actor.get_actor_label(): actor for actor in subsystem.get_all_level_actors()}
    # Validate every target before replacing any actor.
    for name in CHANGES:
        old = by_name.get(name)
        if not old or not isinstance(old, unreal.TextRenderActor):
            raise RuntimeError("Missing expected text actor: " + name)
        if OWNER not in [str(tag) for tag in old.get_editor_property("tags")]:
            raise RuntimeError("Refusing to modify a label without its ownership tag: " + name)
    for name, (location, replacement_text) in CHANGES.items():
        old = by_name[name]
        old_text = old.text_render
        text = replacement_text if replacement_text is not None else str(old_text.get_editor_property("text"))
        size = old_text.get_editor_property("world_size")
        color = old_text.get_editor_property("text_render_color")
        if old.get_class() == cls:
            actor = old
            actor.set_actor_location(unreal.Vector(*location), False, False)
            actor.set_actor_rotation(unreal.Rotator(45.0, -135.0, 0.0), False)
        else:
            actor = subsystem.spawn_actor_from_class(cls, unreal.Vector(*location), unreal.Rotator(45.0, -135.0, 0.0))
            if not actor:
                raise RuntimeError("Could not create floating label: " + name)
            actor.set_folder_path(old.get_folder_path())
            actor.set_editor_property("tags", old.get_editor_property("tags"))
            subsystem.destroy_actor(old)
            actor.set_actor_label(name)
        component = actor.text_render
        component.set_text(text)
        component.set_world_size(size)
        component.set_text_render_color(color)
        component.set_horizontal_alignment(unreal.HorizTextAligment.EHTA_CENTER)
        component.set_collision_profile_name("NoCollision")
        component.set_editor_property("can_ever_affect_navigation", False)
        component.set_cast_shadow(False)
        result["changed_labels"].append({"name": name, "position": list(location), "class": actor.get_class().get_name(), "text": text})
    if not unreal.EditorLevelLibrary.save_current_level():
        raise RuntimeError("Could not save the repaired labels.")
    result["success"] = True
    unreal.log("MIST_CRYPT_LABEL_FIX_SUCCESS")
except Exception as error:
    result["error"] = str(error)
    unreal.log_error("MIST_CRYPT_LABEL_FIX_FAILED: " + str(error))
    raise
finally:
    REPORT.parent.mkdir(parents=True, exist_ok=True)
    REPORT.write_text(json.dumps(result, indent=2), encoding="utf-8")
