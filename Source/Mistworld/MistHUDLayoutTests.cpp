#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MistHUDLayout.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMistHUDResponsiveLayoutTest, "Mistworld.UI.ResponsiveLayout",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMistHUDResponsiveLayoutTest::RunTest(const FString& Parameters)
{
    // Exercise widths and aspect ratios that previously clipped inventory details
    // or pushed controls out of the playable viewport. Verify geometry, not pixels.
    const TArray<FVector2D> Viewports = {
        {640,360}, {800,600}, {1280,720}, {1920,1080}, {2560,1440},
        {3440,1440}, {720,1280}, {320,180}, {1,1}
    };
    for (const FVector2D& Size : Viewports)
    {
        const FString Label = FString::Printf(TEXT("%.0fx%.0f"), Size.X, Size.Y);
        const FMistHUDLayout Layout = FMistHUDLayout::ForViewport(Size.X, Size.Y);
        auto IsInViewport = [&](const FMistHUDRect& R)
        {
            return FMath::IsFinite(R.X) && FMath::IsFinite(R.Y)
                && FMath::IsFinite(R.W) && FMath::IsFinite(R.H)
                && R.W > 0.f && R.H > 0.f && R.X >= -0.001f && R.Y >= -0.001f
                && R.X + R.W <= Size.X + 0.001f && R.Y + R.H <= Size.Y + 0.001f;
        };
        TestTrue(Label + TEXT(": combat dock stays inside viewport"), IsInViewport(Layout.Dock));
        TestTrue(Label + TEXT(": inventory stays inside viewport"), IsInViewport(Layout.Inventory));
        TestEqual(Label + TEXT(": dock remains horizontally centered"),
            Layout.Dock.X + Layout.Dock.W * 0.5f, static_cast<float>(Size.X * 0.5), 0.001f);
        TestEqual(Label + TEXT(": inventory remains horizontally centered"),
            Layout.Inventory.X + Layout.Inventory.W * 0.5f, static_cast<float>(Size.X * 0.5), 0.001f);
        TestEqual(Label + TEXT(": inventory remains vertically centered"),
            Layout.Inventory.Y + Layout.Inventory.H * 0.5f, static_cast<float>(Size.Y * 0.5), 0.001f);
        TestTrue(Label + TEXT(": dock is attached to the bottom edge"),
            Size.Y - Layout.Dock.Y - Layout.Dock.H <= 16.001f);
        TestTrue(Label + TEXT(": text and icons use positive fitted scales"),
            Layout.DockScale > 0.f && Layout.DockScale <= 1.15f
            && Layout.InventoryScale > 0.f && Layout.InventoryScale <= 1.15f);

        // Use the actual inventory controls' design rectangles. Their centers must
        // remain clickable after scaling, and no slot may intercept its neighbors.
        TArray<FMistHUDRect> Slots;
        for (float Row : {138.f,209.f,319.f,390.f,461.f})
            Slots.Add(Layout.InventoryPart(28.f,Row,284.f,61.f));
        for (int32 Index = 0; Index < Slots.Num(); ++Index)
        {
            const FMistHUDRect& R = Slots[Index];
            const FVector2D Center(R.X + R.W * 0.5f, R.Y + R.H * 0.5f);
            TestTrue(Label + TEXT(": slot control fits inventory"), IsInViewport(R)
                && Layout.Inventory.Contains({R.X,R.Y})
                && Layout.Inventory.Contains({R.X+R.W,R.Y+R.H}));
            TestTrue(Label + TEXT(": scaled slot center can be clicked"), R.Contains(Center));
            for (int32 Other = 0; Other < Slots.Num(); ++Other)
                if (Other != Index)
                    TestFalse(Label + TEXT(": slot center does not click another slot"), Slots[Other].Contains(Center));
        }
        const FMistHUDRect Equip = Layout.InventoryPart(780.f,499.f,272.f,42.f);
        const FMistHUDRect Close = Layout.InventoryPart(963.f,27.f,88.f,35.f);
        TestTrue(Label + TEXT(": equip button stays inside inventory"),
            Layout.Inventory.Contains({Equip.X,Equip.Y}) && Layout.Inventory.Contains({Equip.X+Equip.W,Equip.Y+Equip.H}));
        TestTrue(Label + TEXT(": close button stays inside inventory"),
            Layout.Inventory.Contains({Close.X,Close.Y}) && Layout.Inventory.Contains({Close.X+Close.W,Close.Y+Close.H}));
        TestFalse(Label + TEXT(": equip click cannot close the inventory"),
            Close.Contains({Equip.X+Equip.W*0.5f,Equip.Y+Equip.H*0.5f}));
    }
    const FMistHUDLayout ZeroViewport = FMistHUDLayout::ForViewport(0.f,0.f);
    TestTrue(TEXT("A transient zero-size viewport produces finite positive layout"),
        FMath::IsFinite(ZeroViewport.DockScale) && FMath::IsFinite(ZeroViewport.InventoryScale)
        && ZeroViewport.DockScale > 0.f && ZeroViewport.InventoryScale > 0.f);
    return true;
}

#endif
