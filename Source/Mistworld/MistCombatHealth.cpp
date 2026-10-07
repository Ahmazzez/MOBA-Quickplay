#include "MistCombatHealth.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

UMistCombatHealth::UMistCombatHealth()
{
    PrimaryComponentTick.bCanEverTick = false;
}

void UMistCombatHealth::BeginPlay()
{
    Super::BeginPlay();
    // Possession can precede component BeginPlay. Preserve a spawn grant
    // without restarting its clock or retaining unrelated temporary effects.
    const double PriorSpawnExpiry = SpawnProtectedUntil;
    ResetHealth();
    if (CanChangeHealth() && PriorSpawnExpiry > GetTime()) SpawnProtectedUntil = PriorSpawnExpiry;
}

double UMistCombatHealth::GetTime() const
{
    return GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
}

bool UMistCombatHealth::CanChangeHealth() const
{
    return GetOwner() && GetOwner()->HasAuthority();
}

void UMistCombatHealth::ResetHealth()
{
    if (!CanChangeHealth()) return;
    Health = FMath::Max(1.f, MaxHealth);
    DamageReductions.Reset();
    OutgoingHealingModifiers.Reset();
    IncomingHealingModifiers.Reset();
    AntiHealEffects.Reset();
    OutgoingHealingBonus = 0.f;
    IncomingHealingBonus = 0.f;
    InvulnerableUntil = 0.0;
    DebuffImmuneUntil = 0.0;
    SpawnProtectedUntil = 0.0;
    RootUntil = StunUntil = SilenceUntil = 0.0;
    ShieldHealth = 0.f;
    ShieldUntil = 0.0;
    HealingThief.Reset();
    HealingTheftUntil = 0.0;
    // Death/revival do not bypass a combat lock. Sandbox resets clear it
    // explicitly, alongside their intentionally reset ability cooldowns.
}

float UMistCombatHealth::ReceivePhysicalDamage(float RawDamage, AActor* DamageSource, EMistDamageKind Kind)
{
    return ReceiveDamage(RawDamage, Armor, DamageSource, Kind);
}

float UMistCombatHealth::ReceiveMagicDamage(float RawDamage, AActor* DamageSource, EMistDamageKind Kind)
{
    return ReceiveDamage(RawDamage, MagicResistance, DamageSource, Kind);
}

float UMistCombatHealth::ReceiveDamage(float RawDamage, float Defense, AActor* DamageSource, EMistDamageKind Kind)
{
    bLastDamageBlocked = true;
    if (!CanChangeHealth() || !IsAlive() || IsInvulnerable() || !FMath::IsFinite(RawDamage)) return 0.f;
    const float Mitigated = FMath::Max(0.f, RawDamage) * 100.f / (100.f + FMath::Max(0.f, Defense))
        * (1.f - GetDamageReduction());
    if (Mitigated > 0.f && DamageInterceptor.IsBound() && DamageInterceptor.Execute(Mitigated, DamageSource, Kind))
    {
        if (IsValid(DamageSource) && DamageSource != GetOwner())
        {
            MarkCombatActivity();
            if (UMistCombatHealth* SourceHealth = DamageSource->FindComponentByClass<UMistCombatHealth>())
                SourceHealth->MarkCombatActivity();
        }
        return 0.f;
    }
    bLastDamageBlocked = Mitigated <= 0.f;
    ShieldHealth = GetShieldRemainingHealth();
    const float Absorbed = FMath::Min(ShieldHealth, Mitigated);
    ShieldHealth -= Absorbed;
    if (ShieldHealth <= 0.f) ShieldUntil = 0.0;
    const float Actual = FMath::Min(Health, FMath::Max(0.f, Mitigated - Absorbed));
    if ((Actual > 0.f || Absorbed > 0.f) && IsValid(DamageSource) && DamageSource != GetOwner())
    {
        MarkCombatActivity();
        if (UMistCombatHealth* SourceHealth = DamageSource->FindComponentByClass<UMistCombatHealth>())
            SourceHealth->MarkCombatActivity();
    }
    Health = FMath::Max(0.f, Health - Actual);
    if (Actual > 0.f) OnHealthLost.Broadcast(Actual, DamageSource);
    if (Actual > 0.f || Absorbed > 0.f) OnDamageReceived.Broadcast(Actual, Absorbed, DamageSource, Kind);
    if (!IsAlive())
    {
        DamageReductions.Reset();
        OutgoingHealingModifiers.Reset();
        IncomingHealingModifiers.Reset();
        AntiHealEffects.Reset();
        OutgoingHealingBonus = 0.f;
        IncomingHealingBonus = 0.f;
        InvulnerableUntil = 0.0;
        DebuffImmuneUntil = 0.0;
        SpawnProtectedUntil = 0.0;
        RootUntil = StunUntil = SilenceUntil = 0.0;
        ShieldHealth = 0.f;
        ShieldUntil = 0.0;
        HealingThief.Reset();
        HealingTheftUntil = 0.0;
        OnDefeated.Broadcast();
    }
    return Actual;
}

void UMistCombatHealth::MarkCombatActivity(float DurationSeconds)
{
    // Released projectiles/DOT retain source attribution even after death.
    if (!CanChangeHealth() || !FMath::IsFinite(DurationSeconds)) return;
    CombatLockUntil = FMath::Max(CombatLockUntil, GetTime() + FMath::Max(0.f, DurationSeconds));
}

float UMistCombatHealth::GetCombatLockRemaining() const
{
    return FMath::Max(0.f, static_cast<float>(CombatLockUntil - GetTime()));
}

void UMistCombatHealth::ClearCombatLock()
{
    if (CanChangeHealth()) CombatLockUntil = 0.0;
}

void UMistCombatHealth::SetShield(float Amount, float DurationSeconds)
{
    if (!CanChangeHealth() || !IsAlive() || !FMath::IsFinite(Amount) || !FMath::IsFinite(DurationSeconds)) return;
    ShieldHealth = FMath::Max(0.f, Amount);
    ShieldUntil = GetTime() + FMath::Max(0.f, DurationSeconds);
    if (ShieldHealth <= 0.f || DurationSeconds <= 0.f)
    {
        ShieldHealth = 0.f;
        ShieldUntil = 0.0;
    }
}

float UMistCombatHealth::GetShieldRemainingHealth() const
{
    return IsAlive() && ShieldUntil > GetTime() ? ShieldHealth : 0.f;
}

float UMistCombatHealth::GetShieldRemainingTime() const
{
    return GetShieldRemainingHealth() > 0.f ? FMath::Max(0.f, static_cast<float>(ShieldUntil - GetTime())) : 0.f;
}

void UMistCombatHealth::SetTimedModifier(TMap<FName, FTimedModifier>& Modifiers, FName Source,
    float Fraction, float DurationSeconds, float Cap)
{
    if (!CanChangeHealth() || !IsAlive() || Source.IsNone()
        || !FMath::IsFinite(Fraction) || !FMath::IsFinite(DurationSeconds)) return;
    if (DurationSeconds <= 0.f || Fraction <= 0.f)
    {
        Modifiers.Remove(Source);
        return;
    }
    const double Now = GetTime();
    FTimedModifier& Modifier = Modifiers.FindOrAdd(Source);
    const float Current = Modifier.ExpiresAt > Now ? Modifier.Fraction : 0.f;
    Modifier.Fraction = FMath::Max(Current, FMath::Clamp(Fraction, 0.f, Cap));
    Modifier.ExpiresAt = Now + DurationSeconds;
}

void UMistCombatHealth::SetDamageReduction(FName Source, float Fraction, float DurationSeconds)
{
    SetTimedModifier(DamageReductions, Source, Fraction, DurationSeconds, 0.75f);
}

float UMistCombatHealth::GetDamageReduction() const
{
    float DamageMultiplier = 1.f;
    const double Now = GetTime();
    for (const TPair<FName, FTimedModifier>& Entry : DamageReductions)
    {
        if (Entry.Value.ExpiresAt > Now) DamageMultiplier *= 1.f - Entry.Value.Fraction;
    }
    return FMath::Min(0.75f, 1.f - DamageMultiplier);
}

float UMistCombatHealth::GetDamageReductionRemaining(FName Source) const
{
    const FTimedModifier* Modifier = DamageReductions.Find(Source);
    return Modifier ? FMath::Max(0.f, static_cast<float>(Modifier->ExpiresAt - GetTime())) : 0.f;
}

void UMistCombatHealth::GrantInvulnerability(float DurationSeconds)
{
    if (!CanChangeHealth() || !IsAlive() || !FMath::IsFinite(DurationSeconds)) return;
    InvulnerableUntil = FMath::Max(InvulnerableUntil, GetTime() + FMath::Max(0.f, DurationSeconds));
}

void UMistCombatHealth::ClearInvulnerability()
{
    if (CanChangeHealth()) InvulnerableUntil = 0.0;
}

bool UMistCombatHealth::IsInvulnerable() const
{
    return IsAlive() && FMath::Max(InvulnerableUntil, SpawnProtectedUntil) > GetTime();
}

float UMistCombatHealth::GetInvulnerabilityRemaining() const
{
    return IsAlive() ? FMath::Max(0.f, static_cast<float>(FMath::Max(InvulnerableUntil, SpawnProtectedUntil) - GetTime())) : 0.f;
}

void UMistCombatHealth::GrantSpawnProtection(float DurationSeconds)
{
    if (!CanChangeHealth() || !IsAlive() || !FMath::IsFinite(DurationSeconds)) return;
    SpawnProtectedUntil = FMath::Max(SpawnProtectedUntil, GetTime() + FMath::Max(0.f, DurationSeconds));
}

void UMistCombatHealth::EndSpawnProtection()
{
    if (CanChangeHealth()) SpawnProtectedUntil = 0.0;
}

bool UMistCombatHealth::IsSpawnProtected() const
{
    return IsAlive() && SpawnProtectedUntil > GetTime();
}

float UMistCombatHealth::GetSpawnProtectionRemaining() const
{
    return IsAlive() ? FMath::Max(0.f, static_cast<float>(SpawnProtectedUntil - GetTime())) : 0.f;
}

void UMistCombatHealth::GrantDebuffImmunity(float DurationSeconds)
{
    if (!CanChangeHealth() || !IsAlive() || !FMath::IsFinite(DurationSeconds)) return;
    DebuffImmuneUntil = FMath::Max(DebuffImmuneUntil, GetTime() + FMath::Max(0.f, DurationSeconds));
}

void UMistCombatHealth::ClearDebuffImmunity()
{
    if (CanChangeHealth()) DebuffImmuneUntil = 0.0;
}

bool UMistCombatHealth::IsDebuffImmune() const
{
    return IsAlive() && FMath::Max(DebuffImmuneUntil, SpawnProtectedUntil) > GetTime();
}

void UMistCombatHealth::SetCrowdControl(double& ExpiresAt, float DurationSeconds)
{
    if (!CanChangeHealth() || !IsAlive() || IsDebuffImmune() || !FMath::IsFinite(DurationSeconds)) return;
    if (DurationSeconds <= 0.f) ExpiresAt = 0.0;
    else ExpiresAt = FMath::Max(ExpiresAt, GetTime() + DurationSeconds);
}

void UMistCombatHealth::SetRoot(float DurationSeconds) { SetCrowdControl(RootUntil, DurationSeconds); }
void UMistCombatHealth::SetStun(float DurationSeconds) { SetCrowdControl(StunUntil, DurationSeconds); }
void UMistCombatHealth::SetSilence(float DurationSeconds) { SetCrowdControl(SilenceUntil, DurationSeconds); }
bool UMistCombatHealth::IsRooted() const { return IsAlive() && RootUntil > GetTime(); }
bool UMistCombatHealth::IsStunned() const { return IsAlive() && StunUntil > GetTime(); }
bool UMistCombatHealth::IsSilenced() const { return IsAlive() && SilenceUntil > GetTime(); }

void UMistCombatHealth::SetOutgoingHealingBonus(float Fraction)
{
    if (CanChangeHealth() && FMath::IsFinite(Fraction)) OutgoingHealingBonus = FMath::Clamp(Fraction, 0.f, 0.5f);
}

void UMistCombatHealth::SetIncomingHealingBonus(float Fraction)
{
    if (CanChangeHealth() && FMath::IsFinite(Fraction)) IncomingHealingBonus = FMath::Clamp(Fraction, 0.f, 0.5f);
}

void UMistCombatHealth::SetOutgoingHealingModifier(FName Source, float Fraction, float DurationSeconds)
{
    SetTimedModifier(OutgoingHealingModifiers, Source, Fraction, DurationSeconds, 0.5f);
}

void UMistCombatHealth::SetIncomingHealingModifier(FName Source, float Fraction, float DurationSeconds)
{
    SetTimedModifier(IncomingHealingModifiers, Source, Fraction, DurationSeconds, 0.5f);
}

void UMistCombatHealth::SetAntiHeal(FName Source, float Fraction, float DurationSeconds)
{
    if (IsDebuffImmune()) return;
    SetTimedModifier(AntiHealEffects, Source, Fraction, DurationSeconds, 0.75f);
}

float UMistCombatHealth::SumActiveModifiers(const TMap<FName, FTimedModifier>& Modifiers) const
{
    float Total = 0.f;
    const double Now = GetTime();
    for (const TPair<FName, FTimedModifier>& Entry : Modifiers)
    {
        if (Entry.Value.ExpiresAt > Now) Total += Entry.Value.Fraction;
    }
    return Total;
}

float UMistCombatHealth::GetOutgoingHealingBonus() const
{
    return FMath::Min(0.5f, OutgoingHealingBonus + SumActiveModifiers(OutgoingHealingModifiers));
}

float UMistCombatHealth::GetIncomingHealingBonus() const
{
    return FMath::Min(0.5f, IncomingHealingBonus + SumActiveModifiers(IncomingHealingModifiers));
}

float UMistCombatHealth::GetAntiHeal() const
{
    float Strongest = 0.f;
    const double Now = GetTime();
    for (const TPair<FName, FTimedModifier>& Entry : AntiHealEffects)
    {
        if (Entry.Value.ExpiresAt > Now) Strongest = FMath::Max(Strongest, Entry.Value.Fraction);
    }
    return Strongest;
}

float UMistCombatHealth::Heal(float BaseHealing, const UMistCombatHealth* Source)
{
    if (!CanChangeHealth() || !IsAlive() || !FMath::IsFinite(BaseHealing)) return 0.f;
    const UMistCombatHealth* HealingSource = Source ? Source : this;
    float Delivered = FMath::Max(0.f, BaseHealing) * (1.f + HealingSource->GetOutgoingHealingBonus());
    if (GetHealingTheftRemaining() > 0.f)
    {
        // Split before either missing-health cap. The original healer's outgoing
        // multiplier is applied once; redirected healing cannot itself be stolen.
        Delivered *= 0.5f;
        HealingThief->ReceiveDeliveredHealing(Delivered);
    }
    return ReceiveDeliveredHealing(Delivered);
}

float UMistCombatHealth::ReceiveDeliveredHealing(float DeliveredBeforeRecipientModifiers)
{
    if (!CanChangeHealth() || !IsAlive()) return 0.f;
    const float Delivered = DeliveredBeforeRecipientModifiers * (1.f + GetIncomingHealingBonus()) * (1.f - GetAntiHeal());
    const float Actual = FMath::Min(FMath::Max(0.f, MaxHealth - Health), FMath::Max(0.f, Delivered));
    Health += Actual;
    return Actual;
}

bool UMistCombatHealth::SetHealingTheft(UMistCombatHealth* Recipient, float DurationSeconds)
{
    if (!CanChangeHealth() || !IsAlive() || IsDebuffImmune() || bIsStructure || !IsValid(Recipient)
        || Recipient == this || !Recipient->IsAlive() || !FMath::IsFinite(DurationSeconds) || DurationSeconds <= 0.f) return false;
    HealingThief = Recipient;
    HealingTheftUntil = GetTime() + DurationSeconds;
    return true;
}

void UMistCombatHealth::ClearHealingTheft(const UMistCombatHealth* Recipient)
{
    if (!CanChangeHealth() || (Recipient && HealingThief.Get() != Recipient)) return;
    HealingThief.Reset();
    HealingTheftUntil = 0.0;
}

float UMistCombatHealth::GetHealingTheftRemaining(const UMistCombatHealth* Recipient) const
{
    return IsAlive() && HealingThief.IsValid() && HealingThief->IsAlive()
        && (!Recipient || HealingThief.Get() == Recipient)
        ? FMath::Max(0.f, static_cast<float>(HealingTheftUntil - GetTime())) : 0.f;
}
