#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MistInventoryPreview.generated.h"

class APawn;
class UPointLightComponent;
class UPrimitiveComponent;
class USceneCaptureComponent2D;
class USceneComponent;
class UStaticMeshComponent;
class UTextureRenderTarget2D;

// A cosmetic copy, never another gameplay character. Only its components are
// visible to its scene capture; none appear in the world camera or navigation.
UCLASS(Transient, NotPlaceable)
class MISTWORLD_API AMistInventoryPreview : public AActor
{
    GENERATED_BODY()

public:
    AMistInventoryPreview();
    virtual void Tick(float DeltaSeconds) override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    void UpdateFromPawn(APawn* Pawn);
    void SetPreviewActive(bool bActive);
    void Rotate(float Degrees);
    void ResetRotation();

    UTextureRenderTarget2D* GetRenderTarget() const;
    bool IsPreviewActive() const { return bPreviewActive; }
    bool HasCharacterModel() const;
    int32 GetCopiedComponentCount() const { return Bindings.Num(); }
    float GetPreviewYaw() const { return PreviewYaw; }

private:
    friend struct FMistInventoryPreviewTestAccess;

    struct FCopyBinding
    {
        TWeakObjectPtr<UPrimitiveComponent> Source;
        TWeakObjectPtr<UPrimitiveComponent> Copy;
    };

    UFUNCTION() void HandleSourceDestroyed(AActor* DestroyedActor);
    void RefreshSource();
    void RefreshFraming();
    void CapturePortrait();
    void ClearCopies();
    void ConfigurePrimitive(UPrimitiveComponent* Primitive);
    UStaticMeshComponent* CreateStagePart(FName Name, const TCHAR* MeshPath,
        const FVector& Location, const FVector& Scale, const FLinearColor& Color);

    UPROPERTY(Transient) TObjectPtr<USceneComponent> PreviewRoot;
    UPROPERTY(Transient) TObjectPtr<USceneComponent> ModelRoot;
    UPROPERTY(Transient) TObjectPtr<USceneCaptureComponent2D> Capture;
    UPROPERTY(Transient) TObjectPtr<UTextureRenderTarget2D> RenderTarget;
    UPROPERTY(Transient) TArray<TObjectPtr<UPrimitiveComponent>> Copies;
    UPROPERTY(Transient) TArray<TObjectPtr<UPrimitiveComponent>> StageParts;
    UPROPERTY(Transient) TArray<TObjectPtr<UPointLightComponent>> StageLights;

    TWeakObjectPtr<APawn> SourcePawn;
    TArray<FCopyBinding> Bindings;
    float PreviewYaw = -18.f;
    float SecondsUntilRefresh = 0.f;
    bool bPreviewActive = false;
};
