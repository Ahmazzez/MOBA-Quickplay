#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Kismet/GameplayStatics.h"
#include "MistCryptSaveGame.h"
#include "MistProgressionComponent.h"
#include <limits>

struct FMistProgressionTestAccess
{
    static void FailWrites(UMistProgressionComponent* Progress, bool bFail)
    {
        Progress->bForceSaveFailureForTesting = bFail;
    }
    static bool WriteSave(UMistCryptSaveGame* Save, const FString& Slot)
    {
        return UMistProgressionComponent::WriteSaveEnvelope(Save, Slot);
    }
    static UMistCryptSaveGame* ReadSave(UMistProgressionComponent* Progress)
    {
        FString Reason;
        return Progress->ReadValidatedSave(Reason);
    }
    // Read only a known-good fixture envelope to inspect what the writer serialized.
    // Production reads always use the bounded/checksummed ReadValidatedSave path.
    static UMistCryptSaveGame* ReadFixturePayload(const FString& Slot)
    {
        TArray<uint8> Data;
        if (!UGameplayStatics::LoadDataFromSlot(Data, Slot, 0) || Data.Num() <= 16) return nullptr;
        TArray<uint8> Payload;
        Payload.Append(Data.GetData() + 16, Data.Num() - 16);
        return Cast<UMistCryptSaveGame>(UGameplayStatics::LoadGameFromMemory(Payload));
    }
};

namespace
{
    struct FCryptSaveFixture
    {
        UWorld* World = nullptr;
        FString Slot = TEXT("Mistworld.Test.") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
        FCryptSaveFixture()
        {
            const UWorld::InitializationValues Init = UWorld::InitializationValues()
                .InitializeScenes(false).AllowAudioPlayback(false).CreatePhysicsScene(false)
                .CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).CreateFXSystem(false);
            World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
                ERHIFeatureLevel::Num, &Init);
        }
        ~FCryptSaveFixture()
        {
            UGameplayStatics::DeleteGameInSlot(Slot, 0);
            if (World) World->DestroyWorld(false);
        }
        UMistProgressionComponent* CreateProgression() const
        {
            AActor* Owner = World ? World->SpawnActor<AActor>() : nullptr;
            if (!Owner) return nullptr;
            UMistProgressionComponent* Result = NewObject<UMistProgressionComponent>(Owner);
            Owner->AddInstanceComponent(Result);
            Result->RegisterComponent();
            Result->SetSaveSlotForTesting(Slot);
            return Result;
        }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMistCryptPersistenceTest, "Mistworld.Progression.CryptPersistence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMistCryptPersistenceTest::RunTest(const FString& Parameters)
{
    FCryptSaveFixture Fixture;
    UMistProgressionComponent* Progress = Fixture.CreateProgression();
    if (!TestNotNull(TEXT("Isolated progression component"), Progress)) return false;
    TestFalse(TEXT("A missing save starts a new journey"), Progress->LoadProgression());
    TestFalse(TEXT("Missing save grants no item"), Progress->HasReward());
    TestFalse(TEXT("Missing save does not unlock Abyss"), Progress->IsAbyssUnlocked());
    TestEqual(TEXT("Missing save starts at the entrance"), Progress->GetCheckpointID(), FName(TEXT("Crypt.Entrance")));
    TestFalse(TEXT("Cannot claim loot before completing the encounter"), Progress->ClaimCryptReward());
    TestFalse(TEXT("Cannot equip an unowned reward"), Progress->EquipReward(true));
    TestFalse(TEXT("Cannot use reward checkpoint before completing the encounter"), Progress->SetCheckpoint(TEXT("Crypt.Reward")));
    TestFalse(TEXT("Reject unknown checkpoints"), Progress->SetCheckpoint(TEXT("Crypt.Arbitrary")));
    TestFalse(TEXT("Cannot equip a locked Abyss slot"), Progress->SetLoadoutMask(1));
    TestFalse(TEXT("Reject undefined loadout bits"), Progress->SetLoadoutMask(16));
    TestFalse(TEXT("Tests cannot select the live player save slot"), Progress->SetSaveSlotForTesting(TEXT("Mistworld.Crypt.v1")));
    TestFalse(TEXT("Tests cannot escape the save directory"), Progress->SetSaveSlotForTesting(TEXT("Mistworld.Test.../escape")));
    UMistProgressionComponent* IncompleteStaleInstance = Fixture.CreateProgression();
    TestFalse(TEXT("Second pre-completion controller starts without a save"), IncompleteStaleInstance->LoadProgression());

    TestTrue(TEXT("Encounter completion is recorded"), Progress->SetCryptCompleted());
    TestTrue(TEXT("Abyss unlock can be recorded"), Progress->SetAbyssUnlocked(true));
    TestTrue(TEXT("Independent slots are recorded"), Progress->SetLoadoutMask(13));
    TestTrue(TEXT("Completed encounter permits reward checkpoint"), Progress->SetCheckpoint(TEXT("Crypt.Reward")));
    const TArray<float> InitialCooldowns = { 6.f, 12.f, 30.f, 120.f, 60.f, 1.f };
    TestTrue(TEXT("Six cooldown slots can be recorded"), Progress->SetCooldowns(InitialCooldowns));
    TestTrue(TEXT("Progression writes a real local SaveGame"), Progress->SaveProgression());
    UMistProgressionComponent* StaleInstance = Fixture.CreateProgression();
    TestTrue(TEXT("A pre-reward controller can restore its snapshot"), StaleInstance->LoadProgression());
    TestFalse(TEXT("Pre-reward snapshot owns no equipment"), StaleInstance->HasReward());
    TestTrue(TEXT("First reward claim is committed"), Progress->ClaimCryptReward());
    TestFalse(TEXT("Repeated reward claim is rejected"), Progress->ClaimCryptReward());
    TestFalse(TEXT("Claiming does not silently equip the reward"), Progress->IsRewardEquipped());
    TestTrue(TEXT("Reward is legal in Alphonse's greatsword blade slot"), Progress->EquipReward(true));
    TestEqual(TEXT("Equipped item grants exactly five percent ability damage"), Progress->GetAbilityDamageMultiplier(), 1.05f, 0.0001f);
    TestFalse(TEXT("Cannot revoke Abyss unlock while an alternate kit is equipped"), Progress->SetAbyssUnlocked(false));
    TestFalse(TEXT("Stale pre-reward autosave cannot erase a committed reward"), StaleInstance->SaveProgression());
    TestFalse(TEXT("Stale pre-completion autosave cannot erase a completed encounter"), IncompleteStaleInstance->SaveProgression());
    UMistCryptSaveGame* PreservedSave = FMistProgressionTestAccess::ReadSave(Progress);
    if (!TestNotNull(TEXT("Stale autosaves preserve the existing file"), PreservedSave)) return false;
    TestTrue(TEXT("Existing file retains encounter completion"), PreservedSave->bCryptCompleted);
    TestTrue(TEXT("Existing file retains the claimed reward"), PreservedSave->bRewardClaimed);
    TestEqual(TEXT("Existing file retains equipped reward"), PreservedSave->EquippedItemID, UMistProgressionComponent::GetRewardItemID());
    TestFalse(TEXT("A stale controller cannot claim a second reward"), StaleInstance->ClaimCryptReward());
    TestTrue(TEXT("Stale controller refreshes known ownership from the saved claim"), StaleInstance->HasReward());

    UMistCryptSaveGame* DiskSave = FMistProgressionTestAccess::ReadSave(Progress);
    if (!TestNotNull(TEXT("Real SaveGame payload restores through its validated envelope"), DiskSave)) return false;
    TestEqual(TEXT("Save schema is versioned"), DiskSave->FormatVersion, UMistCryptSaveGame::CurrentVersion);
    TestEqual(TEXT("Saved hero ownership is explicit"), DiskSave->HeroID, FName(TEXT("Alphonse")));
    TestEqual(TEXT("Saved blade slot is explicit"), DiskSave->WeaponSlotID, FName(TEXT("GreatswordBlade")));
    TestEqual(TEXT("Saved inventory uses a stable item ID"), DiskSave->OwnedItemID, UMistProgressionComponent::GetRewardItemID());
    TestEqual(TEXT("Saved equipment uses the same legal item ID"), DiskSave->EquippedItemID, UMistProgressionComponent::GetRewardItemID());
    TestEqual(TEXT("Cooldown save has Q/W/E/R/Flash/auto entries"), DiskSave->CooldownSeconds.Num(), 6);

    UMistProgressionComponent* Reloaded = Fixture.CreateProgression();
    TestTrue(TEXT("Restart restores the real saved progression"), Reloaded->LoadProgression());
    TestTrue(TEXT("Restart retains encounter completion"), Reloaded->IsCryptCompleted());
    TestTrue(TEXT("Restart retains exactly one reward"), Reloaded->HasReward());
    TestTrue(TEXT("Restart retains equipped item"), Reloaded->IsRewardEquipped());
    TestTrue(TEXT("Restart retains Abyss unlock"), Reloaded->IsAbyssUnlocked());
    TestEqual(TEXT("Restart retains mixed loadout"), Reloaded->GetLoadoutMask(), static_cast<uint8>(13));
    TestEqual(TEXT("Restart retains checkpoint ID"), Reloaded->GetCheckpointID(), FName(TEXT("Crypt.Reward")));
    TestFalse(TEXT("Restart cannot duplicate an already claimed reward"), Reloaded->ClaimCryptReward());
    const TArray<float> RestoredCooldowns = Reloaded->GetCooldowns();
    for (int32 Index = 0; Index < RestoredCooldowns.Num(); ++Index)
    {
        TestTrue(FString::Printf(TEXT("Cooldown %d cannot increase when reloaded"), Index), RestoredCooldowns[Index] <= InitialCooldowns[Index]);
        TestTrue(FString::Printf(TEXT("Cooldown %d is not reset by reloading"), Index), RestoredCooldowns[Index] >= InitialCooldowns[Index] - 5.f);
    }
    TestTrue(TEXT("Unequipping commits to disk"), Reloaded->EquipReward(false));
    TestEqual(TEXT("Unequipped reward grants no damage bonus"), Reloaded->GetAbilityDamageMultiplier(), 1.f, 0.0001f);
    TestTrue(TEXT("Another restart restores unequipped ownership"), Progress->LoadProgression());
    TestTrue(TEXT("Unequipped item remains owned"), Progress->HasReward());
    TestFalse(TEXT("Unequipped state survives restart"), Progress->IsRewardEquipped());

    // Advance only the persisted timestamp so this test need not sleep for cooldown expiry.
    DiskSave = FMistProgressionTestAccess::ReadSave(Progress);
    DiskSave->CooldownSeconds = { 10.f, 12.f, 30.f, 120.f, 60.f, 1.f };
    DiskSave->SavedUtcTicks = FDateTime::UtcNow().GetTicks() - 20 * ETimespan::TicksPerSecond;
    TestTrue(TEXT("Offline-time fixture is stored in a real save envelope"), FMistProgressionTestAccess::WriteSave(DiskSave, Fixture.Slot));
    TestTrue(TEXT("Offline-time fixture restores"), Reloaded->LoadProgression());
    const TArray<float> OfflineCooldowns = Reloaded->GetCooldowns();
    TestEqual(TEXT("Short Q expires while offline"), OfflineCooldowns[0], 0.f, 0.01f);
    TestEqual(TEXT("Short W expires while offline"), OfflineCooldowns[1], 0.f, 0.01f);
    TestEqual(TEXT("E subtracts elapsed offline seconds"), OfflineCooldowns[2], 10.f, 0.5f);
    TestEqual(TEXT("R subtracts elapsed offline seconds"), OfflineCooldowns[3], 100.f, 0.5f);
    TestEqual(TEXT("Flash subtracts elapsed offline seconds"), OfflineCooldowns[4], 40.f, 0.5f);
    TestEqual(TEXT("Auto-attack interval expires while offline"), OfflineCooldowns[5], 0.f, 0.01f);
    TestTrue(TEXT("Saving after reload preserves elapsed cooldowns"), Reloaded->SaveProgression());
    TestTrue(TEXT("Resaving cooldowns remains restorable"), Progress->LoadProgression());
    TestTrue(TEXT("Second reload cannot recover the original R cooldown"), Progress->GetCooldowns()[3] <= 100.f);
    DiskSave = FMistProgressionTestAccess::ReadSave(Progress);
    DiskSave->CooldownSeconds = InitialCooldowns;
    DiskSave->SavedUtcTicks = FDateTime::UtcNow().GetTicks() + 3600 * ETimespan::TicksPerSecond;
    TestTrue(TEXT("Backward-clock fixture writes"), FMistProgressionTestAccess::WriteSave(DiskSave, Fixture.Slot));
    TestTrue(TEXT("Backward-clock fixture restores safely"), Reloaded->LoadProgression());
    TestEqual(TEXT("A backwards computer clock never increases R cooldown"), Reloaded->GetCooldowns()[3], 120.f, 0.001f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMistEquipmentSlotsMigrationTest, "Mistworld.Progression.EquipmentSlotsMigration",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMistEquipmentSlotsMigrationTest::RunTest(const FString& Parameters)
{
    FCryptSaveFixture Fixture;
    UMistProgressionComponent* Progress = Fixture.CreateProgression();
    if (!TestNotNull(TEXT("Equipment migration fixture"), Progress)) return false;
    const TArray<EMistEquipmentSlot>& Slots = UMistProgressionComponent::GetEquipmentSlots();
    const TArray<FName> IDs = { TEXT("GreatswordBlade"), TEXT("GreatswordGuard"),
        TEXT("HeavyHelmet"), TEXT("HeavyChestplate"), TEXT("HeavyBoots") };
    const TArray<FString> Labels = { TEXT("Greatsword Blade"), TEXT("Greatsword Guard"),
        TEXT("Heavy Helmet"), TEXT("Heavy Chestplate"), TEXT("Heavy Boots") };
    if (!TestEqual(TEXT("Alphonse has exactly five equipment slots"), Slots.Num(), 5)) return false;
    for (int32 Index = 0; Index < Slots.Num(); ++Index)
    {
        TestEqual(TEXT("Stable equipment slot ID"), UMistProgressionComponent::GetEquipmentSlotID(Slots[Index]), IDs[Index]);
        TestEqual(TEXT("Requested equipment label"), UMistProgressionComponent::GetEquipmentSlotDisplayName(Slots[Index]), Labels[Index]);
        TestTrue(TEXT("A new journey has no fabricated gear"), Progress->GetOwnedItemForSlot(Slots[Index]).IsNone());
        TestTrue(TEXT("A new journey has no fabricated equipped gear"), Progress->GetEquippedItemForSlot(Slots[Index]).IsNone());
    }
    const EMistEquipmentSlot Unknown = static_cast<EMistEquipmentSlot>(77);
    TestTrue(TEXT("Unknown slot does not produce a valid ID"), UMistProgressionComponent::GetEquipmentSlotID(Unknown).IsNone());
    TestFalse(TEXT("Unknown slot does not accept known gear"), UMistProgressionComponent::CanItemFitSlot(UMistProgressionComponent::GetRewardItemID(), Unknown));
    TestFalse(TEXT("No item is not a compatible equipment item"), UMistProgressionComponent::CanItemFitSlot(NAME_None, EMistEquipmentSlot::GreatswordBlade));
    TestFalse(TEXT("Unknown blade gear is rejected"), Progress->EquipItemToSlot(TEXT("InventedLoot"), EMistEquipmentSlot::GreatswordBlade));
    TestFalse(TEXT("A compatible but unearned blade cannot be equipped"), Progress->EquipItemToSlot(UMistProgressionComponent::GetRewardItemID(), EMistEquipmentSlot::GreatswordBlade));

    UMistCryptSaveGame* Legacy = NewObject<UMistCryptSaveGame>();
    Legacy->FormatVersion = UMistCryptSaveGame::LegacyHeavyWeaponVersion;
    Legacy->WeaponSlotID = TEXT("HeavyWeapon");
    Legacy->SaveRevision = 41;
    Legacy->CheckpointID = TEXT("Crypt.Reward");
    Legacy->bCryptCompleted = Legacy->bRewardClaimed = Legacy->bAbyssUnlocked = true;
    Legacy->OwnedItemID = Legacy->EquippedItemID = UMistProgressionComponent::GetRewardItemID();
    Legacy->LoadoutMask = 13;
    Legacy->CooldownSeconds = { 26.f, 32.f, 50.f, 140.f, 80.f, 21.f };
    Legacy->SavedUtcTicks = FDateTime::UtcNow().GetTicks() - 10 * ETimespan::TicksPerSecond;
    TestTrue(TEXT("Known v1 envelope is stored in an isolated real slot"), FMistProgressionTestAccess::WriteSave(Legacy, Fixture.Slot));
    TestTrue(TEXT("Known v1 HeavyWeapon save loads safely"), Progress->LoadProgression());
    TestTrue(TEXT("Migration preserves earned completion"), Progress->IsCryptCompleted());
    TestTrue(TEXT("Migration preserves claimed reward"), Progress->HasReward());
    TestTrue(TEXT("Migration preserves equipped reward"), Progress->IsRewardEquipped());
    TestTrue(TEXT("Migration preserves ascension unlock"), Progress->IsAbyssUnlocked());
    TestEqual(TEXT("Migration preserves mixed loadout"), Progress->GetLoadoutMask(), static_cast<uint8>(13));
    TestEqual(TEXT("Migration preserves checkpoint"), Progress->GetCheckpointID(), FName(TEXT("Crypt.Reward")));
    TestEqual(TEXT("Migration grants the bonus exactly once"), Progress->GetAbilityDamageMultiplier(), 1.05f, 0.0001f);
    const TArray<float> Remaining = Progress->GetCooldowns();
    for (int32 Index = 0; Index < Remaining.Num(); ++Index)
        TestEqual(FString::Printf(TEXT("Migration preserves elapsed cooldown %d"), Index), Remaining[Index], Legacy->CooldownSeconds[Index] - 10.f, 1.f);
    for (int32 Index = 0; Index < Slots.Num(); ++Index)
    {
        const bool bBlade = Slots[Index] == EMistEquipmentSlot::GreatswordBlade;
        TestEqual(TEXT("Only the blade accepts the earned prototype item"), UMistProgressionComponent::CanItemFitSlot(UMistProgressionComponent::GetRewardItemID(), Slots[Index]), bBlade);
        TestEqual(TEXT("Owned item belongs to the blade alone"), Progress->GetOwnedItemForSlot(Slots[Index]), bBlade ? UMistProgressionComponent::GetRewardItemID() : NAME_None);
        TestEqual(TEXT("Equipped item belongs to the blade alone"), Progress->GetEquippedItemForSlot(Slots[Index]), bBlade ? UMistProgressionComponent::GetRewardItemID() : NAME_None);
        if (!bBlade)
            TestFalse(TEXT("An item cannot equip into an incompatible slot"), Progress->EquipItemToSlot(UMistProgressionComponent::GetRewardItemID(), Slots[Index]));
    }
    TestFalse(TEXT("Migration cannot duplicate the already claimed reward"), Progress->ClaimCryptReward());
    UMistProgressionComponent* StaleLegacy = Fixture.CreateProgression();
    TestTrue(TEXT("Another legacy reader restores before the canonical write"), StaleLegacy->LoadProgression());
    TestTrue(TEXT("Normal autosave commits the migrated schema"), Progress->SaveProgression());
    UMistCryptSaveGame* Canonical = FMistProgressionTestAccess::ReadFixturePayload(Fixture.Slot);
    if (!TestNotNull(TEXT("Migrated file has a readable native payload"), Canonical)) return false;
    TestEqual(TEXT("Migration writes current schema to disk"), Canonical->FormatVersion, UMistCryptSaveGame::CurrentVersion);
    TestEqual(TEXT("Migration writes the canonical Blade ID to disk"), Canonical->WeaponSlotID, FName(TEXT("GreatswordBlade")));
    TestEqual(TEXT("Migration advances the existing revision exactly once"), Canonical->SaveRevision, static_cast<int64>(42));
    TestFalse(TEXT("Old migrated reader cannot overwrite the newer revision"), StaleLegacy->SaveProgression());
    TestTrue(TEXT("Canonical equipment state survives another restart"), StaleLegacy->LoadProgression());
    TestTrue(TEXT("Slot-aware equipment remains legal"), StaleLegacy->EquipItemToSlot(UMistProgressionComponent::GetRewardItemID(), EMistEquipmentSlot::GreatswordBlade));
    TestTrue(TEXT("Existing unequip API still persists"), StaleLegacy->EquipReward(false));
    TestTrue(TEXT("Unequipped blade remains owned"), !StaleLegacy->GetOwnedItemForSlot(EMistEquipmentSlot::GreatswordBlade).IsNone());
    TestTrue(TEXT("Unequipped blade slot is empty"), StaleLegacy->GetEquippedItemForSlot(EMistEquipmentSlot::GreatswordBlade).IsNone());
    TestEqual(TEXT("Unequipped equipment no longer grants a bonus"), StaleLegacy->GetAbilityDamageMultiplier(), 1.f, 0.0001f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMistCryptSaveFailureTest, "Mistworld.Progression.SaveFailureRollback",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMistCryptSaveFailureTest::RunTest(const FString& Parameters)
{
    FCryptSaveFixture Fixture;
    UMistProgressionComponent* Progress = Fixture.CreateProgression();
    if (!TestNotNull(TEXT("Save failure fixture"), Progress)) return false;
    UMistProgressionComponent* Unowned = NewObject<UMistProgressionComponent>();
    TestFalse(TEXT("An unowned component cannot mutate encounter completion"), Unowned->SetCryptCompleted());
    TestFalse(TEXT("An unowned component cannot mutate unlock state"), Unowned->SetAbyssUnlocked(true));
    TestFalse(TEXT("An unowned component cannot mutate checkpoint state"), Unowned->SetCheckpoint(TEXT("Crypt.Entrance")));
    TestFalse(TEXT("An unowned component cannot load player progression"), Unowned->LoadProgression());
    TestFalse(TEXT("An unowned component cannot save player progression"), Unowned->SaveProgression());
    TestFalse(TEXT("An unowned component cannot claim loot"), Unowned->ClaimCryptReward());
    TestFalse(TEXT("An unowned component cannot equip loot"), Unowned->EquipReward(true));
    Progress->SetCryptCompleted();
    Progress->SetCheckpoint(TEXT("Crypt.Reward"));
    FMistProgressionTestAccess::FailWrites(Progress, true);
    TestFalse(TEXT("Reward claim fails if the save write fails"), Progress->ClaimCryptReward());
    TestFalse(TEXT("Failed reward write rolls back ownership"), Progress->HasReward());
    TestFalse(TEXT("Failed reward write creates no file"), UGameplayStatics::DoesSaveGameExist(Fixture.Slot, 0));
    TestTrue(TEXT("Save failure is visible to the player"), Progress->GetSaveStatus().Contains(TEXT("failed")));
    FMistProgressionTestAccess::FailWrites(Progress, false);
    TestTrue(TEXT("Reward can be retried after a save failure"), Progress->ClaimCryptReward());
    FMistProgressionTestAccess::FailWrites(Progress, true);
    TestFalse(TEXT("Failed equip write rejects the transaction"), Progress->EquipReward(true));
    TestFalse(TEXT("Failed equip write retains old unequipped state"), Progress->IsRewardEquipped());
    TestEqual(TEXT("Failed equip write grants no bonus"), Progress->GetAbilityDamageMultiplier(), 1.f, 0.0001f);
    UMistProgressionComponent* Reloaded = Fixture.CreateProgression();
    TestTrue(TEXT("Prior committed reward survives the failed equip write"), Reloaded->LoadProgression());
    TestTrue(TEXT("Prior committed reward remains owned"), Reloaded->HasReward());
    TestFalse(TEXT("Failed equip state never reached disk"), Reloaded->IsRewardEquipped());
    FMistProgressionTestAccess::FailWrites(Progress, false);
    TestTrue(TEXT("Equip retry commits successfully"), Progress->EquipReward(true));
    FMistProgressionTestAccess::FailWrites(Progress, true);
    TestFalse(TEXT("Failed unequip write rejects the transaction"), Progress->EquipReward(false));
    TestTrue(TEXT("Failed unequip retains previous equipment"), Progress->IsRewardEquipped());
    TestEqual(TEXT("Failed unequip retains the committed ability bonus"), Progress->GetAbilityDamageMultiplier(), 1.05f, 0.0001f);
    TestTrue(TEXT("Disk still contains the committed equipped state"), Reloaded->LoadProgression());
    TestTrue(TEXT("Failed unequip never reached disk"), Reloaded->IsRewardEquipped());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMistCryptSaveValidationTest, "Mistworld.Progression.SaveValidation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMistCryptSaveValidationTest::RunTest(const FString& Parameters)
{
    FCryptSaveFixture Fixture;
    UMistProgressionComponent* Progress = Fixture.CreateProgression();
    if (!TestNotNull(TEXT("Save validation fixture"), Progress)) return false;
    auto AssertSafeDefaults = [&](const FString& Label)
    {
        TestFalse(Label + TEXT(": reward is absent"), Progress->HasReward());
        TestFalse(Label + TEXT(": equipment is absent"), Progress->IsRewardEquipped());
        TestFalse(Label + TEXT(": encounter is incomplete"), Progress->IsCryptCompleted());
        TestFalse(Label + TEXT(": Abyss is locked"), Progress->IsAbyssUnlocked());
        TestEqual(Label + TEXT(": original loadout"), Progress->GetLoadoutMask(), static_cast<uint8>(0));
        TestEqual(Label + TEXT(": entrance checkpoint"), Progress->GetCheckpointID(), FName(TEXT("Crypt.Entrance")));
        TestEqual(Label + TEXT(": no damage bonus"), Progress->GetAbilityDamageMultiplier(), 1.f, 0.0001f);
        for (float Remaining : Progress->GetCooldowns())
            TestEqual(Label + TEXT(": safe cooldown"), Remaining, 0.f, 0.0001f);
    };
    auto RejectSave = [&](const FString& Label, TFunction<void(UMistCryptSaveGame&)> Change)
    {
        UMistCryptSaveGame* Save = NewObject<UMistCryptSaveGame>();
        Change(*Save);
        TestTrue(Label + TEXT(": fixture writes a real save envelope"), FMistProgressionTestAccess::WriteSave(Save, Fixture.Slot));
        TestFalse(Label + TEXT(": load rejects invalid data"), Progress->LoadProgression());
        AssertSafeDefaults(Label);
    };
    RejectSave(TEXT("Unsupported version"), [](UMistCryptSaveGame& S) { S.FormatVersion = UMistCryptSaveGame::CurrentVersion + 1; });
    RejectSave(TEXT("Negative save revision"), [](UMistCryptSaveGame& S) { S.SaveRevision = -1; });
    RejectSave(TEXT("Overflowing save revision"), [](UMistCryptSaveGame& S) { S.SaveRevision = MAX_int64; });
    RejectSave(TEXT("Wrong hero ownership"), [](UMistCryptSaveGame& S) { S.HeroID = TEXT("Celeste"); });
    RejectSave(TEXT("Wrong weapon slot"), [](UMistCryptSaveGame& S) { S.WeaponSlotID = TEXT("LightWeapon"); });
    RejectSave(TEXT("Blade reward assigned to guard slot"), [](UMistCryptSaveGame& S)
    {
        S.WeaponSlotID = TEXT("GreatswordGuard");
        S.bCryptCompleted = S.bRewardClaimed = true;
        S.OwnedItemID = S.EquippedItemID = UMistProgressionComponent::GetRewardItemID();
    });
    RejectSave(TEXT("Legacy slot in canonical v2 schema"), [](UMistCryptSaveGame& S) { S.WeaponSlotID = TEXT("HeavyWeapon"); });
    RejectSave(TEXT("Canonical slot in legacy v1 schema"), [](UMistCryptSaveGame& S) { S.FormatVersion = UMistCryptSaveGame::LegacyHeavyWeaponVersion; });
    RejectSave(TEXT("Malformed legacy ownership is rejected before migration"), [](UMistCryptSaveGame& S)
    {
        S.FormatVersion = UMistCryptSaveGame::LegacyHeavyWeaponVersion;
        S.WeaponSlotID = TEXT("HeavyWeapon");
        S.bCryptCompleted = S.bRewardClaimed = true;
        S.OwnedItemID = TEXT("InventedLegacyLoot");
    });
    RejectSave(TEXT("Unknown checkpoint"), [](UMistCryptSaveGame& S) { S.CheckpointID = TEXT("Crypt.Hidden"); });
    RejectSave(TEXT("Uncompleted reward checkpoint"), [](UMistCryptSaveGame& S) { S.CheckpointID = TEXT("Crypt.Reward"); });
    RejectSave(TEXT("Undefined ability slot"), [](UMistCryptSaveGame& S) { S.LoadoutMask = 16; S.bAbyssUnlocked = true; });
    RejectSave(TEXT("Locked alternate ability"), [](UMistCryptSaveGame& S) { S.LoadoutMask = 1; });
    RejectSave(TEXT("Unearned reward claim"), [](UMistCryptSaveGame& S)
    {
        S.bRewardClaimed = true; S.OwnedItemID = UMistProgressionComponent::GetRewardItemID();
    });
    RejectSave(TEXT("Unknown inventory item"), [](UMistCryptSaveGame& S)
    {
        S.bCryptCompleted = S.bRewardClaimed = true; S.OwnedItemID = TEXT("ArbitraryLoot");
    });
    RejectSave(TEXT("Unknown equipped item"), [](UMistCryptSaveGame& S)
    {
        S.bCryptCompleted = S.bRewardClaimed = true;
        S.OwnedItemID = UMistProgressionComponent::GetRewardItemID(); S.EquippedItemID = TEXT("ArbitraryLoot");
    });
    RejectSave(TEXT("Unowned equipped item"), [](UMistCryptSaveGame& S)
    {
        S.EquippedItemID = UMistProgressionComponent::GetRewardItemID();
    });
    RejectSave(TEXT("Unclaimed inventory item"), [](UMistCryptSaveGame& S)
    {
        S.OwnedItemID = UMistProgressionComponent::GetRewardItemID();
    });
    RejectSave(TEXT("Missing cooldown entry"), [](UMistCryptSaveGame& S) { S.CooldownSeconds.Pop(); });
    RejectSave(TEXT("Negative cooldown"), [](UMistCryptSaveGame& S) { S.CooldownSeconds[3] = -1.f; });
    RejectSave(TEXT("Non-finite cooldown"), [](UMistCryptSaveGame& S) { S.CooldownSeconds[3] = std::numeric_limits<float>::quiet_NaN(); });
    RejectSave(TEXT("Excessive cooldown"), [](UMistCryptSaveGame& S) { S.CooldownSeconds[3] = 1000000.f; });
    RejectSave(TEXT("Invalid cooldown timestamp"), [](UMistCryptSaveGame& S) { S.SavedUtcTicks = 0; });
    RejectSave(TEXT("Out-of-range timestamp"), [](UMistCryptSaveGame& S) { S.SavedUtcTicks = FDateTime::MaxValue().GetTicks() + 1; });
    RejectSave(TEXT("Unknown active character"), [](UMistCryptSaveGame& S) { S.ActiveHero = static_cast<EMistHeroId>(255); });
    RejectSave(TEXT("Locked Flamestealer slot"), [](UMistCryptSaveGame& S) { S.AnimaProfile.LoadoutMask = 1; });
    RejectSave(TEXT("Undefined Flamestealer slot"), [](UMistCryptSaveGame& S) { S.AnimaProfile.bAscensionUnlocked = true; S.AnimaProfile.LoadoutMask = 16; });
    RejectSave(TEXT("Missing Anima cooldown entry"), [](UMistCryptSaveGame& S) { S.AnimaProfile.CooldownSeconds.Pop(); });
    RejectSave(TEXT("Non-finite Anima cooldown"), [](UMistCryptSaveGame& S) { S.AnimaProfile.CooldownSeconds[3] = std::numeric_limits<float>::quiet_NaN(); });
    RejectSave(TEXT("Invalid Anima timestamp"), [](UMistCryptSaveGame& S) { S.AnimaProfile.SavedUtcTicks = 0; });

    // Corruption must be rejected before any UObject string/count field is read.
    UMistCryptSaveGame* EnvelopeFixture = NewObject<UMistCryptSaveGame>();
    TestTrue(TEXT("Integrity fixture writes a valid save envelope"), FMistProgressionTestAccess::WriteSave(EnvelopeFixture, Fixture.Slot));
    TArray<uint8> ValidEnvelope;
    if (!TestTrue(TEXT("Integrity fixture reads back real slot bytes"), UGameplayStatics::LoadDataFromSlot(ValidEnvelope, Fixture.Slot, 0))) return false;
    if (!TestTrue(TEXT("Integrity fixture includes a bounded header and payload"), ValidEnvelope.Num() > 80)) return false;
    auto RejectEnvelope = [&](const FString& Label, const TArray<uint8>& Bytes)
    {
        TestTrue(Label + TEXT(": corrupted bytes write"), UGameplayStatics::SaveDataToSlot(Bytes, Fixture.Slot, 0));
        TestFalse(Label + TEXT(": rejected before payload deserialization"), Progress->LoadProgression());
        AssertSafeDefaults(Label);
    };
    TArray<uint8> ModifiedEnvelope = ValidEnvelope;
    ModifiedEnvelope.Last() ^= 0x40;
    RejectEnvelope(TEXT("Changed payload checksum"), ModifiedEnvelope);
    ModifiedEnvelope = ValidEnvelope;
    ModifiedEnvelope[12] ^= 0x40;
    RejectEnvelope(TEXT("Changed checksum header"), ModifiedEnvelope);
    ModifiedEnvelope = ValidEnvelope;
    ModifiedEnvelope[4] = 2;
    RejectEnvelope(TEXT("Unknown envelope version"), ModifiedEnvelope);
    ModifiedEnvelope = ValidEnvelope;
    ModifiedEnvelope[8] = ModifiedEnvelope[9] = ModifiedEnvelope[10] = ModifiedEnvelope[11] = 0xff;
    RejectEnvelope(TEXT("Unbounded payload length"), ModifiedEnvelope);
    ModifiedEnvelope = ValidEnvelope;
    ModifiedEnvelope.Pop();
    RejectEnvelope(TEXT("Truncated envelope payload"), ModifiedEnvelope);
    TestTrue(TEXT("A native unwrapped SaveGame fixture writes"), UGameplayStatics::SaveGameToSlot(EnvelopeFixture, Fixture.Slot, 0));
    TestFalse(TEXT("Unwrapped native save is rejected rather than guessed or migrated"), Progress->LoadProgression());
    AssertSafeDefaults(TEXT("Unwrapped native save"));

    TArray<uint8> CorruptBytes = { 'G', 'V', 'A', 'S', 0, 0, 0, 0 };
    TestTrue(TEXT("Truncated raw save fixture writes"), UGameplayStatics::SaveDataToSlot(CorruptBytes, Fixture.Slot, 0));
    TestFalse(TEXT("Truncated raw file is rejected before header deserialization"), Progress->LoadProgression());
    AssertSafeDefaults(TEXT("Truncated raw file"));
    CorruptBytes.Init(0, 64 * 1024 + 17);
    TestTrue(TEXT("Excessive raw save fixture writes"), UGameplayStatics::SaveDataToSlot(CorruptBytes, Fixture.Slot, 0));
    TestFalse(TEXT("Oversized raw file is rejected before deserialization"), Progress->LoadProgression());
    AssertSafeDefaults(TEXT("Oversized raw file"));
    CorruptBytes.Init(0, 128);
    TestTrue(TEXT("Invalid header fixture writes"), UGameplayStatics::SaveDataToSlot(CorruptBytes, Fixture.Slot, 0));
    TestFalse(TEXT("Invalid header is rejected before deserialization"), Progress->LoadProgression());
    AssertSafeDefaults(TEXT("Invalid header"));
    TestTrue(TEXT("A valid new checkpoint can overwrite an unusable save"), Progress->SaveProgression());
    TestTrue(TEXT("Recovered save is readable"), Progress->LoadProgression());
    TestFalse(TEXT("Recovered save does not invent rewards"), Progress->HasReward());
    TestFalse(TEXT("Malformed cooldown snapshots cannot be set by the controller"), Progress->SetCooldowns({ 6.f, -2.f }));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMistHeroProfilesPersistenceTest, "Mistworld.Progression.HeroProfilesPersistence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMistHeroProfilesPersistenceTest::RunTest(const FString& Parameters)
{
    FCryptSaveFixture Fixture;
    UMistProgressionComponent* Progress = Fixture.CreateProgression();
    if (!TestNotNull(TEXT("Two-hero progression fixture"), Progress)) return false;
    TestFalse(TEXT("New journey is not loaded from a missing slot"), Progress->LoadProgression());
    TestTrue(TEXT("New journey starts with Alphonse"), Progress->GetActiveHero() == EMistHeroId::Alphonse);
    TestFalse(TEXT("Unknown hero IDs are rejected"), Progress->SetActiveHero(static_cast<EMistHeroId>(255)));
    TestTrue(TEXT("Alphonse unlock recorded"), Progress->SetAbyssUnlocked(true));
    TestTrue(TEXT("Alphonse mixed kit recorded"), Progress->SetLoadoutMask(9));
    TestTrue(TEXT("Alphonse cooldowns recorded"), Progress->SetCooldowns({ 15.f, 30.f, 45.f, 120.f, 60.f, 1.f }));
    TestTrue(TEXT("Shared boss completion recorded"), Progress->SetCryptCompleted());
    TestTrue(TEXT("Reward claim persists"), Progress->ClaimCryptReward());
    TestTrue(TEXT("Greatsword equipped on Alphonse"), Progress->EquipReward(true));
    TestTrue(TEXT("Anima selected"), Progress->SetActiveHero(EMistHeroId::Anima));
    TestEqual(TEXT("Anima starts with original kit"), Progress->GetLoadoutMask(), static_cast<uint8>(0));
    TestFalse(TEXT("Alphonse unlock does not unlock Anima"), Progress->IsAbyssUnlocked());
    TestEqual(TEXT("Alphonse's equipment does not buff Anima"), Progress->GetAbilityDamageMultiplier(), 1.f, .001f);
    TestFalse(TEXT("Anima does not report Alphonse's equipment as equipped"), Progress->IsRewardEquipped());
    TestTrue(TEXT("Account retains its earned reward"), Progress->HasReward());
    TestFalse(TEXT("Anima cannot equip the greatsword"), Progress->EquipReward(true));
    TestFalse(TEXT("Anima cannot unequip Alphonse's gear"), Progress->EquipReward(false));
    TestFalse(TEXT("Anima cannot use greatsword equip-by-slot API"), Progress->EquipItemToSlot(UMistProgressionComponent::GetRewardItemID(), EMistEquipmentSlot::GreatswordBlade));
    TestTrue(TEXT("Anima cannot see greatsword as owned in her slots"), Progress->GetOwnedItemForSlot(EMistEquipmentSlot::GreatswordBlade).IsNone());
    const auto& AnimaSlots = UMistProgressionComponent::GetEquipmentSlotsForHero(EMistHeroId::Anima);
    TestEqual(TEXT("Anima has five explicit equipment slots"), AnimaSlots.Num(), 5);
    TestTrue(TEXT("Anima first weapon slot is Rapier"), AnimaSlots[0] == EMistEquipmentSlot::Rapier);
    TestTrue(TEXT("Anima second weapon slot is Parrying Dagger"), AnimaSlots[1] == EMistEquipmentSlot::ParryingDagger);
    for (const EMistEquipmentSlot Slot : AnimaSlots)
    {
        TestTrue(TEXT("Anima has no fabricated reward items"), Progress->GetOwnedItemForSlot(Slot).IsNone());
        TestTrue(TEXT("Anima has no fabricated equipped items"), Progress->GetEquippedItemForSlot(Slot).IsNone());
        TestFalse(TEXT("Anima slots reject the earned greatsword"), UMistProgressionComponent::CanItemFitSlot(UMistProgressionComponent::GetRewardItemID(), Slot));
    }
    TestTrue(TEXT("Anima Flamestealer unlock recorded independently"), Progress->SetAbyssUnlocked(true));
    TestTrue(TEXT("Anima mixed kit recorded independently"), Progress->SetLoadoutMask(6));
    TestTrue(TEXT("Anima cooldowns recorded independently"), Progress->SetCooldowns({ 2.f, 12.f, 15.f, 110.f, 60.f, .8f }));
    TestTrue(TEXT("Active Anima and both profiles save"), Progress->SaveProgression());
    UMistProgressionComponent* Reloaded = Fixture.CreateProgression();
    TestTrue(TEXT("Two-hero profile restores"), Reloaded->LoadProgression());
    TestTrue(TEXT("Active Anima survives restart"), Reloaded->GetActiveHero() == EMistHeroId::Anima);
    TestEqual(TEXT("Anima mask survives restart"), Reloaded->GetLoadoutMask(), static_cast<uint8>(6));
    TestTrue(TEXT("Anima unlock survives restart"), Reloaded->IsAbyssUnlocked());
    TestTrue(TEXT("Shared encounter completion survives Anima restart"), Reloaded->IsCryptCompleted());
    TestTrue(TEXT("Anima R cooldown remains its own"), Reloaded->GetCooldowns()[3] > 105.f && Reloaded->GetCooldowns()[3] <= 110.f);
    TestTrue(TEXT("Switch back to Alphonse"), Reloaded->SetActiveHero(EMistHeroId::Alphonse));
    TestEqual(TEXT("Alphonse loadout retained"), Reloaded->GetLoadoutMask(), static_cast<uint8>(9));
    TestTrue(TEXT("Alphonse unlock retained"), Reloaded->IsAbyssUnlocked());
    TestTrue(TEXT("Alphonse still owns his equipped blade"), Reloaded->IsRewardEquipped());
    TestEqual(TEXT("Alphonse blade damage bonus restored"), Reloaded->GetAbilityDamageMultiplier(), 1.05f, .001f);
    TestTrue(TEXT("Alphonse R cooldown remains its own"), Reloaded->GetCooldowns()[3] > 115.f && Reloaded->GetCooldowns()[3] <= 120.f);

    UMistCryptSaveGame* Disk = FMistProgressionTestAccess::ReadSave(Reloaded);
    if (!TestNotNull(TEXT("Both profiles are readable on disk"), Disk)) return false;
    Disk->SavedUtcTicks = FDateTime::UtcNow().GetTicks() - 20 * ETimespan::TicksPerSecond;
    Disk->CooldownSeconds = { 15.f, 30.f, 45.f, 120.f, 60.f, 1.f };
    Disk->AnimaProfile.SavedUtcTicks = FDateTime::UtcNow().GetTicks() - 10 * ETimespan::TicksPerSecond;
    Disk->AnimaProfile.CooldownSeconds = { 2.f, 12.f, 15.f, 110.f, 60.f, .8f };
    TestTrue(TEXT("Independent offline clocks fixture writes"), FMistProgressionTestAccess::WriteSave(Disk, Fixture.Slot));
    TestTrue(TEXT("Independent clocks load"), Reloaded->LoadProgression());
    TestEqual(TEXT("Anima subtracts her own ten elapsed seconds"), Reloaded->GetCooldowns()[3], 100.f, 1.f);
    TestTrue(TEXT("Choose inactive Alphonse"), Reloaded->SetActiveHero(EMistHeroId::Alphonse));
    TestEqual(TEXT("Alphonse subtracts twenty elapsed seconds while inactive"), Reloaded->GetCooldowns()[3], 100.f, 1.f);
    TestEqual(TEXT("Short cooldown continues to expire while inactive"), Reloaded->GetCooldowns()[0], 0.f, .01f);
    TestTrue(TEXT("Saving normalizes both independent clocks"), Reloaded->SaveProgression());
    TestTrue(TEXT("Anima profile still available after normalization"), Reloaded->SetActiveHero(EMistHeroId::Anima));
    TestTrue(TEXT("Saving cannot replenish Anima R"), Reloaded->GetCooldowns()[3] <= 100.f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMistSingleHeroSaveMigrationTest, "Mistworld.Progression.SingleHeroSaveMigration",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMistSingleHeroSaveMigrationTest::RunTest(const FString& Parameters)
{
    FCryptSaveFixture Fixture;
    UMistProgressionComponent* Progress = Fixture.CreateProgression();
    if (!TestNotNull(TEXT("Single-hero migration fixture"), Progress)) return false;
    UMistCryptSaveGame* Legacy = NewObject<UMistCryptSaveGame>();
    Legacy->FormatVersion = UMistCryptSaveGame::SingleHeroVersion;
    Legacy->SaveRevision = 15;
    Legacy->CheckpointID = TEXT("Crypt.Reward");
    Legacy->bCryptCompleted = Legacy->bRewardClaimed = Legacy->bAbyssUnlocked = true;
    Legacy->OwnedItemID = Legacy->EquippedItemID = UMistProgressionComponent::GetRewardItemID();
    Legacy->LoadoutMask = 11;
    Legacy->CooldownSeconds = { 16.f, 32.f, 40.f, 140.f, 80.f, 21.f };
    Legacy->SavedUtcTicks = FDateTime::UtcNow().GetTicks() - 10 * ETimespan::TicksPerSecond;
    TestTrue(TEXT("Version-two fixture writes"), FMistProgressionTestAccess::WriteSave(Legacy, Fixture.Slot));
    TestTrue(TEXT("Version-two save migrates"), Progress->LoadProgression());
    TestTrue(TEXT("Migration keeps Alphonse selected"), Progress->GetActiveHero() == EMistHeroId::Alphonse);
    TestTrue(TEXT("Migration preserves owned and equipped reward"), Progress->HasReward() && Progress->IsRewardEquipped());
    TestTrue(TEXT("Migration preserves shared completed crypt"), Progress->IsCryptCompleted());
    TestEqual(TEXT("Migration preserves Alphonse mask"), Progress->GetLoadoutMask(), static_cast<uint8>(11));
    TestEqual(TEXT("Migration preserves elapsed Alphonse R cooldown"), Progress->GetCooldowns()[3], 130.f, 1.f);
    TestTrue(TEXT("Migration permits Anima selection"), Progress->SetActiveHero(EMistHeroId::Anima));
    TestFalse(TEXT("Migration does not grant Anima ascension"), Progress->IsAbyssUnlocked());
    TestEqual(TEXT("Migration gives Anima original abilities"), Progress->GetLoadoutMask(), static_cast<uint8>(0));
    TestEqual(TEXT("Migration does not transfer Alphonse cooldown"), Progress->GetCooldowns()[3], 0.f, .01f);
    TestTrue(TEXT("New active hero and migrated fields persist"), Progress->SaveProgression());
    UMistCryptSaveGame* Canonical = FMistProgressionTestAccess::ReadFixturePayload(Fixture.Slot);
    if (!TestNotNull(TEXT("Version-three profile writes native payload"), Canonical)) return false;
    TestEqual(TEXT("Migration upgrades to version three"), Canonical->FormatVersion, UMistCryptSaveGame::CurrentVersion);
    TestEqual(TEXT("Migration advances known revision once"), Canonical->SaveRevision, static_cast<int64>(16));
    TestEqual(TEXT("Migration preserves Alphonse stable ownership ID"), Canonical->HeroID, FName(TEXT("Alphonse")));
    TestEqual(TEXT("Migration preserves Alphonse reward item"), Canonical->EquippedItemID, UMistProgressionComponent::GetRewardItemID());
    TestTrue(TEXT("Active Anima is stored separately from equipment ownership"), Canonical->ActiveHero == EMistHeroId::Anima);
    return true;
}

#endif
