#pragma once

#include "CoreMinimal.h"
#include "MistTrainingDummy.h"
#include "MistCryptGuardian.generated.h"

class AMistCryptGolem;
class APawn;

/** Bait elemental guardians out of the ward and exploit their committed attacks. */
UCLASS()
class MISTWORLD_API AMistCryptGuardian : public AMistTrainingDummy
{
    GENERATED_BODY()
public:
    AMistCryptGuardian();
    virtual void Tick(float DeltaSeconds) override;
    void Initialize(AMistCryptGolem* Golem, APawn* Player, int32 ElementIndex, float InitialDelay);
    FString GetStateName() const;
    FString GetElementName() const;
    int32 GetElementIndex() const { return Element; }
    float GetStateRemaining() const;
    bool IsWardProtected() const;
    FVector GetCommittedDirection() const { return CommittedDirection; }
    FVector GetHomeLocation() const { return HomeLocation; }
    FVector GetFireAreaCenter() const { return FireAreaCenter; }
    float GetFireAreaRemaining() const;

    UPROPERTY(EditAnywhere, Category="Crypt") float ChargeRange = 850.f;
    UPROPERTY(EditAnywhere, Category="Crypt") float ChargeSpeed = 900.f;
    UPROPERTY(EditAnywhere, Category="Crypt") float ChargeRadius = 65.f;
    UPROPERTY(EditAnywhere, Category="Crypt") float ChargeDamage = 120.f;
    UPROPERTY(EditAnywhere, Category="Crypt") float ChargeWindup = 1.2f;
    UPROPERTY(EditAnywhere, Category="Crypt") float ChargeRecovery = 6.f;
    UPROPERTY(EditAnywhere, Category="Crypt") float ReturnMovementSpeed = 300.f;
    UPROPERTY(EditAnywhere, Category="Crypt|Fire") float FireAreaRadius = 260.f;
    UPROPERTY(EditAnywhere, Category="Crypt|Fire") float FireWindup = 1.f;
    UPROPERTY(EditAnywhere, Category="Crypt|Fire") float FireExplosionDamage = 150.f;
    UPROPERTY(EditAnywhere, Category="Crypt|Fire") float FirePoolDuration = 4.f;
    UPROPERTY(EditAnywhere, Category="Crypt|Fire") float FirePoolTickDamage = 35.f;
    UPROPERTY(EditAnywhere, Category="Crypt|Fire") float FirePoolTickInterval = 0.8f;
    UPROPERTY(EditAnywhere, Category="Crypt|Ice") float IceFreezeDuration = 1.25f;
    static constexpr float MaximumHealth = 300.f;
    static constexpr float FireMaximumHealth = 360.f;
    static constexpr float WindMaximumHealth = 210.f;
    static constexpr float ReturnSpeed = 300.f;
    static constexpr float WardReduction = 0.75f;

private:
    friend struct FMistCryptEncounterTestAccess;
    enum class EGuardianState : uint8 { Waiting, Windup, Charging, FireWindup, Recovery, Returning, Down };
    EGuardianState State = EGuardianState::Waiting;
    TWeakObjectPtr<AMistCryptGolem> Encounter;
    TWeakObjectPtr<APawn> EncounterPlayer;
    FVector HomeLocation = FVector::ZeroVector;
    FVector CommittedDirection = FVector::ForwardVector;
    FVector ChargeStart = FVector::ZeroVector;
    FVector FireAreaCenter = FVector::ZeroVector;
    double StateUntil = 0.0;
    double FireAreaUntil = 0.0;
    double NextFirePulseAt = 0.0;
    float RemainingChargeTravel = 0.f;
    int32 Element = 0;
    bool bChargeHitPlayer = false;
    bool bInitialized = false;

    void ConfigureElement();
    void HandleDefeated();
    void UpdateProtection();
    void UpdatePresentation();
    bool CanTargetPlayer() const;
    bool HasTerrainLineOfSight(const AActor* Target) const;
    bool MoveOnGround(const FVector& DesiredLocation, float SweepRadius);
    void BeginChargeWindup();
    void FinishCharge();
    void BeginRecovery();
    void ApplyChargeHit(const FVector& From, const FVector& To);
    void ApplyFireAreaDamage(float RawDamage, bool bDamageOverTime = false);
    void UpdateFirePool();
    void DrawChargeLane(const FColor& Color, float Lifetime) const;
    void DrawFireArea(const FColor& Color, bool bBurning) const;
};
