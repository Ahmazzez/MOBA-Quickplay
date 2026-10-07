#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/ScopeExit.h"
#include "MistCombatHealth.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMistCombatAttributionTest, "Mistworld.Combat.DamageAttribution",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMistCombatAttributionTest::RunTest(const FString& Parameters)
{
    const UWorld::InitializationValues Init = UWorld::InitializationValues()
        .InitializeScenes(false).AllowAudioPlayback(false).CreatePhysicsScene(false)
        .CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).CreateFXSystem(false);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
        ERHIFeatureLevel::Num, &Init);
    if (!TestNotNull(TEXT("Isolated damage-attribution test world"), World)) return false;
    ON_SCOPE_EXIT { World->DestroyWorld(false); };

    AActor* SourceActor = World->SpawnActor<AActor>();
    AActor* TargetActor = World->SpawnActor<AActor>();
    if (!TestNotNull(TEXT("Damage source actor"), SourceActor)
        || !TestNotNull(TEXT("Damage recipient actor"), TargetActor)) return false;
    auto MakeHealth = [](AActor* Owner)
    {
        UMistCombatHealth* Result = NewObject<UMistCombatHealth>(Owner);
        Owner->AddInstanceComponent(Result);
        Result->RegisterComponent();
        Result->ResetHealth();
        return Result;
    };
    UMistCombatHealth* Source = MakeHealth(SourceActor);
    UMistCombatHealth* HP = MakeHealth(TargetActor);
    int32 HealthLossEvents = 0;
    float ReportedHealthLoss = 0.f;
    AActor* LastDamageSource = nullptr;
    bool bLethalHealthEventSawActiveReduction = false;
    TArray<int32> LethalEventOrder;
    HP->OnHealthLost.AddLambda([&](float ActualHpLoss, AActor* DamageSource)
    {
        ++HealthLossEvents;
        ReportedHealthLoss += ActualHpLoss;
        LastDamageSource = DamageSource;
        if (!HP->IsAlive())
        {
            bLethalHealthEventSawActiveReduction = FMath::IsNearlyEqual(HP->GetDamageReduction(), 0.33f, 0.001f);
            LethalEventOrder.Add(1);
        }
    });

    HP->SetDamageReduction(TEXT("Alphonse.E"), 0.5f, 10.f);
    HP->SetShield(50.f, 10.f);
    // 120 raw -> 100 after 20 armor -> 50 after E -> 50 shield.
    TestEqual(TEXT("Shield absorbs only post-defense damage"), HP->ReceivePhysicalDamage(120.f, SourceActor), 0.f, 0.001f);
    TestEqual(TEXT("Fully absorbed hit leaves HP unchanged"), HP->Health, 1200.f, 0.001f);
    TestEqual(TEXT("Fully absorbed hit consumes fifty shield"), HP->GetShieldRemainingHealth(), 0.f, 0.001f);
    TestEqual(TEXT("Shield absorption never fires actual-HP-loss event"), HealthLossEvents, 0);
    TestEqual(TEXT("A hostile shield hit locks recipient combat for ten seconds"), HP->GetCombatLockRemaining(), 10.f, 0.001f);
    TestEqual(TEXT("A hostile shield hit locks source combat for ten seconds"), Source->GetCombatLockRemaining(), 10.f, 0.001f);

    HP->SetShield(20.f, 10.f);
    TestEqual(TEXT("Partial absorption returns only thirty HP damage"), HP->ReceivePhysicalDamage(120.f, SourceActor), 30.f, 0.001f);
    TestEqual(TEXT("Partial absorption fires one HP event"), HealthLossEvents, 1);
    TestEqual(TEXT("HP event excludes shield absorption"), ReportedHealthLoss, 30.f, 0.001f);
    TestTrue(TEXT("HP event identifies the originating actor"), LastDamageSource == SourceActor);
    TestEqual(TEXT("Magic damage uses the same HP attribution pipeline"), HP->ReceiveMagicDamage(120.f, SourceActor), 50.f, 0.001f);
    TestEqual(TEXT("Magic HP loss contributes actual mitigated health loss"), ReportedHealthLoss, 80.f, 0.001f);

    World->TimeSeconds = 4.0;
    HP->SetShield(100.f, 10.f);
    TestEqual(TEXT("Zero damage produces no actual health loss"), HP->ReceivePhysicalDamage(0.f, SourceActor), 0.f, 0.001f);
    TestEqual(TEXT("Negative damage produces no actual health loss"), HP->ReceivePhysicalDamage(-120.f, SourceActor), 0.f, 0.001f);
    TestEqual(TEXT("Zero/negative damage do not fire HP events"), HealthLossEvents, 2);
    TestEqual(TEXT("Zero/negative damage do not refresh combat lock"), HP->GetCombatLockRemaining(), 6.f, 0.001f);
    TestEqual(TEXT("Zero/negative damage do not refresh source combat lock"), Source->GetCombatLockRemaining(), 6.f, 0.001f);
    TestEqual(TEXT("Healing restores actual missing health"), HP->Heal(80.f, Source), 80.f, 0.001f);
    TestEqual(TEXT("Healing does not refresh combat lock"), HP->GetCombatLockRemaining(), 6.f, 0.001f);
    World->TimeSeconds = 5.0;
    TestEqual(TEXT("A later fully shielded hit still returns no HP damage"), HP->ReceivePhysicalDamage(120.f, SourceActor), 0.f, 0.001f);
    TestEqual(TEXT("Repeated hostile hits refresh recipient's full ten-second lock"), HP->GetCombatLockRemaining(), 10.f, 0.001f);
    TestEqual(TEXT("Repeated hostile hits refresh source's full ten-second lock"), Source->GetCombatLockRemaining(), 10.f, 0.001f);
    TestEqual(TEXT("Shield-only refresh still provides no HP-loss event"), HealthLossEvents, 2);

    World->TimeSeconds = 6.0;
    HP->ResetHealth();
    HP->ClearCombatLock();
    Source->ClearCombatLock();
    HP->ReceivePhysicalDamage(90.f);
    TestTrue(TEXT("One-argument debug damage retains null attribution"), LastDamageSource == nullptr);
    TestEqual(TEXT("Unattributed debug damage does not create hostile combat lock"), HP->GetCombatLockRemaining(), 0.f, 0.001f);
    HP->ReceivePhysicalDamage(90.f, TargetActor);
    TestEqual(TEXT("Self-attributed damage does not create hostile combat lock"), HP->GetCombatLockRemaining(), 0.f, 0.001f);
    TestEqual(TEXT("Debug/self damage may still report actual HP lost"), HealthLossEvents, 4);

    HP->ResetHealth();
    HP->GrantSpawnProtection();
    HP->SetShield(100.f, 20.f);
    TestEqual(TEXT("Immune hostile hits return no HP loss"), HP->ReceivePhysicalDamage(90.f, SourceActor), 0.f, 0.001f);
    TestEqual(TEXT("Immune hits do not consume shields"), HP->GetShieldRemainingHealth(), 100.f, 0.001f);
    TestEqual(TEXT("Immune hits provide no HP-loss event"), HealthLossEvents, 4);
    TestEqual(TEXT("Immune hits do not combat-lock recipient"), HP->GetCombatLockRemaining(), 0.f, 0.001f);
    TestEqual(TEXT("Immune hits do not combat-lock source through damage"), Source->GetCombatLockRemaining(), 0.f, 0.001f);
    HP->EndSpawnProtection();
    HP->SetShield(100.f, 0.5f);
    World->TimeSeconds = 6.5;
    TestEqual(TEXT("Shield expires precisely at its deadline"), HP->GetShieldRemainingHealth(), 0.f, 0.001f);
    TestEqual(TEXT("Expired shield reports zero remaining duration"), HP->GetShieldRemainingTime(), 0.f, 0.001f);
    TestEqual(TEXT("Expired shield no longer absorbs hits"), HP->ReceivePhysicalDamage(90.f, SourceActor), 75.f, 0.001f);

    HP->ResetHealth();
    HP->Health = 10.f;
    HP->SetDamageReduction(TEXT("Alphonse.AbyssE"), 0.33f, 6.f);
    HP->SetShield(20.f, 5.f);
    bool bDefeatObserverSawClearedShieldAndReduction = false;
    HP->OnDefeated.AddLambda([&]()
    {
        LethalEventOrder.Add(2);
        bDefeatObserverSawClearedShieldAndReduction = HP->GetShieldRemainingHealth() == 0.f
            && HP->GetDamageReduction() == 0.f && HP->GetCombatLockRemaining() > 0.f;
    });
    const float ActualLethalDamage = HP->ReceivePhysicalDamage(120.f, SourceActor);
    TestEqual(TEXT("Lethal damage excludes shield absorption and overkill"), ActualLethalDamage, 10.f, 0.001f);
    TestTrue(TEXT("Lethal HP-loss observers see active reduction before death cleanup"), bLethalHealthEventSawActiveReduction);
    TestTrue(TEXT("HP-loss event precedes defeat event"), LethalEventOrder.Num() == 2 && LethalEventOrder[0] == 1 && LethalEventOrder[1] == 2);
    TestTrue(TEXT("Defeat observers see shield/reduction cleared but combat lock preserved"), bDefeatObserverSawClearedShieldAndReduction);
    Source->Health = 500.f;
    TestEqual(TEXT("Damage-derived healing excludes shield damage and overkill"), Source->Heal(ActualLethalDamage * 0.2f), 2.f, 0.001f);
    const int32 EventsAtDeath = HealthLossEvents;
    HP->ReceivePhysicalDamage(9000.f, SourceActor);
    TestEqual(TEXT("Damage to a dead actor provides no additional HP-loss event"), HealthLossEvents, EventsAtDeath);
    HP->ResetHealth();
    TestEqual(TEXT("Revival/reset keeps the existing combat lock"), HP->GetCombatLockRemaining(), 10.f, 0.001f);
    TestEqual(TEXT("Revival/reset keeps shields cleared"), HP->GetShieldRemainingHealth(), 0.f, 0.001f);

    Source->ReceivePhysicalDamage(9000.f);
    World->TimeSeconds = 7.0;
    HP->ReceivePhysicalDamage(90.f, SourceActor);
    TestEqual(TEXT("Released damage still locks its dead source before revival"), Source->GetCombatLockRemaining(), 10.f, 0.001f);
    Source->ResetHealth();
    TestEqual(TEXT("Source revival cannot bypass attributed combat lock"), Source->GetCombatLockRemaining(), 10.f, 0.001f);
    World->TimeSeconds = 17.0;
    TestEqual(TEXT("Recipient combat lock expires exactly ten seconds after last hit"), HP->GetCombatLockRemaining(), 0.f, 0.001f);
    TestEqual(TEXT("Source combat lock expires exactly ten seconds after last hit"), Source->GetCombatLockRemaining(), 0.f, 0.001f);
    HP->MarkCombatActivity(6.f);
    HP->MarkCombatActivity(2.f);
    TestEqual(TEXT("A shorter combat mark cannot shorten the current lock"), HP->GetCombatLockRemaining(), 6.f, 0.001f);
    HP->ClearCombatLock();
    TestEqual(TEXT("Explicit sandbox clear can reset combat lock"), HP->GetCombatLockRemaining(), 0.f, 0.001f);
    return true;
}

#endif
