#pragma once
#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "MistEquipmentSlots.h"
#include "MistCombatHUD.generated.h"
UCLASS()
class MISTWORLD_API AMistCombatHUD : public AHUD
{
    GENERATED_BODY()
public:
    virtual void DrawHUD() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void NotifyHitBoxClick(FName BoxName) override;
    virtual void NotifyHitBoxRelease(FName BoxName) override;
private:
    void DrawCombatDock(class AMistworldPlayerController* PC);
    void DrawQuestCard(class AMistworldPlayerController* PC);
    void DrawLoadoutMenu(class AMistworldPlayerController* PC);
    void DrawInventoryMenu(class AMistworldPlayerController* PC);
    void UpdateInventoryPreview(class AMistworldPlayerController* PC);
    UPROPERTY(Transient) TObjectPtr<class AMistInventoryPreview> InventoryPreview;
    EMistEquipmentSlot SelectedEquipmentSlot = EMistEquipmentSlot::GreatswordBlade;
    double NextPreviewUpdateAt = 0.0;
};
