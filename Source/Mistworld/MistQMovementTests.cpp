#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Tests/AutomationEditorCommon.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerInput.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HAL/PlatformTime.h"
#include "InputKeyEventArgs.h"
#include "EngineUtils.h"
#include "MistworldPlayerController.h"
#include "NavigationSystem.h"
#include "NavigationData.h"
#include "NavigationPath.h"
#include "Navigation/PathFollowingComponent.h"

// Only the test can supply a deterministic cursor destination. Movement,
// input state, controller ticks, character ticks, and Q's timer run in PIE.
struct FMistCombatInputTestAccess
{
    static void ClickDestination(AMistworldPlayerController* Controller, const FVector& Destination)
    {
        Controller->OnInputStarted();
        Controller->CachedDestination = Destination;
        Controller->FollowTime = 0.f;
        Controller->bDestinationInputAccepted = true;
        Controller->OnSetDestinationReleased();
    }

    static bool IsAiming(const AMistworldPlayerController* Controller)
    {
        return Controller->bAimingQ;
    }

    static bool IsCasting(const AMistworldPlayerController* Controller)
    {
        return Controller->bCastingCombat;
    }

    static bool IsNavigating(const AMistworldPlayerController* Controller)
    {
        return Controller->PathFollowingComponent
            && Controller->PathFollowingComponent->GetStatus() == EPathFollowingStatus::Moving;
    }

    static FVector NavigationGoal(const AMistworldPlayerController* Controller)
    {
        const FNavPathSharedPtr Path = Controller->PathFollowingComponent->GetPath();
        return Path.IsValid() ? Path->GetEndLocation() : FVector::ZeroVector;
    }

    static FString MovementDiagnostics(const AMistworldPlayerController* Controller)
    {
        const APawn* Pawn = Controller->GetPawn();
        const ACharacter* Character = Cast<ACharacter>(Pawn);
        const UCharacterMovementComponent* Movement = Character ? Character->GetCharacterMovement() : nullptr;
        const FNavAgentProperties& Agent = Controller->GetNavAgentPropertiesRef();
        UNavigationSystemV1* Navigation = FNavigationSystem::GetCurrent<UNavigationSystemV1>(Controller->GetWorld());
        const ANavigationData* AgentNavData = Navigation ? Navigation->GetNavDataForProps(Agent, Controller->GetNavAgentLocation()) : nullptr;
        return FString::Printf(
            TEXT("Pawn=%s Velocity=%s Goal=%s PathStatus=%d PathAllowed=%d MoveInputIgnored=%d Mode=%d MaxSpeed=%.1f MaxAcceleration=%.1f AgentRadius=%.1f AgentHeight=%.1f AgentLocation=%s AgentNavData=%s PawnTick=%d MovementTick=%d WorldPaused=%d"),
            Pawn ? *Pawn->GetActorLocation().ToCompactString() : TEXT("null"),
            Pawn ? *Pawn->GetVelocity().ToCompactString() : TEXT("null"),
            *NavigationGoal(Controller).ToCompactString(),
            Controller->PathFollowingComponent ? static_cast<int32>(Controller->PathFollowingComponent->GetStatus()) : -1,
            Controller->PathFollowingComponent && Controller->PathFollowingComponent->IsPathFollowingAllowed() ? 1 : 0,
            Controller->IsMoveInputIgnored() ? 1 : 0,
            Movement ? static_cast<int32>(Movement->MovementMode) : -1,
            Movement ? Movement->MaxWalkSpeed : -1.f,
            Movement ? Movement->MaxAcceleration : -1.f,
            Agent.AgentRadius, Agent.AgentHeight,
            *Controller->GetNavAgentLocation().ToCompactString(), *GetNameSafe(AgentNavData),
            Pawn && Pawn->IsActorTickEnabled() ? 1 : 0,
            Movement && Movement->IsComponentTickEnabled() ? 1 : 0,
            Controller->GetWorld()->IsPaused() ? 1 : 0);
    }

    static void CommitCleave(AMistworldPlayerController* Controller, const FVector& Direction)
    {
        // Release without a hardware cursor ray, then exercise the real commit
        // method. Aiming itself above is started through the actual Q key.
        Controller->bAimingQ = false;
        Controller->InputKey(FInputKeyEventArgs(nullptr, INPUTDEVICEID_NONE, EKeys::Q, IE_Released,
            0.f, false, FPlatformTime::Cycles64()));
        Controller->CommittedDirection = Direction;
        Controller->StartCleave();
    }
};

namespace
{
// The template's world-partition map builds navigation asynchronously on load.
// Wait for that build before PIE duplicates the world and its nav data.
class FMistWaitForEditorNavigation final : public IAutomationLatentCommand
{
public:
    explicit FMistWaitForEditorNavigation(FAutomationTestBase* InTest) : Test(InTest) {}

    virtual bool Update() override
    {
        const double Now = FPlatformTime::Seconds();
        if (StartedAt < 0.0) StartedAt = Now;
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        UNavigationSystemV1* Navigation = World ? FNavigationSystem::GetCurrent<UNavigationSystemV1>(World) : nullptr;
        if (Navigation)
        {
            if (!bRequestedBuild)
            {
                Navigation->Build();
                bRequestedBuild = true;
            }
            if (!Navigation->IsNavigationBuildInProgress())
            {
                for (TActorIterator<APlayerStart> It(World); It; ++It)
                {
                    FNavLocation Location;
                    if (Navigation->ProjectPointToNavigation(It->GetActorLocation(), Location, FVector(150.f, 150.f, 400.f)))
                    {
                        Test->AddInfo(TEXT("Editor navigation built at PlayerStart before PIE duplication."));
                        return true;
                    }
                }
            }
        }
        if (Now - StartedAt >= 30.0)
        {
            Test->AddError(TEXT("Timed out waiting for template map editor navigation before PIE startup."));
            return true;
        }
        return false;
    }

private:
    FAutomationTestBase* Test;
    double StartedAt = -1.0;
    bool bRequestedBuild = false;
};

class FMistWaitForQMovement final : public IAutomationLatentCommand
{
public:
    explicit FMistWaitForQMovement(FAutomationTestBase* InTest) : Test(InTest) {}

    virtual bool Update() override
    {
        const double Now = FPlatformTime::Seconds();
        if (StartedAt < 0.0) StartedAt = Now;
        UWorld* World = GEditor ? GEditor->PlayWorld : nullptr;
        if (Now - StartedAt >= 30.0)
        {
            APlayerController* Player = World ? World->GetFirstPlayerController() : nullptr;
            APawn* PlayerPawn = Player ? Player->GetPawn() : nullptr;
            UNavigationSystemV1* Navigation = World ? FNavigationSystem::GetCurrent<UNavigationSystemV1>(World) : nullptr;
            ANavigationData* NavData = Navigation ? Navigation->GetDefaultNavDataInstance(FNavigationSystem::DontCreate) : nullptr;
            FNavLocation ProjectedPawn;
            const bool bPawnProjects = Navigation && PlayerPawn && Navigation->ProjectPointToNavigation(
                PlayerPawn->GetActorLocation(), ProjectedPawn, FVector(150.f, 150.f, 400.f));
            Test->AddError(FString::Printf(
                TEXT("Timed out waiting for Q movement test, stage %d. GameTime=%.3f Pawn=%s NavData=%s PawnProjects=%d NavBuilding=%d"),
                static_cast<int32>(Stage), World ? World->GetTimeSeconds() : -1.0,
                PlayerPawn ? *PlayerPawn->GetActorLocation().ToCompactString() : TEXT("null"),
                *GetNameSafe(NavData), bPawnProjects ? 1 : 0, Navigation && Navigation->IsNavigationBuildInProgress() ? 1 : 0));
            return true;
        }

        if (!World || !World->HasBegunPlay()) return false;
        AMistworldPlayerController* Controller = Cast<AMistworldPlayerController>(World->GetFirstPlayerController());
        APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
        if (!Pawn) return false;

        const double GameTime = World->GetTimeSeconds();
        switch (Stage)
        {
        case EStage::WaitForNavigation:
        {
            if (SettlingAt < 0.0) SettlingAt = GameTime;
            if (GameTime - SettlingAt < 1.0) return false;
            if (!FindDestinations(World, Controller, Pawn->GetActorLocation())) return false;

            MoveStartingLocation = Pawn->GetActorLocation();
            FMistCombatInputTestAccess::ClickDestination(Controller, FirstDestination);
            Test->AddInfo(TEXT("Initial move issued: ") + FMistCombatInputTestAccess::MovementDiagnostics(Controller));
            Advance(EStage::WaitForInitialMovement, GameTime);
            return false;
        }
        case EStage::WaitForInitialMovement:
            if (GameTime - StageStartedAt < 0.5) return false;
            if (FVector::Dist2D(MoveStartingLocation, Pawn->GetActorLocation()) <= 20.f && GameTime - StageStartedAt < 5.0) return false;
            Test->AddInfo(FString::Printf(TEXT("Initial movement after %.3fs: Displacement=%.3f "),
                GameTime - StageStartedAt, FVector::Dist2D(MoveStartingLocation, Pawn->GetActorLocation()))
                + FMistCombatInputTestAccess::MovementDiagnostics(Controller));
            if (!Test->TestTrue(TEXT("Template pawn actually moves along initial click path"),
                FVector::Dist2D(MoveStartingLocation, Pawn->GetActorLocation()) > 20.f)) return true;
            Test->TestTrue(TEXT("Initial click has an active navigation path"), FMistCombatInputTestAccess::IsNavigating(Controller));

            AimStartingLocation = Pawn->GetActorLocation();
            Controller->InputKey(FInputKeyEventArgs(nullptr, INPUTDEVICEID_NONE, EKeys::Q, IE_Pressed,
                1.f, false, FPlatformTime::Cycles64()));
            Advance(EStage::WaitForMovingAim, GameTime);
            return false;

        case EStage::WaitForMovingAim:
            if (GameTime - StageStartedAt < 0.3) return false;
            if (FVector::Dist2D(AimStartingLocation, Pawn->GetActorLocation()) <= 10.f && GameTime - StageStartedAt < 5.0) return false;
            Test->AddInfo(FString::Printf(TEXT("Q aiming after %.3fs: Displacement=%.3f "),
                GameTime - StageStartedAt, FVector::Dist2D(AimStartingLocation, Pawn->GetActorLocation()))
                + FMistCombatInputTestAccess::MovementDiagnostics(Controller));
            Test->TestTrue(TEXT("Actual Q key press starts aiming"), FMistCombatInputTestAccess::IsAiming(Controller));
            Test->TestTrue(TEXT("Holding Q preserves the existing navigation path"), FMistCombatInputTestAccess::IsNavigating(Controller));
            Test->TestTrue(TEXT("Character continues moving while Q preview is held"),
                FVector::Dist2D(AimStartingLocation, Pawn->GetActorLocation()) > 10.f);

            MoveStartingLocation = Pawn->GetActorLocation();
            FMistCombatInputTestAccess::ClickDestination(Controller, SecondDestination);
            Test->AddInfo(TEXT("Aiming redirect issued: ") + FMistCombatInputTestAccess::MovementDiagnostics(Controller));
            Test->TestTrue(TEXT("New click while aiming creates a navigation path"), FMistCombatInputTestAccess::IsNavigating(Controller));
            Test->TestTrue(TEXT("New click while aiming redirects the path destination"),
                FVector::Dist2D(FMistCombatInputTestAccess::NavigationGoal(Controller), SecondDestination) < 10.f);
            Test->TestTrue(TEXT("Clicking to move leaves the Q preview active"), FMistCombatInputTestAccess::IsAiming(Controller));
            Advance(EStage::WaitForRedirectedMovement, GameTime);
            return false;

        case EStage::WaitForRedirectedMovement:
            if (GameTime - StageStartedAt < 0.3) return false;
            if (FVector::Dist2D(MoveStartingLocation, Pawn->GetActorLocation()) <= 10.f && GameTime - StageStartedAt < 5.0) return false;
            Test->AddInfo(FString::Printf(TEXT("Redirect movement after %.3fs: Displacement=%.3f "),
                GameTime - StageStartedAt, FVector::Dist2D(MoveStartingLocation, Pawn->GetActorLocation()))
                + FMistCombatInputTestAccess::MovementDiagnostics(Controller));
            Test->TestTrue(TEXT("Character moves after a new click while holding Q"),
                FVector::Dist2D(MoveStartingLocation, Pawn->GetActorLocation()) > 10.f);

            CastStartingLocation = Pawn->GetActorLocation();
            FMistCombatInputTestAccess::CommitCleave(Controller, FVector::ForwardVector);
            Test->TestTrue(TEXT("Q commit starts the wind-up"), FMistCombatInputTestAccess::IsCasting(Controller));
            Test->TestFalse(TEXT("Q commit stops path following"), FMistCombatInputTestAccess::IsNavigating(Controller));
            FMistCombatInputTestAccess::ClickDestination(Controller, FirstDestination);
            Test->TestFalse(TEXT("Click during committed wind-up cannot restart navigation"), FMistCombatInputTestAccess::IsNavigating(Controller));
            Advance(EStage::WaitForStationaryCast, GameTime);
            return false;

        case EStage::WaitForStationaryCast:
            if (GameTime - StageStartedAt < 0.1) return false;
            Test->TestTrue(TEXT("Q is still in its 0.25-second wind-up"), FMistCombatInputTestAccess::IsCasting(Controller));
            Test->TestTrue(TEXT("Character stays stationary during committed Q wind-up"),
                FVector::Dist2D(CastStartingLocation, Pawn->GetActorLocation()) < 2.f);
            Advance(EStage::WaitForCastResolution, GameTime);
            return false;

        case EStage::WaitForCastResolution:
            if (FMistCombatInputTestAccess::IsCasting(Controller)) return false;
            Test->TestFalse(TEXT("Q timer releases the committed cast lock"), FMistCombatInputTestAccess::IsCasting(Controller));
            MoveStartingLocation = Pawn->GetActorLocation();
            FMistCombatInputTestAccess::ClickDestination(Controller, FirstDestination);
            Advance(EStage::WaitForPostCastMovement, GameTime);
            return false;

        case EStage::WaitForPostCastMovement:
            if (GameTime - StageStartedAt < 0.3) return false;
            if (FVector::Dist2D(MoveStartingLocation, Pawn->GetActorLocation()) <= 10.f && GameTime - StageStartedAt < 5.0) return false;
            Test->AddInfo(FString::Printf(TEXT("Post-cast movement after %.3fs: Displacement=%.3f "),
                GameTime - StageStartedAt, FVector::Dist2D(MoveStartingLocation, Pawn->GetActorLocation()))
                + FMistCombatInputTestAccess::MovementDiagnostics(Controller));
            Test->TestTrue(TEXT("Fresh click resumes character movement after Q resolves"),
                FVector::Dist2D(MoveStartingLocation, Pawn->GetActorLocation()) > 10.f);
            return true;
        }
        return true;
    }

private:
    enum class EStage
    {
        WaitForNavigation, WaitForInitialMovement, WaitForMovingAim,
        WaitForRedirectedMovement, WaitForStationaryCast, WaitForCastResolution,
        WaitForPostCastMovement
    };

    void Advance(EStage NextStage, double GameTime)
    {
        Stage = NextStage;
        StageStartedAt = GameTime;
    }

    bool FindDestinations(UWorld* World, AMistworldPlayerController* Controller, const FVector& Origin)
    {
        UNavigationSystemV1* Navigation = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
        if (!Navigation) return false;
        const FVector Offsets[] = {
            FVector(800.f, 0.f, 0.f), FVector(-800.f, 0.f, 0.f),
            FVector(0.f, 800.f, 0.f), FVector(0.f, -800.f, 0.f),
            FVector(600.f, 600.f, 0.f), FVector(-600.f, -600.f, 0.f)
        };
        TArray<FVector> Destinations;
        for (const FVector& Offset : Offsets)
        {
            FNavLocation Location;
            if (!Navigation->ProjectPointToNavigation(Origin + Offset, Location, FVector(80.f, 80.f, 300.f))) continue;
            if (FVector::Dist2D(Origin, Location.Location) < 500.f) continue;
            UNavigationPath* Path = Navigation->FindPathToLocationSynchronously(World, Origin, Location.Location, Controller);
            if (!Path || !Path->IsValid() || Path->IsPartial()) continue;
            if (Destinations.Num() && FVector::Dist2D(Destinations[0], Location.Location) < 500.f) continue;
            Destinations.Add(Location.Location);
            if (Destinations.Num() == 2)
            {
                FirstDestination = Destinations[0];
                SecondDestination = Destinations[1];
                return true;
            }
        }
        return false;
    }

    FAutomationTestBase* Test;
    EStage Stage = EStage::WaitForNavigation;
    double StartedAt = -1.0;
    double SettlingAt = -1.0;
    double StageStartedAt = 0.0;
    FVector FirstDestination;
    FVector SecondDestination;
    FVector MoveStartingLocation;
    FVector AimStartingLocation;
    FVector CastStartingLocation;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMistQMovementTest, "Mistworld.Combat.QMovement",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMistQMovementTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(TEXT("/Game/TopDown/Lvl_TopDown")));
    ADD_LATENT_AUTOMATION_COMMAND(FMistWaitForEditorNavigation(this));
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(FMistWaitForQMovement(this));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    return true;
}

#endif
