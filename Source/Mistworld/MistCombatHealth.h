#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "MistCombatHealth.generated.h"

class AActor;
UENUM(BlueprintType)
enum class EMistDamageKind : uint8 { Direct, BasicAttack, DamageOverTime };

DECLARE_MULTICAST_DELEGATE_TwoParams(FMistHealthDamageEvent, float, AActor*);
// Called after armor/reductions, before shields or health. Returning true blocks
// the entire event. Invulnerability has already rejected the event by this point.
DECLARE_DELEGATE_RetVal_ThreeParams(bool, FMistDamageInterceptor, float, AActor*, EMistDamageKind);
DECLARE_MULTICAST_DELEGATE_FourParams(FMistDamageReceivedEvent, float, float, AActor*, EMistDamageKind);

// All damage and healing return the actual amount of health changed.
UCLASS(ClassGroup=(Mistworld), meta=(BlueprintSpawnableComponent))
class MISTWORLD_API UMistCombatHealth : public UActorComponent
{
    GENERATED_BODY()
public:
    UMistCombatHealth();
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Combat") float MaxHealth = 1200.f;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Combat") float Armor = 20.f;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Combat") float MagicResistance = 20.f;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Combat") float Health = 1200.f;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Combat") bool bCountsForUltimateRefund = true;
    // Permanent equipment stat. Released abilities snapshot this multiplier;
    // basic attacks do not use it. Temporary healing/defense reset separately.
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Combat") float AbilityDamageMultiplier = 1.f;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Combat") bool bIsMinion = false;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Combat") bool bIsStructure = false;
    // Aggregate raw percentage-Burn damage per rolling second, across sources.
    // Zero means uncapped. Encounter data assigns the boss-specific value.
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Combat") float PercentageBurnMaxTick = 0.f;
    UFUNCTION(BlueprintCallable) float ReceivePhysicalDamage(float RawDamage, AActor* DamageSource = nullptr,
        EMistDamageKind Kind = EMistDamageKind::Direct);
    UFUNCTION(BlueprintCallable) float ReceiveMagicDamage(float RawDamage, AActor* DamageSource = nullptr,
        EMistDamageKind Kind = EMistDamageKind::Direct);
    UFUNCTION(BlueprintCallable) void ResetHealth();
    bool IsAlive() const { return Health > 0.f; }
    FSimpleMulticastDelegate OnDefeated;
    // Actual HP removed, never raw damage, shield absorption, or overkill.
    // Runs before lethal cleanup so active damage-loss passives can resolve.
    FMistHealthDamageEvent OnHealthLost;
    FMistDamageInterceptor DamageInterceptor;
    FMistDamageReceivedEvent OnDamageReceived;
    bool WasLastDamageBlocked() const { return bLastDamageBlocked; }

    void MarkCombatActivity(float DurationSeconds = 10.f);
    float GetCombatLockRemaining() const;
    void ClearCombatLock();
    void SetShield(float Amount, float DurationSeconds);
    float GetShieldRemainingHealth() const;
    float GetShieldRemainingTime() const;

    float Heal(float BaseHealing, const UMistCombatHealth* Source = nullptr);
    bool SetHealingTheft(UMistCombatHealth* Recipient, float DurationSeconds);
    void ClearHealingTheft(const UMistCombatHealth* Recipient = nullptr);
    float GetHealingTheftRemaining(const UMistCombatHealth* Recipient = nullptr) const;
    void SetDamageReduction(FName Source, float Fraction, float DurationSeconds);
    float GetDamageReduction() const;
    float GetDamageReductionRemaining(FName Source) const;
    void GrantInvulnerability(float DurationSeconds);
    void ClearInvulnerability();
    bool IsInvulnerable() const;
    float GetInvulnerabilityRemaining() const;
    void GrantSpawnProtection(float DurationSeconds = 3.f);
    void EndSpawnProtection();
    bool IsSpawnProtected() const;
    float GetSpawnProtectionRemaining() const;
    void GrantDebuffImmunity(float DurationSeconds);
    void ClearDebuffImmunity();
    bool IsDebuffImmune() const;
    void SetRoot(float DurationSeconds);
    void SetStun(float DurationSeconds);
    void SetSilence(float DurationSeconds);
    bool IsRooted() const;
    bool IsStunned() const;
    bool IsSilenced() const;

    // Aggregate bonuses are useful for stats; named effects add within their category.
    void SetOutgoingHealingBonus(float Fraction);
    void SetIncomingHealingBonus(float Fraction);
    void SetOutgoingHealingModifier(FName Source, float Fraction, float DurationSeconds);
    void SetIncomingHealingModifier(FName Source, float Fraction, float DurationSeconds);
    void SetAntiHeal(FName Source, float Fraction, float DurationSeconds);
    float GetOutgoingHealingBonus() const;
    float GetIncomingHealingBonus() const;
    float GetAntiHeal() const;

protected:
    virtual void BeginPlay() override;

private:
    struct FTimedModifier
    {
        float Fraction = 0.f;
        double ExpiresAt = 0.0;
    };
    TMap<FName, FTimedModifier> DamageReductions;
    TMap<FName, FTimedModifier> OutgoingHealingModifiers;
    TMap<FName, FTimedModifier> IncomingHealingModifiers;
    TMap<FName, FTimedModifier> AntiHealEffects;
    float OutgoingHealingBonus = 0.f;
    float IncomingHealingBonus = 0.f;
    double InvulnerableUntil = 0.0;
    double DebuffImmuneUntil = 0.0;
    double SpawnProtectedUntil = 0.0;
    double RootUntil = 0.0;
    double StunUntil = 0.0;
    double SilenceUntil = 0.0;
    double CombatLockUntil = 0.0;
    float ShieldHealth = 0.f;
    double ShieldUntil = 0.0;
    TWeakObjectPtr<UMistCombatHealth> HealingThief;
    double HealingTheftUntil = 0.0;
    bool bLastDamageBlocked = false;
    double GetTime() const;
    bool CanChangeHealth() const;
    float ReceiveDamage(float RawDamage, float Defense, AActor* DamageSource, EMistDamageKind Kind);
    float ReceiveDeliveredHealing(float DeliveredBeforeRecipientModifiers);
    void SetTimedModifier(TMap<FName, FTimedModifier>& Modifiers, FName Source,
        float Fraction, float DurationSeconds, float Cap);
    float SumActiveModifiers(const TMap<FName, FTimedModifier>& Modifiers) const;
    void SetCrowdControl(double& ExpiresAt, float DurationSeconds);
};
