#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Tests/AutomationEditorCommon.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "HAL/PlatformTime.h"
#include "MistworldPlayerController.h"
#include "MistCombatHUD.h"
#include "MistCombatHealth.h"
#include "MistTrainingDummy.h"

namespace
{
// Run the real template map through normal PIE startup. A short settle period
// catches a custom HUD that is created and then replaced during player login.
class FMistWaitForCombatStartup final : public IAutomationLatentCommand
{
public:
    explicit FMistWaitForCombatStartup(FAutomationTestBase* InTest)
        : Test(InTest)
    {
    }

    virtual bool Update() override
    {
        const double Now = FPlatformTime::Seconds();
        if (StartedAt < 0.0)
        {
            StartedAt = Now;
        }

        UWorld* World = GEditor ? GEditor->PlayWorld : nullptr;
        APlayerController* Controller = World ? World->GetFirstPlayerController() : nullptr;
        APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
        const bool bStarted = World && World->HasBegunPlay() && Controller && Pawn;
        if (bStarted && SettlingAt < 0.0)
        {
            SettlingAt = Now;
        }

        if (bStarted && Now - SettlingAt >= 1.0)
        {
            const FString ControllerClass = Controller->GetClass()->GetPathName();
            Test->TestTrue(FString::Printf(TEXT("Template map uses combat controller (actual: %s)"), *ControllerClass),
                Controller->IsA<AMistworldPlayerController>());
            Test->TestNotNull(TEXT("Possessed pawn has combat health"), Pawn->FindComponentByClass<UMistCombatHealth>());

            AHUD* HUD = Controller->GetHUD();
            const FString HUDClass = HUD ? HUD->GetClass()->GetPathName() : TEXT("null");
            Test->TestTrue(FString::Printf(TEXT("Combat HUD survives normal player initialization (actual: %s)"), *HUDClass),
                HUD && HUD->IsA<AMistCombatHUD>());

            // Exercise a runtime-created target, rather than only class defaults.
            FActorSpawnParameters SpawnParameters;
            SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
            AMistTrainingDummy* Dummy = World->SpawnActor<AMistTrainingDummy>(
                Pawn->GetActorLocation() + FVector(500.f, 0.f, 100.f), FRotator::ZeroRotator, SpawnParameters);
            if (Test->TestNotNull(TEXT("Training dummy spawns in PIE world"), Dummy))
            {
                UMistCombatHealth* Health = Dummy->CombatHealth;
                if (Test->TestNotNull(TEXT("Training dummy has combat health"), Health))
                {
                    Health->MaxHealth = 1200.f;
                    Health->Armor = 20.f;
                    Health->ResetHealth();
                    Test->TestEqual(TEXT("90 physical damage against 20 armor deals 75"),
                        Health->ReceivePhysicalDamage(90.f), 75.f, 0.01f);
                    Test->TestEqual(TEXT("Damaged dummy has 1125 health"), Health->Health, 1125.f, 0.01f);
                    Health->ResetHealth();
                    Test->TestEqual(TEXT("Dummy reset restores full health"), Health->Health, 1200.f, 0.01f);
                }
                Dummy->Destroy();
            }
            return true;
        }

        if (Now - StartedAt >= 30.0)
        {
            Test->AddError(TEXT("Timed out after 30 seconds waiting for template map PIE startup and a possessed player pawn."));
            return true;
        }
        return false;
    }

private:
    FAutomationTestBase* Test;
    double StartedAt = -1.0;
    double SettlingAt = -1.0;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMistCombatStartupTest, "Mistworld.Combat.Startup",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMistCombatStartupTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(TEXT("/Game/TopDown/Lvl_TopDown")));
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(FMistWaitForCombatStartup(this));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    return true;
}

#endif
