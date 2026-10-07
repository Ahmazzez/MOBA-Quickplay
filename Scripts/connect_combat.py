"""One-time editor commandlet: select combat classes in the existing game mode."""
import unreal

mode = unreal.load_asset('/Game/TopDown/Blueprints/BP_TopDownGameMode')
if not mode:
    raise RuntimeError('Top Down game mode asset not found')
mode_class = unreal.load_class(None, '/Game/TopDown/Blueprints/BP_TopDownGameMode.BP_TopDownGameMode_C')
controller = unreal.load_class(None, '/Script/Mistworld.MistworldPlayerController')
hud = unreal.load_class(None, '/Script/Mistworld.MistCombatHUD')
if not mode_class or not controller or not hud:
    raise RuntimeError('Combat classes were not loaded; compile first')
defaults = unreal.get_default_object(mode_class)
unreal.log('Previous controller: ' + str(defaults.get_editor_property('player_controller_class')))
mode.modify()
defaults.modify()
defaults.set_editor_property('player_controller_class', controller)
defaults.set_editor_property('hud_class', hud)
if not unreal.EditorAssetLibrary.save_loaded_asset(mode, only_if_is_dirty=False):
    raise RuntimeError('Could not save combat game mode settings')
unreal.log('MIST_COMBAT_WIRING_SAVED: controller=' + str(defaults.get_editor_property('player_controller_class'))
           + ', HUD=' + str(defaults.get_editor_property('hud_class')))
