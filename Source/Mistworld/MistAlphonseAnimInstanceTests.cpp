#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "MistAlphonseAnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Components/SkeletalMeshComponent.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Tests/AutomationEditorCommon.h"

namespace
{
void EvaluateAlphonsePose(USkeletalMeshComponent* Mesh, float DeltaSeconds)
{
    Mesh->TickAnimation(DeltaSeconds, false);
    Mesh->RefreshBoneTransforms();
}

ACharacter* CreateAnimationFixture(UWorld* World, const ACharacter* Hero)
{
    FActorSpawnParameters Spawn;
    Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    Spawn.ObjectFlags |= RF_Transient;
    ACharacter* Fixture = World->SpawnActor<ACharacter>(FVector(0.f, 0.f, -45000.f), FRotator::ZeroRotator, Spawn);
    if (!Fixture) return nullptr;
    Fixture->SetActorEnableCollision(false);
    Fixture->SetActorHiddenInGame(true);
    Fixture->SetActorTickEnabled(false);
    Fixture->GetCharacterMovement()->SetComponentTickEnabled(false);
    Fixture->GetCharacterMovement()->Velocity = FVector(500.f, 0.f, 0.f);
    USkeletalMeshComponent* Mesh = Fixture->GetMesh();
    Mesh->SetSkeletalMeshAsset(Hero->GetMesh()->GetSkeletalMeshAsset());
    Mesh->SetRelativeTransform(Hero->GetMesh()->GetRelativeTransform());
    Mesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
    Mesh->bEnableUpdateRateOptimizations = false;
    Mesh->SetComponentTickEnabled(false);
    // InitAnim otherwise consumes this frame's native proxy update with a
    // zero-delta tick before the explicit, positive-delta pose comparison.
    Mesh->bUseRefPoseOnInitAnim = true;
    Mesh->SetAnimInstanceClass(UMistAlphonseAnimInstance::StaticClass());
    return Fixture;
}

class FMistVerifyAlphonseAnimation final : public IAutomationLatentCommand
{
public:
    explicit FMistVerifyAlphonseAnimation(FAutomationTestBase* InTest) : Test(InTest) {}

    virtual bool Update() override
    {
        if (Started < 0.0) Started = FPlatformTime::Seconds();
        if (FPlatformTime::Seconds() - Started > 30.0)
        {
            Test->AddError(TEXT("Alphonse animation fixture timed out waiting for the actual playable pawn."));
            return true;
        }
        UWorld* World = GEditor ? GEditor->PlayWorld : nullptr;
        ACharacter* Hero = World && World->GetFirstPlayerController()
            ? Cast<ACharacter>(World->GetFirstPlayerController()->GetPawn()) : nullptr;
        if (!World || !World->HasBegunPlay() || !Hero || !Hero->GetMesh()) return false;
        USkeletalMeshComponent* HeroMesh = Hero->GetMesh();
        if (!Test->TestNotNull(TEXT("The real playable pawn has a skeletal mesh"), HeroMesh->GetSkeletalMeshAsset())) return true;
        Test->TestTrue(TEXT("The real pawn uses the imported Alphonse model"),
            HeroMesh->GetSkeletalMeshAsset()->GetPathName().StartsWith(TEXT("/Game/Mistworld/Characters/Alphonse/")));
        Test->TestNotNull(TEXT("The real pawn uses the native Alphonse animation graph"),
            Cast<UMistAlphonseAnimInstance>(HeroMesh->GetAnimInstance()));
        Test->TestTrue(TEXT("Alphonse has the native graph's upper-body branch"), HeroMesh->GetBoneIndex(TEXT("spine_01")) != INDEX_NONE);
        Test->TestTrue(TEXT("Alphonse has the dedicated weapon attachment bone"), HeroMesh->GetBoneIndex(TEXT("weapon_r")) != INDEX_NONE);

        // Two unpossessed fixtures evaluate the same real mesh and graph at
        // identical times. This exercises bone filtering without changing the
        // player's combat state, movement orders, equipment, or saved loadout.
        ACharacter* Baseline = CreateAnimationFixture(World, Hero);
        ACharacter* Guarded = CreateAnimationFixture(World, Hero);
        if (!Baseline || !Guarded)
        {
            Test->AddError(TEXT("Could not create Alphonse animation comparison fixtures."));
            if (Baseline) Baseline->Destroy();
            if (Guarded) Guarded->Destroy();
            return true;
        }
        USkeletalMeshComponent* BaseMesh = Baseline->GetMesh();
        USkeletalMeshComponent* GuardMesh = Guarded->GetMesh();
        UMistAlphonseAnimInstance* Animation = Cast<UMistAlphonseAnimInstance>(GuardMesh->GetAnimInstance());
        if (!Test->TestNotNull(TEXT("A fresh mesh creates the native graph"), Animation))
        {
            Baseline->Destroy(); Guarded->Destroy();
            return true;
        }
        Test->TestEqual(TEXT("The native graph ignores root motion"),
            static_cast<int32>(Animation->RootMotionMode), static_cast<int32>(ERootMotionMode::IgnoreRootMotion));
        Animation->SetGuard(false, 5.f);
        Test->TestEqual(TEXT("Guard presentation receives the specified duration"), Animation->GetGuardRemainingTime(), 5.f, 0.01f);
        EvaluateAlphonsePose(BaseMesh, 0.2f);
        EvaluateAlphonsePose(GuardMesh, 0.2f);

        bool bRunningPose = false;
        const TArray<FTransform>& RefPose = BaseMesh->GetSkeletalMeshAsset()->GetRefSkeleton().GetRefBonePose();
        const TArray<FTransform>& BasePose = BaseMesh->GetBoneSpaceTransforms();
        const TArray<FTransform>& GuardPose = GuardMesh->GetBoneSpaceTransforms();
        for (const FName Bone : {FName(TEXT("thigh_l")), FName(TEXT("calf_l")), FName(TEXT("foot_l")),
                                FName(TEXT("thigh_r")), FName(TEXT("calf_r")), FName(TEXT("foot_r"))})
        {
            const int32 Index = BaseMesh->GetBoneIndex(Bone);
            if (!Test->TestTrue(*FString::Printf(TEXT("Alphonse includes %s"), *Bone.ToString()), Index != INDEX_NONE)) continue;
            if (BasePose.IsValidIndex(Index) && GuardPose.IsValidIndex(Index) && RefPose.IsValidIndex(Index))
            {
                bRunningPose |= !BasePose[Index].Equals(RefPose[Index], 0.001f);
                Test->TestTrue(*FString::Printf(TEXT("Guard preserves running %s"), *Bone.ToString()),
                    BasePose[Index].Equals(GuardPose[Index], 0.001f));
            }
        }
        Test->TestTrue(TEXT("Velocity selects a genuinely animated run pose"), bRunningPose);
        Test->TestFalse(TEXT("Guard changes the upper body while legs continue running"),
            BaseMesh->GetSocketTransform(TEXT("hand_r"), RTS_Component).Equals(
                GuardMesh->GetSocketTransform(TEXT("hand_r"), RTS_Component), 0.001f));

        UAnimSequence* Auto = LoadObject<UAnimSequence>(nullptr,
            TEXT("/Game/Mistworld/Characters/Alphonse/Animations/A_Alphonse_Auto.A_Alphonse_Auto"));
        if (Test->TestNotNull(TEXT("The matching native auto-attack clip exists"), Auto))
        {
            Test->TestTrue(TEXT("Auto attack uses this model's skeleton"), Auto->GetSkeleton() == HeroMesh->GetSkeletalMeshAsset()->GetSkeleton());
            const FTransform GuardHand = GuardMesh->GetSocketTransform(TEXT("hand_r"), RTS_Component);
            UAnimMontage* Montage = Animation->PlaySlotAnimationAsDynamicMontage(Auto, TEXT("DefaultSlot"), 0.02f, 0.06f);
            if (Test->TestNotNull(TEXT("DefaultSlot accepts the action montage"), Montage))
            {
                EvaluateAlphonsePose(GuardMesh, 0.18f);
                Test->TestTrue(TEXT("The native graph keeps the action montage active"), Animation->Montage_IsPlaying(Montage));
                Test->TestFalse(TEXT("The action montage overrides the held guard pose"),
                    GuardHand.Equals(GuardMesh->GetSocketTransform(TEXT("hand_r"), RTS_Component), 0.001f));
            }
        }
        Animation->ResetPresentationState();
        Test->TestEqual(TEXT("A presentation reset removes guard immediately"), Animation->GetGuardRemainingTime(), 0.f);
        EvaluateAlphonsePose(GuardMesh, 0.12f);
        Test->TestFalse(TEXT("A presentation reset stops action montages"), Animation->Montage_IsActive(nullptr));
        Baseline->Destroy();
        Guarded->Destroy();
        return true;
    }

private:
    FAutomationTestBase* Test;
    double Started = -1.0;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMistAlphonseAnimationTest, "Mistworld.Presentation.AlphonseNativeAnimation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMistAlphonseAnimationTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(TEXT("/Game/TopDown/Lvl_TopDown")));
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(FMistVerifyAlphonseAnimation(this));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    return true;
}

#endif
