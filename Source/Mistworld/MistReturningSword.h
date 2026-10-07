#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MistReturningSword.generated.h"

class APawn;
class AMistTrainingDummy;
class UStaticMeshComponent;

DECLARE_DELEGATE(FMistSwordReturned);

// Alphonse's sword follows a fixed outbound line, then seeks the caster's
// current location. Collision and damage are resolved by swept geometry.
UCLASS()
class MISTWORLD_API AMistReturningSword : public AActor
{
    GENERATED_BODY()

public:
    AMistReturningSword();
    virtual void Tick(float DeltaSeconds) override;

    void Initialize(APawn* Caster, const FVector& Direction);
    void DisableCasterBenefits();
    FMistSwordReturned OnReturned;

    bool IsReturning() const { return bReturning; }
    int32 GetHitsOnLeg(bool bReturnLeg) const;

    static constexpr float TravelSpeed = 1800.f;
    static constexpr float OutboundRange = 1000.f;
    static constexpr float HitRadius = 60.f;
    static constexpr float DamagePerLeg = 160.f;

private:
    float AbilityMultiplier = 1.f;
    void DamageAlongSegment(const FVector& Start, const FVector& End);
    bool SweepTerrain(const FVector& Start, const FVector& End, FHitResult& Hit) const;
    void FinishReturn();
    void DrawSword(const FVector& Direction) const;

    UPROPERTY(VisibleAnywhere) TObjectPtr<UStaticMeshComponent> Visual;
    TWeakObjectPtr<APawn> CasterPawn;
    TSet<TWeakObjectPtr<AMistTrainingDummy>> OutboundHits;
    TSet<TWeakObjectPtr<AMistTrainingDummy>> ReturnHits;
    FVector OutboundDirection = FVector::ForwardVector;
    float OutboundDistance = 0.f;
    bool bInitialized = false;
    bool bReturning = false;
    bool bCasterBenefitsAllowed = true;
};
