#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "UObject/SoftObjectPtr.h"
#include "MistAlphonseAnimInstance.generated.h"

class UAnimSequence;
struct FMistAlphonseAnimInstanceProxy;

// The blockout uses its own skeleton. Native nodes provide locomotion, an
// upper-body guard, and DefaultSlot for the existing cosmetic montage API.
UCLASS(Transient, Blueprintable)
class MISTWORLD_API UMistAlphonseAnimInstance : public UAnimInstance
{
    GENERATED_BODY()

public:
    UMistAlphonseAnimInstance();
    virtual void NativeInitializeAnimation() override;

    UFUNCTION(BlueprintCallable, Category="Mistworld|Presentation")
    void SetGuard(bool bAbyss, float Duration);

    UFUNCTION(BlueprintCallable, Category="Mistworld|Presentation")
    void ResetPresentationState();

    UFUNCTION(BlueprintPure, Category="Mistworld|Presentation")
    float GetGuardRemainingTime() const;

protected:
    virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override;
    virtual void DestroyAnimInstanceProxy(FAnimInstanceProxy* InProxy) override;

private:
    friend struct FMistAlphonseAnimInstanceProxy;

    // Soft references allow this class to load before the initial asset import.
    UPROPERTY() TSoftObjectPtr<UAnimSequence> IdleAsset;
    UPROPERTY() TSoftObjectPtr<UAnimSequence> RunAsset;
    UPROPERTY() TSoftObjectPtr<UAnimSequence> GuardAsset;
    UPROPERTY() TSoftObjectPtr<UAnimSequence> AbyssGuardAsset;
    UPROPERTY() TSoftObjectPtr<UAnimSequence> DeathAsset;

    // Strong cached references keep loaded clips alive for the proxy lifetime.
    UPROPERTY(Transient) TObjectPtr<UAnimSequence> IdleSequence;
    UPROPERTY(Transient) TObjectPtr<UAnimSequence> RunSequence;
    UPROPERTY(Transient) TObjectPtr<UAnimSequence> GuardSequence;
    UPROPERTY(Transient) TObjectPtr<UAnimSequence> AbyssGuardSequence;
    UPROPERTY(Transient) TObjectPtr<UAnimSequence> DeathSequence;

    double GuardExpiresAt = 0.0;
    uint32 ResetSerial = 0;
    bool bAbyssGuard = false;
};
