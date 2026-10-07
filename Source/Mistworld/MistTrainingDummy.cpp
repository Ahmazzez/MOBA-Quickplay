#include "MistTrainingDummy.h"
#include "MistCombatHealth.h"
#include "MistCryptGolem.h"
#include "MistCryptGuardian.h"
#include "MistGolemPresentation.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "UObject/ConstructorHelpers.h"
#include "Kismet/GameplayStatics.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
AMistTrainingDummy::AMistTrainingDummy()
{
    PrimaryActorTick.bCanEverTick = true;
    Capsule = CreateDefaultSubobject<UCapsuleComponent>(TEXT("Collision"));
    RootComponent = Capsule;
    Capsule->InitCapsuleSize(40.f, 96.f);
    Capsule->SetCollisionProfileName(TEXT("Pawn"));
    Capsule->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
    Capsule->SetCanEverAffectNavigation(false);
    Visual = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Placeholder"));
    Visual->SetupAttachment(Capsule);
    Visual->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Visual->SetCanEverAffectNavigation(false);
    static ConstructorHelpers::FObjectFinder<UStaticMesh> Mesh(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
    if (Mesh.Succeeded()) Visual->SetStaticMesh(Mesh.Object);
    Visual->SetRelativeScale3D(FVector(0.8f, 0.8f, 1.8f));
    CombatHealth = CreateDefaultSubobject<UMistCombatHealth>(TEXT("CombatHealth"));
    Label = CreateDefaultSubobject<UTextRenderComponent>(TEXT("HealthLabel"));
    Label->SetupAttachment(Capsule);
    Label->SetRelativeLocation(FVector(0.f, 0.f, 150.f));
    Label->SetHorizontalAlignment(EHTA_Center);
    Label->SetWorldSize(25.f);
    Label->SetTextRenderColor(FColor::Yellow);
}
void AMistTrainingDummy::BeginPlay()
{
    Super::BeginPlay();
    if (IsA<AMistCryptGolem>() || IsA<AMistCryptGuardian>())
    {
        UMistGolemPresentation* Presentation = NewObject<UMistGolemPresentation>(this, TEXT("GolemPresentation"));
        AddInstanceComponent(Presentation);
        Presentation->RegisterComponent();
    }
}
void AMistTrainingDummy::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    Label->SetText(FText::FromString(FString::Printf(TEXT("Elite dummy: %.0f / %.0f%s"), CombatHealth->Health,
        CombatHealth->MaxHealth, CombatHealth->IsAlive() ? TEXT("") : TEXT(" [DOWN]"))));
    if (APlayerController* PC = UGameplayStatics::GetPlayerController(this, 0); PC && PC->PlayerCameraManager)
        Label->SetWorldRotation((PC->PlayerCameraManager->GetCameraLocation() - Label->GetComponentLocation()).Rotation());
}
