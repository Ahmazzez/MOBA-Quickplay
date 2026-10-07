#pragma once

#include "CoreMinimal.h"
#include "MistTrainingDummy.h"
#include "MistArenaEnemy.generated.h"

class APawn;

/** A readable first combat opponent. It shares the dummy's damageable components. */
UCLASS()
class MISTWORLD_API AMistArenaEnemy : public AMistTrainingDummy
{
    GENERATED_BODY()

public:
    AMistArenaEnemy();
    virtual void Tick(float DeltaSeconds) override;

    static constexpr float MaximumHealth = 900.f;
    static constexpr float MovementSpeed = 250.f;
    static constexpr float AggroRange = 800.f;
    static constexpr float HomeLeash = 1400.f;
    static constexpr float AttackRange = 250.f;
    static constexpr float AttackHalfAngle = 50.f;
    static constexpr float AttackRawDamage = 100.f;
    static constexpr float AttackWindup = 0.8f;
    static constexpr float AttackRecovery = 1.2f;

    FString GetStateName() const;
    float GetStateRemaining() const;
    FVector GetHomeLocation() const { return HomeLocation; }
    FVector GetCommittedDirection() const { return CommittedDirection; }
    bool IsTargetInsideAttack(AActor* Target) const;

    // Explicit training reset (Y); ordinary disengagement heals only after returning home.
    void ResetEncounter();

protected:
    virtual void BeginPlay() override;

private:
    enum class ECombatState : uint8 { Idle, Approach, Windup, Recovery, Returning, Down };

    ECombatState State = ECombatState::Idle;
    FVector HomeLocation = FVector::ZeroVector;
    FVector CommittedDirection = FVector::ForwardVector;
    double StateUntil = 0.0;
    double NextPathRefresh = 0.0;
    float PreviousHealth = MaximumHealth;
    bool bReportedMovementFailure = false;
    TArray<FVector> PathPoints;
    int32 PathIndex = 0;

    APawn* FindPlayerTarget() const;
    bool CanTarget(const APawn* Target) const;
    bool HasTerrainLineOfSight(const AActor* Target) const;
    void BeginWindup(const APawn* Target);
    void BeginReturn();
    void BeginRecovery();
    void MoveAlongPath(const FVector& Goal, float DeltaSeconds);
    bool MoveOnGround(const FVector& DesiredLocation);
    bool ReportMovementFailure(const FString& Context, const FHitResult* Hit = nullptr);
    void ClearPath();
    void DrawAttackSector(const FColor& Color, float Lifetime) const;
    void UpdateLabel();
};
