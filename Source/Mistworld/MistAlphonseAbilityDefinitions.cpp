#include "MistAlphonseAbilityDefinitions.h"

FMistAlphonseAbilityDefinition FMistAlphonseAbilityDefinitions::Get(
    const FKey& Slot, bool bAbyss, bool bEmpowered)
{
    FMistAlphonseAbilityDefinition Value;
    if (Slot == EKeys::Q)
    {
        Value.DisplayName = bAbyss ? TEXT("Greatsword Quake") : TEXT("Sword Slash");
        Value.Range = bAbyss ? 250.f : 300.f;
        Value.ConeAngle = bAbyss ? 90.f : 110.f;
        Value.DirectDamage = bAbyss ? 150.f : 140.f;
        Value.CastTime = bAbyss ? 0.35f : 0.25f;
        Value.Cooldown = 6.f;
        if (bAbyss)
        {
            Value.ShockwaveRange = 800.f;
            Value.ShockwaveRadius = 75.f;
            Value.ShockwaveSpeed = 1400.f;
            Value.ShockwaveDamage = 80.f;
        }
    }
    else if (Slot == EKeys::W)
    {
        Value.DisplayName = bAbyss ? TEXT("Abyss Dash") : TEXT("Spinning Dash");
        Value.Range = bEmpowered ? 800.f : 600.f;
        Value.Radius = bEmpowered ? (bAbyss ? 80.f : 100.f) : (bAbyss ? 65.f : 75.f);
        Value.DirectDamage = bEmpowered ? (bAbyss ? 140.f : 180.f) : (bAbyss ? 80.f : 100.f);
        Value.CastTime = 0.10f;
        Value.Cooldown = 12.f;
        Value.TravelSpeed = bEmpowered ? 2200.f : 1500.f;
        if (bAbyss)
        {
            Value.TrailRadius = bEmpowered ? 100.f : 75.f;
            Value.TrailDuration = bEmpowered ? 5.f : 4.f;
            Value.TrailDamage = bEmpowered ? 30.f : 20.f;
        }
        Value.UltimateHealingFraction = bEmpowered ? 0.20f : 0.f;
    }
    else if (Slot == EKeys::E)
    {
        Value.DisplayName = bAbyss ? TEXT("Abyss Guard") : TEXT("Guard");
        Value.ReductionFraction = bAbyss ? 0.33f : 0.50f;
        Value.Duration = bAbyss ? 6.f : 5.f;
        Value.Cooldown = 30.f;
    }
    else if (Slot == EKeys::R)
    {
        Value.DisplayName = bAbyss ? TEXT("Abyss Warcry") : TEXT("Returning Sword");
        Value.Range = bAbyss ? 400.f : 1000.f;
        Value.Radius = bAbyss ? 400.f : 60.f;
        Value.DirectDamage = bAbyss ? 260.f : 160.f;
        Value.CastTime = bAbyss ? 0.40f : 0.35f;
        Value.Cooldown = 120.f;
        Value.TravelSpeed = bAbyss ? 0.f : 1800.f;
        Value.Duration = bAbyss ? 10.f : 0.f;
        Value.MoveAttackSpeedBonus = bAbyss ? 0.33f : 0.f;
        Value.bMagicDamage = bAbyss;
        Value.UltimateHealingFraction = bAbyss ? 0.f : 0.20f;
    }
    return Value;
}
