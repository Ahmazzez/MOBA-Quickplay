#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Tests/AutomationEditorCommon.h"
#include "CollisionQueryParams.h"
#include "CollisionShape.h"
#include "Components/CapsuleComponent.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerStart.h"
#include "HAL/PlatformTime.h"
#include "MistCombatHealth.h"
#include "MistTrainingDummy.h"
#include "MistworldPlayerController.h"
#include "NavigationSystem.h"
#include "TimerManager.h"

struct FMistDashTickTestAccess
{
    static void Reset(AMistworldPlayerController* PC) { PC->ResetCombatSandbox(); }
    static bool Endpoint(AMistworldPlayerController* PC, const FVector& Direction, const float Distance, FVector& End)
    {
        return PC->FindLegalMovementEndpoint(Direction, Distance, End);
    }
    static void CommitAndBegin(AMistworldPlayerController* PC, const FVector& Direction, const bool bEmpowered)
    {
        if (bEmpowered) PC->UltimateRecastUntil = PC->GetWorld()->GetTimeSeconds() + 2.0;
        PC->CommitAbility(bEmpowered ? EKeys::R : EKeys::W, Direction);
        // Bypass only the wait; exercise the same BeginDash and UpdateDash
        // that the wind-up timer and controller tick normally call.
        PC->GetWorldTimerManager().ClearTimer(PC->CombatTimer);
        PC->BeginDash();
    }
    static void TickDash(AMistworldPlayerController* PC, const float DeltaTime) { PC->UpdateDash(DeltaTime); }
    static bool IsDashing(AMistworldPlayerController* PC) { return PC->bDashing; }
    static void StopMovement(AMistworldPlayerController* PC) { PC->StopForCommittedAttack(); }
};

namespace
{
class FMistWaitForDashTickNavigation final : public IAutomationLatentCommand
{
public:
    explicit FMistWaitForDashTickNavigation(FAutomationTestBase* InTest) : Test(InTest) {}
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
        if (FPlatformTime::Seconds() - Started > 30.0)
        {
            Test->AddError(TEXT("Dash tick regression navigation unavailable."));
            return true;
        }
        return false;
    }
private:
    FAutomationTestBase* Test;
    double Started = -1.0;
    bool bBuilt = false;
};

class FMistVerifySmallDashTicks final : public IAutomationLatentCommand
{
public:
    explicit FMistVerifySmallDashTicks(FAutomationTestBase* InTest) : Test(InTest) {}
    bool Update() override
    {
        if (Started < 0.0) Started = FPlatformTime::Seconds();
        if (FPlatformTime::Seconds() - Started > 30.0)
        {
            Test->AddError(TEXT("Dash tick regression could not find a legal runtime route."));
            return true;
        }
        UWorld* World = GEditor ? GEditor->PlayWorld : nullptr;
        AMistworldPlayerController* PC = World ? Cast<AMistworldPlayerController>(World->GetFirstPlayerController()) : nullptr;
        APawn* Pawn = PC ? PC->GetPawn() : nullptr;
        UMistCombatHealth* Health = Pawn ? Pawn->FindComponentByClass<UMistCombatHealth>() : nullptr;
        if (!World || !World->HasBegunPlay() || !Health) return false;
        const double Now = World->GetTimeSeconds();
        if (SettledAt < 0.0) SettledAt = Now;
        if (Now - SettledAt < 1.0) return false;

        FVector Direction;
        FVector Endpoint;
        if (!FindRoute(PC, 600.f, Direction, Endpoint)) return false;
        if (!bPrepared)
        {
            // The template spawn is in a small navmesh gap. Prepare a legal
            // on-tile start before checking deliberately tiny dash steps.
            FVector Preparation;
            if (!FMistDashTickTestAccess::Endpoint(PC, Direction, 50.f, Preparation)) return false;
            Pawn->SetActorLocation(Preparation, false);
            FMistDashTickTestAccess::StopMovement(PC);
            bPrepared = true;
            SettledAt = Now;
            return false;
        }
        FMistDashTickTestAccess::Reset(PC);
        FActorSpawnParameters Parameters;
        Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        AMistTrainingDummy* Dummy = World->SpawnActor<AMistTrainingDummy>(Pawn->GetActorLocation() + Direction * 120.f,
            FRotator::ZeroRotator, Parameters);
        if (!Test->TestNotNull(TEXT("Small-tick dash target spawns"), Dummy)) return true;
        Health->Health = 500.f;
        CheckDash(PC, Direction, Endpoint, false);
        Test->TestEqual(TEXT("W still damages a target once after a sub-centimeter first tick"),
            Dummy->CombatHealth->Health, 1200.f - 100.f / 1.2f, 0.1f);
        Test->TestEqual(TEXT("Small-tick ordinary W still supplies no healing"), Health->Health, 500.f, 0.01f);

        FMistDashTickTestAccess::Reset(PC);
        Health->Health = 500.f;
        if (!FindRoute(PC, 800.f, Direction, Endpoint))
        {
            Test->AddError(TEXT("No legal empowered dash route after ordinary dash."));
            Dummy->Destroy();
            return true;
        }
        Dummy->SetActorLocation(Pawn->GetActorLocation() + Direction * 120.f);
        CheckDash(PC, Direction, Endpoint, true);
        Test->TestEqual(TEXT("Empowered W still damages once after a sub-centimeter first tick"),
            Dummy->CombatHealth->Health, 1050.f, 0.1f);
        Test->TestEqual(TEXT("Empowered W still heals from its actual damage after a tiny first tick"), Health->Health, 530.f, 0.1f);
        Dummy->Destroy();
        FMistDashTickTestAccess::Reset(PC);
        return true;
    }

private:
    bool FindRoute(AMistworldPlayerController* PC, const float Distance, FVector& Direction, FVector& Endpoint)
    {
        for (int32 Angle = 0; Angle < 360; Angle += 30)
        {
            const FVector Candidate = FVector::ForwardVector.RotateAngleAxis(Angle, FVector::UpVector);
            if (FMistDashTickTestAccess::Endpoint(PC, Candidate, Distance, Endpoint)
                && FVector::Dist2D(PC->GetPawn()->GetActorLocation(), Endpoint) >= 200.f)
            {
                Direction = Candidate;
                return true;
            }
        }
        return false;
    }
    void CheckDash(AMistworldPlayerController* PC, const FVector& Direction, const FVector& Endpoint, const bool bEmpowered)
    {
        APawn* Pawn = PC->GetPawn();
        FMistDashTickTestAccess::CommitAndBegin(PC, Direction, bEmpowered);
        Test->TestTrue(TEXT("Dash begins normally"), FMistDashTickTestAccess::IsDashing(PC));
        const FVector Start = Pawn->GetActorLocation();
        FMistDashTickTestAccess::TickDash(PC, 0.f);
        Test->TestTrue(TEXT("A zero-time frame pauses the dash without ending it"), FMistDashTickTestAccess::IsDashing(PC));
        Test->TestEqual(TEXT("Zero-time dash update leaves position unchanged"), FVector::Dist2D(Start, Pawn->GetActorLocation()), 0.0, 0.001);
        FMistDashTickTestAccess::TickDash(PC, 0.0001f);
        if (!FMistDashTickTestAccess::IsDashing(PC)) DiagnoseTinyStep(PC, Start, Direction, bEmpowered ? 0.22f : 0.15f);
        Test->TestTrue(TEXT("A legal sub-centimeter step does not abort the dash"), FMistDashTickTestAccess::IsDashing(PC));
        Test->TestEqual(TEXT("Tiny dash step advances by speed times elapsed time"),
            FVector::Dist2D(Start, Pawn->GetActorLocation()), bEmpowered ? 0.22 : 0.15, 0.02);
        for (int32 Index = 0; Index < 40 && FMistDashTickTestAccess::IsDashing(PC); ++Index)
            FMistDashTickTestAccess::TickDash(PC, 0.02f);
        Test->TestFalse(TEXT("Dash completes after the tiny first tick"), FMistDashTickTestAccess::IsDashing(PC));
        Test->TestTrue(TEXT("Dash reaches the same legal endpoint regardless of initial timestep"),
            FVector::Dist2D(Pawn->GetActorLocation(), Endpoint) < 5.f);
    }

    void DiagnoseTinyStep(AMistworldPlayerController* PC, const FVector& Start, const FVector& Direction, const float Step)
    {
        ACharacter* Hero = Cast<ACharacter>(PC->GetPawn());
        if (!Hero) { Test->AddInfo(TEXT("Tiny-step query diagnosis: possessed pawn is not a character.")); return; }
        UCharacterMovementComponent* Movement = Hero->GetCharacterMovement();
        const UCapsuleComponent* Capsule = Hero->GetCapsuleComponent();
        const float Radius = Capsule->GetScaledCapsuleRadius();
        const float Height = Capsule->GetScaledCapsuleHalfHeight();
        const float MaxStep = Movement->MaxStepHeight + 5.f;
        UWorld* World = PC->GetWorld();
        UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
        FVector Candidate = Start + Direction * Step;
        FNavLocation Projected;
        const bool bProjected = Nav && Nav->ProjectPointToNavigation(Candidate, Projected, FVector(20.f, 20.f, Height + MaxStep));
        FCollisionObjectQueryParams Objects;
        Objects.AddObjectTypesToQuery(ECC_WorldStatic);
        Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
        FCollisionQueryParams Params(SCENE_QUERY_STAT(MistTinyDashDiagnosis), false, Hero);
        FHitResult Ground;
        const bool bGround = World->LineTraceSingleByObjectType(Ground, Candidate + FVector(0.f, 0.f, MaxStep),
            Candidate - FVector(0.f, 0.f, Height + MaxStep + 20.f), Objects, Params);
        if (bGround) Candidate.Z = Ground.ImpactPoint.Z + Height + 2.f;
        FHitResult Block;
        const bool bBlock = World->SweepSingleByObjectType(Block, Start, Candidate, FQuat::Identity, Objects,
            FCollisionShape::MakeCapsule(FMath::Max(1.f, Radius - 1.f), FMath::Max(Radius, Height - 2.f)), Params);
        auto ActorName = [](AActor* Actor) { return Actor ? Actor->GetName() : FString(TEXT("null")); };
        Test->AddInfo(FString::Printf(TEXT("Tiny-step diagnosis: step=%.3f start=%s feet=%s capsuleRadius=%.3f halfHeight=%.3f grounded=%d mode=%d floorBlocking=%d floorDist=%.3f floorActor=%s navProjection=%d projectionDistance=%.3f projected=%s groundHit=%d groundZ=%.3f normal=%s groundActor=%s candidateZDelta=%.3f maxStep=%.3f sweepBlock=%d blockActor=%s initialPenetration=%d penetrationDepth=%.3f"),
            Step, *Start.ToString(), *Movement->GetActorFeetLocation().ToString(), Radius, Height,
            Movement->IsMovingOnGround(), static_cast<int32>(Movement->MovementMode),
            Movement->CurrentFloor.bBlockingHit, Movement->CurrentFloor.FloorDist, *ActorName(Movement->CurrentFloor.HitResult.GetActor()),
            bProjected, bProjected ? FVector::Dist2D(Start + Direction * Step, Projected.Location) : -1.0,
            bProjected ? *Projected.Location.ToString() : TEXT("null"),
            bGround, Ground.ImpactPoint.Z, *Ground.ImpactNormal.ToString(), *ActorName(Ground.GetActor()),
            Candidate.Z - Start.Z, MaxStep, bBlock, *ActorName(Block.GetActor()), Block.bStartPenetrating, Block.PenetrationDepth));
    }

    FAutomationTestBase* Test;
    double Started = -1.0;
    double SettledAt = -1.0;
    bool bPrepared = false;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMistSmallDashTicksTest, "Mistworld.Combat.SmallDashTicks",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMistSmallDashTicksTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(TEXT("/Game/TopDown/Lvl_TopDown")));
    ADD_LATENT_AUTOMATION_COMMAND(FMistWaitForDashTickNavigation(this));
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(FMistVerifySmallDashTicks(this));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    return true;
}

#endif
