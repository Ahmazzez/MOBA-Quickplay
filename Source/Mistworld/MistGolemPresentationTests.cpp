#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MistGolemPresentation.h"
#include "MistCryptGolem.h"
#include "MistCryptGuardian.h"
#include "MistCombatHealth.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "Materials/MaterialInstanceDynamic.h"

namespace
{
struct FGolemPresentationWorld
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    FGolemPresentationWorld()
    {
        if (GEngine) GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    }
    ~FGolemPresentationWorld()
    {
        World->DestroyWorld(false);
        if (GEngine) GEngine->DestroyWorldContext(World);
    }
};

bool ReadTint(USkeletalMeshComponent* Body, FName Name, FLinearColor& Value)
{
    for (int32 Index = 0; Index < Body->GetNumMaterials(); ++Index)
    {
        UMaterialInstanceDynamic* Material = Cast<UMaterialInstanceDynamic>(Body->GetMaterial(Index));
        if (!Material) continue;
        TArray<FMaterialParameterInfo> Parameters;
        TArray<FGuid> IDs;
        Material->GetAllVectorParameterInfo(Parameters, IDs);
        for (const FMaterialParameterInfo& Parameter : Parameters)
        {
            if (Parameter.Name == Name)
            {
                Value = Material->K2_GetVectorParameterValue(Name);
                return true;
            }
        }
    }
    return false;
}

void PresentationTick(UMistGolemPresentation* Presentation)
{
    Presentation->TickComponent(0.f, LEVELTICK_All, nullptr);
    Presentation->GetBody()->TickAnimation(0.05f, false);
    Presentation->GetBody()->RefreshBoneTransforms();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMistGolemPresentationTest, "Mistworld.Dungeon.GolemPresentation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMistGolemPresentationTest::RunTest(const FString& Parameters)
{
    USkeletalMesh* ImportedMesh = LoadObject<USkeletalMesh>(nullptr,
        TEXT("/Game/Mistworld/Enemies/Golem/SK_StoneGolem.SK_StoneGolem"));
    if (!TestNotNull(TEXT("Imported online golem mesh is installed"), ImportedMesh)) return false;
    TestTrue(TEXT("Imported reference mesh is normalized to two metres"),
        FMath::Abs(ImportedMesh->GetBounds().BoxExtent.Z * 2.f - 200.f) < 3.f);
    TestTrue(TEXT("Imported model has a usable skeletal hierarchy"), ImportedMesh->GetRefSkeleton().GetNum() >= 5);
    TestTrue(TEXT("Imported model has colored material sections"), ImportedMesh->GetMaterials().Num() >= 2);
    TArray<UAnimSequence*> Clips;
    for (const TCHAR* Name : {TEXT("Idle"), TEXT("Walk"), TEXT("Attack"), TEXT("Death")})
    {
        const FString Path = FString::Printf(TEXT("/Game/Mistworld/Enemies/Golem/Animations/A_Golem_%s.A_Golem_%s"), Name, Name);
        UAnimSequence* Clip = LoadObject<UAnimSequence>(nullptr, *Path);
        if (!TestNotNull(FString::Printf(TEXT("Imported golem %s clip is installed"), Name), Clip)) return false;
        TestTrue(TEXT("Imported clip shares the actual golem skeleton"), Clip->GetSkeleton() == ImportedMesh->GetSkeleton());
        TestTrue(TEXT("Imported clip contains animation time"), Clip->GetPlayLength() > 0.1f);
        Clips.Add(Clip);
    }

    FGolemPresentationWorld Fixture;
    FActorSpawnParameters Spawn;
    Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    AMistCryptGolem* Boss = Fixture.World->SpawnActor<AMistCryptGolem>(FVector(800.f, 0.f, 98.f), FRotator::ZeroRotator, Spawn);
    if (!TestNotNull(TEXT("Presentation fixture creates the real boss class"), Boss)) return false;
    Boss->DispatchBeginPlay();
    UMistGolemPresentation* BossPresentation = Boss->FindComponentByClass<UMistGolemPresentation>();
    if (!TestNotNull(TEXT("Boss BeginPlay installs its native presentation component"), BossPresentation)
        || !TestNotNull(TEXT("Boss BeginPlay constructs an imported skeletal body"), BossPresentation->GetBody())) return false;
    TestTrue(TEXT("Presentation receives component BeginPlay during registration"), BossPresentation->HasBegunPlay());
    USkeletalMeshComponent* BossBody = BossPresentation->GetBody();
    TestTrue(TEXT("Boss body uses the installed online golem"), BossBody->GetSkeletalMeshAsset() == ImportedMesh);
    TestFalse(TEXT("Imported body hides the primitive placeholder"), Boss->Visual->IsVisible());
    TestEqual(TEXT("Imported art has no gameplay collision"), BossBody->GetCollisionEnabled(), ECollisionEnabled::NoCollision);
    TestEqual(TEXT("Boss capsule remains the authored combat radius"), Boss->Capsule->GetUnscaledCapsuleRadius(), 85.f, 0.001f);
    TestTrue(TEXT("Boss body is attached to its gameplay root"), BossBody->GetAttachParent() == Boss->GetRootComponent());
    TestTrue(TEXT("Boss feet offset follows capsule height"), FMath::IsNearlyEqual(BossBody->GetRelativeLocation().Z,
        -Boss->Capsule->GetScaledCapsuleHalfHeight(), 0.001));
    PresentationTick(BossPresentation);
    UAnimSingleNodeInstance* BossAnimation = BossBody->GetSingleNodeInstance();
    if (!TestNotNull(TEXT("Boss uses a live native animation player"), BossAnimation)) return false;
    TestTrue(TEXT("Dormant boss plays its actual idle clip"), BossAnimation->GetCurrentAsset() == Clips[0]);
    TestTrue(TEXT("Boss idle animation loops"), BossAnimation->IsLooping());
    FLinearColor BossRock, BossGlow;
    TestTrue(TEXT("Boss stone has an actual color parameter"), ReadTint(BossBody, TEXT("RockColor"), BossRock));
    TestTrue(TEXT("Boss magic accents have an actual color parameter"), ReadTint(BossBody, TEXT("GlowColor"), BossGlow));
    TestTrue(TEXT("Boss defaults to blue magical accents"), BossGlow.B > BossGlow.R && BossGlow.B > BossGlow.G);
    const FBoxSphereBounds BossBounds = BossBody->CalcBounds(BossBody->GetComponentTransform());
    TestTrue(TEXT("Boss floating health text clears the imported body's head"),
        Boss->Label->GetComponentLocation().Z > BossBounds.Origin.Z + BossBounds.BoxExtent.Z + 10.f);
    TestEqual(TEXT("Boss label is aligned above its position"), Boss->Label->VerticalAlignment.GetValue(), EVRTA_TextBottom);

    TArray<AMistCryptGuardian*> Guardians;
    TArray<FLinearColor> RockColors, GlowColors;
    for (int32 Index = 0; Index < 3; ++Index)
    {
        AMistCryptGuardian* Guardian = Fixture.World->SpawnActor<AMistCryptGuardian>(FVector(0.f, Index * 300.f, 98.f), FRotator::ZeroRotator, Spawn);
        if (!TestNotNull(TEXT("Mini golem fixture creates a real guardian class"), Guardian)) return false;
        // The live encounter spawns into a begun world, then assigns its element.
        // This order reproduces that late Initialize, rather than seeding a CDO.
        Guardian->DispatchBeginPlay();
        UMistGolemPresentation* Presentation = Guardian->FindComponentByClass<UMistGolemPresentation>();
        if (!TestNotNull(TEXT("Guardian BeginPlay installs its visual presentation"), Presentation)
            || !TestNotNull(TEXT("Guardian BeginPlay creates its skeletal body"), Presentation->GetBody())) return false;
        Guardian->Initialize(Boss, nullptr, Index, 10.f);
        PresentationTick(Presentation);
        USkeletalMeshComponent* Body = Presentation->GetBody();
        TestTrue(TEXT("Every mini golem shares the boss's imported mesh"), Body->GetSkeletalMeshAsset() == ImportedMesh);
        TestTrue(TEXT("Mini golems are visibly smaller than the boss"), Body->GetRelativeScale3D().Z < BossBody->GetRelativeScale3D().Z * 0.7f);
        TestFalse(TEXT("Guardian Initialize cannot restore the old primitive shape"), Guardian->Visual->IsVisible());
        TestEqual(TEXT("Mini mesh has no gameplay collision"), Body->GetCollisionEnabled(), ECollisionEnabled::NoCollision);
        TestEqual(TEXT("Mini capsule retains its combat radius"), Guardian->Capsule->GetUnscaledCapsuleRadius(), 40.f, 0.001f);
        const FBoxSphereBounds MiniBounds = Body->CalcBounds(Body->GetComponentTransform());
        TestTrue(TEXT("Mini golem body bounds match its smaller scale"), MiniBounds.BoxExtent.Z < BossBounds.BoxExtent.Z * 0.7f);
        TestTrue(TEXT("Mini golem label floats above its imported body"),
            Guardian->Label->GetComponentLocation().Z > MiniBounds.Origin.Z + MiniBounds.BoxExtent.Z + 10.f);
        FLinearColor Rock, Glow;
        TestTrue(TEXT("Guardian stone material exposes the element color"), ReadTint(Body, TEXT("RockColor"), Rock));
        TestTrue(TEXT("Guardian magic material exposes the element glow"), ReadTint(Body, TEXT("GlowColor"), Glow));
        RockColors.Add(Rock); GlowColors.Add(Glow); Guardians.Add(Guardian);
        TestTrue(TEXT("Late elemental initialization keeps idle animation alive"), Body->GetSingleNodeInstance()
            && Body->GetSingleNodeInstance()->GetCurrentAsset() == Clips[0]);
    }
    TestTrue(TEXT("Fire mini golem uses warm red stone"), RockColors[0].R > RockColors[0].G && RockColors[0].R > RockColors[0].B);
    TestTrue(TEXT("Ice mini golem uses blue stone"), RockColors[1].B > RockColors[1].R && RockColors[1].B > RockColors[1].G);
    TestTrue(TEXT("Wind mini golem uses green jade stone"), RockColors[2].G > RockColors[2].R && RockColors[2].G > RockColors[2].B);
    TestTrue(TEXT("All three elemental glow accents are distinct"), !GlowColors[0].Equals(GlowColors[1], 0.01f)
        && !GlowColors[1].Equals(GlowColors[2], 0.01f) && !GlowColors[0].Equals(GlowColors[2], 0.01f));
    for (AMistCryptGuardian* Guardian : Guardians) Guardian->Destroy();

    Boss->RestoreEncounterComplete();
    PresentationTick(BossPresentation);
    TestTrue(TEXT("Completed boss uses the actual death clip"), BossAnimation->GetCurrentAsset() == Clips[3]);
    TestFalse(TEXT("Boss death plays once instead of looping"), BossAnimation->IsLooping());
    TestTrue(TEXT("Completed boss still exposes its glow parameter"), ReadTint(BossBody, TEXT("GlowColor"), BossGlow));
    TestTrue(TEXT("Boss death extinguishes elemental glow"), BossGlow.Equals(FLinearColor::Black, 0.001f));
    Boss->ResetEncounter(true);
    PresentationTick(BossPresentation);
    TestTrue(TEXT("Explicit replay restores the idle animation"), BossAnimation->GetCurrentAsset() == Clips[0] && BossAnimation->IsLooping());
    TestTrue(TEXT("Replay restores a living visible skeletal body"), BossBody->IsVisible() && Boss->CombatHealth->IsAlive());
    ReadTint(BossBody, TEXT("GlowColor"), BossGlow);
    TestTrue(TEXT("Replay restores magical accent colors"), BossGlow.B > BossGlow.R && BossGlow.B > BossGlow.G);
    return true;
}

#endif
