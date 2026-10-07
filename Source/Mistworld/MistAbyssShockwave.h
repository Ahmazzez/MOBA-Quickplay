#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MistAbyssShockwave.generated.h"

class AMistTrainingDummy;
class APawn;

// A released Abyss Q continues after its caster dies. It can damage each enemy
// once, and the direct sword strike's targets are excluded for this whole cast.
UCLASS()
class MISTWORLD_API AMistAbyssShockwave : public AActor
{
    GENERATED_BODY()
public:
    AMistAbyssShockwave();
    void Initialize(APawn* Caster, const FVector& Direction,
        const TSet<TWeakObjectPtr<AMistTrainingDummy>>& ExcludedDirectHits);
    virtual void Tick(float DeltaSeconds) override;

    int32 GetHitCount() const { return ShockwaveHits; }
    static constexpr float TravelSpeed = 1400.f;
    static constexpr float MaximumRange = 800.f;
    static constexpr float HitRadius = 75.f;
    static constexpr float Damage = 80.f;

private:
    void DamageAlongSegment(const FVector& Start, const FVector& End);
    bool SweepTerrain(const FVector& Start, const FVector& End, FHitResult& Hit) const;
    TWeakObjectPtr<APawn> CasterPawn;
    TSet<TWeakObjectPtr<AMistTrainingDummy>> HitActors;
    FVector TravelDirection = FVector::ForwardVector;
    float TravelledDistance = 0.f;
    int32 ShockwaveHits = 0;
    bool bInitialized = false;
    float AbilityMultiplier = 1.f;
};
