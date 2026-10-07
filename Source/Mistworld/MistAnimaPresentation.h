#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "MistAnimaPresentation.generated.h"

class UAnimInstance;
class UAnimSequence;
class UMaterialInterface;
class USkeletalMesh;
class USkeletalMeshComponent;

// Presentation only: swapping heroes never replaces the gameplay capsule,
// controller, navigation agent, health component, or movement component.
UCLASS(ClassGroup=(Mistworld), meta=(BlueprintSpawnableComponent))
class MISTWORLD_API UMistAnimaPresentation : public UActorComponent
{
    GENERATED_BODY()
public:
    UMistAnimaPresentation();
    virtual void BeginPlay() override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType,
        FActorComponentTickFunction* ThisTickFunction) override;

    void Initialize();
    bool SetHeroActive(bool bWanted);
    bool IsHeroActive() const { return bActive; }
    USkeletalMeshComponent* GetBody() const { return Body; }
    void PlayAttack(float Duration);
    void PlayThrust(float Duration);
    void PlayDash(float Duration = 0.3f);
    void PlayGuard(float Duration);
    void PlayUltimate(float Duration);
    void ResetPresentation();

private:
    void PlayAction(const TCHAR* Name, float Duration);
    void SelectSequence(UAnimSequence* Sequence, bool bLoop, float PlayRate = 1.f);
    UAnimSequence* LoadClip(const TCHAR* Name) const;
    UPROPERTY(Transient) TObjectPtr<USkeletalMeshComponent> Body;
    UPROPERTY(Transient) TObjectPtr<USkeletalMesh> OriginalMesh;
    UPROPERTY(Transient) TSubclassOf<UAnimInstance> OriginalAnimClass;
    UPROPERTY(Transient) TArray<TObjectPtr<UMaterialInterface>> OriginalMaterials;
    UPROPERTY(Transient) TObjectPtr<USkeletalMesh> AnimaMesh;
    UPROPERTY(Transient) TObjectPtr<UAnimSequence> Idle;
    UPROPERTY(Transient) TObjectPtr<UAnimSequence> Run;
    UPROPERTY(Transient) TObjectPtr<UAnimSequence> Death;
    UPROPERTY(Transient) TObjectPtr<UAnimSequence> CurrentSequence;
    UPROPERTY(Transient) TObjectPtr<UAnimSequence> CurrentAction;
    FTransform OriginalTransform;
    uint8 OriginalAnimationMode = 0;
    float ActionRemaining = 0.f;
    bool bCapturedOriginal = false;
    bool bActive = false;
    bool bWasDead = false;
};

