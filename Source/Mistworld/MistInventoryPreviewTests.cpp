#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "MistInventoryPreview.h"
#include "MistMeleePresentation.h"
#include "MistAlphonseAnimInstance.h"
#include "MistworldPlayerController.h"
#include "Animation/AnimSequence.h"
#include "Components/PoseableMeshComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "HAL/PlatformTime.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Tests/AutomationEditorCommon.h"

struct FMistInventoryPreviewTestAccess
{
    static void Refresh(AMistInventoryPreview* Preview) { Preview->RefreshSource(); }
    static UPrimitiveComponent* FindCopy(AMistInventoryPreview* Preview, UPrimitiveComponent* Source)
    {
        for (const AMistInventoryPreview::FCopyBinding& Binding : Preview->Bindings)
            if (Binding.Source.Get() == Source) return Binding.Copy.Get();
        return nullptr;
    }
    static USceneCaptureComponent2D* GetCapture(AMistInventoryPreview* Preview) { return Preview->Capture; }
    static bool AllCopiesAreCosmetic(AMistInventoryPreview* Preview, FString& Diagnostic)
    {
        TArray<TObjectPtr<UPrimitiveComponent>> Primitives = Preview->Copies;
        Primitives.Append(Preview->StageParts);
        for (UPrimitiveComponent* Primitive : Primitives)
        {
            if (!Primitive)
            {
                Diagnostic = TEXT("A portrait primitive is missing.");
                return false;
            }
            if (Primitive->GetCollisionEnabled() != ECollisionEnabled::NoCollision
                || Primitive->GetGenerateOverlapEvents() || Primitive->CanEverAffectNavigation()
                || !Primitive->bVisibleInSceneCaptureOnly || Primitive->IsComponentTickEnabled()
                || Primitive->PrimaryComponentTick.IsTickFunctionRegistered())
            {
                Diagnostic = FString::Printf(TEXT("Portrait primitive %s: collision=%d overlaps=%d navigation=%d capture-only=%d tick-enabled=%d tick-registered=%d"),
                    *Primitive->GetName(), static_cast<int32>(Primitive->GetCollisionEnabled()),
                    Primitive->GetGenerateOverlapEvents(), Primitive->CanEverAffectNavigation(),
                    Primitive->bVisibleInSceneCaptureOnly, Primitive->IsComponentTickEnabled(),
                    Primitive->PrimaryComponentTick.IsTickFunctionRegistered());
                return false;
            }
        }
        return true;
    }
};

namespace
{
class FMistVerifyInventoryPreview final : public IAutomationLatentCommand
{
public:
    explicit FMistVerifyInventoryPreview(FAutomationTestBase* InTest) : Test(InTest) {}

    bool Update() override
    {
        if (Started < 0.) Started = FPlatformTime::Seconds();
        if (FPlatformTime::Seconds() - Started > 30.)
        {
            Test->AddError(TEXT("Inventory preview fixture timed out waiting for the real character."));
            return true;
        }
        UWorld* World = GEditor ? GEditor->PlayWorld : nullptr;
        AMistworldPlayerController* PC = World
            ? Cast<AMistworldPlayerController>(World->GetFirstPlayerController()) : nullptr;
        ACharacter* Hero = PC ? Cast<ACharacter>(PC->GetPawn()) : nullptr;
        UMistMeleePresentation* Sword = Hero ? Hero->FindComponentByClass<UMistMeleePresentation>() : nullptr;
        if (!World || !World->HasBegunPlay() || !Hero || !Hero->GetMesh() || !Sword || !Sword->GetBlade()) return false;

        FActorSpawnParameters Spawn;
        Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        Spawn.ObjectFlags |= RF_Transient;
        AMistInventoryPreview* Preview = World->SpawnActor<AMistInventoryPreview>(
            FVector(0.f, 0.f, -50000.f), FRotator::ZeroRotator, Spawn);
        if (!Test->TestNotNull(TEXT("A standalone cosmetic inventory preview spawns"), Preview)) return true;
        const FTransform HeroTransform = Hero->GetActorTransform();
        const FTransform MeshTransform = Hero->GetMesh()->GetRelativeTransform();
        const int32 HeroComponentCount = Hero->GetInstanceComponents().Num();
        const bool OriginalSwordTick = Sword->GetBlade()->IsComponentTickEnabled();
        const ECollisionEnabled::Type OriginalSwordCollision = Sword->GetBlade()->GetCollisionEnabled();

        Preview->UpdateFromPawn(Hero);
        Preview->SetPreviewActive(true);
        Test->TestTrue(TEXT("Inventory displays the actual skeletal character asset"), Preview->HasCharacterModel());
        UPoseableMeshComponent* BodyCopy = Cast<UPoseableMeshComponent>(
            FMistInventoryPreviewTestAccess::FindCopy(Preview, Hero->GetMesh()));
        if (Test->TestNotNull(TEXT("The preview owns a separate pose copy of the player model"), BodyCopy))
        {
            Test->TestTrue(TEXT("The pose copy uses the current player skeletal mesh"),
                BodyCopy->GetSkinnedAsset() == Hero->GetMesh()->GetSkeletalMeshAsset());
            Test->TestTrue(TEXT("The pose copy uses the current character material"),
                BodyCopy->GetMaterial(0) == Hero->GetMesh()->GetMaterial(0));
            Test->TestEqual(TEXT("Every source bone has a corresponding copied pose bone"),
                BodyCopy->GetNumBones(), Hero->GetMesh()->GetNumBones());
        }
        UStaticMeshComponent* BladeCopy = Cast<UStaticMeshComponent>(
            FMistInventoryPreviewTestAccess::FindCopy(Preview, Sword->GetBlade()));
        if (Test->TestNotNull(TEXT("The currently attached sword is included in the portrait"), BladeCopy))
        {
            Test->TestTrue(TEXT("The sword copy uses the actual equipped blade mesh"),
                BladeCopy->GetStaticMesh() == Sword->GetBlade()->GetStaticMesh());
            Test->TestEqual(TEXT("The sword copy retains every source material slot"),
                BladeCopy->GetNumMaterials(), Sword->GetBlade()->GetNumMaterials());
            for (int32 MaterialIndex = 0; MaterialIndex < Sword->GetBlade()->GetNumMaterials(); ++MaterialIndex)
            {
                Test->TestTrue(FString::Printf(TEXT("The sword copy matches source material slot %d"), MaterialIndex),
                    BladeCopy->GetMaterial(MaterialIndex) == Sword->GetBlade()->GetMaterial(MaterialIndex));
            }
        }
        const bool bAlphonseRig = Hero->GetMesh()->GetAnimInstance()
            && Hero->GetMesh()->GetAnimInstance()->IsA<UMistAlphonseAnimInstance>();
        Test->TestTrue(TEXT("The portrait includes the body and every attached sword mesh"),
            Preview->GetCopiedComponentCount() >= (bAlphonseRig ? 2 : 5));
        USceneCaptureComponent2D* Capture = FMistInventoryPreviewTestAccess::GetCapture(Preview);
        Test->TestEqual(TEXT("The capture renders only explicit portrait components"),
            static_cast<int32>(Capture->PrimitiveRenderMode),
            static_cast<int32>(ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList));
        Test->TestFalse(TEXT("Inventory capture never renders every world frame"), Capture->bCaptureEveryFrame);
        Test->TestFalse(TEXT("Moving the preview stage never implicitly captures"), Capture->bCaptureOnMovement);
        Test->TestFalse(TEXT("Manual portrait capture does not register a component tick"), Capture->PrimaryComponentTick.IsTickFunctionRegistered());
        Test->TestFalse(TEXT("A cosmetic portrait never has gameplay collision"), Preview->GetActorEnableCollision());
        FString CosmeticDiagnostic;
        if (!Test->TestTrue(TEXT("Copies and stage parts cannot affect gameplay or the world camera"),
            FMistInventoryPreviewTestAccess::AllCopiesAreCosmetic(Preview, CosmeticDiagnostic)))
            Test->AddError(CosmeticDiagnostic);
        if (!FApp::CanEverRender())
            Test->TestNull(TEXT("Headless tests avoid allocating a render target"), Preview->GetRenderTarget());

        const float InitialYaw = Preview->GetPreviewYaw();
        Preview->Rotate(45.f);
        Test->TestEqual(TEXT("Rotate button rotates only the portrait"), Preview->GetPreviewYaw(), InitialYaw + 45.f, 0.01f);
        Preview->ResetRotation();
        Test->TestEqual(TEXT("Reset returns to the initial three-quarter view"), Preview->GetPreviewYaw(), InitialYaw, 0.01f);
        Test->TestTrue(TEXT("Portrait rotation does not rotate or move the gameplay character"),
            Hero->GetActorTransform().Equals(HeroTransform));
        Test->TestTrue(TEXT("Portrait pose copying preserves the player's mesh transform"),
            Hero->GetMesh()->GetRelativeTransform().Equals(MeshTransform));
        Test->TestEqual(TEXT("The preview adds no components to the gameplay character"),
            Hero->GetInstanceComponents().Num(), HeroComponentCount);
        Test->TestEqual(TEXT("Copying a sword leaves its gameplay collision untouched"),
            static_cast<int32>(Sword->GetBlade()->GetCollisionEnabled()), static_cast<int32>(OriginalSwordCollision));
        Test->TestEqual(TEXT("Copying a sword leaves its ticking unchanged"),
            Sword->GetBlade()->IsComponentTickEnabled(), OriginalSwordTick);

        // A modular armor component is discovered automatically. Replacing its
        // asset/material on the player also replaces the portrait's appearance.
        USkeletalMeshComponent* Armor = NewObject<USkeletalMeshComponent>(Hero, TEXT("InventoryTestArmor"));
        Hero->AddInstanceComponent(Armor);
        Armor->SetupAttachment(Hero->GetMesh());
        Armor->SetSkeletalMeshAsset(Hero->GetMesh()->GetSkeletalMeshAsset());
        Armor->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Armor->SetCanEverAffectNavigation(false);
        Armor->RegisterComponent();
        Armor->SetLeaderPoseComponent(Hero->GetMesh());
        FMistInventoryPreviewTestAccess::Refresh(Preview);
        UPoseableMeshComponent* ArmorCopy = Cast<UPoseableMeshComponent>(
            FMistInventoryPreviewTestAccess::FindCopy(Preview, Armor));
        if (Test->TestNotNull(TEXT("Attached modular skeletal equipment appears automatically"), ArmorCopy))
        {
            USkeletalMesh* Alternate = LoadObject<USkeletalMesh>(nullptr,
                TEXT("/Game/Characters/Mannequins/Meshes/SKM_Quinn_Simple.SKM_Quinn_Simple"));
            if (Test->TestNotNull(TEXT("An alternate real character mesh is available"), Alternate))
            {
                Armor->SetSkeletalMeshAsset(Alternate);
                FMistInventoryPreviewTestAccess::Refresh(Preview);
                Test->TestTrue(TEXT("Changing a skeletal asset updates its existing portrait copy"),
                    ArmorCopy->GetSkinnedAsset() == Alternate);
            }
            if (UMaterialInterface* Base = Armor->GetMaterial(0))
            {
                UMaterialInstanceDynamic* Replacement = UMaterialInstanceDynamic::Create(Base, Armor);
                Armor->SetMaterial(0, Replacement);
                FMistInventoryPreviewTestAccess::Refresh(Preview);
                Test->TestTrue(TEXT("Replacing an equipment material updates the portrait"),
                    ArmorCopy->GetMaterial(0) == Replacement);
            }
        }
        Armor->DestroyComponent();
        FMistInventoryPreviewTestAccess::Refresh(Preview);
        Test->TestNull(TEXT("Removed armor is removed from the portrait"),
            FMistInventoryPreviewTestAccess::FindCopy(Preview, Armor));

        Preview->SetPreviewActive(false);
        Test->TestFalse(TEXT("Closing inventory disables preview ticking"), Preview->IsActorTickEnabled());
        Test->TestFalse(TEXT("Closing inventory marks the capture inactive"), Preview->IsPreviewActive());
        Preview->SetPreviewActive(true);
        Preview->UpdateFromPawn(nullptr);
        Test->TestEqual(TEXT("Losing the source pawn removes every copied model component"), Preview->GetCopiedComponentCount(), 0);
        Test->TestFalse(TEXT("Losing the source pawn disables rendering and ticking"), Preview->IsPreviewActive());

        // The destroyed-source delegate also cleans a closed inventory. This
        // fixture is not possessed and never touches progression or save slots.
        ACharacter* SourceFixture = World->SpawnActor<ACharacter>(
            FVector(0.f, 0.f, -48000.f), FRotator::ZeroRotator, Spawn);
        if (Test->TestNotNull(TEXT("An unpossessed source lifecycle fixture spawns"), SourceFixture))
        {
            SourceFixture->GetMesh()->SetSkeletalMeshAsset(Hero->GetMesh()->GetSkeletalMeshAsset());
            UAnimSequence* Swing = LoadObject<UAnimSequence>(nullptr,
                Hero->GetMesh()->GetAnimInstance()->IsA<UMistAlphonseAnimInstance>()
                    ? TEXT("/Game/Mistworld/Characters/Alphonse/Animations/A_Alphonse_Auto.A_Alphonse_Auto")
                    : TEXT("/Game/Mistworld/Animations/A_Sword_Auto.A_Sword_Auto"));
            if (Test->TestNotNull(TEXT("A real swing animation is available for the modular-pose fixture"), Swing))
            {
                SourceFixture->GetMesh()->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
                SourceFixture->GetMesh()->bEnableUpdateRateOptimizations = false;
                SourceFixture->GetMesh()->PlayAnimation(Swing, false);
                SourceFixture->GetMesh()->TickAnimation(0.15f, false);
                SourceFixture->GetMesh()->RefreshBoneTransforms();
            }
            USkeletalMeshComponent* Follower = NewObject<USkeletalMeshComponent>(SourceFixture, TEXT("InventoryFollowerArmor"));
            SourceFixture->AddInstanceComponent(Follower);
            Follower->SetupAttachment(SourceFixture->GetMesh());
            Follower->SetSkeletalMeshAsset(SourceFixture->GetMesh()->GetSkeletalMeshAsset());
            Follower->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            Follower->SetCanEverAffectNavigation(false);
            Follower->RegisterComponent();
            Follower->SetLeaderPoseComponent(SourceFixture->GetMesh());
            Preview->UpdateFromPawn(SourceFixture);
            Test->TestTrue(TEXT("Switching to another current pawn builds its model"), Preview->HasCharacterModel());
            UPoseableMeshComponent* FollowerCopy = Cast<UPoseableMeshComponent>(
                FMistInventoryPreviewTestAccess::FindCopy(Preview, Follower));
            if (Test->TestNotNull(TEXT("The animated modular armor has its own portrait mesh"), FollowerCopy))
            {
                const TArray<FTransform> AnimatedBones = SourceFixture->GetMesh()->GetBoneSpaceTransforms();
                const TArray<FTransform>& RefBones = SourceFixture->GetMesh()->GetSkeletalMeshAsset()->GetRefSkeleton().GetRefBonePose();
                const TArray<FTransform>& CopiedBones = FollowerCopy->GetBoneSpaceTransforms();
                int32 ChangedBone = INDEX_NONE;
                for (int32 Bone = 0; Bone < FMath::Min(AnimatedBones.Num(), RefBones.Num()); ++Bone)
                    if (!AnimatedBones[Bone].Equals(RefBones[Bone], 0.001f))
                    { ChangedBone = Bone; break; }
                if (Test->TestTrue(TEXT("The leader fixture has an animated pose distinct from the armor reference pose"), ChangedBone != INDEX_NONE))
                    Test->TestTrue(TEXT("Follower armor copies the animated body leader rather than its stale own pose"),
                        CopiedBones.IsValidIndex(ChangedBone) && CopiedBones[ChangedBone].Equals(AnimatedBones[ChangedBone], 0.001f));
            }
            SourceFixture->Destroy();
            Test->TestEqual(TEXT("Destroying the source removes its portrait immediately"), Preview->GetCopiedComponentCount(), 0);
            Test->TestFalse(TEXT("Source destruction disables the portrait even while closed"), Preview->IsPreviewActive());
        }
        Preview->Destroy();
        Test->TestTrue(TEXT("HUD lifetime can destroy the transient preview cleanly"), Preview->IsActorBeingDestroyed());
        Test->TestNull(TEXT("Destroying the preview releases its capture target"), Capture->TextureTarget.Get());
        Test->TestEqual(TEXT("Destroying the preview clears the show-only list"), Capture->ShowOnlyComponents.Num(), 0);
        return true;
    }

private:
    FAutomationTestBase* Test;
    double Started = -1.;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMistInventoryPreviewTest, "Mistworld.UI.InventoryPreview",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMistInventoryPreviewTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(TEXT("/Game/Mistworld/Maps/Lvl_CombatTest")));
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(FMistVerifyInventoryPreview(this));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    return true;
}

#endif
