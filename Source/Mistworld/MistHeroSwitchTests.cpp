#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Tests/AutomationEditorCommon.h"
#include "Editor.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "GameFramework/Character.h"
#include "Components/SkeletalMeshComponent.h"
#include "MistworldPlayerController.h"
#include "MistCombatHealth.h"
#include "MistAnimaCombat.h"
#include "MistAnimaPresentation.h"
#include "MistTrainingDummy.h"

struct FMistHeroSwitchTestAccess
{
    static void SetCooldowns(AMistworldPlayerController* PC, double Q, double R, double Flash)
    { PC->QReadyAt = Q; PC->RReadyAt = R; PC->FlashReadyAt = Flash; }
    static void SetLoadout(AMistworldPlayerController* PC, uint8 Mask)
    { PC->bAbyssUnlocked = true; PC->EquippedAbyssMask = Mask; }
    static void Reset(AMistworldPlayerController* PC) { PC->ResetCombatSandbox(); }
    static void Aim(AMistworldPlayerController* PC) { PC->ActiveAimKey = EKeys::Q; }
    static void ClearAim(AMistworldPlayerController* PC) { PC->ClearAim(); }
    static bool CanAim(AMistworldPlayerController* PC, FKey Key) { return PC->CanAimAbility(Key); }
    static void RequestAuto(AMistworldPlayerController* PC, AActor* Target) { PC->RequestBasicAttack(Target); }
    static AActor* AutoTarget(AMistworldPlayerController* PC) { return PC->AttackTarget.Get(); }
    static void ClearAuto(AMistworldPlayerController* PC) { PC->AttackTarget.Reset(); PC->StopMovement(); }
};

namespace
{
class FMistVerifyHeroSwitch final : public IAutomationLatentCommand
{
public:
    explicit FMistVerifyHeroSwitch(FAutomationTestBase* InTest) : Test(InTest) {}
    bool Update() override
    {
        if (Started < 0.0) Started = FPlatformTime::Seconds();
        UWorld* World = GEditor ? GEditor->PlayWorld : nullptr;
        auto* PC = World ? Cast<AMistworldPlayerController>(World->GetFirstPlayerController()) : nullptr;
        auto* Hero = PC ? Cast<ACharacter>(PC->GetPawn()) : nullptr;
        auto* Health = Hero ? Hero->FindComponentByClass<UMistCombatHealth>() : nullptr;
        if (!PC || !Hero || !Health || !World->HasBegunPlay())
        {
            if (FPlatformTime::Seconds() - Started > 30.0)
            { Test->AddError(TEXT("Hero-switch test could not start the template map.")); return true; }
            return false;
        }
        FMistHeroSwitchTestAccess::Reset(PC);
        Health->EndSpawnProtection();
        Health->Health = 900.f;
        const FVector Checkpoint = Hero->GetActorLocation();
        USkeletalMesh* OriginalMesh = Hero->GetMesh()->GetSkeletalMeshAsset();
        const double Now = World->GetTimeSeconds();
        FMistHeroSwitchTestAccess::SetLoadout(PC, 5);
        FMistHeroSwitchTestAccess::SetCooldowns(PC, Now + 25.0, Now + 80.0, Now + 35.0);
        PC->ToggleLoadoutMenu();
        Test->TestTrue(TEXT("Checkpoint loadout menu can switch to Anima"), PC->TrySwitchHero(EMistHeroId::Anima));
        Test->TestTrue(TEXT("Anima is the active hero"), PC->IsAnima());
        Test->TestEqual(TEXT("Anima has her own 1000-HP baseline"), Health->MaxHealth, 1000.f);
        Test->TestFalse(TEXT("Switching does not grant spawn immunity"), Health->IsSpawnProtected());
        Test->TestFalse(TEXT("New hero does not inherit Alphonse's mixed kit"), PC->IsAbyssEquipped(EKeys::Q));
        Test->TestEqual(TEXT("Anima attacks every 0.75 seconds"), PC->GetCooldownDisplayDuration(EKeys::SpaceBar), 0.75f);
        auto* Presentation = Hero->FindComponentByClass<UMistAnimaPresentation>();
        Test->TestTrue(TEXT("Anima placeholder presentation is active"), Presentation && Presentation->IsHeroActive());
        Test->TestTrue(TEXT("Switching replaces Alphonse mesh"), Hero->GetMesh()->GetSkeletalMeshAsset() != OriginalMesh);
        PC->ToggleLoadoutMenu();
        Health->Health = 400.f;
        FMistHeroSwitchTestAccess::SetLoadout(PC, 10);
        FMistHeroSwitchTestAccess::SetCooldowns(PC, Now + 5.0, Now + 40.0, Now + 50.0);
        Test->TestTrue(TEXT("Switch back to Alphonse"), PC->TrySwitchHero(EMistHeroId::Alphonse));
        Test->TestEqual(TEXT("Alphonse keeps his injured health"), Health->Health, 900.f, 0.1f);
        Test->TestTrue(TEXT("Alphonse's independent Q cooldown is preserved"), PC->GetCooldownRemaining(EKeys::Q) > 24.f);
        Test->TestTrue(TEXT("Alphonse's independent R cooldown is preserved"), PC->GetCooldownRemaining(EKeys::R) > 79.f);
        Test->TestTrue(TEXT("Alphonse's Flash cooldown is preserved"), PC->GetCooldownRemaining(EKeys::F) > 34.f);
        Test->TestTrue(TEXT("Alphonse mixed Q is restored"), PC->IsAbyssEquipped(EKeys::Q));
        Test->TestFalse(TEXT("Alphonse original W is restored"), PC->IsAbyssEquipped(EKeys::W));
        Test->TestEqual(TEXT("Alphonse authored model is restored"), Hero->GetMesh()->GetSkeletalMeshAsset(), OriginalMesh);
        Test->TestTrue(TEXT("Return to Anima"), PC->TrySwitchHero(EMistHeroId::Anima));
        Test->TestEqual(TEXT("Anima's independent injured health is preserved"), Health->Health, 400.f, 0.1f);
        Test->TestTrue(TEXT("Anima mixed W is restored"), PC->IsAbyssEquipped(EKeys::W));
        Test->TestFalse(TEXT("Anima original Q is restored"), PC->IsAbyssEquipped(EKeys::Q));
        Test->TestTrue(TEXT("Anima Flash did not reset"), PC->GetCooldownRemaining(EKeys::F) > 49.f);
        Health->MarkCombatActivity();
        Test->TestFalse(TEXT("Combat prevents character switching"), PC->TrySwitchHero(EMistHeroId::Alphonse));
        Health->ClearCombatLock();
        Hero->SetActorLocation(Checkpoint + FVector(750.f, 0.f, 0.f), false);
        Test->TestFalse(TEXT("Leaving checkpoint prevents character switching"), PC->TrySwitchHero(EMistHeroId::Alphonse));
        Hero->SetActorLocation(Checkpoint, false);
        FMistHeroSwitchTestAccess::Aim(PC);
        Test->TestFalse(TEXT("A pending ability preview prevents switching"), PC->TrySwitchHero(EMistHeroId::Alphonse));
        FMistHeroSwitchTestAccess::ClearAim(PC);
        Health->SetRoot(2.f);
        FMistHeroSwitchTestAccess::SetLoadout(PC, 0);
        FMistHeroSwitchTestAccess::SetCooldowns(PC, 0.0, 0.0, 0.0);
        Test->TestFalse(TEXT("Root prevents Anima dash ultimate"), FMistHeroSwitchTestAccess::CanAim(PC, EKeys::R));
        FMistHeroSwitchTestAccess::SetLoadout(PC, 8);
        Test->TestTrue(TEXT("Root still permits Flamestealer's self aura"), FMistHeroSwitchTestAccess::CanAim(PC, EKeys::R));
        Test->TestFalse(TEXT("Crowd control prevents hero switching"), PC->TrySwitchHero(EMistHeroId::Alphonse));
        Health->SetRoot(0.f);
        // Target selection shares the existing Space/cursor behavior.
        FMistHeroSwitchTestAccess::Reset(PC);
        FActorSpawnParameters Params;
        Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        auto* Near = World->SpawnActor<AMistTrainingDummy>(Checkpoint + FVector(100.f, 0.f, 0.f), FRotator::ZeroRotator, Params);
        auto* Other = World->SpawnActor<AMistTrainingDummy>(Checkpoint + FVector(150.f, 30.f, 0.f), FRotator::ZeroRotator, Params);
        FMistHeroSwitchTestAccess::RequestAuto(PC, nullptr);
        Test->TestEqual(TEXT("Anima Space selects closest target"), FMistHeroSwitchTestAccess::AutoTarget(PC), static_cast<AActor*>(Near));
        FMistHeroSwitchTestAccess::RequestAuto(PC, Other);
        Test->TestEqual(TEXT("Anima cursor target overrides closest target"), FMistHeroSwitchTestAccess::AutoTarget(PC), static_cast<AActor*>(Other));
        FMistHeroSwitchTestAccess::ClearAuto(PC);
        if (Near) Near->Destroy(); if (Other) Other->Destroy();
        return true;
    }
private:
    FAutomationTestBase* Test;
    double Started = -1.0;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMistHeroSwitchTest, "Mistworld.Heroes.Switching",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FMistHeroSwitchTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(TEXT("/Game/TopDown/Lvl_TopDown")));
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(FMistVerifyHeroSwitch(this));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    return true;
}
#endif
