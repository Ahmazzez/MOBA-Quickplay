// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
#include "MistAlphonseAbilityDefinitions.h"
#include "MistHeroTypes.h"
#include "MistworldPlayerController.generated.h"

class UNiagaraSystem;
class UInputMappingContext;
class UInputAction;
class UPathFollowingComponent;
class AMistReturningSword;
class AMistAbyssTrail;
class AMistCryptSession;
class UMistProgressionComponent;
class UMistAnimaCombat;
class UMistAnimaPresentation;

UCLASS()
class AMistworldPlayerController : public APlayerController
{
    GENERATED_BODY()
    friend class UMistAnimaCombat;
#if WITH_DEV_AUTOMATION_TESTS
    friend struct FMistCombatInputTestAccess;
    friend struct FMistAbilityTestAccess;
    friend struct FMistMeleePresentationTestAccess;
    friend struct FMistDashTickTestAccess;
    friend struct FMistRespawnTestAccess;
    friend struct FMistTargetingGeometryTestAccess;
    friend struct FMistAscensionTestAccess;
    friend struct FMistCryptSliceTestAccess;
    friend struct FMistHeroSwitchTestAccess;
    friend struct FMistAnimaTestAccess;
    friend struct FMistAnimaInputTestAccess;
#endif
public:
    AMistworldPlayerController();
    virtual void BeginPlay() override;
    virtual void OnPossess(APawn* InPawn) override;
    virtual void PlayerTick(float DeltaTime) override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    UFUNCTION(BlueprintPure, Category="Heroes") EMistHeroId GetActiveHero() const { return ActiveHero; }
    bool IsAnima() const { return ActiveHero == EMistHeroId::Anima; }
    FString GetHeroDisplayName() const { return GetMistHeroDisplayName(ActiveHero); }
    FString GetAscensionDisplayName() const { return IsAnima() ? TEXT("Flamestealer") : TEXT("Abyss"); }
    FString GetHeroStatusSummary() const;
    FString GetCharacterSwitchLockReason() const;
    UFUNCTION(BlueprintCallable, Category="Heroes") bool TrySwitchHero(EMistHeroId Hero);
    UMistAnimaCombat* GetAnimaCombat() const { return AnimaCombat; }
    FString GetQStatus() const;
    FString GetAbilityStatus(const FKey& Key) const;
    float GetCooldownRemaining(const FKey& Key) const;
    float GetCooldownDisplayDuration(const FKey& Key) const;
    float GetDefenseRemaining() const;
    float GetUltimateRecastRemaining() const;
    bool IsCombatBusy() const { return bCastingCombat; }
    bool IsRespawning() const { return bAwaitingRespawn; }
    float GetRespawnRemaining() const;
    void OnSwordReturned();
    bool TryEquipAbility(const FKey& Key, bool bAbyss);
    bool IsAbyssEquipped(const FKey& Key) const;
    bool SetAbyssUnlocked(bool bUnlocked);
    bool IsAbyssUnlocked() const { return bAbyssUnlocked; }
    bool IsNearCheckpoint() const;
    FString GetLoadoutLockReason() const;
    bool IsLoadoutMenuOpen() const { return bLoadoutMenuOpen; }
    void ToggleLoadoutMenu();
    bool IsInventoryMenuOpen() const { return bInventoryMenuOpen; }
    bool IsCombatMenuOpen() const { return bLoadoutMenuOpen || bInventoryMenuOpen; }
    void ToggleInventoryMenu();
    UMistProgressionComponent* GetProgression() const;
    AMistCryptSession* GetCryptSession() const;
    FName GetActiveCheckpointID() const { return ActiveCheckpointID; }
    bool TryActivateCryptCheckpoint(FName ID, const FTransform& Transform);
    bool TryEquipCryptReward(bool bEquip);
    bool SaveCryptProgression();
    bool TryReplayCryptEncounter();
#if WITH_DEV_AUTOMATION_TESTS
    static FString AutomationSaveSlot;
#endif
    FMistAlphonseAbilityDefinition GetEquippedDefinition(const FKey& Key, bool bEmpowered = false) const;
    float GetWarcryRemaining() const;
    float GetAttackSpeedMultiplier() const;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Combat") float AbilityHaste = 0.f;

protected:
    UPROPERTY(VisibleDefaultsOnly, Category=AI) TObjectPtr<UPathFollowingComponent> PathFollowingComponent;
    UPROPERTY(EditAnywhere, Category="Input") float ShortPressThreshold = 0.2f;
    UPROPERTY(EditAnywhere, Category="Input") TObjectPtr<UNiagaraSystem> FXCursor;
    UPROPERTY(EditAnywhere, Category="Input") TObjectPtr<UInputMappingContext> DefaultMappingContext;
    UPROPERTY(EditAnywhere, Category="Input") TObjectPtr<UInputAction> SetDestinationClickAction;
    UPROPERTY(EditAnywhere, Category="Input") TObjectPtr<UInputAction> SetDestinationTouchAction;
    uint32 bDestinationInputAccepted : 1;
    uint32 bIsTouch : 1;
    FVector CachedDestination = FVector::ZeroVector;
    float FollowTime = 0.f;
    virtual void SetupInputComponent() override;
    void OnInputStarted();
    void OnSetDestinationTriggered();
    void OnSetDestinationReleased();
    void OnTouchTriggered();
    void OnTouchReleased();
    void UpdateCachedDestination();

    void UpdateAbilityInput();
    bool CanAimAbility(const FKey& Key) const;
    bool CanUseAbility(const FKey& Key) const;
    void ClearAim();
    void CommitAbility(const FKey& Key, const FVector& Direction);
    bool GetCursorDirection(FVector& Direction, FVector* CursorPoint = nullptr) const;
    void StopForCommittedAttack();
    void StartCleave();
    void ResolveCleave();
    void StartDashAbility(bool bEmpowered);
    void BeginDash();
    void UpdateDash(float DeltaTime);
    void EndDash();
    void ApplyDashHits(const FVector& From, const FVector& To);
    void StartDefense();
    void StartUltimate();
    void LaunchSword();
    void ResolveWarcry();
    void HandleHeroHealthLost(float ActualHealthLost, AActor* Source);
    void UpdateCombatStats();
    float GetCooldownDuration(float BaseCooldown) const;
    void InitializeCryptProgression();
    FString GetCombatInteractionLockReason(bool bRequireCheckpoint) const;
    void TryFlash(const FVector& Direction, float Distance);
    bool FindLegalMovementEndpoint(const FVector& Direction, float Distance, FVector& Endpoint) const;
    void DrawAbilityPreview(const FKey& Key, const FVector& Direction, const FColor& Color, float Lifetime = 0.f) const;
    void DrawCleavePreview(const FVector& Direction, const FColor& Color, float Lifetime = 0.f) const;
    void DrawLinePreview(const FVector& Direction, float Distance, float Radius, const FColor& Color, float Lifetime) const;
    void RequestBasicAttack(AActor* HoveredTarget);
    bool IsBasicAttackTargetValid(AActor* Target, bool bRequireRange) const;
    void BeginBasicAttack();
    void ResolveBasicAttack();
    void SpawnTrainingDummy();
    void ResetCombatSandbox(bool bReplayCrypt = false);
    void CancelCombatInput();
    void HandleHeroDefeat();
    bool RespawnHero();
    bool HasCombatLineOfSight(AActor* Target) const;
    bool OverlapsSector(AActor* Target, const FVector& Direction, float Range, float HalfAngle) const;
    bool OverlapsSweptCapsule(AActor* Target, const FVector& From, const FVector& To, float Radius) const;

    void InitializeHeroComponents();
    void RefreshHeroPresentation();
    void ApplyHeroStats(bool bResetHealth);
    float GetBasicAttackCycle() const { return IsAnima() ? 0.75f : 1.f; }
    struct FHeroRuntimeSnapshot
    {
        uint8 AlternateMask = 0;
        bool bAscensionUnlocked = false;
        float HealthFraction = 1.f;
        TArray<double> CooldownDeadlines;
    };
    EMistHeroId ActiveHero = EMistHeroId::Alphonse;
    TMap<EMistHeroId, FHeroRuntimeSnapshot> HeroRuntimeSnapshots;
    UPROPERTY(Transient) TObjectPtr<UMistAnimaCombat> AnimaCombat;
    UPROPERTY(Transient) TObjectPtr<UMistAnimaPresentation> AnimaPresentation;
    FKey ActiveAimKey;
    TWeakObjectPtr<AActor> AttackTarget;
    TWeakObjectPtr<AMistReturningSword> ActiveSword;
    FVector CommittedDirection = FVector::ForwardVector;
    double QReadyAt = 0.0;
    double WReadyAt = 0.0;
    double EReadyAt = 0.0;
    double RReadyAt = 0.0;
    double FlashReadyAt = 0.0;
    double AutoReadyAt = 0.0;
    double UltimateRecastUntil = 0.0;
    // Retained for the existing Q movement regression.
    bool bAimingQ = false;
    bool bCastingCombat = false;
    bool bDashing = false;
    bool bEmpoweredDash = false;
    FVector DashEndpoint = FVector::ZeroVector;
    float DashSpeed = 1500.f;
    float DashRadius = 75.f;
    float DashDamage = 100.f;
    ECollisionResponse DashPawnResponse = ECR_Block;
    TSet<TWeakObjectPtr<AActor>> DashHits;
    FTimerHandle CombatTimer;
    // This sandbox has one checkpoint. Future modes can choose an active node
    // before calling the same revive/protection rules.
    FTransform RespawnTransform = FTransform::Identity;
    double RespawnAt = 0.0;
    bool bAwaitingRespawn = false;
    ECollisionResponse DefeatedPawnResponse = ECR_Block;
    uint8 EquippedAbyssMask = 0;
    bool bAbyssUnlocked = false;
    bool bLoadoutMenuOpen = false;
    bool bInventoryMenuOpen = false;
    UPROPERTY(Transient) TObjectPtr<UMistProgressionComponent> Progression;
    FName ActiveCheckpointID = TEXT("Crypt.Entrance");
    double NextProgressSaveAt = 0.0;
    double AbyssGuardUntil = 0.0;
    double AbyssWarcryUntil = 0.0;
    float AbyssGuardLostHealth = 0.f;
    TWeakObjectPtr<AMistAbyssTrail> ActiveTrail;
};
