#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MistCryptGolem.h"
#include "MistCryptGuardian.h"
#include "MistCombatHealth.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"

struct FMistCryptEncounterTestAccess
{
    static bool WardSymbolLit(const AMistCryptGolem* Golem, int32 Index)
    { return Golem->WardSymbols.IsValidIndex(Index) && Golem->WardSymbols[Index]->IsVisible(); }
    static FString BossAttackState(const AMistCryptGolem* Golem)
    {
        switch (Golem->AttackState)
        {
        case AMistCryptGolem::EAttackState::Windup: return TEXT("Windup");
        case AMistCryptGolem::EAttackState::Recovery: return TEXT("Recovery");
        default: return TEXT("Ready");
        }
    }
};

namespace
{
struct FCryptTestWorld
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    APawn* Player = nullptr;
    UMistCombatHealth* PlayerHealth = nullptr;
    AMistCryptGolem* Golem = nullptr;

    FCryptTestWorld()
    {
        if (GEngine) GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
        FloorOrWall(FVector(800.f, 0.f, -30.f), FVector(3000.f, 2500.f, 30.f));
        Player = World->SpawnActor<APawn>();
        UCapsuleComponent* Capsule = NewObject<UCapsuleComponent>(Player);
        Player->SetRootComponent(Capsule);
        Capsule->InitCapsuleSize(40.f, 96.f);
        Capsule->SetCollisionProfileName(TEXT("Pawn"));
        Capsule->SetCanEverAffectNavigation(false);
        Capsule->RegisterComponent();
        Player->SetActorLocation(FVector(-350.f, 0.f, 98.f));
        PlayerHealth = NewObject<UMistCombatHealth>(Player);
        Player->AddInstanceComponent(PlayerHealth);
        PlayerHealth->RegisterComponent();
        PlayerHealth->ResetHealth();
        FActorSpawnParameters Params;
        Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        Golem = World->SpawnActor<AMistCryptGolem>(FVector(800.f, 0.f, 98.f), FRotator::ZeroRotator, Params);
        Golem->BeginEncounter(Player);
    }
    ~FCryptTestWorld()
    {
        World->DestroyWorld(false);
        if (GEngine) GEngine->DestroyWorldContext(World);
    }
    AActor* FloorOrWall(const FVector& Position, const FVector& Extent)
    {
        AActor* Actor = World->SpawnActor<AActor>();
        UBoxComponent* Box = NewObject<UBoxComponent>(Actor);
        Actor->SetRootComponent(Box);
        Box->SetBoxExtent(Extent);
        Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
        Box->SetCollisionObjectType(ECC_WorldStatic);
        Box->SetCollisionResponseToAllChannels(ECR_Block);
        Box->SetCanEverAffectNavigation(false);
        Box->RegisterComponent();
        Actor->SetActorLocation(Position);
        return Actor;
    }
    void StepGuardian(AMistCryptGuardian* Guardian, float Delta)
    { World->TimeSeconds += Delta; Guardian->Tick(Delta); }
    AMistCryptGuardian* FirstGuardianAs(int32 Element, float InitialDelay = 1.f)
    {
        AMistCryptGuardian* Guardian = Golem->GetGuardians()[0];
        Guardian->Initialize(Golem, Player, Element, InitialDelay);
        return Guardian;
    }
    void KillGuardians()
    {
        const TArray<TObjectPtr<AMistCryptGuardian>> Wave = Golem->GetGuardians();
        for (AMistCryptGuardian* Guardian : Wave) Guardian->CombatHealth->ReceiveMagicDamage(10000.f, Player);
    }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMistCryptPhaseTest, "Mistworld.Dungeon.GolemPhases",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMistCryptPhaseTest::RunTest(const FString& Parameters)
{
    FCryptTestWorld Fixture;
    AMistCryptGolem* Boss = Fixture.Golem;
    TestTrue(TEXT("Beginning the actual golem encounter activates protection"), Boss->IsEncounterActive());
    TestEqual(TEXT("The objective creates exactly three elemental guardians"), Boss->GetRemainingGuardians(), 3);
    TestEqual(TEXT("Boss begins with full 3000 health"), Boss->CombatHealth->Health, 3000.f);
    TestEqual(TEXT("Guardians are ordinary adds rather than ultimate-refund elites"),
        Boss->GetGuardians()[0]->CombatHealth->bCountsForUltimateRefund, false);
    TestTrue(TEXT("The boss is an eligible elite-or-above refund target"), Boss->CombatHealth->bCountsForUltimateRefund);
    TestEqual(TEXT("Protected boss rejects physical damage"), Boss->CombatHealth->ReceivePhysicalDamage(10000.f, Fixture.Player), 0.f);
    TestEqual(TEXT("Protected boss rejects magic damage"), Boss->CombatHealth->ReceiveMagicDamage(10000.f, Fixture.Player), 0.f);
    TestFalse(TEXT("A second begin call cannot duplicate guardians"), Boss->BeginEncounter(Fixture.Player));
    Boss->CombatHealth->SetRoot(5.f);
    Boss->CombatHealth->SetStun(5.f);
    TestFalse(TEXT("Objective protection cannot be bypassed by root"), Boss->CombatHealth->IsRooted());
    TestFalse(TEXT("Objective protection cannot be bypassed by stun"), Boss->CombatHealth->IsStunned());

    TWeakObjectPtr<AMistCryptGuardian> OldGuard = Boss->GetGuardians()[0];
    Boss->GetGuardians()[0]->CombatHealth->ReceiveMagicDamage(10000.f, Fixture.Player);
    TestEqual(TEXT("One defeated guardian leaves two seals"), Boss->GetRemainingGuardians(), 2);
    TestFalse(TEXT("A defeated guardian extinguishes its matching visual symbol"), FMistCryptEncounterTestAccess::WardSymbolLit(Boss, 0));
    TestFalse(TEXT("One guardian cannot open the boss window"), Boss->IsVulnerable());
    Boss->NotifyGuardianDefeated(OldGuard.Get());
    TestEqual(TEXT("Repeated defeat notification cannot consume a different seal"), Boss->GetRemainingGuardians(), 2);
    Boss->GetGuardians()[1]->CombatHealth->ReceiveMagicDamage(10000.f, Fixture.Player);
    TestFalse(TEXT("Two guardians cannot open the boss window"), Boss->IsVulnerable());
    Boss->GetGuardians()[2]->CombatHealth->ReceiveMagicDamage(10000.f, Fixture.Player);
    TestTrue(TEXT("All three actual guardian deaths remove boss protection immediately"), Boss->IsVulnerable());
    TestEqual(TEXT("The damage window lasts fifteen seconds"), Boss->GetVulnerabilityRemaining(), 15.f, 0.001f);
    TestEqual(TEXT("Vulnerable boss damage uses its ordinary 20 armor"), Boss->CombatHealth->ReceivePhysicalDamage(120.f, Fixture.Player), 100.f, 0.001f);
    TestFalse(TEXT("No guardian ward reduction is inherited by the vulnerable boss"), Boss->CombatHealth->GetDamageReduction() > 0.f);
    Fixture.World->TimeSeconds = 14.99;
    Boss->Tick(0.f);
    TestTrue(TEXT("Boss remains vulnerable until the full fifteen seconds elapse"), Boss->IsVulnerable());
    Fixture.World->TimeSeconds = 15.0;
    Boss->Tick(0.f);
    TestFalse(TEXT("Vulnerability expires exactly on its deadline"), Boss->IsVulnerable());
    TestEqual(TEXT("Surviving boss creates a fresh three-guardian wave"), Boss->GetRemainingGuardians(), 3);
    TestTrue(TEXT("Old dead guardians are destroyed when the next wave begins"), !OldGuard.IsValid() || OldGuard->IsActorBeingDestroyed());
    TestEqual(TEXT("Guardian waves preserve damage already dealt to the boss"), Boss->CombatHealth->Health, 2900.f, 0.001f);
    TestEqual(TEXT("Fresh protection immediately prevents more boss damage"), Boss->CombatHealth->ReceivePhysicalDamage(120.f, Fixture.Player), 0.f);

    int32 CompletionEvents = 0;
    Boss->OnEncounterCompleted.AddLambda([&]() { ++CompletionEvents; });
    Fixture.KillGuardians();
    Boss->CombatHealth->ReceivePhysicalDamage(10000.f, Fixture.Player);
    TestTrue(TEXT("Lethal damage in the window completes the encounter"), Boss->IsEncounterComplete());
    TestFalse(TEXT("A completed encounter is no longer active"), Boss->IsEncounterActive());
    TestEqual(TEXT("Boss defeat broadcasts completion once"), CompletionEvents, 1);
    TestEqual(TEXT("Completed encounter owns no remaining guardians"), Boss->GetGuardians().Num(), 0);
    Boss->CombatHealth->ReceivePhysicalDamage(10000.f, Fixture.Player);
    Boss->ResetEncounter();
    TestEqual(TEXT("Further damage or reset cannot duplicate completion"), CompletionEvents, 1);
    TestTrue(TEXT("Reset preserves an earned completion"), Boss->IsEncounterComplete());
    TestFalse(TEXT("A completed encounter cannot restart"), Boss->BeginEncounter(Fixture.Player));

    FCryptTestWorld ResetFixture;
    AMistCryptGolem* ResetBoss = ResetFixture.Golem;
    TWeakObjectPtr<AMistCryptGuardian> ResetGuard = ResetBoss->GetGuardians()[0];
    ResetFixture.KillGuardians();
    ResetBoss->CombatHealth->ReceivePhysicalDamage(120.f, ResetFixture.Player);
    ResetBoss->ResetEncounter();
    TestEqual(TEXT("Wipe returns an unfinished boss to Dormant"), ResetBoss->GetStateName(), FString(TEXT("Dormant")));
    TestEqual(TEXT("Wipe restores full boss health"), ResetBoss->CombatHealth->Health, 3000.f);
    TestTrue(TEXT("Wipe destroys all old guardians"), !ResetGuard.IsValid() || ResetGuard->IsActorBeingDestroyed());
    TestEqual(TEXT("Dormant encounter contains no stale wave"), ResetBoss->GetGuardians().Num(), 0);
    TestTrue(TEXT("A fresh attempt can begin after the reset"), ResetBoss->BeginEncounter(ResetFixture.Player));
    TestEqual(TEXT("A fresh attempt restores exactly three seals"), ResetBoss->GetRemainingGuardians(), 3);
    int32 RestoreEvents = 0;
    ResetBoss->OnEncounterCompleted.AddLambda([&]() { ++RestoreEvents; });
    ResetBoss->RestoreEncounterComplete();
    TestTrue(TEXT("A saved clear can be restored"), ResetBoss->IsEncounterComplete());
    TestEqual(TEXT("Restoring a saved clear never emits a new reward event"), RestoreEvents, 0);
    TestEqual(TEXT("Saved completion removes the active wave"), ResetBoss->GetGuardians().Num(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMistCryptChargeTest, "Mistworld.Dungeon.GuardianCharge",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMistCryptChargeTest::RunTest(const FString& Parameters)
{
    FCryptTestWorld Fixture;
    // Ice retains the baseline charge contract. Fire's area attack and Wind's quicker profile have separate tests.
    AMistCryptGuardian* Guardian = Fixture.FirstGuardianAs(1);
    const FVector Home = Guardian->GetActorLocation();
    TestTrue(TEXT("Guardian begins inside the protective circle"), Guardian->IsWardProtected());
    TestEqual(TEXT("Inside ward: 120 raw through 20 armor and 75% reduction loses only 25 HP"),
        Guardian->CombatHealth->ReceivePhysicalDamage(120.f, Fixture.Player), 25.f, 0.001f);
    Guardian->CombatHealth->SetRoot(5.f);
    Guardian->CombatHealth->SetStun(5.f);
    TestFalse(TEXT("Guardian cannot be rooted instead of solving the charge"), Guardian->CombatHealth->IsRooted());
    TestFalse(TEXT("Guardian cannot be stunned to bypass its mechanic"), Guardian->CombatHealth->IsStunned());
    Fixture.PlayerHealth->GrantSpawnProtection(3.f);
    Fixture.StepGuardian(Guardian, 1.f);
    TestEqual(TEXT("An immune player cannot trigger retaliation or charge procs"), Guardian->GetStateName(), FString(TEXT("Waiting")));
    Fixture.PlayerHealth->EndSpawnProtection();
    Guardian->Tick(0.f);
    TestEqual(TEXT("A visible unprotected target begins a learnable charge tell"), Guardian->GetStateName(), FString(TEXT("Windup")));
    const FVector LockedDirection = Guardian->GetCommittedDirection();
    TestTrue(TEXT("The charge is aimed out toward the baiting player"), LockedDirection.Equals(-FVector::ForwardVector, 0.001f));
    Fixture.Player->SetActorLocation(FVector(-350.f, 140.f, 98.f));
    Fixture.StepGuardian(Guardian, 1.19f);
    TestEqual(TEXT("The full 1.2-second windup deals no early damage"), Fixture.PlayerHealth->Health, 1200.f);
    TestTrue(TEXT("Windup aim remains committed after the player sidesteps"), Guardian->GetCommittedDirection().Equals(LockedDirection, 0.001f));
    TestTrue(TEXT("Guardian remains stationary throughout the tell"), Guardian->GetActorLocation().Equals(Home, 0.001f));
    Fixture.StepGuardian(Guardian, 0.02f);
    TestEqual(TEXT("Charge releases after the telegraph deadline"), Guardian->GetStateName(), FString(TEXT("Charging")));
    Fixture.StepGuardian(Guardian, 1.f);
    TestEqual(TEXT("A completed charge leaves a long attack opportunity"), Guardian->GetStateName(), FString(TEXT("Recovery")));
    TestEqual(TEXT("Charge travels the configured 850 centimeters"), FVector::Dist2D(Home, Guardian->GetActorLocation()), 850.0, 0.1);
    TestFalse(TEXT("Baited charge moves the guardian outside the actual 700cm ward"), Guardian->IsWardProtected());
    TestEqual(TEXT("Dodging the committed lane avoids the actual swept hit"), Fixture.PlayerHealth->Health, 1200.f);
    TestEqual(TEXT("Outside ward: the same 120 raw now loses 100 HP"),
        Guardian->CombatHealth->ReceivePhysicalDamage(120.f, Fixture.Player), 100.f, 0.001f);
    const FVector ExposedPosition = Guardian->GetActorLocation();
    Fixture.StepGuardian(Guardian, 5.99f);
    TestEqual(TEXT("Guardian stays exposed for the full six seconds"), Guardian->GetStateName(), FString(TEXT("Recovery")));
    TestTrue(TEXT("Recovery does not walk the guardian back into protection early"), Guardian->GetActorLocation().Equals(ExposedPosition, 0.01f));
    Fixture.StepGuardian(Guardian, 0.02f);
    TestEqual(TEXT("After six seconds the guardian starts returning"), Guardian->GetStateName(), FString(TEXT("Returning")));
    Fixture.StepGuardian(Guardian, 3.f);
    Guardian->Tick(0.f);
    TestTrue(TEXT("Return reaches the exact original guardian home"), Guardian->GetActorLocation().Equals(Home, 0.1f));
    TestTrue(TEXT("Returning inside re-applies protection"), Guardian->IsWardProtected());
    TestEqual(TEXT("The ward again reduces damage without restoring lost health"),
        Guardian->CombatHealth->ReceiveMagicDamage(120.f, Fixture.Player), 25.f, 0.001f);

    FCryptTestWorld HitFixture;
    AMistCryptGuardian* HitGuardian = HitFixture.FirstGuardianAs(1);
    HitFixture.StepGuardian(HitGuardian, 1.f);
    HitFixture.StepGuardian(HitGuardian, 1.21f);
    HitFixture.StepGuardian(HitGuardian, 1.f);
    TestEqual(TEXT("Remaining in the lane takes the 120-raw attack once after armor"),
        HitFixture.PlayerHealth->Health, 1100.f, 0.001f);
    HitGuardian->Tick(0.1f);
    TestEqual(TEXT("Recovery cannot repeatedly damage the same target"), HitFixture.PlayerHealth->Health, 1100.f);

    FCryptTestWorld WallFixture;
    AMistCryptGuardian* WallGuardian = WallFixture.FirstGuardianAs(1);
    WallFixture.StepGuardian(WallGuardian, 1.f);
    WallFixture.StepGuardian(WallGuardian, 1.21f);
    WallFixture.FloorOrWall(FVector(100.f, 0.f, 98.f), FVector(10.f, 500.f, 150.f));
    WallFixture.StepGuardian(WallGuardian, 1.f);
    TestTrue(TEXT("A charging guardian stops on the near side of real blocking terrain"), WallGuardian->GetActorLocation().X >= 174.f);
    TestEqual(TEXT("A player behind the wall receives no charge damage"), WallFixture.PlayerHealth->Health, 1200.f);
    TestEqual(TEXT("Wall contact still puts the guardian into recovery"), WallGuardian->GetStateName(), FString(TEXT("Recovery")));

    FCryptTestWorld BossFixture;
    BossFixture.KillGuardians();
    BossFixture.Player->SetActorLocation(FVector(600.f, 0.f, 98.f));
    BossFixture.Golem->Tick(0.f);
    TestEqual(TEXT("Vulnerable boss begins its slow melee telegraph"),
        FMistCryptEncounterTestAccess::BossAttackState(BossFixture.Golem), FString(TEXT("Windup")));
    const FVector BossAim = BossFixture.Golem->GetCommittedDirection();
    BossFixture.Player->SetActorLocation(FVector(800.f, 220.f, 98.f));
    TestFalse(TEXT("A lateral step leaves the real committed boss cone"), BossFixture.Golem->IsTargetInsideAttack(BossFixture.Player));
    BossFixture.World->TimeSeconds += 1.11f;
    BossFixture.Golem->Tick(1.11f);
    TestTrue(TEXT("Boss aim stays locked throughout the dodge opportunity"), BossFixture.Golem->GetCommittedDirection().Equals(BossAim, 0.001f));
    TestEqual(TEXT("Dodging the boss cone prevents damage"), BossFixture.PlayerHealth->Health, 1200.f);
    TestEqual(TEXT("Boss attack enters its 1.8-second recovery"),
        FMistCryptEncounterTestAccess::BossAttackState(BossFixture.Golem), FString(TEXT("Recovery")));
    BossFixture.World->TimeSeconds += 1.81f;
    BossFixture.Player->SetActorLocation(FVector(600.f, 0.f, 98.f));
    BossFixture.Golem->Tick(1.81f);
    BossFixture.Golem->Tick(0.f);
    BossFixture.World->TimeSeconds += 1.11f;
    BossFixture.Golem->Tick(1.11f);
    TestEqual(TEXT("Remaining in the boss cone takes its 180 raw through armor"), BossFixture.PlayerHealth->Health, 1050.f, 0.001f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMistCryptFireAreaTest, "Mistworld.Dungeon.GuardianFireArea",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMistCryptFireAreaTest::RunTest(const FString& Parameters)
{
    FCryptTestWorld Fixture;
    AMistCryptGuardian* Fire = Fixture.Golem->GetGuardians()[0];
    TestEqual(TEXT("The first seal is a fire guardian"), Fire->GetElementName(), FString(TEXT("Fire [I]")));
    TestEqual(TEXT("Fire trades speed for a sturdier health pool"), Fire->CombatHealth->MaxHealth, 360.f);
    Fixture.StepGuardian(Fire, 1.f);
    TestEqual(TEXT("Fire announces its baitable charge before any attack"), Fire->GetStateName(), FString(TEXT("Windup")));
    Fixture.Player->SetActorLocation(FVector(-350.f, 350.f, 98.f));
    Fixture.StepGuardian(Fire, 1.41f);
    Fixture.StepGuardian(Fire, 1.3f);
    TestEqual(TEXT("Fire follows its charge with a separate area tell"), Fire->GetStateName(), FString(TEXT("FireWindup")));
    TestFalse(TEXT("The baited landing is outside the protective ward"), Fire->IsWardProtected());
    TestEqual(TEXT("Sidestepping the charge avoids contact damage"), Fixture.PlayerHealth->Health, 1200.f);
    const FVector LockedBlast = Fire->GetFireAreaCenter();
    Fixture.Player->SetActorLocation(LockedBlast);
    Fixture.StepGuardian(Fire, 0.99f);
    TestEqual(TEXT("The orange circle grants the full one-second dodge opportunity"), Fixture.PlayerHealth->Health, 1200.f);
    TestTrue(TEXT("Fire blast location never follows the player"), Fire->GetFireAreaCenter().Equals(LockedBlast, 0.001f));
    Fixture.StepGuardian(Fire, 0.02f);
    TestEqual(TEXT("Standing in the committed area takes 150 magic damage through resistance"),
        Fixture.PlayerHealth->Health, 1075.f, 0.001f);
    TestEqual(TEXT("Fire enters an exposed recovery after the eruption"), Fire->GetStateName(), FString(TEXT("Recovery")));
    TestEqual(TEXT("The visible burning patch lasts four seconds"), Fire->GetFireAreaRemaining(), 4.f, 0.001f);
    Fixture.StepGuardian(Fire, 0.79f);
    TestEqual(TEXT("Burning damage cannot tick earlier than its visible 0.8-second cadence"), Fixture.PlayerHealth->Health, 1075.f, 0.001f);
    Fixture.StepGuardian(Fire, 0.02f);
    TestEqual(TEXT("The first lingering fire pulse deals 35 magic damage"), Fixture.PlayerHealth->Health, 1075.f - 35.f / 1.2f, 0.001f);
    const float AfterFirstPulse = Fixture.PlayerHealth->Health;
    Fixture.Player->SetActorLocation(LockedBlast + FVector(0.f, Fire->FireAreaRadius + 45.f, 0.f));
    Fixture.StepGuardian(Fire, 0.81f);
    TestEqual(TEXT("Leaving the fire patch immediately prevents subsequent area pulses"), Fixture.PlayerHealth->Health, AfterFirstPulse);

    Fixture.Player->SetActorLocation(LockedBlast + FVector(0.f, 220.f, 0.f));
    Fixture.FloorOrWall(LockedBlast + FVector(0.f, 110.f, 0.f), FVector(500.f, 5.f, 150.f));
    Fixture.StepGuardian(Fire, 0.81f);
    TestEqual(TEXT("The lingering area cannot damage a player through solid terrain"), Fixture.PlayerHealth->Health, AfterFirstPulse);
    Fixture.StepGuardian(Fire, 1.6f);
    TestEqual(TEXT("Fire damage expires at the end of its four-second duration"), Fire->GetFireAreaRemaining(), 0.f);
    Fire->CombatHealth->ReceiveMagicDamage(10000.f, Fixture.Player);
    TestEqual(TEXT("A defeated fire guardian cannot keep an invisible damaging patch"), Fire->GetFireAreaRemaining(), 0.f);

    FCryptTestWorld DodgeFixture;
    AMistCryptGuardian* DodgedFire = DodgeFixture.Golem->GetGuardians()[0];
    DodgeFixture.StepGuardian(DodgedFire, 1.f);
    DodgeFixture.Player->SetActorLocation(FVector(-350.f, 350.f, 98.f));
    DodgeFixture.StepGuardian(DodgedFire, 1.41f);
    DodgeFixture.StepGuardian(DodgedFire, 1.3f);
    DodgeFixture.Player->SetActorLocation(DodgedFire->GetFireAreaCenter() + FVector(0.f, 350.f, 0.f));
    DodgeFixture.StepGuardian(DodgedFire, 1.01f);
    DodgeFixture.StepGuardian(DodgedFire, 4.1f);
    TestEqual(TEXT("A player who leaves the outlined blast avoids both eruption and pool damage"), DodgeFixture.PlayerHealth->Health, 1200.f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMistCryptIceFreezeTest, "Mistworld.Dungeon.GuardianIceFreeze",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMistCryptIceFreezeTest::RunTest(const FString& Parameters)
{
    FCryptTestWorld HitFixture;
    AMistCryptGuardian* Ice = HitFixture.FirstGuardianAs(1, 0.f);
    TestEqual(TEXT("The second elemental profile is ice"), Ice->GetElementName(), FString(TEXT("Ice [II]")));
    Ice->Tick(0.f);
    HitFixture.StepGuardian(Ice, 1.21f);
    HitFixture.StepGuardian(Ice, 1.f);
    TestEqual(TEXT("Ice charge contact still applies its physical hit once"), HitFixture.PlayerHealth->Health, 1100.f);
    TestTrue(TEXT("An ice charge hit freezes the player using the shared stun rules"), HitFixture.PlayerHealth->IsStunned());
    HitFixture.World->TimeSeconds += 1.24f;
    TestTrue(TEXT("Freeze persists for its full 1.25 seconds"), HitFixture.PlayerHealth->IsStunned());
    HitFixture.World->TimeSeconds += 0.02f;
    TestFalse(TEXT("Freeze clears automatically after 1.25 seconds"), HitFixture.PlayerHealth->IsStunned());

    FCryptTestWorld ImmuneFixture;
    AMistCryptGuardian* ImmuneIce = ImmuneFixture.FirstGuardianAs(1, 0.f);
    ImmuneIce->Tick(0.f);
    ImmuneFixture.StepGuardian(ImmuneIce, 1.21f);
    ImmuneFixture.PlayerHealth->GrantSpawnProtection(3.f);
    ImmuneFixture.StepGuardian(ImmuneIce, 1.f);
    TestEqual(TEXT("Spawn protection rejects ice contact damage"), ImmuneFixture.PlayerHealth->Health, 1200.f);
    TestFalse(TEXT("Spawn protection also rejects freeze"), ImmuneFixture.PlayerHealth->IsStunned());

    FCryptTestWorld DebuffFixture;
    AMistCryptGuardian* DebuffIce = DebuffFixture.FirstGuardianAs(1, 0.f);
    DebuffIce->Tick(0.f);
    DebuffFixture.StepGuardian(DebuffIce, 1.21f);
    DebuffFixture.PlayerHealth->GrantDebuffImmunity(10.f);
    DebuffFixture.StepGuardian(DebuffIce, 1.f);
    TestEqual(TEXT("Debuff immunity alone does not remove the ordinary damage hit"), DebuffFixture.PlayerHealth->Health, 1100.f);
    TestFalse(TEXT("An existing character immunity prevents ice freeze"), DebuffFixture.PlayerHealth->IsStunned());

    FCryptTestWorld ShieldFixture;
    AMistCryptGuardian* ShieldIce = ShieldFixture.FirstGuardianAs(1, 0.f);
    ShieldFixture.PlayerHealth->SetShield(500.f, 10.f);
    ShieldIce->Tick(0.f);
    ShieldFixture.StepGuardian(ShieldIce, 1.21f);
    ShieldFixture.StepGuardian(ShieldIce, 1.f);
    TestEqual(TEXT("An ordinary shield absorbs damage"), ShieldFixture.PlayerHealth->Health, 1200.f);
    TestTrue(TEXT("A shield is not crowd-control immunity: ice still freezes on contact"), ShieldFixture.PlayerHealth->IsStunned());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMistCryptWindProfileTest, "Mistworld.Dungeon.GuardianWindProfile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMistCryptWindProfileTest::RunTest(const FString& Parameters)
{
    FCryptTestWorld Fixture;
    AMistCryptGuardian* Wind = Fixture.Golem->GetGuardians()[2];
    Wind->Initialize(Fixture.Golem, Fixture.Player, 2, 0.f);
    const FVector Home = Wind->GetHomeLocation();
    const FVector BaitDirection = (Home - Fixture.Golem->GetHomeLocation()).GetSafeNormal2D();
    Fixture.Player->SetActorLocation(Home + BaitDirection * 950.f);
    TestEqual(TEXT("The third elemental profile is wind"), Wind->GetElementName(), FString(TEXT("Wind [III]")));
    TestEqual(TEXT("Wind is considerably frailer than ice and fire"), Wind->CombatHealth->MaxHealth, 210.f);
    TestEqual(TEXT("Wind's exposed body has no armor"), Wind->CombatHealth->Armor, 0.f);
    TestEqual(TEXT("Wind's exposed body has no magic resistance"), Wind->CombatHealth->MagicResistance, 0.f);
    Wind->Tick(0.f);
    Fixture.Player->SetActorLocation(Home + BaitDirection * 950.f + BaitDirection.RotateAngleAxis(90.f, FVector::UpVector) * 200.f);
    Fixture.StepGuardian(Wind, 0.79f);
    TestEqual(TEXT("The swift guardian still provides a readable full 0.8-second committed tell"), Wind->GetStateName(), FString(TEXT("Windup")));
    TestTrue(TEXT("Even the fast guardian remains stationary while telegraphing"), Wind->GetActorLocation().Equals(Home, 0.001f));
    Fixture.StepGuardian(Wind, 0.02f);
    Fixture.StepGuardian(Wind, 0.45f);
    TestEqual(TEXT("Wind actually moves at 1450 cm/s rather than only using a different label"),
        FVector::Dist2D(Home, Wind->GetActorLocation()), 652.5, 0.1);
    Fixture.StepGuardian(Wind, 0.4f);
    TestEqual(TEXT("Wind commits to a longer 1000cm charge"), FVector::Dist2D(Home, Wind->GetActorLocation()), 1000.0, 0.1);
    TestFalse(TEXT("Wind's long charge is still baitable out of the ward"), Wind->IsWardProtected());
    TestEqual(TEXT("Sidestepping the wind lane prevents its damage"), Fixture.PlayerHealth->Health, 1200.f);
    TestFalse(TEXT("The wind element cannot apply the ice freeze effect"), Fixture.PlayerHealth->IsStunned());
    TestEqual(TEXT("Wind's lower defense makes exposed attacks hit for full raw damage"),
        Wind->CombatHealth->ReceivePhysicalDamage(120.f, Fixture.Player), 120.f, 0.001f);
    Fixture.StepGuardian(Wind, 3.99f);
    TestEqual(TEXT("Wind retains a four-second punish window despite its fast movement"), Wind->GetStateName(), FString(TEXT("Recovery")));
    Fixture.StepGuardian(Wind, 0.02f);
    TestEqual(TEXT("Wind returns after its recovery"), Wind->GetStateName(), FString(TEXT("Returning")));
    const FVector ReturnStart = Wind->GetActorLocation();
    Fixture.StepGuardian(Wind, 0.5f);
    TestEqual(TEXT("Wind's return is also faster at 600cm/s"), FVector::Dist2D(ReturnStart, Wind->GetActorLocation()), 300.0, 0.1);
    return true;
}

#endif
