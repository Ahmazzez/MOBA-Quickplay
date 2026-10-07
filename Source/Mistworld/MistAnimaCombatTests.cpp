#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "MistAnimaCombat.h"
#include "MistAnimaAbilityDefinitions.h"
#include "MistworldPlayerController.h"
#include "MistCombatHealth.h"
#include "MistTrainingDummy.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "TimerManager.h"

struct FMistAnimaCombatTestAccess
{
    static void Mark(UMistAnimaCombat* Combat, AActor* Target) { Combat->ApplyMark(Target); }
    static void Brand(UMistAnimaCombat* Combat, AActor* Target) { Combat->ApplyBrand(Target); }
    static void Burns(UMistAnimaCombat* Combat, AActor* Target, int32 Count) { Combat->AddBurn(Target, Count); }
    static void Aura(UMistAnimaCombat* Combat, float Duration = 8.f)
    { Combat->AuraUntil = Combat->Now() + Duration; Combat->NextAuraTick = Combat->Now() + 1.0; }
    static void Stance(UMistAnimaCombat* Combat, bool bFlame)
    {
        Combat->bFlameStance = bFlame; Combat->StanceUntil = Combat->Now() + 1.0;
        Combat->HeroHealth->SetDamageReduction(TEXT("Anima.Riposte"), bFlame ? 0.f : 0.70f, 1.f);
    }
    static double ParryReady(const UMistAnimaCombat* Combat) { return Combat->AutoParryReadyAt; }
    static void Counter(UMistAnimaCombat* Combat) { Combat->ReleaseCounter(); }
    static void ReleaseQ(UMistAnimaCombat* Combat, bool bFlame)
    {
        Combat->PendingDefinition = FMistAnimaAbilityDefinitions::Get(EKeys::Q, bFlame);
        Combat->bPendingAlternate = bFlame; Combat->PendingDamageMultiplier = 1.f;
        Combat->CastDirection = FVector::ForwardVector; Combat->ReleaseThrust();
    }
    static void DashContacts(UMistAnimaCombat* Combat, bool bFlame, const FVector& From, const FVector& To)
    {
        Combat->PendingDefinition = FMistAnimaAbilityDefinitions::Get(EKeys::W, bFlame);
        Combat->PendingDamageMultiplier = 1.f; Combat->bFlameDash = bFlame;
        Combat->bDashHealUsed = false; Combat->DashHits.Reset();
        Combat->CastDirection = (To - From).GetSafeNormal2D(); Combat->ApplyDashHits(From, To);
    }
    static void OpenRecasts(UMistAnimaCombat* Combat)
    { Combat->DashCommitments = 1; Combat->DashRecastUntil = Combat->Now() + 5.0; Combat->NextDashCommitAt = Combat->Now() + 0.20; }
    static void ExpireRecasts(UMistAnimaCombat* Combat) { Combat->FinishDashSequence(); }
};

namespace
{
struct FAnimaWorld
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    AMistworldPlayerController* Controller = nullptr;
    ACharacter* Hero = nullptr;
    UMistCombatHealth* Health = nullptr;
    UMistAnimaCombat* Combat = nullptr;
    FAnimaWorld()
    {
        if (GEngine) GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
        FActorSpawnParameters Params;
        Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        Hero = World->SpawnActor<ACharacter>(FVector(0.f, 0.f, 98.f), FRotator::ZeroRotator, Params);
        Controller = World->SpawnActor<AMistworldPlayerController>();
        Controller->SetPawn(Hero); // Isolate combat from saves and checkpoint setup.
        Health = MakeHealth(Hero);
        Health->MaxHealth = 1000.f; Health->ResetHealth(); Health->Health = 500.f;
        Combat = NewObject<UMistAnimaCombat>(Hero);
        Hero->AddInstanceComponent(Combat); Combat->RegisterComponent();
        Combat->Initialize(Controller); Combat->SetHeroActive(true);
    }
    ~FAnimaWorld()
    {
        Controller->SetPawn(nullptr);
        World->DestroyWorld(false);
        if (GEngine) GEngine->DestroyWorldContext(World);
    }
    UMistCombatHealth* MakeHealth(AActor* Owner)
    {
        UMistCombatHealth* HP = NewObject<UMistCombatHealth>(Owner);
        Owner->AddInstanceComponent(HP); HP->RegisterComponent(); HP->ResetHealth();
        return HP;
    }
    AMistTrainingDummy* Target(float X = 150.f, float Y = 0.f)
    {
        FActorSpawnParameters Params;
        Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        AMistTrainingDummy* Result = World->SpawnActor<AMistTrainingDummy>(FVector(X, Y, 98.f), FRotator::ZeroRotator, Params);
        Result->CombatHealth->ResetHealth();
        return Result;
    }
    void Time(double NewTime) { World->TimeSeconds = NewTime; }
    void TickBurns(AActor* Target)
    {
        if (UMistAnimaBurnStatus* Status = Target->FindComponentByClass<UMistAnimaBurnStatus>())
            Status->TickComponent(0.01f, LEVELTICK_All, nullptr);
    }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMistAnimaMarksTest, "Mistworld.Anima.MarkBrandAndMixedDefinitions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FMistAnimaMarksTest::RunTest(const FString& Parameters)
{
    FAnimaWorld F;
    AMistTrainingDummy* First = F.Target(120.f);
    AMistTrainingDummy* Second = F.Target(260.f);
    FMistAnimaCombatTestAccess::ReleaseQ(F.Combat, false);
    TestEqual(TEXT("Q hits only the first body in its thrust"), First->CombatHealth->Health, 1200.f - 70.f / 1.2f, 0.01f);
    TestEqual(TEXT("Second target is not pierced"), Second->CombatHealth->Health, 1200.f, 0.01f);
    TestEqual(TEXT("Q applies Mark before its own 15% actual-damage healing"), F.Health->Health, 500.f + 70.f / 1.2f * 0.15f, 0.01f);
    FMistAnimaCombatTestAccess::Brand(F.Combat, First);
    FMistAnimaCombatTestAccess::Mark(F.Combat, Second);
    TestTrue(TEXT("Moving the sole Mark preserves the independent Brand"), F.Combat->GetMarkedTarget() == Second && F.Combat->GetBrandedTarget() == First);
    FMistAnimaCombatTestAccess::Mark(F.Combat, First);
    F.Combat->BeginAutoSwing(FVector::ForwardVector);
    F.Time(0.18);
    F.Combat->ApplyBasicAttackDamage(First);
    TestEqual(TEXT("Brand adds one independent burn on a successful ordinary auto"), F.Combat->GetBurnCount(First), 1);
    First->CombatHealth->DamageInterceptor.BindLambda([](float, AActor*, EMistDamageKind) { return true; });
    const float HPBeforeBlock = F.Health->Health;
    F.Combat->ApplyBasicAttackDamage(First);
    TestEqual(TEXT("A fully blocked branded attack cannot apply burn"), F.Combat->GetBurnCount(First), 1);
    TestEqual(TEXT("A fully blocked attack supplies no Mark healing"), F.Health->Health, HPBeforeBlock, 0.01f);
    First->CombatHealth->DamageInterceptor.Unbind();
    First->CombatHealth->SetShield(500.f, 5.f);
    F.Combat->ApplyBasicAttackDamage(First);
    TestEqual(TEXT("Shield absorption is a successful on-hit for Brand"), F.Combat->GetBurnCount(First), 2);
    TestEqual(TEXT("Shield damage supplies no Mark healing"), F.Health->Health, HPBeforeBlock, 0.01f);
    F.Time(4.19);
    TestNull(TEXT("Mark expires after four seconds"), F.Combat->GetMarkedTarget());
    TestNull(TEXT("Brand expires independently after four seconds"), F.Combat->GetBrandedTarget());
    for (int32 Mask = 0; Mask < 16; ++Mask)
    {
        const FKey Keys[] = { EKeys::Q, EKeys::W, EKeys::E, EKeys::R };
        const float Cooldowns[] = { 2.f, 12.f, 15.f, 110.f };
        for (int32 Slot = 0; Slot < 4; ++Slot)
        {
            const auto Def = FMistAnimaAbilityDefinitions::Get(Keys[Slot], (Mask & (1 << Slot)) != 0);
            TestTrue(FString::Printf(TEXT("Mixed mask %d slot %d has an independent equipped definition"), Mask, Slot), !Def.DisplayName.IsEmpty());
            TestEqual(TEXT("Ascension selection retains authored slot cooldown"), Def.Cooldown, Cooldowns[Slot]);
        }
    }
    F.Combat->ResetAttempt();
    TestEqual(TEXT("Attempt reset removes this caster's outgoing burns"), F.Combat->GetBurnCount(First), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMistAnimaParryTest, "Mistworld.Anima.ParryAndRiposte",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FMistAnimaParryTest::RunTest(const FString& Parameters)
{
    FAnimaWorld F;
    AMistTrainingDummy* Attacker = F.Target(140.f);
    F.Combat->BeginAutoSwing(FVector::ForwardVector);
    F.Time(0.18);
    TestEqual(TEXT("Rapier hit-window parries a frontal incoming basic attack"),
        F.Health->ReceivePhysicalDamage(120.f, Attacker, EMistDamageKind::BasicAttack), 0.f);
    TestEqual(TEXT("Auto parry heals would-be post-armor damage"), F.Health->Health, 600.f, 0.01f);
    TestTrue(TEXT("Successful parry starts a three-second internal cooldown"), FMistAnimaCombatTestAccess::ParryReady(F.Combat) > 3.17);
    TestEqual(TEXT("Another basic attack during passive cooldown lands"),
        F.Health->ReceivePhysicalDamage(120.f, Attacker, EMistDamageKind::BasicAttack), 100.f, 0.01f);
    F.Time(3.5); F.Combat->BeginAutoSwing(FVector::ForwardVector); F.Time(3.68);
    TestEqual(TEXT("Ability damage never triggers the basic-only parry"), F.Health->ReceivePhysicalDamage(120.f, Attacker), 100.f, 0.01f);
    Attacker->SetActorLocation(FVector(0.f, 180.f, 98.f));
    TestEqual(TEXT("Basic attacks outside the forward rapier cone cannot be parried"),
        F.Health->ReceivePhysicalDamage(120.f, Attacker, EMistDamageKind::BasicAttack), 100.f, 0.01f);
    Attacker->SetActorLocation(FVector(140.f, 0.f, 98.f));
    FMistAnimaCombatTestAccess::Stance(F.Combat, true);
    const double PassiveReady = FMistAnimaCombatTestAccess::ParryReady(F.Combat);
    TestEqual(TEXT("Flame stance blocks a DOT event completely"),
        F.Health->ReceiveMagicDamage(120.f, Attacker, EMistDamageKind::DamageOverTime), 0.f);
    TestEqual(TEXT("Full Flame block does not itself heal"), F.Health->Health, 300.f, 0.01f);
    TestEqual(TEXT("Flame E takes priority without consuming auto-parry cooldown"), FMistAnimaCombatTestAccess::ParryReady(F.Combat), PassiveReady);
    FMistAnimaCombatTestAccess::Brand(F.Combat, Attacker);
    const float TargetHP = Attacker->CombatHealth->Health;
    F.Combat->ApplyBasicAttackDamage(Attacker);
    TestEqual(TEXT("Enhanced autos receive exactly 20% extra damage"), Attacker->CombatHealth->Health, TargetHP - 84.f / 1.2f, 0.01f);
    TestEqual(TEXT("Enhanced branded attack applies two total stacks, not three"), F.Combat->GetBurnCount(Attacker), 2);
    F.Combat->ResetAttempt();
    F.Health->ResetHealth(); F.Health->Health = 500.f;
    FMistAnimaCombatTestAccess::Mark(F.Combat, Attacker);
    FMistAnimaCombatTestAccess::Stance(F.Combat, false);
    TestEqual(TEXT("Riposte mitigates DOT but DOT cannot trigger its counter"),
        F.Health->ReceiveMagicDamage(120.f, Attacker, EMistDamageKind::DamageOverTime), 30.f, 0.01f);
    TestTrue(TEXT("Riposte remains armed after a DOT tick"), F.Combat->GetDefenseRemaining() > 0.f);
    F.Health->SetShield(200.f, 5.f);
    TestEqual(TEXT("Direct shield damage can trigger Riposte without HP loss"), F.Health->ReceivePhysicalDamage(120.f, Attacker), 0.f, 0.01f);
    TestEqual(TEXT("Counter ends Riposte's mitigation immediately"), F.Health->GetDamageReductionRemaining(TEXT("Anima.Riposte")), 0.f);
    FMistAnimaCombatTestAccess::Counter(F.Combat);
    TestEqual(TEXT("Marked counter heals 30% total, never 45%"), F.Health->Health, 470.f + 110.f / 1.2f * 0.30f, 0.01f);
    F.Combat->ResetAttempt();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMistAnimaDashTest, "Mistworld.Anima.DashContactHealingAndRecasts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FMistAnimaDashTest::RunTest(const FString& Parameters)
{
    FAnimaWorld F;
    AMistTrainingDummy* First = F.Target(100.f);
    AMistTrainingDummy* Second = F.Target(220.f);
    const FVector From(0.f, 0.f, 98.f), To(300.f, 0.f, 98.f);
    FMistAnimaCombatTestAccess::DashContacts(F.Combat, false, From, To);
    TestEqual(TEXT("Original W heals once per dash even through several enemies"), F.Health->Health, 550.f, 0.01f);
    TestEqual(TEXT("Original dash damages each contacted enemy once"), Second->CombatHealth->Health, 1200.f - 45.f / 1.2f, 0.01f);
    F.Combat->ResetAttempt(); F.Health->Health = 500.f;
    FMistAnimaCombatTestAccess::Burns(F.Combat, First, 4);
    FMistAnimaCombatTestAccess::Burns(F.Combat, Second, 10);
    FMistAnimaCombatTestAccess::DashContacts(F.Combat, true, From, To);
    TestEqual(TEXT("Flame W counts first target's own preexisting stacks before adding two"), F.Health->Health, 520.f, 0.01f);
    TestEqual(TEXT("Flame dash adds two independent burn stacks"), F.Combat->GetBurnCount(First), 6);
    TestEqual(TEXT("Each caster's burn stack limit remains ten"), F.Combat->GetBurnCount(Second), 10);
    F.Combat->ResetAttempt(); F.Health->Health = 500.f;
    First->CombatHealth->bIsMinion = true;
    FMistAnimaCombatTestAccess::Burns(F.Combat, First, 10);
    FMistAnimaCombatTestAccess::Burns(F.Combat, Second, 2);
    FMistAnimaCombatTestAccess::DashContacts(F.Combat, true, From, To);
    TestEqual(TEXT("Minions cannot supply Flame W healing; first non-minion can"), F.Health->Health, 510.f, 0.01f);
    F.Combat->ResetAttempt(); F.Health->Health = 500.f;
    First->CombatHealth->GrantInvulnerability(5.f); Second->CombatHealth->GrantInvulnerability(5.f);
    FMistAnimaCombatTestAccess::DashContacts(F.Combat, false, From, To);
    TestEqual(TEXT("Immune contact never supplies original W healing"), F.Health->Health, 500.f, 0.01f);
    FMistAnimaCombatTestAccess::OpenRecasts(F.Combat);
    TestFalse(TEXT("W recasts respect the 0.20-second minimum interval"), F.Combat->CanRecast(EKeys::W));
    F.Time(0.21);
    TestTrue(TEXT("Second dash becomes aimable after minimum interval"), F.Combat->CanRecast(EKeys::W));
    TestEqual(TEXT("Two dash charges remain after first commitment"), F.Combat->GetDashChargesRemaining(), 2);
    F.Time(5.01);
    F.Combat->TickComponent(0.01f, LEVELTICK_All, nullptr);
    TestFalse(TEXT("Five idle seconds expire the staged dash"), F.Combat->CanRecast(EKeys::W));
    TestTrue(TEXT("W's twelve-second cooldown begins only when sequence expires"), F.Controller->GetCooldownRemaining(EKeys::W) > 11.99f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMistAnimaBurnTest, "Mistworld.Anima.BurnAttributionAndBossCap",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FMistAnimaBurnTest::RunTest(const FString& Parameters)
{
    FAnimaWorld F;
    AMistTrainingDummy* Target = F.Target();
    Target->CombatHealth->MaxHealth = 3000.f; Target->CombatHealth->ResetHealth();
    Target->CombatHealth->PercentageBurnMaxTick = 10.f;
    AActor* SecondCaster = F.World->SpawnActor<AActor>();
    UMistCombatHealth* SecondHP = F.MakeHealth(SecondCaster); SecondHP->Health = 500.f;
    UMistAnimaCombat* SecondCombat = NewObject<UMistAnimaCombat>(SecondCaster);
    SecondCaster->AddInstanceComponent(SecondCombat); SecondCombat->RegisterComponent(); SecondCombat->Initialize(nullptr);
    FMistAnimaCombatTestAccess::Aura(F.Combat); FMistAnimaCombatTestAccess::Aura(SecondCombat);
    FMistAnimaCombatTestAccess::Mark(F.Combat, Target);
    FMistAnimaCombatTestAccess::Burns(F.Combat, Target, 10);
    FMistAnimaCombatTestAccess::Burns(SecondCombat, Target, 10);
    F.Time(0.99); F.TickBurns(Target);
    TestEqual(TEXT("Burn's first tick waits a full second"), Target->CombatHealth->Health, 3000.f);
    F.Time(1.0); F.TickBurns(Target);
    TestEqual(TEXT("Ten raw boss burn damage is shared across all sources before MR"), Target->CombatHealth->Health, 3000.f - 10.f / 1.2f, 0.01f);
    TestEqual(TEXT("First contributor receives proportional R plus separate Mark healing"), F.Health->Health, 500.f + 5.f / 1.2f * 1.15f, 0.01f);
    TestEqual(TEXT("Second contributor receives only its own proportional damage healing"), SecondHP->Health, 500.f + 5.f / 1.2f, 0.01f);
    TestEqual(TEXT("Sources retain independent stack caps"), F.Combat->GetBurnCount(Target), 10);
    F.Combat->ResetAttempt();
    TestEqual(TEXT("Resetting one hero removes only that source's burn stacks"), SecondCombat->GetBurnCount(Target), 10);
    SecondCombat->ResetAttempt();
    Target->CombatHealth->PercentageBurnMaxTick = 0.f;
    Target->CombatHealth->MaxHealth = 1000.f; Target->CombatHealth->bIsMinion = true; Target->CombatHealth->ResetHealth();
    F.Health->Health = 500.f;
    FMistAnimaCombatTestAccess::Aura(F.Combat);
    FMistAnimaCombatTestAccess::Burns(F.Combat, Target, 1);
    F.Time(2.0); F.TickBurns(Target);
    TestEqual(TEXT("Minion burn is 10% maximum HP each second"), Target->CombatHealth->Health, 1000.f - 100.f / 1.2f, 0.01f);
    TestEqual(TEXT("Flame ultimate cannot heal from minion burn"), F.Health->Health, 500.f);
    F.Combat->ResetAttempt();
    Target->CombatHealth->bIsMinion = false; Target->CombatHealth->ResetHealth();
    FMistAnimaCombatTestAccess::Burns(F.Combat, Target, 1);
    F.Time(3.0); F.TickBurns(Target);
    F.Time(4.0); F.TickBurns(Target);
    F.Time(5.0); F.TickBurns(Target);
    F.Time(6.0); F.TickBurns(Target);
    TestEqual(TEXT("One independent stack deals exactly four delayed ticks"), Target->CombatHealth->Health, 1000.f - 4.f * 2.5f / 1.2f, 0.01f);
    TestEqual(TEXT("Completed burn stack expires"), F.Combat->GetBurnCount(Target), 0);
    F.Combat->ResetAttempt(); SecondCombat->ResetAttempt();
    Target->CombatHealth->MaxHealth = 3000.f; Target->CombatHealth->PercentageBurnMaxTick = 10.f;
    Target->CombatHealth->ResetHealth(); Target->CombatHealth->Health = 3.f;
    F.Health->Health = SecondHP->Health = 500.f;
    F.Health->AbilityDamageMultiplier = 2.f;
    FMistAnimaCombatTestAccess::Aura(F.Combat); FMistAnimaCombatTestAccess::Aura(SecondCombat);
    FMistAnimaCombatTestAccess::Mark(F.Combat, Target);
    FMistAnimaCombatTestAccess::Burns(F.Combat, Target, 10);
    FMistAnimaCombatTestAccess::Burns(SecondCombat, Target, 10);
    F.Health->AbilityDamageMultiplier = 9.f; // Released stacks retain their original scaling.
    F.Time(7.0); F.TickBurns(Target);
    TestEqual(TEXT("Simultaneous capped burn applies lethal health loss once"), Target->CombatHealth->Health, 0.f);
    TestEqual(TEXT("Overkill credit respects snapshotted first-source 2:1 contribution and Mark"), F.Health->Health, 502.3f, 0.01f);
    TestEqual(TEXT("Other contributor receives its proportional actual HP credit on a lethal tick"), SecondHP->Health, 501.f, 0.01f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMistAnimaHealingTheftTest, "Mistworld.Anima.HealingTheftModifiersAndExpiry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FMistAnimaHealingTheftTest::RunTest(const FString& Parameters)
{
    FAnimaWorld F;
    AMistTrainingDummy* Target = F.Target();
    AActor* HealerActor = F.World->SpawnActor<AActor>();
    UMistCombatHealth* Healer = F.MakeHealth(HealerActor);
    Healer->SetOutgoingHealingBonus(0.20f);
    F.Health->SetOutgoingHealingBonus(0.50f); // Must not be applied to stolen healing.
    F.Health->SetIncomingHealingBonus(0.20f);
    F.Health->SetAntiHeal(TEXT("Wound"), 0.40f, 5.f);
    Target->CombatHealth->Health = 1190.f;
    Target->CombatHealth->SetIncomingHealingBonus(0.10f);
    TestTrue(TEXT("Healing theft can attach to an ordinary damageable enemy"), Target->CombatHealth->SetHealingTheft(F.Health, 7.f));
    TestEqual(TEXT("Target share receives its own modifier and missing-health cap"), Target->CombatHealth->Heal(100.f, Healer), 10.f);
    TestEqual(TEXT("Thief gets half the original heal before target's cap, outgoing applied once"), F.Health->Health, 543.2f, 0.01f);
    // Make the thief itself subject to theft. Redirected healing cannot chain.
    UMistCombatHealth* Other = F.MakeHealth(F.World->SpawnActor<AActor>()); Other->Health = 500.f;
    F.Health->SetHealingTheft(Other, 4.f);
    Target->CombatHealth->Heal(100.f, Healer);
    TestEqual(TEXT("Redirected healing cannot be stolen again"), Other->Health, 500.f);
    F.Time(7.01); Target->CombatHealth->Health = 500.f;
    TestEqual(TEXT("Theft expires after seven seconds"), Target->CombatHealth->GetHealingTheftRemaining(), 0.f);
    TestEqual(TEXT("Full ordinary healing resumes on expiry"), Target->CombatHealth->Heal(100.f, Healer), 132.f, 0.01f);
    Target->CombatHealth->GrantDebuffImmunity(3.f);
    TestFalse(TEXT("Debuff immunity rejects new healing theft"), Target->CombatHealth->SetHealingTheft(F.Health, 7.f));
    return true;
}
#endif

