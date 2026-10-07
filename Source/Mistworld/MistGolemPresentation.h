#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "MistGolemPresentation.generated.h"

class UAnimSequence;
class USkeletalMeshComponent;
class UMaterialInstanceDynamic;

/** Shared imported stone golem visuals. Combat/collision stays on the encounter actor. */
UCLASS()
class MISTWORLD_API UMistGolemPresentation : public UActorComponent
{
    GENERATED_BODY()
public:
    UMistGolemPresentation();
    virtual void BeginPlay() override;
    virtual void TickComponent(float DeltaSeconds, ELevelTick TickType,
        FActorComponentTickFunction* ThisTickFunction) override;
    USkeletalMeshComponent* GetBody() const { return Body; }

private:
    UPROPERTY() TObjectPtr<USkeletalMeshComponent> Body;
    UPROPERTY() TArray<TObjectPtr<UMaterialInstanceDynamic>> Materials;
    UPROPERTY() TObjectPtr<UAnimSequence> Idle;
    UPROPERTY() TObjectPtr<UAnimSequence> Walk;
    UPROPERTY() TObjectPtr<UAnimSequence> Attack;
    UPROPERTY() TObjectPtr<UAnimSequence> Death;
    FString LastState;
    int32 LastElement = INDEX_NONE;
    bool bLastDead = false;
    bool bLastVulnerable = false;
    void SetColors(int32 Element, bool bDead, bool bVulnerable);
    void PlayState(const FString& State, float Duration = 1.f);
};
