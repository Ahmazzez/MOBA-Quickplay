#include "MistCryptSession.h"
#include "MistCryptGolem.h"
#include "MistworldPlayerController.h"
#include "MistProgressionComponent.h"
#include "MistCombatHealth.h"
#include "MistReturningSword.h"
#include "MistAbyssShockwave.h"
#include "MistAbyssTrail.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "DrawDebugHelpers.h"
#include "UObject/ConstructorHelpers.h"

AMistCryptSession::AMistCryptSession()
{
    PrimaryActorTick.bCanEverTick = true;
    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("CryptSessionRoot"));
    Gate = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("EncounterGate"));
    Gate->SetupAttachment(RootComponent);
    static ConstructorHelpers::FObjectFinder<UStaticMesh> Mesh(TEXT("/Engine/BasicShapes/Cube.Cube"));
    if (Mesh.Succeeded()) Gate->SetStaticMesh(Mesh.Object);
    Gate->SetRelativeLocation(FVector(-800.f, 0.f, 150.f));
    Gate->SetRelativeScale3D(FVector(1.f, 5.f, 3.f));
    Gate->SetCollisionProfileName(TEXT("BlockAll"));
    Gate->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Gate->SetCanEverAffectNavigation(true);
    Gate->SetHiddenInGame(true);
}

AMistCryptGolem* AMistCryptSession::GetGolem() const
{
    for (TActorIterator<AMistCryptGolem> It(GetWorld()); It; ++It)
        if (!It->IsActorBeingDestroyed()) return *It;
    return nullptr;
}

AMistworldPlayerController* AMistCryptSession::GetCombatController() const
{
    return GetWorld() ? Cast<AMistworldPlayerController>(GetWorld()->GetFirstPlayerController()) : nullptr;
}

FTransform AMistCryptSession::GetCheckpointTransform(FName ID) const
{
    return FTransform(FRotator::ZeroRotator, ID == TEXT("Crypt.Reward")
        ? FVector(2000.f, 1100.f, 98.15f) : FVector(-1500.f, 0.f, 98.15f));
}

void AMistCryptSession::BeginPlay()
{
    Super::BeginPlay();
    SetGateClosed(false);
    if (AMistCryptGolem* Golem = GetGolem())
        Golem->OnEncounterCompleted.AddUObject(this, &AMistCryptSession::HandleEncounterCompleted);
}

void AMistCryptSession::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (AMistCryptGolem* Golem = GetGolem()) Golem->OnEncounterCompleted.RemoveAll(this);
    Super::EndPlay(EndPlayReason);
}

void AMistCryptSession::SetGateClosed(bool bClosed)
{
    bGateClosed = bClosed;
    Gate->SetHiddenInGame(!bClosed);
    Gate->SetCollisionEnabled(bClosed ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
}

void AMistCryptSession::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (!HasAuthority()) return;
    AMistworldPlayerController* PC = GetCombatController();
    AMistCryptGolem* Golem = GetGolem();
    if (!PC || !PC->GetPawn() || !PC->GetProgression() || !Golem) return;
    UMistProgressionComponent* Progress = PC->GetProgression();
    UMistCombatHealth* Health = PC->GetPawn()->FindComponentByClass<UMistCombatHealth>();
    if (!bRestoredCompletion)
    {
        // Tick runs after every actor's BeginPlay; restoration cannot be undone
        // by a late health-component initialization on the boss.
        bRestoredCompletion = true;
        if (Progress->IsCryptCompleted()) Golem->RestoreEncounterComplete();
    }
    if (!Health || !Health->IsAlive()) return;
    const FVector Position = PC->GetPawn()->GetActorLocation();
    if ((!Progress->IsCryptCompleted() || bReplayArmed) && !Golem->IsEncounterActive()
        && Position.X > -480.f && Position.X < 2450.f && FMath::Abs(Position.Y) < 1550.f)
        if (Golem->BeginEncounter(PC->GetPawn())) SetGateClosed(true);
    if (!Golem->IsEncounterActive() && bGateClosed) SetGateClosed(false);
    const FName IDs[] = { TEXT("Crypt.Entrance"), TEXT("Crypt.Reward") };
    for (const FName ID : IDs)
    {
        if (ID == TEXT("Crypt.Reward") && !Progress->IsCryptCompleted()) continue;
        const FTransform Checkpoint = GetCheckpointTransform(ID);
        if (FVector::Dist2D(Position, Checkpoint.GetLocation()) <= 250.f)
            PC->TryActivateCryptCheckpoint(ID, Checkpoint);
        DrawDebugCircle(GetWorld(), Checkpoint.GetLocation() - FVector(0.f, 0.f, 92.f), 250.f, 40,
            PC->GetActiveCheckpointID() == ID ? FColor(255, 200, 90) : FColor(120, 110, 80),
            false, 0.f, 0, 2.f, FVector::ForwardVector, FVector::RightVector, false);
    }
}

void AMistCryptSession::HandleEncounterCompleted()
{
    bReplayArmed = false;
    SetGateClosed(false);
    if (AMistworldPlayerController* PC = GetCombatController())
        if (UMistProgressionComponent* Progress = PC->GetProgression())
        {
            Progress->SetCryptCompleted();
            PC->SaveCryptProgression();
        }
}

void AMistCryptSession::ResetEncounter()
{
    SetGateClosed(false);
    RemoveAttemptEffects();
    if (AMistCryptGolem* Golem = GetGolem())
    {
        const AMistworldPlayerController* PC = GetCombatController();
        if (!bReplayArmed && PC && PC->GetProgression() && PC->GetProgression()->IsCryptCompleted()) Golem->RestoreEncounterComplete();
        else Golem->ResetEncounter(bReplayArmed);
    }
}

void AMistCryptSession::RemoveAttemptEffects()
{
    // A dungeon wipe ends this attempt. Released effects from it must not
    // damage a fresh guardian wave after the checkpoint revive.
    const AMistworldPlayerController* Controller = GetCombatController();
    if (Controller && Controller->GetPawn())
    {
        for (TActorIterator<AMistReturningSword> It(GetWorld()); It; ++It)
            if (It->GetOwner() == Controller->GetPawn()) It->Destroy();
        for (TActorIterator<AMistAbyssShockwave> It(GetWorld()); It; ++It)
            if (It->GetOwner() == Controller->GetPawn()) It->Destroy();
        for (TActorIterator<AMistAbyssTrail> It(GetWorld()); It; ++It)
            if (It->GetOwner() == Controller->GetPawn()) It->Destroy();
    }
}

bool AMistCryptSession::IsAtReplayCheckpoint() const
{
    const AMistworldPlayerController* PC = GetCombatController();
    if (!PC || !PC->GetPawn()) return false;
    const FVector Position = PC->GetPawn()->GetActorLocation();
    for (const FName ID : {FName(TEXT("Crypt.Entrance")), FName(TEXT("Crypt.Reward"))})
    {
        const FVector Checkpoint = GetCheckpointTransform(ID).GetLocation();
        if (FVector::Dist2D(Position, Checkpoint) <= 275.f && FMath::Abs(Position.Z - Checkpoint.Z) <= 100.f)
            return true;
    }
    return false;
}

void AMistCryptSession::ReplayEncounter()
{
    if (!HasAuthority() || !GetGolem()) return;
    bReplayArmed = true;
    bRestoredCompletion = true;
    SetGateClosed(false);
    RemoveAttemptEffects();
    GetGolem()->ResetEncounter(true);
}

bool AMistCryptSession::TryInteract()
{
    AMistworldPlayerController* PC = GetCombatController();
    if (!PC || !PC->GetPawn() || !PC->GetProgression() || !PC->GetProgression()->IsCryptCompleted()) return false;
    const FTransform Reward = GetCheckpointTransform(TEXT("Crypt.Reward"));
    if (FVector::Dist2D(PC->GetPawn()->GetActorLocation(), Reward.GetLocation()) > 275.f) return false;
    PC->TryActivateCryptCheckpoint(TEXT("Crypt.Reward"), Reward);
    if (!PC->GetLoadoutLockReason().IsEmpty()) return false;
    if (!PC->SaveCryptProgression()) return false;
    return PC->GetProgression()->ClaimCryptReward();
}

FString AMistCryptSession::GetObjectiveText() const
{
    const AMistworldPlayerController* PC = GetCombatController();
    if (!bReplayArmed && PC && PC->GetProgression() && PC->GetProgression()->IsCryptCompleted())
        return PC->GetProgression()->HasReward() ? TEXT("Crypt cleared. Reward owned; I opens inventory.")
            : TEXT("Crypt cleared. Reach the gold chest checkpoint to claim loot.");
    const AMistCryptGolem* Golem = GetGolem();
    if (!Golem || !Golem->IsEncounterActive()) return bReplayArmed
        ? TEXT("Replay ready. Enter the chamber to challenge the elemental ward.")
        : TEXT("Enter the chamber. Bait guardian charges outside the circle.");
    if (Golem->IsVulnerable()) return FString::Printf(TEXT("GOLEM VULNERABLE %.1fs / dodge its heavy swings"), Golem->GetVulnerabilityRemaining());
    return FString::Printf(TEXT("GUARDIANS %d / 3 remaining / bait charges out of the ward"), Golem->GetRemainingGuardians());
}

FString AMistCryptSession::GetInteractionHint() const
{
    const AMistworldPlayerController* PC = GetCombatController();
    if (!PC || !PC->GetPawn() || !PC->GetProgression()) return FString();
    const bool bAtCheckpoint = IsAtReplayCheckpoint();
    const UMistCombatHealth* Health = PC->GetPawn()->FindComponentByClass<UMistCombatHealth>();
    if (bAtCheckpoint && Health && Health->GetCombatLockRemaining() > 0.f)
        return FString::Printf(TEXT("B: replay/reset boss when out of combat (%.1fs)"), Health->GetCombatLockRemaining());
    if (!PC->GetProgression()->IsCryptCompleted()
        || FVector::Dist2D(PC->GetPawn()->GetActorLocation(), GetCheckpointTransform(TEXT("Crypt.Reward")).GetLocation()) > 275.f)
        return bAtCheckpoint ? TEXT("B: replay/reset boss / I: inventory / L: loadout")
            : TEXT("Return to either checkpoint and press B to replay/reset the boss.");
    if (PC->GetProgression()->HasReward()) return TEXT("Chest claimed. B: replay boss / I: equipment (+5% ability damage)");
    const FString Lock = PC->GetLoadoutLockReason();
    return Lock.IsEmpty() ? TEXT("C: claim Necrotic Greatsword / B: replay boss") : Lock;
}
