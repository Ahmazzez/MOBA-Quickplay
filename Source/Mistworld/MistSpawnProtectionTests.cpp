#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/ScopeExit.h"
#include "MistCombatHealth.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMistSpawnProtectionTest, "Mistworld.Combat.SpawnProtection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMistSpawnProtectionTest::RunTest(const FString& Parameters)
{
    const UWorld::InitializationValues Init = UWorld::InitializationValues()
        .InitializeScenes(false).AllowAudioPlayback(false).CreatePhysicsScene(false)
        .CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).CreateFXSystem(false);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
        ERHIFeatureLevel::Num, &Init);
    if (!TestNotNull(TEXT("Isolated spawn-protection test world"), World)) return false;
    ON_SCOPE_EXIT { World->DestroyWorld(false); };

    AActor* Actor = World->SpawnActor<AActor>();
    if (!TestNotNull(TEXT("Protected actor"), Actor)) return false;
    UMistCombatHealth* HP = NewObject<UMistCombatHealth>(Actor);
    Actor->AddInstanceComponent(HP);
    HP->RegisterComponent();
    HP->ResetHealth();

    HP->GrantSpawnProtection();
    TestTrue(TEXT("Default spawn grant activates protection"), HP->IsSpawnProtected());
    TestEqual(TEXT("Default spawn protection lasts three seconds"), HP->GetSpawnProtectionRemaining(), 3.f, 0.001f);
    TestTrue(TEXT("Spawn protection includes damage immunity"), HP->IsInvulnerable());
    TestTrue(TEXT("Spawn protection includes debuff immunity"), HP->IsDebuffImmune());
    TestEqual(TEXT("Physical hits cannot remove protected health"), HP->ReceivePhysicalDamage(9000.f), 0.f, 0.001f);
    TestEqual(TEXT("Magic hits cannot remove protected health"), HP->ReceiveMagicDamage(9000.f), 0.f, 0.001f);
    HP->SetRoot(5.f);
    HP->SetStun(5.f);
    HP->SetSilence(5.f);
    HP->SetAntiHeal(TEXT("Wound"), 0.4f, 5.f);
    TestFalse(TEXT("Protected actor rejects root"), HP->IsRooted());
    TestFalse(TEXT("Protected actor rejects stun"), HP->IsStunned());
    TestFalse(TEXT("Protected actor rejects silence"), HP->IsSilenced());
    TestEqual(TEXT("Protected actor rejects anti-heal"), HP->GetAntiHeal(), 0.f, 0.001f);

    // Possession may grant protection before component BeginPlay. Advance the
    // isolated clock to verify initialization preserves the original expiry.
    World->TimeSeconds = 1.25;
    Actor->DispatchBeginPlay();
    TestTrue(TEXT("Spawn fixture dispatched the real component BeginPlay"), HP->HasBegunPlay());
    TestTrue(TEXT("Actor initialization preserves an earlier spawn grant"), HP->IsSpawnProtected());
    TestEqual(TEXT("BeginPlay does not restart the three-second clock"), HP->GetSpawnProtectionRemaining(), 1.75f, 0.001f);
    HP->Health = 1000.f;
    TestEqual(TEXT("Self-healing is allowed under spawn protection"), HP->Heal(100.f), 100.f, 0.001f);
    TestTrue(TEXT("Self-healing does not end spawn protection"), HP->IsSpawnProtected());
    HP->ClearInvulnerability();
    HP->ClearDebuffImmunity();
    TestTrue(TEXT("Clearing generic damage immunity does not clear spawn immunity"), HP->IsInvulnerable());
    TestTrue(TEXT("Clearing generic debuff immunity does not clear spawn immunity"), HP->IsDebuffImmune());
    World->TimeSeconds = 2.999;
    TestTrue(TEXT("Protection is active immediately before expiry"), HP->IsSpawnProtected());
    World->TimeSeconds = 3.0;
    TestFalse(TEXT("Protection expires exactly at its deadline"), HP->IsSpawnProtected());
    TestEqual(TEXT("Expired protection reports zero remaining time"), HP->GetSpawnProtectionRemaining(), 0.f, 0.001f);
    TestFalse(TEXT("Damage immunity expires with spawn protection"), HP->IsInvulnerable());
    TestFalse(TEXT("Debuff immunity expires with spawn protection"), HP->IsDebuffImmune());
    TestEqual(TEXT("Damage applies normally after expiry"), HP->ReceivePhysicalDamage(90.f), 75.f, 0.001f);
    HP->SetRoot(2.f);
    HP->SetStun(2.f);
    HP->SetSilence(2.f);
    HP->SetAntiHeal(TEXT("Wound"), 0.4f, 5.f);
    TestTrue(TEXT("Root applies after protection expires"), HP->IsRooted());
    TestTrue(TEXT("Stun applies after protection expires"), HP->IsStunned());
    TestTrue(TEXT("Silence applies after protection expires"), HP->IsSilenced());
    TestEqual(TEXT("Anti-heal applies after protection expires"), HP->GetAntiHeal(), 0.4f, 0.001f);

    HP->ResetHealth();
    HP->GrantSpawnProtection();
    HP->GrantInvulnerability(5.f);
    HP->GrantDebuffImmunity(7.f);
    TestEqual(TEXT("Damage immunity remaining time includes the longest source"), HP->GetInvulnerabilityRemaining(), 5.f, 0.001f);
    HP->EndSpawnProtection();
    TestFalse(TEXT("Hostile commitment ends the spawn source"), HP->IsSpawnProtected());
    TestTrue(TEXT("Ending spawn immunity preserves unrelated damage immunity"), HP->IsInvulnerable());
    TestTrue(TEXT("Ending spawn immunity preserves unrelated debuff immunity"), HP->IsDebuffImmune());
    TestEqual(TEXT("Ending spawn protection leaves generic duration unchanged"), HP->GetInvulnerabilityRemaining(), 5.f, 0.001f);
    HP->ClearInvulnerability();
    TestFalse(TEXT("Generic damage immunity can still be ended independently"), HP->IsInvulnerable());
    TestTrue(TEXT("Ending generic damage immunity does not end generic debuff immunity"), HP->IsDebuffImmune());

    HP->GrantSpawnProtection();
    HP->ResetHealth();
    TestFalse(TEXT("Explicit reset clears spawn protection"), HP->IsSpawnProtected());
    TestFalse(TEXT("Explicit reset clears all damage immunity"), HP->IsInvulnerable());
    TestFalse(TEXT("Explicit reset clears all debuff immunity"), HP->IsDebuffImmune());

    int32 DefeatEvents = 0;
    bool bDefeatObserverSawClearedEffects = false;
    HP->OnDefeated.AddLambda([HP, &DefeatEvents, &bDefeatObserverSawClearedEffects]()
    {
        ++DefeatEvents;
        bDefeatObserverSawClearedEffects = !HP->IsAlive() && !HP->IsSpawnProtected()
            && !HP->IsInvulnerable() && !HP->IsDebuffImmune()
            && !HP->IsRooted() && !HP->IsStunned() && !HP->IsSilenced()
            && HP->GetDamageReduction() == 0.f && HP->GetAntiHeal() == 0.f
            && HP->GetOutgoingHealingBonus() == 0.f && HP->GetIncomingHealingBonus() == 0.f;
    });
    HP->Health = 1.f;
    HP->SetDamageReduction(TEXT("Alphonse.E"), 0.5f, 5.f);
    HP->SetRoot(2.f);
    HP->SetStun(2.f);
    HP->SetSilence(2.f);
    HP->SetAntiHeal(TEXT("Wound"), 0.4f, 5.f);
    HP->SetOutgoingHealingBonus(0.2f);
    HP->SetIncomingHealingBonus(0.1f);
    TestEqual(TEXT("Lethal damage returns only remaining health"), HP->ReceivePhysicalDamage(90.f), 1.f, 0.001f);
    TestEqual(TEXT("Lethal transition emits one defeat event"), DefeatEvents, 1);
    TestTrue(TEXT("Defeat delegates see temporary effects already cleared"), bDefeatObserverSawClearedEffects);
    HP->ReceiveMagicDamage(90.f);
    TestEqual(TEXT("Further damage to a dead actor emits no second event"), DefeatEvents, 1);
    HP->ResetHealth();
    TestEqual(TEXT("Revival/reset does not emit a defeat event"), DefeatEvents, 1);
    HP->ReceivePhysicalDamage(90000.f);
    TestEqual(TEXT("A new life may emit its own defeat event"), DefeatEvents, 2);
    return true;
}

#endif
