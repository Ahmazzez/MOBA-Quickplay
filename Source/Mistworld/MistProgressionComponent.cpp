#include "MistProgressionComponent.h"
#include "MistCryptSaveGame.h"
#include "Kismet/GameplayStatics.h"
#include "GameFramework/Actor.h"
#include "Misc/Crc.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"

namespace
{
    constexpr int32 CooldownCount = 6;
    constexpr float MaxStoredCooldown = 3600.f;
    constexpr int32 MaxSaveBytes = 64 * 1024;
    constexpr uint32 EnvelopeMagic = 0x5343574d; // "MWCS" on the supported Windows target.
    constexpr uint32 EnvelopeVersion = 1;
    constexpr int32 EnvelopeHeaderBytes = 16;
    bool IsKnownCheckpoint(FName ID)
    {
        return ID == TEXT("Crypt.Entrance") || ID == TEXT("Crypt.Reward");
    }
}

UMistProgressionComponent::UMistProgressionComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
    ResetToSafeDefaults();
}

void UMistProgressionComponent::ResetToSafeDefaults()
{
    CheckpointID = TEXT("Crypt.Entrance");
    ActiveHero = EMistHeroId::Alphonse;
    AnimaProfile = FMistHeroProgressProfile();
    LoadoutMask = 0;
    bAbyssUnlocked = bCryptCompleted = bRewardClaimed = bRewardEquipped = false;
    CooldownSeconds.Init(0.f, CooldownCount);
    CooldownSnapshotUtcTicks = FDateTime::UtcNow().GetTicks();
    LastKnownSaveRevision = 0;
}

bool UMistProgressionComponent::CanChangeProgression() const
{
    return GetOwner() && GetOwner()->HasAuthority();
}

bool UMistProgressionComponent::ValidateCooldowns(const TArray<float>& Seconds)
{
    if (Seconds.Num() != CooldownCount) return false;
    for (const float Value : Seconds)
        if (!FMath::IsFinite(Value) || Value < 0.f || Value > MaxStoredCooldown) return false;
    return true;
}

bool UMistProgressionComponent::ValidateSave(const UMistCryptSaveGame& Save, FString& Reason)
{
    const bool bLegacyHeavyWeapon = Save.FormatVersion == UMistCryptSaveGame::LegacyHeavyWeaponVersion;
    const bool bSingleHero = Save.FormatVersion == UMistCryptSaveGame::SingleHeroVersion;
    if (Save.FormatVersion != UMistCryptSaveGame::CurrentVersion && !bLegacyHeavyWeapon && !bSingleHero)
        Reason = TEXT("Unsupported save version");
    else if (Save.SaveRevision < 0 || Save.SaveRevision == MAX_int64)
        Reason = TEXT("Invalid save revision");
    else if (Save.HeroID != TEXT("Alphonse")
        || Save.WeaponSlotID != (bLegacyHeavyWeapon ? FName(TEXT("HeavyWeapon"))
            : GetEquipmentSlotID(EMistEquipmentSlot::GreatswordBlade)))
        Reason = TEXT("Unknown character or equipment slot");
    else if (!IsKnownCheckpoint(Save.CheckpointID)
        || (Save.CheckpointID == TEXT("Crypt.Reward") && !Save.bCryptCompleted))
        Reason = TEXT("Invalid checkpoint progression");
    else if (Save.LoadoutMask > 15 || (!Save.bAbyssUnlocked && Save.LoadoutMask != 0))
        Reason = TEXT("Invalid or locked ability loadout");
    else if (!ValidateCooldowns(Save.CooldownSeconds) || Save.SavedUtcTicks <= 0
        || Save.SavedUtcTicks > FDateTime::MaxValue().GetTicks())
        Reason = TEXT("Invalid cooldown record");
    else if (Save.FormatVersion == UMistCryptSaveGame::CurrentVersion && !IsKnownMistHero(Save.ActiveHero))
        Reason = TEXT("Unknown active character");
    else if (Save.FormatVersion == UMistCryptSaveGame::CurrentVersion
        && (Save.AnimaProfile.LoadoutMask > 15
            || (!Save.AnimaProfile.bAscensionUnlocked && Save.AnimaProfile.LoadoutMask != 0)))
        Reason = TEXT("Invalid or locked Anima loadout");
    else if (Save.FormatVersion == UMistCryptSaveGame::CurrentVersion
        && (!ValidateCooldowns(Save.AnimaProfile.CooldownSeconds) || Save.AnimaProfile.SavedUtcTicks <= 0
            || Save.AnimaProfile.SavedUtcTicks > FDateTime::MaxValue().GetTicks()))
        Reason = TEXT("Invalid Anima cooldown record");
    else if (Save.bRewardClaimed && (!Save.bCryptCompleted || Save.OwnedItemID != GetRewardItemID()
        || (!Save.EquippedItemID.IsNone() && Save.EquippedItemID != GetRewardItemID())))
        Reason = TEXT("Invalid reward ownership");
    else if (!Save.bRewardClaimed && (!Save.OwnedItemID.IsNone() || !Save.EquippedItemID.IsNone()))
        Reason = TEXT("Equipment without an earned reward");
    else
        return true;
    return false;
}

UMistCryptSaveGame* UMistProgressionComponent::ReadValidatedSave(FString& Reason) const
{
    if (!UGameplayStatics::DoesSaveGameExist(SaveSlot, 0))
    {
        Reason = TEXT("No saved crypt journey");
        return nullptr;
    }
    TArray<uint8> Data;
    if (!UGameplayStatics::LoadDataFromSlot(Data, SaveSlot, 0))
    {
        Reason = TEXT("Save could not be read");
        return nullptr;
    }
    // A bounded header and payload checksum catch accidental corruption before Unreal
    // reads string/count fields. This is an integrity format, not anti-cheat protection.
    if (Data.Num() < EnvelopeHeaderBytes || Data.Num() > MaxSaveBytes + EnvelopeHeaderBytes)
    {
        Reason = TEXT("Malformed save file");
        return nullptr;
    }
    FMemoryReader Reader(Data, true);
    uint32 Magic = 0, Version = 0, PayloadLength = 0, PayloadChecksum = 0;
    Reader << Magic << Version << PayloadLength << PayloadChecksum;
    if (Reader.IsError() || Magic != EnvelopeMagic || Version != EnvelopeVersion
        || PayloadLength < 64 || PayloadLength > MaxSaveBytes
        || PayloadLength != static_cast<uint32>(Data.Num() - EnvelopeHeaderBytes))
    {
        Reason = Version != EnvelopeVersion && Magic == EnvelopeMagic
            ? TEXT("Unsupported save envelope version") : TEXT("Malformed save envelope");
        return nullptr;
    }
    const uint8* PayloadStart = Data.GetData() + EnvelopeHeaderBytes;
    if (FCrc::MemCrc32(PayloadStart, PayloadLength) != PayloadChecksum
        || PayloadStart[0] != 'G' || PayloadStart[1] != 'V' || PayloadStart[2] != 'A' || PayloadStart[3] != 'S')
    {
        Reason = TEXT("Damaged save payload");
        return nullptr;
    }
    TArray<uint8> Payload;
    Payload.Append(PayloadStart, PayloadLength);
    UMistCryptSaveGame* Save = Cast<UMistCryptSaveGame>(UGameplayStatics::LoadGameFromMemory(Payload));
    if (!Save)
    {
        Reason = TEXT("Unexpected save format");
        return nullptr;
    }
    if (!ValidateSave(*Save, Reason)) return nullptr;
    // Validate the old format before migrating it, and preserve every earned field,
    // cooldown timestamp, and optimistic revision. The next normal save writes v3.
    if (Save->FormatVersion != UMistCryptSaveGame::CurrentVersion)
    {
        Save->FormatVersion = UMistCryptSaveGame::CurrentVersion;
        Save->WeaponSlotID = GetEquipmentSlotID(EMistEquipmentSlot::GreatswordBlade);
        Save->ActiveHero = EMistHeroId::Alphonse;
        Save->AnimaProfile = FMistHeroProgressProfile();
    }
    return Save;
}

const TArray<EMistEquipmentSlot>& UMistProgressionComponent::GetEquipmentSlots()
{
    static const TArray<EMistEquipmentSlot> Slots = {
        EMistEquipmentSlot::GreatswordBlade, EMistEquipmentSlot::GreatswordGuard,
        EMistEquipmentSlot::HeavyHelmet, EMistEquipmentSlot::HeavyChestplate,
        EMistEquipmentSlot::HeavyBoots
    };
    return Slots;
}

const TArray<EMistEquipmentSlot>& UMistProgressionComponent::GetEquipmentSlotsForHero(EMistHeroId Hero)
{
    static const TArray<EMistEquipmentSlot> AnimaSlots = {
        EMistEquipmentSlot::Rapier, EMistEquipmentSlot::ParryingDagger,
        EMistEquipmentSlot::LightHelmet, EMistEquipmentSlot::LightChestplate, EMistEquipmentSlot::LightBoots
    };
    return Hero == EMistHeroId::Anima ? AnimaSlots : GetEquipmentSlots();
}

bool UMistProgressionComponent::SetActiveHero(EMistHeroId Hero)
{
    if (!CanChangeProgression() || !IsKnownMistHero(Hero)) return false;
    ActiveHero = Hero;
    return true;
}

FName UMistProgressionComponent::GetEquipmentSlotID(EMistEquipmentSlot Slot)
{
    switch (Slot)
    {
    case EMistEquipmentSlot::GreatswordBlade: return TEXT("GreatswordBlade");
    case EMistEquipmentSlot::GreatswordGuard: return TEXT("GreatswordGuard");
    case EMistEquipmentSlot::HeavyHelmet: return TEXT("HeavyHelmet");
    case EMistEquipmentSlot::HeavyChestplate: return TEXT("HeavyChestplate");
    case EMistEquipmentSlot::HeavyBoots: return TEXT("HeavyBoots");
    case EMistEquipmentSlot::Rapier: return TEXT("Rapier");
    case EMistEquipmentSlot::ParryingDagger: return TEXT("ParryingDagger");
    case EMistEquipmentSlot::LightHelmet: return TEXT("LightHelmet");
    case EMistEquipmentSlot::LightChestplate: return TEXT("LightChestplate");
    case EMistEquipmentSlot::LightBoots: return TEXT("LightBoots");
    default: return NAME_None;
    }
}

FString UMistProgressionComponent::GetEquipmentSlotDisplayName(EMistEquipmentSlot Slot)
{
    switch (Slot)
    {
    case EMistEquipmentSlot::GreatswordBlade: return TEXT("Greatsword Blade");
    case EMistEquipmentSlot::GreatswordGuard: return TEXT("Greatsword Guard");
    case EMistEquipmentSlot::HeavyHelmet: return TEXT("Heavy Helmet");
    case EMistEquipmentSlot::HeavyChestplate: return TEXT("Heavy Chestplate");
    case EMistEquipmentSlot::HeavyBoots: return TEXT("Heavy Boots");
    case EMistEquipmentSlot::Rapier: return TEXT("Rapier");
    case EMistEquipmentSlot::ParryingDagger: return TEXT("Parrying Dagger");
    case EMistEquipmentSlot::LightHelmet: return TEXT("Light Helmet");
    case EMistEquipmentSlot::LightChestplate: return TEXT("Light Chestplate");
    case EMistEquipmentSlot::LightBoots: return TEXT("Light Boots");
    default: return TEXT("Unknown Slot");
    }
}

bool UMistProgressionComponent::CanItemFitSlot(FName ItemID, EMistEquipmentSlot Slot)
{
    return ItemID == GetRewardItemID() && Slot == EMistEquipmentSlot::GreatswordBlade;
}

FName UMistProgressionComponent::GetOwnedItemForSlot(EMistEquipmentSlot Slot) const
{
    return ActiveHero == EMistHeroId::Alphonse && bRewardClaimed && CanItemFitSlot(GetRewardItemID(), Slot) ? GetRewardItemID() : NAME_None;
}

FName UMistProgressionComponent::GetEquippedItemForSlot(EMistEquipmentSlot Slot) const
{
    return bRewardEquipped ? GetOwnedItemForSlot(Slot) : NAME_None;
}

bool UMistProgressionComponent::EquipItemToSlot(FName ItemID, EMistEquipmentSlot Slot)
{
    if (!CanChangeProgression()) return false;
    if (ActiveHero != EMistHeroId::Alphonse)
    {
        SaveStatus = TEXT("The Necrotic Greatsword belongs to Alphonse. Anima's equipment slots are empty.");
        return false;
    }
    if (!CanItemFitSlot(ItemID, Slot))
    {
        SaveStatus = TEXT("That item does not fit the selected equipment slot.");
        return false;
    }
    return EquipReward(true);
}

bool UMistProgressionComponent::WriteSaveEnvelope(UMistCryptSaveGame* Save, const FString& Slot)
{
    TArray<uint8> Payload;
    if (!Save || !UGameplayStatics::SaveGameToMemory(Save, Payload)
        || Payload.Num() < 64 || Payload.Num() > MaxSaveBytes) return false;
    uint32 Magic = EnvelopeMagic;
    uint32 Version = EnvelopeVersion;
    uint32 PayloadLength = static_cast<uint32>(Payload.Num());
    uint32 Checksum = FCrc::MemCrc32(Payload.GetData(), Payload.Num());
    TArray<uint8> Data;
    FMemoryWriter Writer(Data, true);
    Writer << Magic << Version << PayloadLength << Checksum;
    Data.Append(Payload);
    return !Writer.IsError() && UGameplayStatics::SaveDataToSlot(Data, Slot, 0);
}

void UMistProgressionComponent::RestoreValidatedSave(const UMistCryptSaveGame& Save)
{
    CheckpointID = Save.CheckpointID;
    ActiveHero = Save.ActiveHero;
    AnimaProfile = Save.AnimaProfile;
    LoadoutMask = Save.LoadoutMask;
    bAbyssUnlocked = Save.bAbyssUnlocked;
    bCryptCompleted = Save.bCryptCompleted;
    bRewardClaimed = Save.bRewardClaimed;
    bRewardEquipped = Save.EquippedItemID == GetRewardItemID();
    CooldownSeconds = Save.CooldownSeconds;
    CooldownSnapshotUtcTicks = Save.SavedUtcTicks;
    LastKnownSaveRevision = Save.SaveRevision;
}

bool UMistProgressionComponent::LoadProgression()
{
    if (!CanChangeProgression()) return false;
    ResetToSafeDefaults();
    if (!UGameplayStatics::DoesSaveGameExist(SaveSlot, 0))
    {
        SaveStatus = TEXT("New crypt journey. Progress saves at checkpoints.");
        return false;
    }
    FString Reason;
    if (const UMistCryptSaveGame* Save = ReadValidatedSave(Reason))
    {
        RestoreValidatedSave(*Save);
        SaveStatus = TEXT("Saved crypt progress restored.");
        return true;
    }
    SaveStatus = Reason + TEXT(". Started safely at the entrance.");
    return false;
}

double UMistProgressionComponent::ElapsedSince(int64 UtcTicks)
{
    // Moving the computer clock backwards must never replenish or shorten a cooldown.
    const int64 Now = FDateTime::UtcNow().GetTicks();
    if (UtcTicks <= 0 || Now <= UtcTicks) return 0.0;
    return static_cast<double>(Now - UtcTicks) / ETimespan::TicksPerSecond;
}

TArray<float> UMistProgressionComponent::GetCooldowns() const
{
    TArray<float> Result = ActiveHero == EMistHeroId::Anima ? AnimaProfile.CooldownSeconds : CooldownSeconds;
    const double Elapsed = ElapsedSince(ActiveHero == EMistHeroId::Anima ? AnimaProfile.SavedUtcTicks : CooldownSnapshotUtcTicks);
    for (float& Value : Result)
        Value = static_cast<float>(FMath::Max(0.0, static_cast<double>(Value) - Elapsed));
    return Result;
}

bool UMistProgressionComponent::SetCooldowns(const TArray<float>& Seconds)
{
    if (!CanChangeProgression()) return false;
    if (!ValidateCooldowns(Seconds)) return false;
    if (ActiveHero == EMistHeroId::Anima)
    {
        AnimaProfile.CooldownSeconds = Seconds;
        AnimaProfile.SavedUtcTicks = FDateTime::UtcNow().GetTicks();
    }
    else
    {
        CooldownSeconds = Seconds;
        CooldownSnapshotUtcTicks = FDateTime::UtcNow().GetTicks();
    }
    return true;
}

bool UMistProgressionComponent::SaveProgression()
{
    if (!CanChangeProgression()) return false;
    FString ExistingReason;
    if (const UMistCryptSaveGame* Existing = ReadValidatedSave(ExistingReason))
    {
        // Another controller or PIE session may already have committed newer progress.
        // Never let an autosave from an older instance erase completion or earned loot.
        if (Existing->SaveRevision != LastKnownSaveRevision
            || (Existing->bCryptCompleted && !bCryptCompleted)
            || (Existing->bRewardClaimed && !bRewardClaimed))
        {
            SaveStatus = TEXT("Newer crypt progress exists. Reload progress before saving; the existing save was preserved.");
            return false;
        }
    }
    UMistCryptSaveGame* Save = NewObject<UMistCryptSaveGame>();
    Save->SaveRevision = LastKnownSaveRevision + 1;
    Save->CheckpointID = CheckpointID;
    Save->ActiveHero = ActiveHero;
    Save->AnimaProfile = AnimaProfile;
    Save->LoadoutMask = LoadoutMask;
    Save->bAbyssUnlocked = bAbyssUnlocked;
    Save->bCryptCompleted = bCryptCompleted;
    Save->bRewardClaimed = bRewardClaimed;
    Save->OwnedItemID = bRewardClaimed ? GetRewardItemID() : NAME_None;
    Save->EquippedItemID = bRewardEquipped ? GetRewardItemID() : NAME_None;
    // Store each profile with its own timestamp. Inactive cooldowns continue to
    // expire; changing character or saving cannot refresh a remaining cooldown.
    Save->CooldownSeconds = CooldownSeconds;
    const double AlphonseElapsed = ElapsedSince(CooldownSnapshotUtcTicks);
    const double AnimaElapsed = ElapsedSince(AnimaProfile.SavedUtcTicks);
    for (float& Value : Save->CooldownSeconds)
        Value = static_cast<float>(FMath::Max(0.0, static_cast<double>(Value) - AlphonseElapsed));
    for (float& Value : Save->AnimaProfile.CooldownSeconds)
        Value = static_cast<float>(FMath::Max(0.0, static_cast<double>(Value) - AnimaElapsed));
    Save->SavedUtcTicks = Save->AnimaProfile.SavedUtcTicks = FDateTime::UtcNow().GetTicks();
    FString Reason;
    if (!ValidateSave(*Save, Reason))
    {
        SaveStatus = Reason + TEXT(". Progress was not saved.");
        return false;
    }
    if (bForceSaveFailureForTesting || !WriteSaveEnvelope(Save, SaveSlot))
    {
        SaveStatus = TEXT("Save failed. No reward or equipment change was committed; retry at a checkpoint.");
        return false;
    }
    CooldownSeconds = Save->CooldownSeconds;
    CooldownSnapshotUtcTicks = Save->SavedUtcTicks;
    AnimaProfile = Save->AnimaProfile;
    LastKnownSaveRevision = Save->SaveRevision;
    SaveStatus = TEXT("Crypt progress saved locally.");
    return true;
}

bool UMistProgressionComponent::ClaimCryptReward()
{
    if (!CanChangeProgression()) return false;
    if (bRewardClaimed)
    {
        SaveStatus = TEXT("This crypt reward has already been claimed.");
        return false;
    }
    if (!bCryptCompleted)
    {
        SaveStatus = TEXT("Defeat the crypt guardian before claiming its reward.");
        return false;
    }
    // A stale controller cannot award a second copy after another instance saved it.
    FString Reason;
    if (const UMistCryptSaveGame* Existing = ReadValidatedSave(Reason))
    {
        if (Existing->bRewardClaimed)
        {
            RestoreValidatedSave(*Existing);
            SaveStatus = TEXT("This crypt reward has already been claimed.");
            return false;
        }
    }
    bRewardClaimed = true;
    if (!SaveProgression())
    {
        bRewardClaimed = false;
        return false;
    }
    SaveStatus = ActiveHero == EMistHeroId::Anima
        ? TEXT("Necrotic Greatsword received for Alphonse. Switch to him at a checkpoint to equip it.")
        : TEXT("Necrotic Greatsword received and saved. Open inventory to equip it.");
    return true;
}

bool UMistProgressionComponent::EquipReward(bool bEquip)
{
    if (!CanChangeProgression()) return false;
    if (ActiveHero != EMistHeroId::Alphonse)
    {
        SaveStatus = TEXT("Anima cannot equip Alphonse's greatsword reward.");
        return false;
    }
    if (!bRewardClaimed)
    {
        SaveStatus = TEXT("The Necrotic Greatsword has not been earned.");
        return false;
    }
    if (bRewardEquipped == bEquip) return true;
    const bool Previous = bRewardEquipped;
    bRewardEquipped = bEquip;
    if (!SaveProgression())
    {
        bRewardEquipped = Previous;
        return false;
    }
    SaveStatus = bEquip ? TEXT("Necrotic Greatsword equipped: +5% ability damage. Saved.")
        : TEXT("Necrotic Greatsword unequipped. Saved.");
    return true;
}

bool UMistProgressionComponent::SetCryptCompleted()
{
    if (!CanChangeProgression()) return false;
    bCryptCompleted = true;
    return true;
}

bool UMistProgressionComponent::SetCheckpoint(FName ID)
{
    if (!CanChangeProgression()) return false;
    if (!IsKnownCheckpoint(ID) || (ID == TEXT("Crypt.Reward") && !bCryptCompleted)) return false;
    CheckpointID = ID;
    return true;
}

bool UMistProgressionComponent::SetLoadoutMask(uint8 Mask)
{
    if (!CanChangeProgression()) return false;
    if (Mask > 15 || (Mask != 0 && !IsAbyssUnlocked())) return false;
    if (ActiveHero == EMistHeroId::Anima) AnimaProfile.LoadoutMask = Mask;
    else LoadoutMask = Mask;
    return true;
}

bool UMistProgressionComponent::SetAbyssUnlocked(bool bUnlocked)
{
    if (!CanChangeProgression()) return false;
    if (!bUnlocked && GetLoadoutMask() != 0) return false;
    if (ActiveHero == EMistHeroId::Anima) AnimaProfile.bAscensionUnlocked = bUnlocked;
    else bAbyssUnlocked = bUnlocked;
    return true;
}

bool UMistProgressionComponent::SetSaveSlotForTesting(const FString& Slot)
{
    if (!CanChangeProgression()) return false;
    // Flat names only; tests cannot escape Saved/SaveGames or select the real player slot.
    if (!Slot.StartsWith(TEXT("Mistworld.Test.")) || Slot.Len() > 120) return false;
    for (const TCHAR Character : Slot)
        if (!FChar::IsAlnum(Character) && Character != '.' && Character != '_' && Character != '-') return false;
    SaveSlot = Slot;
    ResetToSafeDefaults();
    SaveStatus = TEXT("Isolated automation save slot selected.");
    return true;
}
