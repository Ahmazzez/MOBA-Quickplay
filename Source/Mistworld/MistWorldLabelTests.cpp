#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MistWorldLabel.h"
#include "Math/RotationMatrix.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMistWorldLabelFacingTest, "Mistworld.UI.WorldLabelFacing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMistWorldLabelFacingTest::RunTest(const FString& Parameters)
{
    const FVector Position(200.f, 300.f, 245.f);
    for (const FVector Offset : {FVector(-1000.f, -1000.f, 1400.f), FVector(1000.f, -1000.f, 1400.f),
        FVector(1000.f, 1000.f, 1400.f), FVector(-1000.f, 1000.f, 1400.f)})
    {
        const FRotator Rotation = AMistWorldLabel::CameraFacingRotation(Position, Position + Offset);
        TestTrue(TEXT("Text normal faces the camera from every side"), Rotation.Vector().Equals(Offset.GetSafeNormal(), 0.0001f));
        TestEqual(TEXT("Billboard text never rolls sideways"), Rotation.Roll, 0.0, 0.0001);
        const FVector TextUp = FRotationMatrix(Rotation).GetScaledAxis(EAxis::Z);
        const FVector ExpectedUp = (FVector::UpVector - Offset.GetSafeNormal() * FVector::DotProduct(FVector::UpVector, Offset.GetSafeNormal())).GetSafeNormal();
        TestTrue(TEXT("Glyph up follows the camera-facing plane's upright direction"), TextUp.Equals(ExpectedUp, 0.0001f));
    }
    return true;
}

#endif
