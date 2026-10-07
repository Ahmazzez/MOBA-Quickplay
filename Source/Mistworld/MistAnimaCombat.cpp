#include "MistAnimaCombat.h"
#include "MistAnimaAbilityDefinitions.h"
#include "MistAnimaPresentation.h"
#include "MistworldPlayerController.h"
#include "MistTrainingDummy.h"
#include "MistCryptGolem.h"
#include "MistCryptGuardian.h"
#include "Components/CapsuleComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "TimerManager.h"

namespace
{
const FName RiposteSource(TEXT("Anima.Riposte"));
UMistCombatHealth* HealthOf(AActor* Actor)
{
    return IsValid(Actor) ? Actor->FindComponentByClass<UMistCombatHealth>() : nullptr;
}
bool CanApplyAnimaStatus(AActor* Target)
{
    const UMistCombatHealth* Health = HealthOf(Target);
    // These encounters currently represent permanent CC immunity with the shared
    // debuff flag. Their ordinary damage statuses are eligible, as the outline says.
    return Health && Health->IsAlive() && !Health->bIsStructure && !Health->IsSpawnProtected()
        && (!Health->IsDebuffImmune() || Target->IsA<AMistCryptGolem>() || Target->IsA<AMistCryptGuardian>());
}
float BurnRawPerTick(const UMistCombatHealth* Health)
{
    return Health->MaxHealth * (Health->bIsMinion ? 0.10f : 0.0025f);
}
UMistAnimaPresentation* PresentationOf(AActor* Actor)
{
    return IsValid(Actor) ? Actor->FindComponentByClass<UMistAnimaPresentation>() : nullptr;
}
}

UMistAnimaBurnStatus::UMistAnimaBurnStatus()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.TickGroup = TG_PostPhysics;
}

UMistAnimaBurnStatus* UMistAnimaBurnStatus::FindOrCreate(AActor* Target)
{
    if (!IsValid(Target)) return nullptr;
    if (UMistAnimaBurnStatus* Existing = Target->FindComponentByClass<UMistAnimaBurnStatus>()) return Existing;
    UMistAnimaBurnStatus* Result = NewObject<UMistAnimaBurnStatus>(Target);
    Target->AddInstanceComponent(Result);
    Result->RegisterComponent();
    return Result;
}

void UMistAnimaBurnStatus::AddStacks(UMistAnimaCombat* Source, int32 Count)
{
    if (!GetOwner()->HasAuthority() || !IsValid(Source) || !CanApplyAnimaStatus(GetOwner())) return;
    const double Time = GetWorld()->GetTimeSeconds();
    for (int32 Index = 0; Index < FMath::Clamp(Count, 0, 10); ++Index)
    {
        if (CountStacks(Source) >= 10)
        {
            int32 Oldest = INDEX_NONE;
            for (int32 StackIndex = 0; StackIndex < Stacks.Num(); ++StackIndex)
                if (Stacks[StackIndex].Source.Get() == Source
                    && (Oldest == INDEX_NONE || Stacks[StackIndex].Serial < Stacks[Oldest].Serial)) Oldest = StackIndex;
            if (Oldest != INDEX_NONE) Stacks.RemoveAt(Oldest);
        }
        FBurnStack Stack;
        Stack.Source = Source; Stack.AppliedAt = Time; Stack.NextTick = Time + 1.0;
        Stack.Serial = NextSerial++;
        if (const UMistCombatHealth* SourceHealth = HealthOf(Source->GetOwner()))
            Stack.DamageMultiplier = FMath::IsFinite(SourceHealth->AbilityDamageMultiplier)
                ? FMath::Max(0.f, SourceHealth->AbilityDamageMultiplier) : 1.f;
        Stacks.Add(Stack);
    }
}

int32 UMistAnimaBurnStatus::CountStacks(const UMistAnimaCombat* Source) const
{
    int32 Count = 0;
    const double Time = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
    for (const FBurnStack& Stack : Stacks)
        if (Stack.Source.Get() == Source && Stack.TicksLeft > 0 && Time <= Stack.AppliedAt + 4.0 + KINDA_SMALL_NUMBER) ++Count;
    return Count;
}

float UMistAnimaBurnStatus::GetRemaining(const UMistAnimaCombat* Source) const
{
    double Until = 0.0;
    for (const FBurnStack& Stack : Stacks)
        if (Stack.Source.Get() == Source && Stack.TicksLeft > 0) Until = FMath::Max(Until, Stack.AppliedAt + 4.0);
    return GetWorld() ? FMath::Max(0.f, static_cast<float>(Until - GetWorld()->GetTimeSeconds())) : 0.f;
}

void UMistAnimaBurnStatus::RemoveSource(const UMistAnimaCombat* Source)
{
    Stacks.RemoveAll([Source](const FBurnStack& Stack) { return Stack.Source.Get() == Source; });
    if (Stacks.IsEmpty()) CapExpenses.Reset();
}

void UMistAnimaBurnStatus::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* Function)
{
    Super::TickComponent(DeltaTime, TickType, Function);
    UMistCombatHealth* Health = HealthOf(GetOwner());
    if (!GetOwner()->HasAuthority() || !Health) return;
    if (!Health->IsAlive() || Health->bIsStructure) { Stacks.Reset(); CapExpenses.Reset(); return; }
    Stacks.RemoveAll([](const FBurnStack& Stack) { return !Stack.Source.IsValid() || Stack.TicksLeft <= 0; });
    const double Time = GetWorld()->GetTimeSeconds();
    // Resolve each scheduled instant in order, including hitch frames. Do not
    // turn a hitch into immediate stack reapplication or additional on-hit procs.
    for (int32 Iteration = 0; Iteration < 40; ++Iteration)
    {
        double Earliest = TNumericLimits<double>::Max();
        for (const FBurnStack& Stack : Stacks)
            if (Stack.TicksLeft > 0) Earliest = FMath::Min(Earliest, Stack.NextTick);
        if (Earliest > Time + KINDA_SMALL_NUMBER || !Health->IsAlive()) break;
        TMap<TWeakObjectPtr<UMistAnimaCombat>, float> DueBySource;
        float ActiveDPS = 0.f;
        for (FBurnStack& Stack : Stacks)
        {
            if (Stack.TicksLeft <= 0 || Stack.AppliedAt > Earliest || !Stack.Source.IsValid()) continue;
            const float StackDamage = BurnRawPerTick(Health) * Stack.DamageMultiplier;
            ActiveDPS += StackDamage;
            if (FMath::Abs(Stack.NextTick - Earliest) <= KINDA_SMALL_NUMBER)
            {
                DueBySource.FindOrAdd(Stack.Source) += StackDamage;
                --Stack.TicksLeft;
                Stack.NextTick += 1.0;
            }
        }
        float DueTotal = 0.f;
        for (const auto& Pair : DueBySource) DueTotal += Pair.Value;
        float Fraction = 1.f;
        const float Cap = FMath::Max(0.f, Health->PercentageBurnMaxTick);
        if (Cap > 0.f)
        {
            CapExpenses.RemoveAll([Earliest](const FCapExpense& Entry) { return Entry.Time <= Earliest - 1.0 + KINDA_SMALL_NUMBER; });
            float Spent = 0.f;
            for (const FCapExpense& Entry : CapExpenses) Spent += Entry.Amount;
            Fraction = FMath::Min(1.f, Cap / FMath::Max(ActiveDPS, 0.0001f));
            Fraction = FMath::Min(Fraction, FMath::Max(0.f, Cap - Spent) / FMath::Max(DueTotal, 0.0001f));
            CapExpenses.Add({ Earliest, DueTotal * Fraction });
        }
        TArray<TWeakObjectPtr<UMistAnimaCombat>> Contributors;
        DueBySource.GetKeys(Contributors);
        Contributors.Sort([](const TWeakObjectPtr<UMistAnimaCombat>& Left, const TWeakObjectPtr<UMistAnimaCombat>& Right)
        {
            return Left.IsValid() && (!Right.IsValid() || Left->GetUniqueID() < Right->GetUniqueID());
        });
        if (!Contributors.IsEmpty() && Contributors[0].IsValid() && DueTotal > 0.f)
        {
            // One authoritative damage event evaluates the target's shield,
            // defenses and remaining HP once. Credit the capped actual HP loss
            // proportionally, including lethal overkill, independent of iteration
            // order. A stable representative source drives incoming-hit reactions.
            const float Actual = Health->ReceiveMagicDamage(DueTotal * Fraction,
                Contributors[0]->GetOwner(), EMistDamageKind::DamageOverTime);
            for (const TWeakObjectPtr<UMistAnimaCombat>& Contributor : Contributors)
            {
                UMistAnimaCombat* Source = Contributor.Get();
                if (!Source) continue;
                if (!Health->WasLastDamageBlocked())
                    if (UMistCombatHealth* SourceHealth = HealthOf(Source->GetOwner())) SourceHealth->MarkCombatActivity();
                Source->CreditDamage(GetOwner(), Actual * DueBySource[Contributor] / DueTotal, true);
            }
        }
    }
    Stacks.RemoveAll([](const FBurnStack& Stack) { return Stack.TicksLeft <= 0 || !Stack.Source.IsValid(); });
    if (!Stacks.IsEmpty())
        DrawDebugCircle(GetWorld(), GetOwner()->GetActorLocation() - FVector(0.f, 0.f, 85.f), 54.f, 16,
            FColor(255, 100, 25), false, 0.f, 0, 2.f, FVector::ForwardVector, FVector::RightVector, false);
}

UMistAnimaCombat::UMistAnimaCombat()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.TickGroup = TG_PostPhysics;
}

double UMistAnimaCombat::Now() const { return GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0; }

void UMistAnimaCombat::Initialize(AMistworldPlayerController* InController)
{
    if (HeroHealth.IsValid())
    {
        HeroHealth->DamageInterceptor.Unbind();
        HeroHealth->OnDamageReceived.RemoveAll(this);
    }
    Controller = InController;
    HeroHealth = HealthOf(GetOwner());
    if (HeroHealth.IsValid())
    {
        HeroHealth->DamageInterceptor.BindUObject(this, &UMistAnimaCombat::InterceptDamage);
        HeroHealth->OnDamageReceived.AddUObject(this, &UMistAnimaCombat::ReceivedDamage);
    }
}

void UMistAnimaCombat::SetHeroActive(bool bNewActive)
{
    if (bActive == bNewActive) return;
    if (!bNewActive) ResetAttempt();
    bActive = bNewActive;
}

FMistAlphonseAbilityDefinition UMistAnimaCombat::GetDefinition(const FKey& Key, bool bFlamestealer) const
{
    return FMistAnimaAbilityDefinitions::Get(Key, bFlamestealer);
}

bool UMistAnimaCombat::IsHostileTarget(AActor* Target, bool bRequireDamageable) const
{
    const UMistCombatHealth* Health = HealthOf(Target);
    return IsValid(Target) && Target != GetOwner() && Health && Health->IsAlive()
        && (!bRequireDamageable || !Health->IsInvulnerable());
}

bool UMistAnimaCombat::IsMinion(AActor* Target) const
{
    const UMistCombatHealth* Health = HealthOf(Target);
    return Health && Health->bIsMinion;
}

void UMistAnimaCombat::SetCooldown(const FKey& Key, float Seconds)
{
    if (!Controller.IsValid()) return;
    const double Ready = Now() + Controller->GetCooldownDuration(Seconds);
    if (Key == EKeys::Q) Controller->QReadyAt = Ready;
    else if (Key == EKeys::W) Controller->WReadyAt = Ready;
    else if (Key == EKeys::E) Controller->EReadyAt = Ready;
    else if (Key == EKeys::R) Controller->RReadyAt = Ready;
}

void UMistAnimaCombat::CommitAbility(const FKey& Key, const FVector& Direction)
{
    if (!bActive || !Controller.IsValid() || !HeroHealth.IsValid() || !Controller->CanUseAbility(Key)) return;
    const bool bAlternate = Controller->IsAbyssEquipped(Key);
    const FMistAlphonseAbilityDefinition Def = GetDefinition(Key, bAlternate);
    if (Key == EKeys::W && DashCommitments > 0 && !CanRecast(Key)) return;
    if ((Key == EKeys::Q || Key == EKeys::W || (Key == EKeys::R && !bAlternate)) && Direction.IsNearlyZero()) return;
    if (Key == EKeys::R && !bAlternate && HeroHealth->IsRooted()) return;
    FVector Endpoint = GetOwner()->GetActorLocation();
    if ((Key == EKeys::W || (Key == EKeys::R && !bAlternate))
        && (!Controller->FindLegalMovementEndpoint(Direction, Def.Range, Endpoint)
            || FVector::Dist2D(GetOwner()->GetActorLocation(), Endpoint) <= 1.f)) return;
    // Controller has already checked initial cooldown legality. Recasts remain
    // usable only within the component's own stage window and minimum interval.
    if (Key != EKeys::Q && Key != EKeys::W && Key != EKeys::E && Key != EKeys::R) return;
    Controller->ClearAim();
    if (Key == EKeys::E)
    {
        SetCooldown(Key, Def.Cooldown);
        bFlameStance = bAlternate; StanceUntil = Now() + 1.0;
        HeroHealth->SetDamageReduction(RiposteSource, Def.ReductionFraction, Def.Duration);
        if (UMistAnimaPresentation* Visual = PresentationOf(GetOwner())) Visual->PlayGuard(1.f);
        return;
    }
    Controller->AttackTarget.Reset();
    Controller->StopForCommittedAttack();
    HeroHealth->EndSpawnProtection();
    Controller->bCastingCombat = true;
    PendingDefinition = Def; bPendingAlternate = bAlternate;
    PendingDamageMultiplier = HeroHealth->AbilityDamageMultiplier;
    CastDirection = Direction.IsNearlyZero() ? GetOwner()->GetActorForwardVector().GetSafeNormal2D() : Direction.GetSafeNormal2D();
    GetOwner()->SetActorRotation(CastDirection.Rotation());
    DrawPreview(Key, CastDirection, FColor(255, 180, 60), Def.CastTime);
    UMistAnimaPresentation* Visual = PresentationOf(GetOwner());
    if (Key == EKeys::Q)
    {
        SetCooldown(Key, Def.Cooldown);
        if (Visual) Visual->PlayThrust(Def.CastTime);
        GetWorld()->GetTimerManager().SetTimer(CastTimer, this, &UMistAnimaCombat::ReleaseThrust, Def.CastTime);
    }
    else if (Key == EKeys::W || (Key == EKeys::R && !bAlternate))
    {
        bUltimateDash = Key == EKeys::R;
        bFlameDash = !bUltimateDash && bAlternate;
        DashEndpoint = Endpoint;
        DashHits.Reset(); bDashHealUsed = false;
        if (bUltimateDash) SetCooldown(Key, Def.Cooldown);
        else
        {
            ++DashCommitments;
            NextDashCommitAt = Now() + 0.20;
            DashRecastUntil = Now() + 5.0;
            if (DashCommitments >= 3) FinishDashSequence();
        }
        if (Visual) Visual->PlayDash(Def.CastTime + Def.Range / Def.TravelSpeed);
        GetWorld()->GetTimerManager().SetTimer(CastTimer, this, &UMistAnimaCombat::BeginDash, Def.CastTime);
    }
    else if (Key == EKeys::R)
    {
        SetCooldown(Key, Def.Cooldown);
        if (Visual) Visual->PlayUltimate(Def.CastTime);
        GetWorld()->GetTimerManager().SetTimer(CastTimer, this, &UMistAnimaCombat::ReleaseAura, Def.CastTime);
    }
}

void UMistAnimaCombat::FinishCast()
{
    if (Controller.IsValid()) Controller->bCastingCombat = false;
}

AActor* UMistAnimaCombat::FindFirstThrustHit(const FVector& Direction, float Range, float Radius) const
{
    if (!Controller.IsValid()) return nullptr;
    AActor* First = nullptr;
    float FirstEntry = TNumericLimits<float>::Max();
    const FVector From = GetOwner()->GetActorLocation();
    const FVector To = From + Direction * Range;
    for (TActorIterator<AMistTrainingDummy> It(GetWorld()); It; ++It)
    {
        if (!IsHostileTarget(*It, false) || !Controller->OverlapsSweptCapsule(*It, From, To, Radius)
            || !Controller->HasCombatLineOfSight(*It)) continue;
        const FVector Delta = It->GetActorLocation() - From;
        const float Along = FVector::DotProduct(Delta, Direction);
        const float CombinedRadius = Radius + It->Capsule->GetScaledCapsuleRadius();
        const float SideSquared = FMath::Max(0.f, Delta.SizeSquared2D() - Along * Along);
        const float Entry = FMath::Max(0.f, Along - FMath::Sqrt(FMath::Max(0.f, CombinedRadius * CombinedRadius - SideSquared)));
        if (Entry < FirstEntry || (FMath::IsNearlyEqual(Entry, FirstEntry) && It->GetUniqueID() < (First ? First->GetUniqueID() : MAX_uint32)))
        { First = *It; FirstEntry = Entry; }
    }
    return First;
}

void UMistAnimaCombat::ApplyMark(AActor* Target)
{
    if (!CanApplyAnimaStatus(Target)) return;
    MarkTarget = Target; MarkUntil = Now() + 4.0;
}

void UMistAnimaCombat::ApplyBrand(AActor* Target)
{
    if (!CanApplyAnimaStatus(Target)) return;
    BrandTarget = Target; BrandUntil = Now() + 4.0;
}

void UMistAnimaCombat::AddBurn(AActor* Target, int32 Count)
{
    if (!CanApplyAnimaStatus(Target)) return;
    if (UMistAnimaBurnStatus* Status = UMistAnimaBurnStatus::FindOrCreate(Target)) Status->AddStacks(this, Count);
}

int32 UMistAnimaCombat::GetBurnCount(AActor* Target) const
{
    const UMistAnimaBurnStatus* Status = IsValid(Target) ? Target->FindComponentByClass<UMistAnimaBurnStatus>() : nullptr;
    return Status ? Status->CountStacks(this) : 0;
}

AActor* UMistAnimaCombat::GetMarkedTarget() const { return MarkUntil > Now() ? MarkTarget.Get() : nullptr; }
AActor* UMistAnimaCombat::GetBrandedTarget() const { return BrandUntil > Now() ? BrandTarget.Get() : nullptr; }

float UMistAnimaCombat::DealPhysical(AActor* Target, float RawDamage, bool bCounter, EMistDamageKind Kind)
{
    UMistCombatHealth* Health = HealthOf(Target);
    if (!Health) return 0.f;
    const float Actual = Health->ReceivePhysicalDamage(RawDamage, GetOwner(), Kind);
    CreditDamage(Target, Actual, false, bCounter);
    return Actual;
}

void UMistAnimaCombat::CreditDamage(AActor* Target, float ActualHealthDamage, bool bBurn, bool bCounter)
{
    const UMistCombatHealth* TargetHealth = HealthOf(Target);
    if (!HeroHealth.IsValid() || !HeroHealth->IsAlive() || !TargetHealth || TargetHealth->bIsStructure || ActualHealthDamage <= 0.f) return;
    float Fraction = 0.f;
    if (bCounter) Fraction = GetMarkedTarget() == Target ? 0.30f : 0.15f;
    else if (GetMarkedTarget() == Target) Fraction = 0.15f;
    if (bBurn && AuraUntil > Now() && !IsMinion(Target)) Fraction += 1.f;
    if (Fraction > 0.f) HeroHealth->Heal(ActualHealthDamage * Fraction, HeroHealth.Get());
}

void UMistAnimaCombat::ReleaseThrust()
{
    FinishCast();
    if (!bActive || !HeroHealth.IsValid() || !HeroHealth->IsAlive() || HeroHealth->IsStunned()) return;
    if (AActor* Target = FindFirstThrustHit(CastDirection, PendingDefinition.Range, PendingDefinition.Radius))
    {
        const TWeakObjectPtr<AActor> PreviousMark = MarkTarget;
        const TWeakObjectPtr<AActor> PreviousBrand = BrandTarget;
        const double PreviousMarkUntil = MarkUntil, PreviousBrandUntil = BrandUntil;
        if (bPendingAlternate) ApplyBrand(Target); else ApplyMark(Target);
        DealPhysical(Target, PendingDefinition.DirectDamage * PendingDamageMultiplier);
        if (HealthOf(Target)->WasLastDamageBlocked())
        {
            MarkTarget = PreviousMark; MarkUntil = PreviousMarkUntil;
            BrandTarget = PreviousBrand; BrandUntil = PreviousBrandUntil;
        }
    }
    DrawPreview(EKeys::Q, CastDirection, bPendingAlternate ? FColor::Orange : FColor(100, 220, 255), 0.12f);
}

void UMistAnimaCombat::BeginDash()
{
    if (!bActive || !HeroHealth.IsValid() || !HeroHealth->IsAlive() || HeroHealth->IsStunned() || HeroHealth->IsRooted())
    { FinishDash(); FinishDashSequence(); return; }
    if (ACharacter* Character = Cast<ACharacter>(GetOwner()))
    {
        OriginalPawnResponse = Character->GetCapsuleComponent()->GetCollisionResponseToChannel(ECC_Pawn);
        Character->GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
    }
    bActiveDash = true;
}

void UMistAnimaCombat::UpdateDash(float DeltaTime)
{
    if (!Controller.IsValid() || DeltaTime <= 0.f) return;
    if (!HeroHealth.IsValid() || !HeroHealth->IsAlive() || HeroHealth->IsRooted() || HeroHealth->IsStunned())
    { FinishDash(); FinishDashSequence(); return; }
    const FVector From = GetOwner()->GetActorLocation();
    const float Remaining = FVector::Dist2D(From, DashEndpoint);
    if (Remaining <= 1.f) { FinishDash(); return; }
    const float Step = FMath::Min(PendingDefinition.TravelSpeed * DeltaTime, Remaining);
    FVector To;
    if (!Controller->FindLegalMovementEndpoint(CastDirection, Step, To)) { FinishDash(); return; }
    // Ultimate stops at the first body contact, including targets protected by an
    // encounter seal. Protection can negate the impact, never permit tunnelling.
    if (bUltimateDash)
    {
        AActor* First = nullptr;
        float EntryDistance = Step + 1.f;
        for (TActorIterator<AMistTrainingDummy> It(GetWorld()); It; ++It)
        {
            if (!IsHostileTarget(*It, false) || !Controller->OverlapsSweptCapsule(*It, From, To, PendingDefinition.Radius)
                || !Controller->HasCombatLineOfSight(*It)) continue;
            const FVector Delta = It->GetActorLocation() - From;
            const float Along = FVector::DotProduct(Delta, CastDirection);
            const float CombinedRadius = PendingDefinition.Radius + It->Capsule->GetScaledCapsuleRadius();
            const float SideSquared = FMath::Max(0.f, Delta.SizeSquared2D() - Along * Along);
            const float Entry = FMath::Max(0.f, Along - FMath::Sqrt(FMath::Max(0.f, FMath::Square(CombinedRadius) - SideSquared)));
            if (Entry < EntryDistance || (FMath::IsNearlyEqual(Entry, EntryDistance) && It->GetUniqueID() < (First ? First->GetUniqueID() : MAX_uint32)))
            { First = *It; EntryDistance = Entry; }
        }
        if (First)
        {
            FVector Contact = From + CastDirection * FMath::Min(Step, EntryDistance);
            Contact.Z = FMath::Lerp(From.Z, To.Z, Step > 0.f ? EntryDistance / Step : 0.f);
            GetOwner()->SetActorLocation(Contact, false);
            const TWeakObjectPtr<AActor> PreviousMark = MarkTarget;
            const double PreviousMarkUntil = MarkUntil;
            ApplyMark(First);
            DealPhysical(First, PendingDefinition.DirectDamage * PendingDamageMultiplier);
            if (HealthOf(First)->WasLastDamageBlocked()) { MarkTarget = PreviousMark; MarkUntil = PreviousMarkUntil; }
            if (!First->IsA<AMistCryptGolem>() && !HealthOf(First)->WasLastDamageBlocked())
                if (UMistCombatHealth* TargetHealth = HealthOf(First); TargetHealth && !TargetHealth->IsInvulnerable())
                {
                    TargetHealth->SetSilence(2.f);
                    if (TargetHealth->SetHealingTheft(HeroHealth.Get(), 7.f)) TheftTargets.AddUnique(First);
                }
            FinishDash();
            return;
        }
    }
    GetOwner()->SetActorLocation(To, false);
    if (ACharacter* Character = Cast<ACharacter>(GetOwner())) Character->GetCharacterMovement()->StopMovementImmediately();
    if (!bUltimateDash) ApplyDashHits(From, To);
    DrawDebugLine(GetWorld(), From - FVector(0.f, 0.f, 75.f), To - FVector(0.f, 0.f, 75.f),
        bFlameDash ? FColor::Orange : FColor(130, 225, 255), false, 0.25f, 0, 3.f);
    if (FVector::Dist2D(From, To) < Step - 1.f || FVector::Dist2D(To, DashEndpoint) <= 1.f) FinishDash();
}

void UMistAnimaCombat::ApplyDashHits(const FVector& From, const FVector& To)
{
    TArray<AMistTrainingDummy*> Contacts;
    for (TActorIterator<AMistTrainingDummy> It(GetWorld()); It; ++It)
        if (!DashHits.Contains(*It) && IsHostileTarget(*It) && Controller->OverlapsSweptCapsule(*It, From, To, PendingDefinition.Radius)
            && Controller->HasCombatLineOfSight(*It)) Contacts.Add(*It);
    Contacts.Sort([&From, this](const AMistTrainingDummy& Left, const AMistTrainingDummy& Right)
    {
        const float LeftAlong = FVector::DotProduct(Left.GetActorLocation() - From, CastDirection);
        const float RightAlong = FVector::DotProduct(Right.GetActorLocation() - From, CastDirection);
        return FMath::IsNearlyEqual(LeftAlong, RightAlong) ? Left.GetUniqueID() < Right.GetUniqueID() : LeftAlong < RightAlong;
    });
    for (AMistTrainingDummy* Target : Contacts)
    {
        DashHits.Add(Target);
        const int32 PreviousBurns = GetBurnCount(Target);
        const bool bEligibleFlameHeal = !IsMinion(Target) && !Target->CombatHealth->bIsStructure;
        DealPhysical(Target, PendingDefinition.DirectDamage * PendingDamageMultiplier);
        if (Target->CombatHealth->WasLastDamageBlocked()) continue;
        if (bFlameDash) AddBurn(Target, 2);
        if (!bDashHealUsed && HeroHealth.IsValid() && HeroHealth->IsAlive())
        {
            if (bFlameDash && bEligibleFlameHeal)
            {
                bDashHealUsed = true;
                HeroHealth->Heal(HeroHealth->MaxHealth * 0.005f * FMath::Min(10, PreviousBurns), HeroHealth.Get());
            }
            else if (!bFlameDash && !Target->CombatHealth->bIsStructure)
            {
                bDashHealUsed = true;
                HeroHealth->Heal(HeroHealth->MaxHealth * 0.05f, HeroHealth.Get());
            }
        }
    }
}

void UMistAnimaCombat::FinishDash()
{
    if (bActiveDash)
        if (ACharacter* Character = Cast<ACharacter>(GetOwner()))
            Character->GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_Pawn, OriginalPawnResponse);
    bActiveDash = false;
    FinishCast();
    if (Controller.IsValid()) Controller->StopForCommittedAttack();
}

void UMistAnimaCombat::FinishDashSequence()
{
    if (DashCommitments > 0) SetCooldown(EKeys::W, 12.f);
    DashCommitments = 0; DashRecastUntil = 0.0; NextDashCommitAt = 0.0;
}

void UMistAnimaCombat::ReleaseAura()
{
    FinishCast();
    if (!bActive || !HeroHealth.IsValid() || !HeroHealth->IsAlive() || HeroHealth->IsStunned()) return;
    AuraUntil = Now() + 8.0; NextAuraTick = Now() + 1.0;
}

void UMistAnimaCombat::BeginAutoSwing(const FVector& Direction, float Windup)
{
    if (!bActive || !HeroHealth.IsValid() || !HeroHealth->IsAlive()) return;
    AutoParryFrom = Now() + FMath::Max(0.f, Windup - 0.075f);
    AutoParryUntil = AutoParryFrom + 0.15;
    AutoParryDirection = Direction.GetSafeNormal2D();
}

float UMistAnimaCombat::ApplyBasicAttackDamage(AActor* Target)
{
    if (!bActive || !HeroHealth.IsValid() || !HeroHealth->IsAlive() || !IsHostileTarget(Target)
        || !Controller.IsValid() || !Controller->HasCombatLineOfSight(Target)
        || !Controller->OverlapsSector(Target, AutoParryDirection, 200.f, 25.f)) return 0.f;
    const bool bEnhanced = EnhancedAutosUntil > Now();
    const float Actual = DealPhysical(Target, 70.f * (bEnhanced ? 1.20f : 1.f), false, EMistDamageKind::BasicAttack);
    // Shield absorption is still a successful attack. Immunity is excluded by
    // target eligibility, misses never brand/burn, and DOT cannot recurse here.
    if (!HealthOf(Target)->bIsStructure && !HealthOf(Target)->WasLastDamageBlocked())
    {
        const bool bBrand = GetBrandedTarget() == Target;
        const int32 BurnCount = bEnhanced ? (bBrand ? 2 : 1) : (bBrand ? 1 : 0);
        if (BurnCount > 0) AddBurn(Target, BurnCount);
    }
    return Actual;
}

bool UMistAnimaCombat::InterceptDamage(float Mitigated, AActor* Source, EMistDamageKind Kind)
{
    if (!bActive || !HeroHealth.IsValid() || !HeroHealth->IsAlive() || !IsValid(Source)
        || Source == GetOwner() || Mitigated <= 0.f) return false;
    if (StanceUntil > Now() && bFlameStance)
    {
        StanceUntil = 0.0;
        HeroHealth->SetDamageReduction(RiposteSource, 0.f, 0.f);
        EnhancedAutosUntil = Now() + 5.0;
        DrawDebugSphere(GetWorld(), GetOwner()->GetActorLocation(), 65.f, 16, FColor::Orange, false, 0.15f);
        return true; // E wins; passive cooldown remains untouched.
    }
    if (Kind == EMistDamageKind::BasicAttack && Now() >= AutoParryFrom && Now() < AutoParryUntil
        && Now() >= AutoParryReadyAt && Controller.IsValid()
        && Controller->OverlapsSector(Source, AutoParryDirection, 200.f, 25.f) && Controller->HasCombatLineOfSight(Source))
    {
        AutoParryReadyAt = Now() + 3.0;
        HeroHealth->Heal(Mitigated, HeroHealth.Get());
        DrawDebugLine(GetWorld(), GetOwner()->GetActorLocation(), Source->GetActorLocation(), FColor(170, 245, 255), false, 0.2f, 0, 4.f);
        return true;
    }
    return false;
}

void UMistAnimaCombat::ReceivedDamage(float ActualHP, float ShieldLoss, AActor* Source, EMistDamageKind Kind)
{
    if (!bActive || bFlameStance || StanceUntil <= Now() || Kind == EMistDamageKind::DamageOverTime
        || ActualHP + ShieldLoss <= 0.f || !IsValid(Source) || Source == GetOwner()
        || !HeroHealth.IsValid() || !HeroHealth->IsAlive() || !Controller.IsValid()) return;
    StanceUntil = 0.0;
    HeroHealth->SetDamageReduction(RiposteSource, 0.f, 0.f);
    // A triggered counter replaces a pending commitment. Spent cooldowns stay
    // spent, and W's staged cooldown is finalized if its sequence is interrupted.
    GetWorld()->GetTimerManager().ClearTimer(CastTimer);
    GetWorld()->GetTimerManager().ClearTimer(Controller->CombatTimer);
    if (bActiveDash) FinishDash();
    FinishDashSequence();
    Controller->AttackTarget.Reset();
    Controller->ClearAim();
    Controller->StopForCommittedAttack();
    Controller->bCastingCombat = true;
    HeroHealth->EndSpawnProtection();
    CastDirection = (Source->GetActorLocation() - GetOwner()->GetActorLocation()).GetSafeNormal2D();
    GetOwner()->SetActorRotation(CastDirection.Rotation());
    PendingDamageMultiplier = HeroHealth->AbilityDamageMultiplier;
    if (UMistAnimaPresentation* Visual = PresentationOf(GetOwner())) Visual->PlayThrust(0.10f);
    GetWorld()->GetTimerManager().SetTimer(CastTimer, this, &UMistAnimaCombat::ReleaseCounter, 0.10f);
}

void UMistAnimaCombat::ReleaseCounter()
{
    FinishCast();
    if (!bActive || !HeroHealth.IsValid() || !HeroHealth->IsAlive() || HeroHealth->IsStunned()) return;
    if (AActor* Target = FindFirstThrustHit(CastDirection, 300.f, 30.f))
        DealPhysical(Target, 110.f * PendingDamageMultiplier, true);
    Controller->DrawLinePreview(CastDirection, 300.f, 30.f, FColor(150, 235, 255), 0.12f);
}

bool UMistAnimaCombat::CanRecast(const FKey& Key) const
{
    return Key == EKeys::W && DashCommitments > 0 && DashCommitments < 3
        && DashRecastUntil > Now() && NextDashCommitAt <= Now() && !bActiveDash;
}

float UMistAnimaCombat::GetRecastRemaining(const FKey& Key) const
{
    return Key == EKeys::W && DashCommitments > 0 ? FMath::Max(0.f, static_cast<float>(DashRecastUntil - Now())) : 0.f;
}

int32 UMistAnimaCombat::GetDashChargesRemaining() const { return DashCommitments > 0 ? 3 - DashCommitments : 3; }
float UMistAnimaCombat::GetDefenseRemaining() const { return FMath::Max(0.f, static_cast<float>(StanceUntil - Now())); }

FString UMistAnimaCombat::GetStatusSummary() const
{
    if (!bActive) return FString();
    TArray<FString> Parts;
    if (StanceUntil > Now()) Parts.Add(bFlameStance ? TEXT("Flame parry ready") : TEXT("Riposte ready"));
    if (EnhancedAutosUntil > Now()) Parts.Add(FString::Printf(TEXT("Flame attacks %.1fs"), EnhancedAutosUntil - Now()));
    if (AuraUntil > Now()) Parts.Add(FString::Printf(TEXT("Inflamed %.1fs"), AuraUntil - Now()));
    if (DashCommitments > 0) Parts.Add(FString::Printf(TEXT("W: %d steps / %.1fs"), 3 - DashCommitments, GetRecastRemaining(EKeys::W)));
    if (GetMarkedTarget()) Parts.Add(TEXT("Mark active"));
    if (GetBrandedTarget()) Parts.Add(TEXT("Brand active"));
    return FString::Join(Parts, TEXT(" | "));
}

FString UMistAnimaCombat::GetActionLockReason() const
{
    if (!bActive) return FString();
    if (bActiveDash || (Controller.IsValid() && Controller->bCastingCombat)) return TEXT("Finish Anima's committed action");
    if (DashCommitments > 0) return TEXT("Finish or let Anima's W recasts expire");
    if (StanceUntil > Now() || EnhancedAutosUntil > Now() || AuraUntil > Now()) return TEXT("Wait for Anima's active stance or aura to end");
    const UMistCombatHealth* MarkHealth = HealthOf(GetMarkedTarget());
    const UMistCombatHealth* BrandHealth = HealthOf(GetBrandedTarget());
    if ((MarkHealth && MarkHealth->IsAlive()) || (BrandHealth && BrandHealth->IsAlive())) return TEXT("Wait for Anima's Mark or Brand to expire");
    for (const TWeakObjectPtr<AActor>& Target : TheftTargets)
        if (const UMistCombatHealth* Health = HealthOf(Target.Get()); Health && Health->GetHealingTheftRemaining(HeroHealth.Get()) > 0.f)
            return TEXT("Wait for Anima's healing theft to end");
    for (TActorIterator<AActor> It(GetWorld()); It; ++It)
        if (const UMistAnimaBurnStatus* Burns = It->FindComponentByClass<UMistAnimaBurnStatus>(); Burns && Burns->GetRemaining(this) > 0.f)
            if (const UMistCombatHealth* Health = HealthOf(*It); Health && Health->IsAlive()) return TEXT("Wait for Anima's burn effects to end");
    return FString();
}

void UMistAnimaCombat::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* Function)
{
    Super::TickComponent(DeltaTime, TickType, Function);
    if (!bActive || !GetOwner()->HasAuthority()) return;
    if (!HeroHealth.IsValid() || !HeroHealth->IsAlive()) { ResetAttempt(); return; }
    if (bActiveDash) UpdateDash(DeltaTime);
    if (DashCommitments > 0 && DashRecastUntil <= Now()) FinishDashSequence();
    if (StanceUntil > 0.0 && StanceUntil <= Now())
    {
        StanceUntil = 0.0;
        HeroHealth->SetDamageReduction(RiposteSource, 0.f, 0.f);
    }
    // Include the eighth whole-second pulse at the exact aura end. Ticks on the
    // newly applied stacks remain delayed by a full second, after aura healing ends.
    while (NextAuraTick > 0.0 && NextAuraTick <= Now() + KINDA_SMALL_NUMBER && NextAuraTick <= AuraUntil + KINDA_SMALL_NUMBER)
    {
        for (TActorIterator<AMistTrainingDummy> It(GetWorld()); It; ++It)
        {
            const FVector Delta = It->GetActorLocation() - GetOwner()->GetActorLocation();
            if (IsHostileTarget(*It) && FMath::Abs(Delta.Z) <= 160.f
                && Delta.SizeSquared2D() <= FMath::Square(300.f + It->Capsule->GetScaledCapsuleRadius())
                && Controller.IsValid() && Controller->HasCombatLineOfSight(*It)) AddBurn(*It, 2);
        }
        NextAuraTick += 1.0;
    }
    if (AuraUntil <= Now()) NextAuraTick = 0.0;
    TheftTargets.RemoveAll([this](const TWeakObjectPtr<AActor>& Target)
    { const UMistCombatHealth* Health = HealthOf(Target.Get()); return !Health || Health->GetHealingTheftRemaining(HeroHealth.Get()) <= 0.f; });
    DrawStatuses();
}

void UMistAnimaCombat::DrawStatuses() const
{
    const FVector Origin = GetOwner()->GetActorLocation() - FVector(0.f, 0.f, 85.f);
    if (AuraUntil > Now())
        DrawDebugCircle(GetWorld(), Origin, 300.f, 48, FColor(255, 115, 30), false, 0.f, 0, 3.f,
            FVector::ForwardVector, FVector::RightVector, false);
    if (AActor* Mark = GetMarkedTarget())
        DrawDebugCircle(GetWorld(), Mark->GetActorLocation() - FVector(0.f, 0.f, 81.f), 70.f, 24,
            FColor(90, 225, 255), false, 0.f, 0, 2.f, FVector::ForwardVector, FVector::RightVector, false);
    if (AActor* Brand = GetBrandedTarget())
        DrawDebugCircle(GetWorld(), Brand->GetActorLocation() - FVector(0.f, 0.f, 79.f), 78.f, 12,
            FColor(255, 150, 25), false, 0.f, 0, 2.f, FVector::ForwardVector, FVector::RightVector, false);
}

void UMistAnimaCombat::DrawPreview(const FKey& Key, const FVector& Direction, const FColor& Color, float Lifetime) const
{
    if (!Controller.IsValid() || !GetOwner()) return;
    const bool bAlternate = Controller->IsAbyssEquipped(Key);
    const FMistAlphonseAbilityDefinition Def = GetDefinition(Key, bAlternate);
    const FVector Origin = GetOwner()->GetActorLocation() - FVector(0.f, 0.f, 85.f);
    if (Key == EKeys::E || (Key == EKeys::R && bAlternate))
        DrawDebugCircle(GetWorld(), Origin, Key == EKeys::E ? 75.f : Def.Radius, 40, Color,
            false, Lifetime, 0, 2.f, FVector::ForwardVector, FVector::RightVector, false);
    else if (Key == EKeys::Q) Controller->DrawLinePreview(Direction, Def.Range, Def.Radius, Color, Lifetime);
    else
    {
        FVector Endpoint = GetOwner()->GetActorLocation();
        Controller->FindLegalMovementEndpoint(Direction, Def.Range, Endpoint);
        Controller->DrawLinePreview(Direction, static_cast<float>(FVector::Dist2D(GetOwner()->GetActorLocation(), Endpoint)), Def.Radius, Color, Lifetime);
        DrawDebugCircle(GetWorld(), Endpoint - FVector(0.f, 0.f, 85.f), 40.f, 24, Color,
            false, Lifetime, 0, 2.f, FVector::ForwardVector, FVector::RightVector, false);
    }
}

void UMistAnimaCombat::ResetAttempt()
{
    if (GetWorld()) GetWorld()->GetTimerManager().ClearTimer(CastTimer);
    if (bActiveDash) FinishDash();
    FinishDashSequence(); // Expiry/interruption starts W's own cooldown; root reset may deliberately clear it afterwards.
    MarkTarget.Reset(); BrandTarget.Reset(); MarkUntil = BrandUntil = 0.0;
    StanceUntil = EnhancedAutosUntil = AuraUntil = NextAuraTick = 0.0;
    AutoParryFrom = AutoParryUntil = 0.0;
    DashHits.Reset();
    if (HeroHealth.IsValid()) HeroHealth->SetDamageReduction(RiposteSource, 0.f, 0.f);
    for (const TWeakObjectPtr<AActor>& Target : TheftTargets)
        if (HeroHealth.IsValid())
            if (UMistCombatHealth* Health = HealthOf(Target.Get())) Health->ClearHealingTheft(HeroHealth.Get());
    TheftTargets.Reset();
    if (GetWorld())
        for (TActorIterator<AActor> It(GetWorld()); It; ++It)
            if (UMistAnimaBurnStatus* Burns = It->FindComponentByClass<UMistAnimaBurnStatus>()) Burns->RemoveSource(this);
    if (bActive) FinishCast();
    if (UMistAnimaPresentation* Visual = PresentationOf(GetOwner())) Visual->ResetPresentation();
}

void UMistAnimaCombat::EndPlay(const EEndPlayReason::Type Reason)
{
    ResetAttempt();
    if (HeroHealth.IsValid())
    {
        HeroHealth->DamageInterceptor.Unbind();
        HeroHealth->OnDamageReceived.RemoveAll(this);
    }
    Super::EndPlay(Reason);
}

