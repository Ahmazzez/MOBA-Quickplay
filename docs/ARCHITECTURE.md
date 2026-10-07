# Architecture notes

Mistworld is currently a local, single-player Unreal C++ prototype. Runtime gameplay is implemented in the `Mistworld` module. Editor Python scripts create test maps and import art; they are development tools, not gameplay services.

## Main boundaries

| Responsibility | Implementation |
| --- | --- |
| Player input and ability lifecycle | [`AMistworldPlayerController`](../Source/Mistworld/MistworldPlayerController.h) owns click orders, held targeting, cast commits, cooldown deadlines, basic-attack targeting, and Alphonse's ability orchestration. Anima dispatches to a dedicated combat component. |
| Hero body and movement | [`AMistworldCharacter`](../Source/Mistworld/MistworldCharacter.h) provides the pawn, capsule, and camera. [`UMistConstantSpeedPathFollowingComponent`](../Source/Mistworld/MistConstantSpeedPathFollowingComponent.cpp) requests full-speed velocity on every path segment and stops at the destination without arrival deceleration. |
| Shared combat math | [`UMistCombatHealth`](../Source/Mistworld/MistCombatHealth.h) resolves armor/resistance, multiplicative reduction, shields, actual HP loss, healing, protection, and timed crowd control. Damage kind and source are explicit so passives can distinguish basic attacks, direct hits, and DOT. |
| Ability definitions | [`MistAlphonseAbilityDefinitions`](../Source/Mistworld/MistAlphonseAbilityDefinitions.cpp) and [`MistAnimaAbilityDefinitions`](../Source/Mistworld/MistAnimaAbilityDefinitions.cpp) contain provisional C++ values. Each slot resolves independently for mixed base/ascension kits. These are code definitions, rather than a finished designer-facing data-asset system. |
| Anima combat | [`UMistAnimaCombat`](../Source/Mistworld/MistAnimaCombat.h) owns marks, brands, parry interception, multi-dash recasts, healing theft, and burn credit. A target-owned burn ledger tracks independently timed, caster-attributed stacks and aggregates the boss DOT cap. |
| Released Alphonse effects | [`MistReturningSword`](../Source/Mistworld/MistReturningSword.cpp) and the ability-effect actors handle travel and delayed damage. Sword-wall contact starts its return; catching it enables an empowered dash using the currently equipped W. |
| Dungeon lifecycle | [`MistCryptSession`](../Source/Mistworld/MistCryptSession.cpp) manages the gate, checkpoints, completion, reward interaction, and session-local replays. [`MistCryptGolem`](../Source/Mistworld/MistCryptGolem.cpp) manages protection/exposure phases; [`MistCryptGuardian`](../Source/Mistworld/MistCryptGuardian.cpp) owns the three elemental behavior profiles. |
| Progression | [`UMistProgressionComponent`](../Source/Mistworld/MistProgressionComponent.h) validates and persists progression. [`UMistCryptSaveGame`](../Source/Mistworld/MistCryptSaveGame.h) is version 3 and retains legacy fields to migrate version 1/2 saves. Failed writes do not commit new rewards or equipment; save revisions reject stale writes. |
| Presentation | [`MistMeleePresentation`](../Source/Mistworld/MistMeleePresentation.cpp), [`MistAnimaPresentation`](../Source/Mistworld/MistAnimaPresentation.cpp), and [`MistGolemPresentation`](../Source/Mistworld/MistGolemPresentation.cpp) map combat actions to placeholder meshes and clips. Their root motion is disabled; combat movement stays authoritative locally. |
| HUD | [`MistCombatHUD`](../Source/Mistworld/MistCombatHUD.cpp) draws the fantasy interface and dispatches menu clicks. [`MistInventoryPreview`](../Source/Mistworld/MistInventoryPreview.cpp) renders a separate cosmetic character preview without changing the player's transform. |

## Important behavior contracts

- Aiming previews an ability without starting a cast or consuming a cooldown. The character can move until the cast commits.
- Health-based refunds and healing use actual HP damage, excluding absorbed shields and overkill. Death cannot be undone by delayed healing.
- Loadout and hero switches require a safe checkpoint and no ongoing combat, recasts, or effects. A legal swap retains cooldown deadlines. Inactive heroes' cooldowns continue to elapse.
- Spawn protection lasts three seconds and ends when the player commits a hostile action. Flash has a separate fixed 60-second cooldown.
- Wiping an encounter clears released attempt effects. Replaying a cleared boss is session-local and retains the earned clear and reward.
- The sole equipment reward belongs to Alphonse's Greatsword Blade slot. Anima has separate light-armor and rapier/dagger slots. Weapon visuals remain fixed placeholders regardless of equipment.

## Review and verification

Tests live beside the implementation as `*Tests.cpp` and register under the `Mistworld` automation prefix. Pure combat checks use small fixtures; navigation, actual input, hero switching, and dungeon progression also have editor Play-In-Editor integration checks. Presentation checks verify imported assets, usable texture sources, skeletons, and animation root constraints. Rendered appearance is checked separately because NullRHI automation does not show pixels.

Representative tests:

- [`MistAnimaIntegrationTests.cpp`](../Source/Mistworld/MistAnimaIntegrationTests.cpp): both kits and mixed configurations through actual controller input.
- [`MistConstantSpeedMovementTests.cpp`](../Source/Mistworld/MistConstantSpeedMovementTests.cpp): close destinations, turns, and final-segment movement.
- [`MistHeroCryptSwitchTests.cpp`](../Source/Mistworld/MistHeroCryptSwitchTests.cpp): checkpoint switching and persistence across Play sessions.
- [`MistProgressionTests.cpp`](../Source/Mistworld/MistProgressionTests.cpp): migration, malformed saves, failure rollback, and stale revision protection.
- [`MistCryptEncounterTests.cpp`](../Source/Mistworld/MistCryptEncounterTests.cpp): protection phases and fire/ice/wind behavior.

## Current constraints

The controller still combines input with a substantial amount of Alphonse's combat orchestration. Extracting a shared ability execution layer and designer-editable definitions would make additional heroes easier to maintain. Animation and art are placeholders, and current values are tuning baselines.

There is no replicated combat, server authority, online persistence, matchmaking, or backend. Before expanding into an MMO, the next technical milestone is a small two-player test that establishes authoritative damage, movement/ability replication, encounter ownership, and progression boundaries. Multiplayer capability should not be inferred from the prototype's local combat tests.
