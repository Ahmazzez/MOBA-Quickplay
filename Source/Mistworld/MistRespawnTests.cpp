#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Tests/AutomationEditorCommon.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/PlatformTime.h"
#include "InputKeyEventArgs.h"
#include "CollisionQueryParams.h"
#include "CollisionShape.h"
#include "TimerManager.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerStart.h"
#include "Components/CapsuleComponent.h"
#include "NavigationSystem.h"
#include "NavigationPath.h"
#include "Blueprint/AIBlueprintHelperLibrary.h"
#include "MistArenaEnemy.h"
#include "MistCombatHealth.h"
#include "MistReturningSword.h"
#include "MistTrainingDummy.h"
#include "MistworldPlayerController.h"

struct FMistRespawnTestAccess
{
    static void Reset(AMistworldPlayerController* PC) { PC->ResetCombatSandbox(); }
    static void Commit(AMistworldPlayerController* PC, FKey Key, const FVector& Direction) { PC->CommitAbility(Key, Direction); }
    static bool CanAim(AMistworldPlayerController* PC, FKey Key) { return PC->CanAimAbility(Key); }
    static void Cancel(AMistworldPlayerController* PC) { PC->CancelCombatInput(); }
    static bool Aiming(AMistworldPlayerController* PC) { return PC->ActiveAimKey.IsValid(); }
    static bool Dashing(AMistworldPlayerController* PC) { return PC->bDashing; }
    static AActor* Target(AMistworldPlayerController* PC) { return PC->AttackTarget.Get(); }
    static AMistReturningSword* Sword(AMistworldPlayerController* PC) { return PC->ActiveSword.Get(); }
    static FTransform SpawnTransform(AMistworldPlayerController* PC) { return PC->RespawnTransform; }
    static double RespawnDeadline(AMistworldPlayerController* PC) { return PC->RespawnAt; }
    static bool Revive(AMistworldPlayerController* PC) { return PC->RespawnHero(); }
    static bool HasCombatTimer(AMistworldPlayerController* PC) { return PC->GetWorldTimerManager().IsTimerActive(PC->CombatTimer); }
    static void Flash(AMistworldPlayerController* PC, const FVector& Direction, float Distance) { PC->TryFlash(Direction, Distance); }
    static bool Endpoint(AMistworldPlayerController* PC, const FVector& Direction, float Distance, FVector& End)
    { return PC->FindLegalMovementEndpoint(Direction, Distance, End); }
    static void Auto(AMistworldPlayerController* PC, AActor* Target)
    {
        PC->RequestBasicAttack(Target);
        PC->BeginBasicAttack();
    }
    static TArray<double> Deadlines(AMistworldPlayerController* PC)
    { return {PC->QReadyAt, PC->WReadyAt, PC->EReadyAt, PC->RReadyAt, PC->FlashReadyAt, PC->AutoReadyAt}; }
    static void Key(AMistworldPlayerController* PC, FKey Key, EInputEvent Event)
    {
        PC->InputKey(FInputKeyEventArgs(nullptr, INPUTDEVICEID_NONE, Key, Event,
            Event == IE_Pressed ? 1.f : 0.f, false, FPlatformTime::Cycles64()));
    }
    static AMistReturningSword* ReleaseSword(AMistworldPlayerController* PC)
    {
        // Control release timing only for the projectile/lifecycle interaction.
        // The ordinary 0.35-second R wind-up is covered by BaseKit.
        PC->GetWorldTimerManager().ClearTimer(PC->CombatTimer);
        PC->LaunchSword();
        AMistReturningSword* Result = PC->ActiveSword.Get();
        if (Result) Result->SetActorTickEnabled(false);
        return Result;
    }
};

namespace
{
class FMistWaitForRespawnNavigation final : public IAutomationLatentCommand
{
public:
    explicit FMistWaitForRespawnNavigation(FAutomationTestBase* InTest) : Test(InTest) {}
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
        { Test->AddError(TEXT("Respawn test cannot find combat arena navigation.")); return true; }
        return false;
    }
private:
    FAutomationTestBase* Test;
    double Started = -1.0;
    bool bBuilt = false;
};

class FMistVerifyRespawnLifecycle final : public IAutomationLatentCommand
{
public:
    explicit FMistVerifyRespawnLifecycle(FAutomationTestBase* InTest) : Test(InTest) {}
    bool Update() override
    {
        if (Started < 0.0) Started = FPlatformTime::Seconds();
        if (FPlatformTime::Seconds() - Started > 60.0)
        {
            Test->AddError(FString::Printf(TEXT("Respawn lifecycle timed out at stage %d; fixture %s, direction %s."),
                Stage, *FixtureOrigin.ToString(), *Direction.ToString()));
            return true;
        }
        UWorld* World = GEditor ? GEditor->PlayWorld : nullptr;
        AMistworldPlayerController* PC = World ? Cast<AMistworldPlayerController>(World->GetFirstPlayerController()) : nullptr;
        ACharacter* Hero = PC ? Cast<ACharacter>(PC->GetPawn()) : nullptr;
        UMistCombatHealth* Health = Hero ? Hero->FindComponentByClass<UMistCombatHealth>() : nullptr;
        if (!World || !World->HasBegunPlay() || !Hero || !Health) return false;
        const double Now = World->GetTimeSeconds();

        switch (Stage)
        {
        case 0:
            OriginalPawn = Hero;
            OriginalSpawn = FMistRespawnTestAccess::SpawnTransform(PC);
            if (!Test->TestTrue(TEXT("Initial possession grants protection before the first playable tick"), Health->IsSpawnProtected())) return true;
            Test->TestTrue(TEXT("Initial spawn protection has approximately three seconds remaining"), Health->GetSpawnProtectionRemaining() > 2.5f);
            VerifyProtected(Health, TEXT("Initial spawn"));
            for (TActorIterator<AMistArenaEnemy> It(World); It; ++It) FixtureOrigin = It->GetHomeLocation();
            for (TActorIterator<AMistTrainingDummy> It(World); It; ++It) It->Destroy();
            Stage = 1; StageAt = Now;
            return false;
        case 1:
            if (Health->IsSpawnProtected()) return false;
            Test->TestTrue(TEXT("Initial immunity expires after three seconds"), Now - StageAt >= 2.5);
            Test->TestFalse(TEXT("Expired initial protection stops granting damage immunity"), Health->IsInvulnerable());
            Test->TestFalse(TEXT("Expired initial protection stops granting crowd-control immunity"), Health->IsDebuffImmune());
            if (!MakeFixture(World, PC, Hero)) return true;
            FMistRespawnTestAccess::Reset(PC);
            Target = SpawnTarget(World, FixtureOrigin + Direction * 150.f);
            if (!Test->TestNotNull(TEXT("Lifecycle damage target spawns"), Target.Get())) return true;
            FMistRespawnTestAccess::Commit(PC, EKeys::E, Direction);
            FMistRespawnTestAccess::Auto(PC, Target.Get());
            Test->TestTrue(TEXT("Actual basic-attack wind-up begins"), PC->IsCombatBusy());
            Stage = 2; StageAt = Now;
            return false;
        case 2:
        {
            if (PC->IsCombatBusy() || Now - StageAt < 0.25) return false;
            Test->TestEqual(TEXT("Setup basic attack performs its timed impact"), Target->CombatHealth->Health, 1125.f, 0.1f);
            FMistRespawnTestAccess::Cancel(PC);
            Target->CombatHealth->ResetHealth();
            FMistRespawnTestAccess::Commit(PC, EKeys::Q, Direction);
            Test->TestTrue(TEXT("Q is in its real wind-up when lethal damage arrives"), PC->IsCombatBusy());
            SavedDeadlines = FMistRespawnTestAccess::Deadlines(PC);
            Health->SetRoot(5.f); Health->SetStun(5.f); Health->SetSilence(5.f);
            Health->SetAntiHeal(TEXT("Test.Wound"), 0.4f, 5.f);
            Health->ReceivePhysicalDamage(100000.f);
            DeathAt = Now;
            Test->TestTrue(TEXT("Lethal damage starts the four-second respawn immediately"), PC->IsRespawning());
            Test->TestTrue(TEXT("Respawn countdown starts at four seconds"), PC->GetRespawnRemaining() >= 3.9f);
            Test->TestFalse(TEXT("Death cancels the committed Q wind-up"), PC->IsCombatBusy());
            Test->TestFalse(TEXT("Death clears the pending combat impact timer"), FMistRespawnTestAccess::HasCombatTimer(PC));
            Test->TestFalse(TEXT("Death clears ability aiming"), FMistRespawnTestAccess::Aiming(PC));
            Test->TestNull(TEXT("Death clears repeated attack targeting"), FMistRespawnTestAccess::Target(PC));
            Test->TestEqual(TEXT("Death disables character movement"), static_cast<int32>(Hero->GetCharacterMovement()->MovementMode.GetValue()), static_cast<int32>(MOVE_None));
            Test->TestTrue(TEXT("Death removes horizontal momentum"), Hero->GetVelocity().Size2D() < 0.1f);
            Test->TestFalse(TEXT("Death clears root"), Health->IsRooted());
            Test->TestFalse(TEXT("Death clears stun"), Health->IsStunned());
            Test->TestFalse(TEXT("Death clears silence"), Health->IsSilenced());
            Test->TestEqual(TEXT("Death clears anti-heal"), Health->GetAntiHeal(), 0.f);
            Test->TestEqual(TEXT("Death clears defensive buffs"), Health->GetDamageReduction(), 0.f);
            Test->TestEqual(TEXT("Death clears empowered recast"), PC->GetUltimateRecastRemaining(), 0.f);
            for (const FKey& Key : {EKeys::Q, EKeys::W, EKeys::E, EKeys::R, EKeys::F})
            {
                Test->TestFalse(FString::Printf(TEXT("A defeated hero cannot aim %s"), *Key.ToString()), FMistRespawnTestAccess::CanAim(PC, Key));
                FMistRespawnTestAccess::Commit(PC, Key, Direction);
            }
            const FVector DefeatedLocation = Hero->GetActorLocation();
            FMistRespawnTestAccess::Flash(PC, Direction, 200.f);
            Test->TestTrue(TEXT("Actual Flash input cannot teleport a defeated hero"), Hero->GetActorLocation().Equals(DefeatedLocation, 0.1f));
            FMistRespawnTestAccess::Auto(PC, Target.Get());
            VerifyDeadlines(PC, TEXT("Rejected dead inputs"));
            FMistRespawnTestAccess::Key(PC, EKeys::Q, IE_Pressed);
            Stage = 3;
            return false;
        }
        case 3:
            if (!bCheckedCancelledImpact && Now - DeathAt >= 0.4)
            {
                Test->TestEqual(TEXT("A Q cancelled by death never applies its delayed damage"), Target->CombatHealth->Health, 1200.f);
                Test->TestFalse(TEXT("Actual Q input while dead cannot begin aiming"), FMistRespawnTestAccess::Aiming(PC));
                FMistRespawnTestAccess::Key(PC, EKeys::Q, IE_Released);
                bCheckedCancelledImpact = true;
            }
            VerifyDeadlines(PC, TEXT("Cooldowns during defeat"));
            if (PC->IsRespawning()) return false;
            Test->TestTrue(TEXT("Natural respawn waits the full four-second delay"), Now - DeathAt >= 3.95);
            Test->TestTrue(TEXT("Respawn preserves the possessed character instance"), Hero == OriginalPawn.Get());
            Test->TestTrue(TEXT("Respawn returns to the original checkpoint, not the death location"),
                FVector::Dist2D(Hero->GetActorLocation(), OriginalSpawn.GetLocation()) < 0.1f);
            Test->TestTrue(TEXT("Respawn restores the original checkpoint facing"), Hero->GetActorRotation().Equals(OriginalSpawn.Rotator(), 0.5f));
            Test->TestEqual(TEXT("Respawn restores maximum health"), Health->Health, Health->MaxHealth);
            Test->TestEqual(TEXT("Respawn reenables walking"), static_cast<int32>(Hero->GetCharacterMovement()->MovementMode.GetValue()), static_cast<int32>(MOVE_Walking));
            Test->TestTrue(TEXT("Respawn starts without movement velocity"), Hero->GetVelocity().IsNearlyZero(0.1f));
            Test->TestTrue(TEXT("Respawn grants a fresh three-second protection window"), Health->GetSpawnProtectionRemaining() > 2.8f);
            VerifyProtected(Health, TEXT("Natural respawn"));
            VerifyDeadlines(PC, TEXT("Cooldowns after natural respawn"));
            Target->Destroy();
            MoveStart = Hero->GetActorLocation();
            UAIBlueprintHelperLibrary::SimpleMoveToLocation(PC, FixtureOrigin);
            Stage = 4; StageAt = Now;
            return false;
        case 4:
        {
            if (FVector::Dist2D(MoveStart, Hero->GetActorLocation()) < 50.f)
            {
                if (Now - StageAt > 2.0)
                { Test->AddError(FString::Printf(TEXT("Respawned hero could not move: position %s, origin %s, mode %d."), *Hero->GetActorLocation().ToString(), *MoveStart.ToString(), Hero->GetCharacterMovement()->MovementMode.GetValue())); return true; }
                return false;
            }
            Test->TestTrue(TEXT("Normal click navigation works after respawn"), FVector::Dist2D(MoveStart, Hero->GetActorLocation()) >= 50.f);
            Test->TestTrue(TEXT("Moving alone keeps spawn protection"), Health->IsSpawnProtected());
            PC->StopMovement();
            FMistRespawnTestAccess::Reset(PC);
            PlaceHero(Hero, FixtureOrigin);
            Health->GrantSpawnProtection(3.f);
            FMistRespawnTestAccess::Commit(PC, EKeys::E, Direction);
            Test->TestTrue(TEXT("Successfully applying defensive E preserves spawn protection"), PC->GetDefenseRemaining() > 4.8f && Health->IsSpawnProtected());
            const FVector FlashStart = Hero->GetActorLocation();
            FMistRespawnTestAccess::Flash(PC, Direction, 200.f);
            Test->TestTrue(TEXT("A successful F Flash preserves spawn protection"), FVector::Dist2D(FlashStart, Hero->GetActorLocation()) > 100.f && Health->IsSpawnProtected());
            FMistRespawnTestAccess::Key(PC, EKeys::Q, IE_Pressed);
            Stage = 5; StageAt = Now;
            return false;
        }
        case 5:
            if (!FMistRespawnTestAccess::Aiming(PC))
            {
                if (Now - StageAt > 1.0) { Test->AddError(TEXT("Q key did not start the protected aiming preview.")); return true; }
                return false;
            }
            Test->TestTrue(TEXT("Holding the real Q key to aim preserves spawn protection"), Health->IsSpawnProtected());
            FMistRespawnTestAccess::Cancel(PC);
            FMistRespawnTestAccess::Key(PC, EKeys::Q, IE_Released);
            Test->TestTrue(TEXT("Cancelling an aiming preview preserves spawn protection"), Health->IsSpawnProtected());
            FMistRespawnTestAccess::Commit(PC, EKeys::W, FVector::ZeroVector);
            FMistRespawnTestAccess::Auto(PC, nullptr);
            Test->TestTrue(TEXT("Invalid dash and empty attack orders preserve spawn protection"), Health->IsSpawnProtected());
            Test->TestFalse(TEXT("Invalid protected orders do not begin a cast"), PC->IsCombatBusy());
            FMistRespawnTestAccess::Commit(PC, EKeys::Q, Direction);
            Test->TestTrue(TEXT("A successful Q commit starts its cast"), PC->IsCombatBusy());
            Test->TestFalse(TEXT("A successful Q commit ends spawn protection immediately"), Health->IsSpawnProtected());
            Test->TestFalse(TEXT("Ending spawn protection removes damage immunity"), Health->IsInvulnerable());
            Test->TestFalse(TEXT("Ending spawn protection removes crowd-control immunity"), Health->IsDebuffImmune());
            Stage = 6; StageAt = Now;
            return false;
        case 6:
            if (PC->IsCombatBusy() || Now - StageAt < 0.3) return false;
            PlaceHero(Hero, FixtureOrigin);
            PreviousPawnResponse = Hero->GetCapsuleComponent()->GetCollisionResponseToChannel(ECC_Pawn);
            FMistRespawnTestAccess::Commit(PC, EKeys::W, Direction);
            Test->TestTrue(TEXT("A real W cast begins before the dash-death check"), PC->IsCombatBusy());
            Stage = 7; StageAt = Now;
            return false;
        case 7:
            if (!FMistRespawnTestAccess::Dashing(PC))
            {
                if (Now - StageAt > 0.5) { Test->AddError(TEXT("W did not enter its dash phase.")); return true; }
                return false;
            }
            Test->TestEqual(TEXT("Active dash temporarily ignores unit collision"), Hero->GetCapsuleComponent()->GetCollisionResponseToChannel(ECC_Pawn), ECR_Ignore);
            SavedDeadlines = FMistRespawnTestAccess::Deadlines(PC);
            Health->ReceivePhysicalDamage(100000.f);
            Test->TestFalse(TEXT("Death interrupts an active W dash"), FMistRespawnTestAccess::Dashing(PC));
            Test->TestFalse(TEXT("Death clears the dash cast flag"), PC->IsCombatBusy());
            if (!Test->TestTrue(TEXT("The controlled second defeat can respawn at the actual checkpoint"), FMistRespawnTestAccess::Revive(PC))) return true;
            Test->TestEqual(TEXT("Respawning after dash death restores the pre-dash unit collision"), Hero->GetCapsuleComponent()->GetCollisionResponseToChannel(ECC_Pawn), PreviousPawnResponse);
            VerifyDeadlines(PC, TEXT("Cooldowns after interrupted W"));
            Target = SpawnTarget(World, Hero->GetActorLocation() + Direction * 150.f);
            if (!Test->TestNotNull(TEXT("Post-respawn basic-attack target spawns"), Target.Get())) return true;
            FMistRespawnTestAccess::Auto(PC, Target.Get());
            Test->TestTrue(TEXT("An in-range Space attack begins an actual wind-up after respawn"), PC->IsCombatBusy());
            Test->TestFalse(TEXT("A successful Space attack ends spawn protection immediately"), Health->IsSpawnProtected());
            Stage = 8; StageAt = Now;
            return false;
        case 8:
        {
            if (PC->IsCombatBusy() || Now - StageAt < 0.25) return false;
            Test->TestEqual(TEXT("The post-respawn Space attack deals its normal timed damage"), Target->CombatHealth->Health, 1125.f, 0.1f);
            FMistRespawnTestAccess::Cancel(PC);
            Target->Destroy();
            FMistRespawnTestAccess::Reset(PC);
            PlaceHero(Hero, FixtureOrigin);
            // The target overlaps the sword radius at both the 810cm dead
            // outbound step and the 1000cm turn, independent of return angle.
            Target = SpawnTarget(World, FixtureOrigin + Direction * 905.f);
            if (!Test->TestNotNull(TEXT("Released ultimate target spawns"), Target.Get())) return true;
            Health->GrantSpawnProtection(3.f);
            FMistRespawnTestAccess::Commit(PC, EKeys::R, Direction);
            Test->TestFalse(TEXT("A successful ultimate commit ends spawn protection"), Health->IsSpawnProtected());
            AMistReturningSword* Sword = FMistRespawnTestAccess::ReleaseSword(PC);
            if (!Test->TestNotNull(TEXT("Actual ultimate release creates the projectile"), Sword)) return true;
            const double UltimateDeadline = FMistRespawnTestAccess::Deadlines(PC)[3];
            Health->ReceivePhysicalDamage(100000.f);
            Test->TestFalse(TEXT("A released sword survives the caster's defeat"), Sword->IsActorBeingDestroyed());
            Test->TestFalse(TEXT("Defeat permanently unbinds the released sword's catch benefit"), Sword->OnReturned.IsBound());
            Test->TestNull(TEXT("A dead hero has no active sword lock or recast tracking"), FMistRespawnTestAccess::Sword(PC));
            Sword->Tick(0.45f);
            Test->TestEqual(TEXT("A released sword still deals outbound damage while its caster is dead"), Target->CombatHealth->Health, 1200.f - 160.f / 1.2f, 0.1f);
            Test->TestEqual(TEXT("Released sword damage cannot heal a dead caster"), Health->Health, 0.f);
            if (!Test->TestTrue(TEXT("Released-sword fixture can revive the same pawn before the return"), FMistRespawnTestAccess::Revive(PC))) return true;
            Health->Health = 500.f;
            const float BeforeReturnHealth = Target->CombatHealth->Health;
            Sword->Tick(2.f);
            Test->TestTrue(TEXT("The old sword deals its separate return-leg hit after the caster revives"), Target->CombatHealth->Health < BeforeReturnHealth - 100.f);
            Test->TestEqual(TEXT("An old released sword cannot resume healing after same-pawn respawn"), Health->Health, 500.f);
            Test->TestEqual(TEXT("Catching a pre-death sword after respawn cannot grant a stale recast"), PC->GetUltimateRecastRemaining(), 0.f);
            Test->TestEqual(TEXT("Accelerated respawn preserves the committed ultimate cooldown"), FMistRespawnTestAccess::Deadlines(PC)[3], UltimateDeadline);
            // An owned projectile outside ActiveSword models the deliberately
            // detached release after defeat; Y must find and remove it too.
            AMistReturningSword* Orphan = World->SpawnActor<AMistReturningSword>();
            if (!Test->TestNotNull(TEXT("Detached owned sword fixture spawns"), Orphan)) return true;
            Orphan->Initialize(Hero, Direction);
            Orphan->SetActorTickEnabled(false);
            Test->TestNull(TEXT("Detached release is absent from active-sword tracking"), FMistRespawnTestAccess::Sword(PC));
            FMistRespawnTestAccess::Reset(PC);
            Test->TestTrue(TEXT("Developer Y reset destroys detached owned returning swords"), Orphan->IsActorBeingDestroyed());
            Test->TestFalse(TEXT("Developer Y reset clears protection for immediate practice"), Health->IsSpawnProtected());
            Test->TestEqual(TEXT("Developer Y reset restores full health"), Health->Health, Health->MaxHealth);
            for (const double Deadline : FMistRespawnTestAccess::Deadlines(PC)) Test->TestEqual(TEXT("Developer Y reset clears combat cooldown deadlines"), Deadline, 0.0);
            Health->EndSpawnProtection();
            Health->ReceivePhysicalDamage(100000.f);
            Test->TestTrue(TEXT("Final developer-reset fixture actually enters defeated state"), PC->IsRespawning());
            FMistRespawnTestAccess::Reset(PC);
            Test->TestFalse(TEXT("Developer Y while dead completes the pending revive immediately"), PC->IsRespawning());
            Test->TestEqual(TEXT("Developer Y while dead clears the stale automatic respawn deadline"), FMistRespawnTestAccess::RespawnDeadline(PC), 0.0);
            Test->TestEqual(TEXT("Developer Y while dead clears its displayed respawn countdown"), PC->GetRespawnRemaining(), 0.f);
            Test->TestEqual(TEXT("Developer Y while dead reenables walking"), static_cast<int32>(Hero->GetCharacterMovement()->MovementMode.GetValue()), static_cast<int32>(MOVE_Walking));
            Test->TestEqual(TEXT("Developer Y while dead restores maximum health"), Health->Health, Health->MaxHealth);
            Test->TestTrue(TEXT("Developer Y while dead preserves a fresh three-second spawn protection"), Health->GetSpawnProtectionRemaining() > 2.8f);
            VerifyProtected(Health, TEXT("Developer revive"));
            for (const double Deadline : FMistRespawnTestAccess::Deadlines(PC)) Test->TestEqual(TEXT("Developer revive also clears combat cooldown deadlines"), Deadline, 0.0);
            Target->Destroy();
            return true;
        }
        }
        return false;
    }

private:
    void VerifyProtected(UMistCombatHealth* Health, const FString& Context)
    {
        Test->TestTrue(Context + TEXT(" grants damage immunity"), Health->IsInvulnerable());
        Test->TestTrue(Context + TEXT(" grants crowd-control immunity"), Health->IsDebuffImmune());
        Test->TestEqual(Context + TEXT(" rejects physical damage"), Health->ReceivePhysicalDamage(100000.f), 0.f);
        Test->TestEqual(Context + TEXT(" rejects magic damage"), Health->ReceiveMagicDamage(100000.f), 0.f);
        Health->SetRoot(5.f); Health->SetStun(5.f); Health->SetSilence(5.f);
        Test->TestFalse(Context + TEXT(" rejects root"), Health->IsRooted());
        Test->TestFalse(Context + TEXT(" rejects stun"), Health->IsStunned());
        Test->TestFalse(Context + TEXT(" rejects silence"), Health->IsSilenced());
    }
    void VerifyDeadlines(AMistworldPlayerController* PC, const FString& Context)
    {
        const TArray<double> Current = FMistRespawnTestAccess::Deadlines(PC);
        const TCHAR* Names[] = {TEXT("Q"), TEXT("W"), TEXT("E"), TEXT("R"), TEXT("Flash"), TEXT("Auto")};
        for (int32 Index = 0; Index < Current.Num(); ++Index)
            Test->TestEqual(FString::Printf(TEXT("%s preserve %s's absolute cooldown deadline"), *Context, Names[Index]), Current[Index], SavedDeadlines[Index]);
    }
    bool MakeFixture(UWorld* World, AMistworldPlayerController* PC, ACharacter* Hero)
    {
        PC->StopMovement();
        UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
        FNavLocation Ground;
        if (!Nav || !Nav->ProjectPointToNavigation(FixtureOrigin, Ground, FVector(150.f, 150.f, 300.f)))
        { Test->AddError(TEXT("Lifecycle fixture's placed sentinel home has no navigation.")); return false; }
        const float HalfHeight = Hero->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
        FHitResult Floor;
        FCollisionObjectQueryParams Objects;
        Objects.AddObjectTypesToQuery(ECC_WorldStatic);
        FCollisionQueryParams Query(SCENE_QUERY_STAT(MistRespawnTestFixture), false, Hero);
        if (!World->LineTraceSingleByObjectType(Floor, Ground.Location + FVector(0.f, 0.f, 200.f),
            Ground.Location - FVector(0.f, 0.f, 200.f), Objects, Query))
        { Test->AddError(TEXT("Lifecycle fixture cannot trace the actual arena floor.")); return false; }
        FixtureOrigin = FVector(Ground.Location.X, Ground.Location.Y, Floor.ImpactPoint.Z + HalfHeight + 2.f);
        if (World->OverlapBlockingTestByChannel(FixtureOrigin, FQuat::Identity, ECC_Pawn,
            FCollisionShape::MakeCapsule(Hero->GetCapsuleComponent()->GetScaledCapsuleRadius(), HalfHeight), Query))
        { Test->AddError(TEXT("Lifecycle fixture's player capsule cannot fit at the projected start.")); return false; }
        PlaceHero(Hero, FixtureOrigin);
        for (int32 Angle = 0; Angle < 360; Angle += 30)
        {
            FVector End;
            const FVector Candidate = FVector::ForwardVector.RotateAngleAxis(Angle, FVector::UpVector);
            if (FMistRespawnTestAccess::Endpoint(PC, Candidate, 600.f, End)
                && FVector::Dist2D(FixtureOrigin, End) > 590.f
                && !World->LineTraceTestByChannel(FixtureOrigin, FixtureOrigin + Candidate * 1000.f, ECC_Visibility, Query))
            {
                Direction = Candidate;
                Test->AddInfo(FString::Printf(TEXT("Lifecycle fixture origin %s; nav projection %s; direction %s; original checkpoint %s."),
                    *FixtureOrigin.ToString(), *Ground.Location.ToString(), *Direction.ToString(), *OriginalSpawn.GetLocation().ToString()));
                return true;
            }
        }
        Test->AddError(TEXT("Lifecycle fixture needs a clear full W route and 1,000cm sword path."));
        return false;
    }
    static void PlaceHero(ACharacter* Hero, const FVector& Position)
    {
        Hero->GetCharacterMovement()->StopMovementImmediately();
        Hero->SetActorLocation(Position, false, nullptr, ETeleportType::TeleportPhysics);
    }
    static AMistTrainingDummy* SpawnTarget(UWorld* World, const FVector& Position)
    {
        FActorSpawnParameters Params;
        Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        return World->SpawnActor<AMistTrainingDummy>(Position, FRotator::ZeroRotator, Params);
    }

    FAutomationTestBase* Test;
    int32 Stage = 0;
    double Started = -1.0, StageAt = 0.0, DeathAt = 0.0;
    bool bCheckedCancelledImpact = false;
    FTransform OriginalSpawn;
    FVector FixtureOrigin, Direction, MoveStart;
    ECollisionResponse PreviousPawnResponse = ECR_Block;
    TArray<double> SavedDeadlines;
    TWeakObjectPtr<ACharacter> OriginalPawn;
    TWeakObjectPtr<AMistTrainingDummy> Target;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMistRespawnLifecycleTest, "Mistworld.Combat.RespawnLifecycle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FMistRespawnLifecycleTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(TEXT("/Game/Mistworld/Maps/Lvl_CombatTest")));
    ADD_LATENT_AUTOMATION_COMMAND(FMistWaitForRespawnNavigation(this));
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(FMistVerifyRespawnLifecycle(this));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    return true;
}

#endif
