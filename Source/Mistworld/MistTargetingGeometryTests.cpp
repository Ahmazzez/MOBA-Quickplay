#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MistworldPlayerController.h"
#include "MistCombatHealth.h"
#include "MistMeleePresentation.h"
#include "MistReturningSword.h"
#include "MistTrainingDummy.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/LineBatchComponent.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "TimerManager.h"

struct FMistTargetingGeometryTestAccess
{
    static bool InCleave(AMistworldPlayerController* PC, AActor* Target)
    { return PC->OverlapsSector(Target, FVector::ForwardVector, 300.f, 55.f); }
    static bool InSweep(AMistworldPlayerController* PC, AActor* Target, float Range, float Radius)
    {
        const FVector Start = PC->GetPawn()->GetActorLocation();
        return PC->OverlapsSweptCapsule(Target, Start, Start + FVector::ForwardVector * Range, Radius);
    }
    static bool Visible(AMistworldPlayerController* PC, AActor* Target)
    { return PC->HasCombatLineOfSight(Target); }
    static void ResolveCleave(AMistworldPlayerController* PC)
    { PC->CommittedDirection = FVector::ForwardVector; PC->ResolveCleave(); }
    static void DrawCleave(AMistworldPlayerController* PC)
    { PC->DrawCleavePreview(FVector::ForwardVector, FColor::Cyan, 1.f); }
    static void DrawSweep(AMistworldPlayerController* PC, float Range, float Radius)
    { PC->DrawLinePreview(FVector::ForwardVector, Range, Radius, FColor::Cyan, 1.f); }
    static void AimQ(AMistworldPlayerController* PC)
    { PC->ActiveAimKey = EKeys::Q; PC->bAimingQ = true; }
    static void Cancel(AMistworldPlayerController* PC) { PC->CancelCombatInput(); }
    static bool HasAim(AMistworldPlayerController* PC) { return PC->ActiveAimKey.IsValid(); }
    static double QDeadline(AMistworldPlayerController* PC) { return PC->QReadyAt; }
    static bool HasPendingCast(AMistworldPlayerController* PC)
    { return PC->GetWorldTimerManager().IsTimerActive(PC->CombatTimer); }
    static void InvalidQ(AMistworldPlayerController* PC)
    { PC->CommitAbility(EKeys::Q, FVector::ZeroVector); }
};

namespace
{
struct FMistGeometryWorld
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    ACharacter* Hero = nullptr;
    AMistworldPlayerController* PC = nullptr;

    FMistGeometryWorld()
    {
        FActorSpawnParameters Params;
        Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        Hero = World->SpawnActor<ACharacter>(FVector(0.f, 0.f, 200.f), FRotator::ZeroRotator, Params);
        // Geometry needs no mannequin. Supply the presentation component
        // before possession so controller setup does not try to attach a
        // sword to this generic character's empty skeletal mesh.
        UMistMeleePresentation* Presentation = NewObject<UMistMeleePresentation>(Hero);
        Hero->AddInstanceComponent(Presentation);
        Presentation->RegisterComponent();
        PC = World->SpawnActor<AMistworldPlayerController>();
        PC->Possess(Hero);
    }
    ~FMistGeometryWorld() { World->DestroyWorld(false); }
    AMistTrainingDummy* Target(const FVector& Offset = FVector(200.f, 0.f, 0.f))
    {
        FActorSpawnParameters Params;
        Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        return World->SpawnActor<AMistTrainingDummy>(Hero->GetActorLocation() + Offset, FRotator::ZeroRotator, Params);
    }
    void Place(AMistTrainingDummy* Target, const FVector& Offset)
    { Target->SetActorLocation(Hero->GetActorLocation() + Offset, false); }
    AActor* Wall()
    {
        AActor* Actor = World->SpawnActor<AActor>();
        UBoxComponent* Box = NewObject<UBoxComponent>(Actor);
        Actor->SetRootComponent(Box);
        Box->SetBoxExtent(FVector(10.f, 500.f, 300.f));
        Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
        Box->SetCollisionObjectType(ECC_WorldStatic);
        Box->SetCollisionResponseToAllChannels(ECR_Block);
        Box->SetCanEverAffectNavigation(false);
        Box->RegisterComponent();
        Actor->SetActorLocation(Hero->GetActorLocation() + FVector(150.f, 0.f, 0.f));
        return Actor;
    }
    void ClearPreview()
    {
        if (ULineBatchComponent* Lines = World->GetLineBatcher(UWorld::ELineBatcherType::World)) Lines->BatchedLines.Reset();
        if (ULineBatchComponent* Lines = World->GetLineBatcher(UWorld::ELineBatcherType::WorldPersistent)) Lines->BatchedLines.Reset();
    }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMistTargetingGeometryTest, "Mistworld.Combat.TargetingGeometry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMistTargetingGeometryTest::RunTest(const FString& Parameters)
{
    {
        FMistGeometryWorld Fixture;
        AMistTrainingDummy* Target = Fixture.Target();
        if (!TestNotNull(TEXT("Geometry fixture has a possessed combat controller"), Fixture.PC)
            || !TestNotNull(TEXT("Geometry fixture has a real target capsule"), Target)) return false;

        // Check the drawn cone's dimensions against actual body overlap,
        // including targets whose centers sit outside a visible boundary.
        for (const float Angle : {-55.f, 55.f})
        {
            Fixture.Place(Target, FVector::ForwardVector.RotateAngleAxis(Angle, FVector::UpVector) * 260.f);
            TestTrue(FString::Printf(TEXT("Q includes its %.0f-degree boundary"), Angle),
                FMistTargetingGeometryTestAccess::InCleave(Fixture.PC, Target));
        }
        Fixture.Place(Target, FVector::ForwardVector.RotateAngleAxis(60.f, FVector::UpVector) * 260.f);
        TestTrue(TEXT("Q includes a capsule overlapping the side despite its center outside 55 degrees"),
            FMistTargetingGeometryTestAccess::InCleave(Fixture.PC, Target));
        Fixture.Place(Target, FVector::ForwardVector.RotateAngleAxis(65.f, FVector::UpVector) * 260.f);
        TestFalse(TEXT("Q excludes a capsule wholly outside the cone side"),
            FMistTargetingGeometryTestAccess::InCleave(Fixture.PC, Target));
        Fixture.Place(Target, FVector(339.f, 0.f, 0.f));
        TestTrue(TEXT("Q 300cm reach includes the near edge of a 40cm target capsule"),
            FMistTargetingGeometryTestAccess::InCleave(Fixture.PC, Target));
        Fixture.Place(Target, FVector(341.f, 0.f, 0.f));
        TestFalse(TEXT("Q excludes a capsule beyond its 300cm reach plus target radius"),
            FMistTargetingGeometryTestAccess::InCleave(Fixture.PC, Target));
        Fixture.Place(Target, FVector(-100.f, 0.f, 0.f));
        TestFalse(TEXT("Q does not cleave a target behind the hero"),
            FMistTargetingGeometryTestAccess::InCleave(Fixture.PC, Target));

        Fixture.Place(Target, FVector(230.f, 0.f, 0.f));
        Target->CombatHealth->ResetHealth();
        TestTrue(TEXT("The clear front target has terrain line of sight"),
            FMistTargetingGeometryTestAccess::Visible(Fixture.PC, Target));
        FMistTargetingGeometryTestAccess::ResolveCleave(Fixture.PC);
        TestEqual(TEXT("The real Q resolver damages a clear target through armor"),
            Target->CombatHealth->Health, 1200.f - 140.f / 1.2f, 0.02f);
        Target->CombatHealth->ResetHealth();
        Fixture.Wall();
        TestFalse(TEXT("Terrain occludes an otherwise geometrically valid Q target"),
            FMistTargetingGeometryTestAccess::Visible(Fixture.PC, Target));
        FMistTargetingGeometryTestAccess::ResolveCleave(Fixture.PC);
        TestEqual(TEXT("The actual Q resolver cannot damage a target through the wall"), Target->CombatHealth->Health, 1200.f);

        Fixture.ClearPreview();
        FMistTargetingGeometryTestAccess::DrawCleave(Fixture.PC);
        ULineBatchComponent* Lines = Fixture.World->GetLineBatcher(UWorld::ELineBatcherType::WorldPersistent);
        if (TestNotNull(TEXT("Q preview supplies an inspectable line batch"), Lines))
        {
            bool bHasLegalOutline = false, bHasBlockedExtension = false;
            for (const FBatchedLine& Line : Lines->BatchedLines)
            {
                if (Line.Color.Equals(FLinearColor(FColor::Cyan), 0.001f))
                {
                    bHasLegalOutline = true;
                    TestTrue(TEXT("Q legal preview stops at the wall's near face"),
                        Line.Start.X <= 140.5f && Line.End.X <= 140.5f);
                }
                else if (Line.Color.R > Line.Color.G && Line.Color.R > Line.Color.B
                    && (Line.Start.X > 141.f || Line.End.X > 141.f)) bHasBlockedExtension = true;
            }
            TestTrue(TEXT("Clipped Q preview still displays the legal cone outline"), bHasLegalOutline);
            TestTrue(TEXT("Q preview marks blocked range with red terrain feedback"), bHasBlockedExtension);
        }
    }

    {
        FMistGeometryWorld Fixture;
        AMistTrainingDummy* Target = Fixture.Target();
        struct FSweepCase { const TCHAR* Name; float Range, Radius; };
        for (const FSweepCase& Shape : {FSweepCase{TEXT("W"), 600.f, 75.f},
            FSweepCase{TEXT("Empowered W"), 800.f, 100.f}, FSweepCase{TEXT("R sword"), 1000.f, 60.f}})
        {
            const float BodyReach = Shape.Radius + Target->Capsule->GetScaledCapsuleRadius();
            Fixture.Place(Target, FVector(-(BodyReach - 1.f), 0.f, 0.f));
            TestTrue(FString::Printf(TEXT("%s includes its rounded start-cap contact"), Shape.Name),
                FMistTargetingGeometryTestAccess::InSweep(Fixture.PC, Target, Shape.Range, Shape.Radius));
            Fixture.Place(Target, FVector(-(BodyReach + 1.f), 0.f, 0.f));
            TestFalse(FString::Printf(TEXT("%s excludes a body beyond its rounded start cap"), Shape.Name),
                FMistTargetingGeometryTestAccess::InSweep(Fixture.PC, Target, Shape.Range, Shape.Radius));
            Fixture.Place(Target, FVector(Shape.Range + BodyReach - 1.f, 0.f, 0.f));
            TestTrue(FString::Printf(TEXT("%s includes its rounded endpoint contact"), Shape.Name),
                FMistTargetingGeometryTestAccess::InSweep(Fixture.PC, Target, Shape.Range, Shape.Radius));
            Fixture.Place(Target, FVector(Shape.Range + BodyReach + 1.f, 0.f, 0.f));
            TestFalse(FString::Printf(TEXT("%s excludes a body beyond its rounded endpoint"), Shape.Name),
                FMistTargetingGeometryTestAccess::InSweep(Fixture.PC, Target, Shape.Range, Shape.Radius));
            Fixture.Place(Target, FVector(Shape.Range * 0.5f, BodyReach - 1.f, 0.f));
            TestTrue(FString::Printf(TEXT("%s includes the body edge overlapping a lateral boundary"), Shape.Name),
                FMistTargetingGeometryTestAccess::InSweep(Fixture.PC, Target, Shape.Range, Shape.Radius));
            Fixture.Place(Target, FVector(Shape.Range * 0.5f, BodyReach + 1.f, 0.f));
            TestFalse(FString::Printf(TEXT("%s excludes a body outside the lateral boundary"), Shape.Name),
                FMistTargetingGeometryTestAccess::InSweep(Fixture.PC, Target, Shape.Range, Shape.Radius));

            Fixture.ClearPreview();
            FMistTargetingGeometryTestAccess::DrawSweep(Fixture.PC, Shape.Range, Shape.Radius);
            ULineBatchComponent* Lines = Fixture.World->GetLineBatcher(UWorld::ELineBatcherType::WorldPersistent);
            if (!TestNotNull(TEXT("Swept ability preview supplies an inspectable line batch"), Lines)) continue;
            float MinX = TNumericLimits<float>::Max(), MaxX = -TNumericLimits<float>::Max(), MaxY = 0.f;
            for (const FBatchedLine& Line : Lines->BatchedLines)
                for (const FVector& Point : {Line.Start, Line.End})
                { MinX = FMath::Min(MinX, static_cast<float>(Point.X)); MaxX = FMath::Max(MaxX, static_cast<float>(Point.X)); MaxY = FMath::Max(MaxY, static_cast<float>(FMath::Abs(Point.Y))); }
            TestEqual(FString::Printf(TEXT("%s preview displays the rounded cap behind its origin"), Shape.Name), MinX, -Shape.Radius, 0.05f);
            TestEqual(FString::Printf(TEXT("%s preview displays the rounded cap past its endpoint"), Shape.Name), MaxX, Shape.Range + Shape.Radius, 0.05f);
            TestEqual(FString::Printf(TEXT("%s preview width matches its actual swept radius"), Shape.Name), MaxY, Shape.Radius, 0.05f);
        }

        Fixture.Place(Target, FVector(200.f, 0.f, 0.f));
        const float Before = Target->CombatHealth->Health;
        const double Deadline = FMistTargetingGeometryTestAccess::QDeadline(Fixture.PC);
        FMistTargetingGeometryTestAccess::AimQ(Fixture.PC);
        FMistTargetingGeometryTestAccess::Cancel(Fixture.PC);
        TestFalse(TEXT("Cancelling Q preview clears its actual aiming state"), FMistTargetingGeometryTestAccess::HasAim(Fixture.PC));
        TestFalse(TEXT("Cancelling an uncommitted preview schedules no delayed impact"), FMistTargetingGeometryTestAccess::HasPendingCast(Fixture.PC));
        TestEqual(TEXT("Cancelled Q preview does not consume cooldown"), FMistTargetingGeometryTestAccess::QDeadline(Fixture.PC), Deadline);
        TestEqual(TEXT("Cancelled Q preview deals no health damage"), Target->CombatHealth->Health, Before);
        FMistTargetingGeometryTestAccess::InvalidQ(Fixture.PC);
        TestFalse(TEXT("A zero-direction Q cannot enter a committed cast"), Fixture.PC->IsCombatBusy());
        TestFalse(TEXT("An invalid Q schedules no delayed impact"), FMistTargetingGeometryTestAccess::HasPendingCast(Fixture.PC));
        TestEqual(TEXT("An invalid Q consumes no cooldown"), FMistTargetingGeometryTestAccess::QDeadline(Fixture.PC), Deadline);
        TestEqual(TEXT("An invalid Q damages no target"), Target->CombatHealth->Health, Before);
    }

    {
        FMistGeometryWorld Fixture;
        AMistTrainingDummy* Behind = Fixture.Target(FVector(-99.f, 0.f, 0.f));
        AMistTrainingDummy* BeyondEnd = Fixture.Target(FVector(1099.f, 0.f, 0.f));
        AMistTrainingDummy* Outside = Fixture.Target(FVector(500.f, 101.f, 0.f));
        AMistReturningSword* Sword = Fixture.World->SpawnActor<AMistReturningSword>();
        Sword->Initialize(Fixture.Hero, FVector::ForwardVector);
        Sword->Tick(AMistReturningSword::OutboundRange / AMistReturningSword::TravelSpeed);
        TestEqual(TEXT("The real R projectile includes its previewed rounded start-cap target"), Behind->CombatHealth->Health, 1200.f - 160.f / 1.2f, 0.02f);
        TestEqual(TEXT("The real R projectile includes its previewed rounded endpoint target"), BeyondEnd->CombatHealth->Health, 1200.f - 160.f / 1.2f, 0.02f);
        TestEqual(TEXT("The real R projectile excludes a capsule beyond its lateral radius"), Outside->CombatHealth->Health, 1200.f);
    }
    return true;
}

#endif
