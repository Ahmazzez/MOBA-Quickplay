#include "MistAnimaPresentation.h"

#include "MistCombatHealth.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/Character.h"

UMistAnimaPresentation::UMistAnimaPresentation()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.TickGroup = TG_PrePhysics;
}

void UMistAnimaPresentation::BeginPlay()
{
    Super::BeginPlay();
    Initialize();
}

UAnimSequence* UMistAnimaPresentation::LoadClip(const TCHAR* Name) const
{
    const FString Path = FString::Printf(
        TEXT("/Game/Mistworld/Characters/AnimaAnime/Animations/A_Anima_%s.A_Anima_%s"), Name, Name);
    return LoadObject<UAnimSequence>(nullptr, *Path);
}

void UMistAnimaPresentation::Initialize()
{
    ACharacter* Character = Cast<ACharacter>(GetOwner());
    Body = Character ? Character->GetMesh() : nullptr;
    if (!Body) return;
    if (!bCapturedOriginal)
    {
        OriginalMesh = Body->GetSkeletalMeshAsset();
        OriginalAnimClass = Body->GetAnimClass();
        OriginalAnimationMode = static_cast<uint8>(Body->GetAnimationMode());
        OriginalTransform = Body->GetRelativeTransform();
        OriginalMaterials.Reset();
        for (int32 Slot = 0; Slot < Body->GetNumMaterials(); ++Slot)
            OriginalMaterials.Add(Body->GetMaterial(Slot));
        bCapturedOriginal = true;
    }
    if (!AnimaMesh)
    {
        AnimaMesh = LoadObject<USkeletalMesh>(nullptr,
            TEXT("/Game/Mistworld/Characters/AnimaAnime/SK_Anima.SK_Anima"));
        Idle = LoadClip(TEXT("Idle"));
        Run = LoadClip(TEXT("Run"));
        Death = LoadClip(TEXT("Death"));
    }
}

bool UMistAnimaPresentation::SetHeroActive(const bool bWanted)
{
    if (bActive == bWanted) return true;
    Initialize();
    if (!Body || (bWanted && !AnimaMesh)) return false;
    Body->Stop();
    if (UAnimInstance* Instance = Body->GetAnimInstance()) Instance->Montage_Stop(0.f);
    CurrentSequence = CurrentAction = nullptr;
    ActionRemaining = 0.f;
    bWasDead = false;
    Body->EmptyOverrideMaterials();
    bActive = bWanted;
    if (bActive)
    {
        Body->SetSkeletalMesh(AnimaMesh, true);
        Body->SetAnimationMode(EAnimationMode::AnimationSingleNode);
        const ACharacter* Character = CastChecked<ACharacter>(GetOwner());
        Body->SetRelativeLocation(FVector(0.f, 0.f, -Character->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight()));
        Body->SetRelativeRotation(FRotator::ZeroRotator);
        Body->SetRelativeScale3D(FVector::OneVector);
        SelectSequence(Idle, true);
    }
    else
    {
        Body->SetSkeletalMesh(OriginalMesh, true);
        Body->SetRelativeTransform(OriginalTransform);
        for (int32 Slot = 0; Slot < OriginalMaterials.Num(); ++Slot)
            Body->SetMaterial(Slot, OriginalMaterials[Slot]);
        Body->SetAnimationMode(static_cast<EAnimationMode::Type>(OriginalAnimationMode));
        if (OriginalAnimClass) Body->SetAnimInstanceClass(OriginalAnimClass);
    }
    return true;
}

void UMistAnimaPresentation::SelectSequence(UAnimSequence* Sequence, const bool bLoop, const float PlayRate)
{
    if (!Body || !Sequence) return;
    if (CurrentSequence != Sequence)
    {
        Body->PlayAnimation(Sequence, bLoop);
        CurrentSequence = Sequence;
    }
    if (UAnimSingleNodeInstance* Instance = Body->GetSingleNodeInstance())
    {
        Instance->SetLooping(bLoop);
        Instance->SetPlayRate(PlayRate);
    }
}

void UMistAnimaPresentation::PlayAction(const TCHAR* Name, const float Duration)
{
    if (!bActive || !Body) return;
    CurrentAction = LoadClip(Name);
    if (!CurrentAction) return;
    ActionRemaining = FMath::Max(0.05f, Duration);
    CurrentSequence = nullptr;
    SelectSequence(CurrentAction, false, CurrentAction->GetPlayLength() / ActionRemaining);
}

void UMistAnimaPresentation::PlayAttack(const float Duration) { PlayAction(TEXT("Attack"), Duration); }
void UMistAnimaPresentation::PlayThrust(const float Duration) { PlayAction(TEXT("Thrust"), Duration); }
void UMistAnimaPresentation::PlayDash(const float Duration) { PlayAction(TEXT("Dash"), Duration); }
void UMistAnimaPresentation::PlayGuard(const float Duration) { PlayAction(TEXT("Guard"), Duration); }
void UMistAnimaPresentation::PlayUltimate(const float Duration) { PlayAction(TEXT("Ultimate"), Duration); }

void UMistAnimaPresentation::ResetPresentation()
{
    CurrentAction = nullptr;
    CurrentSequence = nullptr;
    ActionRemaining = 0.f;
    bWasDead = false;
    if (bActive) SelectSequence(Idle, true);
}

void UMistAnimaPresentation::TickComponent(const float DeltaTime, const ELevelTick TickType,
    FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    if (!bActive || !Body) return;
    const UMistCombatHealth* Health = GetOwner()->FindComponentByClass<UMistCombatHealth>();
    const bool bDead = Health && !Health->IsAlive();
    if (bDead)
    {
        if (!bWasDead) CurrentSequence = nullptr;
        CurrentAction = nullptr;
        ActionRemaining = 0.f;
        SelectSequence(Death, false);
    }
    else
    {
        if (bWasDead) ResetPresentation();
        ActionRemaining = FMath::Max(0.f, ActionRemaining - DeltaTime);
        if (ActionRemaining <= 0.f)
        {
            CurrentAction = nullptr;
            const float Speed = GetOwner()->GetVelocity().Size2D();
            SelectSequence(Speed > 1.f ? Run.Get() : Idle.Get(), true,
                Speed > 1.f ? FMath::Clamp(Speed / 500.f, 0.5f, 1.8f) : 1.f);
        }
    }
    bWasDead = bDead;
}

