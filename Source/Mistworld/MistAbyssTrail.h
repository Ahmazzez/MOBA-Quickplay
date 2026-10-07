#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MistAbyssTrail.generated.h"

class AMistTrainingDummy;
class APawn;
class UMistCombatHealth;

// One actor is one W cast. Segments age independently; internal overlap cannot
// multiply its DOT. An empowered trail retains R attribution after the recast
// window ends, but permanently loses healing when that caster life ends.
UCLASS()
class MISTWORLD_API AMistAbyssTrail : public AActor
{
    GENERATED_BODY()
public:
    AMistAbyssTrail();
    void Initialize(APawn* Caster, bool bEmpowered);
    void AddSegment(const FVector& From, const FVector& To);
    void FinishAddingSegments();
    void DisableCasterBenefits();
    virtual void Tick(float DeltaSeconds) override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    int32 GetSegmentCount() const { return Segments.Num(); }
    float GetTrailRadius() const { return Radius; }
    bool HasUltimateHealing() const { return HealingFraction > 0.f && bCasterBenefitsAllowed; }

private:
    struct FTrailSegment
    {
        FVector From = FVector::ZeroVector;
        FVector To = FVector::ZeroVector;
        double BornAt = 0.0;
        int32 NextTick = 1;
    };
    struct FTrailTick
    {
        int32 SegmentIndex = INDEX_NONE;
        double Time = 0.0;
    };
    bool OccupiesSegment(AMistTrainingDummy* Target, const FTrailSegment& Segment) const;
    void ResolveTick(const FTrailSegment& Segment, double TickTime);
    void DrawSegment(const FTrailSegment& Segment, double Now) const;
    TWeakObjectPtr<APawn> CasterPawn;
    TWeakObjectPtr<UMistCombatHealth> SourceHealth;
    TArray<FTrailSegment> Segments;
    TMap<TWeakObjectPtr<AMistTrainingDummy>, double> LastDamageTick;
    float Radius = 75.f;
    float Duration = 4.f;
    float TickDamage = 20.f;
    float HealingFraction = 0.f;
    float GroundOffset = 90.f;
    bool bInitialized = false;
    bool bFinishedAdding = false;
    bool bCasterBenefitsAllowed = true;
};
