#pragma once

#include "CoreMinimal.h"
#include "Navigation/PathFollowingComponent.h"
#include "MistConstantSpeedPathFollowingComponent.generated.h"

class UCharacterMovementComponent;

// Keep Unreal's navmesh, collision, path corners, and blocked-path handling,
// but use an immediate full-speed velocity for player click movement.
UCLASS()
class UMistConstantSpeedPathFollowingComponent : public UPathFollowingComponent
{
    GENERATED_BODY()

public:
    UMistConstantSpeedPathFollowingComponent();

    static void ConfigureCharacterMovement(UCharacterMovementComponent* Movement, float WalkSpeed = 500.f);
    virtual void SetNavMovementInterface(INavMovementInterface* NavMoveInterface) override;

protected:
    virtual void FollowPathSegment(float DeltaTime) override;
    virtual void UpdatePathSegment() override;
};
