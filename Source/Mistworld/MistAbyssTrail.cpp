#include "MistAbyssTrail.h"

#include "MistAlphonseAbilityDefinitions.h"
#include "MistCombatHealth.h"
#include "MistTrainingDummy.h"
#include "CollisionQueryParams.h"
#include "Components/CapsuleComponent.h"
#include "Components/SceneComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"

namespace
{
FCollisionObjectQueryParams TrailTerrainObjects()
{
    FCollisionObjectQueryParams Objects;
    Objects.AddObjectTypesToQuery(ECC_WorldStatic);
    Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
    return Objects;
}
}

AMistAbyssTrail::AMistAbyssTrail()
{
    PrimaryActorTick.bCanEverTick = true;
    SetReplicates(true);
    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("AbyssTrailRoot"));
}

void AMistAbyssTrail::Initialize(APawn* Caster, bool bEmpowered)
{
    if (!IsValid(Caster))
    {
        Destroy();
        return;
    }
    CasterPawn = Caster;
    SetOwner(Caster);
    SetActorLocation(Caster->GetActorLocation());
    const FMistAlphonseAbilityDefinition Definition =
        FMistAlphonseAbilityDefinitions::Get(EKeys::W, true, bEmpowered);
    Radius = Definition.TrailRadius;
    Duration = Definition.TrailDuration;
    TickDamage = Definition.TrailDamage;
    HealingFraction = Definition.UltimateHealingFraction;
    if (UCapsuleComponent* Capsule = Caster->FindComponentByClass<UCapsuleComponent>())
        GroundOffset = Capsule->GetScaledCapsuleHalfHeight() - 6.f;
    SourceHealth = Caster->FindComponentByClass<UMistCombatHealth>();
    if (SourceHealth.IsValid())
    {
        TickDamage *= SourceHealth->AbilityDamageMultiplier;
        bCasterBenefitsAllowed = SourceHealth->IsAlive();
        SourceHealth->OnDefeated.AddUObject(this, &AMistAbyssTrail::DisableCasterBenefits);
    }
    Segments.Reset();
    LastDamageTick.Reset();
    bInitialized = true;
    bFinishedAdding = false;
    // Safety bound; normal destruction waits for the last segment's final tick.
    SetLifeSpan(Duration + 10.f);
}

void AMistAbyssTrail::AddSegment(const FVector& From, const FVector& To)
{
    if (!HasAuthority() || !bInitialized || bFinishedAdding || From.ContainsNaN() || To.ContainsNaN()
        || FVector::DistSquared(From, To) <= KINDA_SMALL_NUMBER)
    {
        return;
    }
    FTrailSegment& Segment = Segments.AddDefaulted_GetRef();
    Segment.From = From;
    Segment.To = To;
    Segment.BornAt = GetWorld()->GetTimeSeconds();
    Segment.NextTick = 1;
}

void AMistAbyssTrail::FinishAddingSegments()
{
    bFinishedAdding = true;
    if (Segments.IsEmpty()) Destroy();
}

void AMistAbyssTrail::DisableCasterBenefits()
{
    bCasterBenefitsAllowed = false;
}

void AMistAbyssTrail::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (SourceHealth.IsValid()) SourceHealth->OnDefeated.RemoveAll(this);
    Super::EndPlay(EndPlayReason);
}

bool AMistAbyssTrail::OccupiesSegment(AMistTrainingDummy* Target,
    const FTrailSegment& Segment) const
{
    if (!Target || !Target->Capsule || !Target->CombatHealth || !Target->CombatHealth->IsAlive())
        return false;
    const float TargetRadius = Target->Capsule->GetScaledCapsuleRadius();
    const float AxisHalfLength = FMath::Max(0.f,
        Target->Capsule->GetScaledCapsuleHalfHeight() - TargetRadius);
    const FVector Center = Target->Capsule->GetComponentLocation();
    const FVector Axis = Target->Capsule->GetUpVector() * AxisHalfLength;
    FVector TrailPoint;
    FVector TargetPoint;
    FMath::SegmentDistToSegmentSafe(Segment.From, Segment.To, Center - Axis, Center + Axis,
        TrailPoint, TargetPoint);
    if (FVector::DistSquared(TrailPoint, TargetPoint) > FMath::Square(Radius + TargetRadius)) return false;
    const FVector Surface = TargetPoint + (TrailPoint - TargetPoint).GetSafeNormal() * TargetRadius;
    FHitResult Occluder;
    FCollisionQueryParams Params(SCENE_QUERY_STAT(MistAbyssTrailVisibility), false, this);
    Params.AddIgnoredActor(CasterPawn.Get());
    return !GetWorld()->LineTraceSingleByObjectType(Occluder, TrailPoint, Surface,
        TrailTerrainObjects(), Params);
}

void AMistAbyssTrail::ResolveTick(const FTrailSegment& Segment, double TickTime)
{
    for (TActorIterator<AMistTrainingDummy> It(GetWorld()); It; ++It)
    {
        AMistTrainingDummy* Target = *It;
        if (!OccupiesSegment(Target, Segment)) continue;
        // Nearby segments are born in successive dash frames. Deduplicate by
        // target and logical tick time, not merely by segment or frame, so their
        // slightly offset schedules cannot turn one cast into dozens of ticks.
        const double* PreviousTick = LastDamageTick.Find(Target);
        if (PreviousTick && TickTime - *PreviousTick < 1.0 - 1.e-4) continue;
        LastDamageTick.Add(Target, TickTime);
        const float Removed = Target->CombatHealth->ReceiveMagicDamage(TickDamage, CasterPawn.Get(), EMistDamageKind::DamageOverTime);
        if (HealingFraction > 0.f && bCasterBenefitsAllowed && SourceHealth.IsValid()
            && SourceHealth->IsAlive() && Removed > 0.f)
        {
            SourceHealth->Heal(Removed * HealingFraction, SourceHealth.Get());
        }
    }
}

void AMistAbyssTrail::DrawSegment(const FTrailSegment& Segment, double Now) const
{
    const FVector Offset(0.f, 0.f, GroundOffset);
    const FVector From = Segment.From - Offset;
    const FVector To = Segment.To - Offset;
    const FVector Direction = (To - From).GetSafeNormal2D();
    const FVector Side(-Direction.Y * Radius, Direction.X * Radius, 0.f);
    const float Remaining = FMath::Clamp(static_cast<float>((Segment.BornAt + Duration - Now) / Duration), 0.f, 1.f);
    const FColor Color = FLinearColor(0.32f + Remaining * 0.18f, 0.12f, 0.45f + Remaining * 0.45f).ToFColor(true);
    DrawDebugLine(GetWorld(), From, To, Color, false, 0.f, 0, 12.f);
    DrawDebugLine(GetWorld(), From + Side, To + Side, Color, false, 0.f, 0, 1.5f);
    DrawDebugLine(GetWorld(), From - Side, To - Side, Color, false, 0.f, 0, 1.5f);
}

void AMistAbyssTrail::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (!HasAuthority() || !bInitialized) return;
    const double Now = GetWorld()->GetTimeSeconds();
    TArray<FTrailTick> DueTicks;
    for (int32 Index = 0; Index < Segments.Num(); ++Index)
    {
        FTrailSegment& Segment = Segments[Index];
        while (Segment.NextTick <= static_cast<int32>(Duration)
            && Segment.BornAt + Segment.NextTick <= Now + 1.e-4)
        {
            DueTicks.Add({Index, Segment.BornAt + Segment.NextTick});
            ++Segment.NextTick;
        }
    }
    DueTicks.Sort([](const FTrailTick& Left, const FTrailTick& Right)
    {
        return Left.Time < Right.Time;
    });
    for (const FTrailTick& Event : DueTicks) ResolveTick(Segments[Event.SegmentIndex], Event.Time);
    Segments.RemoveAll([Now, this](const FTrailSegment& Segment)
    {
        return Segment.NextTick > static_cast<int32>(Duration)
            && Now >= Segment.BornAt + Duration - 1.e-4;
    });
    for (const FTrailSegment& Segment : Segments) DrawSegment(Segment, Now);
    if (bFinishedAdding && Segments.IsEmpty()) Destroy();
}
