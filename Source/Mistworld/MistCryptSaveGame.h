#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "MistHeroTypes.h"
#include "MistCryptSaveGame.generated.h"

USTRUCT()
struct FMistHeroProgressProfile
{
    GENERATED_BODY()
    UPROPERTY(SaveGame) uint8 LoadoutMask = 0;
    UPROPERTY(SaveGame) bool bAscensionUnlocked = false;
    UPROPERTY(SaveGame) TArray<float> CooldownSeconds;
    UPROPERTY(SaveGame) int64 SavedUtcTicks = 0;

    FMistHeroProgressProfile()
    {
        CooldownSeconds.Init(0.f, 6);
        SavedUtcTicks = FDateTime::UtcNow().GetTicks();
    }
};

// IDs keep this prototype's equipment legal when more characters and slots arrive.
UCLASS()
class MISTWORLD_API UMistCryptSaveGame : public USaveGame
{
    GENERATED_BODY()
public:
    UMistCryptSaveGame();
    static constexpr int32 LegacyHeavyWeaponVersion = 1;
    static constexpr int32 SingleHeroVersion = 2;
    static constexpr int32 CurrentVersion = 3;
    UPROPERTY(SaveGame) int32 FormatVersion = CurrentVersion;
    UPROPERTY(SaveGame) int64 SaveRevision = 0;
    UPROPERTY(SaveGame) FName HeroID = TEXT("Alphonse");
    UPROPERTY(SaveGame) EMistHeroId ActiveHero = EMistHeroId::Alphonse;
    UPROPERTY(SaveGame) FMistHeroProgressProfile AnimaProfile;
    // Retain this serialized field name to migrate the shipped v1 HeavyWeapon save.
    // The sole prototype item occupies GreatswordBlade; the four other slots are empty.
    UPROPERTY(SaveGame) FName WeaponSlotID = TEXT("GreatswordBlade");
    UPROPERTY(SaveGame) FName CheckpointID = TEXT("Crypt.Entrance");
    // Keep the original fields as Alphonse's profile so v1/v2 saves retain gear,
    // ascension, loadout and offline cooldown time without a destructive migration.
    UPROPERTY(SaveGame) uint8 LoadoutMask = 0;
    UPROPERTY(SaveGame) bool bAbyssUnlocked = false;
    UPROPERTY(SaveGame) bool bCryptCompleted = false;
    UPROPERTY(SaveGame) bool bRewardClaimed = false;
    UPROPERTY(SaveGame) FName OwnedItemID = NAME_None;
    UPROPERTY(SaveGame) FName EquippedItemID = NAME_None;
    // Remaining seconds in Q, W, E, R, Flash, auto-attack order.
    UPROPERTY(SaveGame) TArray<float> CooldownSeconds;
    UPROPERTY(SaveGame) int64 SavedUtcTicks = 0;
};
