#pragma once

#include "CoreMinimal.h"
#include "MistTrainingDummy.h"
#include "MistCryptGolem.generated.h"

class AMistCryptGuardian;
class APawn;
class UStaticMeshComponent;

/** The elemental jail encounter. Protection is an objective, never a removable shield. */
UCLASS()
class MISTWORLD_API AMistCryptGolem : public AMistTrainingDummy
{
    GENERATED_BODY()
public:
    AMistCryptGolem();
    virtual void Tick(float DeltaSeconds) override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    bool BeginEncounter(APawn* Player);
    void ResetEncounter(bool bReplayCompleted = false);
    void RestoreEncounterComplete();
    bool IsEncounterActive() const;
    bool IsEncounterComplete() const;
    bool IsVulnerable() const;
    FString GetStateName() const;
    FString GetAttackStateName() const;
    float GetVulnerabilityRemaining() const;
    int32 GetRemainingGuardians() const;
    float GetProtectiveRadius() const { return ProtectiveRadius; }
    const TArray<TObjectPtr<AMistCryptGuardian>>& GetGuardians() const { return Guardians; }
    FVector GetHomeLocation() const { return HomeLocation; }
    FVector GetCommittedDirection() const { return CommittedDirection; }
    bool IsTargetInsideAttack(AActor* Target) const;
    void NotifyGuardianDefeated(AMistCryptGuardian* Guardian);
    FSimpleMulticastDelegate OnEncounterCompleted;

    UPROPERTY(EditAnywhere, Category="Crypt") float ProtectiveRadius = 700.f;
    UPROPERTY(EditAnywhere, Category="Crypt") float VulnerabilityDuration = 15.f;
    static constexpr float MaximumHealth = 3000.f;
    static constexpr float AttackRange = 250.f;
    static constexpr float AttackHalfAngle = 55.f;
    static constexpr float AttackDamage = 180.f;
    static constexpr float AttackWindup = 1.1f;
    static constexpr float AttackRecovery = 1.8f;

protected:
    virtual void BeginPlay() override;

private:
    friend struct FMistCryptEncounterTestAccess;
    enum class EEncounterState : uint8 { Dormant, Protected, Vulnerable, Complete };
    enum class EAttackState : uint8 { Ready, Windup, Recovery };
    EEncounterState State = EEncounterState::Dormant;
    EAttackState AttackState = EAttackState::Ready;
    FVector HomeLocation = FVector::ZeroVector;
    FVector CommittedDirection = FVector::ForwardVector;
    TWeakObjectPtr<APawn> EncounterPlayer;
    UPROPERTY() TArray<TObjectPtr<AMistCryptGuardian>> Guardians;
    UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> WardSymbols;
    double VulnerableUntil = 0.0;
    double AttackUntil = 0.0;
    bool bInitialized = false;

    void InitializeEncounter();
    bool SpawnGuardianWave();
    void RemoveGuardians();
    void BeginVulnerability();
    void HandleDefeated();
    void UpdatePresentation();
    void DrawAttackSector(const FColor& Color, float Lifetime) const;
};
