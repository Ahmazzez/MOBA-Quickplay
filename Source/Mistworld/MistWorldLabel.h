#pragma once

#include "CoreMinimal.h"
#include "Engine/TextRenderActor.h"
#include "MistWorldLabel.generated.h"

/** Floating world instructions face the viewing camera instead of lying on the floor. */
UCLASS()
class MISTWORLD_API AMistWorldLabel : public ATextRenderActor
{
    GENERATED_BODY()
public:
    AMistWorldLabel();
    virtual void Tick(float DeltaSeconds) override;
    // The local +X normal faces the camera and local +Z stays screen-up.
    static FRotator CameraFacingRotation(const FVector& Position, const FVector& CameraPosition);
};
