#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Tests/AutomationEditorCommon.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/PlatformTime.h"
#include "TimerManager.h"
#include "CollisionQueryParams.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerStart.h"
#include "Components/CapsuleComponent.h"
#include "NavigationSystem.h"
#include "MistworldPlayerController.h"
#include "MistCombatHealth.h"
#include "MistCombatHUD.h"
#include "MistTrainingDummy.h"
#include "MistArenaEnemy.h"
#include "MistReturningSword.h"
#include "MistAbyssTrail.h"
#include "MistAbyssShockwave.h"

// Access controls fixture timing and reads authoritative deadlines. Every
// damage assertion below still exercises the actual controller and effect.
struct FMistAscensionTestAccess
{
    static void Reset(AMistworldPlayerController* PC) { PC->ResetCombatSandbox(); }
    static void Commit(AMistworldPlayerController* PC, const FKey& Key, const FVector& Direction)
    { PC->CommitAbility(Key, Direction); }
    static bool Endpoint(AMistworldPlayerController* PC, const FVector& Direction, float Distance, FVector& End)
    { return PC->FindLegalMovementEndpoint(Direction, Distance, End); }
    static TArray<double> Deadlines(const AMistworldPlayerController* PC)
    { return {PC->QReadyAt, PC->WReadyAt, PC->EReadyAt, PC->RReadyAt, PC->FlashReadyAt, PC->AutoReadyAt}; }
    static void SeedDeadlines(AMistworldPlayerController* PC, double Now)
    {
        PC->QReadyAt = Now + 6.; PC->WReadyAt = Now + 12.; PC->EReadyAt = Now + 30.;
        PC->RReadyAt = Now + 120.; PC->FlashReadyAt = Now + 60.; PC->AutoReadyAt = Now + 1.;
    }
    static void SeedR(AMistworldPlayerController* PC, double Deadline) { PC->RReadyAt = Deadline; }
    static void SeedW(AMistworldPlayerController* PC, double Deadline) { PC->WReadyAt = Deadline; }
    static double RDeadline(const AMistworldPlayerController* PC) { return PC->RReadyAt; }
    static double WDeadline(const AMistworldPlayerController* PC) { return PC->WReadyAt; }
    static float GuardRemainder(const AMistworldPlayerController* PC) { return PC->AbyssGuardLostHealth; }
    static void SetGuardRemainder(AMistworldPlayerController* PC, float Remainder) { PC->AbyssGuardLostHealth = Remainder; }
    static float AttackMultiplier(const AMistworldPlayerController* PC) { return PC->GetAttackSpeedMultiplier(); }
    static void Haste(AMistworldPlayerController* PC, float Haste) { PC->AbilityHaste = Haste; }
    static void Aim(AMistworldPlayerController* PC, bool Active)
    {
        PC->ActiveAimKey = Active ? EKeys::Q : FKey(); PC->bAimingQ = Active;
    }
    static void CastFlag(AMistworldPlayerController* PC, bool Active) { PC->bCastingCombat = Active; }
    static void DashFlag(AMistworldPlayerController* PC, bool Active) { PC->bDashing = Active; }
    static void Recast(AMistworldPlayerController* PC, double Until) { PC->UltimateRecastUntil = Until; }
    static void Warcry(AMistworldPlayerController* PC, double Until) { PC->AbyssWarcryUntil = Until; }
    static void TrackSword(AMistworldPlayerController* PC, AMistReturningSword* Sword) { PC->ActiveSword = Sword; }
    static FTransform Checkpoint(const AMistworldPlayerController* PC) { return PC->RespawnTransform; }
    static bool Revive(AMistworldPlayerController* PC) { return PC->RespawnHero(); }
    static void Auto(AMistworldPlayerController* PC, AActor* Target)
    { PC->RequestBasicAttack(Target); PC->BeginBasicAttack(); }
    static bool CanAim(const AMistworldPlayerController* PC, const FKey& Key) { return PC->CanAimAbility(Key); }
    static void MovementPress(AMistworldPlayerController* PC)
    { PC->OnInputStarted(); PC->OnSetDestinationTriggered(); }
    static void MovementRelease(AMistworldPlayerController* PC) { PC->OnSetDestinationReleased(); }
    static bool AcceptedMovement(const AMistworldPlayerController* PC) { return PC->bDestinationInputAccepted; }
};

namespace
{
const FKey Slots[] = {EKeys::Q, EKeys::W, EKeys::E, EKeys::R};

class FMistWaitForAscensionNavigation final : public IAutomationLatentCommand
{
public:
    explicit FMistWaitForAscensionNavigation(FAutomationTestBase* InTest) : Test(InTest) {}
    bool Update() override
    {
        if (Started < 0.) Started = FPlatformTime::Seconds();
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
        if (FPlatformTime::Seconds() - Started > 30.)
        { Test->AddError(TEXT("Ascension fixture could not build combat-map navigation.")); return true; }
        return false;
    }
private:
    FAutomationTestBase* Test;
    double Started = -1.;
    bool bBuilt = false;
};

void PlaceHero(ACharacter* Hero, const FVector& Position)
{
    Hero->GetCharacterMovement()->StopMovementImmediately();
    Hero->SetActorLocation(Position, false, nullptr, ETeleportType::TeleportPhysics);
}

AMistTrainingDummy* SpawnTarget(UWorld* World, const FVector& Position)
{
    FActorSpawnParameters Parameters;
    Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    AMistTrainingDummy* Target = World->SpawnActor<AMistTrainingDummy>(Position, FRotator::ZeroRotator, Parameters);
    if (Target)
    {
        Target->CombatHealth->Armor = Target->CombatHealth->MagicResistance = 0.f;
        Target->CombatHealth->MaxHealth = Target->CombatHealth->Health = 1200.f;
    }
    return Target;
}

class FMistVerifyLoadoutRules final : public IAutomationLatentCommand
{
public:
    explicit FMistVerifyLoadoutRules(FAutomationTestBase* InTest) : Test(InTest) {}
    bool Update() override
    {
        if (Started < 0.) Started = FPlatformTime::Seconds();
        if (FPlatformTime::Seconds() - Started > 40.)
        { Test->AddError(FString::Printf(TEXT("Loadout rules timed out at stage %d."), Stage)); return true; }
        UWorld* World = GEditor ? GEditor->PlayWorld : nullptr;
        AMistworldPlayerController* PC = World ? Cast<AMistworldPlayerController>(World->GetFirstPlayerController()) : nullptr;
        ACharacter* Hero = PC ? Cast<ACharacter>(PC->GetPawn()) : nullptr;
        UMistCombatHealth* Health = Hero ? Hero->FindComponentByClass<UMistCombatHealth>() : nullptr;
        if (!World || !World->HasBegunPlay() || !Health) return false;
        AMistCombatHUD* HUD = Cast<AMistCombatHUD>(PC->GetHUD());
        if (Stage == 0 && !HUD) return false;
        const double Now = World->GetTimeSeconds();
        if (Stage == 0)
        {
            for (TActorIterator<AMistTrainingDummy> It(World); It; ++It) It->Destroy();
            FMistAscensionTestAccess::Reset(PC);
            Checkpoint = FMistAscensionTestAccess::Checkpoint(PC).GetLocation();
            PlaceHero(Hero, Checkpoint);
            Test->TestTrue(TEXT("Possession's checkpoint is an eligible loadout station"), PC->IsNearCheckpoint());
            Test->TestTrue(TEXT("The initial original-kit loadout is outside combat"), PC->GetLoadoutLockReason().IsEmpty());
            Test->TestFalse(TEXT("Abyss initially requires the temporary test-menu unlock"), PC->IsAbyssUnlocked());
            Test->TestFalse(TEXT("Locked ascension cannot equip its Q"), PC->TryEquipAbility(EKeys::Q, true));
            Test->TestFalse(TEXT("Normal combat starts with HUD click dispatch disabled"), static_cast<bool>(PC->bEnableClickEvents));
            PC->ToggleLoadoutMenu();
            Test->TestTrue(TEXT("Opening the loadout menu enables real HUD click dispatch"), PC->IsLoadoutMenuOpen() && PC->bEnableClickEvents);
            Test->TestTrue(TEXT("The menu dispatches left mouse-button clicks"), PC->ClickEventKeys.Contains(EKeys::LeftMouseButton));
            for (const FKey& Key : {EKeys::Q, EKeys::W, EKeys::E, EKeys::R, EKeys::F})
                Test->TestFalse(FString::Printf(TEXT("Open loadout menu blocks combat input %s"), *Key.ToString()), FMistAscensionTestAccess::CanAim(PC, Key));
            const FVector BeforeMenuInput = Hero->GetActorLocation();
            FMistAscensionTestAccess::MovementPress(PC);
            Test->TestFalse(TEXT("A loadout-menu mouse press cannot become an accepted movement order"), FMistAscensionTestAccess::AcceptedMovement(PC));
            FMistAscensionTestAccess::Commit(PC, EKeys::E, FVector::ForwardVector);
            Test->TestEqual(TEXT("Ability input while the menu is open never grants defense"), PC->GetDefenseRemaining(), 0.f);
            for (const double Deadline : FMistAscensionTestAccess::Deadlines(PC))
                Test->TestEqual(TEXT("Blocked menu combat input never spends a cooldown"), Deadline, 0.);
            HUD->NotifyHitBoxClick(TEXT("Mist.Abyss.0"));
            Test->TestFalse(TEXT("The locked Abyss-Q HUD button cannot equip before unlock"), PC->IsAbyssEquipped(EKeys::Q));
            HUD->NotifyHitBoxClick(TEXT("Mist.Unlock"));
            Test->TestTrue(TEXT("The actual HUD unlock button grants test ascension access"), PC->IsAbyssUnlocked());
            for (int32 Index = 0; Index < 4; ++Index)
            {
                HUD->NotifyHitBoxClick(FName(*FString::Printf(TEXT("Mist.Abyss.%d"), Index)));
                Test->TestTrue(FString::Printf(TEXT("HUD Abyss button equips %s independently"), *Slots[Index].ToString()), PC->IsAbyssEquipped(Slots[Index]));
                HUD->NotifyHitBoxClick(FName(*FString::Printf(TEXT("Mist.Original.%d"), Index)));
                Test->TestFalse(FString::Printf(TEXT("HUD original button restores %s independently"), *Slots[Index].ToString()), PC->IsAbyssEquipped(Slots[Index]));
            }
            HUD->NotifyHitBoxClick(TEXT("Mist.Close"));
            Test->TestTrue(TEXT("The close-button press leaves movement blocked until release"), PC->IsLoadoutMenuOpen() && PC->bEnableClickEvents);
            FMistAscensionTestAccess::MovementPress(PC);
            Test->TestFalse(TEXT("The close-button press cannot queue a move through Enhanced Input"), FMistAscensionTestAccess::AcceptedMovement(PC));
            HUD->NotifyHitBoxRelease(TEXT("Mist.Close"));
            Test->TestFalse(TEXT("The HUD close release closes the loadout menu"), PC->IsLoadoutMenuOpen());
            Test->TestFalse(TEXT("Closing the menu disables combat HUD click dispatch"), static_cast<bool>(PC->bEnableClickEvents));
            FMistAscensionTestAccess::MovementRelease(PC);
            Test->TestFalse(TEXT("The release belonging to the close click cannot retain a movement order"), FMistAscensionTestAccess::AcceptedMovement(PC));
            Test->TestTrue(TEXT("Opening, editing, and closing the HUD does not move the hero"), Hero->GetActorLocation().Equals(BeforeMenuInput, 0.01f));
            Test->TestTrue(TEXT("Test unlock flag is exposed to the UI"), PC->IsAbyssUnlocked());
            Test->TestFalse(TEXT("Flash is not a swappable ascension slot"), PC->TryEquipAbility(EKeys::F, true));
            Test->TestFalse(TEXT("Space is not a swappable ascension slot"), PC->TryEquipAbility(EKeys::SpaceBar, true));
            FMistAscensionTestAccess::SeedDeadlines(PC, Now);
            const TArray<double> Saved = FMistAscensionTestAccess::Deadlines(PC);
            for (int32 Mask = 0; Mask < 16; ++Mask)
                for (int32 Index = 0; Index < 4; ++Index)
                {
                    const bool bAbyss = (Mask & (1 << Index)) != 0;
                    Test->TestTrue(FString::Printf(TEXT("Mask %d can equip %s version %d while its slot cools down"),
                        Mask, *Slots[Index].ToString(), bAbyss), PC->TryEquipAbility(Slots[Index], bAbyss));
                    Test->TestEqual(FString::Printf(TEXT("Mask %d stores the selected %s definition"), Mask, *Slots[Index].ToString()),
                        PC->IsAbyssEquipped(Slots[Index]), bAbyss);
                    const TArray<double> Current = FMistAscensionTestAccess::Deadlines(PC);
                    for (int32 Deadline = 0; Deadline < Saved.Num(); ++Deadline)
                        Test->TestEqual(TEXT("A same-slot swap never changes any remaining cooldown"), Current[Deadline], Saved[Deadline]);
                }
            Test->TestFalse(TEXT("The test unlock cannot be revoked while an Abyss slot remains equipped"), PC->SetAbyssUnlocked(false));
            Test->TestTrue(TEXT("A rejected unlock change retains access to the equipped kit"), PC->IsAbyssUnlocked());

            PlaceHero(Hero, Checkpoint + FVector(1000.f, 0.f, 0.f));
            Test->TestFalse(TEXT("Loadout station eligibility does not extend across the arena"), PC->IsNearCheckpoint());
            Reject(PC, TEXT("Outside checkpoint"));
            PlaceHero(Hero, Checkpoint);
            Health->SetStun(2.f); Reject(PC, TEXT("Stun")); Health->SetStun(0.f);
            Health->SetSilence(2.f); Reject(PC, TEXT("Silence")); Health->SetSilence(0.f);
            Health->SetRoot(2.f); Reject(PC, TEXT("Root")); Health->SetRoot(0.f);
            FMistAscensionTestAccess::Aim(PC, true); Reject(PC, TEXT("Held aim preview")); FMistAscensionTestAccess::Aim(PC, false);
            FMistAscensionTestAccess::CastFlag(PC, true); Reject(PC, TEXT("Committed wind-up")); FMistAscensionTestAccess::CastFlag(PC, false);
            FMistAscensionTestAccess::DashFlag(PC, true); Reject(PC, TEXT("Dash sequence")); FMistAscensionTestAccess::DashFlag(PC, false);
            FMistAscensionTestAccess::Recast(PC, Now + 2.); Reject(PC, TEXT("Pending original-R recast")); FMistAscensionTestAccess::Recast(PC, 0.);
            FMistAscensionTestAccess::Warcry(PC, Now + 10.); Reject(PC, TEXT("Active Abyss warcry")); FMistAscensionTestAccess::Warcry(PC, 0.);
            Health->SetDamageReduction(TEXT("Test.ActiveGuard"), 0.33f, 6.f); Reject(PC, TEXT("Active defense buff"));
            Health->ResetHealth(); Health->ClearCombatLock();
            Health->SetShield(100.f, 5.f); Reject(PC, TEXT("Active shield")); Health->SetShield(0.f, 0.f);
            Health->Health = 0.f; Reject(PC, TEXT("Defeated hero")); Health->Health = Health->MaxHealth;
            AMistReturningSword* Sword = World->SpawnActor<AMistReturningSword>();
            if (!Test->TestNotNull(TEXT("Released sword lock fixture"), Sword)) return true;
            Sword->Initialize(Hero, FVector::ForwardVector); Sword->SetActorTickEnabled(false);
            FMistAscensionTestAccess::TrackSword(PC, Sword); Reject(PC, TEXT("Released returning sword"));
            Sword->Destroy(); FMistAscensionTestAccess::TrackSword(PC, nullptr);
            AMistAbyssTrail* Trail = World->SpawnActor<AMistAbyssTrail>();
            if (!Test->TestNotNull(TEXT("Owned trail lock fixture"), Trail)) return true;
            Trail->Initialize(Hero, false); Trail->AddSegment(Checkpoint, Checkpoint + FVector(100.f, 0.f, 0.f));
            Trail->FinishAddingSegments(); Trail->SetActorTickEnabled(false); Reject(PC, TEXT("Live attributed trail")); Trail->Destroy();
            AMistAbyssShockwave* Wave = World->SpawnActor<AMistAbyssShockwave>();
            if (!Test->TestNotNull(TEXT("Owned shockwave lock fixture"), Wave)) return true;
            Wave->Initialize(Hero, FVector::ForwardVector, {}); Wave->SetActorTickEnabled(false);
            Reject(PC, TEXT("Released shockwave")); Wave->Destroy();
            Test->TestTrue(TEXT("Cooldowns alone do not lock a legal checkpoint loadout"), PC->GetLoadoutLockReason().IsEmpty());

            Source = SpawnTarget(World, Checkpoint + FVector(180.f, 0.f, 0.f));
            if (!Test->TestNotNull(TEXT("Hostile source for the actual damage event"), Source.Get())) return true;
            Health->ReceivePhysicalDamage(12.f, Source.Get());
            Test->TestTrue(TEXT("Taking hostile damage starts the full ten-second combat lock"), Health->GetCombatLockRemaining() > 9.9f);
            Reject(PC, TEXT("Damage-based combat lock"));
            LockStartedAt = Now; Stage = 1; return false;
        }
        if (Stage == 1)
        {
            if (Now - LockStartedAt < 9.5) return false;
            Test->TestTrue(TEXT("Switching remains unavailable before ten quiet seconds"), Health->GetCombatLockRemaining() > 0.f);
            Reject(PC, TEXT("Combat lock near expiry")); Stage = 2; return false;
        }
        if (Stage == 2)
        {
            if (Health->GetCombatLockRemaining() > 0.f) return false;
            Test->TestTrue(TEXT("Ten quiet seconds release the damage-based loadout lock"), Now - LockStartedAt >= 9.95);
            Test->TestTrue(TEXT("A legal switch succeeds after actual combat-lock expiry"), PC->TryEquipAbility(EKeys::Q, false));
            Source->Destroy();
            FMistAscensionTestAccess::Reset(PC);
            FMistAscensionTestAccess::Haste(PC, 20.f);
            FMistAscensionTestAccess::Commit(PC, EKeys::Q, FVector::ForwardVector);
            const double Deadline = FMistAscensionTestAccess::Deadlines(PC)[0];
            Test->TestEqual(TEXT("20 ability haste records a five-second Q cooldown at commitment"), Deadline - Now, 5., 0.01);
            FMistAscensionTestAccess::Haste(PC, 100.f);
            Test->TestEqual(TEXT("Changing haste cannot shorten an already recorded Q cooldown"), FMistAscensionTestAccess::Deadlines(PC)[0], Deadline);
            Stage = 3; StageAt = Now; return false;
        }
        if (Stage == 3)
        {
            if (PC->IsCombatBusy()) return false;
            Test->TestEqual(TEXT("A committed damaging miss does not create a damage-based combat lock"), Health->GetCombatLockRemaining(), 0.f);
            const double Deadline = FMistAscensionTestAccess::Deadlines(PC)[0];
            Test->TestTrue(TEXT("A remaining Q cooldown permits switching after sequence/combat ends"), PC->TryEquipAbility(EKeys::Q, true));
            Test->TestEqual(TEXT("A switch preserves the previous haste snapshot's remaining real time"), FMistAscensionTestAccess::Deadlines(PC)[0], Deadline);
            FMistAscensionTestAccess::Haste(PC, 0.f);
            FMistAscensionTestAccess::Reset(PC);
            return true;
        }
        return false;
    }
private:
    void Reject(AMistworldPlayerController* PC, const FString& Context)
    {
        const bool PreviouslyEquipped = PC->IsAbyssEquipped(EKeys::Q);
        const TArray<double> Deadlines = FMistAscensionTestAccess::Deadlines(PC);
        Test->TestFalse(Context + TEXT(" explains why loadout changes are unavailable"), PC->GetLoadoutLockReason().IsEmpty());
        Test->TestFalse(Context + TEXT(" rejects the actual equip operation"), PC->TryEquipAbility(EKeys::Q, !PreviouslyEquipped));
        Test->TestEqual(Context + TEXT(" preserves the current definition"), PC->IsAbyssEquipped(EKeys::Q), PreviouslyEquipped);
        const TArray<double> Current = FMistAscensionTestAccess::Deadlines(PC);
        for (int32 Index = 0; Index < Deadlines.Num(); ++Index)
            Test->TestEqual(Context + TEXT(" preserves every cooldown"), Current[Index], Deadlines[Index]);
    }
    FAutomationTestBase* Test;
    int32 Stage = 0;
    double Started = -1., LockStartedAt = 0., StageAt = 0.;
    FVector Checkpoint;
    TWeakObjectPtr<AMistTrainingDummy> Source;
};

class FMistVerifyAllAscensionKits final : public IAutomationLatentCommand
{
public:
    explicit FMistVerifyAllAscensionKits(FAutomationTestBase* InTest) : Test(InTest) {}
    bool Update() override
    {
        if (Started < 0.) Started = FPlatformTime::Seconds();
        if (FPlatformTime::Seconds() - Started > 180.)
        { Test->AddError(FString::Printf(TEXT("Ascension matrix timed out at mask %d stage %d."), Mask, Stage)); return true; }
        UWorld* World = GEditor ? GEditor->PlayWorld : nullptr;
        AMistworldPlayerController* PC = World ? Cast<AMistworldPlayerController>(World->GetFirstPlayerController()) : nullptr;
        ACharacter* Hero = PC ? Cast<ACharacter>(PC->GetPawn()) : nullptr;
        UMistCombatHealth* Health = Hero ? Hero->FindComponentByClass<UMistCombatHealth>() : nullptr;
        if (!World || !World->HasBegunPlay() || !Health) return false;
        const double Now = World->GetTimeSeconds();
        const bool AbyssQ = (Mask & 1) != 0, AbyssW = (Mask & 2) != 0, AbyssE = (Mask & 4) != 0, AbyssR = (Mask & 8) != 0;
        switch (Stage)
        {
        case 0:
            for (TActorIterator<AMistArenaEnemy> It(World); It; ++It) Origin = It->GetHomeLocation();
            for (TActorIterator<AMistTrainingDummy> It(World); It; ++It) It->Destroy();
            Checkpoint = FMistAscensionTestAccess::Checkpoint(PC).GetLocation();
            FMistAscensionTestAccess::Reset(PC);
            if (!MakeFixture(World, PC, Hero)) return true;
            Stage = 1; return false;
        case 1:
            if (Mask == 16)
            {
                ResetAbility(PC, Hero, Health);
                PlaceHero(Hero, Checkpoint);
                Test->AddInfo(TEXT("All 16 original/Abyss Q-W-E-R combinations completed the actual hit, guard, dash, ultimate, and equipped-W recast checks."));
                return true;
            }
            ResetAbility(PC, Hero, Health);
            PlaceHero(Hero, Checkpoint);
            if (!PC->SetAbyssUnlocked(true)) { Test->AddError(TEXT("Cannot grant test ascension unlock at the checkpoint.")); return true; }
            for (int32 Index = 0; Index < 4; ++Index)
                if (!Test->TestTrue(Label(TEXT("can equip every selected slot")), PC->TryEquipAbility(Slots[Index], (Mask & (1 << Index)) != 0))) return true;
            Test->TestEqual(Label(TEXT("Equipped Q preview uses its actual cone range")), PC->GetEquippedDefinition(EKeys::Q).Range, AbyssQ ? 250.f : 300.f);
            Test->TestEqual(Label(TEXT("Equipped Q preview uses its actual cone angle")), PC->GetEquippedDefinition(EKeys::Q).ConeAngle, AbyssQ ? 90.f : 110.f);
            Test->TestEqual(Label(TEXT("Only Abyss Q exposes a shockwave preview")), PC->GetEquippedDefinition(EKeys::Q).ShockwaveRange, AbyssQ ? 800.f : 0.f);
            Test->TestEqual(Label(TEXT("Equipped W preview uses its actual dash width")), PC->GetEquippedDefinition(EKeys::W).Radius, AbyssW ? 65.f : 75.f);
            Test->TestEqual(Label(TEXT("R recast preview resolves the equipped empowered W width")), PC->GetEquippedDefinition(EKeys::W, true).Radius, AbyssW ? 80.f : 100.f);
            Test->TestEqual(Label(TEXT("Equipped R preview resolves circle versus sword range")), PC->GetEquippedDefinition(EKeys::R).Range, AbyssR ? 400.f : 1000.f);
            PlaceHero(Hero, Origin);
            Target = SpawnTarget(World, Origin + Direction * 150.f);
            Second = SpawnTarget(World, Origin + Direction * 205.f);
            Distant = SpawnTarget(World, Origin + Direction * 600.f);
            if (!TargetsExist()) return true;
            RDeadline = Now + 60.;
            FMistAscensionTestAccess::SeedR(PC, RDeadline);
            FMistAscensionTestAccess::Commit(PC, EKeys::Q, Direction);
            Test->TestTrue(Label(TEXT("Q begins its committed cast")), PC->IsCombatBusy());
            Test->TestEqual(Label(TEXT("Q spends the six-second slot cooldown")), FMistAscensionTestAccess::Deadlines(PC)[0] - Now, 6., 0.01);
            Stage = 2; StageAt = Now; return false;
        case 2:
            if (Now - StageAt < 1.2 || PC->IsCombatBusy()) return false;
            Test->TestEqual(Label(TEXT("Q direct hit has the equipped physical damage and excludes its shockwave")), Target->CombatHealth->Health, AbyssQ ? 1050.f : 1060.f, 0.1f);
            Test->TestEqual(Label(TEXT("Q's second direct target is independently hit once")), Second->CombatHealth->Health, AbyssQ ? 1050.f : 1060.f, 0.1f);
            Test->TestEqual(Label(TEXT("Only Abyss Q reaches the distant line target for 80 magic damage")), Distant->CombatHealth->Health, AbyssQ ? 1120.f : 1200.f, 0.1f);
            Test->TestEqual(Label(TEXT("Multiple direct elites refund R once per Q")), FMistAscensionTestAccess::RDeadline(PC), RDeadline - 5., 0.01);
            ResetAbility(PC, Hero, Health);
            RDeadline = Now + 60.; FMistAscensionTestAccess::SeedR(PC, RDeadline);
            FMistAscensionTestAccess::Commit(PC, EKeys::E, Direction);
            Test->TestEqual(Label(TEXT("E has its equipped reduction")), Health->GetDamageReduction(), AbyssE ? 0.33f : 0.5f, 0.001f);
            Test->TestEqual(Label(TEXT("E has its equipped duration")), PC->GetDefenseRemaining(), AbyssE ? 6.f : 5.f, 0.05f);
            Test->TestEqual(Label(TEXT("E spends the shared thirty-second slot cooldown")), FMistAscensionTestAccess::Deadlines(PC)[2] - Now, 30., 0.01);
            Health->ReceivePhysicalDamage(100.f);
            Test->TestEqual(Label(TEXT("E does not refund before one hundred actual lost HP")), FMistAscensionTestAccess::RDeadline(PC), RDeadline, 0.01);
            Health->ReceivePhysicalDamage(100.f);
            Test->TestEqual(Label(TEXT("Only Abyss E refunds from actual health loss")), FMistAscensionTestAccess::RDeadline(PC), RDeadline - (AbyssE ? 1. : 0.), 0.01);
            if (AbyssE) Test->TestEqual(Label(TEXT("Abyss E retains the 34 HP fractional remainder")), FMistAscensionTestAccess::GuardRemainder(PC), 34.f, 0.05f);
            ResetAbility(PC, Hero, Health);
            Target = SpawnTarget(World, Origin + Direction * 150.f);
            if (!Test->TestNotNull(Label(TEXT("W target exists")), Target.Get())) return true;
            Health->Health = 500.f;
            FMistAscensionTestAccess::Commit(PC, EKeys::W, Direction);
            Test->TestEqual(Label(TEXT("W spends the twelve-second slot cooldown")), FMistAscensionTestAccess::WDeadline(PC) - Now, 12., 0.01);
            Stage = 3; StageAt = Now; return false;
        case 3:
            if (Now - StageAt < 0.7 || PC->IsCombatBusy()) return false;
            Test->TestEqual(Label(TEXT("W direct dash damage matches its equipped variant")), Target->CombatHealth->Health, AbyssW ? 1120.f : 1100.f, 0.1f);
            Test->TestEqual(Label(TEXT("Ordinary W never supplies ultimate healing")), Health->Health, 500.f, 0.01f);
            Test->TestTrue(Label(TEXT("W follows the complete legal six-metre route")), FVector::Dist2D(Hero->GetActorLocation(), Origin + Direction * 600.f) < 5.f);
            Stage = 4; return false;
        case 4:
            if (Now - StageAt < 1.5) return false;
            Test->TestEqual(Label(TEXT("Abyss W's first delayed trail tick is 20 magic damage")), Target->CombatHealth->Health, 1100.f, 0.1f);
            Test->TestEqual(Label(TEXT("An ordinary Abyss trail still cannot heal")), Health->Health, 500.f, 0.01f);
            ResetAbility(PC, Hero, Health);
            Target = SpawnTarget(World, Origin + Direction * 150.f);
            if (!Test->TestNotNull(Label(TEXT("Ultimate target exists")), Target.Get())) return true;
            Health->Health = 500.f;
            FMistAscensionTestAccess::SeedW(PC, Now + 12.);
            FMistAscensionTestAccess::Commit(PC, EKeys::R, Direction);
            RDeadline = FMistAscensionTestAccess::RDeadline(PC);
            WDeadline = FMistAscensionTestAccess::WDeadline(PC);
            Test->TestEqual(Label(TEXT("R spends the shared 120-second slot cooldown")), RDeadline - Now, 120., 0.01);
            Stage = 5; StageAt = Now; return false;
        case 5:
            if (AbyssR)
            {
                if (Now - StageAt < 0.6 || PC->IsCombatBusy()) return false;
                Test->TestEqual(Label(TEXT("Abyss warcry deals 260 magic damage once")), Target->CombatHealth->Health, 940.f, 0.1f);
                Test->TestEqual(Label(TEXT("Abyss R supplies no damage-derived healing")), Health->Health, 500.f, 0.01f);
                Test->TestEqual(Label(TEXT("Abyss R creates no empowered-W recast")), PC->GetUltimateRecastRemaining(), 0.f);
                Test->TestTrue(Label(TEXT("Abyss warcry lasts ten seconds from release")), PC->GetWarcryRemaining() > 9.6f);
                Test->TestEqual(Label(TEXT("Warcry raises constant walk speed by 33 percent")), Hero->GetCharacterMovement()->MaxWalkSpeed, 665.f, 0.05f);
                Test->TestEqual(Label(TEXT("Warcry independently raises auto rate by 33 percent")), FMistAscensionTestAccess::AttackMultiplier(PC), 1.33f, 0.001f);
                FMistAscensionTestAccess::Auto(PC, Target.Get());
                Test->TestTrue(Label(TEXT("Warcry can begin a real basic-attack wind-up")), PC->IsCombatBusy());
                Test->TestEqual(Label(TEXT("Warcry speeds the whole auto interval without changing ability cooldowns")),
                    FMistAscensionTestAccess::Deadlines(PC)[5] - Now, 1. / 1.33, 0.01);
                Stage = 8; StageAt = Now; return false;
            }
            if (PC->GetUltimateRecastRemaining() <= 0.f)
            {
                if (Now - StageAt > 3.) { Test->AddError(Label(TEXT("original sword did not return with a recast"))); return true; }
                return false;
            }
            Test->TestEqual(Label(TEXT("Original sword deals 160 damage on each leg")), Target->CombatHealth->Health, 880.f, 0.1f);
            Test->TestEqual(Label(TEXT("Original sword heals 20 percent of its actual two-leg damage")), Health->Health, 564.f, 0.1f);
            Target->Destroy(); Target = SpawnTarget(World, Origin + Direction * 150.f);
            if (!Test->TestNotNull(Label(TEXT("Empowered dash target exists")), Target.Get())) return true;
            Health->Health = 500.f;
            FMistAscensionTestAccess::Commit(PC, EKeys::R, Direction);
            Test->TestTrue(Label(TEXT("R recast can commit while ordinary W remains unavailable")), PC->IsCombatBusy());
            Test->TestEqual(Label(TEXT("Empowered dash preserves ordinary W's deadline")), FMistAscensionTestAccess::WDeadline(PC), WDeadline);
            Test->TestEqual(Label(TEXT("Empowered dash preserves initial R's deadline")), FMistAscensionTestAccess::RDeadline(PC), RDeadline);
            Stage = 6; StageAt = Now; return false;
        case 6:
            if (Now - StageAt < 0.7 || PC->IsCombatBusy()) return false;
            Test->TestEqual(Label(TEXT("Original R empowers whichever W is currently equipped")), Target->CombatHealth->Health, AbyssW ? 1060.f : 1020.f, 0.1f);
            Test->TestEqual(Label(TEXT("Both empowered W variants heal 20 percent actual dash damage")), Health->Health, AbyssW ? 528.f : 536.f, 0.1f);
            Test->TestEqual(Label(TEXT("The empowered-W recast is consumed once")), PC->GetUltimateRecastRemaining(), 0.f);
            Test->TestTrue(Label(TEXT("Both empowered variants reach their eight-metre endpoint")), FVector::Dist2D(Hero->GetActorLocation(), Origin + Direction * 800.f) < 5.f);
            Stage = 7; return false;
        case 7:
            if (Now - StageAt < 2.6) return false;
            Test->TestEqual(Label(TEXT("Only empowered Abyss W leaves two 30-damage ticks after the original recast window would expire")), Target->CombatHealth->Health, AbyssW ? 1000.f : 1020.f, 0.1f);
            Test->TestEqual(Label(TEXT("Empowered-trail healing retains R attribution beyond the recast window")), Health->Health, AbyssW ? 540.f : 536.f, 0.1f);
            ++Mask; Stage = 1; return false;
        case 8:
            if (Now - StageAt < 0.2 || PC->IsCombatBusy()) return false;
            Test->TestEqual(Label(TEXT("Warcry speeds autos without increasing their 90 physical damage")), Target->CombatHealth->Health, 850.f, 0.1f);
            Test->TestEqual(Label(TEXT("The accelerated auto does not change R's cooldown")), FMistAscensionTestAccess::RDeadline(PC), RDeadline);
            ++Mask; Stage = 1; return false;
        }
        return false;
    }
private:
    FString Label(const TCHAR* Detail) const { return FString::Printf(TEXT("Loadout mask %02d: %s"), Mask, Detail); }
    bool TargetsExist()
    { return Test->TestTrue(Label(TEXT("all direct and wave-only damage targets spawn")), Target.IsValid() && Second.IsValid() && Distant.IsValid()); }
    void ResetAbility(AMistworldPlayerController* PC, ACharacter* Hero, UMistCombatHealth* Health)
    {
        for (TWeakObjectPtr<AMistTrainingDummy>* Handle : {&Target, &Second, &Distant})
        { if (Handle->IsValid()) Handle->Get()->Destroy(); Handle->Reset(); }
        FMistAscensionTestAccess::Reset(PC);
        PlaceHero(Hero, Origin);
        Health->Armor = Health->MagicResistance = 0.f;
    }
    bool MakeFixture(UWorld* World, AMistworldPlayerController* PC, ACharacter* Hero)
    {
        UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
        FNavLocation NavPoint;
        if (!Nav || !Nav->ProjectPointToNavigation(Origin, NavPoint, FVector(150.f, 150.f, 300.f)))
        { Test->AddError(TEXT("Placed sentinel home has no navigation for the ascension fixture.")); return false; }
        FCollisionObjectQueryParams Objects; Objects.AddObjectTypesToQuery(ECC_WorldStatic);
        FCollisionQueryParams Query(SCENE_QUERY_STAT(MistAscensionFixture), false, Hero);
        FHitResult Floor;
        if (!World->LineTraceSingleByObjectType(Floor, NavPoint.Location + FVector(0.f, 0.f, 200.f),
            NavPoint.Location - FVector(0.f, 0.f, 200.f), Objects, Query))
        { Test->AddError(TEXT("Ascension fixture cannot trace the actual arena floor.")); return false; }
        Origin = FVector(NavPoint.Location.X, NavPoint.Location.Y,
            Floor.ImpactPoint.Z + Hero->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 2.f);
        PlaceHero(Hero, Origin);
        for (int32 Angle = 0; Angle < 360; Angle += 30)
        {
            const FVector TryDirection = FVector::ForwardVector.RotateAngleAxis(Angle, FVector::UpVector);
            FVector End;
            if (FMistAscensionTestAccess::Endpoint(PC, TryDirection, 800.f, End)
                && FVector::Dist2D(Origin, End) >= 795.f
                && !World->LineTraceTestByChannel(Origin, Origin + TryDirection * 1050.f, ECC_Visibility, Query))
            {
                Direction = TryDirection;
                Test->AddInfo(FString::Printf(TEXT("Ascension matrix fixture origin %s, direction %s."), *Origin.ToString(), *Direction.ToString()));
                return true;
            }
        }
        Test->AddError(TEXT("Ascension fixture needs a full eight-metre dash route and unobstructed sword path."));
        return false;
    }
    FAutomationTestBase* Test;
    int32 Mask = 0, Stage = 0;
    double Started = -1., StageAt = 0., RDeadline = 0., WDeadline = 0.;
    FVector Checkpoint, Origin = FVector(400.f, 300.f, 100.f), Direction;
    TWeakObjectPtr<AMistTrainingDummy> Target, Second, Distant;
};

class FMistVerifyAscensionEdges final : public IAutomationLatentCommand
{
public:
    explicit FMistVerifyAscensionEdges(FAutomationTestBase* InTest) : Test(InTest) {}
    bool Update() override
    {
        if (Started < 0.) Started = FPlatformTime::Seconds();
        if (FPlatformTime::Seconds() - Started > 70.)
        { Test->AddError(FString::Printf(TEXT("Ascension interaction checks timed out at stage %d."), Stage)); return true; }
        UWorld* World = GEditor ? GEditor->PlayWorld : nullptr;
        AMistworldPlayerController* PC = World ? Cast<AMistworldPlayerController>(World->GetFirstPlayerController()) : nullptr;
        ACharacter* Hero = PC ? Cast<ACharacter>(PC->GetPawn()) : nullptr;
        UMistCombatHealth* Health = Hero ? Hero->FindComponentByClass<UMistCombatHealth>() : nullptr;
        if (!World || !World->HasBegunPlay() || !Health) return false;
        const double Now = World->GetTimeSeconds();
        switch (Stage)
        {
        case 0:
        {
            for (TActorIterator<AMistArenaEnemy> It(World); It; ++It) Origin = It->GetHomeLocation();
            for (TActorIterator<AMistTrainingDummy> It(World); It; ++It) It->Destroy();
            Checkpoint = FMistAscensionTestAccess::Checkpoint(PC).GetLocation();
            FMistAscensionTestAccess::Reset(PC);
            if (!FindFixture(World, PC, Hero)) return true;
            PlaceHero(Hero, Checkpoint);
            if (!PC->SetAbyssUnlocked(true) || !PC->TryEquipAbility(EKeys::Q, true) || !PC->TryEquipAbility(EKeys::E, true))
            { Test->AddError(TEXT("The interaction fixture cannot equip Abyss Q/E at its checkpoint.")); return true; }
            PlaceHero(Hero, Origin);
            Target = SpawnTarget(World, Origin + Direction * 600.f);
            if (!Test->TestNotNull(TEXT("Abyss Q wave-only target spawns"), Target.Get())) return true;
            RDeadline = Now + 60.; FMistAscensionTestAccess::SeedR(PC, RDeadline);
            FMistAscensionTestAccess::Commit(PC, EKeys::Q, Direction);
            Stage = 1; StageAt = Now; return false;
        }
        case 1:
            if (Now - StageAt < 1.2 || PC->IsCombatBusy()) return false;
            Test->TestEqual(TEXT("An Elite hit only by Abyss Q's travelling wave takes its 80 magic damage"), Target->CombatHealth->Health, 1120.f, 0.1f);
            Test->TestEqual(TEXT("An Elite hit only by the shockwave does not refund the current ultimate"), FMistAscensionTestAccess::RDeadline(PC), RDeadline, 0.01);
            Target->Destroy();
            FMistAscensionTestAccess::Reset(PC); PlaceHero(Hero, Origin);
            Health->Armor = 0.f; Health->Health = 1000.f;
            RDeadline = Now + 60.; FMistAscensionTestAccess::SeedR(PC, RDeadline);
            FMistAscensionTestAccess::Commit(PC, EKeys::E, Direction);
            Health->SetShield(200.f, 5.f);
            Test->TestEqual(TEXT("Abyss guard applies before a shield absorbs the hit"), Health->ReceivePhysicalDamage(200.f), 0.f);
            Test->TestEqual(TEXT("The guard's shield absorbs only the post-mitigation 134 damage"), Health->GetShieldRemainingHealth(), 66.f, 0.05f);
            Test->TestEqual(TEXT("Shield loss never advances the Abyss E refund counter"), FMistAscensionTestAccess::GuardRemainder(PC), 0.f);
            Test->TestEqual(TEXT("Shield-only damage never refunds R"), FMistAscensionTestAccess::RDeadline(PC), RDeadline);
            Test->TestEqual(TEXT("A hit that depletes a shield reports only one lost HP"), Health->ReceivePhysicalDamage(100.f), 1.f, 0.05f);
            Test->TestEqual(TEXT("Only the one actual HP contributes to E"), FMistAscensionTestAccess::GuardRemainder(PC), 1.f, 0.05f);
            Health->SetShield(0.f, 0.f);
            Test->TestEqual(TEXT("The next 150 raw damage removes 100.5 HP through Abyss E"), Health->ReceivePhysicalDamage(150.f), 100.5f, 0.05f);
            Test->TestEqual(TEXT("101.5 accumulated lost HP refund exactly one real second"), FMistAscensionTestAccess::RDeadline(PC), RDeadline - 1., 0.01);
            Test->TestEqual(TEXT("Fractional loss survives the refund"), FMistAscensionTestAccess::GuardRemainder(PC), 1.5f, 0.05f);
            Health->GrantInvulnerability(3.f);
            Test->TestEqual(TEXT("Immune damage does not enter Abyss E's actual-loss event"), Health->ReceivePhysicalDamage(100000.f), 0.f);
            Test->TestEqual(TEXT("Immunity cannot farm E refunds"), FMistAscensionTestAccess::RDeadline(PC), RDeadline - 1., 0.01);
            Test->TestEqual(TEXT("An immune hit leaves the fractional counter intact"), FMistAscensionTestAccess::GuardRemainder(PC), 1.5f, 0.05f);
            Health->ClearInvulnerability(); Health->Health = 4.f;
            FMistAscensionTestAccess::SetGuardRemainder(PC, 96.f);
            Test->TestEqual(TEXT("A lethal overkill hit reports the remaining four HP only"), Health->ReceivePhysicalDamage(100000.f), 4.f, 0.01f);
            Test->TestEqual(TEXT("The lethal hit supplies one refund before defeat, without counting overkill"), FMistAscensionTestAccess::RDeadline(PC), RDeadline - 2., 0.01);
            Test->TestEqual(TEXT("Defeat discards Abyss E's expired fractional state"), FMistAscensionTestAccess::GuardRemainder(PC), 0.f);
            Test->TestTrue(TEXT("Lethal E fixture enters the normal respawn flow"), PC->IsRespawning());
            FMistAscensionTestAccess::Reset(PC); FMistAscensionTestAccess::Reset(PC);
            PlaceHero(Hero, Origin); Health->Armor = 0.f;
            RDeadline = Now + 60.; FMistAscensionTestAccess::SeedR(PC, RDeadline);
            FMistAscensionTestAccess::Commit(PC, EKeys::E, Direction);
            Health->ReceivePhysicalDamage(60.f);
            Test->TestEqual(TEXT("Fresh Abyss E records a 40.2 HP partial threshold"), FMistAscensionTestAccess::GuardRemainder(PC), 40.2f, 0.05f);
            Stage = 2; StageAt = Now; return false;
        case 2:
        {
            if (Now - StageAt < 6.2) return false;
            Test->TestEqual(TEXT("Abyss guard expires after six seconds"), PC->GetDefenseRemaining(), 0.f);
            Test->TestEqual(TEXT("Guard expiry discards the unused fractional refund"), FMistAscensionTestAccess::GuardRemainder(PC), 0.f);
            Health->ReceivePhysicalDamage(100.f);
            Test->TestEqual(TEXT("Damage after guard expiry does not refund R"), FMistAscensionTestAccess::RDeadline(PC), RDeadline, 0.01);
            FMistAscensionTestAccess::Reset(PC); PlaceHero(Hero, Origin); Health->Health = 500.f;
            Target = SpawnTarget(World, Origin + Direction * 200.f);
            if (!Test->TestNotNull(TEXT("Overlapping trail target spawns"), Target.Get())) return true;
            Target->CombatHealth->MagicResistance = 20.f;
            NormalTrail = World->SpawnActor<AMistAbyssTrail>();
            EmpoweredTrail = World->SpawnActor<AMistAbyssTrail>();
            if (!Test->TestTrue(TEXT("Ordinary and ultimate-attributed trail actors spawn"), NormalTrail.IsValid() && EmpoweredTrail.IsValid())) return true;
            for (AMistAbyssTrail* Trail : {NormalTrail.Get(), EmpoweredTrail.Get()})
            {
                Trail->Initialize(Hero, Trail == EmpoweredTrail.Get());
                Trail->AddSegment(Origin + Direction * 100.f, Origin + Direction * 300.f);
                Trail->AddSegment(Origin + Direction * 140.f, Origin + Direction * 260.f);
                Trail->AddSegment(Origin + Direction * 180.f, Origin + Direction * 220.f);
                Trail->FinishAddingSegments();
            }
            Test->TestFalse(TEXT("Normal W trail has no ultimate healing attribution"), NormalTrail->HasUltimateHealing());
            Test->TestTrue(TEXT("Empowered W trail carries permanent originating-ultimate attribution"), EmpoweredTrail->HasUltimateHealing());
            Test->TestEqual(TEXT("An ordinary Abyss trail uses the 1.5-metre total width"), NormalTrail->GetTrailRadius(), 75.f);
            Test->TestEqual(TEXT("An empowered Abyss trail uses the two-metre total width"), EmpoweredTrail->GetTrailRadius(), 100.f);
            Stage = 3; StageAt = Now; return false;
        }
        case 3:
            if (Now - StageAt < 0.6) return false;
            Test->TestEqual(TEXT("Trails do not deal immediate or half-second damage"), Target->CombatHealth->Health, 1200.f);
            Test->TestEqual(TEXT("Trails cannot supply healing before their first scheduled tick"), Health->Health, 500.f);
            Stage = 4; return false;
        case 4:
            if (Now - StageAt < 1.25) return false;
            Test->TestEqual(TEXT("Three overlapping segments per cast still total one normal and one empowered tick"), Target->CombatHealth->Health, 1200.f - 50.f / 1.2f, 0.1f);
            Test->TestEqual(TEXT("Only the empowered tick heals, using actual post-resistance health loss"), Health->Health, 505.f, 0.1f);
            Test->TestEqual(TEXT("Ground-effect healing does not depend on a live recast window"), PC->GetUltimateRecastRemaining(), 0.f);
            Health->ReceivePhysicalDamage(100000.f);
            Test->TestFalse(TEXT("A caster's death permanently disables the old trail's healing attribution"), EmpoweredTrail->HasUltimateHealing());
            Test->TestFalse(TEXT("Already released trails survive the caster's death"), EmpoweredTrail->IsActorBeingDestroyed());
            if (!Test->TestTrue(TEXT("The same pawn can revive while its old trails remain"), FMistAscensionTestAccess::Revive(PC))) return true;
            Health->Health = 500.f;
            Stage = 5; return false;
        case 5:
            if (Now - StageAt < 2.25) return false;
            Test->TestEqual(TEXT("Both trails continue their second damaging tick after caster death and revival"), Target->CombatHealth->Health, 1200.f - 100.f / 1.2f, 0.1f);
            Test->TestEqual(TEXT("A pre-death empowered trail never resumes healing the revived life"), Health->Health, 500.f, 0.01f);
            Target->CombatHealth->GrantInvulnerability(1.1f);
            Stage = 6; return false;
        case 6:
            if (Now - StageAt < 3.25) return false;
            Test->TestEqual(TEXT("An immune scheduled trail tick contributes zero HP damage"), Target->CombatHealth->Health, 1200.f - 100.f / 1.2f, 0.1f);
            Test->TestEqual(TEXT("An immune trail tick supplies zero healing"), Health->Health, 500.f, 0.01f);
            Target->CombatHealth->ClearInvulnerability();
            Stage = 7; return false;
        case 7:
            if (Now - StageAt < 4.25) return false;
            Test->TestEqual(TEXT("The ordinary trail delivers its final fourth tick before expiring"), Target->CombatHealth->Health, 1200.f - 150.f / 1.2f, 0.1f);
            Test->TestTrue(TEXT("The four-second ordinary trail expires while the empowered one remains"), !NormalTrail.IsValid() || NormalTrail->IsActorBeingDestroyed());
            Stage = 8; return false;
        case 8:
            if (Now - StageAt < 5.25) return false;
            Test->TestEqual(TEXT("Only the empowered trail supplies its final fifth 30-damage tick"), Target->CombatHealth->Health, 1200.f - 180.f / 1.2f, 0.1f);
            Test->TestTrue(TEXT("The empowered trail expires after its final fifth tick"), !EmpoweredTrail.IsValid() || EmpoweredTrail->IsActorBeingDestroyed());
            Target->Destroy(); FMistAscensionTestAccess::Reset(PC);
            PlaceHero(Hero, Checkpoint);
            if (!Test->TestTrue(TEXT("Abyss R can be equipped after both damaging ground effects expire"), PC->TryEquipAbility(EKeys::R, true))) return true;
            PlaceHero(Hero, Origin); Health->Health = 500.f;
            Target = SpawnTarget(World, Origin + Direction * 430.f);
            Outside = SpawnTarget(World, Origin + Direction * 470.f);
            if (!Test->TestTrue(TEXT("Warcry boundary fixtures spawn"), Target.IsValid() && Outside.IsValid())) return true;
            FMistAscensionTestAccess::Commit(PC, EKeys::R, Direction);
            Test->TestEqual(TEXT("Warcry does not damage its target before the cast releases"), Target->CombatHealth->Health, 1200.f);
            Stage = 9; StageAt = Now; return false;
        case 9:
            if (Now - StageAt < 0.6 || PC->IsCombatBusy()) return false;
            Test->TestEqual(TEXT("Warcry intersects a capsule whose centre is just beyond its radius"), Target->CombatHealth->Health, 940.f, 0.1f);
            Test->TestEqual(TEXT("Warcry misses a capsule wholly beyond its four-metre radius"), Outside->CombatHealth->Health, 1200.f);
            Test->TestEqual(TEXT("Warcry's boundary hit supplies no healing"), Health->Health, 500.f);
            Stage = 10; return false;
        case 10:
            if (Now - StageAt < 10.7) return false;
            Test->TestEqual(TEXT("Warcry's ten-second buff expires after its release"), PC->GetWarcryRemaining(), 0.f);
            Test->TestEqual(TEXT("Warcry expiry restores the five-metre constant walk speed"), Hero->GetCharacterMovement()->MaxWalkSpeed, 500.f, 0.05f);
            Test->TestEqual(TEXT("Warcry expiry restores the ordinary auto interval multiplier"), FMistAscensionTestAccess::AttackMultiplier(PC), 1.f);
            PlaceHero(Hero, Checkpoint);
            Test->TestTrue(TEXT("Expired warcry and ten quiet seconds permit a real R swap"), PC->TryEquipAbility(EKeys::R, false));
            Target->Destroy(); Outside->Destroy(); FMistAscensionTestAccess::Reset(PC); return true;
        }
        return false;
    }
private:
    bool FindFixture(UWorld* World, AMistworldPlayerController* PC, ACharacter* Hero)
    {
        UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
        FNavLocation NavPoint;
        if (!Nav || !Nav->ProjectPointToNavigation(Origin, NavPoint, FVector(150.f, 150.f, 300.f)))
        { Test->AddError(TEXT("Ascension interaction fixture has no projected navigation.")); return false; }
        FCollisionObjectQueryParams Objects; Objects.AddObjectTypesToQuery(ECC_WorldStatic);
        FCollisionQueryParams Query(SCENE_QUERY_STAT(MistAscensionInteractionFixture), false, Hero);
        FHitResult Floor;
        if (!World->LineTraceSingleByObjectType(Floor, NavPoint.Location + FVector(0.f, 0.f, 200.f),
            NavPoint.Location - FVector(0.f, 0.f, 200.f), Objects, Query))
        { Test->AddError(TEXT("Ascension interaction fixture has no actual floor.")); return false; }
        Origin = FVector(NavPoint.Location.X, NavPoint.Location.Y,
            Floor.ImpactPoint.Z + Hero->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 2.f);
        PlaceHero(Hero, Origin);
        for (int32 Angle = 0; Angle < 360; Angle += 30)
        {
            const FVector TryDirection = FVector::ForwardVector.RotateAngleAxis(Angle, FVector::UpVector);
            FVector End;
            if (FMistAscensionTestAccess::Endpoint(PC, TryDirection, 800.f, End)
                && FVector::Dist2D(Origin, End) >= 795.f
                && !World->LineTraceTestByChannel(Origin, Origin + TryDirection * 850.f, ECC_Visibility, Query))
            { Direction = TryDirection; return true; }
        }
        Test->AddError(TEXT("Ascension interaction fixture cannot find a clear trail and wave route.")); return false;
    }
    FAutomationTestBase* Test;
    int32 Stage = 0;
    double Started = -1., StageAt = 0., RDeadline = 0.;
    FVector Checkpoint, Origin = FVector(400.f, 300.f, 100.f), Direction;
    TWeakObjectPtr<AMistTrainingDummy> Target, Outside;
    TWeakObjectPtr<AMistAbyssTrail> NormalTrail, EmpoweredTrail;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMistAscensionLoadoutTest, "Mistworld.Ascension.LoadoutRules",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FMistAscensionLoadoutTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(TEXT("/Game/Mistworld/Maps/Lvl_CombatTest")));
    ADD_LATENT_AUTOMATION_COMMAND(FMistWaitForAscensionNavigation(this));
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(FMistVerifyLoadoutRules(this));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMistAscensionCombinationTest, "Mistworld.Ascension.All16Kits",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FMistAscensionCombinationTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(TEXT("/Game/Mistworld/Maps/Lvl_CombatTest")));
    ADD_LATENT_AUTOMATION_COMMAND(FMistWaitForAscensionNavigation(this));
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(FMistVerifyAllAscensionKits(this));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMistAscensionInteractionTest, "Mistworld.Ascension.Interactions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FMistAscensionInteractionTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(TEXT("/Game/Mistworld/Maps/Lvl_CombatTest")));
    ADD_LATENT_AUTOMATION_COMMAND(FMistWaitForAscensionNavigation(this));
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(FMistVerifyAscensionEdges(this));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    return true;
}

#endif
