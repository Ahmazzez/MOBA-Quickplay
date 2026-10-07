#include "MistArenaEnemy.h"

#include "MistCombatHealth.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "NavigationPath.h"
#include "NavigationSystem.h"
#include "UObject/ConstructorHelpers.h"

AMistArenaEnemy::AMistArenaEnemy()
{
    CombatHealth->MaxHealth = MaximumHealth;
    CombatHealth->Armor = CombatHealth->MagicResistance = 20.f;
    Label->SetWorldSize(23.f);
    Label->SetTextRenderColor(FColor(255, 120, 80));
    static ConstructorHelpers::FObjectFinder<UMaterialInterface> Material(
        TEXT("/Game/LevelPrototyping/Materials/M_FlatCol.M_FlatCol"));
    if (Material.Succeeded()) Visual->SetMaterial(0, Material.Object);
}

void AMistArenaEnemy::BeginPlay()
{
    Super::BeginPlay();
    HomeLocation = GetActorLocation();
    CombatHealth->MaxHealth = MaximumHealth;
    CombatHealth->ResetHealth();
    PreviousHealth = CombatHealth->Health;
    bReportedMovementFailure = false;
    if (UMaterialInstanceDynamic* Material = Visual->CreateDynamicMaterialInstance(0))
    {
        TArray<FMaterialParameterInfo> Parameters;
        TArray<FGuid> ParameterIds;
        Material->GetAllVectorParameterInfo(Parameters, ParameterIds);
        for (const FMaterialParameterInfo& Parameter : Parameters)
        {
            const FString Name = Parameter.Name.ToString().ToLower();
            if (Name.Contains(TEXT("color")) || Name.Contains(TEXT("colour")) || Name.Contains(TEXT("tint")))
                Material->SetVectorParameterValue(Parameter.Name, FLinearColor(0.5f, 0.055f, 0.025f));
        }
    }
    UpdateLabel();
}

FString AMistArenaEnemy::GetStateName() const
{
    switch (State)
    {
    case ECombatState::Approach: return TEXT("Approach");
    case ECombatState::Windup: return TEXT("Windup");
    case ECombatState::Recovery: return TEXT("Recovery");
    case ECombatState::Returning: return TEXT("Returning");
    case ECombatState::Down: return TEXT("Down");
    default: return TEXT("Idle");
    }
}

float AMistArenaEnemy::GetStateRemaining() const
{
    return GetWorld() ? FMath::Max(0.f, static_cast<float>(StateUntil - GetWorld()->GetTimeSeconds())) : 0.f;
}

APawn* AMistArenaEnemy::FindPlayerTarget() const
{
    return UGameplayStatics::GetPlayerPawn(this, 0);
}

bool AMistArenaEnemy::CanTarget(const APawn* Target) const
{
    if (!IsValid(Target)) return false;
    const UMistCombatHealth* Health = Target->FindComponentByClass<UMistCombatHealth>();
    return Health && Health->IsAlive() && !Health->IsInvulnerable()
        && FVector::DistSquared2D(Target->GetActorLocation(), HomeLocation) <= FMath::Square(HomeLeash)
        && FVector::DistSquared2D(GetActorLocation(), HomeLocation) <= FMath::Square(HomeLeash)
        && FMath::Abs(Target->GetActorLocation().Z - GetActorLocation().Z) <= 160.f;
}

bool AMistArenaEnemy::HasTerrainLineOfSight(const AActor* Target) const
{
    if (!IsValid(Target)) return false;
    FCollisionObjectQueryParams Objects;
    Objects.AddObjectTypesToQuery(ECC_WorldStatic);
    Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
    FCollisionQueryParams Params(SCENE_QUERY_STAT(MistEnemyVisibility), false, this);
    Params.AddIgnoredActor(Target);
    FHitResult Hit;
    // Other units cannot become accidental walls or shields.
    return !GetWorld()->LineTraceSingleByObjectType(Hit, GetActorLocation(), Target->GetActorLocation(), Objects, Params);
}

bool AMistArenaEnemy::IsTargetInsideAttack(AActor* Target) const
{
    if (!IsValid(Target) || !HasTerrainLineOfSight(Target)) return false;
    const FVector Delta = Target->GetActorLocation() - GetActorLocation();
    if (FMath::Abs(Delta.Z) > 160.f) return false;
    const FVector Point(Delta.X, Delta.Y, 0.f);
    const float Distance = Point.Size();
    const UCapsuleComponent* TargetCapsule = Target->FindComponentByClass<UCapsuleComponent>();
    const float Radius = TargetCapsule ? TargetCapsule->GetScaledCapsuleRadius() : 0.f;
    if (Distance <= Radius) return true;
    const float Angle = FMath::RadiansToDegrees(FMath::Acos(
        FMath::Clamp(FVector::DotProduct(Point / Distance, CommittedDirection), -1.f, 1.f)));
    if (Angle <= AttackHalfAngle) return Distance <= AttackRange + Radius;
    const FVector EdgeA = CommittedDirection.RotateAngleAxis(AttackHalfAngle, FVector::UpVector) * AttackRange;
    const FVector EdgeB = CommittedDirection.RotateAngleAxis(-AttackHalfAngle, FVector::UpVector) * AttackRange;
    return FMath::PointDistToSegment(Point, FVector::ZeroVector, EdgeA) <= Radius
        || FMath::PointDistToSegment(Point, FVector::ZeroVector, EdgeB) <= Radius;
}

void AMistArenaEnemy::ClearPath()
{
    PathPoints.Reset();
    PathIndex = 0;
    NextPathRefresh = 0.0;
}

void AMistArenaEnemy::BeginWindup(const APawn* Target)
{
    CommittedDirection = (Target->GetActorLocation() - GetActorLocation()).GetSafeNormal2D();
    if (CommittedDirection.IsNearlyZero()) CommittedDirection = GetActorForwardVector().GetSafeNormal2D();
    SetActorRotation(CommittedDirection.Rotation());
    State = ECombatState::Windup;
    StateUntil = GetWorld()->GetTimeSeconds() + AttackWindup;
    ClearPath();
}

void AMistArenaEnemy::BeginRecovery()
{
    State = ECombatState::Recovery;
    StateUntil = GetWorld()->GetTimeSeconds() + AttackRecovery;
    ClearPath();
}

void AMistArenaEnemy::BeginReturn()
{
    State = ECombatState::Returning;
    StateUntil = 0.0;
    Capsule->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
    CombatHealth->GrantInvulnerability(0.5f);
    CombatHealth->GrantDebuffImmunity(0.5f);
    ClearPath();
}

void AMistArenaEnemy::ResetEncounter()
{
    if (!HasAuthority() || !HasActorBegunPlay()) return;
    SetActorLocation(HomeLocation, false);
    Capsule->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
    CombatHealth->ResetHealth();
    PreviousHealth = CombatHealth->Health;
    State = ECombatState::Idle;
    bReportedMovementFailure = false;
    StateUntil = 0.0;
    CommittedDirection = GetActorForwardVector().GetSafeNormal2D();
    ClearPath();
    UpdateLabel();
}

void AMistArenaEnemy::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (!HasAuthority() || !GetWorld()) return;
    const bool bRecentlyDamaged = CombatHealth->Health < PreviousHealth - KINDA_SMALL_NUMBER;
    PreviousHealth = CombatHealth->Health;
    if (!CombatHealth->IsAlive())
    {
        State = ECombatState::Down;
        StateUntil = 0.0;
        Capsule->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
        ClearPath();
        UpdateLabel();
        return;
    }

    const double Now = GetWorld()->GetTimeSeconds();
    APawn* Target = FindPlayerTarget();
    if (State == ECombatState::Returning)
    {
        // An evading enemy cannot be chipped down or repeatedly re-aggroed on its way home.
        CombatHealth->GrantInvulnerability(0.5f);
        CombatHealth->GrantDebuffImmunity(0.5f);
        if (FVector::DistSquared2D(GetActorLocation(), HomeLocation) <= FMath::Square(25.f))
        {
            CombatHealth->ResetHealth();
            PreviousHealth = CombatHealth->Health;
            Capsule->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
            State = ECombatState::Idle;
            StateUntil = 0.0;
            ClearPath();
        }
        else if (!CombatHealth->IsRooted() && !CombatHealth->IsStunned())
            MoveAlongPath(HomeLocation, DeltaSeconds);
        UpdateLabel();
        return;
    }

    if (!CanTarget(Target))
    {
        if (State != ECombatState::Idle) BeginReturn();
        UpdateLabel();
        return;
    }

    if (State == ECombatState::Idle)
    {
        if (!CombatHealth->IsStunned()
            && (bRecentlyDamaged || FVector::DistSquared2D(GetActorLocation(), Target->GetActorLocation()) <= FMath::Square(AggroRange))
            && HasTerrainLineOfSight(Target)) State = ECombatState::Approach;
    }

    if (State == ECombatState::Windup)
    {
        if (CombatHealth->IsStunned()) BeginRecovery();
        else
        {
            DrawAttackSector(FColor(255, 130, 30), 0.f);
            if (Now >= StateUntil)
            {
                if (IsTargetInsideAttack(Target))
                    Target->FindComponentByClass<UMistCombatHealth>()->ReceivePhysicalDamage(AttackRawDamage, this, EMistDamageKind::BasicAttack);
                DrawAttackSector(FColor::Red, 0.15f);
                BeginRecovery();
            }
        }
    }
    else if (State == ECombatState::Recovery)
    {
        if (Now >= StateUntil) State = ECombatState::Approach;
    }
    else if (State == ECombatState::Approach && !CombatHealth->IsStunned())
    {
        const UCapsuleComponent* TargetCapsule = Target->FindComponentByClass<UCapsuleComponent>();
        const float Radius = TargetCapsule ? TargetCapsule->GetScaledCapsuleRadius() : 0.f;
        if (FVector::DistSquared2D(GetActorLocation(), Target->GetActorLocation())
            <= FMath::Square(AttackRange + Radius - 5.f) && HasTerrainLineOfSight(Target))
            BeginWindup(Target);
        else if (!CombatHealth->IsRooted()) MoveAlongPath(Target->GetActorLocation(), DeltaSeconds);
    }
    UpdateLabel();
}

void AMistArenaEnemy::MoveAlongPath(const FVector& Goal, float DeltaSeconds)
{
    if (DeltaSeconds <= 0.f) return;
    const double Now = GetWorld()->GetTimeSeconds();
    if (Now >= NextPathRefresh)
    {
        ClearPath();
        NextPathRefresh = Now + 0.35;
        const float HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
        UNavigationPath* Path = UNavigationSystemV1::FindPathToLocationSynchronously(GetWorld(),
            GetActorLocation() - FVector(0.f, 0.f, HalfHeight), Goal - FVector(0.f, 0.f, HalfHeight), this);
        if (Path && Path->IsValid() && !Path->IsPartial() && Path->PathPoints.Num() >= 2)
        {
            PathPoints = Path->PathPoints;
            PathIndex = 1;
        }
        else
        {
            ReportMovementFailure(FString::Printf(TEXT("no complete navigation path valid=%d partial=%d points=%d start=%s goal=%s"),
                Path && Path->IsValid() ? 1 : 0, Path && Path->IsPartial() ? 1 : 0, Path ? Path->PathPoints.Num() : -1,
                *(GetActorLocation() - FVector(0.f, 0.f, HalfHeight)).ToString(), *(Goal - FVector(0.f, 0.f, HalfHeight)).ToString()));
            return;
        }
    }
    if (PathIndex >= PathPoints.Num()) return;

    float RemainingTravel = MovementSpeed * DeltaSeconds;
    while (RemainingTravel > KINDA_SMALL_NUMBER && PathIndex < PathPoints.Num())
    {
        const FVector Delta = PathPoints[PathIndex] - GetActorLocation();
        const float Distance = Delta.Size2D();
        if (Distance <= 1.f) { ++PathIndex; continue; }
        const FVector Direction = Delta.GetSafeNormal2D();
        const float Travel = FMath::Min(RemainingTravel, Distance);
        const FVector Start = GetActorLocation();
        if (!MoveOnGround(Start + Direction * Travel))
        {
            // Retry a fresh path on the next update rather than tunnelling through terrain.
            PathPoints.Reset();
            PathIndex = 0;
            NextPathRefresh = Now + 0.35;
            break;
        }
        SetActorRotation(Direction.Rotation());
        const float Moved = FVector::Dist2D(Start, GetActorLocation());
        RemainingTravel -= Moved;
        if (Moved + 0.1f < Travel)
        {
            PathPoints.Reset();
            PathIndex = 0;
            NextPathRefresh = Now + 0.35;
            break;
        }
        if (Distance <= Travel + 1.f) ++PathIndex;
    }
}

bool AMistArenaEnemy::MoveOnGround(const FVector& DesiredLocation)
{
    UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
    if (!Nav) return ReportMovementFailure(FString::Printf(TEXT("no navigation system desired=%s"), *DesiredLocation.ToString()));
    const float HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
    const float Radius = Capsule->GetScaledCapsuleRadius();
    const FVector Start = GetActorLocation();
    FCollisionObjectQueryParams Objects;
    Objects.AddObjectTypesToQuery(ECC_WorldStatic);
    Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
    FCollisionQueryParams Params(SCENE_QUERY_STAT(MistEnemyGround), false, this);
    // Small terrain samples also keep large/hitch frames from stepping across gaps.
    const int32 Samples = FMath::Max(1, FMath::CeilToInt(FVector::Dist2D(Start, DesiredLocation) / 25.f));
    FVector Previous = Start;
    for (int32 Step = 1; Step <= Samples; ++Step)
    {
        FVector Candidate = FMath::Lerp(Start, DesiredLocation, static_cast<float>(Step) / Samples);
        Candidate.Z = Previous.Z;
        FNavLocation Projected;
        // Navigation points describe feet on the floor. A broad query can find the nearest polygon,
        // but the unchanged 15cm acceptance limit still rejects gaps and off-mesh shortcuts.
        const FVector Feet = Candidate - FVector(0.f, 0.f, HalfHeight);
        const bool bProjected = Nav->ProjectPointToNavigation(Feet, Projected, FVector(50.f, 50.f, 45.f));
        if (!bProjected || FVector::Dist2D(Candidate, Projected.Location) > 15.f)
            return ReportMovementFailure(FString::Printf(TEXT("navigation projection failed success=%d candidate=%s projected=%s distance2D=%.2f desired=%s"),
                bProjected ? 1 : 0, *Candidate.ToString(), *Projected.Location.ToString(),
                FVector::Dist2D(Candidate, Projected.Location), *DesiredLocation.ToString()));
        FHitResult Ground;
        const bool bGroundHit = GetWorld()->LineTraceSingleByObjectType(Ground, Candidate + FVector(0.f, 0.f, 45.f),
            Candidate - FVector(0.f, 0.f, HalfHeight + 65.f), Objects, Params);
        if (!bGroundHit || Ground.ImpactNormal.Z < 0.7f)
            return ReportMovementFailure(FString::Printf(TEXT("ground trace failed hit=%d candidate=%s projected=%s desired=%s"),
                bGroundHit ? 1 : 0, *Candidate.ToString(), *Projected.Location.ToString(), *DesiredLocation.ToString()), &Ground);
        Candidate.Z = Ground.ImpactPoint.Z + HalfHeight + 2.f;
        if (FMath::Abs(Candidate.Z - Previous.Z) > 45.f)
            return ReportMovementFailure(FString::Printf(TEXT("terrain height exceeds step limit previous=%s candidate=%s"),
                *Previous.ToString(), *Candidate.ToString()), &Ground);
        FHitResult Wall;
        if (GetWorld()->SweepSingleByObjectType(Wall, Previous, Candidate, FQuat::Identity, Objects,
            FCollisionShape::MakeCapsule(FMath::Max(1.f, Radius - 1.f), FMath::Max(Radius, HalfHeight - 2.f)), Params))
            return ReportMovementFailure(FString::Printf(TEXT("terrain capsule sweep blocked from=%s to=%s projected=%s desired=%s"),
                *Previous.ToString(), *Candidate.ToString(), *Projected.Location.ToString(), *DesiredLocation.ToString()), &Wall);
        Previous = Candidate;
    }
    FHitResult UnitCollision;
    SetActorLocation(Previous, true, &UnitCollision);
    if (UnitCollision.bStartPenetrating || FVector::DistSquared2D(Start, GetActorLocation()) <= KINDA_SMALL_NUMBER)
        return ReportMovementFailure(FString::Printf(TEXT("SetActorLocation failed start=%s endpoint=%s actual=%s desired=%s"),
            *Start.ToString(), *Previous.ToString(), *GetActorLocation().ToString(), *DesiredLocation.ToString()), &UnitCollision);
    return true;
}

bool AMistArenaEnemy::ReportMovementFailure(const FString& Context, const FHitResult* Hit)
{
    if (!bReportedMovementFailure)
    {
        bReportedMovementFailure = true;
        UE_LOG(LogTemp, Display, TEXT("Sentinel movement blocked: %s; actor=%s state=%s location=%s capsuleRadius=%.2f halfHeight=%.2f cachedPathPoints=%d pathIndex=%d nextRefresh=%.3f worldTime=%.3f blocker=%s startPenetrating=%d impact=%s normal=%s"),
            *Context, *GetName(), *GetStateName(), *GetActorLocation().ToString(), Capsule->GetScaledCapsuleRadius(), Capsule->GetScaledCapsuleHalfHeight(),
            PathPoints.Num(), PathIndex, NextPathRefresh, GetWorld()->GetTimeSeconds(),
            Hit ? *GetNameSafe(Hit->GetActor()) : TEXT("none"), Hit && Hit->bStartPenetrating ? 1 : 0,
            Hit ? *Hit->ImpactPoint.ToString() : TEXT("none"), Hit ? *Hit->ImpactNormal.ToString() : TEXT("none"));
    }
    return false;
}

void AMistArenaEnemy::DrawAttackSector(const FColor& Color, float Lifetime) const
{
    constexpr int32 Segments = 24;
    const FVector Origin = GetActorLocation() - FVector(0.f, 0.f, Capsule->GetScaledCapsuleHalfHeight() - 4.f);
    TArray<FVector> Vertices;
    TArray<int32> Indices;
    Vertices.Reserve(Segments + 2);
    Indices.Reserve(Segments * 3);
    Vertices.Add(Origin);
    for (int32 Index = 0; Index <= Segments; ++Index)
    {
        const float Angle = -AttackHalfAngle + AttackHalfAngle * 2.f * Index / Segments;
        Vertices.Add(Origin + CommittedDirection.RotateAngleAxis(Angle, FVector::UpVector) * AttackRange);
        if (Index > 0)
        {
            Indices.Add(0); Indices.Add(Index); Indices.Add(Index + 1);
            DrawDebugLine(GetWorld(), Vertices[Index], Vertices[Index + 1], Color, false, Lifetime, 0, 3.f);
        }
    }
    DrawDebugLine(GetWorld(), Origin, Vertices[1], Color, false, Lifetime, 0, 3.f);
    DrawDebugLine(GetWorld(), Origin, Vertices.Last(), Color, false, Lifetime, 0, 3.f);
    DrawDebugMesh(GetWorld(), Vertices, Indices, FColor(Color.R, Color.G, Color.B, 35), false, Lifetime);
}

void AMistArenaEnemy::UpdateLabel()
{
    FString Action;
    switch (State)
    {
    case ECombatState::Windup: Action = FString::Printf(TEXT("DODGE! %.1fs"), GetStateRemaining()); break;
    case ECombatState::Recovery: Action = FString::Printf(TEXT("Recovering %.1fs"), GetStateRemaining()); break;
    case ECombatState::Returning: Action = TEXT("Returning home"); break;
    case ECombatState::Down: Action = TEXT("DEFEATED"); break;
    case ECombatState::Approach: Action = TEXT("Approaching"); break;
    default: Action = TEXT("Patrol sentinel"); break;
    }
    Label->SetText(FText::FromString(FString::Printf(TEXT("Sentinel %.0f / %.0f\n%s"),
        CombatHealth->Health, CombatHealth->MaxHealth, *Action)));
    Label->SetTextRenderColor(State == ECombatState::Windup ? FColor::Orange
        : State == ECombatState::Down ? FColor(120, 120, 120) : FColor(255, 120, 80));
}
