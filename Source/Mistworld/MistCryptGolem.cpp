#include "MistCryptGolem.h"

#include "MistCryptGuardian.h"
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
void SetCryptTint(UStaticMeshComponent* Mesh, const FLinearColor& Color)
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

bool CryptLineOfSight(const AActor* Source, const AActor* Target)
{
    if (!IsValid(Source) || !IsValid(Target) || !Source->GetWorld()) return false;
    FCollisionObjectQueryParams Objects;
    Objects.AddObjectTypesToQuery(ECC_WorldStatic);
    Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
    FCollisionQueryParams Query(SCENE_QUERY_STAT(MistCryptBossVisibility), false, Source);
    Query.AddIgnoredActor(Target);
    FHitResult Hit;
    return !Source->GetWorld()->LineTraceSingleByObjectType(Hit, Source->GetActorLocation(),
        Target->GetActorLocation(), Objects, Query);
}
}

AMistCryptGolem::AMistCryptGolem()
{
    CombatHealth->MaxHealth = MaximumHealth;
    CombatHealth->Armor = CombatHealth->MagicResistance = 20.f;
    CombatHealth->bCountsForUltimateRefund = true;
    CombatHealth->PercentageBurnMaxTick = 10.f;
    Capsule->InitCapsuleSize(85.f, 96.f);
    Visual->SetRelativeScale3D(FVector(2.2f, 2.2f, 1.9f));
    Label->SetRelativeLocation(FVector(0.f, 0.f, 235.f));
    Label->SetWorldSize(25.f);
    static ConstructorHelpers::FObjectFinder<UMaterialInterface> Material(
        TEXT("/Game/LevelPrototyping/Materials/M_FlatCol.M_FlatCol"));
    if (Material.Succeeded()) Visual->SetMaterial(0, Material.Object);
    static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
    static ConstructorHelpers::FObjectFinder<UStaticMesh> Sphere(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
    static ConstructorHelpers::FObjectFinder<UStaticMesh> Cone(TEXT("/Engine/BasicShapes/Cone.Cone"));
    UStaticMesh* Shapes[] = { Cube.Object, Sphere.Object, Cone.Object };
    for (int32 Index = 0; Index < 3; ++Index)
    {
        UStaticMeshComponent* Symbol = CreateDefaultSubobject<UStaticMeshComponent>(
            *FString::Printf(TEXT("WardSymbol%d"), Index));
        Symbol->SetupAttachment(Capsule);
        Symbol->SetStaticMesh(Shapes[Index]);
        Symbol->SetRelativeScale3D(FVector(0.45f));
        Symbol->SetRelativeLocation(FVector(0.f, (Index - 1) * 65.f, 145.f));
        Symbol->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Symbol->SetCanEverAffectNavigation(false);
        if (Material.Succeeded()) Symbol->SetMaterial(0, Material.Object);
        WardSymbols.Add(Symbol);
    }
}

void AMistCryptGolem::BeginPlay()
{
    Super::BeginPlay();
    InitializeEncounter();
}

void AMistCryptGolem::InitializeEncounter()
{
    if (bInitialized || !HasAuthority() || !GetWorld()) return;
    bInitialized = true;
    HomeLocation = GetActorLocation();
    CombatHealth->MaxHealth = MaximumHealth;
    CombatHealth->ResetHealth();
    CombatHealth->ClearCombatLock();
    CombatHealth->OnDefeated.AddUObject(this, &AMistCryptGolem::HandleDefeated);
    CombatHealth->GrantInvulnerability(86400.f);
    CombatHealth->GrantDebuffImmunity(86400.f);
    const FLinearColor Colors[] = {
        FLinearColor(0.65f, 0.13f, 0.02f), FLinearColor(0.025f, 0.4f, 0.8f), FLinearColor(0.08f, 0.55f, 0.12f)
    };
    for (int32 Index = 0; Index < WardSymbols.Num(); ++Index)
        SetCryptTint(WardSymbols[Index], Colors[Index]);
    UpdatePresentation();
}

bool AMistCryptGolem::BeginEncounter(APawn* Player)
{
    InitializeEncounter();
    const UMistCombatHealth* PlayerHealth = IsValid(Player) ? Player->FindComponentByClass<UMistCombatHealth>() : nullptr;
    if (!HasAuthority() || !bInitialized || State != EEncounterState::Dormant
        || !PlayerHealth || !PlayerHealth->IsAlive()) return false;
    EncounterPlayer = Player;
    State = EEncounterState::Protected;
    AttackState = EAttackState::Ready;
    CombatHealth->GrantInvulnerability(86400.f);
    CombatHealth->GrantDebuffImmunity(86400.f);
    if (!SpawnGuardianWave())
    {
        ResetEncounter();
        return false;
    }
    UpdatePresentation();
    return true;
}

bool AMistCryptGolem::SpawnGuardianWave()
{
    RemoveGuardians();
    const FVector Homes[] = { FVector(-400.f, 0.f, 0.f), FVector(-200.f, 346.f, 0.f), FVector(-200.f, -346.f, 0.f) };
    FActorSpawnParameters Params;
    Params.Owner = this;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    for (int32 Index = 0; Index < 3; ++Index)
    {
        AMistCryptGuardian* Guardian = GetWorld()->SpawnActor<AMistCryptGuardian>(
            HomeLocation + Homes[Index], FRotator(0.f, 180.f, 0.f), Params);
        if (!Guardian)
        {
            RemoveGuardians();
            UE_LOG(LogTemp, Error, TEXT("Crypt encounter could not spawn its complete three-guardian wave."));
            return false;
        }
        Guardians.Add(Guardian);
        Guardian->Initialize(this, EncounterPlayer.Get(), Index, 1.f + Index * 2.5f);
    }
    return true;
}

void AMistCryptGolem::RemoveGuardians()
{
    for (AMistCryptGuardian* Guardian : Guardians)
        if (IsValid(Guardian) && !Guardian->IsActorBeingDestroyed()) Guardian->Destroy();
    Guardians.Reset();
}

void AMistCryptGolem::ResetEncounter(bool bReplayCompleted)
{
    if (!HasAuthority() || !GetWorld()) return;
    InitializeEncounter();
    // Ordinary resets retain the clear. Only the explicit checkpoint replay
    // rearms a completed boss; earned loot remains in the progression component.
    if (State == EEncounterState::Complete && !bReplayCompleted) return;
    State = EEncounterState::Dormant;
    RemoveGuardians();
    EncounterPlayer.Reset();
    AttackState = EAttackState::Ready;
    AttackUntil = VulnerableUntil = 0.0;
    CommittedDirection = FVector::ForwardVector;
    SetActorLocation(HomeLocation, false, nullptr, ETeleportType::TeleportPhysics);
    Capsule->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
    CombatHealth->ResetHealth();
    CombatHealth->ClearCombatLock();
    CombatHealth->GrantInvulnerability(86400.f);
    CombatHealth->GrantDebuffImmunity(86400.f);
    UpdatePresentation();
}

void AMistCryptGolem::RestoreEncounterComplete()
{
    if (!HasAuthority() || !GetWorld()) return;
    InitializeEncounter();
    State = EEncounterState::Complete;
    RemoveGuardians();
    EncounterPlayer.Reset();
    AttackState = EAttackState::Ready;
    AttackUntil = VulnerableUntil = 0.0;
    CombatHealth->ResetHealth();
    CombatHealth->ClearCombatLock();
    CombatHealth->Health = 0.f;
    Capsule->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
    UpdatePresentation();
    // Restoring a save deliberately does not broadcast an earned completion.
}

bool AMistCryptGolem::IsEncounterActive() const
{
    return State == EEncounterState::Protected || State == EEncounterState::Vulnerable;
}

bool AMistCryptGolem::IsEncounterComplete() const { return State == EEncounterState::Complete; }
bool AMistCryptGolem::IsVulnerable() const { return State == EEncounterState::Vulnerable; }

FString AMistCryptGolem::GetAttackStateName() const
{
    switch (AttackState)
    {
    case EAttackState::Windup: return TEXT("Windup");
    case EAttackState::Recovery: return TEXT("Recovery");
    default: return TEXT("Ready");
    }
}

FString AMistCryptGolem::GetStateName() const
{
    switch (State)
    {
    case EEncounterState::Protected: return TEXT("Protected");
    case EEncounterState::Vulnerable: return TEXT("Vulnerable");
    case EEncounterState::Complete: return TEXT("Complete");
    default: return TEXT("Dormant");
    }
}

float AMistCryptGolem::GetVulnerabilityRemaining() const
{
    return IsVulnerable() && GetWorld() ? FMath::Max(0.f,
        static_cast<float>(VulnerableUntil - GetWorld()->GetTimeSeconds())) : 0.f;
}

int32 AMistCryptGolem::GetRemainingGuardians() const
{
    int32 Remaining = 0;
    for (const AMistCryptGuardian* Guardian : Guardians)
        if (IsValid(Guardian) && !Guardian->IsActorBeingDestroyed() && Guardian->CombatHealth->IsAlive()) ++Remaining;
    return Remaining;
}

void AMistCryptGolem::NotifyGuardianDefeated(AMistCryptGuardian* Guardian)
{
    if (!HasAuthority() || State != EEncounterState::Protected || !Guardians.Contains(Guardian)) return;
    if (GetRemainingGuardians() == 0) BeginVulnerability();
    UpdatePresentation();
}

void AMistCryptGolem::BeginVulnerability()
{
    State = EEncounterState::Vulnerable;
    VulnerableUntil = GetWorld()->GetTimeSeconds() + FMath::Max(0.1f, VulnerabilityDuration);
    AttackState = EAttackState::Ready;
    AttackUntil = 0.0;
    CombatHealth->ClearInvulnerability();
    CombatHealth->GrantDebuffImmunity(86400.f);
}

void AMistCryptGolem::HandleDefeated()
{
    if (!HasAuthority() || !IsEncounterActive()) return;
    State = EEncounterState::Complete;
    RemoveGuardians();
    AttackState = EAttackState::Ready;
    AttackUntil = VulnerableUntil = 0.0;
    Capsule->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
    UpdatePresentation();
    OnEncounterCompleted.Broadcast();
}

bool AMistCryptGolem::IsTargetInsideAttack(AActor* Target) const
{
    if (!IsValid(Target) || !CryptLineOfSight(this, Target)) return false;
    const FVector Delta = Target->GetActorLocation() - GetActorLocation();
    if (FMath::Abs(Delta.Z) > 160.f) return false;
    const FVector Point(Delta.X, Delta.Y, 0.f);
    const UCapsuleComponent* TargetCapsule = Target->FindComponentByClass<UCapsuleComponent>();
    const float Radius = TargetCapsule ? TargetCapsule->GetScaledCapsuleRadius() : 0.f;
    const float Distance = Point.Size();
    if (Distance <= Radius) return true;
    const float Angle = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(
        FVector::DotProduct(Point / Distance, CommittedDirection), -1.f, 1.f)));
    if (Angle <= AttackHalfAngle) return Distance <= AttackRange + Radius;
    const FVector EdgeA = CommittedDirection.RotateAngleAxis(AttackHalfAngle, FVector::UpVector) * AttackRange;
    const FVector EdgeB = CommittedDirection.RotateAngleAxis(-AttackHalfAngle, FVector::UpVector) * AttackRange;
    return FMath::PointDistToSegment(Point, FVector::ZeroVector, EdgeA) <= Radius
        || FMath::PointDistToSegment(Point, FVector::ZeroVector, EdgeB) <= Radius;
}

void AMistCryptGolem::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (!HasAuthority() || !GetWorld()) return;
    InitializeEncounter();
    const double Now = GetWorld()->GetTimeSeconds();
    if (State == EEncounterState::Dormant || State == EEncounterState::Protected)
    {
        CombatHealth->GrantInvulnerability(1.f);
        CombatHealth->GrantDebuffImmunity(1.f);
    }
    if (State == EEncounterState::Protected && GetRemainingGuardians() == 0) BeginVulnerability();
    if (IsVulnerable())
    {
        CombatHealth->GrantDebuffImmunity(1.f);
        if (Now >= VulnerableUntil)
        {
            State = EEncounterState::Protected;
            AttackState = EAttackState::Ready;
            AttackUntil = 0.0;
            CombatHealth->GrantInvulnerability(86400.f);
            if (!SpawnGuardianWave()) ResetEncounter();
        }
        else if (APawn* Player = EncounterPlayer.Get())
        {
            UMistCombatHealth* PlayerHealth = Player->FindComponentByClass<UMistCombatHealth>();
            const bool bCanAttack = PlayerHealth && PlayerHealth->IsAlive() && !PlayerHealth->IsInvulnerable()
                && Player->GetActorLocation().X >= -740.f;
            if (AttackState == EAttackState::Windup)
            {
                DrawAttackSector(FColor::Orange, 0.f);
                if (Now >= AttackUntil)
                {
                    if (bCanAttack && IsTargetInsideAttack(Player))
                        PlayerHealth->ReceivePhysicalDamage(AttackDamage, this);
                    DrawAttackSector(FColor::Red, 0.15f);
                    AttackState = EAttackState::Recovery;
                    AttackUntil = Now + AttackRecovery;
                }
            }
            else if (AttackState == EAttackState::Recovery)
            {
                if (Now >= AttackUntil) AttackState = EAttackState::Ready;
            }
            else if (bCanAttack && FVector::DistSquared2D(GetActorLocation(), Player->GetActorLocation())
                <= FMath::Square(AttackRange + 40.f) && CryptLineOfSight(this, Player))
            {
                CommittedDirection = (Player->GetActorLocation() - GetActorLocation()).GetSafeNormal2D();
                if (CommittedDirection.IsNearlyZero()) CommittedDirection = GetActorForwardVector().GetSafeNormal2D();
                SetActorRotation(CommittedDirection.Rotation());
                AttackState = EAttackState::Windup;
                AttackUntil = Now + AttackWindup;
                DrawAttackSector(FColor::Orange, 0.f);
            }
        }
    }
    if (IsEncounterActive())
    {
        FVector Circle = HomeLocation;
        Circle.Z -= Capsule->GetScaledCapsuleHalfHeight() - 4.f;
        DrawDebugCircle(GetWorld(), Circle, ProtectiveRadius, 80,
            IsVulnerable() ? FColor(90, 90, 90) : FColor(70, 210, 245), false, 0.f, 0, 4.f,
            FVector::ForwardVector, FVector::RightVector, false);
    }
    UpdatePresentation();
}

void AMistCryptGolem::DrawAttackSector(const FColor& Color, float Lifetime) const
{
    constexpr int32 Segments = 24;
    const FVector Origin = GetActorLocation() - FVector(0.f, 0.f, Capsule->GetScaledCapsuleHalfHeight() - 5.f);
    TArray<FVector> Vertices;
    TArray<int32> Indices;
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

void AMistCryptGolem::UpdatePresentation()
{
    FString Action;
    FColor Color = FColor(70, 210, 245);
    switch (State)
    {
    case EEncounterState::Protected:
        Action = FString::Printf(TEXT("WARD: %d / 3 seals\nBait charges OUTSIDE the circle"), GetRemainingGuardians());
        break;
    case EEncounterState::Vulnerable:
        Color = FColor::Orange;
        Action = FString::Printf(TEXT("VULNERABLE %.1fs\n%s"), GetVulnerabilityRemaining(),
            AttackState == EAttackState::Windup ? TEXT("DODGE the orange cone!") : TEXT("Attack the golem!"));
        break;
    case EEncounterState::Complete:
        Color = FColor(125, 125, 125);
        Action = TEXT("JAIL SECURED\nReturn to the checkpoint chest");
        break;
    default: Action = TEXT("ELEMENTAL JAIL\nEnter the chamber to challenge the ward"); break;
    }
    Label->SetText(FText::FromString(FString::Printf(TEXT("Jail Golem %.0f / %.0f\n%s"),
        CombatHealth->Health, CombatHealth->MaxHealth, *Action)));
    Label->SetTextRenderColor(Color);
    SetCryptTint(Visual, State == EEncounterState::Complete ? FLinearColor(0.07f, 0.07f, 0.07f)
        : IsVulnerable() ? FLinearColor(0.65f, 0.16f, 0.035f) : FLinearColor(0.045f, 0.28f, 0.35f));
    for (int32 Index = 0; Index < WardSymbols.Num(); ++Index)
    {
        const bool bLit = State == EEncounterState::Dormant || (State == EEncounterState::Protected
            && Guardians.IsValidIndex(Index) && IsValid(Guardians[Index]) && Guardians[Index]->CombatHealth->IsAlive());
        WardSymbols[Index]->SetVisibility(bLit);
    }
}

void AMistCryptGolem::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    CombatHealth->OnDefeated.RemoveAll(this);
    RemoveGuardians();
    Super::EndPlay(EndPlayReason);
}
