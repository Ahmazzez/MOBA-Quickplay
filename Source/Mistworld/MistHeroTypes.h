#pragma once

#include "CoreMinimal.h"
#include "MistHeroTypes.generated.h"

UENUM(BlueprintType)
enum class EMistHeroId : uint8
{
    Alphonse,
    Anima
};

inline bool IsKnownMistHero(EMistHeroId Hero)
{
    return Hero == EMistHeroId::Alphonse || Hero == EMistHeroId::Anima;
}

inline FName GetMistHeroID(EMistHeroId Hero)
{
    return Hero == EMistHeroId::Anima ? FName(TEXT("Anima")) : FName(TEXT("Alphonse"));
}

inline FString GetMistHeroDisplayName(EMistHeroId Hero)
{
    return Hero == EMistHeroId::Anima ? TEXT("Anima") : TEXT("Alphonse");
}
