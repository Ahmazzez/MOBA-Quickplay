#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MistAnimaPresentation.h"
#include "MistAlphonseAnimInstance.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMistAnimaPresentationTest, "Mistworld.Anima.Presentation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMistAnimaPresentationTest::RunTest(const FString& Parameters)
{
    USkeletalMesh* Anima = LoadObject<USkeletalMesh>(nullptr,
        TEXT("/Game/Mistworld/Characters/AnimaAnime/SK_Anima.SK_Anima"));
    USkeletalMesh* Alphonse = LoadObject<USkeletalMesh>(nullptr,
        TEXT("/Game/Mistworld/Characters/Alphonse/SK_Alphonse_Blockout.SK_Alphonse_Blockout"));
    if (!TestNotNull(TEXT("Downloaded anime Anima model is installed"), Anima)
        || !TestNotNull(TEXT("Existing Alphonse mesh is retained"), Alphonse)) return false;
    TestTrue(TEXT("Anima has a complete humanoid skeleton"), Anima->GetRefSkeleton().GetNum() >= 20);
    for (const TCHAR* Bone : {TEXT("Root"), TEXT("Head"), TEXT("Wrist_R"), TEXT("Wrist_L")})
        TestTrue(FString::Printf(TEXT("Anime Anima retains required bone %s"), Bone),
            Anima->GetRefSkeleton().FindBoneIndex(FName(Bone)) != INDEX_NONE);
    TestTrue(TEXT("Anima is normalized to a human height"),
        FMath::Abs(Anima->GetBounds().BoxExtent.Z * 2.f - 170.f) < 4.f);
    TestTrue(TEXT("Anima has distinct clothing, face and weapon sections"), Anima->GetMaterials().Num() >= 6);
    int32 TexturedSlots = 0;
    for (const FSkeletalMaterial& Slot : Anima->GetMaterials())
    {
        UMaterialInterface* Material = Slot.MaterialInterface.Get();
        if (!TestNotNull(TEXT("Every Anima material slot is assigned"), Material)) continue;
        UMaterial* Base = Material->GetMaterial();
        TestTrue(TEXT("Anima materials retain their skeletal mesh shader usage"), Base && Base->GetUsageByFlag(MATUSAGE_SkeletalMesh));
        TSet<const UTexture*> Textures;
        Material->GetReferencedTexturesAndOverrides(Textures);
        if (!Textures.IsEmpty()) ++TexturedSlots;
        for (const UTexture* Texture : Textures)
        {
            const UTexture2D* Image = Cast<UTexture2D>(Texture);
            TestTrue(FString::Printf(TEXT("Anima texture %s retains a usable source image"), *Texture->GetPathName()),
                Image && Image->Source.IsValid() && Image->Source.GetSizeX() > 1 && Image->Source.GetSizeY() > 1);
        }
    }
    TestTrue(TEXT("Anime face and clothing use imported texture maps"), TexturedSlots > 0);
    TMap<FName, UAnimSequence*> Clips;
    for (const TCHAR* Name : {TEXT("Idle"), TEXT("Run"), TEXT("Attack"), TEXT("Thrust"),
        TEXT("Dash"), TEXT("Guard"), TEXT("Ultimate"), TEXT("Death")})
    {
        const FString Path = FString::Printf(
            TEXT("/Game/Mistworld/Characters/AnimaAnime/Animations/A_Anima_%s.A_Anima_%s"), Name, Name);
        UAnimSequence* Clip = LoadObject<UAnimSequence>(nullptr, *Path);
        if (!TestNotNull(FString::Printf(TEXT("Anima %s motion is installed"), Name), Clip)) return false;
        TestTrue(TEXT("Skill motion uses the anime model's skeleton"), Clip->GetSkeleton() == Anima->GetSkeleton());
        TestTrue(TEXT("Skill motion has usable time"), Clip->GetPlayLength() > 0.1f);
        TestFalse(TEXT("Anima animations cannot add gameplay root motion"), Clip->bEnableRootMotion);
        Clips.Add(FName(Name), Clip);
    }

    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    if (GEngine) GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    ACharacter* Hero = World->SpawnActor<ACharacter>();
    USkeletalMeshComponent* Body = Hero->GetMesh();
    Body->SetSkeletalMesh(Alphonse);
    Body->SetAnimInstanceClass(UMistAlphonseAnimInstance::StaticClass());
    Body->SetRelativeLocation(FVector(0.f, 0.f, -90.f));
    Body->SetRelativeRotation(FRotator(0.f, 15.f, 0.f));
    Body->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
    Body->bEnableUpdateRateOptimizations = false;
    const FTransform OriginalTransform = Body->GetRelativeTransform();
    const float CapsuleRadius = Hero->GetCapsuleComponent()->GetUnscaledCapsuleRadius();
    const float CapsuleHalfHeight = Hero->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight();
    const auto CollisionMode = Body->GetCollisionEnabled();
    UCharacterMovementComponent* Movement = Hero->GetCharacterMovement();
    UMaterialInterface* OriginalMaterial = Body->GetMaterial(0);
    UMistAnimaPresentation* Presentation = NewObject<UMistAnimaPresentation>(Hero);
    Hero->AddInstanceComponent(Presentation); Presentation->RegisterComponent(); Presentation->Initialize();
    TestTrue(TEXT("A visual switch succeeds"), Presentation->SetHeroActive(true));
    TestTrue(TEXT("Anima uses the character's existing visual mesh component"), Presentation->GetBody() == Body);
    TestTrue(TEXT("Switching selects the downloaded anime model asset"), Body->GetSkeletalMeshAsset() == Anima);
    TestTrue(TEXT("Idle has a live native animation player"), Body->GetSingleNodeInstance()
        && Body->GetSingleNodeInstance()->GetCurrentAsset() == Clips[TEXT("Idle")]);
    TestEqual(TEXT("Visual switch keeps the mesh's collision mode"), Body->GetCollisionEnabled(), CollisionMode);
    TestTrue(TEXT("Visual switch retains the movement component"), Hero->GetCharacterMovement() == Movement);
    TestEqual(TEXT("Visual switch retains gameplay radius"), Hero->GetCapsuleComponent()->GetUnscaledCapsuleRadius(), CapsuleRadius);
    TestEqual(TEXT("Visual switch retains gameplay capsule height"), Hero->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight(), CapsuleHalfHeight);
    Body->TickAnimation(0.05f, false); Body->RefreshBoneTransforms();
    TestTrue(TEXT("Imported right weapon hand exists under the FBX-sanitized name"), Body->GetBoneIndex(TEXT("Wrist_R")) != INDEX_NONE);
    const FVector Before = Body->GetSocketLocation(TEXT("Wrist_R"));
    Presentation->PlayAttack(0.6f);
    Body->TickAnimation(0.25f, false); Body->RefreshBoneTransforms();
    TestTrue(TEXT("Attack selects the matching native clip"), Body->GetSingleNodeInstance()->GetCurrentAsset() == Clips[TEXT("Attack")]);
    TestTrue(TEXT("Attack deforms the rig and moves the weapon hand"), !Before.Equals(Body->GetSocketLocation(TEXT("Wrist_R")), 0.5f));
    Presentation->TickComponent(0.7f, LEVELTICK_All, nullptr);
    TestTrue(TEXT("Timed commitment returns to idle"), Body->GetSingleNodeInstance()->GetCurrentAsset() == Clips[TEXT("Idle")]);
    Presentation->PlayThrust(0.3f);
    TestTrue(TEXT("Rapier thrust uses its own matching motion"), Body->GetSingleNodeInstance()->GetCurrentAsset() == Clips[TEXT("Thrust")]);
    Presentation->ResetPresentation();
    TestTrue(TEXT("Reset cancels active skill motion"), Body->GetSingleNodeInstance()->GetCurrentAsset() == Clips[TEXT("Idle")]);
    TestTrue(TEXT("Switching back succeeds"), Presentation->SetHeroActive(false));
    TestTrue(TEXT("Switching back restores Alphonse's asset"), Body->GetSkeletalMeshAsset() == Alphonse);
    TestTrue(TEXT("Switching back restores the full mesh transform"), Body->GetRelativeTransform().Equals(OriginalTransform, 0.001f));
    TestTrue(TEXT("Switching back restores Alphonse's animation class"), Body->GetAnimClass() == UMistAlphonseAnimInstance::StaticClass());
    TestTrue(TEXT("Switching back preserves the original material"), Body->GetMaterial(0) == OriginalMaterial);
    TestTrue(TEXT("Repeated switches remain usable"), Presentation->SetHeroActive(true) && Presentation->SetHeroActive(false));
    World->DestroyWorld(false);
    if (GEngine) GEngine->DestroyWorldContext(World);
    return true;
}

#endif

