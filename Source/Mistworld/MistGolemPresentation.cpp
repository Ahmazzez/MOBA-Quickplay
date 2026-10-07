#include "MistGolemPresentation.h"
#include "MistTrainingDummy.h"
#include "MistCryptGolem.h"
#include "MistCryptGuardian.h"
#include "MistCombatHealth.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Animation/AnimSequence.h"
#include "Engine/SkeletalMesh.h"
#include "Materials/MaterialInstanceDynamic.h"

UMistGolemPresentation::UMistGolemPresentation()
{
    PrimaryComponentTick.bCanEverTick = true;
}

void UMistGolemPresentation::BeginPlay()
{
    Super::BeginPlay();
    AMistTrainingDummy* Owner = Cast<AMistTrainingDummy>(GetOwner());
    if (!Owner || GetNetMode() == NM_DedicatedServer) return;
    USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr,
        TEXT("/Game/Mistworld/Enemies/Golem/SK_StoneGolem.SK_StoneGolem"));
    if (!Mesh) return; // Keep the existing shape when source assets are unavailable.
    Body = NewObject<USkeletalMeshComponent>(Owner, TEXT("StoneGolemBody"));
    Owner->AddInstanceComponent(Body);
    Body->SetupAttachment(Owner->GetRootComponent());
    Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Body->SetCanEverAffectNavigation(false);
    Body->SetSkeletalMeshAsset(Mesh);
    Body->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
    const bool bBoss = Owner->IsA<AMistCryptGolem>();
    const float Scale = bBoss ? 1.4f : 0.8f;
    Body->SetRelativeScale3D(FVector(Scale));
    Body->SetRelativeLocation(FVector(0.f, 0.f, -Owner->Capsule->GetScaledCapsuleHalfHeight()));
    Body->RegisterComponent();
    Body->AddTickPrerequisiteActor(Owner);
    AddTickPrerequisiteActor(Owner);
    Owner->Visual->SetVisibility(false);
    Owner->Label->SetRelativeLocation(FVector(0.f, 0.f,
        200.f * Scale - Owner->Capsule->GetScaledCapsuleHalfHeight() + 35.f));
    Owner->Label->SetVerticalAlignment(EVRTA_TextBottom);
    Owner->Label->SetWorldSize(bBoss ? 24.f : 20.f);
    for (int32 Index = 0; Index < Body->GetNumMaterials(); ++Index)
        if (UMaterialInstanceDynamic* Material = Body->CreateDynamicMaterialInstance(Index))
            Materials.Add(Material);
    Idle = LoadObject<UAnimSequence>(nullptr, TEXT("/Game/Mistworld/Enemies/Golem/Animations/A_Golem_Idle.A_Golem_Idle"));
    Walk = LoadObject<UAnimSequence>(nullptr, TEXT("/Game/Mistworld/Enemies/Golem/Animations/A_Golem_Walk.A_Golem_Walk"));
    Attack = LoadObject<UAnimSequence>(nullptr, TEXT("/Game/Mistworld/Enemies/Golem/Animations/A_Golem_Attack.A_Golem_Attack"));
    Death = LoadObject<UAnimSequence>(nullptr, TEXT("/Game/Mistworld/Enemies/Golem/Animations/A_Golem_Death.A_Golem_Death"));
    SetColors(bBoss ? -1 : 0, false, false);
    PlayState(TEXT("Idle"));
}

void UMistGolemPresentation::SetColors(int32 Element, bool bDead, bool bVulnerable)
{
    const FLinearColor RockColors[] = {
        FLinearColor(0.42f, 0.075f, 0.025f), // fire: scorched red stone
        FLinearColor(0.075f, 0.36f, 0.53f),  // ice: blue crystal stone
        FLinearColor(0.18f, 0.43f, 0.31f)    // wind: light jade stone
    };
    const FLinearColor GlowColors[] = {
        FLinearColor(1.f, 0.16f, 0.025f), FLinearColor(0.20f, 0.82f, 1.f), FLinearColor(0.40f, 1.f, 0.65f)
    };
    FLinearColor Rock = Element >= 0 ? RockColors[FMath::Clamp(Element, 0, 2)] : FLinearColor(0.19f, 0.22f, 0.28f);
    FLinearColor Glow = Element >= 0 ? GlowColors[FMath::Clamp(Element, 0, 2)] : FLinearColor(0.35f, 0.68f, 1.f);
    if (bVulnerable) Glow = FLinearColor(1.f, 0.58f, 0.10f);
    if (bDead) { Rock *= 0.32f; Glow = FLinearColor::Black; }
    for (UMaterialInstanceDynamic* Material : Materials)
    {
        Material->SetVectorParameterValue(TEXT("RockColor"), Rock);
        Material->SetVectorParameterValue(TEXT("GlowColor"), Glow);
    }
    LastElement = Element;
    bLastDead = bDead;
    bLastVulnerable = bVulnerable;
}

void UMistGolemPresentation::PlayState(const FString& State, float Duration)
{
    if (!Body || State == LastState) return;
    UAnimSequence* Sequence = Idle;
    bool bLoop = true;
    float Rate = 1.f;
    if (State == TEXT("Windup") || State == TEXT("FireWindup"))
    {
        Sequence = Attack;
        bLoop = false;
        if (Sequence) Rate = Sequence->GetPlayLength() / FMath::Max(Duration, 0.1f);
    }
    else if (State == TEXT("Charging") || State == TEXT("Returning"))
    {
        Sequence = Walk;
        Rate = State == TEXT("Charging") ? 1.7f : 1.f;
    }
    else if (State == TEXT("Down") || State == TEXT("Complete"))
    {
        Sequence = Death ? Death : Idle;
        bLoop = false;
    }
    if (Sequence) { Body->PlayAnimation(Sequence, bLoop); Body->SetPlayRate(Rate); }
    LastState = State;
}

void UMistGolemPresentation::TickComponent(float DeltaSeconds, ELevelTick TickType,
    FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaSeconds, TickType, ThisTickFunction);
    if (!Body) return;
    AMistTrainingDummy* Owner = CastChecked<AMistTrainingDummy>(GetOwner());
    int32 Element = -1;
    bool bVulnerable = false;
    FString State = TEXT("Idle");
    float Duration = 1.f;
    if (AMistCryptGuardian* Guardian = Cast<AMistCryptGuardian>(Owner))
    {
        Element = Guardian->GetElementIndex();
        State = Guardian->GetStateName();
        Duration = Guardian->GetStateRemaining();
    }
    else if (AMistCryptGolem* Boss = Cast<AMistCryptGolem>(Owner))
    {
        bVulnerable = Boss->IsVulnerable();
        State = Boss->IsEncounterComplete() ? TEXT("Complete") : Boss->GetAttackStateName();
        Duration = AMistCryptGolem::AttackWindup;
    }
    const bool bDead = !Owner->CombatHealth->IsAlive();
    if (Element != LastElement || bDead != bLastDead || bVulnerable != bLastVulnerable)
        SetColors(Element, bDead, bVulnerable);
    PlayState(State, Duration);
}
