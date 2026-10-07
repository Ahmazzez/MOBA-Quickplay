#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MistTrainingDummy.generated.h"
class UCapsuleComponent;
class UStaticMeshComponent;
class UMistCombatHealth;
class UTextRenderComponent;
UCLASS()
class MISTWORLD_API AMistTrainingDummy : public AActor
{
    GENERATED_BODY()
public:
    AMistTrainingDummy();
    virtual void Tick(float DeltaSeconds) override;
protected:
    virtual void BeginPlay() override;
public:
    UPROPERTY(VisibleAnywhere) TObjectPtr<UCapsuleComponent> Capsule;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UStaticMeshComponent> Visual;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UMistCombatHealth> CombatHealth;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UTextRenderComponent> Label;
};
