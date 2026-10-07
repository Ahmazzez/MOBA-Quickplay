#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "InputCoreTypes.h"
#include "TimerManager.h"
#include "MistCombatHealth.h"
#include "MistAlphonseAbilityDefinitions.h"
#include "MistAnimaCombat.generated.h"

class AMistworldPlayerController;
class UMistAnimaCombat;

// One target-owned ledger aggregates all Anima contributors before the boss cap.
// Each independently timed stack belongs to its caster, not its equipped Q.
UCLASS()
class MISTWORLD_API UMistAnimaBurnStatus : public UActorComponent
{
    GENERATED_BODY()
public:
    UMistAnimaBurnStatus();
    static UMistAnimaBurnStatus* FindOrCreate(AActor* Target);
    void AddStacks(UMistAnimaCombat* Source, int32 Count);
    int32 CountStacks(const UMistAnimaCombat* Source) const;
    float GetRemaining(const UMistAnimaCombat* Source) const;
    void RemoveSource(const UMistAnimaCombat* Source);
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* Function) override;
private:
    friend struct FMistAnimaCombatTestAccess;
    struct FBurnStack
    {
        TWeakObjectPtr<UMistAnimaCombat> Source;
        double AppliedAt = 0.0;
        double NextTick = 0.0;
        int32 TicksLeft = 4;
        uint64 Serial = 0;
        float DamageMultiplier = 1.f;
    };
    struct FCapExpense { double Time = 0.0; float Amount = 0.f; };
    TArray<FBurnStack> Stacks;
    TArray<FCapExpense> CapExpenses;
    uint64 NextSerial = 0;
};

UCLASS(ClassGroup=(Mistworld), meta=(BlueprintSpawnableComponent))
class MISTWORLD_API UMistAnimaCombat : public UActorComponent
{
    GENERATED_BODY()
public:
    UMistAnimaCombat();
    void Initialize(AMistworldPlayerController* InController);
    void SetHeroActive(bool bNewActive);
    bool IsHeroActive() const { return bActive; }
    FMistAlphonseAbilityDefinition GetDefinition(const FKey& Key, bool bFlamestealer) const;
    void CommitAbility(const FKey& Key, const FVector& Direction);
    void DrawPreview(const FKey& Key, const FVector& Direction, const FColor& Color, float Lifetime = 0.f) const;
    void BeginAutoSwing(const FVector& Direction, float Windup = 0.18f);
    float ApplyBasicAttackDamage(AActor* Target);
    void ResetAttempt();
    FString GetActionLockReason() const;
    float GetDefenseRemaining() const;
    FString GetStatusSummary() const;
    bool CanRecast(const FKey& Key) const;
    float GetRecastRemaining(const FKey& Key) const;
    int32 GetDashChargesRemaining() const;
    int32 GetBurnCount(AActor* Target) const;
    AActor* GetMarkedTarget() const;
    AActor* GetBrandedTarget() const;
    // Called only by this caster's actual direct/auto/burn impacts.
    void CreditDamage(AActor* Target, float ActualHealthDamage, bool bBurn, bool bCounter = false);
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* Function) override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
    friend struct FMistAnimaCombatTestAccess;
    TWeakObjectPtr<AMistworldPlayerController> Controller;
    TWeakObjectPtr<UMistCombatHealth> HeroHealth;
    bool bActive = false;
    TWeakObjectPtr<AActor> MarkTarget;
    TWeakObjectPtr<AActor> BrandTarget;
    double MarkUntil = 0.0;
    double BrandUntil = 0.0;
    double StanceUntil = 0.0;
    bool bFlameStance = false;
    double EnhancedAutosUntil = 0.0;
    double AuraUntil = 0.0;
    double NextAuraTick = 0.0;
    double AutoParryFrom = 0.0;
    double AutoParryUntil = 0.0;
    double AutoParryReadyAt = 0.0;
    FVector AutoParryDirection = FVector::ForwardVector;
    int32 DashCommitments = 0;
    double DashRecastUntil = 0.0;
    double NextDashCommitAt = 0.0;
    bool bActiveDash = false;
    bool bUltimateDash = false;
    bool bFlameDash = false;
    bool bDashHealUsed = false;
    FVector CastDirection = FVector::ForwardVector;
    FVector DashEndpoint = FVector::ZeroVector;
    FMistAlphonseAbilityDefinition PendingDefinition;
    bool bPendingAlternate = false;
    float PendingDamageMultiplier = 1.f;
    TSet<TWeakObjectPtr<AActor>> DashHits;
    ECollisionResponse OriginalPawnResponse = ECR_Block;
    FTimerHandle CastTimer;
    TArray<TWeakObjectPtr<AActor>> TheftTargets;
    double Now() const;
    bool IsHostileTarget(AActor* Target, bool bRequireDamageable = true) const;
    bool IsMinion(AActor* Target) const;
    bool InterceptDamage(float Mitigated, AActor* Source, EMistDamageKind Kind);
    void ReceivedDamage(float ActualHP, float ShieldLoss, AActor* Source, EMistDamageKind Kind);
    void ApplyMark(AActor* Target);
    void ApplyBrand(AActor* Target);
    void AddBurn(AActor* Target, int32 Count);
    float DealPhysical(AActor* Target, float RawDamage, bool bCounter = false,
        EMistDamageKind Kind = EMistDamageKind::Direct);
    AActor* FindFirstThrustHit(const FVector& Direction, float Range, float Radius) const;
    void ReleaseThrust();
    void BeginDash();
    void UpdateDash(float DeltaTime);
    void FinishDash();
    void FinishDashSequence();
    void ApplyDashHits(const FVector& From, const FVector& To);
    void ReleaseAura();
    void ReleaseCounter();
    void SetCooldown(const FKey& Key, float Seconds);
    void FinishCast();
    void DrawStatuses() const;
};

