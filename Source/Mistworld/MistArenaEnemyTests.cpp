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
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "NavigationSystem.h"
#include "NavigationPath.h"
#include "MistArenaEnemy.h"
#include "MistCombatHealth.h"
#include "MistworldPlayerController.h"

namespace
{
class FMistWaitForEnemyTestNavigation final : public IAutomationLatentCommand
{
public:
    explicit FMistWaitForEnemyTestNavigation(FAutomationTestBase* InTest) : Test(InTest) {}
    bool Update() override
    {
        if (Started < 0.0) Started = FPlatformTime::Seconds();
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        UNavigationSystemV1* Nav = World ? FNavigationSystem::GetCurrent<UNavigationSystemV1>(World) : nullptr;
        if (Nav)
        {
            if (!bBuilt) { Nav->Build(); bBuilt = true; }
            if (!Nav->IsNavigationBuildInProgress())
            {
                for (TActorIterator<APlayerStart> It(World); It; ++It)
                {
                    FNavLocation Point;
                    if (Nav->ProjectPointToNavigation(It->GetActorLocation(), Point, FVector(150.f, 150.f, 400.f)))
                        return true;
                }
            }
        }
        if (FPlatformTime::Seconds() - Started > 30.0)
        {
            Test->AddError(TEXT("Combat arena has no navigation at its PlayerStart."));
            return true;
        }
        return false;
    }
private:
    FAutomationTestBase* Test;
    double Started = -1.0;
    bool bBuilt = false;
};

// Exercise the real enemy actor, physics visibility, pawn, navigation and
// timed health effects. Only the enemy's automatic tick is disabled, allowing
// each assertion to run immediately before or after the actual state update.
class FMistVerifyArenaEnemy final : public IAutomationLatentCommand
{
public:
    explicit FMistVerifyArenaEnemy(FAutomationTestBase* InTest) : Test(InTest) {}

    bool Update() override
    {
        if (Started < 0.0) Started = FPlatformTime::Seconds();
        if (FPlatformTime::Seconds() - Started > 45.0)
        {
            Test->AddError(FString::Printf(TEXT("Arena enemy test timed out at stage %d; enemy state %s; location %s, home %s, forward %s, player half-height %.2f, enemy half-height %.2f, ground Z %.2f."),
                Stage, Enemy.IsValid() ? *Enemy->GetStateName() : TEXT("missing"),
                Enemy.IsValid() ? *Enemy->GetActorLocation().ToString() : TEXT("missing"), *Home.ToString(),
                *Forward.ToString(), PlayerHalfHeight, EnemyHalfHeight, GroundZ));
            return true;
        }
        UWorld* World = GEditor ? GEditor->PlayWorld : nullptr;
        AMistworldPlayerController* PC = World
            ? Cast<AMistworldPlayerController>(World->GetFirstPlayerController()) : nullptr;
        ACharacter* Pawn = PC ? Cast<ACharacter>(PC->GetPawn()) : nullptr;
        UMistCombatHealth* HP = Pawn ? Pawn->FindComponentByClass<UMistCombatHealth>() : nullptr;
        if (!World || !World->HasBegunPlay() || !HP) return false;
        const double Now = World->GetTimeSeconds();

        if (Stage == 0)
        {
            if (SettledAt < 0.0) SettledAt = Now;
            if (Now - SettledAt < 1.0) return false;
            UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
            if (!Test->TestNotNull(TEXT("Combat PIE world has navigation"), Nav)) return true;
            bool bFoundPlacedEnemy = false;
            FVector PlacedHome = FVector::ZeroVector;
            for (TActorIterator<AMistArenaEnemy> It(World); It; ++It)
            {
                FNavLocation PlacedPoint;
                const FVector ActualHome = It->GetHomeLocation();
                const float HalfHeight = It->Capsule->GetScaledCapsuleHalfHeight();
                const bool bHomeProjected = Nav->ProjectPointToNavigation(ActualHome, PlacedPoint,
                    FVector(20.f, 20.f, HalfHeight + 45.f));
                if (!Test->TestTrue(TEXT("The map's placed enemy home is navigable within movement tolerance"),
                    bHomeProjected && FVector::Dist2D(ActualHome, PlacedPoint.Location) <= 15.f))
                    return true;
                if (!bFoundPlacedEnemy) PlacedHome = ActualHome;
                bFoundPlacedEnemy = true;
            }
            if (!Test->TestTrue(TEXT("Dedicated combat map contains its real placed enemy"), bFoundPlacedEnemy)) return true;
            for (TActorIterator<AMistArenaEnemy> It(World); It; ++It) It->Destroy();
            PC->StopMovement();
            Pawn->GetCharacterMovement()->StopMovementImmediately();
            HP->ResetHealth();
            Home = PlacedHome;
            PlayerHalfHeight = Pawn->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
            EnemyHalfHeight = GetDefault<AMistArenaEnemy>()->Capsule->GetScaledCapsuleHalfHeight();
            FNavLocation HomePoint;
            if (!Test->TestTrue(TEXT("Fixture home projects onto actual navigable ground"),
                Nav->ProjectPointToNavigation(Home - FVector(0.f, 0.f, EnemyHalfHeight),
                    HomePoint, FVector(150.f, 150.f, 300.f)))) return true;
            Home.X = HomePoint.Location.X;
            Home.Y = HomePoint.Location.Y;
            FHitResult Ground;
            FCollisionObjectQueryParams GroundObjects;
            GroundObjects.AddObjectTypesToQuery(ECC_WorldStatic);
            FCollisionQueryParams GroundQuery(SCENE_QUERY_STAT(MistEnemyTestGround), false, Pawn);
            if (!World->LineTraceSingleByObjectType(Ground, Home,
                Home - FVector(0.f, 0.f, PlayerHalfHeight + 100.f), GroundObjects, GroundQuery)
                || Ground.ImpactNormal.Z < 0.7f)
            {
                Test->AddError(TEXT("Cannot determine the actual combat test floor under the player."));
                return true;
            }
            GroundZ = Ground.ImpactPoint.Z;
            // Ground the fixture using its own capsule rather than assuming the
            // player and enemy share dimensions or floor clearance.
            Home.Z = GroundZ + EnemyHalfHeight + 2.f;
            FNavLocation StrictHomePoint;
            const bool bStrictHomeProjected = Nav->ProjectPointToNavigation(Home, StrictHomePoint,
                FVector(20.f, 20.f, EnemyHalfHeight + 45.f));
            if (!Test->TestTrue(TEXT("Fixture movement starts on navigation within 15cm tolerance"),
                bStrictHomeProjected && FVector::Dist2D(Home, StrictHomePoint.Location) <= 15.f)) return true;
            Test->AddInfo(FString::Printf(TEXT("Enemy fixture: player capsule half-height %.2f; enemy half-height %.2f; actual ground Z %.2f; enemy home %s; player capsule nav relevance %d; player mesh nav relevance %d."),
                PlayerHalfHeight, EnemyHalfHeight, GroundZ, *Home.ToString(),
                Pawn->GetCapsuleComponent()->CanEverAffectNavigation() ? 1 : 0,
                Pawn->GetMesh() && Pawn->GetMesh()->CanEverAffectNavigation() ? 1 : 0));
            if (!FindFixture(World, Pawn))
            {
                Test->AddError(TEXT("Combat test map needs a clear, flat, navigable 1,900cm lane and lateral dodge space."));
                return true;
            }
            PlacePlayer(Pawn, Home + Forward * 600.f);
            FActorSpawnParameters Params;
            Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
            Enemy = World->SpawnActor<AMistArenaEnemy>(Home, Forward.Rotation(), Params);
            if (!Test->TestNotNull(TEXT("Actual arena enemy spawns"), Enemy.Get())) return true;
            Enemy->SetActorTickEnabled(false);
            Test->TestEqual(TEXT("Arena enemy starts with 900 health"), Enemy->CombatHealth->Health, 900.f);
            Test->TestEqual(TEXT("Arena enemy has 20 armor"), Enemy->CombatHealth->Armor, 20.f);
            Test->TestTrue(TEXT("Spawn location becomes the persistent encounter home"),
                Enemy->GetHomeLocation().Equals(Home, 0.1f));
            UNavigationPath* Path = UNavigationSystemV1::FindPathToLocationSynchronously(
                World, Enemy->GetActorLocation(), Pawn->GetActorLocation());
            Test->TestTrue(TEXT("Test approach uses a complete real navigation path"),
                Path && Path->IsValid() && !Path->IsPartial() && Path->PathPoints.Num() >= 2);
            Enemy->Tick(0.01f);
            Test->TestEqual(TEXT("A visible player inside aggro range begins approach"), Enemy->GetStateName(), FString(TEXT("Approach")));
            Stage = 1; StageAt = Now; LastTickAt = Now;
            return false;
        }
        if (!Enemy.IsValid()) { Test->AddError(TEXT("Arena enemy fixture disappeared.")); return true; }

        const FVector BeforeMove = Enemy->GetActorLocation();
        const float Delta = FMath::Clamp(static_cast<float>(Now - LastTickAt), 0.001f, 0.05f);
        LastTickAt = Now;
        Enemy->Tick(Delta);

        switch (Stage)
        {
        case 1:
            Test->TestTrue(TEXT("Enemy approach never exceeds its configured 250cm/s swept movement"),
                FVector::Dist2D(BeforeMove, Enemy->GetActorLocation()) <= 250.f * Delta + 0.1f);
            if (FVector::Dist2D(Home, Enemy->GetActorLocation()) < 100.f)
            {
                if (Now - StageAt >= 4.0)
                {
                    Test->AddError(FString::Printf(TEXT("Enemy approach made only %.2fcm progress in %.2f game seconds; location %s, home %s, state %s, forward %s."),
                        FVector::Dist2D(Home, Enemy->GetActorLocation()), Now - StageAt,
                        *Enemy->GetActorLocation().ToString(), *Home.ToString(), *Enemy->GetStateName(), *Forward.ToString()));
                    return true;
                }
                return false;
            }
            Test->TestTrue(TEXT("Enemy actually follows navigation toward the player"),
                FVector::DotProduct(Enemy->GetActorLocation() - Home, Forward) > 95.f);
            Test->TestEqual(TEXT("Approaching outside attack range causes no damage"), HP->Health, HP->MaxHealth);
            BeginCloseAttack(Pawn, HP, Now);
            Stage = 2;
            return false;
        case 2:
            if (Now - StageAt < 0.4) return false;
            Test->TestEqual(TEXT("A telegraphed attack causes no early damage"), HP->Health, HP->MaxHealth);
            Test->TestEqual(TEXT("Enemy remains in its 0.8-second wind-up"), Enemy->GetStateName(), FString(TEXT("Windup")));
            Test->TestTrue(TEXT("Attack reports remaining time before impact"), Enemy->GetStateRemaining() > 0.f);
            LockedDirection = Enemy->GetCommittedDirection();
            PlacePlayer(Pawn, Home + Side * 220.f);
            Test->TestFalse(TEXT("A lateral dodge exits the committed sector"), Enemy->IsTargetInsideAttack(Pawn));
            Stage = 3;
            return false;
        case 3:
            Test->TestTrue(TEXT("Wind-up aim stays locked when the target moves"),
                Enemy->GetCommittedDirection().Equals(LockedDirection, 0.001f));
            if (Enemy->GetStateName() == TEXT("Windup")) return false;
            Test->TestEqual(TEXT("Leaving the telegraph avoids the actual impact"), HP->Health, HP->MaxHealth);
            Test->TestEqual(TEXT("A missed attack still enters recovery"), Enemy->GetStateName(), FString(TEXT("Recovery")));
            PlacePlayer(Pawn, Home + Forward * 150.f);
            Stage = 4; StageAt = Now;
            return false;
        case 4:
            if (Now - StageAt < 1.1)
            {
                Test->TestEqual(TEXT("Enemy cannot chain another attack during its recovery"), Enemy->GetStateName(), FString(TEXT("Recovery")));
                Test->TestEqual(TEXT("Recovery causes no repeated impact damage"), HP->Health, HP->MaxHealth);
                return false;
            }
            if (Enemy->GetStateName() != TEXT("Windup")) return false;
            Test->TestTrue(TEXT("The next attack waits at least 1.2 seconds after impact"), Now - StageAt >= 1.19);
            BeginCloseAttack(Pawn, HP, Now);
            Stage = 5;
            return false;
        case 5:
            if (Enemy->GetStateName() == TEXT("Windup")) return false;
            Test->TestEqual(TEXT("Enemy attack deals actual 100-raw physical damage through player armor"),
                HP->Health, HP->MaxHealth - 100.f / 1.2f, 0.1f);
            BeginCloseAttack(Pawn, HP, Now);
            HP->SetDamageReduction(TEXT("Alphonse.E"), 0.5f, 5.f);
            Stage = 6;
            return false;
        case 6:
            if (Enemy->GetStateName() == TEXT("Windup")) return false;
            Test->TestEqual(TEXT("Alphonse E halves the actual enemy impact after armor"),
                HP->Health, HP->MaxHealth - 100.f / 1.2f * 0.5f, 0.1f);
            BeginCloseAttack(Pawn, HP, Now);
            HP->GrantInvulnerability(3.f);
            Stage = 7;
            return false;
        case 7:
            if (Now - StageAt < 0.9) return false;
            Test->TestEqual(TEXT("Spawn-style immunity prevents a pending enemy impact"), HP->Health, HP->MaxHealth);
            Test->TestTrue(TEXT("Enemy cancels an attack against an immune target"), Enemy->GetStateName() != TEXT("Windup"));
            BeginCloseAttack(Pawn, HP, Now);
            Enemy->CombatHealth->SetStun(5.f);
            Stage = 8;
            return false;
        case 8:
            if (Now - StageAt < 0.9) return false;
            Test->TestEqual(TEXT("Stunning the enemy interrupts its pending attack"), HP->Health, HP->MaxHealth);
            Test->TestTrue(TEXT("A stunned enemy cannot preserve its wind-up"), Enemy->GetStateName() != TEXT("Windup"));
            BeginCloseAttack(Pawn, HP, Now);
            Enemy->CombatHealth->ReceivePhysicalDamage(5000.f);
            Stage = 9;
            return false;
        case 9:
            if (Now - StageAt < 0.9) return false;
            Test->TestEqual(TEXT("Killing the enemy interrupts its pending attack"), HP->Health, HP->MaxHealth);
            Test->TestEqual(TEXT("Defeated enemies remain down until encounter reset"), Enemy->GetStateName(), FString(TEXT("Down")));
            Enemy->ResetEncounter();
            Test->TestEqual(TEXT("Training reset revives a downed enemy"), Enemy->CombatHealth->Health, 900.f);
            Test->TestEqual(TEXT("Training reset clears encounter state"), Enemy->GetStateName(), FString(TEXT("Idle")));
            Test->TestTrue(TEXT("Training reset restores the encounter home"), Enemy->GetActorLocation().Equals(Home, 0.1f));
            HP->ResetHealth();
            PlacePlayer(Pawn, Home + Forward * 150.f);
            Wall = SpawnWall(World, Home + Forward * 75.f, Forward.Rotation());
            if (!Test->TestNotNull(TEXT("Real blocking test terrain spawns"), Wall.Get())) return true;
            Stage = 10; StageAt = Now;
            return false;
        case 10:
            if (Now - StageAt < 0.9) return false;
            Test->TestEqual(TEXT("An enemy does not acquire the player through a terrain wall"), Enemy->GetStateName(), FString(TEXT("Idle")));
            Test->TestEqual(TEXT("A nearby player behind terrain takes no damage"), HP->Health, HP->MaxHealth);
            Wall->Destroy();
            BeginCloseAttack(Pawn, HP, Now);
            Wall = SpawnWall(World, Home + Forward * 75.f, Forward.Rotation());
            Stage = 11;
            return false;
        case 11:
            if (Now - StageAt < 0.9) return false;
            Test->TestEqual(TEXT("Terrain introduced during wind-up prevents damage through the wall"), HP->Health, HP->MaxHealth);
            Wall->Destroy();
            HP->ResetHealth();
            Enemy->ResetEncounter();
            Enemy->SetActorLocation(Home + Forward * 600.f);
            Enemy->CombatHealth->Health = 300.f;
            PlacePlayer(Pawn, Home + Forward * 750.f);
            Enemy->Tick(0.01f);
            Test->TestEqual(TEXT("Leash fixture first engages the nearby player"), Enemy->GetStateName(), FString(TEXT("Windup")));
            PlacePlayer(Pawn, Home + Forward * 1900.f);
            Enemy->Tick(0.01f);
            Test->TestEqual(TEXT("Leaving the home leash starts a return"), Enemy->GetStateName(), FString(TEXT("Returning")));
            Test->TestTrue(TEXT("Disengagement grants damage immunity on the return transition itself"), Enemy->CombatHealth->IsInvulnerable());
            Test->TestTrue(TEXT("Disengagement grants debuff immunity on the return transition itself"), Enemy->CombatHealth->IsDebuffImmune());
            Test->TestEqual(TEXT("An attack immediately after return begins cannot chip the escaping enemy"),
                Enemy->CombatHealth->ReceivePhysicalDamage(90.f), 0.f);
            Enemy->CombatHealth->SetStun(5.f);
            Test->TestFalse(TEXT("Crowd control immediately after return begins cannot trap the escaping enemy"), Enemy->CombatHealth->IsStunned());
            Test->TestEqual(TEXT("Returning does not immediately restore health before reaching home"), Enemy->CombatHealth->Health, 300.f);
            Stage = 12; StageAt = Now;
            return false;
        case 12:
            if (Enemy->GetStateName() != TEXT("Idle")) return false;
            Test->TestTrue(TEXT("Leashed enemy actually paths back to its original home"), FVector::Dist2D(Enemy->GetActorLocation(), Home) <= 25.1f);
            Test->TestEqual(TEXT("Reaching home restores the enemy's encounter health"), Enemy->CombatHealth->Health, 900.f);
            Test->TestEqual(TEXT("Enemy returning home does not damage a disengaged player"), HP->Health, HP->MaxHealth);
            PlacePlayer(Pawn, Home + Forward * 1000.f);
            Enemy->Tick(0.01f);
            Test->TestEqual(TEXT("A passive enemy does not acquire a player beyond its 800cm aggro radius"), Enemy->GetStateName(), FString(TEXT("Idle")));
            Test->TestEqual(TEXT("Player damage outside passive aggro still applies through enemy armor"),
                Enemy->CombatHealth->ReceivePhysicalDamage(90.f), 75.f);
            Enemy->Tick(0.01f);
            Test->TestEqual(TEXT("Damaging the enemy within its home leash causes retaliation outside passive aggro"), Enemy->GetStateName(), FString(TEXT("Approach")));
            Stage = 13; StageAt = Now;
            return false;
        case 13:
            if (FVector::Dist2D(Enemy->GetActorLocation(), Home) < 100.f) return false;
            Test->TestTrue(TEXT("A damaged enemy actually chases the distant attacking player"),
                FVector::DotProduct(Enemy->GetActorLocation() - Home, Forward) > 95.f);
            Test->TestEqual(TEXT("Retaliation does not silently heal the damage that triggered it"), Enemy->CombatHealth->Health, 825.f);
            Test->TestEqual(TEXT("The distant player's health is unchanged until the enemy reaches attack range"), HP->Health, HP->MaxHealth);
            Enemy->ResetEncounter();
            Test->TestEqual(TEXT("Explicit reset also restores health after distant retaliation"), Enemy->CombatHealth->Health, 900.f);
            Enemy->Destroy();
            HP->ResetHealth();
            return true;
        }
        return false;
    }

private:
    bool FindFixture(UWorld* World, ACharacter* Pawn)
    {
        UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
        if (!Nav) return false;
        FCollisionQueryParams Query(SCENE_QUERY_STAT(MistEnemyTestFixture), false, Pawn);
        for (int32 Angle = 0; Angle < 360; Angle += 30)
        {
            const FVector Direction = FVector::ForwardVector.RotateAngleAxis(Angle, FVector::UpVector);
            const FVector Lateral = Direction.RotateAngleAxis(90.f, FVector::UpVector);
            bool bClear = true;
            for (const FVector& Offset : {Direction * 600.f, Direction * 1900.f, Lateral * 220.f})
            {
                FNavLocation Point;
                const FVector Desired = Home + Offset;
                if (!Nav->ProjectPointToNavigation(Desired, Point, FVector(80.f, 80.f, 300.f))
                    || FVector::Dist2D(Desired, Point.Location) > 25.f
                    || FMath::Abs(Point.Location.Z - GroundZ) > 15.f
                    || World->LineTraceTestByChannel(Home, Desired, ECC_Visibility, Query))
                { bClear = false; break; }
            }
            if (bClear) { Forward = Direction; Side = Lateral; return true; }
        }
        return false;
    }

    void BeginCloseAttack(ACharacter* Pawn, UMistCombatHealth* HP, double Now)
    {
        Enemy->ResetEncounter();
        HP->ResetHealth();
        PlacePlayer(Pawn, Home + Forward * 150.f);
        // Idle acquisition and approach-to-wind-up are separate state updates.
        for (int32 Attempt = 0; Attempt < 3 && Enemy->GetStateName() != TEXT("Windup"); ++Attempt)
            Enemy->Tick(0.01f);
        Test->TestEqual(TEXT("A visible target in melee range starts the real telegraphed attack"),
            Enemy->GetStateName(), FString(TEXT("Windup")));
        Test->TestEqual(TEXT("Beginning a swing does not deal immediate damage"), HP->Health, HP->MaxHealth);
        StageAt = Now;
    }

    void PlacePlayer(ACharacter* Pawn, const FVector& Position)
    {
        Pawn->GetCharacterMovement()->StopMovementImmediately();
        const FVector GroundedPosition(Position.X, Position.Y, GroundZ + PlayerHalfHeight + 2.f);
        Pawn->SetActorLocation(GroundedPosition, false, nullptr, ETeleportType::TeleportPhysics);
    }

    static AActor* SpawnWall(UWorld* World, const FVector& Position, const FRotator& Rotation)
    {
        AActor* Actor = World->SpawnActor<AActor>();
        if (!Actor) return nullptr;
        UBoxComponent* Box = NewObject<UBoxComponent>(Actor);
        Actor->SetRootComponent(Box);
        Box->SetBoxExtent(FVector(10.f, 65.f, 180.f));
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
    double Started = -1.0, SettledAt = -1.0, StageAt = 0.0, LastTickAt = 0.0;
    float PlayerHalfHeight = 0.f, EnemyHalfHeight = 0.f;
    double GroundZ = 0.0;
    FVector Home, Forward, Side, LockedDirection;
    TWeakObjectPtr<AMistArenaEnemy> Enemy;
    TWeakObjectPtr<AActor> Wall;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMistArenaEnemyAITest, "Mistworld.Combat.EnemyAI",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FMistArenaEnemyAITest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(TEXT("/Game/Mistworld/Maps/Lvl_CombatTest")));
    ADD_LATENT_AUTOMATION_COMMAND(FMistWaitForEnemyTestNavigation(this));
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(FMistVerifyArenaEnemy(this));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    return true;
}

#endif
