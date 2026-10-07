#include "MistCryptGuardian.h"

#include "MistCryptGolem.h"
#include "MistCombatHealth.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
const FName GuardianWardSource(TEXT("Crypt.GuardianWard"));
const FColor ElementColors[] = { FColor(255, 115, 35), FColor(75, 190, 255), FColor(115, 235, 205) };

void TintGuardian(UStaticMeshComponent* Mesh, const FLinearColor& Color)
{
    UMaterialInstanceDynamic* Material = Cast<UMaterialInstanceDynamic>(Mesh->GetMaterial(0));
    if (!Material) Material = Mesh->CreateDynamicMaterialInstance(0);
    if (!Material) return;
    TArray<FMaterialParameterInfo> Parameters;
    TArray<FGuid> ParameterIds;
    Material->GetAllVectorParameterInfo(Parameters, ParameterIds);
    for (const FMaterialParameterInfo& Parameter : Parameters)
    {
        const FString Name = Parameter.Name.ToString().ToLower();
        if (Name.Contains(TEXT("color")) || Name.Contains(TEXT("colour")) || Name.Contains(TEXT("tint")))
            Material->SetVectorParameterValue(Parameter.Name, Color);
    }
}
}

AMistCryptGuardian::AMistCryptGuardian()
{
    CombatHealth->MaxHealth = MaximumHealth;
    CombatHealth->Armor = CombatHealth->MagicResistance = 20.f;
    CombatHealth->bCountsForUltimateRefund = false;
    Label->SetWorldSize(22.f);
    Label->SetRelativeLocation(FVector(0.f, 0.f, 155.f));
    static ConstructorHelpers::FObjectFinder<UMaterialInterface> Material(
        TEXT("/Game/LevelPrototyping/Materials/M_FlatCol.M_FlatCol"));
    if (Material.Succeeded()) Visual->SetMaterial(0, Material.Object);
}

void AMistCryptGuardian::Initialize(AMistCryptGolem* Golem, APawn* Player, int32 ElementIndex, float InitialDelay)
{
    if (!HasAuthority() || !GetWorld() || !IsValid(Golem)) return;
    Encounter = Golem;
    EncounterPlayer = Player;
    Element = FMath::Clamp(ElementIndex, 0, 2);
    ConfigureElement();
    HomeLocation = GetActorLocation();
    State = EGuardianState::Waiting;
    StateUntil = GetWorld()->GetTimeSeconds() + FMath::Max(0.f, InitialDelay);
    RemainingChargeTravel = 0.f;
    FireAreaUntil = NextFirePulseAt = 0.0;
    FireAreaCenter = HomeLocation;
    bChargeHitPlayer = false;
    bInitialized = true;
    CombatHealth->ResetHealth();
    CombatHealth->ClearCombatLock();
    CombatHealth->GrantDebuffImmunity(86400.f);
    CombatHealth->OnDefeated.RemoveAll(this);
    CombatHealth->OnDefeated.AddUObject(this, &AMistCryptGuardian::HandleDefeated);
    // Geometry and roman symbols make the three elements recognizable without color.
    const TCHAR* MeshPaths[] = {
        TEXT("/Engine/BasicShapes/Cube.Cube"), TEXT("/Engine/BasicShapes/Sphere.Sphere"), TEXT("/Engine/BasicShapes/Cone.Cone")
    };
    if (UStaticMesh* Shape = LoadObject<UStaticMesh>(nullptr, MeshPaths[Element])) Visual->SetStaticMesh(Shape);
    const FVector Scales[] = { FVector(0.8f, 0.8f, 1.7f), FVector(1.f, 1.f, 1.7f), FVector(1.f, 1.f, 1.8f) };
    Visual->SetRelativeScale3D(Scales[Element]);
    TintGuardian(Visual, FLinearColor(ElementColors[Element]) * 0.5f);
    UpdateProtection();
    UpdatePresentation();
}

void AMistCryptGuardian::ConfigureElement()
{
    // Every wave gets the same elemental profile; restarting never retains another profile's tuning.
    CombatHealth->MaxHealth = MaximumHealth;
    CombatHealth->Armor = CombatHealth->MagicResistance = 20.f;
    ChargeRange = 850.f;
    ChargeSpeed = 900.f;
    ChargeRadius = 65.f;
    ChargeDamage = 120.f;
    ChargeWindup = 1.2f;
    ChargeRecovery = 6.f;
    ReturnMovementSpeed = ReturnSpeed;
    if (Element == 0)
    {
        CombatHealth->MaxHealth = FireMaximumHealth;
        ChargeSpeed = 700.f;
        ChargeDamage = 45.f;
        ChargeWindup = 1.4f;
    }
    else if (Element == 2)
    {
        CombatHealth->MaxHealth = WindMaximumHealth;
        CombatHealth->Armor = CombatHealth->MagicResistance = 0.f;
        ChargeRange = 1000.f;
        ChargeSpeed = 1450.f;
        ChargeDamage = 80.f;
        ChargeWindup = 0.8f;
        ChargeRecovery = 4.f;
        ReturnMovementSpeed = 600.f;
    }
}

FString AMistCryptGuardian::GetElementName() const
{
    switch (Element)
    {
    case 1: return TEXT("Ice [II]");
    case 2: return TEXT("Wind [III]");
    default: return TEXT("Fire [I]");
    }
}

FString AMistCryptGuardian::GetStateName() const
{
    switch (State)
    {
    case EGuardianState::Windup: return TEXT("Windup");
    case EGuardianState::Charging: return TEXT("Charging");
    case EGuardianState::FireWindup: return TEXT("FireWindup");
    case EGuardianState::Recovery: return TEXT("Recovery");
    case EGuardianState::Returning: return TEXT("Returning");
    case EGuardianState::Down: return TEXT("Down");
    default: return TEXT("Waiting");
    }
}

float AMistCryptGuardian::GetFireAreaRemaining() const
{
    return GetWorld() ? FMath::Max(0.f, static_cast<float>(FireAreaUntil - GetWorld()->GetTimeSeconds())) : 0.f;
}

float AMistCryptGuardian::GetStateRemaining() const
{
    if (!GetWorld()) return 0.f;
    if (State == EGuardianState::Charging)
        return RemainingChargeTravel / FMath::Max(1.f, ChargeSpeed);
    return FMath::Max(0.f, static_cast<float>(StateUntil - GetWorld()->GetTimeSeconds()));
}

bool AMistCryptGuardian::IsWardProtected() const
{
    return Encounter.IsValid() && Encounter->IsEncounterActive()
        && FVector::DistSquared2D(GetActorLocation(), Encounter->GetHomeLocation())
            <= FMath::Square(Encounter->GetProtectiveRadius());
}

void AMistCryptGuardian::UpdateProtection()
{
    if (!CombatHealth->IsAlive()) return;
    CombatHealth->SetDamageReduction(GuardianWardSource, IsWardProtected() ? WardReduction : 0.f,
        IsWardProtected() ? 86400.f : 0.f);
    CombatHealth->GrantDebuffImmunity(1.f);
}

bool AMistCryptGuardian::CanTargetPlayer() const
{
    if (!Encounter.IsValid() || !Encounter->IsEncounterActive() || !EncounterPlayer.IsValid()) return false;
    const APawn* Player = EncounterPlayer.Get();
    const UMistCombatHealth* Health = Player->FindComponentByClass<UMistCombatHealth>();
    const FVector Position = Player->GetActorLocation();
    return Health && Health->IsAlive() && !Health->IsInvulnerable()
        && Position.X >= -740.f && Position.X <= 2440.f && FMath::Abs(Position.Y) <= 1540.f
        && FMath::Abs(Position.Z - GetActorLocation().Z) <= 160.f;
}

bool AMistCryptGuardian::HasTerrainLineOfSight(const AActor* Target) const
{
    if (!IsValid(Target)) return false;
    FCollisionObjectQueryParams Objects;
    Objects.AddObjectTypesToQuery(ECC_WorldStatic);
    Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
    FCollisionQueryParams Query(SCENE_QUERY_STAT(MistGuardianVisibility), false, this);
    Query.AddIgnoredActor(Target);
    FHitResult Hit;
    return !GetWorld()->LineTraceSingleByObjectType(Hit, GetActorLocation(), Target->GetActorLocation(), Objects, Query);
}

void AMistCryptGuardian::BeginChargeWindup()
{
    CommittedDirection = (EncounterPlayer->GetActorLocation() - GetActorLocation()).GetSafeNormal2D();
    if (CommittedDirection.IsNearlyZero()) CommittedDirection = GetActorForwardVector().GetSafeNormal2D();
    SetActorRotation(CommittedDirection.Rotation());
    ChargeStart = GetActorLocation();
    State = EGuardianState::Windup;
    StateUntil = GetWorld()->GetTimeSeconds() + FMath::Max(0.1f, ChargeWindup);
    RemainingChargeTravel = FMath::Max(0.f, ChargeRange);
    bChargeHitPlayer = false;
    DrawChargeLane(FColor::Orange, 0.f);
}

void AMistCryptGuardian::BeginRecovery()
{
    State = EGuardianState::Recovery;
    StateUntil = GetWorld()->GetTimeSeconds() + FMath::Max(0.1f, ChargeRecovery);
    RemainingChargeTravel = 0.f;
    Capsule->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
    UpdateProtection();
}

void AMistCryptGuardian::FinishCharge()
{
    if (Element != 0) { BeginRecovery(); return; }
    // The fire guardian commits its blast to its landing point, including when terrain stops its charge.
    // This second tell gives melee characters time to leave before the large area attack releases.
    FireAreaCenter = GetActorLocation();
    State = EGuardianState::FireWindup;
    StateUntil = GetWorld()->GetTimeSeconds() + FMath::Max(0.1f, FireWindup);
    RemainingChargeTravel = 0.f;
    Capsule->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
    DrawFireArea(FColor::Orange, false);
    UpdateProtection();
}

void AMistCryptGuardian::HandleDefeated()
{
    if (!HasAuthority() || State == EGuardianState::Down) return;
    State = EGuardianState::Down;
    RemainingChargeTravel = 0.f;
    StateUntil = 0.0;
    FireAreaUntil = NextFirePulseAt = 0.0;
    Capsule->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
    TintGuardian(Visual, FLinearColor(0.055f, 0.055f, 0.055f));
    UpdatePresentation();
    if (Encounter.IsValid()) Encounter->NotifyGuardianDefeated(this);
}

bool AMistCryptGuardian::MoveOnGround(const FVector& DesiredLocation, float SweepRadius)
{
    if (!GetWorld()) return false;
    const FVector Start = GetActorLocation();
    const float HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
    FCollisionObjectQueryParams Objects;
    Objects.AddObjectTypesToQuery(ECC_WorldStatic);
    Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
    FCollisionQueryParams Query(SCENE_QUERY_STAT(MistGuardianGround), false, this);
    const int32 Samples = FMath::Max(1, FMath::CeilToInt(FVector::Dist2D(Start, DesiredLocation) / 25.f));
    FVector Last = Start;
    bool bComplete = true;
    for (int32 Sample = 1; Sample <= Samples; ++Sample)
    {
        FVector Candidate = FMath::Lerp(Start, DesiredLocation, static_cast<float>(Sample) / Samples);
        Candidate.Z = Last.Z;
        FHitResult Ground;
        if (!GetWorld()->LineTraceSingleByObjectType(Ground, Candidate + FVector(0.f, 0.f, 45.f),
            Candidate - FVector(0.f, 0.f, HalfHeight + 65.f), Objects, Query) || Ground.ImpactNormal.Z < 0.7f)
        { bComplete = false; break; }
        Candidate.Z = Ground.ImpactPoint.Z + HalfHeight + 2.f;
        if (FMath::Abs(Candidate.Z - Last.Z) > 45.f) { bComplete = false; break; }
        FHitResult Wall;
        if (GetWorld()->SweepSingleByObjectType(Wall, Last, Candidate, FQuat::Identity, Objects,
            FCollisionShape::MakeCapsule(FMath::Max(1.f, SweepRadius), FMath::Max(SweepRadius, HalfHeight - 2.f)), Query))
        {
            if (!Wall.bStartPenetrating)
                Last = FMath::Lerp(Last, Candidate, FMath::Max(0.f, Wall.Time - 0.02f));
            bComplete = false;
            break;
        }
        Last = Candidate;
    }
    // Only world geometry blocks scripted charge/return motion. Units are swept for damage separately.
    SetActorLocation(Last, false, nullptr, ETeleportType::TeleportPhysics);
    UpdateProtection();
    return bComplete;
}

void AMistCryptGuardian::ApplyChargeHit(const FVector& From, const FVector& To)
{
    if (bChargeHitPlayer || !CanTargetPlayer() || !HasTerrainLineOfSight(EncounterPlayer.Get())) return;
    APawn* Player = EncounterPlayer.Get();
    const UCapsuleComponent* PlayerCapsule = Player->FindComponentByClass<UCapsuleComponent>();
    const float PlayerRadius = PlayerCapsule ? PlayerCapsule->GetScaledCapsuleRadius() : 0.f;
    const FVector Position = Player->GetActorLocation();
    if (FMath::Abs(Position.Z - From.Z) > 160.f) return;
    const FVector PlanarPlayer(Position.X, Position.Y, From.Z);
    FVector PlanarTo(To.X, To.Y, From.Z);
    if (FMath::PointDistToSegment(PlanarPlayer, From, PlanarTo) > ChargeRadius + PlayerRadius) return;
    bChargeHitPlayer = true;
    if (UMistCombatHealth* Health = Player->FindComponentByClass<UMistCombatHealth>())
    {
        Health->ReceivePhysicalDamage(ChargeDamage, this);
        // A shield can absorb damage without avoiding a hit. The shared status rules decide immunity.
        if (Element == 1 && Health->IsAlive() && !Health->IsInvulnerable() && !Health->WasLastDamageBlocked())
            Health->SetStun(IceFreezeDuration);
    }
}

void AMistCryptGuardian::ApplyFireAreaDamage(float RawDamage, bool bDamageOverTime)
{
    if (!CanTargetPlayer() || !HasTerrainLineOfSight(EncounterPlayer.Get())) return;
    APawn* Player = EncounterPlayer.Get();
    const UCapsuleComponent* PlayerCapsule = Player->FindComponentByClass<UCapsuleComponent>();
    const float PlayerRadius = PlayerCapsule ? PlayerCapsule->GetScaledCapsuleRadius() : 0.f;
    if (FVector::DistSquared2D(Player->GetActorLocation(), FireAreaCenter)
        > FMath::Square(FMath::Max(0.f, FireAreaRadius) + PlayerRadius)) return;
    if (UMistCombatHealth* Health = Player->FindComponentByClass<UMistCombatHealth>())
        Health->ReceiveMagicDamage(RawDamage, this,
            bDamageOverTime ? EMistDamageKind::DamageOverTime : EMistDamageKind::Direct);
}

void AMistCryptGuardian::UpdateFirePool()
{
    if (FireAreaUntil <= 0.0) return;
    const double Now = GetWorld()->GetTimeSeconds();
    if (Now >= FireAreaUntil)
    {
        FireAreaUntil = NextFirePulseAt = 0.0;
        return;
    }
    DrawFireArea(FColor(255, 80, 20), true);
    if (Now >= NextFirePulseAt)
    {
        ApplyFireAreaDamage(FirePoolTickDamage, true);
        // Never apply accumulated unseen hits after a pause or a long frame.
        NextFirePulseAt = Now + FMath::Max(0.1f, FirePoolTickInterval);
    }
}

void AMistCryptGuardian::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (!HasAuthority() || !GetWorld() || !bInitialized || !Encounter.IsValid()) return;
    if (!CombatHealth->IsAlive())
    {
        if (State != EGuardianState::Down) HandleDefeated();
        UpdatePresentation();
        return;
    }
    UpdateProtection();
    if (!Encounter->IsEncounterActive()) { UpdatePresentation(); return; }
    const double Now = GetWorld()->GetTimeSeconds();
    UpdateFirePool();
    if (State == EGuardianState::Waiting)
    {
        if (Now >= StateUntil && CanTargetPlayer() && HasTerrainLineOfSight(EncounterPlayer.Get())) BeginChargeWindup();
    }
    else if (State == EGuardianState::Windup)
    {
        DrawChargeLane(FColor::Orange, 0.f);
        if (Now >= StateUntil)
        {
            State = EGuardianState::Charging;
            Capsule->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
            DrawChargeLane(FColor::Red, 0.1f);
        }
    }
    else if (State == EGuardianState::Charging)
    {
        const FVector From = GetActorLocation();
        const float Travel = FMath::Min(RemainingChargeTravel, FMath::Max(0.f, ChargeSpeed * DeltaSeconds));
        const bool bClear = MoveOnGround(From + CommittedDirection * Travel, ChargeRadius);
        const FVector To = GetActorLocation();
        ApplyChargeHit(From, To);
        RemainingChargeTravel = FMath::Max(0.f, RemainingChargeTravel - FVector::Dist2D(From, To));
        if (!bClear || RemainingChargeTravel <= 0.1f) FinishCharge();
    }
    else if (State == EGuardianState::FireWindup)
    {
        DrawFireArea(FColor::Orange, false);
        if (Now >= StateUntil)
        {
            ApplyFireAreaDamage(FireExplosionDamage);
            FireAreaUntil = Now + FMath::Max(0.f, FirePoolDuration);
            NextFirePulseAt = Now + FMath::Max(0.1f, FirePoolTickInterval);
            BeginRecovery();
        }
    }
    else if (State == EGuardianState::Recovery)
    {
        if (Now >= StateUntil)
        {
            State = EGuardianState::Returning;
            Capsule->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
        }
    }
    else if (State == EGuardianState::Returning)
    {
        const FVector Delta = HomeLocation - GetActorLocation();
        const float Distance = Delta.Size2D();
        if (Distance <= 1.f)
        {
            State = EGuardianState::Waiting;
            StateUntil = Now + (Element == 2 ? 1.1f : 1.5f + Element * 0.5f);
            Capsule->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
        }
        else if (DeltaSeconds > 0.f)
        {
            const FVector Direction = Delta.GetSafeNormal2D();
            MoveOnGround(GetActorLocation() + Direction * FMath::Min(Distance, ReturnMovementSpeed * DeltaSeconds),
                Capsule->GetScaledCapsuleRadius() - 1.f);
            SetActorRotation(Direction.Rotation());
        }
    }
    UpdatePresentation();
}

void AMistCryptGuardian::DrawChargeLane(const FColor& Color, float Lifetime) const
{
    if (!GetWorld()) return;
    const FVector Origin = ChargeStart - FVector(0.f, 0.f, Capsule->GetScaledCapsuleHalfHeight() - 5.f);
    FVector End = Origin + CommittedDirection * ChargeRange;
    FCollisionObjectQueryParams Terrain;
    Terrain.AddObjectTypesToQuery(ECC_WorldStatic);
    Terrain.AddObjectTypesToQuery(ECC_WorldDynamic);
    FCollisionQueryParams Query(SCENE_QUERY_STAT(MistGuardianTelegraph), false, this);
    FHitResult Wall;
    const float HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
    if (GetWorld()->SweepSingleByObjectType(Wall, ChargeStart,
        ChargeStart + CommittedDirection * ChargeRange, FQuat::Identity, Terrain,
        FCollisionShape::MakeCapsule(ChargeRadius, FMath::Max(ChargeRadius, HalfHeight - 2.f)), Query))
    {
        End = Origin + CommittedDirection * FMath::Max(0.f, Wall.Distance - 1.f);
    }
    const FVector Side = CommittedDirection.RotateAngleAxis(90.f, FVector::UpVector) * ChargeRadius;
    TArray<FVector> Vertices = { Origin - Side, Origin + Side, End + Side, End - Side };
    TArray<int32> Indices = { 0, 1, 2, 0, 2, 3 };
    DrawDebugMesh(GetWorld(), Vertices, Indices, FColor(Color.R, Color.G, Color.B, 35), false, Lifetime);
    for (int32 Index = 0; Index < 4; ++Index)
        DrawDebugLine(GetWorld(), Vertices[Index], Vertices[(Index + 1) % 4], Color, false, Lifetime, 0, 3.f);
    DrawDebugCircle(GetWorld(), Origin, ChargeRadius, 24, Color, false, Lifetime, 0, 2.f,
        FVector::ForwardVector, FVector::RightVector, false);
    DrawDebugCircle(GetWorld(), End, ChargeRadius, 24, Color, false, Lifetime, 0, 2.f,
        FVector::ForwardVector, FVector::RightVector, false);
    DrawDebugDirectionalArrow(GetWorld(), Origin, End, 35.f, Color, false, Lifetime, 0, 2.f);
}

void AMistCryptGuardian::DrawFireArea(const FColor& Color, bool bBurning) const
{
    if (!GetWorld()) return;
    const FVector Origin = FireAreaCenter - FVector(0.f, 0.f, Capsule->GetScaledCapsuleHalfHeight() - 6.f);
    const float Radius = FMath::Max(0.f, FireAreaRadius);
    constexpr int32 Segments = 40;
    TArray<FVector> Vertices;
    TArray<int32> Indices;
    Vertices.Add(Origin);
    for (int32 Index = 0; Index <= Segments; ++Index)
    {
        const float Angle = Index * 2.f * PI / Segments;
        Vertices.Add(Origin + FVector(FMath::Cos(Angle), FMath::Sin(Angle), 0.f) * Radius);
        if (Index > 0)
        {
            Indices.Add(0);
            Indices.Add(Index);
            Indices.Add(Index + 1);
        }
    }
    DrawDebugMesh(GetWorld(), Vertices, Indices, FColor(Color.R, Color.G, Color.B, bBurning ? 65 : 28), false, 0.f);
    DrawDebugCircle(GetWorld(), Origin, Radius, Segments, Color, false, 0.f, 0, 4.f,
        FVector::ForwardVector, FVector::RightVector, false);
    const float Fraction = bBurning
        ? GetFireAreaRemaining() / FMath::Max(0.1f, FirePoolDuration)
        : GetStateRemaining() / FMath::Max(0.1f, FireWindup);
    DrawDebugCircle(GetWorld(), Origin + FVector(0.f, 0.f, 1.f), Radius * FMath::Clamp(Fraction, 0.f, 1.f),
        Segments, Color, false, 0.f, 0, 2.f, FVector::ForwardVector, FVector::RightVector, false);
}

void AMistCryptGuardian::UpdatePresentation()
{
    FString Action;
    switch (State)
    {
    case EGuardianState::Windup:
        Action = FString::Printf(TEXT("%s %.1fs - dodge the locked lane!"),
            Element == 1 ? TEXT("FREEZE CHARGE") : Element == 2 ? TEXT("SWIFT CHARGE") : TEXT("FIRE CHARGE"),
            GetStateRemaining()); break;
    case EGuardianState::Charging: Action = Element == 1 ? TEXT("ICE CHARGE - freezes on hit") : TEXT("CHARGING"); break;
    case EGuardianState::FireWindup:
        Action = FString::Printf(TEXT("ERUPTION %.1fs - leave the orange circle!"), GetStateRemaining()); break;
    case EGuardianState::Recovery:
        Action = GetFireAreaRemaining() > 0.f
            ? FString::Printf(TEXT("%s - FIRE POOL %.1fs (%.1fs ticks)"),
                IsWardProtected() ? TEXT("Warded") : TEXT("EXPOSED"), GetFireAreaRemaining(), FirePoolTickInterval)
            : FString::Printf(TEXT("%s %.1fs"), IsWardProtected() ? TEXT("Still warded") : TEXT("EXPOSED - attack!"), GetStateRemaining()); break;
    case EGuardianState::Returning: Action = TEXT("Returning to ward"); break;
    case EGuardianState::Down: Action = TEXT("[X] SEAL EXTINGUISHED"); break;
    default: Action = Element == 0 ? TEXT("Area blasts - bait out, then dodge the eruption")
        : Element == 1 ? TEXT("Freezes on hit - sidestep its locked charge")
        : TEXT("Fast but frail - bait out, then burst it down"); break;
    }
    Label->SetText(FText::FromString(FString::Printf(TEXT("%s %.0f / %.0f%s\n%s"), *GetElementName(),
        CombatHealth->Health, CombatHealth->MaxHealth,
        CombatHealth->IsAlive() && IsWardProtected() ? TEXT(" [75% WARD]") : TEXT(""), *Action)));
    Label->SetTextRenderColor(State == EGuardianState::Down ? FColor(115, 115, 115)
        : State == EGuardianState::Windup || State == EGuardianState::FireWindup ? FColor::Orange : ElementColors[Element]);
}
