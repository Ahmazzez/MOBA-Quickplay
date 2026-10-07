#include "MistMeleePresentation.h"

#include "Mistworld.h"
#include "MistAlphonseAnimInstance.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Character.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
const FName SwordHand(TEXT("hand_r"));
const FName SwingSlot(TEXT("DefaultSlot"));

FString NormalizedParameterName(const FName Name)
{
    FString Result = Name.ToString().ToLower();
    Result.ReplaceInline(TEXT(" "), TEXT(""));
    Result.ReplaceInline(TEXT("_"), TEXT(""));
    return Result;
}
}

UMistMeleePresentation::UMistMeleePresentation()
{
    PrimaryComponentTick.bCanEverTick = false;

    static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
    static ConstructorHelpers::FObjectFinder<UStaticMesh> Cylinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
    static ConstructorHelpers::FObjectFinder<UStaticMesh> Sphere(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
    static ConstructorHelpers::FObjectFinder<UMaterialInterface> Material(
        TEXT("/Game/LevelPrototyping/Materials/M_FlatCol.M_FlatCol"));
    static ConstructorHelpers::FObjectFinder<UAnimSequence> Auto(
        TEXT("/Game/Mistworld/Animations/A_Sword_Auto.A_Sword_Auto"));
    static ConstructorHelpers::FObjectFinder<UAnimSequence> Cleave(
        TEXT("/Game/Mistworld/Animations/A_Sword_Cleave.A_Sword_Cleave"));

    CubeMesh = Cube.Object;
    CylinderMesh = Cylinder.Object;
    SphereMesh = Sphere.Object;
    SwordMaterial = Material.Object;
    AutoSwing = Auto.Object;
    CleaveSwing = Cleave.Object;
}

void UMistMeleePresentation::BeginPlay()
{
    Super::BeginPlay();
    InitializeSword();
}

void UMistMeleePresentation::InitializeSword()
{
    if (IsValid(SwordRoot)) return;

    const ACharacter* Character = Cast<ACharacter>(GetOwner());
    CharacterMesh = Character ? Character->GetMesh() : nullptr;
    if (!CharacterMesh || CharacterMesh->GetBoneIndex(SwordHand) == INDEX_NONE || !CubeMesh)
    {
        UE_LOG(LogMistworld, Warning, TEXT("Cannot attach placeholder sword: character mesh or hand_r bone is missing."));
        return;
    }

    AActor* Owner = GetOwner();
    SwordRoot = NewObject<USceneComponent>(Owner,
        MakeUniqueObjectName(Owner, USceneComponent::StaticClass(), TEXT("MistSwordRoot")));
    Owner->AddInstanceComponent(SwordRoot);
    CreatedComponents.Add(SwordRoot);
    SwordRoot->SetMobility(EComponentMobility::Movable);
    const FName Grip = CharacterMesh->DoesSocketExist(TEXT("MistWeaponGrip"))
        ? FName(TEXT("MistWeaponGrip")) : SwordHand;
    SwordRoot->SetupAttachment(CharacterMesh, Grip);
    SwordRoot->SetRelativeLocation(Grip == SwordHand ? FVector(0.f, 0.f, 4.f) : FVector::ZeroVector);
    SwordRoot->RegisterComponent();

    if (UsesAlphonseRig())
    {
        UStaticMesh* Weapon = LoadObject<UStaticMesh>(nullptr,
            TEXT("/Game/Mistworld/Characters/Alphonse/SM_Alphonse_Greatsword.SM_Alphonse_Greatsword"));
        if (Weapon)
        {
            Blade = NewObject<UStaticMeshComponent>(Owner, TEXT("MistAlphonseGreatsword"));
            Owner->AddInstanceComponent(Blade);
            CreatedComponents.Add(Blade);
            Blade->SetStaticMesh(Weapon);
            Blade->SetMobility(EComponentMobility::Movable);
            Blade->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            Blade->SetGenerateOverlapEvents(false);
            Blade->SetCanEverAffectNavigation(false);
            Blade->SetupAttachment(SwordRoot);
            Blade->RegisterComponent();
            bImportedSword = true;
            return;
        }
    }

    // The mannequin's right-hand local X axis points along the held weapon.
    // The blade is 100cm long, with its grip and guard behind its base.
    Blade = AddSwordPart(TEXT("MistSwordBlade"), CubeMesh,
        FVector(58.f, 0.f, 0.f), FVector(1.f, 0.035f, 0.09f), FRotator::ZeroRotator,
        FLinearColor(0.68f, 0.78f, 0.9f), 0.85f, 0.25f);
    AddSwordPart(TEXT("MistSwordGuard"), CubeMesh,
        FVector(8.f, 0.f, 0.f), FVector(0.045f, 0.24f, 0.055f), FRotator::ZeroRotator,
        FLinearColor(0.46f, 0.24f, 0.07f), 0.65f, 0.35f);
    AddSwordPart(TEXT("MistSwordGrip"), CylinderMesh,
        FVector(-2.f, 0.f, 0.f), FVector(0.055f, 0.055f, 0.18f), FRotator(90.f, 0.f, 0.f),
        FLinearColor(0.06f, 0.035f, 0.025f), 0.f, 0.8f);
    AddSwordPart(TEXT("MistSwordPommel"), SphereMesh,
        FVector(-13.f, 0.f, 0.f), FVector(0.075f), FRotator::ZeroRotator,
        FLinearColor(0.46f, 0.24f, 0.07f), 0.65f, 0.35f);
    SetAbyssAppearance(bAbyssAppearance);
}

UStaticMeshComponent* UMistMeleePresentation::AddSwordPart(const FName Name, UStaticMesh* Mesh,
    const FVector& Location, const FVector& Scale, const FRotator& Rotation,
    const FLinearColor& Color, const float Metallic, const float Roughness)
{
    if (!Mesh || !SwordRoot) return nullptr;
    AActor* Owner = GetOwner();
    UStaticMeshComponent* Part = NewObject<UStaticMeshComponent>(Owner,
        MakeUniqueObjectName(Owner, UStaticMeshComponent::StaticClass(), Name));
    Owner->AddInstanceComponent(Part);
    CreatedComponents.Add(Part);
    Part->SetMobility(EComponentMobility::Movable);
    Part->SetStaticMesh(Mesh);
    Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Part->SetGenerateOverlapEvents(false);
    Part->SetCanEverAffectNavigation(false);
    Part->SetCastShadow(false);
    Part->SetupAttachment(SwordRoot);
    Part->SetRelativeLocation(Location);
    Part->SetRelativeRotation(Rotation);
    Part->SetRelativeScale3D(Scale);
    Part->RegisterComponent();

    if (SwordMaterial)
    {
        UMaterialInstanceDynamic* Material = UMaterialInstanceDynamic::Create(SwordMaterial, Part);
        TArray<FMaterialParameterInfo> Parameters;
        TArray<FGuid> ParameterIds;
        SwordMaterial->GetAllVectorParameterInfo(Parameters, ParameterIds);
        for (const FMaterialParameterInfo& Parameter : Parameters)
        {
            const FString ParameterName = NormalizedParameterName(Parameter.Name);
            if (ParameterName == TEXT("basecolor") || ParameterName == TEXT("color"))
                Material->SetVectorParameterValue(Parameter.Name, Color);
        }
        Parameters.Reset();
        ParameterIds.Reset();
        SwordMaterial->GetAllScalarParameterInfo(Parameters, ParameterIds);
        for (const FMaterialParameterInfo& Parameter : Parameters)
        {
            const FString ParameterName = NormalizedParameterName(Parameter.Name);
            if (ParameterName == TEXT("metallic")) Material->SetScalarParameterValue(Parameter.Name, Metallic);
            if (ParameterName == TEXT("roughness")) Material->SetScalarParameterValue(Parameter.Name, Roughness);
        }
        Part->SetMaterial(0, Material);
    }
    return Part;
}

void UMistMeleePresentation::SetAbyssAppearance(const bool bAbyss)
{
    bAbyssAppearance = bAbyss;
    // The authored base weapon retains its ivory/gold appearance across
    // equipment and loadout changes. Ascension already has separate VFX.
    if (bImportedSword) return;
    if (!Blade || !SwordMaterial) return;
    UMaterialInstanceDynamic* Material = Cast<UMaterialInstanceDynamic>(Blade->GetMaterial(0));
    if (!Material) return;
    TArray<FMaterialParameterInfo> Parameters;
    TArray<FGuid> ParameterIds;
    SwordMaterial->GetAllVectorParameterInfo(Parameters, ParameterIds);
    const FLinearColor Color = bAbyss ? FLinearColor(0.48f, 0.16f, 0.78f)
        : FLinearColor(0.68f, 0.78f, 0.9f);
    for (const FMaterialParameterInfo& Parameter : Parameters)
    {
        const FString Name = NormalizedParameterName(Parameter.Name);
        if (Name == TEXT("basecolor") || Name == TEXT("color"))
            Material->SetVectorParameterValue(Parameter.Name, Color);
    }
}

void UMistMeleePresentation::PlaySwing(const bool bHeavy, const float PlayRate)
{
    InitializeSword();
    UAnimSequence* Sequence = UsesAlphonseRig()
        ? LoadAlphonseClip(bHeavy ? TEXT("Slash") : TEXT("Auto"))
        : bHeavy ? CleaveSwing.Get() : AutoSwing.Get();
    PlaySequence(Sequence, PlayRate);
}

bool UMistMeleePresentation::UsesAlphonseRig() const
{
    return CharacterMesh && CharacterMesh->GetAnimInstance()
        && CharacterMesh->GetAnimInstance()->IsA<UMistAlphonseAnimInstance>();
}

UAnimSequence* UMistMeleePresentation::LoadAlphonseClip(const TCHAR* Name) const
{
    const FString Path = FString::Printf(
        TEXT("/Game/Mistworld/Characters/Alphonse/Animations/A_Alphonse_%s.A_Alphonse_%s"), Name, Name);
    return LoadObject<UAnimSequence>(nullptr, *Path);
}

void UMistMeleePresentation::PlaySequence(UAnimSequence* Sequence, const float PlayRate)
{
    UAnimInstance* Animation = CharacterMesh ? CharacterMesh->GetAnimInstance() : nullptr;
    if (!Animation || !Sequence) return;

    CancelSwing();
    ActiveSwing = Animation->PlaySlotAnimationAsDynamicMontage(
        Sequence, SwingSlot, 0.02f, 0.06f, FMath::Clamp(PlayRate, 0.1f, 4.f), 1);
}

void UMistMeleePresentation::PlayCleave(const bool bAbyss, const float CastTime)
{
    InitializeSword();
    if (!UsesAlphonseRig()) { PlaySwing(true, 0.25f / FMath::Max(0.01f, CastTime)); return; }
    const float Contact = bAbyss ? 0.35f : 0.25f;
    PlaySequence(LoadAlphonseClip(bAbyss ? TEXT("Quake") : TEXT("Slash")),
        Contact / FMath::Max(0.01f, CastTime));
}

void UMistMeleePresentation::PlayDash(const bool bAbyss)
{
    InitializeSword();
    if (UsesAlphonseRig())
        PlaySequence(LoadAlphonseClip(bAbyss ? TEXT("AbyssDash") : TEXT("SpinDash")));
}

void UMistMeleePresentation::PlayUltimate(const bool bAbyss, const float CastTime)
{
    InitializeSword();
    if (UsesAlphonseRig())
        PlaySequence(LoadAlphonseClip(bAbyss ? TEXT("Warcry") : TEXT("Throw")),
            (bAbyss ? 0.40f : 0.35f) / FMath::Max(0.01f, CastTime));
}

void UMistMeleePresentation::PlayGuard(const bool bAbyss, const float Duration)
{
    InitializeSword();
    if (UMistAlphonseAnimInstance* Animation = CharacterMesh
        ? Cast<UMistAlphonseAnimInstance>(CharacterMesh->GetAnimInstance()) : nullptr)
        Animation->SetGuard(bAbyss, Duration);
}

void UMistMeleePresentation::PlayFlash()
{
    InitializeSword();
    if (UsesAlphonseRig()) PlaySequence(LoadAlphonseClip(TEXT("Flash")));
}

void UMistMeleePresentation::ResetPresentation()
{
    CancelSwing();
    if (UMistAlphonseAnimInstance* Animation = CharacterMesh
        ? Cast<UMistAlphonseAnimInstance>(CharacterMesh->GetAnimInstance()) : nullptr)
        Animation->ResetPresentationState();
}

void UMistMeleePresentation::CancelSwing()
{
    if (ActiveSwing)
        if (UAnimInstance* Animation = CharacterMesh ? CharacterMesh->GetAnimInstance() : nullptr)
            Animation->Montage_Stop(0.05f, ActiveSwing);
    ActiveSwing = nullptr;
}

bool UMistMeleePresentation::IsSwingActive() const
{
    const UAnimInstance* Animation = CharacterMesh ? CharacterMesh->GetAnimInstance() : nullptr;
    return ActiveSwing && Animation && Animation->Montage_IsPlaying(ActiveSwing);
}

void UMistMeleePresentation::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    CancelSwing();
    // Components belong to the character so they inherit its visibility and
    // lifetime. Remove them explicitly if only this presentation is removed.
    for (int32 Index = CreatedComponents.Num() - 1; Index >= 0; --Index)
        if (IsValid(CreatedComponents[Index])) CreatedComponents[Index]->DestroyComponent();
    CreatedComponents.Reset();
    Blade = nullptr;
    SwordRoot = nullptr;
    CharacterMesh = nullptr;
    Super::EndPlay(EndPlayReason);
}
