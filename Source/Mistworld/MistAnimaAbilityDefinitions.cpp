#include "MistAnimaAbilityDefinitions.h"

FMistAlphonseAbilityDefinition FMistAnimaAbilityDefinitions::Get(const FKey& Slot, bool bFlame)
{
    FMistAlphonseAbilityDefinition Def;
    if (Slot == EKeys::Q)
    {
        Def.DisplayName = bFlame ? TEXT("Brand") : TEXT("Mark");
        Def.Range = 300.f; Def.Radius = 30.f; Def.DirectDamage = 70.f;
        Def.CastTime = 0.15f; Def.Cooldown = 2.f; Def.Duration = 4.f;
    }
    else if (Slot == EKeys::W)
    {
        Def.DisplayName = bFlame ? TEXT("Ember Steps") : TEXT("Duelist Steps");
        Def.Range = 300.f; Def.Radius = 45.f; Def.DirectDamage = 45.f;
        Def.CastTime = 0.08f; Def.TravelSpeed = 1600.f;
        Def.Cooldown = 12.f; Def.Duration = 5.f;
    }
    else if (Slot == EKeys::E)
    {
        Def.DisplayName = bFlame ? TEXT("Flame Stance") : TEXT("Riposte");
        Def.Range = 300.f; Def.Radius = 30.f; Def.DirectDamage = bFlame ? 0.f : 110.f;
        Def.CastTime = 0.f; Def.Cooldown = 15.f; Def.Duration = 1.f;
        Def.ReductionFraction = bFlame ? 0.f : 0.70f;
    }
    else if (Slot == EKeys::R)
    {
        Def.DisplayName = bFlame ? TEXT("Inflamed Soul") : TEXT("Anima Stealer");
        Def.Range = bFlame ? 300.f : 900.f; Def.Radius = bFlame ? 300.f : 50.f;
        Def.DirectDamage = bFlame ? 0.f : 200.f; Def.CastTime = 0.20f;
        Def.TravelSpeed = bFlame ? 0.f : 2200.f; Def.Cooldown = 110.f;
        Def.Duration = bFlame ? 8.f : 7.f;
    }
    return Def;
}
