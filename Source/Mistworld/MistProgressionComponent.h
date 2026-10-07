#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "MistEquipmentSlots.h"
#include "MistCryptSaveGame.h"
#include "MistHeroTypes.h"
#include "MistProgressionComponent.generated.h"

class UMistCryptSaveGame;

// Added to the crypt controller only. Combat sandboxes do not touch this save slot.
UCLASS(ClassGroup=(Mistworld), meta=(BlueprintSpawnableComponent))
class MISTWORLD_API UMistProgressionComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UMistProgressionComponent();
    bool LoadProgression();
    bool SaveProgression();
    bool ClaimCryptReward();
    bool EquipReward(bool bEquip);
    bool SetActiveHero(EMistHeroId Hero);
    EMistHeroId GetActiveHero() const { return ActiveHero; }
    // The first slice owns one Blade item; the other four real slots remain empty.
    static const TArray<EMistEquipmentSlot>& GetEquipmentSlots();
    static const TArray<EMistEquipmentSlot>& GetEquipmentSlotsForHero(EMistHeroId Hero);
    static FName GetEquipmentSlotID(EMistEquipmentSlot Slot);
    static FString GetEquipmentSlotDisplayName(EMistEquipmentSlot Slot);
    static bool CanItemFitSlot(FName ItemID, EMistEquipmentSlot Slot);
    FName GetOwnedItemForSlot(EMistEquipmentSlot Slot) const;
    FName GetEquippedItemForSlot(EMistEquipmentSlot Slot) const;
    bool EquipItemToSlot(FName ItemID, EMistEquipmentSlot Slot);
    bool HasReward() const { return bRewardClaimed; }
    bool IsRewardEquipped() const { return ActiveHero == EMistHeroId::Alphonse && bRewardEquipped; }
    bool IsCryptCompleted() const { return bCryptCompleted; }
    bool SetCryptCompleted();
    bool SetCheckpoint(FName ID);
    FName GetCheckpointID() const { return CheckpointID; }
    float GetAbilityDamageMultiplier() const { return ActiveHero == EMistHeroId::Alphonse && bRewardClaimed && bRewardEquipped ? 1.05f : 1.f; }
    uint8 GetLoadoutMask() const { return ActiveHero == EMistHeroId::Anima ? AnimaProfile.LoadoutMask : LoadoutMask; }
    bool SetLoadoutMask(uint8 Mask);
    bool IsAbyssUnlocked() const { return ActiveHero == EMistHeroId::Anima ? AnimaProfile.bAscensionUnlocked : bAbyssUnlocked; }
    bool SetAbyssUnlocked(bool bUnlocked);
    bool SetCooldowns(const TArray<float>& Seconds);
    TArray<float> GetCooldowns() const;
    FString GetSaveStatus() const { return SaveStatus; }
    // Automation uses unique slots and never edits the player's progression.
    bool SetSaveSlotForTesting(const FString& Slot);
    static FName GetRewardItemID() { return TEXT("NecroticGreatsword.Prototype"); }

private:
    friend struct FMistProgressionTestAccess;
    FString SaveSlot = TEXT("Mistworld.Crypt.v1");
    FString SaveStatus = TEXT("Progress has not been loaded.");
    FName CheckpointID = TEXT("Crypt.Entrance");
    EMistHeroId ActiveHero = EMistHeroId::Alphonse;
    FMistHeroProgressProfile AnimaProfile;
    uint8 LoadoutMask = 0;
    bool bAbyssUnlocked = false;
    bool bCryptCompleted = false;
    bool bRewardClaimed = false;
    bool bRewardEquipped = false;
    TArray<float> CooldownSeconds;
    int64 CooldownSnapshotUtcTicks = 0;
    int64 LastKnownSaveRevision = 0;
    // Only the friend automation fixture can force failure; no gameplay API exposes it.
    bool bForceSaveFailureForTesting = false;
    void ResetToSafeDefaults();
    void RestoreValidatedSave(const UMistCryptSaveGame& Save);
    UMistCryptSaveGame* ReadValidatedSave(FString& Reason) const;
    static bool WriteSaveEnvelope(UMistCryptSaveGame* Save, const FString& Slot);
    static bool ValidateSave(const UMistCryptSaveGame& Save, FString& Reason);
    static bool ValidateCooldowns(const TArray<float>& Seconds);
    static double ElapsedSince(int64 UtcTicks);
    bool CanChangeProgression() const;
};
