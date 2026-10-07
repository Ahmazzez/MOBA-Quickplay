#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Tests/AutomationEditorCommon.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/PlatformTime.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerStart.h"
#include "Blueprint/AIBlueprintHelperLibrary.h"
#include "NavigationSystem.h"
#include "Navigation/PathFollowingComponent.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "MistworldPlayerController.h"
#include "MistCombatHealth.h"
#include "MistTrainingDummy.h"

struct FMistAbilityTestAccess
{
    static void Reset(AMistworldPlayerController* PC) { PC->ResetCombatSandbox(); }
    static void Commit(AMistworldPlayerController* PC, const FKey& Key, const FVector& Direction) { PC->CommitAbility(Key, Direction); }
    static bool Endpoint(AMistworldPlayerController* PC, const FVector& Direction, float Distance, FVector& End) { return PC->FindLegalMovementEndpoint(Direction, Distance, End); }
    static void Flash(AMistworldPlayerController* PC, const FVector& Direction, float Distance) { PC->TryFlash(Direction, Distance); }
    static bool CanAim(AMistworldPlayerController* PC, const FKey& Key) { return PC->CanAimAbility(Key); }
    static double WReady(AMistworldPlayerController* PC) { return PC->WReadyAt; }
    static double RReady(AMistworldPlayerController* PC) { return PC->RReadyAt; }
    static bool Moving(AMistworldPlayerController* PC) { return PC->PathFollowingComponent->GetStatus() == EPathFollowingStatus::Moving; }
    static void Stop(AMistworldPlayerController* PC) { PC->StopForCommittedAttack(); }
    static void RequestAuto(AMistworldPlayerController* PC, AActor* HoveredTarget) { PC->RequestBasicAttack(HoveredTarget); }
    static AActor* AutoTarget(AMistworldPlayerController* PC) { return PC->AttackTarget.Get(); }
    static double AutoReady(AMistworldPlayerController* PC) { return PC->AutoReadyAt; }
    static bool ValidAutoTarget(AMistworldPlayerController* PC, AActor* Target, bool bRequireRange = true)
    { return PC->IsBasicAttackTargetValid(Target, bRequireRange); }
    static void CancelAuto(AMistworldPlayerController* PC) { PC->CancelCombatInput(); }
    static void MovementClick(AMistworldPlayerController* PC) { PC->OnInputStarted(); }
};

namespace
{
class FMistWaitForKitNavigation final : public IAutomationLatentCommand
{
public:
    explicit FMistWaitForKitNavigation(FAutomationTestBase* InTest) : Test(InTest) {}
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
                    if (Nav->ProjectPointToNavigation(It->GetActorLocation(), Point, FVector(150.f,150.f,400.f))) return true;
                }
        }
        if (FPlatformTime::Seconds() - Started > 30.0) { Test->AddError(TEXT("Base-kit test navigation unavailable.")); return true; }
        return false;
    }
private:
    FAutomationTestBase* Test;
    double Started = -1.0;
    bool bBuilt = false;
};

class FMistVerifyBaseKit final : public IAutomationLatentCommand
{
public:
    explicit FMistVerifyBaseKit(FAutomationTestBase* InTest) : Test(InTest) {}
    bool Update() override
    {
        if (Started < 0.0) Started = FPlatformTime::Seconds();
        if (FPlatformTime::Seconds() - Started > 35.0)
        {
            Test->AddError(FString::Printf(TEXT("Base-kit test timed out at stage %d."), Stage)); return true;
        }
        UWorld* World = GEditor ? GEditor->PlayWorld : nullptr;
        AMistworldPlayerController* PC = World ? Cast<AMistworldPlayerController>(World->GetFirstPlayerController()) : nullptr;
        APawn* Pawn = PC ? PC->GetPawn() : nullptr;
        UMistCombatHealth* HP = Pawn ? Pawn->FindComponentByClass<UMistCombatHealth>() : nullptr;
        if (!World || !World->HasBegunPlay() || !HP) return false;
        const double Now = World->GetTimeSeconds();
        switch (Stage)
        {
        case 0:
            if (Settled < 0.0) Settled = Now;
            if (Now - Settled < 1.0) return false;
            if (!FindDirection(PC, 600.f, Direction, Endpoint)) return false;
            FMistAbilityTestAccess::Reset(PC);
            HP->Health = 700.f;
            StartPoint = Pawn->GetActorLocation();
            Dummy = SpawnDummy(World, StartPoint + Direction * 120.f);
            SecondDummy = SpawnDummy(World, StartPoint + Direction * 180.f);
            if (!Dummy.IsValid() || !SecondDummy.IsValid()) { Test->AddError(TEXT("Failed to spawn dash test targets.")); return true; }
            FMistAbilityTestAccess::Commit(PC, EKeys::W, Direction);
            Test->TestTrue(TEXT("W commitment starts wind-up"), PC->IsCombatBusy());
            Test->TestFalse(TEXT("Ordinary W cannot be aimed while on cooldown"), FMistAbilityTestAccess::CanAim(PC, EKeys::W));
            Stage = 1; StageAt = Now; return false;
        case 1:
            if (PC->IsCombatBusy() || Now - StageAt < 0.5) return false;
            Test->TestEqual(TEXT("W deals 100 physical damage once to first target"), Dummy->CombatHealth->Health, 1200.f - 100.f / 1.2f, 0.1f);
            Test->TestEqual(TEXT("A unit does not shield a second W target"), SecondDummy->CombatHealth->Health, 1200.f - 100.f / 1.2f, 0.1f);
            Test->TestEqual(TEXT("Ordinary W provides no healing"), HP->Health, 700.f, 0.01f);
            Test->TestTrue(TEXT("W reaches its legal endpoint"), FVector::Dist2D(Pawn->GetActorLocation(), Endpoint) < 5.f);
            Dummy->Destroy(); SecondDummy->Destroy();
            if (!FindDirection(PC, 300.f, Direction, Endpoint)) { Test->AddError(TEXT("No route for E movement check.")); return true; }
            UAIBlueprintHelperLibrary::SimpleMoveToLocation(PC, Endpoint);
            FMistAbilityTestAccess::Commit(PC, EKeys::E, FVector::ZeroVector);
            Test->TestTrue(TEXT("Instant E preserves a running navigation order"), FMistAbilityTestAccess::Moving(PC));
            Test->TestTrue(TEXT("E provides its five-second guard"), PC->GetDefenseRemaining() > 4.9f);
            Test->TestEqual(TEXT("E halves post-armor damage"), HP->ReceivePhysicalDamage(90.f), 37.5f, 0.01f);
            FMistAbilityTestAccess::Stop(PC);
            HP->ResetHealth(); HP->Health = 500.f;
            if (!FindDirection(PC, 600.f, Direction, Endpoint)) { Test->AddError(TEXT("No route for ultimate test.")); return true; }
            Dummy = SpawnDummy(World, Pawn->GetActorLocation() + Direction * 120.f);
            FMistAbilityTestAccess::Commit(PC, EKeys::R, Direction);
            Test->TestTrue(TEXT("R initially spends a 120-second cooldown"), FMistAbilityTestAccess::RReady(PC) - Now > 119.9);
            Stage = 2; StageAt = Now; return false;
        case 2:
            if (PC->GetUltimateRecastRemaining() <= 0.f) return false;
            Test->TestEqual(TEXT("Ultimate hits the target once on each leg"), Dummy->CombatHealth->Health, 1200.f - 320.f / 1.2f, 0.1f);
            Test->TestEqual(TEXT("Both sword legs heal for 20 percent actual health removed"), HP->Health, 500.f + (320.f / 1.2f) * 0.2f, 0.1f);
            Dummy->Destroy();
            if (!FindDirection(PC, 800.f, Direction, Endpoint)) { Test->AddError(TEXT("No route for empowered dash.")); return true; }
            Dummy = SpawnDummy(World, Pawn->GetActorLocation() + Direction * 120.f);
            WCooldown = FMistAbilityTestAccess::WReady(PC);
            RCooldown = FMistAbilityTestAccess::RReady(PC);
            Test->TestTrue(TEXT("Ordinary W remains on cooldown when R recast is available"), WCooldown > Now);
            FMistAbilityTestAccess::Commit(PC, EKeys::R, Direction);
            Test->TestTrue(TEXT("R recast starts despite W cooldown"), PC->IsCombatBusy());
            Test->TestEqual(TEXT("R recast leaves W cooldown unchanged"), FMistAbilityTestAccess::WReady(PC), WCooldown);
            Test->TestEqual(TEXT("R recast leaves R cooldown unchanged"), FMistAbilityTestAccess::RReady(PC), RCooldown);
            Stage = 3; StageAt = Now; return false;
        case 3:
            if (PC->IsCombatBusy() || Now - StageAt < 0.5) return false;
            Test->TestEqual(TEXT("Empowered dash deals 180 raw damage once"), Dummy->CombatHealth->Health, 1050.f, 0.1f);
            Test->TestEqual(TEXT("Empowered dash adds 20 percent actual-damage healing"), HP->Health, 500.f + (320.f / 1.2f) * 0.2f + 30.f, 0.1f);
            Test->TestEqual(TEXT("Recast is consumed"), PC->GetUltimateRecastRemaining(), 0.f);
            Dummy->Destroy();
            if (!FindDirection(PC, 600.f, Direction, Endpoint)) { Test->AddError(TEXT("No legal Flash endpoint.")); return true; }
            StartPoint = Pawn->GetActorLocation();
            FMistAbilityTestAccess::Flash(PC, Direction, 600.f);
            Test->TestTrue(TEXT("Flash instantly reaches its legal endpoint"), FVector::Dist2D(Pawn->GetActorLocation(), Endpoint) < 1.f);
            Test->TestTrue(TEXT("Flash moves at most 600cm"), FVector::Dist2D(StartPoint, Pawn->GetActorLocation()) <= 600.1f);
            Test->TestFalse(TEXT("F Flash is unavailable during its 60-second cooldown"), FMistAbilityTestAccess::CanAim(PC, EKeys::F));
            FMistAbilityTestAccess::Reset(PC);
            HP->SetRoot(5.f);
            StartPoint = Pawn->GetActorLocation();
            FMistAbilityTestAccess::Flash(PC, Direction, 600.f);
            Test->TestTrue(TEXT("Root prevents Flash and consumes no movement"), FVector::Dist2D(StartPoint, Pawn->GetActorLocation()) < 0.01f);
            HP->SetRoot(0.f); HP->SetSilence(5.f);
            Test->TestTrue(TEXT("Silence permits F Flash"), FMistAbilityTestAccess::CanAim(PC, EKeys::F));
            Test->TestFalse(TEXT("Space is no longer a Flash ability key"), FMistAbilityTestAccess::CanAim(PC, EKeys::SpaceBar));
            Test->TestFalse(TEXT("Silence prevents normal abilities"), FMistAbilityTestAccess::CanAim(PC, EKeys::Q));
            HP->ResetHealth();
            if (!FindDirection(PC, 300.f, Direction, Endpoint)) { Test->AddError(TEXT("No route for refund test.")); return true; }
            Dummy = SpawnDummy(World, Pawn->GetActorLocation() + Direction * 120.f);
            SecondDummy = SpawnDummy(World, Pawn->GetActorLocation() + Direction * 180.f);
            FMistAbilityTestAccess::Commit(PC, EKeys::R, Direction);
            Stage = 4; StageAt = Now; return false;
        case 4:
            if (PC->IsCombatBusy()) return false;
            RCooldown = FMistAbilityTestAccess::RReady(PC);
            FMistAbilityTestAccess::Commit(PC, EKeys::Q, Direction);
            Stage = 5; StageAt = Now; return false;
        case 5:
            if (PC->IsCombatBusy() || Now - StageAt < 0.3) return false;
            Test->TestEqual(TEXT("Q hitting multiple Elite targets refunds R exactly once"), FMistAbilityTestAccess::RReady(PC), RCooldown - 5.0, 0.01);
            Dummy->Destroy(); SecondDummy->Destroy();
            FMistAbilityTestAccess::Reset(PC);
            return true;
        }
        return true;
    }
private:
    bool FindDirection(AMistworldPlayerController* PC, float Distance, FVector& OutDirection, FVector& OutEndpoint)
    {
        for (int32 Angle = 0; Angle < 360; Angle += 30)
        {
            const FVector TryDirection = FVector::ForwardVector.RotateAngleAxis(Angle, FVector::UpVector);
            if (FMistAbilityTestAccess::Endpoint(PC, TryDirection, Distance, OutEndpoint)
                && FVector::Dist2D(PC->GetPawn()->GetActorLocation(), OutEndpoint) >= 220.f)
            { OutDirection = TryDirection; return true; }
        }
        return false;
    }
    AMistTrainingDummy* SpawnDummy(UWorld* World, const FVector& Position)
    {
        FActorSpawnParameters Params; Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        return World->SpawnActor<AMistTrainingDummy>(Position, FRotator::ZeroRotator, Params);
    }
    FAutomationTestBase* Test;
    int32 Stage = 0;
    double Started = -1.0, Settled = -1.0, StageAt = 0.0, WCooldown = 0.0, RCooldown = 0.0;
    FVector Direction, Endpoint, StartPoint;
    TWeakObjectPtr<AMistTrainingDummy> Dummy, SecondDummy;
};

// Exercise the actual possessed controller and impact timer. The targets have
// health and a Pawn collision capsule but are not training-dummy subclasses.
class FMistVerifyBasicAttackTargeting final : public IAutomationLatentCommand
{
public:
    explicit FMistVerifyBasicAttackTargeting(FAutomationTestBase* InTest) : Test(InTest) {}

    bool Update() override
    {
        if (Started < 0.0) Started = FPlatformTime::Seconds();
        if (FPlatformTime::Seconds() - Started > 30.0)
        {
            Test->AddError(FString::Printf(TEXT("Basic-attack targeting timed out at stage %d."), Stage));
            return true;
        }
        UWorld* World = GEditor ? GEditor->PlayWorld : nullptr;
        AMistworldPlayerController* PC = World
            ? Cast<AMistworldPlayerController>(World->GetFirstPlayerController()) : nullptr;
        APawn* Pawn = PC ? PC->GetPawn() : nullptr;
        UMistCombatHealth* PlayerHealth = Pawn ? Pawn->FindComponentByClass<UMistCombatHealth>() : nullptr;
        if (!World || !World->HasBegunPlay() || !PlayerHealth) return false;
        const double Now = World->GetTimeSeconds();

        if (Stage == 0)
        {
            if (SettledAt < 0.0) SettledAt = Now;
            if (Now - SettledAt < 1.0) return false;
            FMistAbilityTestAccess::Reset(PC);
            Near = SpawnTarget(World, Pawn->GetActorLocation());
            Far = SpawnTarget(World, Pawn->GetActorLocation());
            if (!Near.IsValid() || !Far.IsValid())
            {
                Test->AddError(TEXT("Cannot create generic health-bearing basic-attack targets."));
                return true;
            }
            const FVector Origin = Pawn->GetActorLocation();
            bool bFoundClearPair = false;
            for (int32 Angle = 0; Angle < 360; Angle += 30)
            {
                NearDirection = FVector::ForwardVector.RotateAngleAxis(Angle, FVector::UpVector);
                FarDirection = NearDirection.RotateAngleAxis(90.f, FVector::UpVector);
                Near->SetActorLocation(Origin + NearDirection * 160.f);
                Far->SetActorLocation(Origin + FarDirection * 220.f);
                if (FMistAbilityTestAccess::ValidAutoTarget(PC, Near.Get())
                    && FMistAbilityTestAccess::ValidAutoTarget(PC, Far.Get()))
                {
                    bFoundClearPair = true;
                    break;
                }
            }
            if (!Test->TestTrue(TEXT("Test arena provides two visible targets within basic range"), bFoundClearPair)) return true;
            Test->TestFalse(TEXT("Targeting supports actors beyond the training-dummy class"), Near->IsA<AMistTrainingDummy>());

            AActor* Dead = SpawnTarget(World, Origin + NearDirection * 60.f);
            AActor* Immune = SpawnTarget(World, Origin + NearDirection * 80.f);
            AActor* Hidden = SpawnTarget(World, Origin + NearDirection * 100.f);
            AActor* Above = SpawnTarget(World, Origin + NearDirection * 110.f + FVector(0.f, 0.f, 200.f));
            AActor* Outside = SpawnTarget(World, Origin + FarDirection * 350.f);
            AActor* NoHealth = SpawnTarget(World, Origin + NearDirection * 40.f, false);
            AActor* Destroyed = SpawnTarget(World, Origin + NearDirection * 30.f);
            HealthOf(Dead)->Health = 0.f;
            HealthOf(Immune)->GrantInvulnerability(10.f);
            Hidden->SetActorHiddenInGame(true);
            Destroyed->Destroy();
            Test->TestFalse(TEXT("Dead units are not basic-attack candidates"), FMistAbilityTestAccess::ValidAutoTarget(PC, Dead));
            Test->TestFalse(TEXT("Invulnerable units are not basic-attack candidates"), FMistAbilityTestAccess::ValidAutoTarget(PC, Immune));
            Test->TestFalse(TEXT("Hidden units are not basic-attack candidates"), FMistAbilityTestAccess::ValidAutoTarget(PC, Hidden));
            Test->TestFalse(TEXT("Targets on a different elevation are excluded"), FMistAbilityTestAccess::ValidAutoTarget(PC, Above));
            Test->TestFalse(TEXT("Untargeted units beyond attack range are excluded"), FMistAbilityTestAccess::ValidAutoTarget(PC, Outside));
            Test->TestFalse(TEXT("Actors without combat health are excluded"), FMistAbilityTestAccess::ValidAutoTarget(PC, NoHealth));
            Test->TestFalse(TEXT("A destroyed target is excluded"), FMistAbilityTestAccess::ValidAutoTarget(PC, Destroyed));
            Test->TestFalse(TEXT("The player cannot attack their own pawn"), FMistAbilityTestAccess::ValidAutoTarget(PC, Pawn));

            FMistAbilityTestAccess::RequestAuto(PC, nullptr);
            Test->TestTrue(TEXT("A Space attack order without a hovered target picks the nearest valid enemy"),
                FMistAbilityTestAccess::AutoTarget(PC) == Near.Get());
            FMistAbilityTestAccess::CancelAuto(PC);
            FMistAbilityTestAccess::RequestAuto(PC, Far.Get());
            Test->TestTrue(TEXT("A hovered enemy overrides a closer enemy"), FMistAbilityTestAccess::AutoTarget(PC) == Far.Get());
            FMistAbilityTestAccess::CancelAuto(PC);
            FMistAbilityTestAccess::RequestAuto(PC, Dead);
            Test->TestTrue(TEXT("An invalid hovered target falls back to the nearest enemy"), FMistAbilityTestAccess::AutoTarget(PC) == Near.Get());
            FMistAbilityTestAccess::CancelAuto(PC);
            FMistAbilityTestAccess::RequestAuto(PC, Pawn);
            Test->TestTrue(TEXT("Hovering the player still allows nearest-enemy targeting"), FMistAbilityTestAccess::AutoTarget(PC) == Near.Get());
            FMistAbilityTestAccess::CancelAuto(PC);
            HealthOf(Near.Get())->Health = 0.f;
            HealthOf(Far.Get())->Health = 0.f;
            FMistAbilityTestAccess::RequestAuto(PC, nullptr);
            Test->TestNull(TEXT("Automatic targeting does not approach the only surviving out-of-range enemy"),
                FMistAbilityTestAccess::AutoTarget(PC));
            HealthOf(Near.Get())->ResetHealth();
            HealthOf(Far.Get())->ResetHealth();
            FMistAbilityTestAccess::RequestAuto(PC, Outside);
            Test->TestTrue(TEXT("An explicitly hovered enemy beyond range retains its approach order"),
                FMistAbilityTestAccess::AutoTarget(PC) == Outside);
            Test->TestFalse(TEXT("Selecting a distant target does not begin an out-of-range attack"), PC->IsCombatBusy());
            Test->TestEqual(TEXT("Selection alone does not spend the basic-attack cooldown"), FMistAbilityTestAccess::AutoReady(PC), 0.0);
            FMistAbilityTestAccess::CancelAuto(PC);
            for (AActor* Decoy : {Dead, Immune, Hidden, Above, Outside, NoHealth}) Decoy->Destroy();

            FMistAbilityTestAccess::RequestAuto(PC, nullptr);
            Test->TestEqual(TEXT("The target takes no damage at selection time"), HealthOf(Near.Get())->Health, 1200.f);
            Stage = 1;
            StageAt = Now;
            return false;
        }

        if (!Near.IsValid() || !Far.IsValid())
        {
            Test->AddError(TEXT("Basic-attack test target disappeared."));
            return true;
        }

        if (Stage == 1)
        {
            if (PC->IsCombatBusy() || Now - StageAt < 0.25) return false;
            Test->TestEqual(TEXT("The nearest generic enemy takes one timed 90-raw basic attack"), HealthOf(Near.Get())->Health, 1125.f, 0.1f);
            Test->TestEqual(TEXT("A farther enemy is not hit by the nearest-target attack"), HealthOf(Far.Get())->Health, 1200.f);
            const double Cooldown = FMistAbilityTestAccess::AutoReady(PC);
            Test->TestTrue(TEXT("Impact retains the one-second basic-attack cooldown"), Cooldown > Now);
            FMistAbilityTestAccess::RequestAuto(PC, Far.Get());
            Test->TestTrue(TEXT("Hover override can change the repeat order during cooldown"), FMistAbilityTestAccess::AutoTarget(PC) == Far.Get());
            Test->TestEqual(TEXT("Retargeting does not reset or extend the existing cooldown"), FMistAbilityTestAccess::AutoReady(PC), Cooldown);
            Test->TestFalse(TEXT("Retargeting cannot start a second wind-up during cooldown"), PC->IsCombatBusy());
            FMistAbilityTestAccess::MovementClick(PC);
            Test->TestNull(TEXT("A movement click cancels the basic-attack repeat order"), FMistAbilityTestAccess::AutoTarget(PC));
            Stage = 2;
            StageAt = Now;
            return false;
        }

        if (Stage == 2)
        {
            if (Now - StageAt < 0.25) return false;
            Test->TestEqual(TEXT("A cancelled override deals no extra damage during cooldown"), HealthOf(Far.Get())->Health, 1200.f);
            FMistAbilityTestAccess::Reset(PC);
            const FVector Origin = Pawn->GetActorLocation();
            Near->SetActorLocation(Origin + NearDirection * 160.f);
            Far->SetActorLocation(Origin + FarDirection * 220.f);
            HealthOf(Near.Get())->ResetHealth();
            HealthOf(Far.Get())->ResetHealth();
            Wall = SpawnWall(World, Origin + NearDirection * 80.f, NearDirection.Rotation());
            if (!Test->TestNotNull(TEXT("LOS test wall spawns"), Wall.Get())) return true;
            Test->TestFalse(TEXT("An otherwise eligible enemy behind a wall is excluded from automatic selection"),
                FMistAbilityTestAccess::ValidAutoTarget(PC, Near.Get()));
            Test->TestTrue(TEXT("The other target remains visible beside the wall"), FMistAbilityTestAccess::ValidAutoTarget(PC, Far.Get()));
            FMistAbilityTestAccess::RequestAuto(PC, Near.Get());
            Test->TestTrue(TEXT("Hovering an enemy behind terrain retains an explicit approach order"),
                FMistAbilityTestAccess::AutoTarget(PC) == Near.Get());
            Test->TestFalse(TEXT("A hovered target behind a wall cannot immediately start a swing"), PC->IsCombatBusy());
            FMistAbilityTestAccess::CancelAuto(PC);

            HealthOf(Far.Get())->Health = 0.f;
            const double Cooldown = FMistAbilityTestAccess::AutoReady(PC);
            FMistAbilityTestAccess::RequestAuto(PC, nullptr);
            Test->TestNull(TEXT("With only blocked or dead enemies there is no automatic target"), FMistAbilityTestAccess::AutoTarget(PC));
            Test->TestFalse(TEXT("An empty attack request does not begin a wind-up"), PC->IsCombatBusy());
            Test->TestEqual(TEXT("An empty attack request consumes no cooldown"), FMistAbilityTestAccess::AutoReady(PC), Cooldown);
            HealthOf(Far.Get())->ResetHealth();
            FMistAbilityTestAccess::RequestAuto(PC, nullptr);
            Test->TestTrue(TEXT("Automatic targeting selects the visible enemy instead of the nearer blocked one"),
                FMistAbilityTestAccess::AutoTarget(PC) == Far.Get());
            Stage = 3;
            StageAt = Now;
            return false;
        }

        if (Stage == 3)
        {
            if (PC->IsCombatBusy() || Now - StageAt < 0.25) return false;
            Test->TestEqual(TEXT("The visible alternative receives the actual basic-attack damage"), HealthOf(Far.Get())->Health, 1125.f, 0.1f);
            Test->TestEqual(TEXT("Basic attacks do not damage a nearer enemy through terrain"), HealthOf(Near.Get())->Health, 1200.f);
            FMistAbilityTestAccess::CancelAuto(PC);
            Near->Destroy();
            Far->Destroy();
            Wall->Destroy();
            FMistAbilityTestAccess::Reset(PC);
            return true;
        }
        return false;
    }

private:
    static UMistCombatHealth* HealthOf(AActor* Actor)
    {
        return Actor ? Actor->FindComponentByClass<UMistCombatHealth>() : nullptr;
    }
    static AActor* SpawnTarget(UWorld* World, const FVector& Position, bool bWithHealth = true)
    {
        AActor* Actor = World->SpawnActor<AActor>();
        if (!Actor) return nullptr;
        UCapsuleComponent* Capsule = NewObject<UCapsuleComponent>(Actor);
        Actor->SetRootComponent(Capsule);
        Capsule->InitCapsuleSize(40.f, 96.f);
        Capsule->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
        Capsule->SetCollisionObjectType(ECC_Pawn);
        Capsule->SetCollisionResponseToAllChannels(ECR_Ignore);
        Capsule->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
        Capsule->SetCanEverAffectNavigation(false);
        Capsule->RegisterComponent();
        Actor->SetActorLocation(Position);
        if (bWithHealth)
        {
            UMistCombatHealth* Health = NewObject<UMistCombatHealth>(Actor);
            Actor->AddInstanceComponent(Health);
            Health->RegisterComponent();
        }
        return Actor;
    }
    static AActor* SpawnWall(UWorld* World, const FVector& Position, const FRotator& Rotation)
    {
        AActor* Actor = World->SpawnActor<AActor>();
        if (!Actor) return nullptr;
        UBoxComponent* Box = NewObject<UBoxComponent>(Actor);
        Actor->SetRootComponent(Box);
        Box->SetBoxExtent(FVector(15.f, 35.f, 180.f));
        Box->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
        Box->SetCollisionObjectType(ECC_WorldStatic);
        Box->SetCollisionResponseToAllChannels(ECR_Block);
        Box->SetCanEverAffectNavigation(false);
        Box->RegisterComponent();
        Actor->SetActorLocationAndRotation(Position, Rotation);
        return Actor;
    }
    FAutomationTestBase* Test;
    int32 Stage = 0;
    double Started = -1.0, SettledAt = -1.0, StageAt = 0.0;
    FVector NearDirection, FarDirection;
    TWeakObjectPtr<AActor> Near, Far, Wall;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMistBaseKitTest, "Mistworld.Combat.BaseKit",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FMistBaseKitTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(TEXT("/Game/TopDown/Lvl_TopDown")));
    ADD_LATENT_AUTOMATION_COMMAND(FMistWaitForKitNavigation(this));
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(FMistVerifyBaseKit(this));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMistBasicAttackTargetingTest, "Mistworld.Combat.BasicAttackTargeting",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FMistBasicAttackTargetingTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(TEXT("/Game/TopDown/Lvl_TopDown")));
    ADD_LATENT_AUTOMATION_COMMAND(FMistWaitForKitNavigation(this));
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(FMistVerifyBasicAttackTargeting(this));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    return true;
}
#endif
