#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Tests/AutomationEditorCommon.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/WorldSettings.h"
#include "Components/CapsuleComponent.h"
#include "CollisionQueryParams.h"
#include "CollisionShape.h"
#include "HAL/PlatformTime.h"
#include "Kismet/GameplayStatics.h"
#include "NavigationSystem.h"
#include "NavigationPath.h"
#include "MistCombatHUD.h"
#include "MistCombatHealth.h"
#include "MistAbyssShockwave.h"
#include "MistAbyssTrail.h"
#include "MistCryptGolem.h"
#include "MistCryptGuardian.h"
#include "MistCryptSession.h"
#include "MistProgressionComponent.h"
#include "MistReturningSword.h"
#include "MistTrainingDummy.h"
#include "MistworldPlayerController.h"
#include "MistWorldLabel.h"
#include "Components/TextRenderComponent.h"

struct FMistCryptSliceTestAccess
{
    static void CommitQ(AMistworldPlayerController* PC)
    { PC->CommitAbility(EKeys::Q, FVector::ForwardVector); }
    static void Auto(AMistworldPlayerController* PC, AActor* Target)
    { PC->RequestBasicAttack(Target); PC->BeginBasicAttack(); }
    static void Cancel(AMistworldPlayerController* PC) { PC->CancelCombatInput(); }
    static bool MovementPressAccepted(AMistworldPlayerController* PC)
    { PC->OnInputStarted(); return PC->bDestinationInputAccepted; }
    static void SeedCooldowns(AMistworldPlayerController* PC, const TArray<float>& Seconds)
    {
        const double Now = PC->GetWorld()->GetTimeSeconds();
        PC->QReadyAt = Now + Seconds[0]; PC->WReadyAt = Now + Seconds[1];
        PC->EReadyAt = Now + Seconds[2]; PC->RReadyAt = Now + Seconds[3];
        PC->FlashReadyAt = Now + Seconds[4]; PC->AutoReadyAt = Now + Seconds[5];
    }
    static TArray<float> Cooldowns(AMistworldPlayerController* PC)
    {
        const double Now = PC->GetWorld()->GetTimeSeconds();
        TArray<float> Result;
        for (double Deadline : {PC->QReadyAt, PC->WReadyAt, PC->EReadyAt, PC->RReadyAt, PC->FlashReadyAt, PC->AutoReadyAt})
            Result.Add(static_cast<float>(FMath::Max(0.0, Deadline - Now)));
        return Result;
    }
};

namespace
{
constexpr const TCHAR* CryptMap = TEXT("/Game/Mistworld/Maps/Lvl_CryptSlice");
const FName CryptOwnerTag(TEXT("Mistworld_CryptSlice_Generated_v1"));
const FName EntranceID(TEXT("Crypt.Entrance"));
const FName RewardID(TEXT("Crypt.Reward"));
const FVector Entrance(-1500.f, 0.f, 98.15f);
const FVector Reward(2000.f, 1100.f, 98.15f);

struct FCryptJourney
{
    FString Slot = TEXT("Mistworld.Test.CryptSlice.") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    TArray<float> SavedCooldowns = {18.f, 24.f, 36.f, 120.f, 60.f, 10.f};
    FDateTime SavedUtc;
    TWeakObjectPtr<UWorld> FirstWorld;
    TWeakObjectPtr<AMistworldPlayerController> FirstController;
    bool bFirstVisitFinished = false;
};

AMistCryptSession* FindSession(UWorld* World)
{
    if (World)
        for (TActorIterator<AMistCryptSession> It(World); It; ++It) return *It;
    return nullptr;
}

bool TeleportHero(ACharacter* Hero, AMistworldPlayerController* PC, const FVector& Location)
{
    FMistCryptSliceTestAccess::Cancel(PC);
    Hero->GetCharacterMovement()->StopMovementImmediately();
    return Hero->TeleportTo(Location, FRotator::ZeroRotator, false, false);
}

int32 CountOwnedAttemptEffects(UWorld* World, APawn* Hero)
{
    int32 Result = 0;
    for (TActorIterator<AActor> It(World); It; ++It)
        if (!It->IsActorBeingDestroyed() && It->GetOwner() == Hero
            && (It->IsA<AMistReturningSword>() || It->IsA<AMistAbyssShockwave>() || It->IsA<AMistAbyssTrail>())) ++Result;
    return Result;
}

class FCryptMapNavigation final : public IAutomationLatentCommand
{
public:
    explicit FCryptMapNavigation(FAutomationTestBase* InTest) : Test(InTest) {}
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
                FNavLocation Point;
                if (Nav->ProjectPointToNavigation(Entrance, Point, FVector(150.f, 150.f, 400.f)))
                {
                    Verify(World, Nav);
                    return true;
                }
            }
        }
        if (FPlatformTime::Seconds() - Started > 30.0)
        { Test->AddError(TEXT("Crypt slice navigation failed to initialize within 30 seconds.")); return true; }
        return false;
    }
private:
    FAutomationTestBase* Test;
    double Started = -1.0;
    bool bBuilt = false;

    void Verify(UWorld* World, UNavigationSystemV1* Nav)
    {
        Test->TestTrue(TEXT("Generated crypt map carries its ownership marker"),
            World->GetWorldSettings()->ActorHasTag(CryptOwnerTag));
        AActor* Floor = nullptr;
        APlayerStart* Start = nullptr;
        int32 Sessions = 0, Golems = 0;
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            if (It->GetActorLabel() == TEXT("Crypt_Floor")) Floor = *It;
            if (APlayerStart* PlayerStart = Cast<APlayerStart>(*It)) Start = PlayerStart;
            if (It->IsA<AMistCryptSession>()) ++Sessions;
            if (It->IsA<AMistCryptGolem>()) ++Golems;
        }
        Test->TestEqual(TEXT("Map contains one native crypt session"), Sessions, 1);
        Test->TestEqual(TEXT("Map contains one native crypt golem"), Golems, 1);
        int32 FloatingInstructions = 0;
        for (TActorIterator<AMistWorldLabel> It(World); It; ++It)
        {
            if (!It->ActorHasTag(CryptOwnerTag)) continue;
            ++FloatingInstructions;
            Test->TestTrue(TEXT("Crypt instruction text floats clear of the floor"), It->GetActorLocation().Z >= 200.f);
            Test->TestEqual(TEXT("Floating instructions are centered"), It->GetTextRender()->HorizontalAlignment.GetValue(), EHTA_Center);
        }
        Test->TestEqual(TEXT("All three crypt instruction signs use camera-facing labels"), FloatingInstructions, 3);
        if (Test->TestNotNull(TEXT("Dedicated crypt floor exists"), Floor))
        {
            FVector Origin, Extent;
            Floor->GetActorBounds(false, Origin, Extent);
            Test->TestEqual(TEXT("Floor upper surface is exactly Z zero"), Origin.Z + Extent.Z, 0.0, 0.1);
            Test->TestEqual(TEXT("Floor west extent"), Origin.X - Extent.X, -1950.0, 0.1);
            Test->TestEqual(TEXT("Floor east extent"), Origin.X + Extent.X, 2500.0, 0.1);
            Test->TestEqual(TEXT("Floor north extent"), Origin.Y + Extent.Y, 1600.0, 0.1);
        }
        if (Test->TestNotNull(TEXT("Entrance player start exists"), Start))
            Test->TestTrue(TEXT("Player start is centered on the safe checkpoint"), Start->GetActorLocation().Equals(Entrance, 1.f));

        struct FRoute { const TCHAR* Name; FVector From; FVector To; };
        const FRoute Routes[] = {
            {TEXT("entrance through portal"), FVector(-1500.f, 0.f, 0.f), FVector(-500.f, 0.f, 0.f)},
            {TEXT("portal to golem"), FVector(-500.f, 0.f, 0.f), FVector(800.f, 0.f, 0.f)},
            {TEXT("golem to reward checkpoint"), FVector(800.f, 0.f, 0.f), FVector(2000.f, 1100.f, 0.f)},
            {TEXT("golem to bait point outside protection"), FVector(800.f, 0.f, 0.f), FVector(-200.f, 900.f, 0.f)},
            {TEXT("open south charge strip"), FVector(800.f, 0.f, 0.f), FVector(800.f, -1100.f, 0.f)}
        };
        bool bAllRoutesValid = true;
        for (const FRoute& Route : Routes)
        {
            UNavigationPath* Path = UNavigationSystemV1::FindPathToLocationSynchronously(World, Route.From, Route.To);
            const bool bValid = Path && Path->IsValid() && !Path->IsPartial() && Path->PathPoints.Num() >= 2;
            Test->TestTrue(FString(TEXT("Real map navigation connects ")) + Route.Name, bValid);
            bAllRoutesValid &= bValid;
        }
        for (const FVector& Location : {Entrance, Reward, FVector(-400.f, 0.f, 98.15f)})
        {
            FNavLocation Projected;
            const bool bProjected = Nav->ProjectPointToNavigation(Location, Projected, FVector(150.f, 150.f, 400.f));
            Test->TestTrue(TEXT("Playable checkpoint/entry projects onto real navigation"), bProjected);
            if (bProjected)
                Test->TestTrue(TEXT("Playable position lies within the production 15 cm navigation tolerance"),
                    FVector::DistSquared2D(Location, Projected.Location) <= FMath::Square(15.f));
        }
        // This is the script-owned generated level. Persisting its verified
        // native rebuild supplies navigation to normal future editor startup.
        if (bAllRoutesValid && World->GetWorldSettings()->ActorHasTag(CryptOwnerTag))
            Test->TestTrue(TEXT("Verified crypt navigation saves into the generated map"),
                FEditorFileUtils::SaveLevel(World->PersistentLevel));
    }
};

class FCryptFirstVisit final : public IAutomationLatentCommand
{
public:
    FCryptFirstVisit(FAutomationTestBase* InTest, TSharedRef<FCryptJourney> InJourney)
        : Test(InTest), Journey(InJourney) {}
    bool Update() override
    {
        if (Started < 0.0) Started = FPlatformTime::Seconds();
        if (FPlatformTime::Seconds() - Started > 60.0)
        { Test->AddError(FString::Printf(TEXT("Crypt end-to-end visit timed out at stage %d."), Stage)); return true; }
        UWorld* World = GEditor ? GEditor->PlayWorld : nullptr;
        AMistworldPlayerController* PC = World ? Cast<AMistworldPlayerController>(World->GetFirstPlayerController()) : nullptr;
        ACharacter* Hero = PC ? Cast<ACharacter>(PC->GetPawn()) : nullptr;
        UMistCombatHealth* Health = Hero ? Hero->FindComponentByClass<UMistCombatHealth>() : nullptr;
        AMistCryptSession* Session = FindSession(World);
        UMistProgressionComponent* Progress = PC ? PC->GetProgression() : nullptr;
        if (!World || !World->HasBegunPlay() || !PC || !Hero || !Health || !Session || !Progress) return false;
        AMistCryptGolem* Golem = Session->GetGolem();
        if (!Test->TestNotNull(TEXT("Session resolves its real golem"), Golem)) return true;
        const double Now = World->GetTimeSeconds();
        switch (Stage)
        {
        case 0:
            Journey->FirstWorld = World;
            Journey->FirstController = PC;
            Test->TestTrue(TEXT("Crypt starts with the combat HUD"), PC->GetHUD() && PC->GetHUD()->IsA<AMistCombatHUD>());
            Test->TestEqual(TEXT("New journey starts at entrance checkpoint"), PC->GetActiveCheckpointID(), EntranceID);
            Test->TestTrue(TEXT("New hero starts at the entrance"), FVector::Dist2D(Hero->GetActorLocation(), Entrance) < 20.f);
            {
                FHitResult Ground;
                FCollisionQueryParams Query(SCENE_QUERY_STAT(MistCryptSpawnFloor), false, Hero);
                const bool bGrounded = World->LineTraceSingleByChannel(Ground,
                    Hero->GetActorLocation(), Hero->GetActorLocation() - FVector(0.f, 0.f, 250.f), ECC_Visibility, Query);
                Test->TestTrue(TEXT("Actual spawned capsule has the crypt floor below it"), bGrounded);
                if (bGrounded)
                    Test->TestTrue(TEXT("Actual capsule feet sit safely just above the flat floor"),
                        FMath::Abs(Hero->GetActorLocation().Z - Hero->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() - Ground.ImpactPoint.Z) < 4.f);
                Test->TestFalse(TEXT("Actual spawn capsule is clear of blocking terrain"),
                    World->OverlapBlockingTestByChannel(Hero->GetActorLocation(), FQuat::Identity, ECC_Pawn,
                        FCollisionShape::MakeCapsule(Hero->GetCapsuleComponent()->GetScaledCapsuleRadius(),
                            Hero->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()), Query));
            }
            Test->TestTrue(TEXT("Initial checkpoint grants spawn protection"), Health->IsSpawnProtected());
            Test->TestFalse(TEXT("Safe foyer initially leaves portal open"), Session->IsGateClosed());
            Test->TestEqual(TEXT("Dormant encounter does not spawn guardians in the foyer"), Golem->GetRemainingGuardians(), 0);
            Test->TestFalse(TEXT("Fresh isolated save grants no reward"), Progress->HasReward());
            Test->TestFalse(TEXT("Cannot equip an unearned item"), PC->TryEquipCryptReward(true));
            Test->TestTrue(TEXT("Prototype Abyss unlock works at the safe checkpoint"), PC->SetAbyssUnlocked(true));
            Test->TestTrue(TEXT("Checkpoint equips independent Abyss Q"), PC->TryEquipAbility(EKeys::Q, true));
            Test->TestTrue(TEXT("Checkpoint equips independent Abyss E"), PC->TryEquipAbility(EKeys::E, true));
            Test->TestTrue(TEXT("Mixed loadout can be saved at its checkpoint"), PC->SaveCryptProgression());
            if (!Test->TestTrue(TEXT("Actual hero capsule safely enters the crypt"), TeleportHero(Hero, PC, FVector(-400.f, 0.f, 98.15f)))) return true;
            Health->EndSpawnProtection();
            Stage = 1; return false;
        case 1:
            if (!Session->IsGateClosed() || !Golem->IsEncounterActive()) return false;
            Test->TestEqual(TEXT("Crossing the entrance creates exactly three guardians"), Golem->GetRemainingGuardians(), 3);
            Test->TestEqual(TEXT("Golem is protected while its three guardians live"), Golem->CombatHealth->ReceivePhysicalDamage(1000.f, Hero), 0.f, 0.001f);
            Test->TestFalse(TEXT("Checkpoint cannot change during the active encounter"),
                PC->TryActivateCryptCheckpoint(EntranceID, Session->GetCheckpointTransform(EntranceID)));
            // An encounter remains a loadout boundary even while no damage is
            // exchanged. Immunity keeps this controlled wait free of combat.
            Health->GrantInvulnerability(20.f);
            Stage = 11; StageAt = Now; return false;
        case 11:
            if (Now - StageAt < 11.0) return false;
            if (!Test->TestTrue(TEXT("Lock fixture can stand inside its active checkpoint circle"), TeleportHero(Hero, PC, Entrance))) return true;
            Test->TestTrue(TEXT("Loadout lock fixture is near its own checkpoint"), PC->IsNearCheckpoint());
            Test->TestEqual(TEXT("Eleven damage-free seconds have no residual combat timer"), Health->GetCombatLockRemaining(), 0.f, 0.001f);
            Test->TestTrue(TEXT("Encounter remains active beyond the combat timer"), Golem->IsEncounterActive());
            Test->TestTrue(TEXT("Encounter, rather than recent damage or distance, blocks loadout changes"),
                PC->GetLoadoutLockReason().Contains(TEXT("encounter")));
            Test->TestFalse(TEXT("A quiet active encounter still refuses a real kit change"), PC->TryEquipAbility(EKeys::Q, false));
            Test->TestTrue(TEXT("Refused active-encounter swap preserves equipped Q"), PC->IsAbyssEquipped(EKeys::Q));
            {
                const FString PriorStatus = Progress->GetSaveStatus();
                Test->TestFalse(TEXT("Active encounter refuses equipment interaction before inventory processing"), PC->TryEquipCryptReward(true));
                Test->TestEqual(TEXT("Encounter equipment lock never reaches the inventory/save transaction"), Progress->GetSaveStatus(), PriorStatus);
            }
            if (!Test->TestTrue(TEXT("Wipe fixture safely returns inside the encounter"), TeleportHero(Hero, PC, FVector(-400.f, 0.f, 98.15f)))) return true;
            {
                FActorSpawnParameters Params;
                Params.Owner = Hero;
                Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
                AMistReturningSword* Sword = World->SpawnActor<AMistReturningSword>(Hero->GetActorLocation(), FRotator::ZeroRotator, Params);
                AMistAbyssShockwave* Wave = World->SpawnActor<AMistAbyssShockwave>(Hero->GetActorLocation(), FRotator::ZeroRotator, Params);
                AMistAbyssTrail* Trail = World->SpawnActor<AMistAbyssTrail>(Hero->GetActorLocation(), FRotator::ZeroRotator, Params);
                if (!Test->TestNotNull(TEXT("Released sword cleanup fixture"), Sword)
                    || !Test->TestNotNull(TEXT("Released shockwave cleanup fixture"), Wave)
                    || !Test->TestNotNull(TEXT("Released trail cleanup fixture"), Trail)) return true;
                Sword->Initialize(Hero, FVector::ForwardVector);
                Wave->Initialize(Hero, FVector::ForwardVector, {});
                Trail->Initialize(Hero, false);
                Trail->AddSegment(FVector(200.f, 0.f, 98.15f), FVector(650.f, 0.f, 98.15f));
                Trail->FinishAddingSegments();
                // Freeze normal movement/expiry so this check proves immediate
                // wipe cleanup, rather than actors disappearing by themselves.
                for (AActor* Effect : {static_cast<AActor*>(Sword), static_cast<AActor*>(Wave), static_cast<AActor*>(Trail)})
                { Effect->SetActorTickEnabled(false); Effect->SetLifeSpan(0.f); }
                Test->TestEqual(TEXT("Wipe starts with all three real player-owned effect types"), CountOwnedAttemptEffects(World, Hero), 3);
            }
            Health->ClearInvulnerability();
            Health->EndSpawnProtection();
            Health->ReceivePhysicalDamage(100000.f, Golem);
            Test->TestTrue(TEXT("Crypt defeat enters the existing respawn lifecycle"), PC->IsRespawning());
            Test->TestEqual(TEXT("Death immediately removes sword, shockwave, and trail from the failed attempt"), CountOwnedAttemptEffects(World, Hero), 0);
            Stage = 2; return false;
        case 2:
            if (PC->IsRespawning()) return false;
            Test->TestTrue(TEXT("Defeat returns the same hero to the entrance checkpoint"), FVector::Dist2D(Hero->GetActorLocation(), Entrance) < 20.f);
            Test->TestTrue(TEXT("Crypt respawn grants three seconds of protection"), Health->GetSpawnProtectionRemaining() > 2.5f);
            Test->TestFalse(TEXT("A wipe opens the entrance gate"), Session->IsGateClosed());
            Test->TestEqual(TEXT("A wipe removes the failed guardian wave"), Golem->GetRemainingGuardians(), 0);
            Test->TestEqual(TEXT("Checkpoint revive cannot carry effects from the old attempt"), CountOwnedAttemptEffects(World, Hero), 0);
            Test->TestTrue(TEXT("Death retains equipped Abyss Q"), PC->IsAbyssEquipped(EKeys::Q));
            Test->TestTrue(TEXT("Death retains equipped Abyss E"), PC->IsAbyssEquipped(EKeys::E));
            if (!Test->TestTrue(TEXT("Hero can safely re-enter after a wipe"), TeleportHero(Hero, PC, FVector(-400.f, 0.f, 98.15f)))) return true;
            Health->EndSpawnProtection();
            Stage = 3; return false;
        case 3:
            if (!Golem->IsEncounterActive() || Golem->GetRemainingGuardians() != 3) return false;
            Test->TestEqual(TEXT("Fresh guardian wave has no stale player-owned projectiles or trail"), CountOwnedAttemptEffects(World, Hero), 0);
            for (const AMistCryptGuardian* Guardian : Golem->GetGuardians())
                Test->TestEqual(TEXT("New-attempt guardians start with full elemental health"), Guardian->CombatHealth->Health, Guardian->CombatHealth->MaxHealth, 0.01f);
            // The final death removes the golem's owned array. Copy its handles
            // before delivering lethal damage so iteration survives that event.
            for (AMistCryptGuardian* Guardian : TArray<TObjectPtr<AMistCryptGuardian>>(Golem->GetGuardians()))
                if (IsValid(Guardian) && Guardian->CombatHealth->IsAlive())
                    Guardian->CombatHealth->ReceivePhysicalDamage(100000.f, Hero);
            Test->TestEqual(TEXT("Actual guardian deaths clear all three ward symbols"), Golem->GetRemainingGuardians(), 0);
            Test->TestTrue(TEXT("Three actual deaths open the vulnerability window"), Golem->IsVulnerable());
            Test->TestTrue(TEXT("Vulnerability starts at fifteen seconds"), Golem->GetVulnerabilityRemaining() > 14.8f);
            Golem->CombatHealth->ReceivePhysicalDamage(100000.f, Hero);
            if (!Test->TestTrue(TEXT("Actual boss damage completes the encounter"), Golem->IsEncounterComplete())) return true;
            Stage = 4; StageAt = Now; return false;
        case 4:
            if (!Progress->IsCryptCompleted() || Session->IsGateClosed()) return false;
            Test->TestTrue(TEXT("Completion opens the gate and records local progression"), Progress->IsCryptCompleted());
            if (!Test->TestTrue(TEXT("Reward checkpoint has a safe capsule destination"), TeleportHero(Hero, PC, Reward))) return true;
            Test->TestFalse(TEXT("Recent boss combat prevents immediate checkpoint switching"),
                PC->TryActivateCryptCheckpoint(RewardID, Session->GetCheckpointTransform(RewardID)));
            Stage = 5; return false;
        case 5:
            if (Health->GetCombatLockRemaining() > 0.f) return false;
            Test->TestTrue(TEXT("Reward interaction activates its checkpoint and claims loot"), Session->TryInteract());
            Test->TestEqual(TEXT("Claim selects the reward checkpoint"), PC->GetActiveCheckpointID(), RewardID);
            Test->TestTrue(TEXT("First reward is owned"), Progress->HasReward());
            Test->TestFalse(TEXT("Reward claim does not silently equip"), Progress->IsRewardEquipped());
            Test->TestFalse(TEXT("Second reward claim cannot duplicate the item"), Progress->ClaimCryptReward());
            PC->ToggleInventoryMenu();
            Test->TestTrue(TEXT("Earned inventory can open at its checkpoint"), PC->IsInventoryMenuOpen());
            {
                AMistCombatHUD* HUD = Cast<AMistCombatHUD>(PC->GetHUD());
                if (!Test->TestNotNull(TEXT("Inventory callbacks have the combat HUD"), HUD)) return true;
                HUD->NotifyHitBoxClick(TEXT("Mist.Equip"));
                Test->TestTrue(TEXT("Actual inventory Equip click commits the earned weapon"), Progress->IsRewardEquipped());
                HUD->NotifyHitBoxClick(TEXT("Mist.Close"));
                Test->TestTrue(TEXT("Inventory stays open while its Close press is processed"), PC->IsInventoryMenuOpen());
                Test->TestFalse(TEXT("Close press cannot leak into a map movement order"), FMistCryptSliceTestAccess::MovementPressAccepted(PC));
                HUD->NotifyHitBoxRelease(TEXT("Mist.Close"));
            }
            Test->TestEqual(TEXT("Reward grants exactly five percent ability damage"), Progress->GetAbilityDamageMultiplier(), 1.05f, 0.0001f);
            Test->TestFalse(TEXT("Actual Close release dismisses inventory before combat resumes"), PC->IsInventoryMenuOpen());
            DirectTarget = SpawnTarget(World, Hero->GetActorLocation() + FVector(180.f, 0.f, 0.f));
            WaveTarget = SpawnTarget(World, Hero->GetActorLocation() + FVector(400.f, 0.f, 0.f));
            if (!Test->TestNotNull(TEXT("Direct reward-damage fixture exists"), DirectTarget.Get())
                || !Test->TestNotNull(TEXT("Shockwave reward-damage fixture exists"), WaveTarget.Get())) return true;
            FMistCryptSliceTestAccess::CommitQ(PC);
            Test->TestTrue(TEXT("Equipped Abyss Q begins its real cast"), PC->IsCombatBusy());
            Stage = 6; StageAt = Now; return false;
        case 6:
            if (Now - StageAt < 0.85 || PC->IsCombatBusy()) return false;
            Test->TestEqual(TEXT("Equipment increases Q direct damage to 157.5"), 1000.f - DirectTarget->CombatHealth->Health, 157.5f, 0.1f);
            Test->TestEqual(TEXT("Equipment increases the released Q shockwave to 84"), 1000.f - WaveTarget->CombatHealth->Health, 84.f, 0.1f);
            DirectTarget->CombatHealth->ResetHealth();
            FMistCryptSliceTestAccess::Auto(PC, DirectTarget.Get());
            Test->TestTrue(TEXT("Fixture begins the actual auto attack windup"), PC->IsCombatBusy());
            Stage = 7; StageAt = Now; return false;
        case 7:
            if (Now - StageAt < 0.22 || PC->IsCombatBusy()) return false;
            FMistCryptSliceTestAccess::Cancel(PC);
            Test->TestEqual(TEXT("Ability-only equipment leaves the auto attack at raw 90"), 1000.f - DirectTarget->CombatHealth->Health, 90.f, 0.1f);
            DirectTarget->Destroy(); WaveTarget->Destroy();
            Stage = 8; return false;
        case 8:
            if (Health->GetCombatLockRemaining() > 0.f || !PC->GetLoadoutLockReason().IsEmpty()) return false;
            FMistCryptSliceTestAccess::SeedCooldowns(PC, Journey->SavedCooldowns);
            if (!Test->TestTrue(TEXT("Checkpoint writes the completed equipped mixed-kit journey"), PC->SaveCryptProgression())) return true;
            Journey->SavedUtc = FDateTime::UtcNow();
            Journey->bFirstVisitFinished = true;
            return true;
        }
        return true;
    }
private:
    FAutomationTestBase* Test;
    TSharedRef<FCryptJourney> Journey;
    int32 Stage = 0;
    double Started = -1.0, StageAt = 0.0;
    TWeakObjectPtr<AMistTrainingDummy> DirectTarget, WaveTarget;
    static AMistTrainingDummy* SpawnTarget(UWorld* World, const FVector& Location)
    {
        FActorSpawnParameters Params;
        Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        AMistTrainingDummy* Target = World->SpawnActor<AMistTrainingDummy>(Location, FRotator::ZeroRotator, Params);
        if (Target)
        {
            Target->CombatHealth->MaxHealth = 1000.f;
            Target->CombatHealth->Armor = Target->CombatHealth->MagicResistance = 0.f;
            Target->CombatHealth->ResetHealth();
        }
        return Target;
    }
};

class FCryptReloadVisit final : public IAutomationLatentCommand
{
public:
    FCryptReloadVisit(FAutomationTestBase* InTest, TSharedRef<FCryptJourney> InJourney)
        : Test(InTest), Journey(InJourney) {}
    bool Update() override
    {
        if (!Journey->bFirstVisitFinished) return true;
        if (Started > 0.0 && FPlatformTime::Seconds() - Started > 30.0)
        { Test->AddError(FString::Printf(TEXT("Restored crypt/replay session timed out at stage %d."), ReplayStage)); return true; }
        if (Started < 0.0) Started = FPlatformTime::Seconds();
        UWorld* World = GEditor ? GEditor->PlayWorld : nullptr;
        AMistworldPlayerController* PC = World ? Cast<AMistworldPlayerController>(World->GetFirstPlayerController()) : nullptr;
        APawn* Hero = PC ? PC->GetPawn() : nullptr;
        UMistProgressionComponent* Progress = PC ? PC->GetProgression() : nullptr;
        AMistCryptSession* Session = FindSession(World);
        if (ReplayStage > 0 && World && World->HasBegunPlay() && Hero && Progress && Session && Session->GetGolem())
        {
            AMistCryptGolem* Boss = Session->GetGolem();
            UMistCombatHealth* Health = Hero->FindComponentByClass<UMistCombatHealth>();
            if (!Boss->IsEncounterActive() || Boss->GetRemainingGuardians() != 3) return false;
            Test->TestTrue(TEXT("Explicit replay starts another real guarded encounter"), Session->IsGateClosed());
            Test->TestTrue(TEXT("Replaying does not remove saved completion"), Progress->IsCryptCompleted());
            Test->TestTrue(TEXT("Replaying does not remove the earned equipped weapon"), Progress->HasReward() && Progress->IsRewardEquipped());
            for (const AMistCryptGuardian* Guardian : Boss->GetGuardians())
                Test->TestEqual(TEXT("Replay guardians have their own full elemental health"), Guardian->CombatHealth->Health, Guardian->CombatHealth->MaxHealth, 0.01f);
            if (ReplayStage == 1)
            {
                Test->TestFalse(TEXT("B cannot reset an encounter away from a checkpoint"), PC->TryReplayCryptEncounter());
                TArray<TWeakObjectPtr<AMistCryptGuardian>> OldGuardians;
                for (AMistCryptGuardian* Guardian : Boss->GetGuardians()) OldGuardians.Add(Guardian);
                FActorSpawnParameters Params;
                Params.Owner = Hero;
                Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
                AMistAbyssTrail* Trail = World->SpawnActor<AMistAbyssTrail>(Hero->GetActorLocation(), FRotator::ZeroRotator, Params);
                if (!Test->TestNotNull(TEXT("Replay cleanup has an actual player-owned trail"), Trail)) return true;
                Trail->SetActorTickEnabled(false);
                Trail->SetLifeSpan(0.f);
                if (!Test->TestTrue(TEXT("Replay reset fixture reaches the entrance checkpoint"),
                    TeleportHero(Cast<ACharacter>(Hero), PC, Entrance))) return true;
                Health->MarkCombatActivity(10.f);
                Test->TestFalse(TEXT("B refuses to bypass recent combat even at a checkpoint"), PC->TryReplayCryptEncounter());
                Health->ClearCombatLock();
                Test->TestTrue(TEXT("B can reset the active attempt at a safe checkpoint"), PC->TryReplayCryptEncounter());
                Test->TestEqual(TEXT("Active replay reset removes released player effects"), CountOwnedAttemptEffects(World, Hero), 0);
                for (const TWeakObjectPtr<AMistCryptGuardian>& Old : OldGuardians)
                    Test->TestTrue(TEXT("Active replay reset destroys every old guardian"), !Old.IsValid() || Old->IsActorBeingDestroyed());
                Test->TestEqual(TEXT("Reset boss returns to dormant with full health"), Boss->GetStateName(), FString(TEXT("Dormant")));
                Test->TestEqual(TEXT("Reset boss restores health"), Boss->CombatHealth->Health, AMistCryptGolem::MaximumHealth, 0.01f);
                Test->TestFalse(TEXT("Reset boss opens the gate"), Session->IsGateClosed());
                Test->TestTrue(TEXT("Replay reset grants three seconds of spawn protection"), Health->GetSpawnProtectionRemaining() > 2.5f);
                if (!Test->TestTrue(TEXT("Another replay attempt can safely enter the chamber"),
                    TeleportHero(Cast<ACharacter>(Hero), PC, FVector(-400.f, 0.f, 98.15f)))) return true;
                ReplayStage = 2;
                return false;
            }
            Health->EndSpawnProtection();
            for (AMistCryptGuardian* Guardian : TArray<TObjectPtr<AMistCryptGuardian>>(Boss->GetGuardians()))
                Guardian->CombatHealth->ReceivePhysicalDamage(100000.f, Hero);
            Boss->CombatHealth->ReceivePhysicalDamage(100000.f, Hero);
            Test->TestTrue(TEXT("An explicit replay can be completed again"), Boss->IsEncounterComplete());
            Test->TestTrue(TEXT("Repeat completion preserves owned and equipped loot"), Progress->HasReward() && Progress->IsRewardEquipped());
            Test->TestFalse(TEXT("Repeat victory cannot duplicate the equipment reward"), Progress->ClaimCryptReward());
            Session->ResetEncounter();
            Test->TestTrue(TEXT("Ordinary reset after replay victory retains the clear"), Boss->IsEncounterComplete());
            return true;
        }
        const bool bFreshWorld = World && TWeakObjectPtr<UWorld>(World) != Journey->FirstWorld;
        const bool bFreshController = PC && TWeakObjectPtr<AMistworldPlayerController>(PC) != Journey->FirstController;
        // Start/End PIE commands only enqueue editor operations. A true reload
        // must use distinct UObject identities, and session restoration runs on
        // its first gameplay tick after every actor has received BeginPlay.
        if (bFreshWorld && World->HasBegunPlay() && bFreshController && Hero && Progress
            && Session && Session->GetGolem() && Session->GetGolem()->IsEncounterComplete())
        {
            Test->TestTrue(TEXT("Reload uses a newly created PIE world"), bFreshWorld);
            Test->TestTrue(TEXT("Reload uses a newly created combat controller"), bFreshController);
            Test->TestEqual(TEXT("Restart restores reward checkpoint ID"), PC->GetActiveCheckpointID(), RewardID);
            Test->TestTrue(TEXT("Restart actually places the hero at the reward checkpoint"), FVector::Dist2D(Hero->GetActorLocation(), Reward) < 20.f);
            Test->TestTrue(TEXT("Restart restores earned completion"), Progress->IsCryptCompleted());
            Test->TestTrue(TEXT("Restart restores one owned reward"), Progress->HasReward());
            Test->TestTrue(TEXT("Restart restores equipped reward"), Progress->IsRewardEquipped());
            Test->TestTrue(TEXT("Restart restores Abyss unlock"), PC->IsAbyssUnlocked());
            Test->TestTrue(TEXT("Restart restores mixed Abyss Q"), PC->IsAbyssEquipped(EKeys::Q));
            Test->TestFalse(TEXT("Restart preserves original W"), PC->IsAbyssEquipped(EKeys::W));
            Test->TestTrue(TEXT("Restart restores mixed Abyss E"), PC->IsAbyssEquipped(EKeys::E));
            Test->TestFalse(TEXT("Restart preserves original R"), PC->IsAbyssEquipped(EKeys::R));
            Test->TestTrue(TEXT("Restart presents the already completed golem"), Session->GetGolem()->IsEncounterComplete());
            Test->TestEqual(TEXT("Restart does not spawn another reward encounter"), Session->GetGolem()->GetRemainingGuardians(), 0);
            Test->TestFalse(TEXT("Restart leaves the foyer gate open"), Session->IsGateClosed());
            Test->TestFalse(TEXT("Reloading cannot duplicate a claimed reward"), Progress->ClaimCryptReward());
            const UMistCombatHealth* Health = Hero->FindComponentByClass<UMistCombatHealth>();
            Test->TestTrue(FString::Printf(TEXT("Loaded checkpoint grants three seconds of spawn immunity (world %.3fs, remaining %.3fs)"),
                World->GetTimeSeconds(), Health ? Health->GetSpawnProtectionRemaining() : -1.f),
                Health && Health->IsSpawnProtected() && Health->GetSpawnProtectionRemaining() > 2.5f);
            const TArray<float> Restored = FMistCryptSliceTestAccess::Cooldowns(PC);
            const double Elapsed = (FDateTime::UtcNow() - Journey->SavedUtc).GetTotalSeconds();
            for (int32 Index = 0; Index < Restored.Num(); ++Index)
            {
                Test->TestTrue(FString::Printf(TEXT("Reload cannot increase cooldown slot %d"), Index),
                    Restored[Index] <= Journey->SavedCooldowns[Index] + 0.1f);
                Test->TestTrue(FString::Printf(TEXT("Reload retains elapsed cooldown slot %d instead of resetting it"), Index),
                    Restored[Index] >= FMath::Max(0.0, Journey->SavedCooldowns[Index] - Elapsed) - 2.0);
            }
            Test->TestTrue(TEXT("Saved-clear checkpoint advertises B replay"), Session->GetInteractionHint().Contains(TEXT("B:")));
            if (!Test->TestTrue(TEXT("B rearms a boss after loading an already completed save"), PC->TryReplayCryptEncounter())) return true;
            Test->TestEqual(TEXT("Explicit replay sends the hero to entrance"), PC->GetActiveCheckpointID(), EntranceID);
            Test->TestTrue(TEXT("Explicit replay positions the hero safely at entrance"), FVector::Dist2D(Hero->GetActorLocation(), Entrance) < 20.f);
            Test->TestEqual(TEXT("Explicit replay rearms the completed boss without an immediate wave"), Session->GetGolem()->GetStateName(), FString(TEXT("Dormant")));
            Test->TestFalse(TEXT("Explicit replay initially leaves the gate open"), Session->IsGateClosed());
            Test->TestEqual(TEXT("Explicit replay clears old ability cooldowns"), PC->GetCooldownRemaining(EKeys::Q), 0.f);
            Test->TestTrue(TEXT("Explicit replay preserves mixed Abyss Q"), PC->IsAbyssEquipped(EKeys::Q));
            if (!Test->TestTrue(TEXT("Explicit replay can enter the actual room"),
                TeleportHero(Cast<ACharacter>(Hero), PC, FVector(-400.f, 0.f, 98.15f)))) return true;
            ReplayStage = 1;
            return false;
        }
        if (FPlatformTime::Seconds() - Started > 30.0)
        { Test->AddError(TEXT("Restored crypt PIE session failed to initialize within 30 seconds.")); return true; }
        return false;
    }
private:
    FAutomationTestBase* Test;
    TSharedRef<FCryptJourney> Journey;
    double Started = -1.0;
    int32 ReplayStage = 0;
};

class FCryptAwaitPIEStopped final : public IAutomationLatentCommand
{
public:
    explicit FCryptAwaitPIEStopped(FAutomationTestBase* InTest) : Test(InTest) {}
    bool Update() override
    {
        if (Started < 0.0) Started = FPlatformTime::Seconds();
        if (!GEditor || !GEditor->PlayWorld) return true;
        if (FPlatformTime::Seconds() - Started > 30.0)
        { Test->AddError(TEXT("Requested crypt PIE teardown did not finish within 30 seconds.")); return true; }
        return false;
    }
private:
    FAutomationTestBase* Test;
    double Started = -1.0;
};

class FCryptCleanup final : public IAutomationLatentCommand
{
public:
    FCryptCleanup(FAutomationTestBase* InTest, TSharedRef<FCryptJourney> InJourney) : Test(InTest), Journey(InJourney) {}
    bool Update() override
    {
        if (GEditor && GEditor->PlayWorld)
        {
            if (Started < 0.0) Started = FPlatformTime::Seconds();
            if (FPlatformTime::Seconds() - Started <= 30.0) return false;
            Test->AddError(TEXT("Crypt cleanup retained its isolated save because PIE teardown is still active."));
            AMistworldPlayerController::AutomationSaveSlot.Empty();
            return true;
        }
        // EndPlay writes the controller snapshot. Delete only after that world
        // is fully gone, so teardown cannot recreate or overwrite the test slot.
        AMistworldPlayerController::AutomationSaveSlot.Empty();
        UGameplayStatics::DeleteGameInSlot(Journey->Slot, 0);
        return true;
    }
private:
    FAutomationTestBase* Test;
    TSharedRef<FCryptJourney> Journey;
    double Started = -1.0;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMistCryptPlayableSliceTest, "Mistworld.Crypt.PlayableSlice",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMistCryptPlayableSliceTest::RunTest(const FString& Parameters)
{
    TSharedRef<FCryptJourney> Journey = MakeShared<FCryptJourney>();
    // The override is set before the first controller is constructed. Both
    // visits use this unique slot; neither loads nor writes the player's save.
    AMistworldPlayerController::AutomationSaveSlot = Journey->Slot;
    ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(CryptMap));
    ADD_LATENT_AUTOMATION_COMMAND(FCryptMapNavigation(this));
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(FCryptFirstVisit(this, Journey));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    ADD_LATENT_AUTOMATION_COMMAND(FCryptAwaitPIEStopped(this));
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(FCryptReloadVisit(this, Journey));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    ADD_LATENT_AUTOMATION_COMMAND(FCryptAwaitPIEStopped(this));
    ADD_LATENT_AUTOMATION_COMMAND(FCryptCleanup(this, Journey));
    return true;
}

#endif
