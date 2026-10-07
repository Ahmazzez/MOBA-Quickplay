#include "MistConstantSpeedPathFollowingComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/NavMovementInterface.h"

UMistConstantSpeedPathFollowingComponent::UMistConstantSpeedPathFollowingComponent()
{
    // A clicked point is a precise destination, rather than something the
    // capsule only needs to overlap. Close clicks should still make a step.
    SetPreciseReachThreshold(0.f, 1.05f);
}

void UMistConstantSpeedPathFollowingComponent::ConfigureCharacterMovement(
    UCharacterMovementComponent* Movement, float WalkSpeed)
{
    if (!Movement) return;
    Movement->MaxWalkSpeed = WalkSpeed;
    Movement->GetNavMovementProperties()->bUseAccelerationForPaths = false;
    Movement->bRequestedMoveUseAcceleration = false;
    // A negative rotation rate means an instant turn in CharacterMovement.
    Movement->RotationRate = FRotator(0.f, -1.f, 0.f);
}

void UMistConstantSpeedPathFollowingComponent::SetNavMovementInterface(INavMovementInterface* NavMoveInterface)
{
    if (UActorComponent* PreviousMovement = Cast<UActorComponent>(NavMovementInterface.GetObject()))
        PreviousMovement->RemoveTickPrerequisiteComponent(this);
    Super::SetNavMovementInterface(NavMoveInterface);
    // Consume the latest click/ability decision, then provide velocity before
    // CharacterMovement simulates. This avoids an extra stale-direction frame.
    if (AActor* Controller = GetOwner()) AddTickPrerequisiteActor(Controller);
    if (UActorComponent* Movement = Cast<UActorComponent>(NavMovementInterface.GetObject()))
        Movement->AddTickPrerequisiteComponent(this);
}

void UMistConstantSpeedPathFollowingComponent::FollowPathSegment(float DeltaTime)
{
    if (!Path.IsValid() || !NavMovementInterface.IsValid() || DeltaTime <= 0.f) return;

    bIsDecelerating = false;
    FVector MoveVelocity = (GetCurrentTargetLocation() - NavMovementInterface->GetFeetLocation()) / DeltaTime;
    PostProcessMove.ExecuteIfBound(this, MoveVelocity);
    // The base component only forces full speed on intermediate segments.
    // Force it on the final segment too, avoiding the last-frame slowdown.
    NavMovementInterface->RequestDirectMove(MoveVelocity, true);
}

void UMistConstantSpeedPathFollowingComponent::UpdatePathSegment()
{
    Super::UpdatePathSegment();

    // With full-speed movement the final point can be crossed within a frame.
    // The engine's segment reach test already detects that crossing. Use it
    // for fixed point goals so they stop, instead of oscillating around a tiny
    // acceptance radius. Moving actors and custom links keep the base rules.
    if (GetStatus() == EPathFollowingStatus::Moving
        && Path.IsValid() && Path->IsValid() && NavMovementInterface.IsValid()
        && HasMovementAuthority() && !DestinationActor.IsValid()
        && !bIsUsingMetaPath && !bMoveSegmentIsUsingCustomLinkReachCondition
        && !CurrentCustomLinkOb.IsValid()
        && MoveSegmentEndIndex >= Path->GetPathPoints().Num() - 1
        && HasReachedCurrentTarget(NavMovementInterface->GetFeetLocation()))
    {
        OnSegmentFinished();
        OnPathFinished(EPathFollowingResult::Success, FPathFollowingResultFlags::None);
    }
}
