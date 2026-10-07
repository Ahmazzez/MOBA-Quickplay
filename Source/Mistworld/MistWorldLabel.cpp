#include "MistWorldLabel.h"

#include "Components/TextRenderComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"

AMistWorldLabel::AMistWorldLabel()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickGroup = TG_PostUpdateWork;
    UTextRenderComponent* Text = GetTextRender();
    Text->SetMobility(EComponentMobility::Movable);
    Text->SetHorizontalAlignment(EHTA_Center);
    Text->SetVerticalAlignment(EVRTA_TextBottom);
    Text->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Text->SetCanEverAffectNavigation(false);
    Text->SetCastShadow(false);
}

FRotator AMistWorldLabel::CameraFacingRotation(const FVector& Position, const FVector& CameraPosition)
{
    const FVector ToCamera = CameraPosition - Position;
    if (ToCamera.IsNearlyZero()) return FRotator::ZeroRotator;
    FRotator Rotation = ToCamera.Rotation();
    Rotation.Roll = 0.f;
    return Rotation;
}

void AMistWorldLabel::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    const APlayerController* PC = UGameplayStatics::GetPlayerController(this, 0);
    if (PC && PC->IsLocalController() && PC->PlayerCameraManager)
        GetTextRender()->SetWorldRotation(CameraFacingRotation(
            GetTextRender()->GetComponentLocation(), PC->PlayerCameraManager->GetCameraLocation()));
}
