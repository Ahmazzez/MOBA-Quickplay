#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Tests/AutomationEditorCommon.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "HAL/PlatformTime.h"
#include "MistCombatHealth.h"
#include "MistMeleePresentation.h"
#include "MistAlphonseAnimInstance.h"
#include "MistTrainingDummy.h"
#include "MistworldPlayerController.h"
#include "TimerManager.h"

struct FMistMeleePresentationTestAccess
{
    static void AutoAttack(AMistworldPlayerController* PC, AMistTrainingDummy* Target)
    {
        PC->AttackTarget = Target;
        PC->BeginBasicAttack();
    }
    static void AimWithoutCommit(AMistworldPlayerController* PC)
    {
        PC->ActiveAimKey = EKeys::Q;
        PC->bAimingQ = true;
    }
    static void ClearPreview(AMistworldPlayerController* PC) { PC->ClearAim(); }
    static void Cleave(AMistworldPlayerController* PC, const FVector& Direction)
    {
        PC->ClearAim();
        PC->CommittedDirection = Direction;
        PC->StartCleave();
    }
    static void AcceptedMovementClick(AMistworldPlayerController* PC) { PC->OnInputStarted(); }
    static float RemainingWindup(AMistworldPlayerController* PC)
    {
        return PC->GetWorldTimerManager().GetTimerRemaining(PC->CombatTimer);
    }
};

namespace
{
class FMistVerifyMeleePresentation final : public IAutomationLatentCommand
{
public:
    explicit FMistVerifyMeleePresentation(FAutomationTestBase* InTest) : Test(InTest) {}

    bool Update() override
    {
        if (Started < 0.0) Started = FPlatformTime::Seconds();
        if (FPlatformTime::Seconds() - Started > 30.0)
        {
            Test->AddError(FString::Printf(TEXT("Melee presentation test timed out at stage %d."), Stage));
            return true;
        }
        UWorld* World = GEditor ? GEditor->PlayWorld : nullptr;
        AMistworldPlayerController* PC = World
            ? Cast<AMistworldPlayerController>(World->GetFirstPlayerController()) : nullptr;
        ACharacter* Hero = PC ? Cast<ACharacter>(PC->GetPawn()) : nullptr;
        if (!World || !World->HasBegunPlay() || !Hero || !Hero->GetMesh()) return false;
        const double Now = World->GetTimeSeconds();
        USkeletalMeshComponent* Mesh = Hero->GetMesh();
        UMistMeleePresentation* Presentation = Hero->FindComponentByClass<UMistMeleePresentation>();

        if (Stage == 0)
        {
            if (SettledAt < 0.0) SettledAt = Now;
            if (Now - SettledAt < 1.0) return false;
            if (!Test->TestNotNull(TEXT("Possession installs melee presentation on the template mannequin"), Presentation)) return true;
            UAnimInstance* Animation = Mesh->GetAnimInstance();
            if (!Test->TestNotNull(TEXT("Template mannequin still has an animation Blueprint instance"), Animation)) return true;
            AnimClass = Animation->GetClass();
            Test->TestEqual(TEXT("Presentation keeps the character in animation Blueprint mode"),
                static_cast<int32>(Mesh->GetAnimationMode()), static_cast<int32>(EAnimationMode::AnimationBlueprint));

            USceneComponent* Root = Presentation->GetSwordRoot();
            if (!Test->TestNotNull(TEXT("Placeholder sword root exists"), Root)) return true;
            Test->TestTrue(TEXT("Sword is attached to the mannequin skeletal mesh"), Root->GetAttachParent() == Mesh);
            const bool bAlphonseRig = Animation->IsA<UMistAlphonseAnimInstance>();
            Test->TestEqual(TEXT("Sword follows the authored grip or legacy right hand"), Root->GetAttachSocketName(),
                bAlphonseRig ? FName(TEXT("MistWeaponGrip")) : FName(TEXT("hand_r")));
            Test->TestEqual(TEXT("Sword has the authored model or four placeholder parts"),
                Root->GetNumChildrenComponents(), bAlphonseRig ? 1 : 4);
            const int32 ComponentCount = Hero->GetInstanceComponents().Num();
            Presentation->InitializeSword();
            Test->TestTrue(TEXT("Repeated initialization keeps the same sword root"), Presentation->GetSwordRoot() == Root);
            Test->TestEqual(TEXT("Repeated initialization creates no duplicate components"), Hero->GetInstanceComponents().Num(), ComponentCount);
            for (USceneComponent* Child : Root->GetAttachChildren())
                if (const UStaticMeshComponent* Part = Cast<UStaticMeshComponent>(Child))
                {
                    Test->TestTrue(TEXT("Sword part is registered"), Part->IsRegistered());
                    Test->TestEqual(TEXT("Sword part never supplies combat collision"),
                        static_cast<int32>(Part->GetCollisionEnabled()), static_cast<int32>(ECollisionEnabled::NoCollision));
                    Test->TestFalse(TEXT("Sword part cannot alter navigation"), Part->CanEverAffectNavigation());
                    Test->TestFalse(TEXT("Sword part does not generate overlaps"), Part->GetGenerateOverlapEvents());
                }

            // NullRHI has no rendered mesh. Force pose evaluation only for
            // this test so the real animation Blueprint's slot is exercised.
            Mesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
            Mesh->bEnableUpdateRateOptimizations = false;
            EvaluatePose(Mesh, 0.05f);
            Test->TestFalse(TEXT("An idle mannequin does not swing"), Presentation->IsSwingActive());
            FMistMeleePresentationTestAccess::AimWithoutCommit(PC);
            Test->TestFalse(TEXT("Holding Q preview does not start a sword swing"), Presentation->IsSwingActive());

            // Use a real controller commitment and real dummy. Animation
            // updates must not apply damage before the existing combat timer.
            FActorSpawnParameters Parameters;
            Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
            Direction = Hero->GetActorForwardVector().GetSafeNormal2D();
            Dummy = World->SpawnActor<AMistTrainingDummy>(Hero->GetActorLocation() + Direction * 160.f,
                FRotator::ZeroRotator, Parameters);
            if (!Test->TestNotNull(TEXT("Melee target spawns"), Dummy.Get())) return true;
            BeforeAutoHand = Mesh->GetSocketTransform(TEXT("hand_r"), RTS_Component);
            FMistMeleePresentationTestAccess::ClearPreview(PC);
            FMistMeleePresentationTestAccess::AutoAttack(PC, Dummy.Get());
            Test->TestTrue(TEXT("Basic attack begins a swing montage"), Presentation->IsSwingActive());
            Test->TestTrue(TEXT("Basic attack retains its commitment lock"), PC->IsCombatBusy());
            Test->TestEqual(TEXT("Basic attack keeps the 0.18 second impact timer"),
                FMistMeleePresentationTestAccess::RemainingWindup(PC), 0.18f, 0.01f);
            CheckMontage(Presentation, Mesh, TEXT("Basic attack"));
            EvaluatePose(Mesh, 0.18f);
            Test->TestTrue(TEXT("Basic attack moves the mannequin right hand through the animation Blueprint slot"),
                PoseChanged(BeforeAutoHand, Mesh->GetSocketTransform(TEXT("hand_r"), RTS_Component)));
            Test->TestEqual(TEXT("Evaluating the animation alone does not deal early auto damage"),
                Dummy->CombatHealth->Health, 1200.f, 0.01f);
            Stage = 1;
            StageAt = Now;
            return false;
        }

        if (!Presentation || !Dummy.IsValid())
        {
            Test->AddError(TEXT("Melee presentation or target disappeared during the test."));
            return true;
        }

        if (Stage == 1)
        {
            if (PC->IsCombatBusy() || Now - StageAt < 0.18) return false;
            Test->TestEqual(TEXT("Basic attack still deals 90 raw damage once against 20 armor"),
                Dummy->CombatHealth->Health, 1125.f, 0.1f);
            FMistMeleePresentationTestAccess::AcceptedMovementClick(PC);
            Test->TestFalse(TEXT("Accepted movement cancels basic-attack visual recovery"), Presentation->IsSwingActive());
            Test->TestTrue(TEXT("Cancelling recovery preserves the animation Blueprint instance"), Mesh->GetAnimInstance()->GetClass() == AnimClass.Get());
            EvaluatePose(Mesh, 0.06f);
            const FTransform BeforeCleaveHand = Mesh->GetSocketTransform(TEXT("hand_r"), RTS_Component);
            FMistMeleePresentationTestAccess::Cleave(PC, Direction);
            Test->TestTrue(TEXT("Q commitment begins its heavy swing montage"), Presentation->IsSwingActive());
            Test->TestEqual(TEXT("Q keeps the 0.25 second impact timer"),
                FMistMeleePresentationTestAccess::RemainingWindup(PC), 0.25f, 0.01f);
            CheckMontage(Presentation, Mesh, TEXT("Q"));
            EvaluatePose(Mesh, 0.25f);
            Test->TestTrue(TEXT("Q moves the mannequin right hand through the animation Blueprint slot"),
                PoseChanged(BeforeCleaveHand, Mesh->GetSocketTransform(TEXT("hand_r"), RTS_Component)));
            Test->TestEqual(TEXT("Evaluating Q animation alone does not deal early Q damage"),
                Dummy->CombatHealth->Health, 1125.f, 0.1f);
            Stage = 2;
            StageAt = Now;
            return false;
        }

        if (Stage == 2)
        {
            if (PC->IsCombatBusy() || Now - StageAt < 0.25) return false;
            Test->TestEqual(TEXT("Q still deals 140 raw damage once after its impact timer"),
                Dummy->CombatHealth->Health, 1125.f - 140.f / 1.2f, 0.1f);
            FMistMeleePresentationTestAccess::AcceptedMovementClick(PC);
            Test->TestFalse(TEXT("Accepted movement cancels Q visual recovery"), Presentation->IsSwingActive());
            Test->TestEqual(TEXT("Both swings leave locomotion in Blueprint animation mode"),
                static_cast<int32>(Mesh->GetAnimationMode()), static_cast<int32>(EAnimationMode::AnimationBlueprint));
            Test->TestTrue(TEXT("Both swings preserve the original animation Blueprint class"), Mesh->GetAnimInstance()->GetClass() == AnimClass.Get());
            Dummy->Destroy();
            return true;
        }
        return false;
    }

private:
    static void EvaluatePose(USkeletalMeshComponent* Mesh, const float DeltaTime)
    {
        Mesh->TickAnimation(DeltaTime, false);
        Mesh->RefreshBoneTransforms();
    }
    static bool PoseChanged(const FTransform& Before, const FTransform& After)
    {
        return FVector::Distance(Before.GetLocation(), After.GetLocation()) > 5.f
            || Before.GetRotation().AngularDistance(After.GetRotation()) > FMath::DegreesToRadians(5.f);
    }
    void CheckMontage(UMistMeleePresentation* Presentation, USkeletalMeshComponent* Mesh, const TCHAR* Label)
    {
        UAnimMontage* Montage = Presentation->GetActiveSwing();
        if (Test->TestNotNull(FString::Printf(TEXT("%s has an active dynamic montage"), Label), Montage))
        {
            if (Test->TestEqual(TEXT("Swing has one animation slot"), Montage->SlotAnimTracks.Num(), 1))
                Test->TestEqual(TEXT("Swing uses the existing locomotion Blueprint's DefaultSlot"),
                    Montage->SlotAnimTracks[0].SlotName, FName(TEXT("DefaultSlot")));
        }
        Test->TestTrue(TEXT("Playing a swing keeps the existing animation Blueprint class"), Mesh->GetAnimInstance()->GetClass() == AnimClass.Get());
        Test->TestEqual(TEXT("Playing a swing does not replace Blueprint animation with a single-node animation"),
            static_cast<int32>(Mesh->GetAnimationMode()), static_cast<int32>(EAnimationMode::AnimationBlueprint));
    }

    FAutomationTestBase* Test;
    TWeakObjectPtr<UClass> AnimClass;
    TWeakObjectPtr<AMistTrainingDummy> Dummy;
    FVector Direction;
    FTransform BeforeAutoHand;
    double Started = -1.0;
    double SettledAt = -1.0;
    double StageAt = 0.0;
    int32 Stage = 0;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMistMeleePresentationTest, "Mistworld.Combat.MeleePresentation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMistMeleePresentationTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(TEXT("/Game/TopDown/Lvl_TopDown")));
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(FMistVerifyMeleePresentation(this));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    return true;
}

#endif
