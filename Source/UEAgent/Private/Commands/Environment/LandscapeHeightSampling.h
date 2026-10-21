// Copyright (c) 2026 CarlxsMG.

#pragma once

#include "CoreMinimal.h"
#include "Landscape.h"            // ALandscape
#include "LandscapeInfo.h"        // ULandscapeInfo
#include "LandscapeEdit.h"        // FHeightmapAccessor
#include "LandscapeDataAccess.h"  // LANDSCAPE_ZSCALE (uint16 heightmap value <-> local Z)

/**
 * Terrain height read straight from a landscape's heightmap: no collision, no line trace and no PIE,
 * so it answers headless, where a trace misses the landscape. Shared by get_landscape_height_at and
 * the commands that ground actors on the terrain.
 */
namespace LandscapeHeightSampling
{
    /** The terrain surface at one world XY. */
    struct FHeightSample
    {
        /** World Z of the surface. */
        double WorldZ = 0.0;
        /** Lower-left vertex (column, row) of the interpolated quad. */
        int32 VertexX = 0;
        int32 VertexY = 0;
        /** False when the XY lies outside the vertex extent; the nearest edge is sampled instead. */
        bool bInBounds = false;
    };

    /**
     * The surface height at world (WorldX, WorldY), bilinear over the enclosing quad of the heightmap, with
     * the point clamped into the vertex extent [ExtMinX..ExtMaxX] x [ExtMinY..ExtMaxY]. A vertex sits at
     * (World - ActorLocation) / Scale, the mapping the sculpt tools write through, so a write and a read land
     * on the same vertices (LandscapeActorToWorld folds in a section-base offset that mapping does not).
     * Assumes an axis-aligned landscape. False, with Out untouched, when the XY scale is degenerate.
     */
    inline bool SampleHeightAt(const ALandscape* Landscape, ULandscapeInfo* Info,
        int32 ExtMinX, int32 ExtMinY, int32 ExtMaxX, int32 ExtMaxY,
        double WorldX, double WorldY, FHeightSample& Out)
    {
        const FVector ActorLoc = Landscape->GetActorLocation();
        const FVector ActorScale = Landscape->GetActorScale3D();
        if (FMath::Abs(ActorScale.X) <= KINDA_SMALL_NUMBER || FMath::Abs(ActorScale.Y) <= KINDA_SMALL_NUMBER)
        {
            return false;
        }
        const double VertexXf = (WorldX - ActorLoc.X) / ActorScale.X;
        const double VertexYf = (WorldY - ActorLoc.Y) / ActorScale.Y;

        const bool bInBounds = VertexXf >= ExtMinX && VertexXf <= ExtMaxX
                            && VertexYf >= ExtMinY && VertexYf <= ExtMaxY;

        // Clamp into the extent, then take the lower-left vertex of the enclosing quad
        // (kept one quad inside the max edge so the 2x2 read stays in range).
        const double SampleXf = FMath::Clamp(VertexXf, static_cast<double>(ExtMinX), static_cast<double>(ExtMaxX));
        const double SampleYf = FMath::Clamp(VertexYf, static_cast<double>(ExtMinY), static_cast<double>(ExtMaxY));
        const int32 X0 = FMath::Clamp(static_cast<int32>(FMath::FloorToDouble(SampleXf)), ExtMinX, FMath::Max(ExtMinX, ExtMaxX - 1));
        const int32 Y0 = FMath::Clamp(static_cast<int32>(FMath::FloorToDouble(SampleYf)), ExtMinY, FMath::Max(ExtMinY, ExtMaxY - 1));
        const int32 X1 = FMath::Min(X0 + 1, ExtMaxX);
        const int32 Y1 = FMath::Min(Y0 + 1, ExtMaxY);

        // The enclosing vertex block, row-major over the inclusive rect.
        const int32 RectW = (X1 - X0) + 1;
        const int32 RectH = (Y1 - Y0) + 1;
        TArray<uint16> Heights;
        Heights.SetNumZeroed(RectW * RectH);
        {
            FHeightmapAccessor<false> Accessor(Info);
            Accessor.GetDataFast(X0, Y0, X1, Y1, Heights.GetData());
        }

        // Bilinear over the 2x2 block; at an edge Tx/Ty are pinned to 0 and it degenerates to 1xN.
        const double Tx = (X1 > X0) ? (SampleXf - X0) : 0.0;
        const double Ty = (Y1 > Y0) ? (SampleYf - Y0) : 0.0;
        auto HeightAt = [&](int32 VX, int32 VY) -> double
        {
            const int32 Col = FMath::Clamp(VX - X0, 0, RectW - 1);
            const int32 Row = FMath::Clamp(VY - Y0, 0, RectH - 1);
            return static_cast<double>(Heights[Row * RectW + Col]);
        };
        const double HTop = FMath::Lerp(HeightAt(X0, Y0), HeightAt(X1, Y0), Tx);
        const double HBot = FMath::Lerp(HeightAt(X0, Y1), HeightAt(X1, Y1), Tx);
        const double HInterp = FMath::Lerp(HTop, HBot, Ty);

        // uint16 -> local Z (LandscapeDataAccess::GetLocalHeight) -> world Z, honoring the actor's Z and Z scale.
        const double LocalZ = (HInterp - 32768.0) * static_cast<double>(LANDSCAPE_ZSCALE);
        Out.WorldZ = ActorLoc.Z + LocalZ * static_cast<double>(ActorScale.Z);
        Out.VertexX = X0;
        Out.VertexY = Y0;
        Out.bInBounds = bInBounds;
        return true;
    }
}
