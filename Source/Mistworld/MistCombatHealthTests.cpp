#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/ScopeExit.h"
#include "MistCombatHealth.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMistCombatHealthRulesTest, "Mistworld.Combat.HealthRules",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMistCombatHealthRulesTest::RunTest(const FString& Parameters)
{
    const UWorld::InitializationValues Init = UWorld::InitializationValues()
        .InitializeScenes(false).AllowAudioPlayback(false).CreatePhysicsScene(false)
        .CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).CreateFXSystem(false);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
        ERHIFeatureLevel::Num, &Init);
    if (!TestNotNull(TEXT("Isolated combat test world"), World)) return false;
    ON_SCOPE_EXIT { World->DestroyWorld(false); };

    AActor* SourceActor = World->SpawnActor<AActor>();
    AActor* TargetActor = World->SpawnActor<AActor>();
    if (!TestNotNull(TEXT("Healing source actor"), SourceActor)
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
    UMistCombatHealth* Target = MakeHealth(TargetActor);

    TestEqual(TEXT("Baseline physical mitigation: 90 against 20 armor"),
        Target->ReceivePhysicalDamage(90.f), 75.f, 0.001f);
    Target->ResetHealth();
    Target->SetDamageReduction(TEXT("Alphonse.E"), 0.5f, 5.f);
    TestEqual(TEXT("E halves damage after armor"), Target->ReceivePhysicalDamage(90.f), 37.5f, 0.001f);
    Target->SetDamageReduction(TEXT("Windwitch.E"), 0.15f, 3.f);
    TestEqual(TEXT("Distinct reductions multiply to 57.5%"), Target->GetDamageReduction(), 0.575f, 0.001f);
    TestEqual(TEXT("57.5% reduction applies after armor"), Target->ReceivePhysicalDamage(90.f), 31.875f, 0.001f);
    Target->SetDamageReduction(TEXT("Alphonse.E"), 0.2f, 9.f);
    TestEqual(TEXT("Duplicate weaker named buff does not stack or replace stronger"),
        Target->GetDamageReduction(), 0.575f, 0.001f);
    TestEqual(TEXT("Named reapplication refreshes its duration"),
        Target->GetDamageReductionRemaining(TEXT("Alphonse.E")), 9.f, 0.001f);
    Target->SetDamageReduction(TEXT("Another.Defense"), 0.75f, 5.f);
    TestEqual(TEXT("Total generic reduction caps at 75%"), Target->GetDamageReduction(), 0.75f, 0.001f);
    TestEqual(TEXT("75% cap preserves a quarter of post-armor damage"),
        Target->ReceivePhysicalDamage(90.f), 18.75f, 0.001f);
    Target->ResetHealth();
    TestEqual(TEXT("Reset removes reductions"), Target->GetDamageReduction(), 0.f, 0.001f);
    Target->MagicResistance = 100.f;
    TestEqual(TEXT("Magic damage uses magic resistance"), Target->ReceiveMagicDamage(90.f), 45.f, 0.001f);
    TestEqual(TEXT("Physical damage still uses physical armor"), Target->ReceivePhysicalDamage(90.f), 75.f, 0.001f);
    Target->ResetHealth();
    Target->GrantInvulnerability(3.f);
    TestTrue(TEXT("Spawn-style immunity is active"), Target->IsInvulnerable());
    TestEqual(TEXT("Immunity prevents physical health loss"), Target->ReceivePhysicalDamage(9000.f), 0.f, 0.001f);
    TestEqual(TEXT("Immunity prevents magic health loss"), Target->ReceiveMagicDamage(9000.f), 0.f, 0.001f);
    Target->ClearInvulnerability();
    TestFalse(TEXT("Committed hostile action may clear immunity"), Target->IsInvulnerable());

    // Healing attribution must use the returned HP damage, rather than raw damage or overkill.
    Target->Health = 40.f;
    Source->Health = 1000.f;
    const float HealthDamage = Target->ReceivePhysicalDamage(90.f);
    TestEqual(TEXT("Overkill returns only removed HP"), HealthDamage, 40.f, 0.001f);
    TestEqual(TEXT("20% ultimate healing excludes overkill"), Source->Heal(0.2f * HealthDamage), 8.f, 0.001f);
    TestEqual(TEXT("A dead recipient cannot be healed without revival"), Target->Heal(100.f), 0.f, 0.001f);

    Target->ResetHealth();
    Target->Health = 500.f;
    Source->SetOutgoingHealingBonus(0.2f);
    Target->SetIncomingHealingBonus(0.1f);
    Target->SetAntiHeal(TEXT("Wound"), 0.4f, 5.f);
    TestEqual(TEXT("Healing modifiers match 100 x 1.2 x 1.1 x 0.6"),
        Target->Heal(100.f, Source), 79.2f, 0.001f);
    Target->SetAntiHeal(TEXT("WeakerWound"), 0.2f, 5.f);
    TestEqual(TEXT("Different anti-heals use strongest"), Target->GetAntiHeal(), 0.4f, 0.001f);
    Source->SetOutgoingHealingBonus(1.f);
    Target->SetIncomingHealingBonus(1.f);
    Target->SetAntiHeal(TEXT("Wound"), 1.f, 5.f);
    TestEqual(TEXT("Outgoing bonus caps at 50%"), Source->GetOutgoingHealingBonus(), 0.5f, 0.001f);
    TestEqual(TEXT("Incoming bonus caps at 50%"), Target->GetIncomingHealingBonus(), 0.5f, 0.001f);
    TestEqual(TEXT("Anti-heal caps at 75%"), Target->GetAntiHeal(), 0.75f, 0.001f);
    TestEqual(TEXT("Capped categories combine multiplicatively"), Target->Heal(100.f, Source), 56.25f, 0.001f);
    Target->Health = Target->MaxHealth - 3.f;
    TestEqual(TEXT("Delivered healing clamps to missing HP"), Target->Heal(100.f, Source), 3.f, 0.001f);
    TestEqual(TEXT("Full health cannot produce extra healing"), Target->Heal(100.f, Source), 0.f, 0.001f);

    Source->ResetHealth();
    Source->SetOutgoingHealingModifier(TEXT("Blessing"), 0.2f, 5.f);
    Source->SetOutgoingHealingModifier(TEXT("Blessing"), 0.1f, 5.f);
    Source->SetOutgoingHealingModifier(TEXT("Equipment"), 0.25f, 5.f);
    TestEqual(TEXT("Named healing bonuses add; duplicates retain strongest"),
        Source->GetOutgoingHealingBonus(), 0.45f, 0.001f);
    Source->SetOutgoingHealingModifier(TEXT("Extra"), 0.2f, 5.f);
    TestEqual(TEXT("Summed named healing bonuses respect category cap"),
        Source->GetOutgoingHealingBonus(), 0.5f, 0.001f);

    Target->GrantInvulnerability(3.f);
    Target->SetDamageReduction(TEXT("Alphonse.E"), 0.5f, 5.f);
    Target->ResetHealth();
    TestFalse(TEXT("Reset clears spawn immunity"), Target->IsInvulnerable());
    TestEqual(TEXT("Reset clears anti-heal"), Target->GetAntiHeal(), 0.f, 0.001f);
    TestEqual(TEXT("Reset clears incoming healing bonuses"), Target->GetIncomingHealingBonus(), 0.f, 0.001f);
    TestEqual(TEXT("Reset clears E duration"), Target->GetDamageReductionRemaining(TEXT("Alphonse.E")), 0.f, 0.001f);
    Target->GrantDebuffImmunity(3.f);
    Target->SetRoot(2.f);
    Target->SetStun(2.f);
    Target->SetSilence(2.f);
    Target->SetAntiHeal(TEXT("Wound"), 0.4f, 5.f);
    TestFalse(TEXT("Debuff immunity rejects root"), Target->IsRooted());
    TestFalse(TEXT("Debuff immunity rejects stun"), Target->IsStunned());
    TestFalse(TEXT("Debuff immunity rejects silence"), Target->IsSilenced());
    TestEqual(TEXT("Debuff immunity rejects anti-heal"), Target->GetAntiHeal(), 0.f, 0.001f);
    Target->ClearDebuffImmunity();
    Target->GrantInvulnerability(3.f);
    Target->SetRoot(2.f);
    Target->SetStun(2.f);
    Target->SetSilence(2.f);
    TestTrue(TEXT("Damage-only immunity does not reject root"), Target->IsRooted());
    TestTrue(TEXT("Damage-only immunity does not reject stun"), Target->IsStunned());
    TestTrue(TEXT("Damage-only immunity does not reject silence"), Target->IsSilenced());
    Target->ResetHealth();
    TestFalse(TEXT("Reset clears root"), Target->IsRooted());
    TestFalse(TEXT("Reset clears stun"), Target->IsStunned());
    TestFalse(TEXT("Reset clears silence"), Target->IsSilenced());
    TestFalse(TEXT("Reset clears debuff immunity"), Target->IsDebuffImmune());
    return true;
}

#endif
