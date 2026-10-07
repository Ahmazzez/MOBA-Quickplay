"""Create Mistworld's dedicated Milestone 1 combat greybox in a running editor.

Run once after compiling AMistArenaEnemy:
    py ".../Mistworld/Scripts/create_combat_arena.py"

Creates only this script's map/materials. It refuses unsaved user packages or an
existing unmarked map. Navigation is checked after editor ticks, then saved.
"""
import json
import math
from pathlib import Path
import time
import traceback

import unreal


MAP = "/Game/Mistworld/Maps/Lvl_CombatTest"
MATERIAL_DIR = "/Game/Mistworld/Materials/CombatArena"
OWNER_TAG = "Mistworld_CombatArena_Generated_v1"
REPORT = Path(unreal.Paths.project_saved_dir()) / "CombatArenaSetup.json"
PLAYER_START = (-700.0, -650.0, 100.0)
ENEMY_HOME = (400.0, 300.0, 100.0)
DUMMY_HOME = (-1000.0, 700.0, 100.0)
NAV_CENTER = (0.0, 0.0, 250.0)
NAV_HALF_EXTENT = (2100.0, 2100.0, 650.0)

STATE = {"success": False, "status": "starting", "map": MAP,
         "enemy_class": "/Script/Mistworld.MistArenaEnemy",
         "floor_top_z": 0.0, "floor_center": [0.0, 0.0, -50.0],
         "floor_scale": [40.0, 40.0, 1.0],
         "player_start": list(PLAYER_START), "enemy_home": list(ENEMY_HOME),
         "dummy_home": list(DUMMY_HOME),
         "nav_center": list(NAV_CENTER), "nav_half_extent": list(NAV_HALF_EXTENT)}


def write_report():
    REPORT.parent.mkdir(parents=True, exist_ok=True)
    REPORT.write_text(json.dumps(STATE, indent=2), encoding="utf-8")


def vec(values):
    return unreal.Vector(*values)


def fail(message):
    raise RuntimeError(message)


def require_clean_editor():
    level_editor = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if level_editor.is_in_play_in_editor():
        fail("Stop Play before creating the combat arena; the editor can stay open.")
    maps = unreal.EditorLoadingAndSavingUtils.get_dirty_map_packages()
    contents = unreal.EditorLoadingAndSavingUtils.get_dirty_content_packages()
    dirty = [package.get_name() for package in list(maps) + list(contents)]
    if dirty:
        STATE["dirty_packages"] = dirty
        fail("Unsaved packages exist. Save your work, then run the script again: "
             + ", ".join(dirty))


def class_named(path):
    cls = unreal.load_class(None, path)
    if not cls:
        fail("Required class is unavailable; compile/reload first: " + path)
    return cls


def named_actor(subsystem, cls, name, location, rotation=None, folder="Arena"):
    actor = subsystem.spawn_actor_from_class(
        cls, vec(location), rotation or unreal.Rotator(0.0, 0.0, 0.0))
    if not actor:
        fail("Could not spawn " + name)
    actor.set_actor_label(name)
    actor.set_folder_path(folder)
    actor.set_editor_property("tags", [unreal.Name(OWNER_TAG)])
    return actor


def make_material(name, color, metallic=0.0, roughness=0.75):
    path = MATERIAL_DIR + "/" + name
    existing = unreal.load_asset(path) if unreal.EditorAssetLibrary.does_asset_exist(path) else None
    if existing:
        if unreal.EditorAssetLibrary.get_metadata_tag(existing, "MistworldArenaOwner") != OWNER_TAG:
            fail("Refusing to overwrite an unrelated material: " + path)
        return existing
    parent = unreal.load_asset("/Game/LevelPrototyping/Materials/M_FlatCol")
    if not parent:
        fail("The template M_FlatCol material is missing.")
    factory = unreal.MaterialInstanceConstantFactoryNew()
    asset = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        name, MATERIAL_DIR, unreal.MaterialInstanceConstant, factory)
    if not asset:
        fail("Could not create material: " + path)
    edit = unreal.MaterialEditingLibrary
    edit.set_material_instance_parent(asset, parent)
    params = edit.get_vector_parameter_names(parent)
    color_found = False
    for param in params:
        if str(param).replace("_", "").replace(" ", "").lower() in ("color", "basecolor"):
            edit.set_material_instance_vector_parameter_value(asset, param, unreal.LinearColor(*color))
            color_found = True
    if not color_found:
        fail("M_FlatCol exposes no color parameter; refusing uncolored arena materials.")
    for param in edit.get_scalar_parameter_names(parent):
        normalized = str(param).replace("_", "").replace(" ", "").lower()
        if normalized == "metallic":
            edit.set_material_instance_scalar_parameter_value(asset, param, metallic)
        elif normalized == "roughness":
            edit.set_material_instance_scalar_parameter_value(asset, param, roughness)
    unreal.EditorAssetLibrary.set_metadata_tag(asset, "MistworldArenaOwner", OWNER_TAG)
    if not unreal.EditorAssetLibrary.save_loaded_asset(asset, only_if_is_dirty=False):
        fail("Could not save material: " + path)
    return asset


def mesh_actor(subsystem, name, shape, location, scale, material, collision=True,
               rotation=None, folder="Arena/Geometry"):
    actor = named_actor(subsystem, unreal.StaticMeshActor, name, location, rotation, folder)
    component = actor.static_mesh_component
    component.set_static_mesh(unreal.load_asset("/Engine/BasicShapes/" + shape))
    component.set_material(0, material)
    component.set_mobility(unreal.ComponentMobility.STATIC)
    component.set_collision_profile_name("BlockAll" if collision else "NoCollision")
    component.set_editor_property("can_ever_affect_navigation", collision)
    if not collision:
        component.set_cast_shadow(False)
    actor.set_actor_scale3d(vec(scale))
    return actor


def ring(subsystem, prefix, xy, radius, width, material, segments=32):
    # Short tangent pieces make an outline without introducing navigation walls.
    length = 2.0 * radius * math.sin(math.pi / segments)
    for index in range(segments):
        angle = 2.0 * math.pi * (index + 0.5) / segments
        location = (xy[0] + radius * math.cos(angle),
                    xy[1] + radius * math.sin(angle), 0.65)
        mesh_actor(subsystem, f"{prefix}_{index:02d}", "Cube", location,
                   (length / 100.0, width / 100.0, 0.008), material, False,
                   unreal.Rotator(0.0, math.degrees(angle) + 90.0, 0.0),
                   "Arena/Markings")


def create_navigation(subsystem):
    # SpawnActorFromClass uses the editor actor factory, which creates a default
    # box brush for volumes. CubeBuilder.Build is not reflected to Python in 5.8.
    nav = named_actor(subsystem, unreal.NavMeshBoundsVolume, "CombatArena_NavBounds",
                      NAV_CENTER, folder="Arena/Navigation")
    builder = nav.get_editor_property("brush_builder")
    origin, extent = nav.get_actor_bounds(False, False)
    if not builder or min(extent.x, extent.y, extent.z) < 1.0:
        fail("The NavMeshBoundsVolume actor factory did not build a box brush. "
             "Create its box brush in the editor before retrying.")
    nav.set_actor_scale3d(unreal.Vector(NAV_HALF_EXTENT[0] / extent.x,
                                      NAV_HALF_EXTENT[1] / extent.y,
                                      NAV_HALF_EXTENT[2] / extent.z))
    origin, extent = nav.get_actor_bounds(False, False)
    STATE["nav_brush_builder"] = builder.get_class().get_name()
    STATE["nav_actual_origin"] = [origin.x, origin.y, origin.z]
    STATE["nav_actual_half_extent"] = [extent.x, extent.y, extent.z]
    if any(abs(value - desired) > 2.0 for value, desired in zip(
            (extent.x, extent.y, extent.z), NAV_HALF_EXTENT)):
        fail("Navigation volume geometry does not match its requested dimensions.")
    navigation = unreal.NavigationSystemV1.get_navigation_system(
        unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world())
    if navigation:
        # Actor.set_actor_scale3d does not produce the editor property-change
        # event used by ANavMeshBoundsVolume to refresh its registered bounds.
        navigation.on_navigation_bounds_updated(nav)
        STATE["nav_bounds_update_requested"] = True
    else:
        STATE["nav_bounds_update_requested"] = False
    return nav


def construct():
    require_clean_editor()
    enemy_class = class_named(STATE["enemy_class"])
    dummy_class = class_named("/Script/Mistworld.MistTrainingDummy")
    game_mode = class_named("/Game/TopDown/Blueprints/BP_TopDownGameMode.BP_TopDownGameMode_C")
    editor = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
    subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

    if unreal.EditorAssetLibrary.does_asset_exist(MAP):
        if not unreal.EditorLevelLibrary.load_level(MAP):
            fail("Could not load existing arena.")
        world = editor.get_editor_world()
        world_tags = [str(tag) for tag in world.get_world_settings().get_editor_property("tags")]
        if OWNER_TAG not in world_tags:
            fail("Refusing to change an existing map not marked as script-owned: " + MAP)
        labels = {actor.get_actor_label(): actor for actor in subsystem.get_all_level_actors()}
        if not all(name in labels for name in ("CombatArena_Floor", "CombatArena_Enemy", "CombatArena_NavBounds")):
            fail("A previous arena build is incomplete. Inspect it before repairing.")
        STATE["reused_existing_map"] = True
        return world

    if not unreal.EditorLevelLibrary.new_level(MAP):
        fail("Could not create combat arena; the current map was not changed deliberately.")
    world = editor.get_editor_world()
    settings = world.get_world_settings()
    settings.set_editor_property("tags", [unreal.Name(OWNER_TAG)])
    settings.set_editor_property("default_game_mode", game_mode)

    slate = make_material("MI_CombatArena_Slate", (0.085, 0.115, 0.155, 1.0))
    wall = make_material("MI_CombatArena_Wall", (0.15, 0.21, 0.27, 1.0))
    grid = make_material("MI_CombatArena_Grid", (0.17, 0.22, 0.27, 1.0))
    brass = make_material("MI_CombatArena_Brass", (0.7, 0.48, 0.16, 1.0), 0.25, 0.5)
    warm = make_material("MI_CombatArena_Warm", (0.48, 0.14, 0.095, 1.0))
    cool = make_material("MI_CombatArena_Cool", (0.11, 0.40, 0.48, 1.0))
    mesh_actor(subsystem, "CombatArena_Floor", "Cube", (0.0, 0.0, -50.0), (40.0, 40.0, 1.0), slate)
    for label, location, scale in (
            ("West", (-2025.0, 0.0, 150.0), (0.5, 41.0, 3.0)),
            ("East", (2025.0, 0.0, 150.0), (0.5, 41.0, 3.0)),
            ("North", (0.0, 2025.0, 150.0), (40.0, 0.5, 3.0)),
            ("South", (0.0, -2025.0, 150.0), (40.0, 0.5, 3.0))):
        mesh_actor(subsystem, "CombatArena_Wall_" + label, "Cube", location, scale, wall)
    for index, coordinate in enumerate(range(-1600, 1601, 400)):
        mesh_actor(subsystem, f"CombatArena_GridX_{index}", "Cube", (coordinate, 0.0, 0.2),
                   (0.02, 39.8, 0.004), grid, False, folder="Arena/Markings")
        mesh_actor(subsystem, f"CombatArena_GridY_{index}", "Cube", (0.0, coordinate, 0.2),
                   (39.8, 0.02, 0.004), grid, False, folder="Arena/Markings")
    ring(subsystem, "CombatArena_SpawnRing", PLAYER_START[:2], 220.0, 10.0, brass)
    ring(subsystem, "CombatArena_EnemyRing", ENEMY_HOME[:2], 800.0, 4.0, warm, 48)
    ring(subsystem, "CombatArena_PracticeRing", DUMMY_HOME[:2], 180.0, 5.0, cool)
    # Cover stays well away from the center and the movement/dash test strips.
    mesh_actor(subsystem, "CombatArena_Cover", "Cube", (-1400.0, 1300.0, 100.0),
               (2.6, 2.0, 2.0), wall)
    mesh_actor(subsystem, "CombatArena_CoverTrim", "Cube", (-1400.0, 1300.0, 201.0),
               (2.64, 2.04, 0.02), brass, False)
    named_actor(subsystem, unreal.PlayerStart, "CombatArena_PlayerStart", PLAYER_START,
                unreal.Rotator(0.0, 40.0, 0.0), "Arena/Gameplay")
    named_actor(subsystem, enemy_class, "CombatArena_Enemy", ENEMY_HOME,
                unreal.Rotator(0.0, -135.0, 0.0), "Arena/Gameplay")
    named_actor(subsystem, dummy_class, "CombatArena_PracticeDummy", DUMMY_HOME,
                folder="Arena/Gameplay")
    create_navigation(subsystem)

    sun = named_actor(subsystem, unreal.DirectionalLight, "CombatArena_KeyLight",
                      (0.0, 0.0, 1000.0), unreal.Rotator(-55.0, -35.0, 0.0), "Arena/Lighting")
    sun.light_component.set_mobility(unreal.ComponentMobility.MOVABLE)
    sun.light_component.set_intensity(5.0)
    sun.light_component.set_light_color(unreal.LinearColor(0.78, 0.86, 1.0, 1.0))
    sun.light_component.set_editor_property("atmosphere_sun_light", True)
    sky = named_actor(subsystem, unreal.SkyLight, "CombatArena_SkyLight",
                      (0.0, 0.0, 1000.0), folder="Arena/Lighting")
    sky.light_component.set_mobility(unreal.ComponentMobility.MOVABLE)
    sky.light_component.set_intensity(1.0)
    sky.light_component.set_editor_property("real_time_capture", True)
    named_actor(subsystem, unreal.SkyAtmosphere, "CombatArena_Atmosphere",
                (0.0, 0.0, 0.0), folder="Arena/Lighting")
    unreal.EditorLevelLibrary.set_level_viewport_camera_info(
        unreal.Vector(-2600.0, -2600.0, 3100.0), unreal.Rotator(-42.0, 45.0, 0.0))
    subsystem.set_selected_level_actors([])
    STATE["actor_count"] = len(subsystem.get_all_level_actors())
    return world


def validate_paths(world):
    routes = (
        ("spawn_east_1800cm", (-700.0, -650.0, 0.0), (1100.0, -650.0, 0.0)),
        ("center_east_1800cm", (0.0, 0.0, 0.0), (1800.0, 0.0, 0.0)),
        ("center_north_200cm", (0.0, 0.0, 0.0), (0.0, 200.0, 0.0)),
        ("spawn_to_enemy", PLAYER_START[:2] + (0.0,), ENEMY_HOME[:2] + (0.0,)),
    )
    result = []
    for label, start, end in routes:
        path = unreal.NavigationSystemV1.find_path_to_location_synchronously(world, vec(start), vec(end))
        valid = bool(path and path.is_valid() and not path.is_partial())
        points = list(path.get_editor_property("path_points")) if path else []
        length = path.get_path_length() if path else 0.0
        direct = math.dist(start, end)
        # Recast's small vertical projection offset does not alter plan-view length.
        straight = valid and len(points) >= 2 and abs(length - direct) < 10.0
        result.append({"route": label, "valid": valid, "straight": straight,
                       "length_cm": length, "point_count": len(points)})
    STATE["navigation_checks"] = result
    return all(item["straight"] for item in result)


def finish_after_navigation(world):
    unreal.SystemLibrary.execute_console_command(world, "RebuildNavigation")
    STATE["status"] = "waiting_for_navigation"
    write_report()
    started = time.monotonic()
    tick_state = {"handle": None, "next_check": started + 0.5, "valid_checks": 0}

    def tick(_delta):
        now = time.monotonic()
        if now < tick_state["next_check"]:
            return
        tick_state["next_check"] = now + 0.25
        try:
            editor_world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
            if not editor_world or not editor_world.get_path_name().startswith(MAP + "."):
                fail("The open map changed while navigation was building; no map was saved.")
            if validate_paths(world):
                tick_state["valid_checks"] += 1
            else:
                tick_state["valid_checks"] = 0
            if tick_state["valid_checks"] >= 3:
                if not unreal.EditorLevelLibrary.save_current_level():
                    fail("Combat arena navigation is valid, but saving the map failed.")
                STATE.update(success=True, status="complete")
                write_report()
                unreal.unregister_slate_post_tick_callback(tick_state["handle"])
                unreal.log("MIST_COMBAT_ARENA_READY: " + MAP + " report=" + str(REPORT))
                return
            if now - started > 60.0:
                fail("Navigation did not become valid within 60 seconds. The generated "
                     "map remains open for inspection; do not start Play yet.")
        except Exception as error:
            STATE.update(success=False, status="failed", error=str(error))
            write_report()
            unreal.unregister_slate_post_tick_callback(tick_state["handle"])
            unreal.log_error("MIST_COMBAT_ARENA_FAILED: " + str(error))

    tick_state["handle"] = unreal.register_slate_post_tick_callback(tick)


try:
    write_report()
    finish_after_navigation(construct())
except Exception as error:
    STATE.update(success=False, status="failed", error=str(error), traceback=traceback.format_exc())
    write_report()
    unreal.log_error("MIST_COMBAT_ARENA_FAILED: " + str(error))
    raise
