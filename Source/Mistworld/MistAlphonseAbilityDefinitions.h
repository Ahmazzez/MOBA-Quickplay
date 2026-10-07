#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h"

// Values are world-space centimetres/seconds and pre-mitigation damage. Slots
// resolve independently so a mixed loadout never silently inherits another
// slot's ascension. The existing playtest adjustment keeps ordinary W at 15 m/s.
struct MISTWORLD_API FMistAlphonseAbilityDefinition
{
    FString DisplayName;
    float Range = 0.f;
    float Radius = 0.f;
    float ConeAngle = 0.f;
    float DirectDamage = 0.f;
    float CastTime = 0.f;
    float Cooldown = 0.f;
    float TravelSpeed = 0.f;
    float Duration = 0.f;
    float ReductionFraction = 0.f;
    float MoveAttackSpeedBonus = 0.f;
    float ShockwaveRange = 0.f;
    float ShockwaveRadius = 0.f;
    float ShockwaveSpeed = 0.f;
    float ShockwaveDamage = 0.f;
    float TrailRadius = 0.f;
    float TrailDuration = 0.f;
    float TrailDamage = 0.f;
    float UltimateHealingFraction = 0.f;
    bool bMagicDamage = false;
};

struct MISTWORLD_API FMistAlphonseAbilityDefinitions
{
    static FMistAlphonseAbilityDefinition Get(const FKey& Slot, bool bAbyss,
        bool bEmpowered = false);
};
