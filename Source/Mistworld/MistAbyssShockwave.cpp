#include "MistAbyssShockwave.h"

#include "MistCombatHealth.h"
#include "MistTrainingDummy.h"
#include "CollisionQueryParams.h"
#include "CollisionShape.h"
#include "Components/CapsuleComponent.h"
#include "Components/SceneComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"

namespace
{
FCollisionObjectQueryParams ShockwaveTerrainObjects()
{
    FCollisionObjectQueryParams Objects;
    Objects.AddObjectTypesToQuery(ECC_WorldStatic);
    Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
    return Objects;
}
}

AMistAbyssShockwave::AMistAbyssShockwave()
{
    PrimaryActorTick.bCanEverTick = true;
    SetReplicates(true);
    SetReplicateMovement(true);
    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("ShockwaveRoot"));
}

void AMistAbyssShockwave::Initialize(APawn* Caster, const FVector& Direction,
    const TSet<TWeakObjectPtr<AMistTrainingDummy>>& ExcludedDirectHits)
{
    if (!IsValid(Caster) || Direction.GetSafeNormal2D().IsNearlyZero())
    {
        Destroy();
        return;
    }
    CasterPawn = Caster;
    if (const UMistCombatHealth* Health = Caster->FindComponentByClass<UMistCombatHealth>())
        AbilityMultiplier = Health->AbilityDamageMultiplier;
    SetOwner(Caster);
    TravelDirection = Direction.GetSafeNormal2D();
    SetActorLocation(Caster->GetActorLocation());
    SetActorRotation(TravelDirection.Rotation());
    HitActors = ExcludedDirectHits;
    ShockwaveHits = 0;
    TravelledDistance = 0.f;
    bInitialized = true;
    SetLifeSpan(MaximumRange / TravelSpeed + 1.f);
}

bool AMistAbyssShockwave::SweepTerrain(const FVector& Start, const FVector& End,
    FHitResult& Hit) const
{
    FCollisionQueryParams Params(SCENE_QUERY_STAT(MistAbyssShockwaveTerrain), false, this);
    Params.AddIgnoredActor(CasterPawn.Get());
    return GetWorld()->SweepSingleByObjectType(Hit, Start, End, FQuat::Identity,
        ShockwaveTerrainObjects(), FCollisionShape::MakeSphere(HitRadius), Params);
}

void AMistAbyssShockwave::DamageAlongSegment(const FVector& Start, const FVector& End)
{
    for (TActorIterator<AMistTrainingDummy> It(GetWorld()); It; ++It)
    {
        AMistTrainingDummy* Target = *It;
        if (HitActors.Contains(Target) || !Target->CombatHealth || !Target->CombatHealth->IsAlive()
            || !Target->Capsule)
        {
            continue;
        }
        const float TargetRadius = Target->Capsule->GetScaledCapsuleRadius();
        const float AxisHalfLength = FMath::Max(0.f,
            Target->Capsule->GetScaledCapsuleHalfHeight() - TargetRadius);
        const FVector Center = Target->Capsule->GetComponentLocation();
        const FVector Axis = Target->Capsule->GetUpVector() * AxisHalfLength;
        FVector WavePoint;
        FVector TargetPoint;
        FMath::SegmentDistToSegmentSafe(Start, End, Center - Axis, Center + Axis,
            WavePoint, TargetPoint);
        if (FVector::DistSquared(WavePoint, TargetPoint) > FMath::Square(HitRadius + TargetRadius))
        {
            continue;
        }
        const FVector Surface = TargetPoint + (WavePoint - TargetPoint).GetSafeNormal() * TargetRadius;
        FHitResult Occluder;
        FCollisionQueryParams Params(SCENE_QUERY_STAT(MistAbyssShockwaveVisibility), false, this);
        Params.AddIgnoredActor(CasterPawn.Get());
        if (GetWorld()->LineTraceSingleByObjectType(Occluder, WavePoint, Surface,
            ShockwaveTerrainObjects(), Params))
        {
            continue;
        }
        HitActors.Add(Target);
        ++ShockwaveHits;
        Target->CombatHealth->ReceiveMagicDamage(Damage * AbilityMultiplier, CasterPawn.Get());
        DrawDebugSphere(GetWorld(), Surface, 20.f, 12, FColor(170, 100, 255), false, 0.20f, 0, 2.f);
    }
}

void AMistAbyssShockwave::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (!HasAuthority() || !bInitialized || DeltaSeconds <= 0.f) return;
    const FVector Start = GetActorLocation();
    const float Step = FMath::Min(MaximumRange - TravelledDistance, TravelSpeed * DeltaSeconds);
    const FVector DesiredEnd = Start + TravelDirection * FMath::Max(0.f, Step);
    FHitResult TerrainHit;
    const bool bBlocked = SweepTerrain(Start, DesiredEnd, TerrainHit);
    const FVector End = bBlocked ? TerrainHit.Location : DesiredEnd;
    DamageAlongSegment(Start, End);
    TravelledDistance += FVector::Dist(Start, End);
    SetActorLocation(End);
    DrawDebugBox(GetWorld(), End, FVector(16.f, HitRadius, 12.f),
        TravelDirection.Rotation().Quaternion(), FColor(165, 85, 255), false, 0.f, 0, 3.f);
    DrawDebugLine(GetWorld(), Start, End, FColor(110, 65, 210), false, 0.10f, 0, 6.f);
    if (bBlocked || TravelledDistance >= MaximumRange - KINDA_SMALL_NUMBER)
    {
        if (bBlocked)
            DrawDebugSphere(GetWorld(), TerrainHit.ImpactPoint, HitRadius, 16,
                FColor::Red, false, 0.25f, 0, 2.f);
        Destroy();
    }
}
