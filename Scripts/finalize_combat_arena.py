"""Refresh/verify the generated arena's nav bounds and save only when ready.

Run from the open generated map, not in Play:
    py ".../Mistworld/Scripts/finalize_combat_arena.py"

This does not recreate or move anything. Updating nav bounds is queued by UE;
if the report is pending_navigation, run again after an editor tick/short pause.
"""
import json
import math
from pathlib import Path
import traceback
import unreal

MAP = "/Game/Mistworld/Maps/Lvl_CombatTest"
TAG = "Mistworld_CombatArena_Generated_v1"
REPORT = Path(unreal.Paths.project_saved_dir()) / "CombatArenaSetup.json"
STATE = json.loads(REPORT.read_text(encoding="utf-8")) if REPORT.exists() else {"map": MAP}


def property_value(obj, name):
    try:
        value = obj.get_editor_property(name)
        if isinstance(value, (str, bool, float, int)) or value is None:
            return value
        return str(value)
    except Exception as error:
        return "unavailable: " + str(error)


def write():
    REPORT.parent.mkdir(parents=True, exist_ok=True)
    REPORT.write_text(json.dumps(STATE, indent=2), encoding="utf-8")


try:
    if unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).is_in_play_in_editor():
        raise RuntimeError("Stop Play before verifying/saving the combat arena.")
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    if not world or not world.get_path_name().startswith(MAP + "."):
        raise RuntimeError("The generated Lvl_CombatTest must be open; no map was changed.")
    settings = world.get_world_settings()
    if TAG not in [str(tag) for tag in settings.get_editor_property("tags")]:
        raise RuntimeError("Refusing to save a map not marked as script-owned.")
    subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = subsystem.get_all_level_actors()
    by_label = {actor.get_actor_label(): actor for actor in actors}
    for name in ("CombatArena_Floor", "CombatArena_Enemy", "CombatArena_NavBounds"):
        if name not in by_label:
            raise RuntimeError("The generated arena is missing " + name)

    nav = unreal.NavigationSystemV1.get_navigation_system(world)
    data = [actor for actor in actors if isinstance(actor, unreal.RecastNavMesh)]
    floor = by_label["CombatArena_Floor"]
    floor_component = floor.static_mesh_component
    center, extent = floor.get_actor_bounds(False, False)
    diagnostics = {
        "world": world.get_path_name(),
        "navigation_system": nav.get_path_name() if nav else None,
        "navigation_system_config": property_value(settings, "navigation_system_config"),
        "navigation_system_config_override": property_value(settings, "navigation_system_config_override"),
        "floor_bounds_center": [center.x, center.y, center.z],
        "floor_bounds_extent": [extent.x, extent.y, extent.z],
        "floor_collision_profile": str(floor_component.get_collision_profile_name()),
        "floor_can_affect_navigation": property_value(floor_component, "can_ever_affect_navigation"),
        "navigation_data": [{"actor": actor.get_path_name(),
                             "runtime_generation": property_value(actor, "runtime_generation"),
                             "agent_radius": property_value(actor, "agent_radius"),
                             "agent_height": property_value(actor, "agent_height")}
                            for actor in data],
    }
    STATE["navigation_diagnostics"] = diagnostics
    if nav:
        nav.on_navigation_bounds_updated(by_label["CombatArena_NavBounds"])
        STATE["nav_bounds_update_requested"] = True
        # The runtime exec command requires existing nav data. Once bounds updates
        # have been processed, this explicitly completes its rebuild synchronously.
        if data:
            unreal.SystemLibrary.execute_console_command(world, "RebuildNavigation")
    else:
        STATE["nav_bounds_update_requested"] = False

    routes = (
        ("spawn_east_1800cm", (-700., -650., 0.), (1100., -650., 0.)),
        ("center_east_1800cm", (0., 0., 0.), (1800., 0., 0.)),
        ("center_north_200cm", (0., 0., 0.), (0., 200., 0.)),
        ("spawn_to_enemy", (-700., -650., 0.), (400., 300., 0.)),
    )
    checks = []
    for label, start, end in routes:
        path = unreal.NavigationSystemV1.find_path_to_location_synchronously(
            world, unreal.Vector(*start), unreal.Vector(*end))
        valid = bool(path and path.is_valid() and not path.is_partial())
        points = list(path.get_editor_property("path_points")) if path else []
        length = path.get_path_length() if path else -1.0
        checks.append({"route": label, "valid": valid,
                       "straight": valid and len(points) >= 2 and abs(length - math.dist(start, end)) < 10.,
                       "length_cm": length, "point_count": len(points)})
    STATE["navigation_checks"] = checks
    STATE["actor_count"] = len(actors)
    if all(check["straight"] for check in checks):
        if not unreal.EditorLevelLibrary.save_current_level():
            raise RuntimeError("Navigation is valid, but saving the arena failed.")
        STATE.update(success=True, status="complete")
        STATE.pop("error", None)
        STATE.pop("traceback", None)
        unreal.log("MIST_COMBAT_ARENA_READY: " + MAP)
    else:
        STATE.update(success=False, status="pending_navigation")
        STATE["error"] = ("Navigation bounds were refreshed; run this script again after an editor tick."
                          if nav else "The new world has no navigation system; inspect its navigation config.")
        unreal.log_warning("MIST_COMBAT_ARENA_PENDING: " + STATE["error"])
    write()
except Exception as error:
    STATE.update(success=False, status="failed", error=str(error), traceback=traceback.format_exc())
    write()
    unreal.log_error("MIST_COMBAT_ARENA_FAILED: " + str(error))
    raise
