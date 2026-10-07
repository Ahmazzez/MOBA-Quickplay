#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Tests/AutomationEditorCommon.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/PlatformTime.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerStart.h"
#include "Components/CapsuleComponent.h"
#include "NavigationSystem.h"
#include "Navigation/PathFollowingComponent.h"
#include "Blueprint/AIBlueprintHelperLibrary.h"
#include "MistworldPlayerController.h"
#include "MistAnimaCombat.h"
#include "MistAnimaAbilityDefinitions.h"
#include "MistCombatHealth.h"
#include "MistTrainingDummy.h"
#include "MistArenaEnemy.h"

struct FMistAnimaInputTestAccess
{
    static void Reset(AMistworldPlayerController* PC) { PC->ResetCombatSandbox(); }
    static void Commit(AMistworldPlayerController* PC, const FKey& Key, const FVector& Direction)
    { PC->CommitAbility(Key, Direction); }
    static bool CanAim(AMistworldPlayerController* PC, const FKey& Key) { return PC->CanAimAbility(Key); }
    static bool Endpoint(AMistworldPlayerController* PC, const FVector& Direction, float Distance, FVector& End)
    { return PC->FindLegalMovementEndpoint(Direction, Distance, End); }
    static void Mask(AMistworldPlayerController* PC, int32 Value) { PC->EquippedAbyssMask = Value; PC->bAbyssUnlocked = true; }
    static void Auto(AMistworldPlayerController* PC, AActor* Target) { PC->RequestBasicAttack(Target); PC->BeginBasicAttack(); }
    static void CancelRepeat(AMistworldPlayerController* PC) { PC->AttackTarget.Reset(); }
    static void Stop(AMistworldPlayerController* PC) { PC->StopForCommittedAttack(); }
    static bool Moving(AMistworldPlayerController* PC) { return PC->PathFollowingComponent->GetStatus() == EPathFollowingStatus::Moving; }
    static double WDeadline(AMistworldPlayerController* PC) { return PC->WReadyAt; }
};

namespace
{
class FMistWaitForAnimaNavigation final : public IAutomationLatentCommand
{
public:
    explicit FMistWaitForAnimaNavigation(FAutomationTestBase* InTest) : Test(InTest) {}
    bool Update() override
    {
        if (Started < 0.0) Started = FPlatformTime::Seconds();
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        UNavigationSystemV1* Nav = World ? FNavigationSystem::GetCurrent<UNavigationSystemV1>(World) : nullptr;
        if (Nav)
        {
            if (!bBuilt) { Nav->Build(); bBuilt = true; }
            if (!Nav->IsNavigationBuildInProgress())
                for (TActorIterator<APlayerStart> It(World); It; ++It)
                {
                    FNavLocation Point;
                    if (Nav->ProjectPointToNavigation(It->GetActorLocation(), Point, FVector(150.f, 150.f, 400.f))) return true;
                }
        }
        if (FPlatformTime::Seconds() - Started > 30.0) { Test->AddError(TEXT("Anima integration navigation unavailable")); return true; }
        return false;
    }
private:
    FAutomationTestBase* Test;
    double Started = -1.0;
    bool bBuilt = false;
};

class FMistVerifyAnimaInputs final : public IAutomationLatentCommand
{
public:
    explicit FMistVerifyAnimaInputs(FAutomationTestBase* InTest) : Test(InTest) {}
    bool Update() override
    {
        if (Started < 0.0) Started = FPlatformTime::Seconds();
        if (FPlatformTime::Seconds() - Started > 50.0)
        { Test->AddError(FString::Printf(TEXT("Anima input integration timed out at stage %d mask %d"), Stage, Mask)); return true; }
        UWorld* World = GEditor ? GEditor->PlayWorld : nullptr;
        AMistworldPlayerController* PC = World ? Cast<AMistworldPlayerController>(World->GetFirstPlayerController()) : nullptr;
        ACharacter* Hero = PC ? Cast<ACharacter>(PC->GetPawn()) : nullptr;
        UMistCombatHealth* HP = Hero ? Hero->FindComponentByClass<UMistCombatHealth>() : nullptr;
        UMistAnimaCombat* Combat = PC ? PC->GetAnimaCombat() : nullptr;
        if (!World || !World->HasBegunPlay() || !HP || !Combat) return false;
        const double Now = World->GetTimeSeconds();
        switch (Stage)
        {
        case 0:
            if (StageAt < 0.0) StageAt = Now;
            if (Now - StageAt < 1.0) return false;
            FMistAnimaInputTestAccess::Reset(PC);
            Test->TestTrue(TEXT("Public checkpoint switching selects Anima"), PC->TrySwitchHero(EMistHeroId::Anima));
            if (!PC->IsAnima()) return true;
            Test->TestTrue(TEXT("Switching activates Anima's component"), Combat->IsHeroActive());
            Test->TestEqual(TEXT("Anima has her authored normalized maximum HP"), HP->MaxHealth, 1000.f);
            Checkpoint = Hero->GetActorTransform();
            // Remove passive target fixtures and disable existing sentinel AI so
            // damage below comes only from the committed skill under examination.
            for (TActorIterator<AMistTrainingDummy> It(World); It; ++It) It->Destroy();
            if (!FindRoute(PC, Hero)) return true;
            Mask = 0;
            Prepare(PC, Hero, HP, 0);
            First = Spawn(World, Origin + Direction * 150.f);
            FMistAnimaInputTestAccess::Commit(PC, EKeys::Q, Direction);
            Test->TestTrue(TEXT("Routed Q uses Anima's short commitment"), PC->IsCombatBusy());
            Test->TestTrue(TEXT("Routed Q spends the two-second cooldown"), PC->GetCooldownRemaining(EKeys::Q) > 1.9f);
            Stage = 1; StageAt = Now; return false;
        case 1:
            if (PC->IsCombatBusy() || Now - StageAt < 0.20) return false;
            if (!First.IsValid()) { Test->AddError(TEXT("Anima Q target disappeared")); return true; }
            Test->TestEqual(FString::Printf(TEXT("Mask %d Q is a real timed 70-raw thrust"), Mask), First->CombatHealth->Health, 1200.f - 70.f / 1.2f, 0.2f);
            Test->TestEqual(FString::Printf(TEXT("Mask %d only original Q supplies Mark healing"), Mask), HP->Health,
                500.f + ((Mask & 1) == 0 ? 70.f / 1.2f * 0.15f : 0.f), 0.2f);
            Test->TestTrue(TEXT("Q variant routes to sole Mark or Brand as equipped"),
                (Mask & 1) ? Combat->GetBrandedTarget() == First.Get() : Combat->GetMarkedTarget() == First.Get());
            First->Destroy(); First.Reset();
            ++Mask;
            if (Mask < 16)
            {
                Prepare(PC, Hero, HP, Mask);
                for (const FKey& Key : {EKeys::Q, EKeys::W, EKeys::E, EKeys::R})
                {
                    const int32 Slot = Key == EKeys::Q ? 0 : Key == EKeys::W ? 1 : Key == EKeys::E ? 2 : 3;
                    Test->TestEqual(TEXT("Every mixed slot resolves through the active hero"), PC->GetEquippedDefinition(Key).DisplayName,
                        FMistAnimaAbilityDefinitions::Get(Key, (Mask & (1 << Slot)) != 0).DisplayName);
                }
                First = Spawn(World, Origin + Direction * 150.f);
                FMistAnimaInputTestAccess::Commit(PC, EKeys::Q, Direction);
                StageAt = Now; return false;
            }
            Prepare(PC, Hero, HP, 0);
            First = Spawn(World, Origin + Direction * 150.f);
            Second = Spawn(World, Origin + Direction * 450.f);
            Third = Spawn(World, Origin + Direction * 750.f);
            FMistAnimaInputTestAccess::Commit(PC, EKeys::W, Direction);
            Test->TestFalse(TEXT("The next W is unavailable during its commitment and minimum gap"), FMistAnimaInputTestAccess::CanAim(PC, EKeys::W));
            Test->TestEqual(TEXT("W cooldown stays unspent during the first recast stage"), FMistAnimaInputTestAccess::WDeadline(PC), 0.0);
            Stage = 2; StageAt = Now; return false;
        case 2:
        case 3:
            if (PC->IsCombatBusy() || Now - StageAt < 0.30) return false;
            Test->TestEqual(TEXT("Each original W dash provides one 5% maximum-HP heal"), HP->Health, Stage == 2 ? 550.f : 600.f, 0.2f);
            Test->TestTrue(TEXT("Routed W recast becomes available after the actual dash"), FMistAnimaInputTestAccess::CanAim(PC, EKeys::W));
            FMistAnimaInputTestAccess::Commit(PC, EKeys::W, Direction);
            ++Stage; StageAt = Now; return false;
        case 4:
            if (PC->IsCombatBusy() || Now - StageAt < 0.30) return false;
            Test->TestEqual(TEXT("All three steps reach the full legal 9-metre route"), static_cast<float>(FVector::Dist2D(Origin, Hero->GetActorLocation())), 900.f, 6.f);
            Test->TestEqual(TEXT("Three W contacts heal exactly three times"), HP->Health, 650.f, 0.2f);
            Test->TestTrue(TEXT("Third W commitment starts the twelve-second cooldown"), PC->GetCooldownRemaining(EKeys::W) > 11.3f);
            DestroyTargets(); Prepare(PC, Hero, HP, 0);
            First = Spawn(World, Origin + Direction * 130.f);
            UAIBlueprintHelperLibrary::SimpleMoveToLocation(PC, Origin + Direction * 220.f);
            FMistAnimaInputTestAccess::Commit(PC, EKeys::E, FVector::ZeroVector);
            Test->TestTrue(TEXT("Riposte self stance preserves a navigation order"), FMistAnimaInputTestAccess::Moving(PC));
            Test->TestEqual(TEXT("Routed original E reduces incoming raw 120 to 30 HP"), HP->ReceivePhysicalDamage(120.f, First.Get()), 30.f, 0.2f);
            Stage = 5; StageAt = Now; return false;
        case 5:
            if (PC->IsCombatBusy() || Now - StageAt < 0.15) return false;
            Test->TestEqual(TEXT("Riposte's real counter timer damages the attacker"), First->CombatHealth->Health, 1200.f - 110.f / 1.2f, 0.2f);
            Test->TestEqual(TEXT("Unmarked real counter heals 15% actual damage"), HP->Health, 470.f + 110.f / 1.2f * 0.15f, 0.2f);
            DestroyTargets(); Prepare(PC, Hero, HP, 0);
            First = Spawn(World, Origin + Direction * 150.f);
            Second = Spawn(World, Origin + Direction * 450.f);
            FMistAnimaInputTestAccess::Commit(PC, EKeys::R, Direction);
            Stage = 6; StageAt = Now; return false;
        case 6:
            if (PC->IsCombatBusy() || Now - StageAt < 0.30) return false;
            Test->TestTrue(TEXT("Original R stops at first swept body contact"), FVector::Dist2D(Origin, Hero->GetActorLocation()) < 120.f);
            Test->TestEqual(TEXT("Original R impacts the first enemy for 200 physical"), First->CombatHealth->Health, 1200.f - 200.f / 1.2f, 0.2f);
            Test->TestEqual(TEXT("Original R does not pierce a second enemy"), Second->CombatHealth->Health, 1200.f);
            Test->TestEqual(TEXT("Original R establishes Mark before impact healing"), HP->Health, 525.f, 0.2f);
            Test->TestTrue(TEXT("Original R applies silence and healing theft to an ordinary enemy"),
                First->CombatHealth->IsSilenced() && First->CombatHealth->GetHealingTheftRemaining(HP) > 6.5f);
            DestroyTargets(); Prepare(PC, Hero, HP, 5); // Brand Q + Flame Stance E.
            First = Spawn(World, Origin + Direction * 150.f);
            FMistAnimaInputTestAccess::Commit(PC, EKeys::Q, Direction);
            Stage = 7; StageAt = Now; return false;
        case 7:
            if (PC->IsCombatBusy() || Now - StageAt < 0.20) return false;
            FMistAnimaInputTestAccess::Commit(PC, EKeys::E, FVector::ZeroVector);
            Test->TestEqual(TEXT("Routed Flame stance fully blocks an incoming DOT event"),
                HP->ReceiveMagicDamage(120.f, First.Get(), EMistDamageKind::DamageOverTime), 0.f);
            TargetHealthBefore = First->CombatHealth->Health;
            FMistAnimaInputTestAccess::Auto(PC, First.Get());
            Stage = 8; StageAt = Now; return false;
        case 8:
            if (PC->IsCombatBusy() || Now - StageAt < 0.23) return false;
            FMistAnimaInputTestAccess::CancelRepeat(PC);
            Test->TestEqual(TEXT("Active-hero basic attack uses Anima's enhanced 84 raw damage"), First->CombatHealth->Health, TargetHealthBefore - 84.f / 1.2f, 0.3f);
            Test->TestEqual(TEXT("Real enhanced branded auto applies two total burn stacks"), Combat->GetBurnCount(First.Get()), 2);
            Test->TestEqual(TEXT("Flame E's block supplies no intrinsic healing"), HP->Health, 500.f, 0.2f);
            FMistAnimaInputTestAccess::Mask(PC, 7); // Preserve the applied burns to test W's pre-hit count.
            FMistAnimaInputTestAccess::Commit(PC, EKeys::W, Direction);
            Stage = 9; StageAt = Now; return false;
        case 9:
            if (PC->IsCombatBusy() || Now - StageAt < 0.30) return false;
            Test->TestEqual(TEXT("Real Flame W heals from two existing stacks before adding its two"), HP->Health, 510.f, 0.2f);
            Test->TestEqual(TEXT("Real Flame W supplies two additional independent burns"), Combat->GetBurnCount(First.Get()), 4);
            DestroyTargets(); Prepare(PC, Hero, HP, 8); // Original Mark Q mixed with aura R.
            First = Spawn(World, Origin + Direction * 150.f);
            FMistAnimaInputTestAccess::Commit(PC, EKeys::Q, Direction);
            Stage = 10; StageAt = Now; return false;
        case 10:
            if (PC->IsCombatBusy() || Now - StageAt < 0.20) return false;
            FMistAnimaInputTestAccess::Commit(PC, EKeys::R, FVector::ZeroVector);
            Stage = 11; StageAt = Now; return false;
        case 11:
            if (PC->IsCombatBusy() || Now - StageAt < 0.23) return false;
            TargetHealthBefore = First->CombatHealth->Health;
            HeroHealthBefore = HP->Health;
            Test->TestEqual(TEXT("Self aura starts without immediately applying burn"), Combat->GetBurnCount(First.Get()), 0);
            Test->TestFalse(TEXT("An active aura prevents checkpoint swapping"), PC->GetCharacterSwitchLockReason().IsEmpty());
            Stage = 12; StageAt = Now; return false;
        case 12:
            if (Now - StageAt < 1.05) return false;
            Test->TestEqual(TEXT("Aura's first whole-second pulse applies two independent stacks"), Combat->GetBurnCount(First.Get()), 2);
            Test->TestEqual(TEXT("Fresh aura stacks do not damage immediately on application"), First->CombatHealth->Health, TargetHealthBefore, 0.2f);
            Stage = 13; return false;
        case 13:
            if (Now - StageAt < 2.05) return false;
            Test->TestTrue(TEXT("Aura's delayed burn ticks damage the nearby enemy"), First->CombatHealth->Health <= TargetHealthBefore - 4.8f);
            Test->TestTrue(TEXT("Mixed original Mark + Flame R adds both damage-derived heals"), HP->Health >= HeroHealthBefore + 5.5f);
            DestroyTargets(); FMistAnimaInputTestAccess::Reset(PC);
            Hero->TeleportTo(Checkpoint.GetLocation(), Checkpoint.Rotator(), false, false);
            Test->TestTrue(TEXT("Reset resolves Anima effects so public switching can return to Alphonse"), PC->TrySwitchHero(EMistHeroId::Alphonse));
            return true;
        }
        return true;
    }
private:
    void Prepare(AMistworldPlayerController* PC, ACharacter* Hero, UMistCombatHealth* HP, int32 NewMask)
    {
        FMistAnimaInputTestAccess::Reset(PC);
        FMistAnimaInputTestAccess::Mask(PC, NewMask);
        Hero->SetActorLocation(Origin, false); Hero->SetActorRotation(Direction.Rotation());
        Hero->GetCharacterMovement()->StopMovementImmediately(); Hero->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
        HP->EndSpawnProtection(); HP->AbilityDamageMultiplier = 1.f; HP->Health = 500.f;
    }
    AMistTrainingDummy* Spawn(UWorld* World, const FVector& Position)
    {
        FActorSpawnParameters Params; Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        AMistTrainingDummy* Target = World->SpawnActor<AMistTrainingDummy>(Position, FRotator::ZeroRotator, Params);
        Target->CombatHealth->ResetHealth(); Target->Capsule->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
        return Target;
    }
    void DestroyTargets()
    {
        for (const TWeakObjectPtr<AMistTrainingDummy>& Target : {First, Second, Third}) if (Target.IsValid()) Target->Destroy();
        First.Reset(); Second.Reset(); Third.Reset();
    }
    bool FindRoute(AMistworldPlayerController* PC, ACharacter* Hero)
    {
        UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(PC->GetWorld());
        if (!Nav) { Test->AddError(TEXT("Anima PIE has no navigation system")); return false; }
        const FVector Candidates[] = { FVector(400.f, 300.f, 100.f), FVector(0.f, 0.f, 100.f), Hero->GetActorLocation() };
        for (const FVector& Candidate : Candidates)
        {
            FNavLocation Projected;
            if (!Nav->ProjectPointToNavigation(Candidate, Projected, FVector(150.f, 150.f, 400.f))) continue;
            FCollisionObjectQueryParams Objects; Objects.AddObjectTypesToQuery(ECC_WorldStatic); Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
            FCollisionQueryParams Query(SCENE_QUERY_STAT(MistAnimaInputFloor), false, Hero);
            FHitResult Floor;
            if (!PC->GetWorld()->LineTraceSingleByObjectType(Floor, Projected.Location + FVector(0.f, 0.f, 200.f),
                Projected.Location - FVector(0.f, 0.f, 200.f), Objects, Query)) continue;
            Origin = FVector(Projected.Location.X, Projected.Location.Y, Floor.ImpactPoint.Z + Hero->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 2.f);
            Hero->SetActorLocation(Origin, false);
            for (int32 Angle = 0; Angle < 360; Angle += 30)
            {
                Direction = FVector::ForwardVector.RotateAngleAxis(Angle, FVector::UpVector);
                FVector End;
                if (FMistAnimaInputTestAccess::Endpoint(PC, Direction, 900.f, End) && FVector::Dist2D(Origin, End) >= 895.f) return true;
            }
        }
        Test->AddError(TEXT("No full legal 9-metre route for Anima's three-step integration test")); return false;
    }
    FAutomationTestBase* Test;
    int32 Stage = 0, Mask = 0;
    double Started = -1.0, StageAt = -1.0;
    FVector Origin, Direction;
    FTransform Checkpoint;
    TWeakObjectPtr<AMistTrainingDummy> First, Second, Third;
    float TargetHealthBefore = 0.f, HeroHealthBefore = 0.f;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMistAnimaInputIntegrationTest, "Mistworld.Anima.AllKitsAndRealInputAbilities",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FMistAnimaInputIntegrationTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(TEXT("/Game/Mistworld/Maps/Lvl_CombatTest")));
    ADD_LATENT_AUTOMATION_COMMAND(FMistWaitForAnimaNavigation(this));
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(FMistVerifyAnimaInputs(this));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    return true;
}
#endif
