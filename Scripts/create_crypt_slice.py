"""Create/save Mistworld's script-owned Milestone 3 crypt greybox.

Run after compiling MistCryptSession and MistCryptGolem, with Unreal closed:
  UnrealEditor-Cmd.exe Mistworld.uproject -Unattended -NullRHI -NoSound \
      -ExecutePythonScript=Scripts/create_crypt_slice.py

Creates only this map and explicitly marked materials. An existing marked map
is verified and reused; an unmarked map is never overwritten. Navigation gets a
synchronous rebuild after saving/reloading the map. No Slate callbacks are used.
The setup report distinguishes saved geometry from verified navigation.
"""
import json
import math
from pathlib import Path
import traceback

import unreal


MAP = "/Game/Mistworld/Maps/Lvl_CryptSlice"
MATERIAL_DIR = "/Game/Mistworld/Materials/CryptSlice"
OWNER_TAG = "Mistworld_CryptSlice_Generated_v1"
META_KEY = "MistworldCryptOwner"
REPORT = Path(unreal.Paths.project_saved_dir()) / "CryptSliceSetup.json"
PLAYER_START = (-1500.0, 0.0, 98.15)
GOLEM_HOME = (800.0, 0.0, 98.15)
REWARD_HOME = (2000.0, 1100.0, 98.15)
FLOOR_CENTER = (275.0, 0.0, -50.0)
FLOOR_HALF_EXTENT = (2225.0, 1600.0, 50.0)
NAV_CENTER = (275.0, 0.0, 250.0)
NAV_HALF_EXTENT = (2375.0, 1750.0, 650.0)

STATE = {
    "success": False, "status": "starting", "map": MAP,
    "script_owner_tag": OWNER_TAG,
    "session_class": "/Script/Mistworld.MistCryptSession",
    "golem_class": "/Script/Mistworld.MistCryptGolem",
    "floor_top_z": 0.0, "floor_center": list(FLOOR_CENTER),
    "floor_half_extent": list(FLOOR_HALF_EXTENT),
    "hero_capsule_half_height": 96.0,
    "player_start": list(PLAYER_START), "golem_home": list(GOLEM_HOME),
    "reward_checkpoint": list(REWARD_HOME),
    "nav_center": list(NAV_CENTER), "nav_half_extent": list(NAV_HALF_EXTENT),
    "divider_x": -800.0, "door_opening_y": [-250.0, 250.0],
    "protection_radius": 700.0,
}


def write_report():
    REPORT.parent.mkdir(parents=True, exist_ok=True)
    REPORT.write_text(json.dumps(STATE, indent=2), encoding="utf-8")


def vec(values):
    return unreal.Vector(*values)


def fail(message):
    raise RuntimeError(message)


def require_clean_editor():
    if unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).is_in_play_in_editor():
        fail("Stop Play before creating the crypt slice.")
    dirty = [package.get_name() for package in
             list(unreal.EditorLoadingAndSavingUtils.get_dirty_map_packages()) +
             list(unreal.EditorLoadingAndSavingUtils.get_dirty_content_packages())]
    if dirty:
        STATE["dirty_packages"] = dirty
        fail("Unsaved packages exist. Save them before running this script: " + ", ".join(dirty))


def class_named(path):
    cls = unreal.load_class(None, path)
    if not cls:
        fail("Required class is unavailable; compile first: " + path)
    return cls


def named_actor(subsystem, cls, name, location, rotation=None, folder="Crypt"):
    actor = subsystem.spawn_actor_from_class(
        cls, vec(location), rotation or unreal.Rotator(0.0, 0.0, 0.0))
    if not actor:
        fail("Could not spawn " + name)
    actor.set_actor_label(name)
    actor.set_folder_path(folder)
    actor.set_editor_property("tags", [unreal.Name(OWNER_TAG)])
    return actor


def material(name, color, metallic=0.0, roughness=0.8):
    path = MATERIAL_DIR + "/" + name
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        asset = unreal.load_asset(path)
        if unreal.EditorAssetLibrary.get_metadata_tag(asset, META_KEY) != OWNER_TAG:
            fail("Refusing to overwrite an unrelated material: " + path)
        return asset
    parent = unreal.load_asset("/Game/LevelPrototyping/Materials/M_FlatCol")
    if not parent:
        fail("The existing template M_FlatCol material is missing.")
    asset = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        name, MATERIAL_DIR, unreal.MaterialInstanceConstant,
        unreal.MaterialInstanceConstantFactoryNew())
    if not asset:
        fail("Could not create material: " + path)
    edit = unreal.MaterialEditingLibrary
    edit.set_material_instance_parent(asset, parent)
    found = False
    for param in edit.get_vector_parameter_names(parent):
        if str(param).replace("_", "").replace(" ", "").lower() in ("color", "basecolor"):
            edit.set_material_instance_vector_parameter_value(asset, param, unreal.LinearColor(*color))
            found = True
    if not found:
        fail("M_FlatCol has no color parameter; no uncolored crypt will be saved.")
    for param in edit.get_scalar_parameter_names(parent):
        normalized = str(param).replace("_", "").replace(" ", "").lower()
        if normalized == "metallic":
            edit.set_material_instance_scalar_parameter_value(asset, param, metallic)
        elif normalized == "roughness":
            edit.set_material_instance_scalar_parameter_value(asset, param, roughness)
    unreal.EditorAssetLibrary.set_metadata_tag(asset, META_KEY, OWNER_TAG)
    if not unreal.EditorAssetLibrary.save_loaded_asset(asset, only_if_is_dirty=False):
        fail("Could not save material: " + path)
    return asset


def mesh(subsystem, name, shape, location, scale, mat, collision=True,
         rotation=None, folder="Crypt/Architecture"):
    actor = named_actor(subsystem, unreal.StaticMeshActor, name, location, rotation, folder)
    component = actor.static_mesh_component
    static_mesh = unreal.load_asset("/Engine/BasicShapes/" + shape)
    if not static_mesh:
        fail("Required basic mesh is missing: " + shape)
    component.set_static_mesh(static_mesh)
    component.set_material(0, mat)
    component.set_mobility(unreal.ComponentMobility.STATIC)
    component.set_collision_profile_name("BlockAll" if collision else "NoCollision")
    component.set_editor_property("can_ever_affect_navigation", collision)
    if not collision:
        component.set_cast_shadow(False)
    actor.set_actor_scale3d(vec(scale))
    return actor


def ring(subsystem, name, xy, radius, width, mat, segments=48):
    length = 2.0 * radius * math.sin(math.pi / segments)
    for index in range(segments):
        angle = 2.0 * math.pi * (index + 0.5) / segments
        location = (xy[0] + radius * math.cos(angle), xy[1] + radius * math.sin(angle), 0.8)
        mesh(subsystem, f"{name}_{index:02d}", "Cube", location,
             (length / 100.0, width / 100.0, 0.01), mat, False,
             unreal.Rotator(0.0, math.degrees(angle) + 90.0, 0.0), "Crypt/Floor Markings")


def line(subsystem, name, start, end, width, mat, z=1.7):
    dx, dy = end[0] - start[0], end[1] - start[1]
    length = math.hypot(dx, dy)
    mesh(subsystem, name, "Cube", ((start[0] + end[0]) * 0.5, (start[1] + end[1]) * 0.5, z),
         (length / 100.0, width / 100.0, 0.01), mat, False,
         unreal.Rotator(0.0, math.degrees(math.atan2(dy, dx)), 0.0), "Crypt/Floor Markings")


def label(subsystem, name, text, location, color=(210, 216, 226, 255), size=24.0):
    actor = named_actor(subsystem, class_named("/Script/Mistworld.MistWorldLabel"), name, location,
                        unreal.Rotator(45.0, -135.0, 0.0), "Crypt/Instructions")
    component = actor.text_render
    component.set_text(text)
    component.set_world_size(size)
    component.set_horizontal_alignment(unreal.HorizTextAligment.EHTA_CENTER)
    component.set_text_render_color(unreal.Color(*color))
    component.set_collision_profile_name("NoCollision")
    component.set_editor_property("can_ever_affect_navigation", False)
    component.set_cast_shadow(False)
    return actor


def elemental_symbols(subsystem, ember, tide, stone):
    # Match the cube, sphere, and cone seals carried by the three guardians.
    homes = [(400.0, 0.0), (600.0, 346.0), (600.0, -346.0)]
    STATE["guardian_symbol_homes"] = [list(home) for home in homes]
    x, y = homes[0]
    square = [(x - 65.0, y - 65.0), (x + 65.0, y - 65.0), (x + 65.0, y + 65.0), (x - 65.0, y + 65.0)]
    for index in range(4):
        line(subsystem, "Crypt_EmberSquare_" + str(index), square[index], square[(index + 1) % 4], 8.0, ember)
    x, y = homes[1]
    ring(subsystem, "Crypt_TideCircle", (x, y), 70.0, 7.0, tide, 24)
    x, y = homes[2]
    points = [(x - 65.0, y - 45.0), (x + 65.0, y - 45.0), (x, y + 70.0)]
    for index in range(3):
        line(subsystem, "Crypt_StoneTriangle_" + str(index), points[index], points[(index + 1) % 3], 8.0, stone)


def create_navigation(subsystem):
    # UE's editor actor factory constructs a box brush. CubeBuilder.Build is not
    # reflected to Python in this engine, so scale the actual factory brush.
    nav = named_actor(subsystem, unreal.NavMeshBoundsVolume, "Crypt_NavBounds",
                      NAV_CENTER, folder="Crypt/Navigation")
    builder = nav.get_editor_property("brush_builder")
    origin, extent = nav.get_actor_bounds(False, False)
    if not builder or min(extent.x, extent.y, extent.z) < 1.0:
        fail("The editor actor factory did not create a navigation box brush.")
    nav.set_actor_scale3d(vec(tuple(desired / actual for desired, actual in zip(
        NAV_HALF_EXTENT, (extent.x, extent.y, extent.z)))))
    origin, extent = nav.get_actor_bounds(False, False)
    STATE["nav_brush_builder"] = builder.get_class().get_name()
    STATE["nav_actual_origin"] = [origin.x, origin.y, origin.z]
    STATE["nav_actual_half_extent"] = [extent.x, extent.y, extent.z]
    if any(abs(actual - desired) > 2.0 for actual, desired in zip(
            (extent.x, extent.y, extent.z), NAV_HALF_EXTENT)):
        fail("Navigation brush dimensions do not match the crypt's bounds.")
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    nav_system = unreal.NavigationSystemV1.get_navigation_system(world)
    if nav_system:
        nav_system.on_navigation_bounds_updated(nav)
    STATE["nav_bounds_update_requested"] = bool(nav_system)


def verify_ownership(world, subsystem):
    tags = [str(tag) for tag in world.get_world_settings().get_editor_property("tags")]
    if OWNER_TAG not in tags:
        fail("Refusing to change an existing unmarked map: " + MAP)
    actors = {actor.get_actor_label(): actor for actor in subsystem.get_all_level_actors()}
    for name in ("Crypt_Floor", "Crypt_PlayerStart", "Crypt_Golem", "Crypt_Session", "Crypt_NavBounds"):
        if name not in actors:
            fail("The existing marked map is incomplete; inspect its missing " + name)
        if OWNER_TAG not in [str(tag) for tag in actors[name].get_editor_property("tags")]:
            fail("A required crypt actor is not script-owned: " + name)
    return actors


def construct():
    require_clean_editor()
    session_cls = class_named(STATE["session_class"])
    golem_cls = class_named(STATE["golem_class"])
    mode_cls = class_named("/Game/TopDown/Blueprints/BP_TopDownGameMode.BP_TopDownGameMode_C")
    editor = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
    subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    if unreal.EditorAssetLibrary.does_asset_exist(MAP):
        if not unreal.EditorLevelLibrary.load_level(MAP):
            fail("Could not load the existing crypt slice.")
        world = editor.get_editor_world()
        verify_ownership(world, subsystem)
        STATE["reused_existing_map"] = True
        return world
    if not unreal.EditorLevelLibrary.new_level(MAP):
        fail("Could not create the new crypt map.")
    world = editor.get_editor_world()
    settings = world.get_world_settings()
    settings.set_editor_property("tags", [unreal.Name(OWNER_TAG)])
    settings.set_editor_property("default_game_mode", mode_cls)
    stone = material("MI_Crypt_Floor", (0.19, 0.215, 0.25, 1.0))
    wall = material("MI_Crypt_Wall", (0.115, 0.145, 0.185, 1.0))
    trim = material("MI_Crypt_Trim", (0.28, 0.31, 0.355, 1.0))
    seam = material("MI_Crypt_Seam", (0.135, 0.15, 0.175, 1.0))
    gold = material("MI_Crypt_Gold", (0.85, 0.58, 0.14, 1.0), 0.3, 0.45)
    wood = material("MI_Crypt_Chest", (0.29, 0.15, 0.065, 1.0))
    amber = material("MI_Crypt_Amber", (1.0, 0.43, 0.065, 1.0))
    cyan = material("MI_Crypt_Cyan", (0.055, 0.72, 0.95, 1.0))
    green = material("MI_Crypt_Stone", (0.20, 0.82, 0.35, 1.0))
    shield = material("MI_Crypt_Protection", (0.24, 0.39, 0.58, 1.0))

    mesh(subsystem, "Crypt_Floor", "Cube", FLOOR_CENTER, (44.5, 32.0, 1.0), stone)
    for name, location, scale in (
            ("West", (-1975.0, 0.0, 150.0), (0.5, 33.0, 3.0)),
            ("East", (2525.0, 0.0, 150.0), (0.5, 33.0, 3.0)),
            ("North", (275.0, 1625.0, 150.0), (44.5, 0.5, 3.0)),
            ("South", (275.0, -1625.0, 150.0), (44.5, 0.5, 3.0)),
            ("FoyerNorth", (-800.0, 925.0, 150.0), (0.6, 13.5, 3.0)),
            ("FoyerSouth", (-800.0, -925.0, 150.0), (0.6, 13.5, 3.0))):
        mesh(subsystem, "Crypt_Wall_" + name, "Cube", location, scale, wall)
    # The portal lintel is deliberately omitted: it would obstruct the camera.
    for side, y in (("North", 290.0), ("South", -290.0)):
        mesh(subsystem, "Crypt_PortalPost_" + side, "Cube", (-800.0, y, 170.0),
             (0.82, 0.70, 3.4), trim)
    for index, x in enumerate((-1700.0, -1100.0, -350.0, 350.0, 1050.0, 1750.0, 2350.0)):
        line(subsystem, "Crypt_FloorSeamX_" + str(index), (x, -1590.0), (x, 1590.0), 2.0, seam, 0.15)
    for index, y in enumerate((-1200.0, -800.0, -400.0, 400.0, 800.0, 1200.0)):
        line(subsystem, "Crypt_FloorSeamY_" + str(index), (-1940.0, y), (2490.0, y), 2.0, seam, 0.15)
    for index, (x, y) in enumerate(((0.0, -1430.0), (0.0, 1430.0), (1250.0, -1430.0), (1250.0, 1430.0))):
        mesh(subsystem, f"Crypt_PillarBase_{index}", "Cube", (x, y, 25.0), (1.5, 1.5, 0.5), trim)
        mesh(subsystem, f"Crypt_PillarShaft_{index}", "Cylinder", (x, y, 155.0), (1.1, 1.1, 2.6), wall)
        mesh(subsystem, f"Crypt_PillarCap_{index}", "Cube", (x, y, 292.0), (1.4, 1.4, 0.22), trim)
    for index, x in enumerate((-1500.0, -300.0, 500.0, 1300.0, 2100.0)):
        for side, y in (("North", 1575.0), ("South", -1575.0)):
            mesh(subsystem, f"Crypt_Niche_{side}_{index}", "Cube", (x, y, 100.0),
                 (2.4, 0.2, 1.65), seam, False)
            mesh(subsystem, f"Crypt_NicheSill_{side}_{index}", "Cube", (x, y - (20.0 if y > 0 else -20.0), 30.0),
                 (2.6, 0.5, 0.17), trim)
    for index, (x, y) in enumerate(((-1550.0, -1470.0), (-1550.0, 1470.0), (-450.0, -1470.0),
                                     (-450.0, 1470.0), (1600.0, -1470.0), (1600.0, 1470.0))):
        fire_mat = amber if index % 2 == 0 else cyan
        mesh(subsystem, f"Crypt_TorchStem_{index}", "Cube", (x, y, 125.0), (0.15, 0.15, 1.0), trim, False)
        mesh(subsystem, f"Crypt_TorchFlame_{index}", "Sphere", (x, y, 182.0), (0.28, 0.28, 0.46), fire_mat, False)
        light = named_actor(subsystem, unreal.PointLight, f"Crypt_TorchLight_{index}",
                            (x, y, 200.0), folder="Crypt/Lighting")
        light.light_component.set_mobility(unreal.ComponentMobility.MOVABLE)
        light.light_component.set_intensity(2200.0)
        light.light_component.set_editor_property("attenuation_radius", 560.0)
        color = (1.0, 0.49, 0.16, 1.0) if index % 2 == 0 else (0.18, 0.68, 1.0, 1.0)
        light.light_component.set_light_color(unreal.LinearColor(*color))
    ring(subsystem, "Crypt_EntranceCheckpoint", PLAYER_START[:2], 250.0, 10.0, gold)
    ring(subsystem, "Crypt_RewardCheckpoint", REWARD_HOME[:2], 250.0, 10.0, gold)
    ring(subsystem, "Crypt_GolemProtection", GOLEM_HOME[:2], 700.0, 8.0, shield, 64)
    elemental_symbols(subsystem, amber, cyan, green)
    for index, (part, location, scale, mat) in enumerate((
            ("Base", (2140.0, 1100.0, 30.0), (1.35, 0.9, 0.55), wood),
            ("Lid", (2140.0, 1100.0, 64.0), (1.4, 0.94, 0.18), wood),
            ("BandWest", (2090.0, 1100.0, 47.0), (0.09, 0.96, 0.9), gold),
            ("BandEast", (2190.0, 1100.0, 47.0), (0.09, 0.96, 0.9), gold),
            ("Latch", (2140.0, 1053.0, 54.0), (0.22, 0.07, 0.30), gold))):
        mesh(subsystem, "Crypt_RewardChest_" + part, "Cube", location, scale, mat, False,
             folder="Crypt/Rewards")
    label(subsystem, "Crypt_EntranceInstructions", "CRYPT CHECKPOINT<br>L: loadout / B: replay boss", (-1500.0, 0.0, 245.0), (242, 201, 104, 255), 25.0)
    label(subsystem, "Crypt_EncounterInstructions", "BAIT CHARGES OUTSIDE THE BLUE CIRCLE<br>Defeat three guardians: 15 seconds to attack the golem",
          (800.0, -1120.0, 245.0), size=23.0)
    label(subsystem, "Crypt_RewardInstructions", "REWARD CHECKPOINT<br>C: claim loot / B: replay boss", (2000.0, 1100.0, 245.0), (242, 201, 104, 255), 22.0)
    named_actor(subsystem, unreal.PlayerStart, "Crypt_PlayerStart", PLAYER_START,
                unreal.Rotator(0.0, 0.0, 0.0), "Crypt/Gameplay")
    named_actor(subsystem, session_cls, "Crypt_Session", (0.0, 0.0, 0.0), folder="Crypt/Gameplay")
    named_actor(subsystem, golem_cls, "Crypt_Golem", GOLEM_HOME, folder="Crypt/Gameplay")
    create_navigation(subsystem)
    sun = named_actor(subsystem, unreal.DirectionalLight, "Crypt_KeyLight", (0.0, 0.0, 1000.0),
                      unreal.Rotator(-58.0, -35.0, 0.0), "Crypt/Lighting")
    sun.light_component.set_mobility(unreal.ComponentMobility.MOVABLE)
    sun.light_component.set_intensity(4.0)
    sun.light_component.set_light_color(unreal.LinearColor(0.82, 0.88, 1.0, 1.0))
    sun.light_component.set_editor_property("atmosphere_sun_light", True)
    sky = named_actor(subsystem, unreal.SkyLight, "Crypt_AmbientLight", (0.0, 0.0, 1000.0), folder="Crypt/Lighting")
    sky.light_component.set_mobility(unreal.ComponentMobility.MOVABLE)
    sky.light_component.set_intensity(0.8)
    sky.light_component.set_editor_property("real_time_capture", True)
    named_actor(subsystem, unreal.SkyAtmosphere, "Crypt_Atmosphere", (0.0, 0.0, 0.0), folder="Crypt/Lighting")
    unreal.EditorLevelLibrary.set_level_viewport_camera_info(
        unreal.Vector(-2500.0, -2500.0, 3300.0), unreal.Rotator(-50.0, 45.0, 0.0))
    subsystem.set_selected_level_actors([])
    STATE["actor_count"] = len(subsystem.get_all_level_actors())
    verify_ownership(world, subsystem)
    return world


def rebuild_and_verify_navigation(world):
    # Reloading the saved level initializes navigation from its real persisted
    # volume, so this works with -ExecutePythonScript without any Slate ticks.
    if not unreal.EditorLevelLibrary.save_current_level():
        fail("Could not save the crypt slice geometry.")
    STATE["geometry_saved"] = True
    if not unreal.EditorLevelLibrary.load_level(MAP):
        fail("Crypt saved successfully, but could not reload it for navigation validation.")
    editor = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
    subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    world = editor.get_editor_world()
    actors = verify_ownership(world, subsystem)
    nav = unreal.NavigationSystemV1.get_navigation_system(world)
    nav_data = [actor for actor in subsystem.get_all_level_actors() if isinstance(actor, unreal.RecastNavMesh)]
    STATE["navigation_system"] = nav.get_path_name() if nav else None
    STATE["navigation_data"] = [actor.get_path_name() for actor in nav_data]
    if nav:
        nav.on_navigation_bounds_updated(actors["Crypt_NavBounds"])
    if nav and nav_data:
        unreal.SystemLibrary.execute_console_command(world, "RebuildNavigation")
    checks = []
    for name, start, end in (
            ("entrance_to_golem", (-1500.0, 0.0, 0.0), (800.0, 0.0, 0.0)),
            ("golem_to_reward", (800.0, 0.0, 0.0), (2000.0, 1100.0, 0.0)),
            ("charge_west_outside_circle", (800.0, 0.0, 0.0), (-300.0, 0.0, 0.0)),
            ("charge_south_outside_circle", (800.0, 0.0, 0.0), (800.0, -1100.0, 0.0)),
            ("foyer_to_portal", (-1500.0, -450.0, 0.0), (-500.0, 0.0, 0.0))):
        path = unreal.NavigationSystemV1.find_path_to_location_synchronously(world, vec(start), vec(end))
        points = list(path.get_editor_property("path_points")) if path else []
        length = path.get_path_length() if path else 0.0
        valid = bool(path and path.is_valid() and not path.is_partial() and len(points) >= 2)
        checks.append({"route": name, "valid": valid, "point_count": len(points), "length_cm": length,
                       "start": list(start), "end": list(end),
                       "direct_distance_cm": math.dist(start, end)})
    STATE["navigation_checks"] = checks
    STATE["navigation_verified"] = all(check["valid"] for check in checks)
    STATE["actor_count"] = len(subsystem.get_all_level_actors())
    if not unreal.EditorLevelLibrary.save_current_level():
        fail("Could not save the crypt slice after its navigation rebuild.")
    STATE["success"] = True
    STATE["status"] = "complete" if STATE["navigation_verified"] else "saved_pending_navigation_validation"
    if not STATE["navigation_verified"]:
        STATE["navigation_note"] = "Geometry is saved; native real-map acceptance test must rebuild and verify navigation before play."
        unreal.log_warning("MIST_CRYPT_NAV_PENDING: " + STATE["navigation_note"])
    unreal.log("MIST_CRYPT_SLICE_SAVED: " + MAP + " report=" + str(REPORT))


try:
    write_report()
    rebuild_and_verify_navigation(construct())
    write_report()
except Exception as error:
    STATE.update(success=False, status="failed", error=str(error), traceback=traceback.format_exc())
    write_report()
    unreal.log_error("MIST_CRYPT_SLICE_FAILED: " + str(error))
    raise
