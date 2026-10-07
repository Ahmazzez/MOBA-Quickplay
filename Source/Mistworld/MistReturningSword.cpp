#include "MistReturningSword.h"

#include "MistCombatHealth.h"
#include "MistTrainingDummy.h"
#include "CollisionQueryParams.h"
#include "CollisionShape.h"
#include "Components/CapsuleComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "Misc/PackageName.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
FCollisionObjectQueryParams SwordTerrainObjects()
{
    FCollisionObjectQueryParams Objects;
    Objects.AddObjectTypesToQuery(ECC_WorldStatic);
    Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
    return Objects;
}
}

AMistReturningSword::AMistReturningSword()
{
    PrimaryActorTick.bCanEverTick = true;
    SetReplicates(true);
    SetReplicateMovement(true);
    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("SwordRoot"));
    Visual = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("SwordPlaceholder"));
    Visual->SetupAttachment(RootComponent);
    Visual->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Visual->SetCanEverAffectNavigation(false);
    Visual->SetRelativeScale3D(FVector(1.4f, 0.18f, 0.12f));
    static ConstructorHelpers::FObjectFinder<UStaticMesh> Mesh(TEXT("/Engine/BasicShapes/Cube.Cube"));
    if (Mesh.Succeeded())
    {
        Visual->SetStaticMesh(Mesh.Object);
    }
    // Cosmetic mesh and material only. The existing sphere sweep, range,
    // damage timers and blocked-wall return logic remain authoritative.
    const TCHAR* WeaponPackage = TEXT("/Game/Mistworld/Characters/Alphonse/SM_Alphonse_Greatsword");
    if (FPackageName::DoesPackageExist(WeaponPackage))
        if (UStaticMesh* Greatsword = LoadObject<UStaticMesh>(nullptr,
            TEXT("/Game/Mistworld/Characters/Alphonse/SM_Alphonse_Greatsword.SM_Alphonse_Greatsword")))
        {
            Visual->SetStaticMesh(Greatsword);
            Visual->SetRelativeScale3D(FVector::OneVector);
            Visual->SetRelativeLocation(FVector(-55.f, 0.f, 30.f));
        }
}

void AMistReturningSword::Initialize(APawn* Caster, const FVector& Direction)
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
    OutboundDirection = Direction.GetSafeNormal2D();
    SetActorLocation(Caster->GetActorLocation());
    SetActorRotation(OutboundDirection.Rotation());
    OutboundDistance = 0.f;
    bReturning = false;
    bInitialized = true;
    OutboundHits.Reset();
    ReturnHits.Reset();
    // A destroyed caster or an unreachable return must not leave an actor alive.
    SetLifeSpan(10.f);
}

int32 AMistReturningSword::GetHitsOnLeg(bool bReturnLeg) const
{
    return bReturnLeg ? ReturnHits.Num() : OutboundHits.Num();
}

void AMistReturningSword::DisableCasterBenefits()
{
    // Already released damage finishes, but this life cannot grant healing or
    // an empowered recast to a later life of the same pawn.
    bCasterBenefitsAllowed = false;
    OnReturned.Unbind();
}

bool AMistReturningSword::SweepTerrain(const FVector& Start, const FVector& End, FHitResult& Hit) const
{
    FCollisionQueryParams Params(SCENE_QUERY_STAT(MistSwordTerrain), false, this);
    Params.AddIgnoredActor(CasterPawn.Get());
    // Dummies use the Pawn object type; terrain alone stops the sword.
    return GetWorld()->SweepSingleByObjectType(Hit, Start, End, FQuat::Identity,
        SwordTerrainObjects(), FCollisionShape::MakeSphere(HitRadius), Params);
}

void AMistReturningSword::DamageAlongSegment(const FVector& Start, const FVector& End)
{
    TSet<TWeakObjectPtr<AMistTrainingDummy>>& HitActors = bReturning ? ReturnHits : OutboundHits;
    UMistCombatHealth* CasterHealth = CasterPawn.IsValid()
        ? CasterPawn->FindComponentByClass<UMistCombatHealth>() : nullptr;

    for (TActorIterator<AMistTrainingDummy> It(GetWorld()); It; ++It)
    {
        AMistTrainingDummy* Dummy = *It;
        if (HitActors.Contains(Dummy) || !Dummy->CombatHealth || !Dummy->CombatHealth->IsAlive() || !Dummy->Capsule)
        {
            continue;
        }

        const float TargetRadius = Dummy->Capsule->GetScaledCapsuleRadius();
        const float AxisHalfLength = FMath::Max(0.f, Dummy->Capsule->GetScaledCapsuleHalfHeight() - TargetRadius);
        const FVector Center = Dummy->Capsule->GetComponentLocation();
        const FVector Axis = Dummy->Capsule->GetUpVector() * AxisHalfLength;
        FVector SweepPoint;
        FVector TargetAxisPoint;
        FMath::SegmentDistToSegmentSafe(Start, End, Center - Axis, Center + Axis, SweepPoint, TargetAxisPoint);
        if (FVector::DistSquared(SweepPoint, TargetAxisPoint) > FMath::Square(HitRadius + TargetRadius))
        {
            continue;
        }

        // A capsule can be within the swept radius on the far side of a thin
        // wall. Test the contact direction too, so that cannot hit through it.
        const FVector TowardSword = (SweepPoint - TargetAxisPoint).GetSafeNormal();
        const FVector TargetSurface = TargetAxisPoint + TowardSword * TargetRadius;
        FHitResult Occluder;
        FCollisionQueryParams VisibilityParams(SCENE_QUERY_STAT(MistSwordContactVisibility), false, this);
        VisibilityParams.AddIgnoredActor(CasterPawn.Get());
        if (GetWorld()->LineTraceSingleByObjectType(Occluder, SweepPoint, TargetSurface,
            SwordTerrainObjects(), VisibilityParams))
        {
            continue;
        }

        HitActors.Add(Dummy);
        const float RemovedHealth = Dummy->CombatHealth->ReceivePhysicalDamage(DamagePerLeg * AbilityMultiplier, CasterPawn.Get());
        if (bCasterBenefitsAllowed && CasterHealth && CasterHealth->IsAlive() && RemovedHealth > 0.f)
        {
            CasterHealth->Heal(RemovedHealth * 0.2f, CasterHealth);
        }
        DrawDebugSphere(GetWorld(), TargetSurface, 22.f, 12,
            bReturning ? FColor::Cyan : FColor::Orange, false, 0.2f, 0, 2.f);
    }
}

void AMistReturningSword::DrawSword(const FVector& Direction) const
{
    DrawDebugBox(GetWorld(), GetActorLocation(), FVector(70.f, 10.f, 7.f), Direction.Rotation().Quaternion(),
        bReturning ? FColor::Cyan : FColor::Yellow, false, 0.08f, 0, 3.f);
}

void AMistReturningSword::FinishReturn()
{
    OnReturned.ExecuteIfBound();
    Destroy();
}

void AMistReturningSword::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (!HasAuthority() || !bInitialized || DeltaSeconds <= 0.f)
    {
        return;
    }
    if (!CasterPawn.IsValid())
    {
        Destroy();
        return;
    }

    float RemainingTime = DeltaSeconds;
    // At most one outbound segment and one return segment are needed, even
    // when a long frame reaches the turn point halfway through the update.
    for (int32 Segment = 0; Segment < 2 && RemainingTime > SMALL_NUMBER; ++Segment)
    {
        const FVector Start = GetActorLocation();
        const FVector ReturnOffset = CasterPawn->GetActorLocation() - Start;
        const float DistanceLeft = bReturning ? ReturnOffset.Size() : OutboundRange - OutboundDistance;
        if (DistanceLeft <= KINDA_SMALL_NUMBER)
        {
            if (bReturning)
            {
                FinishReturn();
                return;
            }
            bReturning = true;
            continue;
        }

        const FVector Direction = bReturning ? ReturnOffset / DistanceLeft : OutboundDirection;
        const float Step = FMath::Min(DistanceLeft, TravelSpeed * RemainingTime);
        const FVector DesiredEnd = Start + Direction * Step;
        FHitResult TerrainHit;
        const bool bBlocked = SweepTerrain(Start, DesiredEnd, TerrainHit);
        const FVector ContactEnd = bBlocked ? TerrainHit.Location : DesiredEnd;
        const float Travelled = FVector::Dist(Start, ContactEnd);
        FVector End = ContactEnd;
        const float ClearedOutboundDistance = OutboundDistance + Travelled;

        if (bBlocked && !bReturning && ClearedOutboundDistance > KINDA_SMALL_NUMBER)
        {
            // A sweep ends with the sword touching the wall. Starting the
            // return sweep there can report another zero-time collision and
            // destroy the sword as if its return were obstructed. Retreat
            // along the already clear outbound segment before turning, with
            // enough clearance for glancing contacts as well as head-on hits.
            // Include earlier frames: an exact contact at the previous frame
            // boundary can produce a zero-distance hit in this frame.
            constexpr float WallClearance = 2.f;
            const FVector ContactNormal = TerrainHit.ImpactNormal.IsNearlyZero()
                ? TerrainHit.Normal : TerrainHit.ImpactNormal;
            const float Approach = FMath::Max(SMALL_NUMBER,
                static_cast<float>(-FVector::DotProduct(Direction, ContactNormal)));
            const float Retreat = FMath::Min(ClearedOutboundDistance,
                (WallClearance + FMath::Max(0.f, TerrainHit.PenetrationDepth)) / Approach);
            End -= Direction * Retreat;
        }

        // Resolve the full outbound path up to contact. The separating offset
        // is numerical clearance, not a shorter damage range.
        DamageAlongSegment(Start, ContactEnd);
        SetActorLocation(End);
        SetActorRotation(Direction.Rotation());
        DrawDebugLine(GetWorld(), Start, ContactEnd, bReturning ? FColor::Cyan : FColor::Yellow, false, 0.18f, 0, 3.f);
        DrawSword(Direction);
        RemainingTime = FMath::Max(0.f, RemainingTime - Travelled / TravelSpeed);

        if (bReturning)
        {
            if (bBlocked)
            {
                // Ending here is intentional: the returning sword never
                // teleports through terrain to grant an empowered recast.
                DrawDebugSphere(GetWorld(), TerrainHit.ImpactPoint, HitRadius, 16, FColor::Red, false, 0.3f, 0, 2.f);
                Destroy();
                return;
            }
            if (Step >= DistanceLeft - KINDA_SMALL_NUMBER)
            {
                FinishReturn();
                return;
            }
        }
        else
        {
            OutboundDistance += Travelled;
            if (bBlocked || OutboundDistance >= OutboundRange - KINDA_SMALL_NUMBER)
            {
                bReturning = true;
            }
        }
    }
}
