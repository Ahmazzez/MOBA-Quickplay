#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MistReturningSword.h"
#include "MistCombatHealth.h"
#include "MistTrainingDummy.h"
#include "CollisionQueryParams.h"
#include "CollisionShape.h"
#include "Components/BoxComponent.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"

namespace
{
struct FMistSwordTestWorld
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    ACharacter* Caster = nullptr;
    UMistCombatHealth* Health = nullptr;

    FMistSwordTestWorld()
    {
        FActorSpawnParameters Params;
        Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        Caster = World->SpawnActor<ACharacter>(FVector(0.f, 0.f, 200.f), FRotator::ZeroRotator, Params);
        Health = NewObject<UMistCombatHealth>(Caster);
        Caster->AddInstanceComponent(Health);
        Health->RegisterComponent();
        Health->Health = 500.f;
    }

    ~FMistSwordTestWorld()
    {
        World->DestroyWorld(false);
    }

    AMistTrainingDummy* Dummy(const FVector& Position)
    {
        FActorSpawnParameters Params;
        Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        return World->SpawnActor<AMistTrainingDummy>(Position, FRotator::ZeroRotator, Params);
    }

    AMistReturningSword* Sword(const FVector& Direction = FVector::ForwardVector)
    {
        AMistReturningSword* Result = World->SpawnActor<AMistReturningSword>();
        Result->Initialize(Caster, Direction);
        return Result;
    }

    void Wall(float X)
    {
        AActor* Actor = World->SpawnActor<AActor>();
        UBoxComponent* Box = NewObject<UBoxComponent>(Actor);
        Actor->SetRootComponent(Box);
        Box->SetBoxExtent(FVector(10.f, 500.f, 300.f));
        Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
        Box->SetCollisionObjectType(ECC_WorldStatic);
        Box->SetCollisionResponseToAllChannels(ECR_Block);
        Box->RegisterComponent();
        Actor->SetActorLocation(FVector(X, 0.f, 200.f));
    }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMistReturningSwordTest, "Mistworld.Combat.ReturningSword",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMistReturningSwordTest::RunTest(const FString& Parameters)
{
    {
        FMistSwordTestWorld Fixture;
        // The center lies outside the 60 cm projectile radius. Its capsule
        // edge is inside, and a large frame must still sweep through it.
        AMistTrainingDummy* Target = Fixture.Dummy(FVector(400.f, 85.f, 200.f));
        AMistReturningSword* Sword = Fixture.Sword();
        bool bReturned = false;
        Sword->OnReturned.BindLambda([&bReturned]() { bReturned = true; });
        Sword->Tick(0.3f);
        TestEqual(TEXT("Swept sword includes the target capsule edge"), Target->CombatHealth->Health, 1066.6666f, 0.02f);
        TestEqual(TEXT("Each outbound hit heals 20 percent of actual mitigated damage"), Fixture.Health->Health, 526.6666f, 0.02f);
        Sword->Tick(0.1f);
        TestEqual(TEXT("The same outbound target is not hit twice"), Target->CombatHealth->Health, 1066.6666f, 0.02f);
        Sword->Tick(0.5f);
        TestEqual(TEXT("The returning leg can hit the same target once"), Target->CombatHealth->Health, 933.3333f, 0.03f);
        TestEqual(TEXT("Both legs heal using mitigated damage"), Fixture.Health->Health, 553.3333f, 0.03f);
        TestEqual(TEXT("One outbound hit is recorded"), Sword->GetHitsOnLeg(false), 1);
        TestEqual(TEXT("One return hit is recorded"), Sword->GetHitsOnLeg(true), 1);
        Sword->Tick(0.5f);
        TestTrue(TEXT("Catching the sword opens the return callback"), bReturned);
    }

    {
        FMistSwordTestWorld Fixture;
        AMistReturningSword* Sword = Fixture.Sword();
        Sword->Tick(AMistReturningSword::OutboundRange / AMistReturningSword::TravelSpeed);
        TestTrue(TEXT("The sword turns after its full outbound range"), Sword->IsReturning());
        Fixture.Caster->SetActorLocation(FVector(0.f, 500.f, 200.f));
        Sword->Tick(0.2f);
        TestTrue(TEXT("Return trajectory follows a moved caster"), Sword->GetActorLocation().Y > 100.f);
        Fixture.Caster->SetActorLocation(FVector(0.f, -400.f, 200.f));
        bool bReturned = false;
        Sword->OnReturned.BindLambda([&bReturned]() { bReturned = true; });
        Sword->Tick(1.f);
        TestTrue(TEXT("The sword catches the caster after another position change"), bReturned);
        TestTrue(TEXT("The catch is at the caster's current position"),
            Sword->GetActorLocation().Equals(Fixture.Caster->GetActorLocation(), 0.1f));
    }

    {
        FMistSwordTestWorld Fixture;
        Fixture.Wall(600.f);
        AMistTrainingDummy* BeforeWall = Fixture.Dummy(FVector(300.f, 0.f, 200.f));
        AMistTrainingDummy* BehindWall = Fixture.Dummy(FVector(700.f, 0.f, 200.f));
        AMistReturningSword* Sword = Fixture.Sword();
        int32 ReturnCount = 0;
        Sword->OnReturned.BindLambda([&ReturnCount]() { ++ReturnCount; });
        Sword->Tick(0.4f);
        TestTrue(TEXT("A wall turns the outbound sword before the full range"), Sword->IsReturning());
        TestTrue(TEXT("The sword never crosses the blocking wall"), Sword->GetActorLocation().X < 540.f);
        TestEqual(TEXT("The sword cannot damage an enemy behind terrain"), BehindWall->CombatHealth->Health, 1200.f);
        Sword->Tick(0.4f);
        TestEqual(TEXT("An outbound wall contact grants the recast callback once"), ReturnCount, 1);
        TestEqual(TEXT("A wall bounce hits the same enemy once on each leg"), BeforeWall->CombatHealth->Health, 933.3333f, 0.03f);
        TestEqual(TEXT("A wall bounce preserves healing on both legs"), Fixture.Health->Health, 553.3333f, 0.03f);
        TestEqual(TEXT("Wall bounce records one outbound hit"), Sword->GetHitsOnLeg(false), 1);
        TestEqual(TEXT("Wall bounce records one return hit"), Sword->GetHitsOnLeg(true), 1);
    }

    for (const float Angle : { 0.f, 30.f, 60.f, 80.f })
    {
        FMistSwordTestWorld Fixture;
        Fixture.Wall(100.f);
        const FVector Direction = FRotator(0.f, Angle, 0.f).Vector();
        AMistReturningSword* Sword = Fixture.Sword(Direction);
        FCollisionObjectQueryParams TerrainObjects;
        TerrainObjects.AddObjectTypesToQuery(ECC_WorldStatic);
        FCollisionQueryParams Query(SCENE_QUERY_STAT(MistSwordWallRegression), false, Sword);
        Query.AddIgnoredActor(Fixture.Caster);
        FHitResult Contact;
        const bool bWallHit = Fixture.World->SweepSingleByObjectType(Contact,
            Sword->GetActorLocation(), Sword->GetActorLocation() + Direction * AMistReturningSword::OutboundRange,
            FQuat::Identity, TerrainObjects, FCollisionShape::MakeSphere(AMistReturningSword::HitRadius), Query);
        TestTrue(FString::Printf(TEXT("%g degree fixture reaches a wall"), Angle), bWallHit);
        if (!bWallHit)
        {
            continue;
        }

        int32 ReturnCount = 0;
        Sword->OnReturned.BindLambda([&ReturnCount]() { ++ReturnCount; });
        // End the first frame just after contact so the next frame begins
        // beside the wall, rather than hiding a contact issue in a long tick.
        Sword->Tick(Contact.Distance / AMistReturningSword::TravelSpeed + 0.00001f);
        TestTrue(FString::Printf(TEXT("%g degree contact begins the return leg"), Angle), Sword->IsReturning());
        TestFalse(FString::Printf(TEXT("%g degree contact keeps the sword alive"), Angle), Sword->IsActorBeingDestroyed());
        TestTrue(FString::Printf(TEXT("%g degree turn separates from the wall surface"), Angle),
            FVector::DotProduct(Sword->GetActorLocation() - Contact.Location, Contact.ImpactNormal) > 1.f);
        Sword->Tick(0.5f);
        TestEqual(FString::Printf(TEXT("%g degree wall bounce grants the recast callback"), Angle), ReturnCount, 1);
        TestTrue(FString::Printf(TEXT("%g degree wall bounce catches the current caster position"), Angle),
            Sword->GetActorLocation().Equals(Fixture.Caster->GetActorLocation(), 0.1f));
    }

    {
        FMistSwordTestWorld Fixture;
        AMistReturningSword* Sword = Fixture.Sword();
        int32 ReturnCount = 0;
        Sword->OnReturned.BindLambda([&ReturnCount]() { ++ReturnCount; });
        Sword->Tick(0.25f);
        const FVector ContactPosition = Sword->GetActorLocation();
        // Put the near wall face exactly one projectile radius from the
        // already traveled endpoint. This starts the following frame at the
        // same touching state as a frame ending exactly on a wall boundary.
        Fixture.Wall(ContactPosition.X + AMistReturningSword::HitRadius + 10.f);
        Sword->Tick(0.0001f);
        TestTrue(TEXT("A zero-distance outbound wall contact turns the sword"), Sword->IsReturning());
        TestFalse(TEXT("A zero-distance contact does not destroy the returning sword"), Sword->IsActorBeingDestroyed());
        TestTrue(TEXT("A boundary contact retreats onto the previously clear outbound path"),
            Sword->GetActorLocation().X < ContactPosition.X - 1.f);
        Sword->Tick(0.5f);
        TestEqual(TEXT("A frame-boundary wall contact still grants the recast callback"), ReturnCount, 1);
        TestTrue(TEXT("A frame-boundary wall contact catches the caster"),
            Sword->GetActorLocation().Equals(Fixture.Caster->GetActorLocation(), 0.1f));
    }

    {
        FMistSwordTestWorld Fixture;
        // The caster is closer to this wall than the sword sphere radius,
        // so the outgoing sword starts overlapping terrain with no cleared
        // path to retreat along. Its safe return is an immediate catch.
        Fixture.Wall(50.f);
        AMistTrainingDummy* BehindWall = Fixture.Dummy(FVector(120.f, 0.f, 200.f));
        AMistReturningSword* Sword = Fixture.Sword();
        int32 ReturnCount = 0;
        Sword->OnReturned.BindLambda([&ReturnCount]() { ++ReturnCount; });
        Sword->Tick(0.01f);
        TestEqual(TEXT("Casting into an overlapping wall immediately grants the recast callback"), ReturnCount, 1);
        TestTrue(TEXT("An initially overlapping sword catches without moving through terrain"),
            Sword->GetActorLocation().Equals(Fixture.Caster->GetActorLocation(), 0.1f));
        TestTrue(TEXT("An initially overlapping sword cleans up after its catch"), Sword->IsActorBeingDestroyed());
        TestEqual(TEXT("An initially overlapping sword cannot damage through the wall"), BehindWall->CombatHealth->Health, 1200.f);
    }

    {
        FMistSwordTestWorld Fixture;
        AMistReturningSword* Sword = Fixture.Sword();
        Sword->Tick(AMistReturningSword::OutboundRange / AMistReturningSword::TravelSpeed);
        Fixture.Wall(600.f);
        AMistTrainingDummy* BehindWall = Fixture.Dummy(FVector(400.f, 0.f, 200.f));
        bool bReturned = false;
        Sword->OnReturned.BindLambda([&bReturned]() { bReturned = true; });
        Sword->Tick(1.f);
        TestFalse(TEXT("A blocked return does not grant an empowered recast"), bReturned);
        TestTrue(TEXT("A blocked return ends before crossing the wall"), Sword->GetActorLocation().X > 650.f);
        TestTrue(TEXT("A blocked return destroys its projectile"), Sword->IsActorBeingDestroyed());
        TestEqual(TEXT("Return damage cannot hit behind its blocking wall"), BehindWall->CombatHealth->Health, 1200.f);
    }

    {
        FMistSwordTestWorld Fixture;
        AMistTrainingDummy* Target = Fixture.Dummy(FVector(400.f, 0.f, 200.f));
        Target->CombatHealth->Health = 10.f;
        AMistReturningSword* Sword = Fixture.Sword();
        Sword->Tick(0.3f);
        TestEqual(TEXT("Overkill healing counts only health actually removed"), Fixture.Health->Health, 502.f, 0.01f);
        Sword->Tick(1.f);
        TestEqual(TEXT("The dead target grants no additional return healing"), Fixture.Health->Health, 502.f, 0.01f);
    }

    {
        FMistSwordTestWorld Fixture;
        AMistTrainingDummy* Target = Fixture.Dummy(FVector(400.f, 0.f, 200.f));
        Target->CombatHealth->GrantInvulnerability(3.f);
        AMistReturningSword* Sword = Fixture.Sword();
        Sword->Tick(0.3f);
        TestEqual(TEXT("Immunity prevents sword damage"), Target->CombatHealth->Health, 1200.f);
        TestEqual(TEXT("A zero-damage immune hit grants no healing"), Fixture.Health->Health, 500.f);
    }

    {
        FMistSwordTestWorld Fixture;
        Fixture.Health->Health = 0.f;
        Fixture.Dummy(FVector(400.f, 0.f, 200.f));
        AMistReturningSword* Sword = Fixture.Sword();
        Sword->Tick(0.3f);
        TestEqual(TEXT("A projectile cannot heal or revive its dead caster"), Fixture.Health->Health, 0.f);
    }
    return true;
}

#endif
