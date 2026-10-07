#pragma once

#include "CoreMinimal.h"
#include "MistEquipmentSlots.generated.h"

// Alphonse's actual equipment slots. Empty slots do not imply an item or a stat bonus.
UENUM(BlueprintType)
enum class EMistEquipmentSlot : uint8
{
    GreatswordBlade UMETA(DisplayName="Greatsword Blade"),
    GreatswordGuard UMETA(DisplayName="Greatsword Guard"),
    HeavyHelmet UMETA(DisplayName="Heavy Helmet"),
    HeavyChestplate UMETA(DisplayName="Heavy Chestplate"),
    HeavyBoots UMETA(DisplayName="Heavy Boots"),
    Rapier UMETA(DisplayName="Rapier"),
    ParryingDagger UMETA(DisplayName="Parrying Dagger"),
    LightHelmet UMETA(DisplayName="Light Helmet"),
    LightChestplate UMETA(DisplayName="Light Chestplate"),
    LightBoots UMETA(DisplayName="Light Boots"),
    Invalid = 255 UMETA(Hidden)
};
