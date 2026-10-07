#pragma once
#include "CoreMinimal.h"

// Shared by drawing and hit testing. All UI sizes use one fitted design scale.
struct FMistHUDRect
{
    float X = 0.f, Y = 0.f, W = 0.f, H = 0.f;
    bool Contains(const FVector2D& Point) const
    { return Point.X >= X && Point.X <= X + W && Point.Y >= Y && Point.Y <= Y + H; }
};

struct FMistHUDLayout
{
    FMistHUDRect Dock, Inventory;
    float DockScale = 1.f, InventoryScale = 1.f;
    static FMistHUDLayout ForViewport(float Width, float Height)
    {
        Width = FMath::Max(1.f, Width); Height = FMath::Max(1.f, Height);
        const float Margin = FMath::Min(16.f, FMath::Min(Width, Height) * 0.03f);
        FMistHUDLayout Result;
        Result.DockScale = FMath::Min(1.15f, FMath::Min((Width - 2.f * Margin) / 860.f,
            (Height - 2.f * Margin) / 130.f));
        Result.Dock = {(Width - 860.f * Result.DockScale) * 0.5f,
            Height - Margin - 130.f * Result.DockScale, 860.f * Result.DockScale, 130.f * Result.DockScale};
        Result.InventoryScale = FMath::Min(1.15f, FMath::Min((Width - 2.f * Margin) / 1080.f,
            (Height - 2.f * Margin) / 640.f));
        Result.Inventory = {(Width - 1080.f * Result.InventoryScale) * 0.5f,
            (Height - 640.f * Result.InventoryScale) * 0.5f,
            1080.f * Result.InventoryScale, 640.f * Result.InventoryScale};
        return Result;
    }
    FMistHUDRect InventoryPart(float X, float Y, float W, float H) const
    { return {Inventory.X + X * InventoryScale, Inventory.Y + Y * InventoryScale, W * InventoryScale, H * InventoryScale}; }
};
