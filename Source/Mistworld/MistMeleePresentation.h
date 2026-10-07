#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "MistMeleePresentation.generated.h"

class UAnimMontage;
class UAnimSequence;
class UMaterialInterface;
class USceneComponent;
class USkeletalMeshComponent;
class UStaticMesh;
class UStaticMeshComponent;

// Cosmetic melee presentation. Combat timers and sector tests remain the
// controller's responsibility; the sword never supplies gameplay collision.
UCLASS(ClassGroup=(Mistworld), meta=(BlueprintSpawnableComponent))
class MISTWORLD_API UMistMeleePresentation : public UActorComponent
{
    GENERATED_BODY()

public:
    UMistMeleePresentation();

    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    void InitializeSword();
    void PlaySwing(bool bHeavy, float PlayRate = 1.f);
    void PlayCleave(bool bAbyss, float CastTime);
    void PlayDash(bool bAbyss);
    void PlayUltimate(bool bAbyss, float CastTime);
    void PlayGuard(bool bAbyss, float Duration);
    void PlayFlash();
    void ResetPresentation();
    void SetAbyssAppearance(bool bAbyss);
    void CancelSwing();

    USceneComponent* GetSwordRoot() const { return SwordRoot; }
    UStaticMeshComponent* GetBlade() const { return Blade; }
    UAnimMontage* GetActiveSwing() const { return ActiveSwing; }
    bool IsSwingActive() const;

private:
    bool UsesAlphonseRig() const;
    UAnimSequence* LoadAlphonseClip(const TCHAR* Name) const;
    void PlaySequence(UAnimSequence* Sequence, float PlayRate = 1.f);
    UStaticMeshComponent* AddSwordPart(const FName Name, UStaticMesh* Mesh,
        const FVector& Location, const FVector& Scale, const FRotator& Rotation,
        const FLinearColor& Color, float Metallic, float Roughness);

    UPROPERTY(Transient) TObjectPtr<USkeletalMeshComponent> CharacterMesh;
    UPROPERTY(Transient) TObjectPtr<USceneComponent> SwordRoot;
    UPROPERTY(Transient) TObjectPtr<UStaticMeshComponent> Blade;
    UPROPERTY(Transient) TArray<TObjectPtr<USceneComponent>> CreatedComponents;
    UPROPERTY(Transient) TObjectPtr<UAnimMontage> ActiveSwing;

    UPROPERTY() TObjectPtr<UStaticMesh> CubeMesh;
    UPROPERTY() TObjectPtr<UStaticMesh> CylinderMesh;
    UPROPERTY() TObjectPtr<UStaticMesh> SphereMesh;
    UPROPERTY() TObjectPtr<UMaterialInterface> SwordMaterial;
    UPROPERTY() TObjectPtr<UAnimSequence> AutoSwing;
    UPROPERTY() TObjectPtr<UAnimSequence> CleaveSwing;
    bool bAbyssAppearance = false;
    bool bImportedSword = false;
};
