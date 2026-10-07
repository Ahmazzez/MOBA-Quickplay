#include "MistAlphonseAnimInstance.h"
#include "MistCombatHealth.h"

#include "Animation/AnimInstanceProxy.h"
#include "Animation/AnimNode_SequencePlayer.h"
#include "Animation/AnimSequence.h"
#include "AnimNodes/AnimNode_LayeredBoneBlend.h"
#include "AnimNodes/AnimNode_Slot.h"
#include "AnimNodes/AnimNode_TwoWayBlend.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

namespace
{
constexpr float GuardBlendSeconds = 0.12f;
constexpr float LocomotionBlendSeconds = 0.08f;
constexpr float AuthoredRunSpeed = 500.f;
const FName CombatSlot(TEXT("DefaultSlot"));
}

struct FMistAlphonseAnimInstanceProxy final : public FAnimInstanceProxy
{
    explicit FMistAlphonseAnimInstanceProxy(UAnimInstance* Instance)
        : FAnimInstanceProxy(Instance)
    {
        Idle.SetLoopAnimation(true);
        Run.SetLoopAnimation(true);
        Guard.SetLoopAnimation(true);

        Locomotion.A.SetLinkNode(&Idle);
        Locomotion.B.SetLinkNode(&Run);

        UpperBodyGuard.BasePose.SetLinkNode(&Locomotion);
        UpperBodyGuard.AddPose();
        UpperBodyGuard.BlendPoses[0].SetLinkNode(&Guard);
        UpperBodyGuard.BlendWeights[0] = 0.f;
        FBranchFilter Filter;
        Filter.BoneName = TEXT("spine_01");
        Filter.BlendDepth = 1;
        UpperBodyGuard.LayerSetup[0].BranchFilters.Add(Filter);
        UpperBodyGuard.bMeshSpaceRotationBlend = true;
        UpperBodyGuard.bBlendRootMotionBasedOnRootBone = true;

        // Placing the slot last gives committed actions the complete body.
        // Guard only affects the spine branch; pelvis and legs keep running.
        FullBody.SlotName = CombatSlot;
        FullBody.bAlwaysUpdateSourcePose = true;
        FullBody.Source.SetLinkNode(&UpperBodyGuard);
    }

    virtual void Initialize(UAnimInstance* Instance) override
    {
        const UMistAlphonseAnimInstance* Alphonse = CastChecked<UMistAlphonseAnimInstance>(Instance);
        BindSequences(*Alphonse);
        SeenResetSerial = Alphonse->ResetSerial;
        bWasDead = false;
        LocomotionAlpha = GuardAlpha = Speed = 0.f;
        FAnimInstanceProxy::Initialize(Instance);
    }

    void BindSequences(const UMistAlphonseAnimInstance& Alphonse)
    {
        IdleClip = Alphonse.IdleSequence.Get();
        DeathClip = Alphonse.DeathSequence.Get();
        Idle.SetSequence(IdleClip);
        Run.SetSequence(Alphonse.RunSequence);
        GuardClip = Alphonse.GuardSequence.Get();
        Guard.SetSequence(GuardClip);
    }

    virtual FAnimNode_Base* GetCustomRootNode() override { return &FullBody; }

    virtual void GetCustomNodes(TArray<FAnimNode_Base*>& OutNodes) override
    {
        OutNodes.Append({&Idle, &Run, &Guard, &Locomotion, &UpperBodyGuard, &FullBody});
    }

    virtual void PreUpdate(UAnimInstance* Instance, float DeltaSeconds) override
    {
        FAnimInstanceProxy::PreUpdate(Instance, DeltaSeconds);
        check(IsInGameThread());
        const UMistAlphonseAnimInstance* Alphonse = CastChecked<UMistAlphonseAnimInstance>(Instance);
        const AActor* Owner = Alphonse->GetOwningActor();
        const float OwnerSpeed = Owner ? Owner->GetVelocity().Size2D() : 0.f;
        Speed = FMath::IsFinite(OwnerSpeed) ? FMath::Max(0.f, OwnerSpeed) : 0.f;
        GuardClip = Alphonse->bAbyssGuard ? Alphonse->AbyssGuardSequence.Get() : Alphonse->GuardSequence.Get();
        const UMistCombatHealth* Health = Owner ? Owner->FindComponentByClass<UMistCombatHealth>() : nullptr;
        bIsDead = Health && !Health->IsAlive();
        bGuardWanted = Alphonse->GetGuardRemainingTime() > 0.f && GuardClip != nullptr
            && (!Health || (Health->IsAlive() && !Health->IsStunned()));
        bFullBodyAction = Alphonse->Montage_IsActive(nullptr);
        bResetRequested = SeenResetSerial != Alphonse->ResetSerial;
        SeenResetSerial = Alphonse->ResetSerial;
    }

    virtual void Update(float DeltaSeconds) override
    {
        // This stage may run on a worker. Only the game-thread snapshot above
        // is read here; no actor, world, or mutable AnimInstance access occurs.
        const float SafeDelta = FMath::IsFinite(DeltaSeconds) ? FMath::Max(0.f, DeltaSeconds) : 0.f;
        if (bResetRequested)
        {
            LocomotionAlpha = GuardAlpha = 0.f;
            Idle.SetAccumulatedTime(0.f);
            Run.SetAccumulatedTime(0.f);
            Guard.SetAccumulatedTime(0.f);
            bResetRequested = false;
        }
        if (bIsDead != bWasDead)
        {
            Idle.SetAccumulatedTime(0.f);
            bWasDead = bIsDead;
        }
        Idle.SetSequence(bIsDead && DeathClip ? DeathClip : IdleClip);
        Idle.SetLoopAnimation(!bIsDead);
        const float RunTarget = !bIsDead && Speed > 1.f && Run.GetSequence() ? 1.f : 0.f;
        LocomotionAlpha = FMath::FInterpConstantTo(LocomotionAlpha, RunTarget, SafeDelta, 1.f / LocomotionBlendSeconds);
        Locomotion.Alpha = LocomotionAlpha;
        Run.SetPlayRate(FMath::Clamp(Speed / AuthoredRunSpeed, 0.25f, 1.6f));
        Guard.SetSequence(GuardClip);
        const float GuardTarget = bGuardWanted && !bFullBodyAction ? 1.f : 0.f;
        GuardAlpha = FMath::FInterpConstantTo(GuardAlpha, GuardTarget, SafeDelta, 1.f / GuardBlendSeconds);
        UpperBodyGuard.BlendWeights[0] = GuardAlpha;
    }

private:
    FAnimNode_SequencePlayer_Standalone Idle;
    FAnimNode_SequencePlayer_Standalone Run;
    FAnimNode_SequencePlayer_Standalone Guard;
    FAnimNode_TwoWayBlend Locomotion;
    FAnimNode_LayeredBoneBlend UpperBodyGuard;
    FAnimNode_Slot FullBody;

    // Assets remain strongly owned by the AnimInstance for the proxy lifetime.
    UAnimSequence* GuardClip = nullptr;
    UAnimSequence* IdleClip = nullptr;
    UAnimSequence* DeathClip = nullptr;
    float Speed = 0.f;
    float LocomotionAlpha = 0.f;
    float GuardAlpha = 0.f;
    uint32 SeenResetSerial = 0;
    bool bGuardWanted = false;
    bool bFullBodyAction = false;
    bool bResetRequested = false;
    bool bIsDead = false;
    bool bWasDead = false;
};

UMistAlphonseAnimInstance::UMistAlphonseAnimInstance()
{
    // The small prototype graph updates synchronously. Its evaluation is also
    // safe in the engine's parallel pose task because all UObject state was
    // copied in PreUpdate; nodes only evaluate immutable animation assets.
    bUseMultiThreadedAnimationUpdate = false;
    RootMotionMode = ERootMotionMode::IgnoreRootMotion;

    IdleAsset = FSoftObjectPath(TEXT("/Game/Mistworld/Characters/Alphonse/Animations/A_Alphonse_Idle.A_Alphonse_Idle"));
    RunAsset = FSoftObjectPath(TEXT("/Game/Mistworld/Characters/Alphonse/Animations/A_Alphonse_Run.A_Alphonse_Run"));
    GuardAsset = FSoftObjectPath(TEXT("/Game/Mistworld/Characters/Alphonse/Animations/A_Alphonse_Guard.A_Alphonse_Guard"));
    AbyssGuardAsset = FSoftObjectPath(TEXT("/Game/Mistworld/Characters/Alphonse/Animations/A_Alphonse_AbyssGuard.A_Alphonse_AbyssGuard"));
    DeathAsset = FSoftObjectPath(TEXT("/Game/Mistworld/Characters/Alphonse/Animations/A_Alphonse_Death.A_Alphonse_Death"));
}

void UMistAlphonseAnimInstance::NativeInitializeAnimation()
{
    Super::NativeInitializeAnimation();
    check(IsInGameThread());
    IdleSequence = IdleAsset.LoadSynchronous();
    RunSequence = RunAsset.LoadSynchronous();
    GuardSequence = GuardAsset.LoadSynchronous();
    AbyssGuardSequence = AbyssGuardAsset.LoadSynchronous();
    DeathSequence = DeathAsset.LoadSynchronous();
    // UE creates the proxy before this callback, then initializes its root
    // afterwards. Bind the newly loaded clips before that root initialization.
    // A missing clip remains null: the sequence player safely yields ref pose.
    GetProxyOnGameThread<FMistAlphonseAnimInstanceProxy>().BindSequences(*this);
}

void UMistAlphonseAnimInstance::SetGuard(bool bAbyss, float Duration)
{
    check(IsInGameThread());
    bAbyssGuard = bAbyss;
    const UWorld* World = GetWorld();
    GuardExpiresAt = World && FMath::IsFinite(Duration) && Duration > 0.f
        ? World->GetTimeSeconds() + FMath::Min(Duration, 60.f) : 0.0;
}

float UMistAlphonseAnimInstance::GetGuardRemainingTime() const
{
    const UWorld* World = GetWorld();
    return World ? FMath::Max(0.f, static_cast<float>(GuardExpiresAt - World->GetTimeSeconds())) : 0.f;
}

void UMistAlphonseAnimInstance::ResetPresentationState()
{
    check(IsInGameThread());
    GuardExpiresAt = 0.0;
    bAbyssGuard = false;
    ++ResetSerial;
    Montage_Stop(0.05f);
}

FAnimInstanceProxy* UMistAlphonseAnimInstance::CreateAnimInstanceProxy()
{
    return new FMistAlphonseAnimInstanceProxy(this);
}

void UMistAlphonseAnimInstance::DestroyAnimInstanceProxy(FAnimInstanceProxy* InProxy)
{
    delete static_cast<FMistAlphonseAnimInstanceProxy*>(InProxy);
}
