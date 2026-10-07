#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Tests/AutomationEditorCommon.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HAL/PlatformTime.h"
#include "Kismet/GameplayStatics.h"
#include "MistCombatHUD.h"
#include "MistCombatHealth.h"
#include "MistCryptGolem.h"
#include "MistCryptSession.h"
#include "MistProgressionComponent.h"
#include "MistworldPlayerController.h"

namespace
{
struct FHeroCryptJourney
{
    FString Slot = TEXT("Mistworld.Test.HeroCrypt.") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    TArray<float> AlphonseCooldowns = { 26.f, 32.f, 45.f, 180.f, 60.f, 3.f };
    TArray<float> AnimaCooldowns = { 18.f, 24.f, 35.f, 110.f, 60.f, 2.f };
    TArray<float> SavedAnimaCooldowns;
    FDateTime LastSavedUtc;
    float LastAlphonseR = 0.f;
    FDateTime LastAlphonseUtc;
    bool bSeeded = false;
    bool bFirstVisitComplete = false;
};

// This fixture only uses a new Mistworld.Test.* slot. It cannot select, read,
// overwrite or delete the player's Mistworld.Crypt.v1 file.
struct FHeroProfileFixture
{
    UWorld* World = nullptr;
    UMistProgressionComponent* Progress = nullptr;
    explicit FHeroProfileFixture(const FString& Slot)
    {
        const UWorld::InitializationValues Init = UWorld::InitializationValues()
            .InitializeScenes(false).AllowAudioPlayback(false).CreatePhysicsScene(false)
            .CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).CreateFXSystem(false);
        World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
            ERHIFeatureLevel::Num, &Init);
        AActor* Owner = World ? World->SpawnActor<AActor>() : nullptr;
        if (!Owner) return;
        Progress = NewObject<UMistProgressionComponent>(Owner);
        Owner->AddInstanceComponent(Progress);
        Progress->RegisterComponent();
        if (!Progress->SetSaveSlotForTesting(Slot)) Progress = nullptr;
    }
    ~FHeroProfileFixture() { if (World) World->DestroyWorld(false); }
};

TArray<float> ControllerCooldowns(const AMistworldPlayerController* PC)
{
    TArray<float> Result;
    for (FKey Key : {EKeys::Q, EKeys::W, EKeys::E, EKeys::R, EKeys::F, EKeys::SpaceBar})
        Result.Add(PC->GetCooldownRemaining(Key));
    return Result;
}

bool ClickHero(FAutomationTestBase* Test, AMistworldPlayerController* PC, EMistHeroId Hero)
{
    auto* HUD = Cast<AMistCombatHUD>(PC->GetHUD());
    if (!Test->TestNotNull(TEXT("Production combat HUD owns the selector"), HUD)) return false;
    if (!PC->IsLoadoutMenuOpen()) PC->ToggleLoadoutMenu();
    Test->TestTrue(TEXT("L opens the character/loadout menu"), PC->IsLoadoutMenuOpen());
    HUD->NotifyHitBoxClick(Hero == EMistHeroId::Anima ? FName(TEXT("Mist.Hero.Anima")) : FName(TEXT("Mist.Hero.Alphonse")));
    return Test->TestTrue(FString::Printf(TEXT("L character button selects %s"), *GetMistHeroDisplayName(Hero)), PC->GetActiveHero() == Hero);
}

class FSeedHeroCryptJourney final : public IAutomationLatentCommand
{
public:
    FSeedHeroCryptJourney(FAutomationTestBase* InTest, TSharedRef<FHeroCryptJourney> InJourney)
        : Test(InTest), Journey(InJourney) {}
    bool Update() override
    {
        FHeroProfileFixture Fixture(Journey->Slot);
        if (!Test->TestNotNull(TEXT("Isolated hero-crypt save fixture"), Fixture.Progress)) return true;
        UMistProgressionComponent* Progress = Fixture.Progress;
        Test->TestTrue(TEXT("Seed completed crypt for legitimate reward checkpoint access"), Progress->SetCryptCompleted());
        Test->TestTrue(TEXT("Seed Alphonse ascension"), Progress->SetAbyssUnlocked(true));
        Test->TestTrue(TEXT("Seed Alphonse mixed Q/R"), Progress->SetLoadoutMask(9));
        Test->TestTrue(TEXT("Seed Alphonse independent cooldowns"), Progress->SetCooldowns(Journey->AlphonseCooldowns));
        Test->TestTrue(TEXT("Earn account reward in isolated fixture"), Progress->ClaimCryptReward());
        Test->TestTrue(TEXT("Equip only Alphonse's owned blade"), Progress->EquipReward(true));
        Test->TestTrue(TEXT("Select Anima's profile for independent cooldown seed"), Progress->SetActiveHero(EMistHeroId::Anima));
        Test->TestTrue(TEXT("Seed Anima independent cooldowns"), Progress->SetCooldowns(Journey->AnimaCooldowns));
        Test->TestTrue(TEXT("Return seeded startup hero to Alphonse"), Progress->SetActiveHero(EMistHeroId::Alphonse));
        Journey->bSeeded = Test->TestTrue(TEXT("Persist both profiles before the production controller starts"), Progress->SaveProgression());
        return true;
    }
private:
    FAutomationTestBase* Test;
    TSharedRef<FHeroCryptJourney> Journey;
};

class FHeroCryptFirstVisit final : public IAutomationLatentCommand
{
public:
    FHeroCryptFirstVisit(FAutomationTestBase* InTest, TSharedRef<FHeroCryptJourney> InJourney)
        : Test(InTest), Journey(InJourney) {}
    bool Update() override
    {
        if (!Journey->bSeeded) return true;
        if (Started < 0.0) Started = FPlatformTime::Seconds();
        UWorld* World = GEditor ? GEditor->PlayWorld : nullptr;
        auto* PC = World ? Cast<AMistworldPlayerController>(World->GetFirstPlayerController()) : nullptr;
        auto* Hero = PC ? Cast<ACharacter>(PC->GetPawn()) : nullptr;
        auto* Health = Hero ? Hero->FindComponentByClass<UMistCombatHealth>() : nullptr;
        AMistCryptSession* Session = PC ? PC->GetCryptSession() : nullptr;
        UMistProgressionComponent* Progress = PC ? PC->GetProgression() : nullptr;
        if (!World || !World->HasBegunPlay() || !PC || !Hero || !Health || !Progress || !Session
            || !Cast<AMistCombatHUD>(PC->GetHUD()) || !Session->GetGolem() || !Session->GetGolem()->IsEncounterComplete())
        {
            if (FPlatformTime::Seconds() - Started > 30.0)
            { Test->AddError(TEXT("Hero-crypt first visit could not initialize the production map within 30 seconds.")); return true; }
            return false;
        }
        Test->TestFalse(TEXT("Production crypt starts with saved Alphonse"), PC->IsAnima());
        Test->TestEqual(TEXT("First visit starts at saved entrance"), PC->GetActiveCheckpointID(), FName(TEXT("Crypt.Entrance")));
        Test->TestTrue(TEXT("Entrance circle permits character switching"), PC->IsNearCheckpoint() && PC->GetCharacterSwitchLockReason().IsEmpty());
        Health->EndSpawnProtection();
        Health->Health = 840.f;
        const float AlphonseR = PC->GetCooldownRemaining(EKeys::R);
        if (!ClickHero(Test, PC, EMistHeroId::Anima)) return true;
        Test->TestTrue(TEXT("Anima profile is selected in the progression component"), Progress->GetActiveHero() == EMistHeroId::Anima);
        Test->TestEqual(TEXT("Anima has 1000 HP, with no inherited blade multiplier"), Health->MaxHealth, 1000.f);
        Test->TestEqual(TEXT("Anima does not receive Alphonse's ability bonus"), Health->AbilityDamageMultiplier, 1.f, .001f);
        Test->TestFalse(TEXT("Character change does not create spawn immunity"), Health->IsSpawnProtected());
        Test->TestFalse(TEXT("Alphonse's ascension does not unlock Anima's"), PC->IsAbyssUnlocked());
        auto* HUD = Cast<AMistCombatHUD>(PC->GetHUD());
        HUD->NotifyHitBoxClick(TEXT("Mist.Unlock"));
        Test->TestTrue(TEXT("Anima's test-access button unlocks Flamestealer"), PC->IsAbyssUnlocked());
        HUD->NotifyHitBoxClick(TEXT("Mist.Abyss.1"));
        HUD->NotifyHitBoxClick(TEXT("Mist.Abyss.2"));
        Test->TestTrue(TEXT("Anima mixed W/E slots are selected through the production menu"), PC->IsAbyssEquipped(EKeys::W) && PC->IsAbyssEquipped(EKeys::E));
        Test->TestFalse(TEXT("Anima original Q remains independent"), PC->IsAbyssEquipped(EKeys::Q));
        Test->TestFalse(TEXT("Anima original R remains independent"), PC->IsAbyssEquipped(EKeys::R));
        Health->Health = 400.f;
        if (!ClickHero(Test, PC, EMistHeroId::Alphonse)) return true;
        Test->TestEqual(TEXT("Real crypt switch retains Alphonse's injured HP"), Health->Health, 840.f, .1f);
        Test->TestTrue(TEXT("Real crypt switch retains Alphonse's independent R cooldown"), PC->GetCooldownRemaining(EKeys::R) <= AlphonseR + .1f && PC->GetCooldownRemaining(EKeys::R) >= AlphonseR - 3.f);
        Test->TestTrue(TEXT("Alphonse mixed Q/R are restored"), PC->IsAbyssEquipped(EKeys::Q) && PC->IsAbyssEquipped(EKeys::R));
        Test->TestFalse(TEXT("Anima's alternate W does not leak into Alphonse"), PC->IsAbyssEquipped(EKeys::W));
        Test->TestEqual(TEXT("Alphonse's earned blade multiplier returns"), Health->AbilityDamageMultiplier, 1.05f, .001f);
        if (PC->IsLoadoutMenuOpen()) PC->ToggleLoadoutMenu();
        Hero->GetCharacterMovement()->StopMovementImmediately();
        const FTransform Reward = Session->GetCheckpointTransform(TEXT("Crypt.Reward"));
        if (!Test->TestTrue(TEXT("Production hero can stand on the reward checkpoint"), Hero->TeleportTo(Reward.GetLocation(), Reward.Rotator(), false, false))) return true;
        Test->TestTrue(TEXT("Reward circle permits character switching"), PC->IsNearCheckpoint() && PC->GetCharacterSwitchLockReason().IsEmpty());
        Test->TestTrue(TEXT("Reward checkpoint activates and saves"), PC->TryActivateCryptCheckpoint(TEXT("Crypt.Reward"), Reward));
        Journey->LastAlphonseR = PC->GetCooldownRemaining(EKeys::R);
        Journey->LastAlphonseUtc = FDateTime::UtcNow();
        if (!ClickHero(Test, PC, EMistHeroId::Anima)) return true;
        Test->TestEqual(TEXT("Reward-circle change preserves Anima's injured HP"), Health->Health, 400.f, .1f);
        Test->TestTrue(TEXT("Reward-circle change restores Anima mixed W/E"), PC->IsAbyssEquipped(EKeys::W) && PC->IsAbyssEquipped(EKeys::E));
        Test->TestEqual(TEXT("Hero switching does not erase active reward checkpoint"), PC->GetActiveCheckpointID(), FName(TEXT("Crypt.Reward")));
        Test->TestFalse(TEXT("Anima cannot equip Alphonse's blade from controller API"), PC->TryEquipCryptReward(true));
        Test->TestFalse(TEXT("Anima cannot clear Alphonse's equipped blade"), PC->TryEquipCryptReward(false));
        Test->TestTrue(TEXT("Shared reward remains owned"), Progress->HasReward());
        Test->TestFalse(TEXT("Character switching cannot duplicate shared reward"), Progress->ClaimCryptReward());
        if (PC->IsLoadoutMenuOpen()) PC->ToggleLoadoutMenu();
        Journey->SavedAnimaCooldowns = ControllerCooldowns(PC);
        Journey->LastSavedUtc = FDateTime::UtcNow();
        Journey->bFirstVisitComplete = Test->TestTrue(TEXT("Save active Anima at reward checkpoint"), PC->SaveCryptProgression());
        // Re-read, without saving, to verify both profiles and blade ownership on disk.
        FHeroProfileFixture Disk(Journey->Slot);
        if (!Test->TestNotNull(TEXT("Disk verification uses isolated progression fixture"), Disk.Progress)) return true;
        Test->TestTrue(TEXT("Controller save writes a valid versioned payload"), Disk.Progress->LoadProgression());
        Test->TestTrue(TEXT("Disk selects active Anima"), Disk.Progress->GetActiveHero() == EMistHeroId::Anima);
        Test->TestEqual(TEXT("Disk stores Anima mixed W/E"), Disk.Progress->GetLoadoutMask(), static_cast<uint8>(6));
        Test->TestTrue(TEXT("Disk retains Anima test ascension unlock"), Disk.Progress->IsAbyssUnlocked());
        Test->TestTrue(TEXT("Disk permits reading inactive Alphonse profile"), Disk.Progress->SetActiveHero(EMistHeroId::Alphonse));
        Test->TestEqual(TEXT("Disk retains Alphonse Q/R mask"), Disk.Progress->GetLoadoutMask(), static_cast<uint8>(9));
        Test->TestTrue(TEXT("Disk retains Alphonse's equipped earned blade"), Disk.Progress->IsRewardEquipped());
        return true;
    }
private:
    FAutomationTestBase* Test;
    TSharedRef<FHeroCryptJourney> Journey;
    double Started = -1.0;
};

class FHeroCryptReloadVisit final : public IAutomationLatentCommand
{
public:
    FHeroCryptReloadVisit(FAutomationTestBase* InTest, TSharedRef<FHeroCryptJourney> InJourney)
        : Test(InTest), Journey(InJourney) {}
    bool Update() override
    {
        if (!Journey->bFirstVisitComplete) return true;
        if (Started < 0.0) Started = FPlatformTime::Seconds();
        UWorld* World = GEditor ? GEditor->PlayWorld : nullptr;
        auto* PC = World ? Cast<AMistworldPlayerController>(World->GetFirstPlayerController()) : nullptr;
        auto* Hero = PC ? Cast<ACharacter>(PC->GetPawn()) : nullptr;
        auto* Health = Hero ? Hero->FindComponentByClass<UMistCombatHealth>() : nullptr;
        auto* Session = PC ? PC->GetCryptSession() : nullptr;
        if (!World || !World->HasBegunPlay() || !PC || !Hero || !Health || !Session || !PC->GetProgression() || !Cast<AMistCombatHUD>(PC->GetHUD()))
        {
            if (FPlatformTime::Seconds() - Started > 30.0)
            { Test->AddError(TEXT("Hero-crypt reload could not initialize within 30 seconds.")); return true; }
            return false;
        }
        Test->TestTrue(TEXT("Fresh controller restores active Anima"), PC->IsAnima());
        Test->TestEqual(TEXT("Fresh controller restores reward checkpoint"), PC->GetActiveCheckpointID(), FName(TEXT("Crypt.Reward")));
        Test->TestTrue(TEXT("Fresh controller spawns on saved reward circle"), FVector::Dist2D(Hero->GetActorLocation(), Session->GetCheckpointTransform(TEXT("Crypt.Reward")).GetLocation()) < 20.f);
        Test->TestEqual(TEXT("Fresh spawn has Anima's full 1000-HP baseline"), Health->Health, 1000.f, .1f);
        Test->TestTrue(TEXT("Fresh spawn retains normal spawn protection"), Health->IsSpawnProtected());
        Test->TestTrue(TEXT("Fresh controller restores Anima unlock and mixed W/E"), PC->IsAbyssUnlocked() && PC->IsAbyssEquipped(EKeys::W) && PC->IsAbyssEquipped(EKeys::E));
        Test->TestFalse(TEXT("Fresh controller does not replace original Anima Q"), PC->IsAbyssEquipped(EKeys::Q));
        Test->TestEqual(TEXT("Fresh Anima still receives no greatsword bonus"), Health->AbilityDamageMultiplier, 1.f, .001f);
        const double Elapsed = (FDateTime::UtcNow() - Journey->LastSavedUtc).GetTotalSeconds();
        const TArray<float> Restored = ControllerCooldowns(PC);
        for (int32 I = 0; I < Restored.Num(); ++I)
        {
            Test->TestTrue(FString::Printf(TEXT("Restart does not replenish Anima cooldown %d"), I), Restored[I] <= Journey->SavedAnimaCooldowns[I] + .1f);
            Test->TestTrue(FString::Printf(TEXT("Restart does not reset Anima cooldown %d"), I), Restored[I] >= FMath::Max(0.0, Journey->SavedAnimaCooldowns[I] - Elapsed) - 2.0);
        }
        if (!ClickHero(Test, PC, EMistHeroId::Alphonse)) return true;
        Test->TestTrue(TEXT("Reward circle also switches after restart"), !PC->IsAnima());
        Test->TestTrue(TEXT("Inactive Alphonse mixed Q/R survives restart"), PC->IsAbyssEquipped(EKeys::Q) && PC->IsAbyssEquipped(EKeys::R));
        const double AlphonseElapsed = (FDateTime::UtcNow() - Journey->LastAlphonseUtc).GetTotalSeconds();
        Test->TestTrue(TEXT("Inactive Alphonse R is not replaced with Anima's R or replenished"),
            PC->GetCooldownRemaining(EKeys::R) <= Journey->LastAlphonseR + .1f
            && PC->GetCooldownRemaining(EKeys::R) >= FMath::Max(0.0, Journey->LastAlphonseR - AlphonseElapsed) - 2.0);
        Test->TestEqual(TEXT("Inactive Alphonse equipment survives restart"), Health->AbilityDamageMultiplier, 1.05f, .001f);
        if (PC->IsLoadoutMenuOpen()) PC->ToggleLoadoutMenu();
        Hero->GetCharacterMovement()->StopMovementImmediately();
        const FTransform Entrance = Session->GetCheckpointTransform(TEXT("Crypt.Entrance"));
        if (!Test->TestTrue(TEXT("Restored hero can return to entrance checkpoint"), Hero->TeleportTo(Entrance.GetLocation(), Entrance.Rotator(), false, false))) return true;
        Test->TestTrue(TEXT("Entrance checkpoint can be selected after a reward-circle restart"), PC->TryActivateCryptCheckpoint(TEXT("Crypt.Entrance"), Entrance));
        if (!ClickHero(Test, PC, EMistHeroId::Anima)) return true;
        Test->TestTrue(TEXT("Entrance L selector works after restart"), PC->IsAnima());
        return true;
    }
private:
    FAutomationTestBase* Test;
    TSharedRef<FHeroCryptJourney> Journey;
    double Started = -1.0;
};

class FAwaitHeroCryptPIEStopped final : public IAutomationLatentCommand
{
public:
    explicit FAwaitHeroCryptPIEStopped(FAutomationTestBase* InTest) : Test(InTest) {}
    bool Update() override
    {
        if (!GEditor || !GEditor->PlayWorld) return true;
        if (Started < 0.0) Started = FPlatformTime::Seconds();
        if (FPlatformTime::Seconds() - Started > 30.0)
        { Test->AddError(TEXT("Hero-crypt PIE teardown did not finish in 30 seconds.")); return true; }
        return false;
    }
private:
    FAutomationTestBase* Test;
    double Started = -1.0;
};

class FCleanupHeroCryptJourney final : public IAutomationLatentCommand
{
public:
    FCleanupHeroCryptJourney(FAutomationTestBase* InTest, TSharedRef<FHeroCryptJourney> InJourney)
        : Test(InTest), Journey(InJourney) {}
    bool Update() override
    {
        if (GEditor && GEditor->PlayWorld)
        {
            if (Started < 0.0) Started = FPlatformTime::Seconds();
            if (FPlatformTime::Seconds() - Started <= 30.0) return false;
            Test->AddError(TEXT("Hero-crypt cleanup retained its test slot because PIE remains active."));
            AMistworldPlayerController::AutomationSaveSlot.Empty();
            return true;
        }
        AMistworldPlayerController::AutomationSaveSlot.Empty();
        UGameplayStatics::DeleteGameInSlot(Journey->Slot, 0);
        return true;
    }
private:
    FAutomationTestBase* Test;
    TSharedRef<FHeroCryptJourney> Journey;
    double Started = -1.0;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMistHeroCryptSwitchTest, "Mistworld.Heroes.CryptSwitchPersistence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMistHeroCryptSwitchTest::RunTest(const FString& Parameters)
{
    const TSharedRef<FHeroCryptJourney> Journey = MakeShared<FHeroCryptJourney>();
    AMistworldPlayerController::AutomationSaveSlot = Journey->Slot;
    ADD_LATENT_AUTOMATION_COMMAND(FSeedHeroCryptJourney(this, Journey));
    ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(TEXT("/Game/Mistworld/Maps/Lvl_CryptSlice")));
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(FHeroCryptFirstVisit(this, Journey));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    ADD_LATENT_AUTOMATION_COMMAND(FAwaitHeroCryptPIEStopped(this));
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(FHeroCryptReloadVisit(this, Journey));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    ADD_LATENT_AUTOMATION_COMMAND(FAwaitHeroCryptPIEStopped(this));
    ADD_LATENT_AUTOMATION_COMMAND(FCleanupHeroCryptJourney(this, Journey));
    return true;
}

#endif
