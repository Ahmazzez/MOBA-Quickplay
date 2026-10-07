// Copyright Epic Games, Inc. All Rights Reserved.
#include "MistworldPlayerController.h"
#include "Mistworld.h"
#include "MistCombatHealth.h"
#include "MistCombatHUD.h"
#include "MistAnimaCombat.h"
#include "MistAnimaAbilityDefinitions.h"
#include "MistAnimaPresentation.h"
#include "MistMeleePresentation.h"
#include "MistTrainingDummy.h"
#include "MistArenaEnemy.h"
#include "MistReturningSword.h"
#include "MistAbyssShockwave.h"
#include "MistAbyssTrail.h"
#include "MistCryptSession.h"
#include "MistCryptGolem.h"
#include "MistCryptGuardian.h"
#include "MistProgressionComponent.h"
#include "MistConstantSpeedPathFollowingComponent.h"
#include "Blueprint/AIBlueprintHelperLibrary.h"
#include "Components/CapsuleComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "NavigationSystem.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
const FName DefenseSource(TEXT("Alphonse.E"));
constexpr float BasicAttackRange = 260.f;
int32 AbilitySlot(const FKey& Key)
{
    return Key == EKeys::Q ? 0 : Key == EKeys::W ? 1 : Key == EKeys::E ? 2 : Key == EKeys::R ? 3 : INDEX_NONE;
}
UMistCombatHealth* HealthOf(const APawn* Pawn)
{
    return Pawn ? Pawn->FindComponentByClass<UMistCombatHealth>() : nullptr;
}
}

#if WITH_DEV_AUTOMATION_TESTS
FString AMistworldPlayerController::AutomationSaveSlot;
#endif

AMistworldPlayerController::AMistworldPlayerController()
{
    bIsTouch = false;
    bDestinationInputAccepted = false;
    PathFollowingComponent = CreateDefaultSubobject<UMistConstantSpeedPathFollowingComponent>(TEXT("Path Following Component"));
    bShowMouseCursor = true;
    DefaultMouseCursor = EMouseCursor::Default;
    static ConstructorHelpers::FObjectFinder<UInputMappingContext> Mapping(TEXT("/Game/TopDown/Input/IMC_Default.IMC_Default"));
    static ConstructorHelpers::FObjectFinder<UInputAction> Click(TEXT("/Game/TopDown/Input/Actions/IA_SetDestination_Click.IA_SetDestination_Click"));
    static ConstructorHelpers::FObjectFinder<UInputAction> Touch(TEXT("/Game/TopDown/Input/Actions/IA_SetDestination_Touch.IA_SetDestination_Touch"));
    static ConstructorHelpers::FObjectFinder<UNiagaraSystem> Cursor(TEXT("/Game/TopDown/Cursor/FX_Cursor_Success.FX_Cursor_Success"));
    DefaultMappingContext = Mapping.Object;
    SetDestinationClickAction = Click.Object;
    SetDestinationTouchAction = Touch.Object;
    FXCursor = Cursor.Object;
}

void AMistworldPlayerController::SetupInputComponent()
{
    Super::SetupInputComponent();
    if (!IsLocalPlayerController()) return;
    if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer()))
        if (DefaultMappingContext) Subsystem->AddMappingContext(DefaultMappingContext, 0);
    UEnhancedInputComponent* Input = Cast<UEnhancedInputComponent>(InputComponent);
    if (!Input || !SetDestinationClickAction || !SetDestinationTouchAction)
    {
        UE_LOG(LogMistworld, Error, TEXT("Top Down movement input assets or Enhanced Input component are missing"));
        return;
    }
    Input->BindAction(SetDestinationClickAction, ETriggerEvent::Started, this, &AMistworldPlayerController::OnInputStarted);
    Input->BindAction(SetDestinationClickAction, ETriggerEvent::Triggered, this, &AMistworldPlayerController::OnSetDestinationTriggered);
    Input->BindAction(SetDestinationClickAction, ETriggerEvent::Completed, this, &AMistworldPlayerController::OnSetDestinationReleased);
    Input->BindAction(SetDestinationClickAction, ETriggerEvent::Canceled, this, &AMistworldPlayerController::OnSetDestinationReleased);
    Input->BindAction(SetDestinationTouchAction, ETriggerEvent::Started, this, &AMistworldPlayerController::OnInputStarted);
    Input->BindAction(SetDestinationTouchAction, ETriggerEvent::Triggered, this, &AMistworldPlayerController::OnTouchTriggered);
    Input->BindAction(SetDestinationTouchAction, ETriggerEvent::Completed, this, &AMistworldPlayerController::OnTouchReleased);
    Input->BindAction(SetDestinationTouchAction, ETriggerEvent::Canceled, this, &AMistworldPlayerController::OnTouchReleased);
}

void AMistworldPlayerController::BeginPlay()
{
    Super::BeginPlay();
    UE_LOG(LogMistworld, Display, TEXT("Combat controller active: %s"), *GetName());
}

void AMistworldPlayerController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (AnimaCombat) AnimaCombat->ResetAttempt();
    SaveCryptProgression();
    Super::EndPlay(EndPlayReason);
}

void AMistworldPlayerController::OnPossess(APawn* InPawn)
{
    if (UMistCombatHealth* PreviousHealth = HealthOf(GetPawn()))
    {
        PreviousHealth->OnDefeated.RemoveAll(this);
        PreviousHealth->OnHealthLost.RemoveAll(this);
    }
    if (AnimaCombat) { AnimaCombat->ResetAttempt(); AnimaCombat->SetHeroActive(false); }
    GetWorldTimerManager().ClearTimer(CombatTimer);
    if (ActiveSword.IsValid()) ActiveSword->DisableCasterBenefits();
    for (TActorIterator<AMistAbyssTrail> It(GetWorld()); It; ++It)
        if (It->GetOwner() == GetPawn()) It->DisableCasterBenefits();
    ActiveSword.Reset();
    if (bDashing) EndDash();
    ClearAim();
    AttackTarget.Reset();
    bCastingCombat = bAwaitingRespawn = false;
    bLoadoutMenuOpen = false;
    bInventoryMenuOpen = false;
    bEnableClickEvents = false;
    AbyssGuardUntil = AbyssWarcryUntil = 0.0;
    AbyssGuardLostHealth = 0.f;
    UltimateRecastUntil = RespawnAt = 0.0;
    Super::OnPossess(InPawn);
    if (HasAuthority() && InPawn && !HealthOf(InPawn))
    {
        UMistCombatHealth* Health = NewObject<UMistCombatHealth>(InPawn, TEXT("MistCombatHealth"));
        InPawn->AddInstanceComponent(Health);
        Health->RegisterComponent();
    }
    if (UMistCombatHealth* Health = HealthOf(InPawn))
    {
        Health->OnDefeated.AddUObject(this, &AMistworldPlayerController::HandleHeroDefeat);
        Health->OnHealthLost.AddUObject(this, &AMistworldPlayerController::HandleHeroHealthLost);
        Health->GrantSpawnProtection(3.f);
    }
    if (InPawn) RespawnTransform = InPawn->GetActorTransform();
    if (ACharacter* Hero = Cast<ACharacter>(InPawn))
        UMistConstantSpeedPathFollowingComponent::ConfigureCharacterMovement(Hero->GetCharacterMovement(), 500.f);
    if (InPawn && !InPawn->FindComponentByClass<UMistMeleePresentation>())
    {
        UMistMeleePresentation* Presentation = NewObject<UMistMeleePresentation>(InPawn, TEXT("MistMeleePresentation"));
        InPawn->AddInstanceComponent(Presentation);
        Presentation->RegisterComponent();
        Presentation->InitializeSword();
    }
    if (InPawn)
        if (UMistMeleePresentation* Presentation = InPawn->FindComponentByClass<UMistMeleePresentation>())
            Presentation->SetAbyssAppearance(IsAbyssEquipped(EKeys::Q));
    InitializeHeroComponents();
    InitializeCryptProgression();
    ApplyHeroStats(false);
    RefreshHeroPresentation();
}

void AMistworldPlayerController::OnInputStarted()
{
    bDestinationInputAccepted = false;
    const UMistCombatHealth* Health = HealthOf(GetPawn());
    if (IsCombatMenuOpen() || bCastingCombat || !Health || !Health->IsAlive() || Health->IsRooted() || Health->IsStunned()) return;
    bDestinationInputAccepted = true;
    AttackTarget.Reset();
    if (!IsAnima())
        if (UMistMeleePresentation* Presentation = GetPawn()->FindComponentByClass<UMistMeleePresentation>()) Presentation->CancelSwing();
    StopMovement();
    UpdateCachedDestination();
}

void AMistworldPlayerController::OnSetDestinationTriggered()
{
    const UMistCombatHealth* Health = HealthOf(GetPawn());
    if (IsCombatMenuOpen() || bCastingCombat || !Health || !Health->IsAlive() || Health->IsRooted() || Health->IsStunned()) return;
    bDestinationInputAccepted = true;
    if (!IsAnima())
        if (UMistMeleePresentation* Presentation = GetPawn()->FindComponentByClass<UMistMeleePresentation>()) Presentation->CancelSwing();
    FollowTime += GetWorld()->GetDeltaSeconds();
    UpdateCachedDestination();
    UAIBlueprintHelperLibrary::SimpleMoveToLocation(this, CachedDestination);
}

void AMistworldPlayerController::OnSetDestinationReleased()
{
    const UMistCombatHealth* Health = HealthOf(GetPawn());
    if (!IsCombatMenuOpen() && !bCastingCombat && bDestinationInputAccepted && Health && Health->IsAlive()
        && !Health->IsRooted() && !Health->IsStunned() && FollowTime <= ShortPressThreshold)
    {
        UAIBlueprintHelperLibrary::SimpleMoveToLocation(this, CachedDestination);
        if (FXCursor) UNiagaraFunctionLibrary::SpawnSystemAtLocation(this, FXCursor, CachedDestination);
    }
    FollowTime = 0.f;
    bDestinationInputAccepted = false;
}

void AMistworldPlayerController::OnTouchTriggered() { bIsTouch = true; OnSetDestinationTriggered(); }
void AMistworldPlayerController::OnTouchReleased() { bIsTouch = false; OnSetDestinationReleased(); }

void AMistworldPlayerController::UpdateCachedDestination()
{
    FHitResult Hit;
    const bool bHit = bIsTouch ? GetHitResultUnderFinger(ETouchIndex::Touch1, ECC_Visibility, true, Hit)
                              : GetHitResultUnderCursor(ECC_Visibility, true, Hit);
    if (bHit) CachedDestination = Hit.Location;
}

FString AMistworldPlayerController::GetQStatus() const { return GetAbilityStatus(EKeys::Q); }

float AMistworldPlayerController::GetCooldownRemaining(const FKey& Key) const
{
    if (!GetWorld()) return 0.f;
    double Deadline = 0.0;
    if (Key == EKeys::Q) Deadline = QReadyAt;
    else if (Key == EKeys::W) Deadline = WReadyAt;
    else if (Key == EKeys::E) Deadline = EReadyAt;
    else if (Key == EKeys::R) Deadline = RReadyAt;
    else if (Key == EKeys::F) Deadline = FlashReadyAt;
    else if (Key == EKeys::SpaceBar) Deadline = AutoReadyAt;
    return static_cast<float>(FMath::Max(0.0, Deadline - GetWorld()->GetTimeSeconds()));
}

float AMistworldPlayerController::GetCooldownDisplayDuration(const FKey& Key) const
{
    if (Key == EKeys::F) return 60.f;
    if (Key == EKeys::SpaceBar) return GetBasicAttackCycle() / GetAttackSpeedMultiplier();
    return GetCooldownDuration(GetEquippedDefinition(Key).Cooldown);
}

FString AMistworldPlayerController::GetAbilityStatus(const FKey& Key) const
{
    if (IsAnima() && AnimaCombat && Key == EKeys::W && AnimaCombat->GetRecastRemaining(EKeys::W) > 0.f) return TEXT("RECAST");
    if (!IsAnima() && Key == EKeys::R && GetUltimateRecastRemaining() > 0.f) return TEXT("RECAST");
    const double ReadyAt = Key == EKeys::Q ? QReadyAt : Key == EKeys::W ? WReadyAt
        : Key == EKeys::E ? EReadyAt : Key == EKeys::R ? RReadyAt : FlashReadyAt;
    const double Remaining = FMath::Max(0.0, ReadyAt - GetWorld()->GetTimeSeconds());
    if (Remaining > 0.0) return FString::Printf(TEXT("%.1fs"), Remaining);
    return ActiveAimKey == Key ? TEXT("AIMING") : TEXT("READY");
}

float AMistworldPlayerController::GetDefenseRemaining() const
{
    if (IsAnima() && AnimaCombat) return AnimaCombat->GetDefenseRemaining();
    const UMistCombatHealth* Health = HealthOf(GetPawn());
    return Health ? Health->GetDamageReductionRemaining(DefenseSource) : 0.f;
}

float AMistworldPlayerController::GetUltimateRecastRemaining() const
{
    return IsAnima() ? 0.f : FMath::Max(0.0, UltimateRecastUntil - GetWorld()->GetTimeSeconds());
}

float AMistworldPlayerController::GetRespawnRemaining() const
{
    return bAwaitingRespawn ? FMath::Max(0.0, RespawnAt - GetWorld()->GetTimeSeconds()) : 0.f;
}

FMistAlphonseAbilityDefinition AMistworldPlayerController::GetEquippedDefinition(const FKey& Key, bool bEmpowered) const
{
    return IsAnima() ? FMistAnimaAbilityDefinitions::Get(Key, IsAbyssEquipped(Key))
        : FMistAlphonseAbilityDefinitions::Get(Key, IsAbyssEquipped(Key), bEmpowered);
}

bool AMistworldPlayerController::IsAbyssEquipped(const FKey& Key) const
{
    const int32 Slot = AbilitySlot(Key);
    return Slot != INDEX_NONE && (EquippedAbyssMask & (1 << Slot)) != 0;
}

bool AMistworldPlayerController::IsNearCheckpoint() const
{
    if (GetPawn())
        if (const AMistCryptSession* Session = GetCryptSession())
            for (FName ID : {FName(TEXT("Crypt.Entrance")), FName(TEXT("Crypt.Reward"))})
            {
                const FVector Point = Session->GetCheckpointTransform(ID).GetLocation();
                if (FVector::Dist2D(GetPawn()->GetActorLocation(), Point) <= 250.f
                    && FMath::Abs(GetPawn()->GetActorLocation().Z - Point.Z) <= 100.f) return true;
            }
    return GetPawn() && FVector::Dist2D(GetPawn()->GetActorLocation(), RespawnTransform.GetLocation()) <= 250.f
        && FMath::Abs(GetPawn()->GetActorLocation().Z - RespawnTransform.GetLocation().Z) <= 100.f;
}

FString AMistworldPlayerController::GetLoadoutLockReason() const
{
    return GetCombatInteractionLockReason(true);
}

FString AMistworldPlayerController::GetCombatInteractionLockReason(bool bRequireCheckpoint) const
{
    const UMistCombatHealth* Health = HealthOf(GetPawn());
    if (!HasAuthority() || !Health || !Health->IsAlive() || bAwaitingRespawn) return TEXT("Respawn before changing loadout");
    if (const AMistCryptSession* Session = GetCryptSession())
        if (const AMistCryptGolem* Golem = Session->GetGolem())
            if (Golem->IsEncounterActive()) return TEXT("Finish the encounter before changing equipment or loadout");
    if (bRequireCheckpoint && !IsNearCheckpoint()) return TEXT("Return to the gold checkpoint circle");
    if (bCastingCombat || bDashing || ActiveAimKey.IsValid()) return TEXT("Finish or cancel the current action");
    if (IsAnima() && AnimaCombat)
    {
        const FString Reason = AnimaCombat->GetActionLockReason();
        if (!Reason.IsEmpty()) return Reason;
    }
    if (ActiveSword.IsValid() || GetUltimateRecastRemaining() > 0.f) return TEXT("Finish the returning-sword sequence");
    if (Health->IsRooted() || Health->IsStunned() || Health->IsSilenced()) return TEXT("Wait for crowd control to end");
    if (GetWarcryRemaining() > 0.f || Health->GetDamageReduction() > 0.f || Health->GetShieldRemainingHealth() > 0.f)
        return TEXT("Wait for active combat buffs to end");
    for (TActorIterator<AMistAbyssTrail> It(GetWorld()); It; ++It)
        if (!It->IsActorBeingDestroyed() && It->GetOwner() == GetPawn()) return TEXT("Wait for your abyss trail to expire");
    for (TActorIterator<AMistAbyssShockwave> It(GetWorld()); It; ++It)
        if (!It->IsActorBeingDestroyed() && It->GetOwner() == GetPawn()) return TEXT("Wait for your shockwave to finish");
    if (Health->GetCombatLockRemaining() > 0.f)
        return FString::Printf(TEXT("Out of combat in %.1fs"), Health->GetCombatLockRemaining());
    return FString();
}

bool AMistworldPlayerController::TryEquipAbility(const FKey& Key, bool bAbyss)
{
    const int32 Slot = AbilitySlot(Key);
    if (Slot == INDEX_NONE || (bAbyss && !bAbyssUnlocked) || !GetLoadoutLockReason().IsEmpty()) return false;
    if (bAbyss) EquippedAbyssMask |= 1 << Slot;
    else EquippedAbyssMask &= ~(1 << Slot);
    if (!IsAnima())
        if (UMistMeleePresentation* Presentation = GetPawn()->FindComponentByClass<UMistMeleePresentation>())
            Presentation->SetAbyssAppearance(IsAbyssEquipped(EKeys::Q));
    // Slot deadlines are intentionally preserved, even when the variant changes.
    SaveCryptProgression();
    return true;
}

bool AMistworldPlayerController::SetAbyssUnlocked(bool bUnlocked)
{
    if (!GetLoadoutLockReason().IsEmpty() || (!bUnlocked && EquippedAbyssMask != 0)) return false;
    bAbyssUnlocked = bUnlocked;
    SaveCryptProgression();
    return true;
}

void AMistworldPlayerController::ToggleLoadoutMenu()
{
    bLoadoutMenuOpen = !bLoadoutMenuOpen;
    if (bLoadoutMenuOpen) bInventoryMenuOpen = false;
    bEnableClickEvents = IsCombatMenuOpen();
    ClickEventKeys.AddUnique(EKeys::LeftMouseButton);
    if (bLoadoutMenuOpen) CancelCombatInput();
}

void AMistworldPlayerController::ToggleInventoryMenu()
{
    if (!Progression) return;
    bInventoryMenuOpen = !bInventoryMenuOpen;
    if (bInventoryMenuOpen) { bLoadoutMenuOpen = false; CancelCombatInput(); }
    bEnableClickEvents = IsCombatMenuOpen();
    ClickEventKeys.AddUnique(EKeys::LeftMouseButton);
}

AMistCryptSession* AMistworldPlayerController::GetCryptSession() const
{
    for (TActorIterator<AMistCryptSession> It(GetWorld()); It; ++It)
        if (!It->IsActorBeingDestroyed()) return *It;
    return nullptr;
}

UMistProgressionComponent* AMistworldPlayerController::GetProgression() const
{
    return Progression;
}

void AMistworldPlayerController::InitializeCryptProgression()
{
    AMistCryptSession* Session = GetCryptSession();
    if (!HasAuthority() || !Session || !GetPawn()) return;
    if (!Progression)
    {
        Progression = NewObject<UMistProgressionComponent>(this, TEXT("MistCryptProgression"));
        AddInstanceComponent(Progression);
        Progression->RegisterComponent();
#if WITH_DEV_AUTOMATION_TESTS
        if (!AutomationSaveSlot.IsEmpty()) Progression->SetSaveSlotForTesting(AutomationSaveSlot);
#endif
        Progression->LoadProgression();
        ActiveHero = Progression->GetActiveHero();
        bAbyssUnlocked = Progression->IsAbyssUnlocked();
        EquippedAbyssMask = Progression->GetLoadoutMask();
        const TArray<float> Cooldowns = Progression->GetCooldowns();
        const double Now = GetWorld()->GetTimeSeconds();
        if (Cooldowns.Num() == 6)
        {
            QReadyAt = Now + Cooldowns[0]; WReadyAt = Now + Cooldowns[1]; EReadyAt = Now + Cooldowns[2];
            RReadyAt = Now + Cooldowns[3]; FlashReadyAt = Now + Cooldowns[4]; AutoReadyAt = Now + Cooldowns[5];
        }
    }
    ActiveCheckpointID = Progression->GetCheckpointID();
    RespawnTransform = Session->GetCheckpointTransform(ActiveCheckpointID);
    if (!GetPawn()->TeleportTo(RespawnTransform.GetLocation(), RespawnTransform.Rotator(), false, false))
    {
        ActiveCheckpointID = TEXT("Crypt.Entrance");
        Progression->SetCheckpoint(ActiveCheckpointID);
        RespawnTransform = Session->GetCheckpointTransform(ActiveCheckpointID);
        GetPawn()->TeleportTo(RespawnTransform.GetLocation(), RespawnTransform.Rotator(), false, false);
    }
    if (UMistMeleePresentation* Presentation = GetPawn()->FindComponentByClass<UMistMeleePresentation>())
        Presentation->SetAbyssAppearance(IsAbyssEquipped(EKeys::Q));
    ApplyHeroStats(true);
    RefreshHeroPresentation();
    UpdateCombatStats();
    NextProgressSaveAt = GetWorld()->GetTimeSeconds() + 2.0;
}

bool AMistworldPlayerController::SaveCryptProgression()
{
    if (!HasAuthority() || !Progression || !GetWorld()) return false;
    if (!Progression->SetActiveHero(ActiveHero)) return false;
    if (bAbyssUnlocked) Progression->SetAbyssUnlocked(true);
    if (!Progression->SetLoadoutMask(EquippedAbyssMask)) return false;
    if (!bAbyssUnlocked) Progression->SetAbyssUnlocked(false);
    Progression->SetCheckpoint(ActiveCheckpointID);
    const double Now = GetWorld()->GetTimeSeconds();
    TArray<float> Cooldowns;
    for (const double Deadline : {QReadyAt, WReadyAt, EReadyAt, RReadyAt, FlashReadyAt, AutoReadyAt})
        Cooldowns.Add(static_cast<float>(FMath::Max(0.0, Deadline - Now)));
    // A save cannot restore a half-finished dash sequence. Preserve its eventual
    // cooldown in crash recovery while keeping live W recasts usable.
    if (IsAnima() && AnimaCombat && AnimaCombat->GetRecastRemaining(EKeys::W) > 0.f)
        Cooldowns[1] = FMath::Max(Cooldowns[1], GetCooldownDuration(GetEquippedDefinition(EKeys::W).Cooldown));
    if (!Progression->SetCooldowns(Cooldowns)) return false;
    return Progression->SaveProgression();
}

bool AMistworldPlayerController::TryActivateCryptCheckpoint(FName ID, const FTransform& Transform)
{
    if (!Progression || !GetPawn() || !GetCombatInteractionLockReason(false).IsEmpty()) return false;
    AMistCryptSession* Session = GetCryptSession();
    if (!Session || (ID != TEXT("Crypt.Entrance") && ID != TEXT("Crypt.Reward"))) return false;
    const FTransform Known = Session->GetCheckpointTransform(ID);
    if (!Known.GetLocation().Equals(Transform.GetLocation(), 1.f)
        || FVector::Dist2D(GetPawn()->GetActorLocation(), Known.GetLocation()) > 250.f
        || FMath::Abs(GetPawn()->GetActorLocation().Z - Known.GetLocation().Z) > 100.f) return false;
    if (ActiveCheckpointID == ID) return true;
    if (!Progression->SetCheckpoint(ID)) return false;
    const FName PreviousID = ActiveCheckpointID;
    const FTransform PreviousTransform = RespawnTransform;
    ActiveCheckpointID = ID;
    RespawnTransform = Known;
    if (SaveCryptProgression()) return true;
    ActiveCheckpointID = PreviousID;
    RespawnTransform = PreviousTransform;
    Progression->SetCheckpoint(PreviousID);
    return false;
}

bool AMistworldPlayerController::TryEquipCryptReward(bool bEquip)
{
    if (!Progression || !GetLoadoutLockReason().IsEmpty()) return false;
    if (!SaveCryptProgression()) return false;
    const bool bChanged = Progression->EquipReward(bEquip);
    UpdateCombatStats();
    return bChanged;
}

float AMistworldPlayerController::GetCooldownDuration(float BaseCooldown) const
{
    const float Haste = FMath::IsFinite(AbilityHaste) ? FMath::Clamp(AbilityHaste, 0.f, 100.f) : 0.f;
    return BaseCooldown * 100.f / (100.f + Haste);
}

float AMistworldPlayerController::GetWarcryRemaining() const
{
    return IsAnima() ? 0.f : FMath::Max(0.0, AbyssWarcryUntil - GetWorld()->GetTimeSeconds());
}

float AMistworldPlayerController::GetAttackSpeedMultiplier() const
{
    return GetWarcryRemaining() > 0.f ? 1.f + GetEquippedDefinition(EKeys::R).MoveAttackSpeedBonus : 1.f;
}

void AMistworldPlayerController::UpdateCombatStats()
{
    const double Now = GetWorld()->GetTimeSeconds();
    if (Now >= AbyssGuardUntil) { AbyssGuardUntil = 0.0; AbyssGuardLostHealth = 0.f; }
    if (Now >= AbyssWarcryUntil) AbyssWarcryUntil = 0.0;
    if (ACharacter* Hero = Cast<ACharacter>(GetPawn()))
        Hero->GetCharacterMovement()->MaxWalkSpeed = 500.f * GetAttackSpeedMultiplier();
    if (UMistCombatHealth* Health = HealthOf(GetPawn()))
        Health->AbilityDamageMultiplier = Progression ? Progression->GetAbilityDamageMultiplier() : 1.f;
}

void AMistworldPlayerController::HandleHeroHealthLost(float ActualHealthLost, AActor* Source)
{
    if (GetWorld()->GetTimeSeconds() >= AbyssGuardUntil || ActualHealthLost <= 0.f) return;
    AbyssGuardLostHealth += ActualHealthLost;
    const int32 Refund = FMath::FloorToInt((AbyssGuardLostHealth + 0.0001f) / 100.f);
    if (Refund > 0)
    {
        AbyssGuardLostHealth = FMath::Max(0.f, AbyssGuardLostHealth - Refund * 100.f);
        RReadyAt = FMath::Max(static_cast<double>(GetWorld()->GetTimeSeconds()), RReadyAt - Refund);
    }
}

bool AMistworldPlayerController::CanUseAbility(const FKey& Key) const
{
    if (Key != EKeys::Q && Key != EKeys::W && Key != EKeys::E && Key != EKeys::R && Key != EKeys::F) return false;
    const UMistCombatHealth* Health = HealthOf(GetPawn());
    if (!HasAuthority() || !Health || !Health->IsAlive() || IsCombatMenuOpen() || bCastingCombat || Health->IsStunned()) return false;
    if (Key != EKeys::F && Health->IsSilenced()) return false;
    if ((Key == EKeys::W || Key == EKeys::F || (Key == EKeys::R && ((!IsAnima() && GetUltimateRecastRemaining() > 0.f)
        || (IsAnima() && !IsAbyssEquipped(EKeys::R))))) && Health->IsRooted()) return false;
    return true;
}

bool AMistworldPlayerController::CanAimAbility(const FKey& Key) const
{
    if (!CanUseAbility(Key)) return false;
    const double Now = GetWorld()->GetTimeSeconds();
    if (Key == EKeys::Q) return Now >= QReadyAt;
    if (Key == EKeys::W)
        return IsAnima() && AnimaCombat && AnimaCombat->GetRecastRemaining(EKeys::W) > 0.f
            ? AnimaCombat->CanRecast(EKeys::W) : Now >= WReadyAt;
    if (Key == EKeys::E) return Now >= EReadyAt;
    if (Key == EKeys::R) return (!IsAnima() && GetUltimateRecastRemaining() > 0.f) || (Now >= RReadyAt && !ActiveSword.IsValid());
    return Now >= FlashReadyAt;
}

bool AMistworldPlayerController::GetCursorDirection(FVector& Direction, FVector* CursorPoint) const
{
    FHitResult Hit;
    if (!GetPawn() || !GetHitResultUnderCursor(ECC_Visibility, false, Hit)) return false;
    Direction = (Hit.ImpactPoint - GetPawn()->GetActorLocation()).GetSafeNormal2D();
    if (CursorPoint) *CursorPoint = Hit.ImpactPoint;
    return !Direction.IsNearlyZero();
}

void AMistworldPlayerController::ClearAim()
{
    ActiveAimKey = FKey();
    bAimingQ = false;
}

void AMistworldPlayerController::UpdateAbilityInput()
{
    if (WasInputKeyJustPressed(EKeys::RightMouseButton) || WasInputKeyJustPressed(EKeys::Escape)) CancelCombatInput();
    for (const FKey& Key : {EKeys::Q, EKeys::W, EKeys::E, EKeys::R})
    {
        if (WasInputKeyJustPressed(Key) && CanAimAbility(Key))
        {
            ClearAim();
            ActiveAimKey = Key;
            bAimingQ = Key == EKeys::Q;
            AttackTarget.Reset();
            // Aim previews neither stop nor replace the current move order.
        }
    }
    if (ActiveAimKey.IsValid() && !CanAimAbility(ActiveAimKey)) ClearAim();
    if (ActiveAimKey.IsValid())
    {
        FVector Direction = FVector::ZeroVector;
        const bool bHasDirection = GetCursorDirection(Direction);
        if (bHasDirection || ActiveAimKey == EKeys::E || (ActiveAimKey == EKeys::R && IsAbyssEquipped(EKeys::R)))
            DrawAbilityPreview(ActiveAimKey, Direction, FColor(80, 220, 255));
        if (!IsInputKeyDown(ActiveAimKey))
        {
            const FKey Key = ActiveAimKey;
            ClearAim();
            if (bHasDirection || Key == EKeys::E || (Key == EKeys::R && IsAbyssEquipped(Key))) CommitAbility(Key, Direction);
        }
    }
    if (WasInputKeyJustPressed(EKeys::F) && CanAimAbility(EKeys::F))
    {
        FVector Direction, Point;
        if (GetCursorDirection(Direction, &Point))
            TryFlash(Direction, FMath::Min(600.f, static_cast<float>(FVector::Dist2D(Point, GetPawn()->GetActorLocation()))));
    }
}

void AMistworldPlayerController::PlayerTick(float DeltaTime)
{
    Super::PlayerTick(DeltaTime);
    if (!IsLocalController() || !GetPawn()) return;
    if (!GetHUD() || !GetHUD()->IsA<AMistCombatHUD>()) ClientSetHUD(AMistCombatHUD::StaticClass());
    if (WasInputKeyJustPressed(EKeys::L)) ToggleLoadoutMenu();
    if (WasInputKeyJustPressed(EKeys::I)) ToggleInventoryMenu();
    if (WasInputKeyJustPressed(EKeys::C))
        if (AMistCryptSession* Session = GetCryptSession()) Session->TryInteract();
    if (WasInputKeyJustPressed(EKeys::B)) TryReplayCryptEncounter();
    UpdateCombatStats();
    if (Progression && GetWorld()->GetTimeSeconds() >= NextProgressSaveAt)
    {
        SaveCryptProgression();
        NextProgressSaveAt = GetWorld()->GetTimeSeconds() + 2.0;
    }
    if (WasInputKeyJustPressed(EKeys::Y)) ResetCombatSandbox();
    if (WasInputKeyJustPressed(EKeys::T)) SpawnTrainingDummy();
    if (WasInputKeyJustPressed(EKeys::J))
        if (UMistCombatHealth* Health = HealthOf(GetPawn())) Health->ReceivePhysicalDamage(120.f);
    UMistCombatHealth* Health = HealthOf(GetPawn());
    if (!Health) return;
    if (!Health->IsAlive())
    {
        HandleHeroDefeat();
        if (GetWorld()->GetTimeSeconds() >= RespawnAt) RespawnHero();
        if (!Health->IsAlive())
        {
            DrawDebugCircle(GetWorld(), GetPawn()->GetActorLocation() - FVector(0.f, 0.f, 85.f), 75.f, 32,
                FColor(255, 70, 70), false, 0.f, 0, 2.f, FVector::ForwardVector, FVector::RightVector, false);
            return;
        }
    }
    if (Health->IsStunned() || Health->IsRooted())
    {
        StopForCommittedAttack();
        if (bDashing) EndDash();
    }
    if (bDashing) UpdateDash(DeltaTime);
    UpdateAbilityInput();
    DrawDebugCircle(GetWorld(), RespawnTransform.GetLocation() - FVector(0.f, 0.f, 85.f), 250.f, 48,
        FColor(220, 180, 90), false, 0.f, 0, 1.f, FVector::ForwardVector, FVector::RightVector, false);
    if (IsAbyssEquipped(EKeys::R))
        DrawDebugCircle(GetWorld(), GetPawn()->GetActorLocation() - FVector(0.f, 0.f, 82.f), 55.f, 32,
            FColor(150, 90, 255), false, 0.f, 0, 1.f, FVector::ForwardVector, FVector::RightVector, false);
    if (Health->IsSpawnProtected())
        DrawDebugCircle(GetWorld(), GetPawn()->GetActorLocation() - FVector(0.f, 0.f, 85.f), 85.f, 32,
            FColor(100, 220, 255), false, 0.f, 0, 3.f, FVector::ForwardVector, FVector::RightVector, false);
    if (GetDefenseRemaining() > 0.f)
        DrawDebugCircle(GetWorld(), GetPawn()->GetActorLocation() - FVector(0.f, 0.f, 85.f), 90.f, 32,
            FColor(80, 255, 150), false, 0.f, 0, 2.f, FVector::ForwardVector, FVector::RightVector, false);
    if (WasInputKeyJustPressed(EKeys::SpaceBar))
    {
        FHitResult Hit;
        GetHitResultUnderCursor(ECC_Visibility, false, Hit);
        RequestBasicAttack(Hit.GetActor());
    }
    if (AttackTarget.IsValid() && !bCastingCombat && !ActiveAimKey.IsValid() && !Health->IsStunned())
    {
        if (!IsBasicAttackTargetValid(AttackTarget.Get(), false)) { AttackTarget.Reset(); StopMovement(); }
        else if (IsBasicAttackTargetValid(AttackTarget.Get(), true))
        {
            StopMovement();
            if (GetWorld()->GetTimeSeconds() >= AutoReadyAt) BeginBasicAttack();
        }
        else if (!Health->IsRooted()) UAIBlueprintHelperLibrary::SimpleMoveToActor(this, AttackTarget.Get());
    }
}

void AMistworldPlayerController::CommitAbility(const FKey& Key, const FVector& Direction)
{
    if (!CanAimAbility(Key)) return;
    CommittedDirection = Direction;
    if (IsAnima() && AnimaCombat) { AnimaCombat->CommitAbility(Key, Direction); return; }
    if (Key == EKeys::Q) StartCleave();
    else if (Key == EKeys::W) StartDashAbility(false);
    else if (Key == EKeys::E) StartDefense();
    else if (Key == EKeys::R)
    {
        if (GetUltimateRecastRemaining() > 0.f) StartDashAbility(true);
        else StartUltimate();
    }
}

void AMistworldPlayerController::StopForCommittedAttack()
{
    if (!bCastingCombat && GetPawn())
        if (!IsAnima())
        if (UMistMeleePresentation* Presentation = GetPawn()->FindComponentByClass<UMistMeleePresentation>()) Presentation->CancelSwing();
    StopMovement();
    bDestinationInputAccepted = false;
    FollowTime = 0.f;
    if (ACharacter* Hero = Cast<ACharacter>(GetPawn()))
    {
        Hero->GetCharacterMovement()->StopMovementImmediately();
        Hero->ConsumeMovementInputVector();
    }
}

void AMistworldPlayerController::StartCleave()
{
    if (!CanAimAbility(EKeys::Q) || CommittedDirection.IsNearlyZero()) return;
    ClearAim();
    AttackTarget.Reset();
    StopForCommittedAttack();
    HealthOf(GetPawn())->EndSpawnProtection();
    GetPawn()->SetActorRotation(CommittedDirection.Rotation());
    bCastingCombat = true;
    const FMistAlphonseAbilityDefinition Def = GetEquippedDefinition(EKeys::Q);
    QReadyAt = GetWorld()->GetTimeSeconds() + GetCooldownDuration(Def.Cooldown);
    if (UMistMeleePresentation* Presentation = GetPawn()->FindComponentByClass<UMistMeleePresentation>())
        Presentation->PlayCleave(IsAbyssEquipped(EKeys::Q), Def.CastTime);
    DrawAbilityPreview(EKeys::Q, CommittedDirection, FColor::Orange, Def.CastTime);
    GetWorldTimerManager().SetTimer(CombatTimer, this, &AMistworldPlayerController::ResolveCleave, Def.CastTime);
}

void AMistworldPlayerController::ResolveCleave()
{
    bCastingCombat = false;
    const UMistCombatHealth* Health = HealthOf(GetPawn());
    if (!Health || !Health->IsAlive() || Health->IsStunned()) return;
    const FMistAlphonseAbilityDefinition Def = GetEquippedDefinition(EKeys::Q);
    DrawCleavePreview(CommittedDirection, FColor::Red, 0.12f);
    bool bQualifyingHit = false;
    TSet<TWeakObjectPtr<AMistTrainingDummy>> DirectHits;
    for (TActorIterator<AMistTrainingDummy> It(GetWorld()); It; ++It)
        if (It->CombatHealth->IsAlive() && OverlapsSector(*It, CommittedDirection, Def.Range, Def.ConeAngle * 0.5f) && HasCombatLineOfSight(*It))
        {
            const bool bImmune = It->CombatHealth->IsInvulnerable();
            DirectHits.Add(*It);
            It->CombatHealth->ReceivePhysicalDamage(Def.DirectDamage * Health->AbilityDamageMultiplier, GetPawn());
            // Training targets represent Elite enemies; ordinary minions do not refund R.
            bQualifyingHit |= !bImmune && It->CombatHealth->bCountsForUltimateRefund;
        }
    if (bQualifyingHit) RReadyAt = FMath::Max(static_cast<double>(GetWorld()->GetTimeSeconds()), RReadyAt - 5.0);
    if (IsAbyssEquipped(EKeys::Q))
    {
        FActorSpawnParameters Params;
        Params.Owner = GetPawn();
        Params.Instigator = GetPawn();
        Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        if (AMistAbyssShockwave* Wave = GetWorld()->SpawnActor<AMistAbyssShockwave>(Params))
            Wave->Initialize(GetPawn(), CommittedDirection, DirectHits);
    }
}

void AMistworldPlayerController::StartDashAbility(bool bEmpowered)
{
    const FKey Key = bEmpowered ? EKeys::R : EKeys::W;
    if (!CanAimAbility(Key) || (bEmpowered && GetUltimateRecastRemaining() <= 0.f)) return;
    const FMistAlphonseAbilityDefinition Def = GetEquippedDefinition(EKeys::W, bEmpowered);
    FVector Endpoint;
    if (CommittedDirection.IsNearlyZero() || !FindLegalMovementEndpoint(CommittedDirection, Def.Range, Endpoint)
        || FVector::Dist2D(GetPawn()->GetActorLocation(), Endpoint) <= 1.f) return;
    ClearAim();
    AttackTarget.Reset();
    StopForCommittedAttack();
    HealthOf(GetPawn())->EndSpawnProtection();
    bEmpoweredDash = bEmpowered;
    DashEndpoint = Endpoint;
    DashSpeed = Def.TravelSpeed;
    DashRadius = Def.Radius;
    DashDamage = Def.DirectDamage * HealthOf(GetPawn())->AbilityDamageMultiplier;
    DashHits.Reset();
    GetPawn()->SetActorRotation(CommittedDirection.Rotation());
    bCastingCombat = true;
    DrawAbilityPreview(Key, CommittedDirection, FColor::Orange, Def.CastTime);
    if (UMistMeleePresentation* Presentation = GetPawn()->FindComponentByClass<UMistMeleePresentation>())
        Presentation->PlayDash(IsAbyssEquipped(EKeys::W));
    if (bEmpowered) UltimateRecastUntil = 0.0;
    else WReadyAt = GetWorld()->GetTimeSeconds() + GetCooldownDuration(Def.Cooldown);
    GetWorldTimerManager().SetTimer(CombatTimer, this, &AMistworldPlayerController::BeginDash, Def.CastTime);
}

void AMistworldPlayerController::BeginDash()
{
    const UMistCombatHealth* Health = HealthOf(GetPawn());
    if (!Health || !Health->IsAlive() || Health->IsStunned() || Health->IsRooted()) { EndDash(); return; }
    if (ACharacter* Hero = Cast<ACharacter>(GetPawn()))
    {
        DashPawnResponse = Hero->GetCapsuleComponent()->GetCollisionResponseToChannel(ECC_Pawn);
        Hero->GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
    }
    bDashing = true;
    if (IsAbyssEquipped(EKeys::W))
    {
        FActorSpawnParameters Params;
        Params.Owner = GetPawn();
        Params.Instigator = GetPawn();
        Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        if (AMistAbyssTrail* Trail = GetWorld()->SpawnActor<AMistAbyssTrail>(Params))
        {
            ActiveTrail = Trail;
            Trail->Initialize(GetPawn(), bEmpoweredDash);
        }
    }
}

void AMistworldPlayerController::UpdateDash(float DeltaTime)
{
    if (DeltaTime <= 0.f) return;
    const FVector From = GetPawn()->GetActorLocation();
    const float Remaining = FVector::Dist2D(From, DashEndpoint);
    if (Remaining <= 1.f) { EndDash(); return; }
    FVector To;
    const float Step = FMath::Min(DashSpeed * DeltaTime, Remaining);
    if (!FindLegalMovementEndpoint(CommittedDirection, Step, To)) { EndDash(); return; }
    // Endpoint legality sweeps terrain and checks each section of navigation.
    // Unit collision is handled as damage and never stops the dash itself.
    GetPawn()->SetActorLocation(To, false);
    if (ACharacter* Hero = Cast<ACharacter>(GetPawn())) Hero->GetCharacterMovement()->StopMovementImmediately();
    ApplyDashHits(From, To);
    if (ActiveTrail.IsValid()) ActiveTrail->AddSegment(From, To);
    DrawDebugLine(GetWorld(), From - FVector(0.f, 0.f, 75.f), To - FVector(0.f, 0.f, 75.f),
        bEmpoweredDash ? FColor::Yellow : FColor::Cyan, false, 0.25f, 0, 4.f);
    if (FVector::Dist2D(From, To) < Step - 1.f || FVector::Dist2D(To, DashEndpoint) <= 1.f) EndDash();
}

void AMistworldPlayerController::EndDash()
{
    if (ActiveTrail.IsValid()) ActiveTrail->FinishAddingSegments();
    ActiveTrail.Reset();
    if (bDashing)
        if (ACharacter* Hero = Cast<ACharacter>(GetPawn()))
            Hero->GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_Pawn, DashPawnResponse);
    bDashing = false;
    bCastingCombat = false;
    StopForCommittedAttack();
}

void AMistworldPlayerController::ApplyDashHits(const FVector& From, const FVector& To)
{
    UMistCombatHealth* CasterHealth = HealthOf(GetPawn());
    for (TActorIterator<AMistTrainingDummy> It(GetWorld()); It; ++It)
    {
        if (DashHits.Contains(*It) || !It->CombatHealth->IsAlive()
            || !OverlapsSweptCapsule(*It, From, To, DashRadius) || !HasCombatLineOfSight(*It)) continue;
        DashHits.Add(*It);
        const float Actual = It->CombatHealth->ReceivePhysicalDamage(DashDamage, GetPawn());
        if (bEmpoweredDash && CasterHealth && CasterHealth->IsAlive()) CasterHealth->Heal(Actual * 0.20f, CasterHealth);
    }
}

void AMistworldPlayerController::StartDefense()
{
    if (!CanAimAbility(EKeys::E)) return;
    ClearAim();
    const FMistAlphonseAbilityDefinition Def = GetEquippedDefinition(EKeys::E);
    EReadyAt = GetWorld()->GetTimeSeconds() + GetCooldownDuration(Def.Cooldown);
    HealthOf(GetPawn())->SetDamageReduction(DefenseSource, Def.ReductionFraction, Def.Duration);
    AbyssGuardUntil = IsAbyssEquipped(EKeys::E) ? GetWorld()->GetTimeSeconds() + Def.Duration : 0.0;
    AbyssGuardLostHealth = 0.f;
    if (UMistMeleePresentation* Presentation = GetPawn()->FindComponentByClass<UMistMeleePresentation>())
        Presentation->PlayGuard(IsAbyssEquipped(EKeys::E), Def.Duration);
    // Instant self-buffs do not interrupt walking.
}

void AMistworldPlayerController::StartUltimate()
{
    if (!CanAimAbility(EKeys::R) || (!IsAbyssEquipped(EKeys::R) && CommittedDirection.IsNearlyZero())) return;
    const FMistAlphonseAbilityDefinition Def = GetEquippedDefinition(EKeys::R);
    ClearAim();
    AttackTarget.Reset();
    StopForCommittedAttack();
    HealthOf(GetPawn())->EndSpawnProtection();
    RReadyAt = GetWorld()->GetTimeSeconds() + GetCooldownDuration(Def.Cooldown);
    UltimateRecastUntil = 0.0;
    bCastingCombat = true;
    if (!CommittedDirection.IsNearlyZero()) GetPawn()->SetActorRotation(CommittedDirection.Rotation());
    DrawAbilityPreview(EKeys::R, CommittedDirection, FColor::Orange, Def.CastTime);
    if (UMistMeleePresentation* Presentation = GetPawn()->FindComponentByClass<UMistMeleePresentation>())
        Presentation->PlayUltimate(IsAbyssEquipped(EKeys::R), Def.CastTime);
    GetWorldTimerManager().SetTimer(CombatTimer, this,
        IsAbyssEquipped(EKeys::R) ? &AMistworldPlayerController::ResolveWarcry : &AMistworldPlayerController::LaunchSword, Def.CastTime);
}

void AMistworldPlayerController::ResolveWarcry()
{
    bCastingCombat = false;
    const UMistCombatHealth* Health = HealthOf(GetPawn());
    if (!Health || !Health->IsAlive() || Health->IsStunned()) return;
    const FMistAlphonseAbilityDefinition Def = GetEquippedDefinition(EKeys::R);
    for (TActorIterator<AMistTrainingDummy> It(GetWorld()); It; ++It)
    {
        const float BodyRadius = It->Capsule->GetScaledCapsuleRadius();
        const FVector Delta = It->GetActorLocation() - GetPawn()->GetActorLocation();
        if (It->CombatHealth->IsAlive() && FMath::Abs(Delta.Z) <= 160.f
            && Delta.SizeSquared2D() <= FMath::Square(Def.Radius + BodyRadius) && HasCombatLineOfSight(*It))
            It->CombatHealth->ReceiveMagicDamage(Def.DirectDamage * Health->AbilityDamageMultiplier, GetPawn());
    }
    AbyssWarcryUntil = GetWorld()->GetTimeSeconds() + Def.Duration;
    UpdateCombatStats();
    DrawDebugCircle(GetWorld(), GetPawn()->GetActorLocation() - FVector(0.f, 0.f, 85.f), Def.Radius, 48,
        FColor(170, 90, 255), false, 0.2f, 0, 3.f, FVector::ForwardVector, FVector::RightVector, false);
}

void AMistworldPlayerController::LaunchSword()
{
    bCastingCombat = false;
    const UMistCombatHealth* Health = HealthOf(GetPawn());
    if (!Health || !Health->IsAlive() || Health->IsStunned()) return;
    FActorSpawnParameters Params;
    Params.Owner = GetPawn();
    Params.Instigator = GetPawn();
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    AMistReturningSword* Sword = GetWorld()->SpawnActor<AMistReturningSword>(GetPawn()->GetActorLocation(), CommittedDirection.Rotation(), Params);
    if (!Sword) return;
    ActiveSword = Sword;
    Sword->OnReturned.BindUObject(this, &AMistworldPlayerController::OnSwordReturned);
    Sword->Initialize(GetPawn(), CommittedDirection);
}

void AMistworldPlayerController::OnSwordReturned()
{
    ActiveSword.Reset();
    const UMistCombatHealth* Health = HealthOf(GetPawn());
    if (Health && Health->IsAlive()) UltimateRecastUntil = GetWorld()->GetTimeSeconds() + 2.0;
}

bool AMistworldPlayerController::FindLegalMovementEndpoint(const FVector& Direction, float Distance, FVector& Endpoint) const
{
    const ACharacter* Hero = Cast<ACharacter>(GetPawn());
    UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
    if (!Hero || !Nav || Direction.IsNearlyZero() || Distance <= 0.f) return false;
    const UCapsuleComponent* Capsule = Hero->GetCapsuleComponent();
    const float Radius = Capsule->GetScaledCapsuleRadius();
    const float Height = Capsule->GetScaledCapsuleHalfHeight();
    const float MaxStep = Hero->GetCharacterMovement()->MaxStepHeight + 5.f;
    const FVector Start = Hero->GetActorLocation();
    Endpoint = Start;
    FCollisionObjectQueryParams Objects;
    Objects.AddObjectTypesToQuery(ECC_WorldStatic);
    Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
    FCollisionQueryParams Params(SCENE_QUERY_STAT(MistMovementTerrain), false, Hero);
    const int32 Samples = FMath::Max(1, FMath::CeilToInt(Distance / 25.f));
    for (int32 Index = 1; Index <= Samples; ++Index)
    {
        FVector Candidate = Start + Direction * FMath::Min(Distance, Index * 25.f);
        Candidate.Z = Endpoint.Z;
        FNavLocation Projected;
        if (!Nav->ProjectPointToNavigation(Candidate, Projected, FVector(20.f, 20.f, Height + MaxStep))
            || FVector::Dist2D(Candidate, Projected.Location) > 15.f) break;
        FHitResult Ground;
        if (!GetWorld()->LineTraceSingleByObjectType(Ground, Candidate + FVector(0.f, 0.f, MaxStep),
            Candidate - FVector(0.f, 0.f, Height + MaxStep + 20.f), Objects, Params)
            || Ground.ImpactNormal.Z < Hero->GetCharacterMovement()->GetWalkableFloorZ()) break;
        Candidate.Z = Ground.ImpactPoint.Z + Height + 2.f;
        if (FMath::Abs(Candidate.Z - Endpoint.Z) > MaxStep) break;
        FHitResult Block;
        if (GetWorld()->SweepSingleByObjectType(Block, Endpoint, Candidate, FQuat::Identity, Objects,
            FCollisionShape::MakeCapsule(FMath::Max(1.f, Radius - 1.f), FMath::Max(Radius, Height - 2.f)), Params)) break;
        Endpoint = Candidate;
    }
    return FVector::Dist2D(Start, Endpoint) > KINDA_SMALL_NUMBER;
}

void AMistworldPlayerController::TryFlash(const FVector& Direction, float Distance)
{
    if (!CanAimAbility(EKeys::F)) return;
    FVector Endpoint;
    if (!FindLegalMovementEndpoint(Direction, FMath::Min(Distance, 600.f), Endpoint)
        || FVector::Dist2D(GetPawn()->GetActorLocation(), Endpoint) <= 1.f) return;
    const FVector Start = GetPawn()->GetActorLocation();
    ClearAim();
    AttackTarget.Reset();
    StopForCommittedAttack();
    GetPawn()->SetActorLocation(Endpoint, false);
    FlashReadyAt = GetWorld()->GetTimeSeconds() + 60.0;
    if (IsAnima()) { if (AnimaPresentation) AnimaPresentation->PlayDash(0.15f); }
    else if (UMistMeleePresentation* Presentation = GetPawn()->FindComponentByClass<UMistMeleePresentation>())
        Presentation->PlayFlash();
    DrawDebugSphere(GetWorld(), Start, 45.f, 16, FColor::Cyan, false, 0.30f);
    DrawDebugSphere(GetWorld(), Endpoint, 45.f, 16, FColor::Cyan, false, 0.30f);
}

bool AMistworldPlayerController::HasCombatLineOfSight(AActor* Target) const
{
    FHitResult Hit;
    FCollisionQueryParams Params(SCENE_QUERY_STAT(MistCombatVisibility), false, GetPawn());
    FCollisionObjectQueryParams Objects;
    Objects.AddObjectTypesToQuery(ECC_WorldStatic);
    Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
    // Units do not shield other units from cleaves, dashes, or selected autos.
    return !GetWorld()->LineTraceSingleByObjectType(Hit, GetPawn()->GetActorLocation(), Target->GetActorLocation(), Objects, Params);
}

bool AMistworldPlayerController::OverlapsSector(AActor* Target, const FVector& Direction, float Range, float HalfAngle) const
{
    const FVector Delta = Target->GetActorLocation() - GetPawn()->GetActorLocation();
    if (FMath::Abs(Delta.Z) > 160.f) return false;
    const FVector Point(Delta.X, Delta.Y, 0.f);
    const float Distance = Point.Size();
    const UCapsuleComponent* Capsule = Target->FindComponentByClass<UCapsuleComponent>();
    const float Radius = Capsule ? Capsule->GetScaledCapsuleRadius() : 0.f;
    if (Distance <= Radius) return true;
    const float Angle = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(Point / Distance, Direction), -1.f, 1.f)));
    if (Angle <= HalfAngle) return Distance <= Range + Radius;
    const FVector EdgeA = Direction.RotateAngleAxis(HalfAngle, FVector::UpVector) * Range;
    const FVector EdgeB = Direction.RotateAngleAxis(-HalfAngle, FVector::UpVector) * Range;
    return FMath::PointDistToSegment(Point, FVector::ZeroVector, EdgeA) <= Radius
        || FMath::PointDistToSegment(Point, FVector::ZeroVector, EdgeB) <= Radius;
}

bool AMistworldPlayerController::OverlapsSweptCapsule(AActor* Target, const FVector& From, const FVector& To, float Radius) const
{
    const UCapsuleComponent* Capsule = Target->FindComponentByClass<UCapsuleComponent>();
    if (!Capsule) return false;
    const FVector Closest = FMath::ClosestPointOnSegment(Target->GetActorLocation(), From, To);
    const float AxisHalfHeight = FMath::Max(0.f, Capsule->GetScaledCapsuleHalfHeight() - Capsule->GetScaledCapsuleRadius());
    const FVector OnAxis(Target->GetActorLocation().X, Target->GetActorLocation().Y,
        FMath::Clamp(Closest.Z, Target->GetActorLocation().Z - AxisHalfHeight, Target->GetActorLocation().Z + AxisHalfHeight));
    return FVector::DistSquared(Closest, OnAxis) <= FMath::Square(Radius + Capsule->GetScaledCapsuleRadius());
}

void AMistworldPlayerController::DrawCleavePreview(const FVector& Direction, const FColor& Color, float Lifetime) const
{
    const ACharacter* Hero = Cast<ACharacter>(GetPawn());
    const float Height = Hero ? Hero->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() : 96.f;
    const FVector Origin = GetPawn()->GetActorLocation() - FVector(0.f, 0.f, Height - 10.f);
    const FMistAlphonseAbilityDefinition Def = GetEquippedDefinition(EKeys::Q);
    FCollisionObjectQueryParams Objects;
    Objects.AddObjectTypesToQuery(ECC_WorldStatic);
    Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
    FCollisionQueryParams Params(SCENE_QUERY_STAT(MistCleavePreviewTerrain), false, GetPawn());
    FVector Previous = Origin;
    FVector PreviousFull = Origin;
    bool bPreviousBlocked = false;
    for (int32 Step = 0; Step <= 24; ++Step)
    {
        const FVector Ray = Direction.RotateAngleAxis(-Def.ConeAngle * 0.5f + Def.ConeAngle * Step / 24.f, FVector::UpVector);
        const FVector Full = Origin + Ray * Def.Range;
        FHitResult Hit;
        const bool bBlocked = GetWorld()->LineTraceSingleByObjectType(Hit, GetPawn()->GetActorLocation(),
            GetPawn()->GetActorLocation() + Ray * Def.Range, Objects, Params);
        const FVector Next = Origin + Ray * (bBlocked ? Hit.Distance : Def.Range);
        if (Step == 0) DrawDebugLine(GetWorld(), Origin, Next, Color, false, Lifetime, 0, 2.f);
        else DrawDebugLine(GetWorld(), Previous, Next, Color, false, Lifetime, 0, 2.f);
        if (bBlocked)
            DrawDebugLine(GetWorld(), Next, Full, FColor(150, 55, 55), false, Lifetime, 0, 1.f);
        if (Step > 0 && (bBlocked || bPreviousBlocked))
            DrawDebugLine(GetWorld(), PreviousFull, Full, FColor(150, 55, 55), false, Lifetime, 0, 1.f);
        Previous = Next;
        PreviousFull = Full;
        bPreviousBlocked = bBlocked;
    }
    DrawDebugLine(GetWorld(), Origin, Previous, Color, false, Lifetime, 0, 2.f);
}

void AMistworldPlayerController::DrawLinePreview(const FVector& Direction, float Distance, float Radius, const FColor& Color, float Lifetime) const
{
    const ACharacter* Hero = Cast<ACharacter>(GetPawn());
    const float Height = Hero ? Hero->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() : 96.f;
    const FVector Origin = GetPawn()->GetActorLocation() - FVector(0.f, 0.f, Height - 10.f);
    const FVector Side = FVector::CrossProduct(Direction, FVector::UpVector) * Radius;
    const FVector End = Origin + Direction * Distance;
    DrawDebugLine(GetWorld(), Origin + Side, End + Side, Color, false, Lifetime, 0, 2.f);
    DrawDebugLine(GetWorld(), Origin - Side, End - Side, Color, false, Lifetime, 0, 2.f);
    DrawDebugCircle(GetWorld(), Origin, Radius, 24, Color, false, Lifetime, 0, 2.f, FVector::ForwardVector, FVector::RightVector, false);
    DrawDebugCircle(GetWorld(), End, Radius, 24, Color, false, Lifetime, 0, 2.f, FVector::ForwardVector, FVector::RightVector, false);
}

void AMistworldPlayerController::DrawAbilityPreview(const FKey& Key, const FVector& Direction, const FColor& Color, float Lifetime) const
{
    if (IsAnima() && AnimaCombat) { AnimaCombat->DrawPreview(Key, Direction, Color, Lifetime); return; }
    if (Key == EKeys::Q) DrawCleavePreview(Direction, Color, Lifetime);
    if (Key == EKeys::Q && !IsAbyssEquipped(Key)) return;
    if (Key == EKeys::E)
    {
        DrawDebugCircle(GetWorld(), GetPawn()->GetActorLocation() - FVector(0.f, 0.f, 85.f), 90.f, 32,
            Color, false, Lifetime, 0, 2.f, FVector::ForwardVector, FVector::RightVector, false);
        return;
    }
    if (Key == EKeys::R && IsAbyssEquipped(Key))
    {
        DrawDebugCircle(GetWorld(), GetPawn()->GetActorLocation() - FVector(0.f, 0.f, 85.f),
            GetEquippedDefinition(Key).Radius, 48, Color, false, Lifetime, 0, 2.f,
            FVector::ForwardVector, FVector::RightVector, false);
        return;
    }
    const bool bRecast = Key == EKeys::R && GetUltimateRecastRemaining() > 0.f;
    const FMistAlphonseAbilityDefinition Def = GetEquippedDefinition(bRecast ? EKeys::W : Key, bRecast);
    const float Range = Key == EKeys::Q ? Def.ShockwaveRange : Def.Range;
    const float Radius = Key == EKeys::Q ? Def.ShockwaveRadius : Def.Radius;
    FVector Endpoint;
    float LegalRange = Range;
    bool bLegal = true;
    if (Key == EKeys::W || bRecast)
    {
        bLegal = FindLegalMovementEndpoint(Direction, Range, Endpoint);
        LegalRange = bLegal ? FVector::Dist2D(GetPawn()->GetActorLocation(), Endpoint) : 0.f;
    }
    else
    {
        FHitResult Hit;
        FCollisionObjectQueryParams Objects;
        Objects.AddObjectTypesToQuery(ECC_WorldStatic);
        Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
        if (GetWorld()->SweepSingleByObjectType(Hit, GetPawn()->GetActorLocation(), GetPawn()->GetActorLocation() + Direction * Range,
            FQuat::Identity, Objects, FCollisionShape::MakeSphere(Radius), FCollisionQueryParams(SCENE_QUERY_STAT(MistSwordPreview), false, GetPawn())))
            LegalRange = Range * Hit.Time;
    }
    DrawLinePreview(Direction, LegalRange, Radius, bLegal ? Color : FColor::Red, Lifetime);
    if ((Key == EKeys::W || bRecast) && Def.TrailRadius > 0.f)
        DrawLinePreview(Direction, LegalRange, Def.TrailRadius, FColor(155, 100, 200), Lifetime);
    if (LegalRange < Range - 1.f)
    {
        const FVector Origin = GetPawn()->GetActorLocation() - FVector(0.f, 0.f, 85.f);
        DrawDebugLine(GetWorld(), Origin + Direction * LegalRange, Origin + Direction * Range, FColor(200, 65, 65), false, Lifetime, 0, 1.f);
    }
}

bool AMistworldPlayerController::IsBasicAttackTargetValid(AActor* Target, bool bRequireRange) const
{
    if (!GetPawn() || !IsValid(Target) || Target == GetPawn() || Target->IsActorBeingDestroyed() || Target->IsHidden()) return false;
    const UMistCombatHealth* TargetHealth = Target->FindComponentByClass<UMistCombatHealth>();
    if (!TargetHealth || !TargetHealth->IsAlive() || TargetHealth->IsInvulnerable()) return false;
    const FVector Delta = Target->GetActorLocation() - GetPawn()->GetActorLocation();
    if (FMath::Abs(Delta.Z) > 160.f) return false;
    const UCapsuleComponent* TargetCapsule = Target->FindComponentByClass<UCapsuleComponent>();
    const float Range = IsAnima() ? 200.f + (TargetCapsule ? TargetCapsule->GetScaledCapsuleRadius() : 0.f) : BasicAttackRange;
    return !bRequireRange || (Delta.SizeSquared2D() <= FMath::Square(Range) && HasCombatLineOfSight(Target));
}

void AMistworldPlayerController::RequestBasicAttack(AActor* HoveredTarget)
{
    const UMistCombatHealth* Health = HealthOf(GetPawn());
    if (!HasAuthority() || !Health || !Health->IsAlive() || IsCombatMenuOpen() || Health->IsStunned() || bCastingCombat || ActiveAimKey.IsValid()) return;
    AActor* SelectedTarget = IsBasicAttackTargetValid(HoveredTarget, false) ? HoveredTarget : nullptr;
    if (!SelectedTarget)
    {
        float ClosestDistanceSquared = TNumericLimits<float>::Max();
        // The combat sandbox has one hero and hostile health-bearing targets.
        // Selecting by health component also works for future ranged attack targets.
        for (TActorIterator<AActor> It(GetWorld()); It; ++It)
        {
            if (!IsBasicAttackTargetValid(*It, true)) continue;
            const float DistanceSquared = FVector::DistSquared2D(GetPawn()->GetActorLocation(), It->GetActorLocation());
            if (DistanceSquared < ClosestDistanceSquared)
            {
                ClosestDistanceSquared = DistanceSquared;
                SelectedTarget = *It;
            }
        }
    }
    const bool bReplacingAttackOrder = AttackTarget.IsValid();
    AttackTarget = SelectedTarget;
    if (!SelectedTarget)
    {
        if (bReplacingAttackOrder) StopMovement();
        return;
    }
    if (!Health->IsRooted() && !IsBasicAttackTargetValid(SelectedTarget, true))
        UAIBlueprintHelperLibrary::SimpleMoveToActor(this, SelectedTarget);
}

void AMistworldPlayerController::BeginBasicAttack()
{
    UMistCombatHealth* Health = HealthOf(GetPawn());
    if (!HasAuthority() || !Health || !Health->IsAlive() || IsCombatMenuOpen() || Health->IsStunned() || bCastingCombat || ActiveAimKey.IsValid()
        || GetWorld()->GetTimeSeconds() < AutoReadyAt || !IsBasicAttackTargetValid(AttackTarget.Get(), true)) return;
    CommittedDirection = (AttackTarget->GetActorLocation() - GetPawn()->GetActorLocation()).GetSafeNormal2D();
    StopForCommittedAttack();
    Health->EndSpawnProtection();
    GetPawn()->SetActorRotation(CommittedDirection.Rotation());
    bCastingCombat = true;
    const float AttackRate = GetAttackSpeedMultiplier();
    AutoReadyAt = GetWorld()->GetTimeSeconds() + GetBasicAttackCycle() / AttackRate;
    if (IsAnima())
    {
        if (AnimaCombat) AnimaCombat->BeginAutoSwing(CommittedDirection, 0.18f / AttackRate);
        if (AnimaPresentation) AnimaPresentation->PlayAttack(GetBasicAttackCycle() / AttackRate);
    }
    else if (UMistMeleePresentation* Presentation = GetPawn()->FindComponentByClass<UMistMeleePresentation>()) Presentation->PlaySwing(false, AttackRate);
    GetWorldTimerManager().SetTimer(CombatTimer, this, &AMistworldPlayerController::ResolveBasicAttack, 0.18f / AttackRate);
}

void AMistworldPlayerController::ResolveBasicAttack()
{
    bCastingCombat = false;
    const UMistCombatHealth* Health = HealthOf(GetPawn());
    if (!Health || !Health->IsAlive() || Health->IsStunned() || !AttackTarget.IsValid()) return;
    if (IsBasicAttackTargetValid(AttackTarget.Get(), true))
        if (UMistCombatHealth* TargetHealth = AttackTarget->FindComponentByClass<UMistCombatHealth>())
        {
            if (IsAnima())
            {
                if (AnimaCombat && OverlapsSector(AttackTarget.Get(), CommittedDirection, 200.f, 25.f))
                    AnimaCombat->ApplyBasicAttackDamage(AttackTarget.Get());
            }
            else TargetHealth->ReceivePhysicalDamage(90.f, GetPawn(), EMistDamageKind::BasicAttack);
            DrawDebugLine(GetWorld(), GetPawn()->GetActorLocation(), AttackTarget->GetActorLocation(),
                IsAbyssEquipped(EKeys::Q) ? FColor(170, 70, 255) : FColor::Yellow, false, 0.15f, 0, 4.f);
        }
}

void AMistworldPlayerController::CancelCombatInput()
{
    ClearAim();
    AttackTarget.Reset();
    if (!bCastingCombat) StopForCommittedAttack();
}

void AMistworldPlayerController::HandleHeroDefeat()
{
    const UMistCombatHealth* Health = HealthOf(GetPawn());
    if (!HasAuthority() || !Health || Health->IsAlive() || bAwaitingRespawn) return;
    bAwaitingRespawn = true;
    if (AnimaCombat) AnimaCombat->ResetAttempt();
    RespawnAt = GetWorld()->GetTimeSeconds() + 4.0;
    GetWorldTimerManager().ClearTimer(CombatTimer);
    if (bDashing) EndDash();
    bDashing = bCastingCombat = bEmpoweredDash = false;
    AbyssGuardUntil = AbyssWarcryUntil = 0.0;
    AbyssGuardLostHealth = 0.f;
    UpdateCombatStats();
    ClearAim();
    AttackTarget.Reset();
    DashHits.Reset();
    UltimateRecastUntil = 0.0;
    if (ActiveSword.IsValid()) ActiveSword->DisableCasterBenefits();
    ActiveSword.Reset();
    StopForCommittedAttack();
    if (ACharacter* Hero = Cast<ACharacter>(GetPawn()))
    {
        DefeatedPawnResponse = Hero->GetCapsuleComponent()->GetCollisionResponseToChannel(ECC_Pawn);
        if (!IsAnima())
            if (UMistMeleePresentation* Presentation = Hero->FindComponentByClass<UMistMeleePresentation>())
                Presentation->ResetPresentation();
        Hero->GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
        Hero->GetCharacterMovement()->DisableMovement();
    }
    if (AMistCryptSession* Session = GetCryptSession()) Session->ResetEncounter();
    UE_LOG(LogMistworld, Display, TEXT("%s defeated; respawning at checkpoint in 4 seconds."), *GetHeroDisplayName());
}

bool AMistworldPlayerController::RespawnHero()
{
    UMistCombatHealth* Health = HealthOf(GetPawn());
    if (!HasAuthority() || !GetPawn() || !Health || !bAwaitingRespawn) return false;
    ACharacter* Hero = Cast<ACharacter>(GetPawn());
    if (Hero) Hero->GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_Pawn, DefeatedPawnResponse);
    // TeleportTo checks that the checkpoint can safely fit the capsule. If a
    // blocking object occupies it, retry rather than reviving inside terrain.
    if (!GetPawn()->TeleportTo(RespawnTransform.GetLocation(), RespawnTransform.Rotator(), false, false))
    {
        if (Hero) Hero->GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
        RespawnAt = GetWorld()->GetTimeSeconds() + 0.25;
        return false;
    }
    StopForCommittedAttack();
    Health->ResetHealth();
    Health->GrantSpawnProtection(3.f);
    if (IsAnima()) { if (AnimaPresentation) AnimaPresentation->ResetPresentation(); }
    else if (UMistMeleePresentation* Presentation = GetPawn()->FindComponentByClass<UMistMeleePresentation>())
        Presentation->ResetPresentation();
    if (Hero) Hero->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
    bAwaitingRespawn = false;
    RespawnAt = 0.0;
    // Ability and attack deadlines are deliberately untouched.
    UE_LOG(LogMistworld, Display, TEXT("%s respawned with 3 seconds of spawn protection."), *GetHeroDisplayName());
    return true;
}

void AMistworldPlayerController::SpawnTrainingDummy()
{
    if (!HasAuthority() || bCastingCombat) return;
    FHitResult Hit;
    if (!GetHitResultUnderCursor(ECC_Visibility, false, Hit) || Cast<AMistTrainingDummy>(Hit.GetActor())) return;
    UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
    FNavLocation Location;
    if (!Nav || !Nav->ProjectPointToNavigation(Hit.ImpactPoint, Location, FVector(60.f, 60.f, 200.f))) return;
    FActorSpawnParameters Params;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::DontSpawnIfColliding;
    GetWorld()->SpawnActor<AMistTrainingDummy>(AMistTrainingDummy::StaticClass(), Location.Location + FVector(0.f, 0.f, 100.f), FRotator::ZeroRotator, Params);
}

bool AMistworldPlayerController::TryReplayCryptEncounter()
{
    AMistCryptSession* Session = GetCryptSession();
    UMistCombatHealth* Health = HealthOf(GetPawn());
    if (!HasAuthority() || !Session || !Session->GetGolem() || !Progression
        || !Health || !Health->IsAlive() || bAwaitingRespawn || !Session->IsAtReplayCheckpoint()
        || IsCombatMenuOpen() || bCastingCombat || bDashing || ActiveAimKey.IsValid()
        || ActiveSword.IsValid() || Health->GetCombatLockRemaining() > 0.f
        || Health->IsRooted() || Health->IsStunned() || Health->IsSilenced()) return false;
    const FTransform Entrance = Session->GetCheckpointTransform(TEXT("Crypt.Entrance"));
    // Fit the actual character capsule before clearing the current attempt.
    if (!GetPawn()->TeleportTo(Entrance.GetLocation(), Entrance.Rotator(), false, false)) return false;
    ActiveCheckpointID = TEXT("Crypt.Entrance");
    RespawnTransform = Entrance;
    Progression->SetCheckpoint(ActiveCheckpointID);
    ResetCombatSandbox(true);
    return true;
}

void AMistworldPlayerController::ResetCombatSandbox(bool bReplayCrypt)
{
    if (!HasAuthority()) return;
    if (AnimaCombat) AnimaCombat->ResetAttempt();
    if (IsAnima() && AnimaPresentation) AnimaPresentation->ResetPresentation();
    // Y is an explicit development reset, separate from the normal revive.
    const bool bReviving = bAwaitingRespawn;
    if (bReviving && !RespawnHero()) return;
    if (GetPawn() && !IsAnima())
        if (UMistMeleePresentation* Presentation = GetPawn()->FindComponentByClass<UMistMeleePresentation>()) Presentation->ResetPresentation();
    GetWorldTimerManager().ClearTimer(CombatTimer);
    for (TActorIterator<AMistReturningSword> It(GetWorld()); It; ++It)
        if (It->GetOwner() == GetPawn()) It->Destroy();
    for (TActorIterator<AMistAbyssShockwave> It(GetWorld()); It; ++It)
        if (It->GetOwner() == GetPawn()) It->Destroy();
    for (TActorIterator<AMistAbyssTrail> It(GetWorld()); It; ++It)
        if (It->GetOwner() == GetPawn()) It->Destroy();
    ActiveSword.Reset();
    ClearAim();
    AttackTarget.Reset();
    if (bDashing) EndDash();
    bDashing = bCastingCombat = false;
    bAwaitingRespawn = false;
    RespawnAt = 0.0;
    AbyssGuardUntil = AbyssWarcryUntil = 0.0;
    AbyssGuardLostHealth = 0.f;
    UpdateCombatStats();
    StopForCommittedAttack();
    QReadyAt = WReadyAt = EReadyAt = RReadyAt = FlashReadyAt = AutoReadyAt = UltimateRecastUntil = 0.0;
    if (UMistCombatHealth* Health = HealthOf(GetPawn()))
    {
        Health->ResetHealth();
        Health->ClearCombatLock();
        if (bReviving) Health->GrantSpawnProtection(3.f);
    }
    for (TActorIterator<AMistTrainingDummy> It(GetWorld()); It; ++It)
    {
        if (It->IsA<AMistCryptGolem>() || It->IsA<AMistCryptGuardian>()) continue;
        if (AMistArenaEnemy* Enemy = Cast<AMistArenaEnemy>(*It)) Enemy->ResetEncounter();
        else It->CombatHealth->ResetHealth();
    }
    if (AMistCryptSession* Session = GetCryptSession())
    {
        if (bReplayCrypt) Session->ReplayEncounter();
        else Session->ResetEncounter();
        GetPawn()->TeleportTo(RespawnTransform.GetLocation(), RespawnTransform.Rotator(), false, false);
        HealthOf(GetPawn())->GrantSpawnProtection(3.f);
        SaveCryptProgression();
    }
}


void AMistworldPlayerController::InitializeHeroComponents()
{
    APawn* HeroPawn = GetPawn();
    if (!HeroPawn) return;
    AnimaCombat = HeroPawn->FindComponentByClass<UMistAnimaCombat>();
    if (!AnimaCombat)
    {
        AnimaCombat = NewObject<UMistAnimaCombat>(HeroPawn, TEXT("MistAnimaCombat"));
        HeroPawn->AddInstanceComponent(AnimaCombat);
        AnimaCombat->RegisterComponent();
    }
    AnimaCombat->Initialize(this);
    AnimaPresentation = HeroPawn->FindComponentByClass<UMistAnimaPresentation>();
    if (!AnimaPresentation)
    {
        AnimaPresentation = NewObject<UMistAnimaPresentation>(HeroPawn, TEXT("MistAnimaPresentation"));
        HeroPawn->AddInstanceComponent(AnimaPresentation);
        AnimaPresentation->RegisterComponent();
    }
    AnimaPresentation->Initialize();
}

void AMistworldPlayerController::ApplyHeroStats(bool bResetHealth)
{
    UMistCombatHealth* Health = HealthOf(GetPawn());
    if (!Health) return;
    const float DesiredMax = IsAnima() ? 1000.f : 1200.f;
    const float PreviousFraction = Health->MaxHealth > 0.f ? Health->Health / Health->MaxHealth : 1.f;
    Health->MaxHealth = DesiredMax;
    Health->Armor = Health->MagicResistance = 20.f;
    if (bResetHealth)
    {
        const float SpawnRemaining = Health->GetSpawnProtectionRemaining();
        Health->ResetHealth();
        if (SpawnRemaining > 0.f) Health->GrantSpawnProtection(SpawnRemaining);
    }
    else Health->Health = FMath::Clamp(PreviousFraction * DesiredMax, 0.f, DesiredMax);
    UpdateCombatStats();
}

void AMistworldPlayerController::RefreshHeroPresentation()
{
    if (AnimaCombat) AnimaCombat->SetHeroActive(IsAnima());
    if (AnimaPresentation) AnimaPresentation->SetHeroActive(IsAnima());
    if (GetPawn())
        if (UMistMeleePresentation* Melee = GetPawn()->FindComponentByClass<UMistMeleePresentation>())
        {
            if (USceneComponent* Sword = Melee->GetSwordRoot()) Sword->SetVisibility(!IsAnima(), true);
            if (!IsAnima()) Melee->SetAbyssAppearance(IsAbyssEquipped(EKeys::Q));
        }
}

FString AMistworldPlayerController::GetHeroStatusSummary() const
{
    const FString Status = IsAnima() && AnimaCombat ? AnimaCombat->GetStatusSummary() : FString();
    return IsAnima() && Status.IsEmpty() ? TEXT("Rapier / Light armor") : Status;
}

FString AMistworldPlayerController::GetCharacterSwitchLockReason() const
{
    return GetCombatInteractionLockReason(true);
}

bool AMistworldPlayerController::TrySwitchHero(EMistHeroId Hero)
{
    if (!IsKnownMistHero(Hero) || !GetCharacterSwitchLockReason().IsEmpty()) return false;
    if (Hero == ActiveHero) return true;
    UMistCombatHealth* Health = HealthOf(GetPawn());
    if (!Health) return false;
    if (Progression && !SaveCryptProgression()) return false;
    const EMistHeroId PreviousHero = ActiveHero;
    FHeroRuntimeSnapshot& Departing = HeroRuntimeSnapshots.FindOrAdd(ActiveHero);
    Departing.AlternateMask = EquippedAbyssMask;
    Departing.bAscensionUnlocked = bAbyssUnlocked;
    Departing.HealthFraction = Health->Health / FMath::Max(1.f, Health->MaxHealth);
    Departing.CooldownDeadlines = {QReadyAt, WReadyAt, EReadyAt, RReadyAt, FlashReadyAt, AutoReadyAt};
    const FHeroRuntimeSnapshot PreviousSnapshot = Departing;
    if (Progression && !Progression->SetActiveHero(Hero)) return false;
    CancelCombatInput();
    GetWorldTimerManager().ClearTimer(CombatTimer);
    if (AnimaCombat) AnimaCombat->ResetAttempt();
    ActiveHero = Hero;
    FHeroRuntimeSnapshot& Arriving = HeroRuntimeSnapshots.FindOrAdd(Hero);
    EquippedAbyssMask = Progression ? Progression->GetLoadoutMask() : Arriving.AlternateMask;
    bAbyssUnlocked = Progression ? Progression->IsAbyssUnlocked() : Arriving.bAscensionUnlocked;
    const double Now = GetWorld()->GetTimeSeconds();
    TArray<double> Deadlines = Arriving.CooldownDeadlines;
    if (Progression)
    {
        const TArray<float> Seconds = Progression->GetCooldowns();
        Deadlines.Reset();
        for (float Remaining : Seconds) Deadlines.Add(Now + Remaining);
    }
    if (Deadlines.Num() != 6) Deadlines.Init(0.0, 6);
    QReadyAt = Deadlines[0]; WReadyAt = Deadlines[1]; EReadyAt = Deadlines[2];
    RReadyAt = Deadlines[3]; FlashReadyAt = Deadlines[4]; AutoReadyAt = Deadlines[5];
    UltimateRecastUntil = AbyssGuardUntil = AbyssWarcryUntil = 0.0;
    AbyssGuardLostHealth = 0.f;
    ApplyHeroStats(true);
    Health->Health = FMath::Clamp(Arriving.HealthFraction, 0.001f, 1.f) * Health->MaxHealth;
    // A roster change is never a fresh spawn or an escape from combat.
    Health->EndSpawnProtection();
    RefreshHeroPresentation();
    if (Progression && !SaveCryptProgression())
    {
        // Keep the visible selection consistent with the last successful save.
        Progression->SetActiveHero(PreviousHero);
        ActiveHero = PreviousHero;
        EquippedAbyssMask = PreviousSnapshot.AlternateMask;
        bAbyssUnlocked = PreviousSnapshot.bAscensionUnlocked;
        const TArray<double>& PreviousDeadlines = PreviousSnapshot.CooldownDeadlines;
        QReadyAt = PreviousDeadlines[0]; WReadyAt = PreviousDeadlines[1]; EReadyAt = PreviousDeadlines[2];
        RReadyAt = PreviousDeadlines[3]; FlashReadyAt = PreviousDeadlines[4]; AutoReadyAt = PreviousDeadlines[5];
        ApplyHeroStats(true);
        Health->Health = PreviousSnapshot.HealthFraction * Health->MaxHealth;
        Health->EndSpawnProtection();
        RefreshHeroPresentation();
        return false;
    }
    UE_LOG(LogMistworld, Display, TEXT("Checkpoint hero switched to %s."), *GetHeroDisplayName());
    return true;
}
