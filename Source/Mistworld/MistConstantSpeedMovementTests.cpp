#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Tests/AutomationEditorCommon.h"
#include "Editor.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerStart.h"
#include "Blueprint/AIBlueprintHelperLibrary.h"
#include "NavigationSystem.h"
#include "NavigationPath.h"
#include "MistworldPlayerController.h"
#include "MistConstantSpeedPathFollowingComponent.h"

namespace
{
class FMistWaitForMovementNavigation final : public IAutomationLatentCommand
{
public:
    explicit FMistWaitForMovementNavigation(FAutomationTestBase* InTest) : Test(InTest) {}
    virtual bool Update() override
    {
        const double Now = FPlatformTime::Seconds();
        if (StartedAt < 0.0) StartedAt = Now;
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        UNavigationSystemV1* Navigation = World ? FNavigationSystem::GetCurrent<UNavigationSystemV1>(World) : nullptr;
        if (Navigation)
        {
            if (!bBuilt)
            {
                Navigation->Build();
                bBuilt = true;
            }
            if (!Navigation->IsNavigationBuildInProgress())
            {
                for (TActorIterator<APlayerStart> It(World); It; ++It)
                {
                    FNavLocation Location;
                    if (Navigation->ProjectPointToNavigation(It->GetActorLocation(), Location, FVector(150.f, 150.f, 400.f)))
                        return true;
                }
            }
        }
        if (Now - StartedAt >= 30.0)
        {
            Test->AddError(TEXT("Timed out waiting for editor navigation before constant-speed PIE test."));
            return true;
        }
        return false;
    }
private:
    FAutomationTestBase* Test;
    double StartedAt = -1.0;
    bool bBuilt = false;
};

// Exercise live navmesh requests and CharacterMovement on the template pawn.
// Sample the first nonzero velocity and every near-goal movement frame, rather
// than only reading configuration flags or waiting for a steady-state speed.
class FMistVerifyConstantSpeedMovement final : public IAutomationLatentCommand
{
public:
    explicit FMistVerifyConstantSpeedMovement(FAutomationTestBase* InTest) : Test(InTest) {}
    virtual bool Update() override
    {
        const double Now = FPlatformTime::Seconds();
        if (StartedAt < 0.0) StartedAt = Now;
        if (Now - StartedAt >= 35.0)
        {
            UWorld* PlayWorld = GEditor ? GEditor->PlayWorld : nullptr;
            APlayerController* Player = PlayWorld ? PlayWorld->GetFirstPlayerController() : nullptr;
            ACharacter* Character = Player ? Cast<ACharacter>(Player->GetPawn()) : nullptr;
            UCharacterMovementComponent* CharacterMovement = Character ? Character->GetCharacterMovement() : nullptr;
            Test->AddInfo(FString::Printf(TEXT("Movement timeout state: gameTime=%.3f delta=%.6f grounded=%d mode=%d pawn=%s feet=%s originProjection=%d goalProjection=%d heightReject=%d invalidPath=%d partialPath=%d pointCountReject=%d lastPathPoints=%d"),
                PlayWorld ? PlayWorld->GetTimeSeconds() : -1.f, PlayWorld ? PlayWorld->GetDeltaSeconds() : -1.f,
                CharacterMovement ? CharacterMovement->IsMovingOnGround() : false,
                CharacterMovement ? static_cast<int32>(CharacterMovement->MovementMode) : -1,
                Character ? *Character->GetActorLocation().ToString() : TEXT("null"),
                CharacterMovement ? *CharacterMovement->GetActorFeetLocation().ToString() : TEXT("null"),
                bOriginProjected, GoalProjectionCount, HeightRejectCount, InvalidPathCount, PartialPathCount, PointCountRejectCount, LastPathPointCount));
            Test->AddError(FString::Printf(TEXT("Constant-speed movement timed out in stage %d."), static_cast<int32>(Stage)));
            return true;
        }
        UWorld* World = GEditor ? GEditor->PlayWorld : nullptr;
        AMistworldPlayerController* Controller = World ? Cast<AMistworldPlayerController>(World->GetFirstPlayerController()) : nullptr;
        ACharacter* Hero = Controller ? Cast<ACharacter>(Controller->GetPawn()) : nullptr;
        UMistConstantSpeedPathFollowingComponent* Following = Controller ? Controller->FindComponentByClass<UMistConstantSpeedPathFollowingComponent>() : nullptr;
        if (!World || !World->HasBegunPlay() || !Hero) return false;
        if (!Following)
        {
            Test->AddError(TEXT("The actual player controller must use constant-speed path following."));
            return true;
        }
        UCharacterMovementComponent* Movement = Hero->GetCharacterMovement();
        const double GameTime = World->GetTimeSeconds();
        const FVector Position = Hero->GetActorLocation();
        const FVector Velocity = Hero->GetVelocity();
        const float Speed = Velocity.Size2D();
        switch (Stage)
        {
        case EStage::FindRoute:
            if (SettlingAt < 0.0) SettlingAt = GameTime;
            if (GameTime - SettlingAt < 1.0 || !Movement->IsMovingOnGround()) return false;
            if (!FindStraightRoute(World, Controller, Movement->GetActorFeetLocation()))
            {
                if (!bRepositioned && TryPrepareFlatRoute(World, Controller, Hero))
                {
                    bRepositioned = true;
                    SettlingAt = GameTime;
                    Test->AddInfo(FString::Printf(TEXT("Movement fixture moved to a collision-checked flat navigation route at %s."),
                        *Hero->GetActorLocation().ToString()));
                }
                return false;
            }
            Test->TestFalse(TEXT("Navigation uses direct velocity, without acceleration input"), Movement->UseAccelerationForPathFollowing());
            Test->TestFalse(TEXT("Requested movement velocity is not smoothed by acceleration"), Movement->bRequestedMoveUseAcceleration);
            UAIBlueprintHelperLibrary::SimpleMoveToLocation(Controller, FarDestination);
            MoveStart = Position;
            Advance(EStage::FirstMovement, GameTime);
            return false;

        case EStage::FirstMovement:
            if (Speed <= 1.f) return false;
            if (!AtFullSpeed(TEXT("The first moving frame is already at 500cm/s"), Speed)) return true;
            Test->AddInfo(FString::Printf(TEXT("First moving frame: %.3fcm/s."), Speed));
            Advance(EStage::MoveForward, GameTime);
            return false;

        case EStage::MoveForward:
            if (Speed > 1.f && !AtFullSpeed(TEXT("Straight movement stays at constant speed"), Speed)) return true;
            if (FVector::Dist2D(MoveStart, Position) < 120.f) return false;
            // This is a real replacement navigation request while in motion.
            UAIBlueprintHelperLibrary::SimpleMoveToLocation(Controller, Origin);
            MoveStart = Position;
            Advance(EStage::Reverse, GameTime);
            return false;

        case EStage::Reverse:
            // The latent command may run before this frame's movement tick.
            if (GameTime <= StageStartedAt || Speed <= 1.f) return false;
            if (!AtFullSpeed(TEXT("A 180-degree redirect immediately keeps full speed"), Speed)) return true;
            if (!Test->TestTrue(TEXT("The first movement frame after redirect goes in the new direction"),
                FVector::DotProduct(Velocity.GetSafeNormal2D(), Forward) < -0.99f)) return true;
            Test->AddInfo(FString::Printf(TEXT("First reversal frame: %.3fcm/s."), Speed));
            Advance(EStage::MoveBackward, GameTime);
            return false;

        case EStage::MoveBackward:
            if (Speed > 1.f && !AtFullSpeed(TEXT("Movement after reversal remains constant"), Speed)) return true;
            if (FVector::Dist2D(MoveStart, Position) < 60.f) return false;
            CloseDestination = Movement->GetActorFeetLocation() + Forward * 32.f;
            MoveStart = Position;
            UAIBlueprintHelperLibrary::SimpleMoveToLocation(Controller, CloseDestination);
            Advance(EStage::CloseMove, GameTime);
            return false;

        case EStage::CloseMove:
            if (GameTime <= StageStartedAt) return false;
            if (Speed > 1.f)
            {
                bSawCloseMovement = true;
                MaxCloseFrameTime = FMath::Max(MaxCloseFrameTime, World->GetDeltaSeconds());
                if (!AtFullSpeed(TEXT("A close click never slows down before arriving"), Speed)) return true;
                return false;
            }
            if (Following->GetStatus() != EPathFollowingStatus::Idle) return false;
            Test->TestTrue(TEXT("A point only 32cm away still causes movement"), bSawCloseMovement);
            Test->TestTrue(TEXT("Close click completes successfully"), Following->DidMoveReachGoal());
            Test->TestTrue(TEXT("Close click stops within one movement frame of its destination"),
                FVector::Dist2D(Position, CloseDestination) <= FMath::Max(10.f, 500.f * MaxCloseFrameTime + 5.f));
            Test->TestTrue(TEXT("Arrival immediately sets velocity to zero"), Speed < 0.01f);
            StopPosition = Position;
            Advance(EStage::Stopped, GameTime);
            return false;

        case EStage::Stopped:
            if (GameTime - StageStartedAt < 0.2) return false;
            Test->TestTrue(TEXT("No post-arrival drift or oscillation"), FVector::Dist2D(StopPosition, Position) < 0.1f);
            Test->TestTrue(TEXT("Character remains fully stopped after arrival"), Speed < 0.01f);
            return true;
        }
        return true;
    }

private:
    enum class EStage { FindRoute, FirstMovement, MoveForward, Reverse, MoveBackward, CloseMove, Stopped };
    void Advance(EStage NextStage, double Time) { Stage = NextStage; StageStartedAt = Time; }
    bool AtFullSpeed(const TCHAR* Message, float Speed)
    {
        if (FMath::IsNearlyEqual(Speed, 500.f, 1.f)) return true;
        Test->AddError(FString::Printf(TEXT("%s: measured %.3fcm/s."), Message, Speed));
        return false;
    }
    bool FindStraightRoute(UWorld* World, AMistworldPlayerController* Controller, const FVector& Feet)
    {
        bOriginProjected = false;
        GoalProjectionCount = HeightRejectCount = InvalidPathCount = PartialPathCount = PointCountRejectCount = 0;
        LastPathPointCount = -1;
        UNavigationSystemV1* Navigation = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
        if (!Navigation) return false;
        FNavLocation ProjectedOrigin;
        if (!Navigation->ProjectPointToNavigation(Feet, ProjectedOrigin, FVector(80.f, 80.f, 100.f))) return false;
        bOriginProjected = true;
        const FVector Offsets[] = {
            // Prefer the player's raised starting platform; long routes can
            // descend to lower floors in the template and change walking mode.
            FVector(180.f, 0.f, 0.f), FVector(-180.f, 0.f, 0.f),
            FVector(0.f, 180.f, 0.f), FVector(0.f, -180.f, 0.f),
            FVector(200.f, 0.f, 0.f), FVector(-200.f, 0.f, 0.f),
            FVector(0.f, 200.f, 0.f), FVector(0.f, -200.f, 0.f),
            FVector(300.f, 0.f, 0.f), FVector(-300.f, 0.f, 0.f),
            FVector(0.f, 300.f, 0.f), FVector(0.f, -300.f, 0.f),
            FVector(220.f, 220.f, 0.f), FVector(-220.f, -220.f, 0.f),
            FVector(220.f, -220.f, 0.f), FVector(-220.f, 220.f, 0.f),
            FVector(700.f, 0.f, 0.f), FVector(-700.f, 0.f, 0.f),
            FVector(0.f, 700.f, 0.f), FVector(0.f, -700.f, 0.f),
            FVector(500.f, 500.f, 0.f), FVector(-500.f, -500.f, 0.f)
        };
        for (const FVector& Offset : Offsets)
        {
            FNavLocation ProjectedGoal;
            if (!Navigation->ProjectPointToNavigation(Feet + Offset, ProjectedGoal, FVector(80.f, 80.f, 100.f))) continue;
            ++GoalProjectionCount;
            if (FMath::Abs(ProjectedGoal.Location.Z - ProjectedOrigin.Location.Z) > 2.f) { ++HeightRejectCount; continue; }
            UNavigationPath* Path = Navigation->FindPathToLocationSynchronously(World, ProjectedOrigin.Location, ProjectedGoal.Location, Controller);
            if (!Path || !Path->IsValid()) { ++InvalidPathCount; continue; }
            LastPathPointCount = Path->PathPoints.Num();
            if (Path->IsPartial()) { ++PartialPathCount; continue; }
            if (Path->PathPoints.Num() != 2) { ++PointCountRejectCount; continue; }
            Origin = ProjectedOrigin.Location;
            FarDestination = ProjectedGoal.Location;
            Forward = (FarDestination - Origin).GetSafeNormal2D();
            return true;
        }
        return false;
    }
    bool TryPrepareFlatRoute(UWorld* World, AMistworldPlayerController* Controller, ACharacter* Hero)
    {
        UNavigationSystemV1* Navigation = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
        if (!Navigation || !Hero->GetCapsuleComponent()) return false;
        UCharacterMovementComponent* Movement = Hero->GetCharacterMovement();
        const FVector Feet = Movement->GetActorFeetLocation();
        const FVector Offsets[] = {
            FVector(300.f, 0.f, 0.f), FVector(-300.f, 0.f, 0.f),
            FVector(0.f, 300.f, 0.f), FVector(0.f, -300.f, 0.f),
            FVector(700.f, 0.f, 0.f), FVector(-700.f, 0.f, 0.f),
            FVector(0.f, 700.f, 0.f), FVector(0.f, -700.f, 0.f),
            FVector(500.f, 500.f, 0.f), FVector(-500.f, -500.f, 0.f),
            FVector(500.f, -500.f, 0.f), FVector(-500.f, 500.f, 0.f)
        };
        for (const FVector& Offset : Offsets)
        {
            FNavLocation Candidate;
            if (!Navigation->ProjectPointToNavigation(Feet + Offset, Candidate, FVector(80.f, 80.f, 500.f))) continue;
            // Qualify the fixture with the original route checks before
            // moving the pawn; the speed and reversal checks stay unchanged.
            if (!FindStraightRoute(World, Controller, Candidate.Location)) continue;
            const FVector Start = Candidate.Location + FVector(0.f, 0.f,
                Hero->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 5.f);
            if (!Hero->TeleportTo(Start, Hero->GetActorRotation(), false, false)) continue;
            Movement->StopMovementImmediately();
            return true;
        }
        return false;
    }
    FAutomationTestBase* Test;
    EStage Stage = EStage::FindRoute;
    double StartedAt = -1.0;
    double SettlingAt = -1.0;
    double StageStartedAt = 0.0;
    FVector Origin;
    FVector FarDestination;
    FVector Forward;
    FVector MoveStart;
    FVector CloseDestination;
    FVector StopPosition;
    float MaxCloseFrameTime = 0.f;
    bool bSawCloseMovement = false;
    bool bRepositioned = false;
    bool bOriginProjected = false;
    int32 GoalProjectionCount = 0;
    int32 HeightRejectCount = 0;
    int32 InvalidPathCount = 0;
    int32 PartialPathCount = 0;
    int32 PointCountRejectCount = 0;
    int32 LastPathPointCount = -1;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMistConstantSpeedMovementTest, "Mistworld.Movement.ConstantSpeed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMistConstantSpeedMovementTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(TEXT("/Game/TopDown/Lvl_TopDown")));
    ADD_LATENT_AUTOMATION_COMMAND(FMistWaitForMovementNavigation(this));
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(FMistVerifyConstantSpeedMovement(this));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    return true;
}

#endif
