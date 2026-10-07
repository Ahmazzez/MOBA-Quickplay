#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MistCryptSession.generated.h"

class AMistCryptGolem;
class AMistworldPlayerController;
class UStaticMeshComponent;

// The dungeon slice owns its encounter boundary and safe checkpoint/reward flow.
UCLASS()
class MISTWORLD_API AMistCryptSession : public AActor
{
    GENERATED_BODY()
public:
    AMistCryptSession();
    virtual void Tick(float DeltaSeconds) override;
    AMistCryptGolem* GetGolem() const;
    FTransform GetCheckpointTransform(FName ID) const;
    bool IsGateClosed() const { return bGateClosed; }
    bool TryInteract();
    void ResetEncounter();
    bool IsAtReplayCheckpoint() const;
    void ReplayEncounter();
    FString GetObjectiveText() const;
    FString GetInteractionHint() const;
protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
private:
    UPROPERTY(VisibleAnywhere) TObjectPtr<UStaticMeshComponent> Gate;
    bool bGateClosed = false;
    bool bRestoredCompletion = false;
    // Replays are session-local. The saved clear and reward never get erased.
    bool bReplayArmed = false;
    void SetGateClosed(bool bClosed);
    void HandleEncounterCompleted();
    void RemoveAttemptEffects();
    AMistworldPlayerController* GetCombatController() const;
};
