// Copyright (c) 2026 CarlxsMG.

//
// Read-only UV-mapping diagnostics (base + per-vertex extension):
// list_uv_channels + get_uv_health_report, both pure-read, headless-safe.
// The write side of this family is described on the namespace below.
//
// Read-side mesh access pattern (UStaticMesh only — no UDynamicMesh / GeometryScript):
//   - UStaticMesh::GetNumUVChannels(LODIndex)
//   - UStaticMesh::GetNumVertices(LODIndex)
//   - UStaticMesh::GetLightMapCoordinateIndex()
//   - UStaticMesh::GetMeshDescription(LODIndex) for triangle counts + UV hint
//
// Per-vertex UV iteration: goes through the baked render data path
// because UE 5.8 stores UVs in per-channel FUVID containers in the authoring
// FMeshDescription but BAKE them into a flat FStaticMeshVertexBuffer that the
// GPU reads. The flat buffer is what `get_uv_health_report` iterates — it
// reflects what actually goes to the rasterizer. The path is:
//   1. Mesh->GetRenderData() -> FStaticMeshRenderData*
//   2. RenderData->LODResources[LODIndex].IndexBuffer.GetArrayView() -> indices
//   3. .VertexBuffers.StaticMeshVertexBuffer.GetVertexUV(VertexIdx, UVCh) -> FVector2f
//
// The read-side queries never SavePackage / Modify. Domain = UVMapping.
#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

#include "Engine/StaticMesh.h"
#include "Engine/Engine.h"
#include "StaticMeshDescription.h"
#include "MeshDescription.h"
#include "StaticMeshResources.h"
#include "RawIndexBuffer.h"

#include "UObject/UObjectIterator.h"

// Write-side UV ops via UGeometryScriptLibrary_MeshUVFunctions (static<->dynamic round-trip)
#include "UDynamicMesh.h"
#include "GeometryScript/MeshAssetFunctions.h"   // CopyMeshFromStaticMesh / CopyMeshToStaticMesh
#include "GeometryScript/MeshUVFunctions.h"      // UGeometryScriptLibrary_MeshUVFunctions::ClearUVChannel / SetMeshTriangleUVs / RepackMeshUVs
#include "GeometryScript/MeshQueryFunctions.h"   // UGeometryScriptLibrary_MeshQueryFunctions::GetNumUVSets
#include "GeometryScript/GeometryScriptTypes.h"  // FGeometryScriptUVTriangle, EGeometryScriptUVLayoutType, FGeometryScriptRepackUVsOptions

/**
 * UV-mapping diagnostics + targeted write operations.
 *
 * Read-side (headless-safe, no SavePackage):
 *   - list_uv_channels: per-LOD inventory of UV channels (count, lightmap
 *     binding, vertices, triangles)
 *   - get_uv_health_report: same inventory plus heuristic health metrics
 *     (in-unit-square ratio, overlap count, lightmap hard-fail flag).
 *
 * Write-side (mutates the static mesh via UGeometryScriptLibrary_MeshUVFunctions
 * static<->dynamic round-trip; saves the package on success):
 *   - clear_uv_channel: drop all UVs from one channel.
 *   - set_triangle_uvs: set the 3 UV corners of a single triangle (granular
 *     UV editing without needing a full repack / regeneration).
 *
 * Domain: UVMapping — covers diagnostics + targeted UV writes that don't fit
 * cleanly under GeometryScript (which owns the broader mesh-mutation surface).
 */
namespace UEAgentUVMappingCommands
{

// MCP-DOMAIN: UVMapping

namespace
{
    // How many triangle pairs the overlap test may visit before it stops and reports itself incomplete: the grid
    // keeps an ordinary layout far below it, but every triangle stacked in one spot would make the pass quadratic.
    constexpr int64 UVMapping_MaxOverlapPairVisits = 20000000;

    // Slack, in UV units, that two triangles must exceed along every separating axis to count as overlapping:
    // triangles that share an edge or a corner only touch, and their projections meet exactly there.
    constexpr double UVMapping_OverlapEpsilon = 1e-6;

    struct FUVOverlapCount
    {
        int32 OverlappingTriangles = 0; // triangles whose UV triangle overlaps at least one other with positive area
        bool bCapped = false;           // the pair budget ran out, so the count is a lower bound
    };

    // Whether no edge normal of A separates A from B (separating axis test on A's three edges).
    bool UVMapping_NoSeparatingEdge(const FVector2D* A, const FVector2D* B)
    {
        for (int32 Edge = 0; Edge < 3; ++Edge)
        {
            const FVector2D Dir = A[(Edge + 1) % 3] - A[Edge];
            const double Length = Dir.Size();
            if (Length <= 1e-12)
            {
                continue;
            }
            const FVector2D Axis(-Dir.Y / Length, Dir.X / Length);
            double MinA = TNumericLimits<double>::Max();
            double MaxA = TNumericLimits<double>::Lowest();
            double MinB = TNumericLimits<double>::Max();
            double MaxB = TNumericLimits<double>::Lowest();
            for (int32 Corner = 0; Corner < 3; ++Corner)
            {
                const double OnA = FVector2D::DotProduct(A[Corner], Axis);
                const double OnB = FVector2D::DotProduct(B[Corner], Axis);
                MinA = FMath::Min(MinA, OnA);
                MaxA = FMath::Max(MaxA, OnA);
                MinB = FMath::Min(MinB, OnB);
                MaxB = FMath::Max(MaxB, OnB);
            }
            if (MaxA <= MinB + UVMapping_OverlapEpsilon || MaxB <= MinA + UVMapping_OverlapEpsilon)
            {
                return false;
            }
        }
        return true;
    }

    // Two triangles overlap with positive area exactly when no edge normal of either one separates them.
    bool UVMapping_TrianglesOverlap(const FVector2D* A, const FVector2D* B)
    {
        return UVMapping_NoSeparatingEdge(A, B) && UVMapping_NoSeparatingEdge(B, A);
    }

    /**
     * How many triangles (three corners each in Corners) overlap another one in UV space with positive area.
     * Degenerate triangles (no area) cannot overlap and are skipped. A uniform grid over the UV bounds limits the
     * pairs tested to triangles whose boxes share a cell, and each pair is tested once, in the first cell both
     * boxes cover; at most MaxPairVisits pairs are visited.
     */
    FUVOverlapCount UVMapping_CountOverlappingTriangles(const TArray<FVector2D>& Corners, int64 MaxPairVisits)
    {
        FUVOverlapCount Result;
        const int32 NumTris = Corners.Num() / 3;

        TArray<int32> Live;
        Live.Reserve(NumTris);
        FBox2D Bounds(ForceInit);
        for (int32 Tri = 0; Tri < NumTris; ++Tri)
        {
            const FVector2D& P0 = Corners[Tri * 3];
            const FVector2D& P1 = Corners[Tri * 3 + 1];
            const FVector2D& P2 = Corners[Tri * 3 + 2];
            const double TwiceArea = FMath::Abs((P1.X - P0.X) * (P2.Y - P0.Y) - (P1.Y - P0.Y) * (P2.X - P0.X));
            if (TwiceArea <= 1e-12)
            {
                continue;
            }
            Live.Add(Tri);
            Bounds += P0;
            Bounds += P1;
            Bounds += P2;
        }
        if (Live.Num() < 2)
        {
            return Result;
        }

        const int32 GridSize = FMath::Clamp(static_cast<int32>(FMath::Sqrt(static_cast<double>(Live.Num()))), 1, 512);
        const FVector2D Size = Bounds.GetSize();
        const double CellX = FMath::Max(Size.X / GridSize, 1e-12);
        const double CellY = FMath::Max(Size.Y / GridSize, 1e-12);
        auto CellIndex = [GridSize](double Value, double Min, double Cell)
        {
            return FMath::Clamp(static_cast<int32>(FMath::FloorToDouble((Value - Min) / Cell)), 0, GridSize - 1);
        };

        // Each live triangle's cell range, and the triangles of each cell.
        TArray<FIntRect> Ranges;
        Ranges.SetNum(Live.Num());
        TArray<TArray<int32>> Cells;
        Cells.SetNum(GridSize * GridSize);
        for (int32 K = 0; K < Live.Num(); ++K)
        {
            const FVector2D* P = &Corners[Live[K] * 3];
            const double MinX = FMath::Min3(P[0].X, P[1].X, P[2].X);
            const double MaxX = FMath::Max3(P[0].X, P[1].X, P[2].X);
            const double MinY = FMath::Min3(P[0].Y, P[1].Y, P[2].Y);
            const double MaxY = FMath::Max3(P[0].Y, P[1].Y, P[2].Y);
            FIntRect& Range = Ranges[K];
            Range.Min = FIntPoint(CellIndex(MinX, Bounds.Min.X, CellX), CellIndex(MinY, Bounds.Min.Y, CellY));
            Range.Max = FIntPoint(CellIndex(MaxX, Bounds.Min.X, CellX), CellIndex(MaxY, Bounds.Min.Y, CellY));
            for (int32 CY = Range.Min.Y; CY <= Range.Max.Y; ++CY)
            {
                for (int32 CX = Range.Min.X; CX <= Range.Max.X; ++CX)
                {
                    Cells[CY * GridSize + CX].Add(K);
                }
            }
        }

        TBitArray<> Overlapping(false, Live.Num());
        int64 Visits = 0;
        for (int32 CY = 0; CY < GridSize && !Result.bCapped; ++CY)
        {
            for (int32 CX = 0; CX < GridSize && !Result.bCapped; ++CX)
            {
                const TArray<int32>& InCell = Cells[CY * GridSize + CX];
                for (int32 I = 0; I < InCell.Num() && !Result.bCapped; ++I)
                {
                    for (int32 J = I + 1; J < InCell.Num(); ++J)
                    {
                        if (++Visits > MaxPairVisits)
                        {
                            Result.bCapped = true;
                            break;
                        }
                        const int32 A = InCell[I];
                        const int32 B = InCell[J];
                        // Tested only in the first cell both boxes cover, so a pair is never tested twice.
                        if (FMath::Max(Ranges[A].Min.X, Ranges[B].Min.X) != CX || FMath::Max(Ranges[A].Min.Y, Ranges[B].Min.Y) != CY)
                        {
                            continue;
                        }
                        if (Overlapping[A] && Overlapping[B])
                        {
                            continue; // both already counted: the test could not change the result
                        }
                        if (UVMapping_TrianglesOverlap(&Corners[Live[A] * 3], &Corners[Live[B] * 3]))
                        {
                            Overlapping[A] = true;
                            Overlapping[B] = true;
                        }
                    }
                }
            }
        }

        for (int32 K = 0; K < Live.Num(); ++K)
        {
            Result.OverlappingTriangles += Overlapping[K] ? 1 : 0;
        }
        return Result;
    }

    /** The UStaticMesh at Path: null once OutMesh holds it, otherwise the error reply "<FailurePrefix>: <Path>". */
    TSharedPtr<FJsonObject> UVMapping_LoadMesh(const FString& Path, const TCHAR* FailurePrefix, UStaticMesh*& OutMesh)
    {
        OutMesh = mcp_common::LoadAssetOrSoftPath<UStaticMesh>(Path);
        if (!OutMesh)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("%s: %s"), FailurePrefix, *Path));
        }
        return nullptr;
    }

    /**
     * Why CopyMeshFromStaticMesh cannot read LOD 0 of StaticMesh with an EGeometryScriptLODType::SourceModel request,
     * or empty when it can. The rule is the one the GeometryScript static-mesh queries apply
     * (GeometryScript_ResolveStaticMeshReadLOD): in the editor the engine reads the source model of the LOD when it
     * holds geometry, else that LOD's render data, and a mesh with no source model, or with neither for LOD 0, makes
     * it index LOD -1 and kill the editor.
     */
    FString UVMapping_ReadLOD0Error(const UStaticMesh* StaticMesh)
    {
        if (StaticMesh->GetNumSourceModels() == 0)
        {
            return FString::Printf(TEXT("Static mesh '%s' has no source model, so no source geometry to read; nothing was changed"),
                *StaticMesh->GetPathName());
        }
        if (!StaticMesh->GetSourceModel(0).IsSourceModelInitialized() && StaticMesh->GetNumLODs() == 0)
        {
            return FString::Printf(TEXT("Static mesh '%s' LOD 0 has no source geometry and no render LOD to read; nothing was changed"),
                *StaticMesh->GetPathName());
        }
        return FString();
    }

    /** LOD 0 of StaticMesh (build settings applied), the LOD the write side writes back, copied into a new transient
     *  dynamic mesh: null once OutDynMesh holds it, otherwise the error reply. */
    TSharedPtr<FJsonObject> UVMapping_CopyToDynamicMesh(UStaticMesh* StaticMesh, UDynamicMesh*& OutDynMesh)
    {
        const FString LODError = UVMapping_ReadLOD0Error(StaticMesh);
        if (!LODError.IsEmpty())
        {
            return mcp_common::MakeErrorReply(LODError);
        }
        OutDynMesh = NewObject<UDynamicMesh>(GetTransientPackage());
        FGeometryScriptCopyMeshFromAssetOptions Opts;
        Opts.bApplyBuildSettings = true;
        // LOD 0 itself, which the default FGeometryScriptMeshWriteLOD of UVMapping_CopyToStaticMesh writes back:
        // MaxAvailable would read the hi-res source model of a mesh that has one, and write it over LOD 0.
        FGeometryScriptMeshReadLOD ReadLOD;
        ReadLOD.LODType = EGeometryScriptLODType::SourceModel;
        ReadLOD.LODIndex = 0;
        EGeometryScriptOutcomePins Outcome = EGeometryScriptOutcomePins::Failure;
        UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromStaticMesh(StaticMesh, OutDynMesh, Opts, ReadLOD, Outcome);
        if (Outcome != EGeometryScriptOutcomePins::Success)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("CopyMeshFromStaticMesh failed (outcome=%d)"), static_cast<int32>(Outcome)));
        }
        return nullptr;
    }

    /** DynMesh written back into StaticMesh: null, or the error reply. */
    TSharedPtr<FJsonObject> UVMapping_CopyToStaticMesh(UDynamicMesh* DynMesh, UStaticMesh* StaticMesh)
    {
        FGeometryScriptCopyMeshToAssetOptions WriteOpts;
        EGeometryScriptOutcomePins WriteOutcome = EGeometryScriptOutcomePins::Failure;
        FGeometryScriptMeshWriteLOD WriteLOD;
        UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshToStaticMesh(DynMesh, StaticMesh, WriteOpts, WriteLOD, WriteOutcome);
        if (WriteOutcome != EGeometryScriptOutcomePins::Success)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("CopyMeshToStaticMesh failed (outcome=%d)"), static_cast<int32>(WriteOutcome)));
        }
        return nullptr;
    }
}

// MCP:list_uv_channels - List the UV channels of a static mesh asset per LOD: for each LOD uv_channel_count, vertex_count, triangle_count, the mesh's lightmap_coordinate_index and has_lightmap (that index names an existing channel), and one entry per channel with is_lightmap. Use it first to see which LODs and channels exist; then get_uv_health_report checks one channel's layout, get_static_mesh_geometry_stats measures UV area, and set_static_mesh_lightmap changes the lightmap channel. Read-only and idempotent: no undo step, nothing dirtied or saved. Channel and vertex counts come from the built render data, triangle_count from the LOD's source mesh description (0 when it has none); each channel's vertex_count is the LOD's vertex count. A mesh that does not load, or a lod_index past the last LOD, is an error.
//   @param mesh_path - Content path of the UStaticMesh asset, e.g. "/Game/Meshes/SM_Rock" (the object path "/Game/Meshes/SM_Rock.SM_Rock" also works); find_assets_by_name finds it
//   @param lod_index - Optional 0-based LOD to inspect, or -1 to list every LOD (default -1); total_lods in the reply says how many exist, and one past the last is an error
//   @returns { mesh_path: string, total_lods: int, lods: [{lod_index, uv_channel_count, vertex_count, triangle_count, lightmap_coordinate_index, has_lightmap, channels: [{channel, is_lightmap, vertex_count}]}] }
TSharedPtr<FJsonObject> HandleListUVChannels(const TSharedPtr<FJsonObject>& Params)
{
    FString MeshPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("mesh_path"), MeshPath)) { return Missing; }
    int32 LodIndex = -1;
    Params->TryGetNumberField(TEXT("lod_index"), LodIndex);

    UStaticMesh* Mesh = nullptr;
    if (TSharedPtr<FJsonObject> LoadError = UVMapping_LoadMesh(MeshPath, TEXT("Could not load static mesh"), Mesh)) { return LoadError; }

    const int32 TotalLODs = Mesh->GetNumLODs();
    const int32 LightmapIdx = Mesh->GetLightMapCoordinateIndex();

    auto BuildLodBlock = [&](int32 LOD) -> TSharedPtr<FJsonObject>
    {
        TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
        const int32 UVChannels = Mesh->GetNumUVChannels(LOD);
        const int32 VertexCount = Mesh->GetNumVertices(LOD);
        int32 TriangleCount = 0;
        if (const FMeshDescription* MD = Mesh->GetMeshDescription(LOD))
        {
            TriangleCount = MD->Triangles().Num();
        }
        O->SetNumberField(TEXT("lod_index"), LOD);
        O->SetNumberField(TEXT("uv_channel_count"), UVChannels);
        O->SetNumberField(TEXT("vertex_count"), VertexCount);
        O->SetNumberField(TEXT("triangle_count"), TriangleCount);
        O->SetNumberField(TEXT("lightmap_coordinate_index"), LightmapIdx);
        O->SetBoolField(TEXT("has_lightmap"), LightmapIdx >= 0 && LightmapIdx < UVChannels);

        TArray<TSharedPtr<FJsonValue>> Chans;
        for (int32 c = 0; c < UVChannels; ++c)
        {
            TSharedPtr<FJsonObject> C = MakeShared<FJsonObject>();
            C->SetNumberField(TEXT("channel"), c);
            C->SetBoolField(TEXT("is_lightmap"), c == LightmapIdx);
            C->SetNumberField(TEXT("vertex_count"), VertexCount);  // UV vertex count = vertex count
            Chans.Add(MakeShared<FJsonValueObject>(C));
        }
        O->SetArrayField(TEXT("channels"), Chans);
        return O;
    };

    TArray<TSharedPtr<FJsonValue>> LodArr;
    if (LodIndex >= 0)
    {
        if (LodIndex >= TotalLODs)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("lod_index %d out of range (mesh has %d LODs)"), LodIndex, TotalLODs));
        }
        LodArr.Add(MakeShared<FJsonValueObject>(BuildLodBlock(LodIndex)));
    }
    else
    {
        for (int32 LOD = 0; LOD < TotalLODs; ++LOD)
        {
            LodArr.Add(MakeShared<FJsonValueObject>(BuildLodBlock(LOD)));
        }
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("mesh_path"), Mesh->GetPathName());
    Resp->SetNumberField(TEXT("total_lods"), TotalLODs);
    Resp->SetArrayField(TEXT("lods"), LodArr);
    return mcp_common::MakeOkReply(Resp);
}

// MCP:get_uv_health_report - Check the layout of one UV channel of one LOD of a static mesh asset, reading its built render data: in_unit_square_triangle_ratio (share of triangles with all three UVs inside 0..1), overlapping_triangle_count (triangles whose UV triangle overlaps another one's with positive area; a shared edge or corner does not count, so a clean unwrap gives 0 and a lightmap channel should), overlap_test_complete, approx_unique_islands, plus vertex and triangle counts, lightmap_coordinate_index and is_lightmap_hard_failing (no lightmap index set, or no UV channels). Pick the LOD and channel with list_uv_channels first; change the lightmap channel with set_static_mesh_lightmap, and edit the asset's UVs with set_triangle_uvs or clear_uv_channel. Read-only and idempotent; nothing dirtied or saved. A mesh that does not load, or a LOD or channel out of range, is an error.
//   @param mesh_path - Content path of the UStaticMesh asset, e.g. "/Game/Meshes/SM_Rock" (the object path "/Game/Meshes/SM_Rock.SM_Rock" also works); find_assets_by_name finds it
//   @param lod_index - 0-based LOD to inspect (default 0), below the total_lods list_uv_channels reports
//   @param uv_channel - 0-based UV channel to inspect (default 0), below that LOD's uv_channel_count from list_uv_channels
//   @returns { mesh_path, lod_index, uv_channel, vertex_count, triangle_count, lightmap_coordinate_index, no_lightmap_configured: bool, is_lightmap_hard_failing: bool, overlapping_triangle_count: int, overlap_test_complete: bool, in_unit_square_triangle_ratio, approx_unique_islands, per_triangle_iteration_error?: string } -- overlapping_triangle_count: triangles overlapping another in UV space with positive area (a lower bound when overlap_test_complete is false: the test stops after 20 million pair visits); approx_unique_islands counts distinct UV positions (rounded to 4 decimals), not islands; when per_triangle_iteration_error is set the per-triangle counters stay 0.
//
// Per-vertex UV iteration goes through the BAKED
// FStaticMeshVertexBuffer — same data the GPU rasterizer reads. The path is:
//   Mesh->GetRenderData()->LODResources[LODIndex] (IndexBuffer + VertexBuffers).
//   For each baked triangle, pull UV[uv_channel] from the vertex buffer and:
//     - mark in-unit-square (all 3 UVs in [0,1]^2),
//     - collect the distinct rounded UVs (= approx_unique_islands),
//     - keep the UV triangle for the overlap test (grid + separating axes) after the pass.
// If the render data isn't built (uncooked / very-new mesh), the per-triangle
// counters come back as 0 and `per_triangle_iteration_error` carries the
// reason; the rest of the response is still well-shaped.
TSharedPtr<FJsonObject> HandleGetUVHealthReport(const TSharedPtr<FJsonObject>& Params)
{
    FString MeshPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("mesh_path"), MeshPath)) { return Missing; }
    int32 LodIndex = 0;
    Params->TryGetNumberField(TEXT("lod_index"), LodIndex);
    int32 UVChannel = 0;
    Params->TryGetNumberField(TEXT("uv_channel"), UVChannel);

    UStaticMesh* Mesh = nullptr;
    if (TSharedPtr<FJsonObject> LoadError = UVMapping_LoadMesh(MeshPath, TEXT("Could not load static mesh"), Mesh)) { return LoadError; }
    if (LodIndex < 0 || LodIndex >= Mesh->GetNumLODs())
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("lod_index %d out of range (mesh has %d LODs)"), LodIndex, Mesh->GetNumLODs()));
    }

    const int32 LightmapIdx = Mesh->GetLightMapCoordinateIndex();
    const int32 UVChCount = Mesh->GetNumUVChannels(LodIndex);
    if (UVChannel < 0 || UVChannel >= UVChCount)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("uv_channel %d out of range (LOD %d has %d UV channels)"), UVChannel, LodIndex, UVChCount));
    }

    const int32 VertexCount = Mesh->GetNumVertices(LodIndex);
    int32 TriangleCount = 0;
    if (const FMeshDescription* MD = Mesh->GetMeshDescription(LodIndex))
    {
        TriangleCount = MD->Triangles().Num();
    }

    // -- Lightmap hard-fail detection (unchanged from the base version).
    const bool bNoLightmapConfigured = (LightmapIdx < 0);
    const bool bIsLightmapHardFailing = bNoLightmapConfigured || (UVChCount == 0);

    // -- Per-triangle UV iteration through the BAKED render-data vertex buffer:
    //    in-unit-square count, the distinct UV positions (rounded to 4 decimals),
    //    and every triangle's three UV corners for the UV-space overlap test after
    //    the pass.
    int32 TrianglesInUnitSquare = 0;
    int32 TrianglesTotal = 0;
    int32 ApproxUniqueIslands = 0;
    TArray<FVector2D> UVCorners; // three per tested triangle
    FString PerTriError;

    auto RoundUV = [](const FVector2f& V) -> FVector2D
    {
        return FVector2D(FMath::RoundToFloat(V.X * 10000.f) / 10000.f,
                         FMath::RoundToFloat(V.Y * 10000.f) / 10000.f);
    };
    auto InUnitSquare = [](const FVector2f& V) -> bool
    {
        return V.X >= 0.f && V.X <= 1.f && V.Y >= 0.f && V.Y <= 1.f;
    };

    const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
    if (!RenderData)
    {
        PerTriError = TEXT("render data unavailable (mesh may not have been built or rendered yet)");
    }
    else if (LodIndex >= RenderData->LODResources.Num())
    {
        PerTriError = FString::Printf(TEXT("render data has %d LODs, lod_index %d out of range"),
                                      RenderData->LODResources.Num(), LodIndex);
    }
    else
    {
        const FStaticMeshLODResources& LODRes = RenderData->LODResources[LodIndex];
        const FStaticMeshVertexBuffer& VertBuf = LODRes.VertexBuffers.StaticMeshVertexBuffer;
        const int32 RenderUVChCount = VertBuf.GetNumTexCoords();
        const int32 NumRenderVerts = VertBuf.GetNumVertices();

        if (UVChannel >= RenderUVChCount)
        {
            // Authoring FD told us this UV channel exists, but the baked vertex
            // buffer doesn't reflect it (would happen on a half-built mesh or
            // partial UV-channel cover). Document via per_tri_error rather than
            // throw — other fields still answer.
            PerTriError = FString::Printf(
                TEXT("uv_channel %d not present in baked vertex buffer (RenderUVChCount=%d); per-vertex stats left at 0"),
                UVChannel, RenderUVChCount);
        }
        else
        {
            const FIndexArrayView IndicesView = LODRes.IndexBuffer.GetArrayView();
            const int32 NumIndices = IndicesView.Num();
            const int32 NumTris = NumIndices / 3;

            if (NumTris == 0)
            {
                PerTriError = TEXT("baked LOD has zero triangles");
            }
            else
            {
                TSet<FVector2D> RoundedUVs;
                RoundedUVs.Reserve(FMath::Max(256, NumRenderVerts));
                UVCorners.Reserve(NumTris * 3);

                for (int32 T = 0; T < NumTris; ++T)
                {
                    const int32 IBase = T * 3;
                    const uint32 I0 = IndicesView[IBase + 0];
                    const uint32 I1 = IndicesView[IBase + 1];
                    const uint32 I2 = IndicesView[IBase + 2];
                    // Bounds-check against NumRenderVerts: baked index buffer should
                    // never reference OOR vertex indices, but a malformed mesh COULD.
                    // Skip such triangles with a once-only note.
                    if (I0 >= static_cast<uint32>(NumRenderVerts) ||
                        I1 >= static_cast<uint32>(NumRenderVerts) ||
                        I2 >= static_cast<uint32>(NumRenderVerts))
                    {
                        if (PerTriError.IsEmpty())
                        {
                            PerTriError = TEXT("baked index references out-of-range vertex; skipping malformed triangles");
                        }
                        continue;
                    }

                    const FVector2f UV0 = VertBuf.GetVertexUV(I0, UVChannel);
                    const FVector2f UV1 = VertBuf.GetVertexUV(I1, UVChannel);
                    const FVector2f UV2 = VertBuf.GetVertexUV(I2, UVChannel);
                    ++TrianglesTotal;
                    if (InUnitSquare(UV0) && InUnitSquare(UV1) && InUnitSquare(UV2))
                    {
                        ++TrianglesInUnitSquare;
                    }
                    RoundedUVs.Add(RoundUV(UV0));
                    RoundedUVs.Add(RoundUV(UV1));
                    RoundedUVs.Add(RoundUV(UV2));
                    UVCorners.Add(FVector2D(UV0));
                    UVCorners.Add(FVector2D(UV1));
                    UVCorners.Add(FVector2D(UV2));
                }
                // Distinct rounded UV positions, the historical approx_unique_islands.
                ApproxUniqueIslands = RoundedUVs.Num();
            }
        }
    }

    const float InUnitSquareRatio = (TrianglesTotal > 0)
        ? static_cast<float>(TrianglesInUnitSquare) / static_cast<float>(TrianglesTotal)
        : 0.0f;

    // Triangles whose UV triangle overlaps another one's with positive area (a shared edge or corner is not an
    // overlap): what a lightmap channel must not have, and what a stacked or mirrored layout has on purpose.
    const FUVOverlapCount Overlap = UVMapping_CountOverlappingTriangles(UVCorners, UVMapping_MaxOverlapPairVisits);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("mesh_path"), Mesh->GetPathName());
    Resp->SetNumberField(TEXT("lod_index"), LodIndex);
    Resp->SetNumberField(TEXT("uv_channel"), UVChannel);
    Resp->SetNumberField(TEXT("vertex_count"), VertexCount);
    Resp->SetNumberField(TEXT("triangle_count"), TriangleCount);
    Resp->SetNumberField(TEXT("lightmap_coordinate_index"), LightmapIdx);
    Resp->SetBoolField(TEXT("no_lightmap_configured"), bNoLightmapConfigured);
    Resp->SetBoolField(TEXT("is_lightmap_hard_failing"), bIsLightmapHardFailing);
    Resp->SetNumberField(TEXT("overlapping_triangle_count"), Overlap.OverlappingTriangles);
    Resp->SetBoolField(TEXT("overlap_test_complete"), !Overlap.bCapped);
    Resp->SetNumberField(TEXT("in_unit_square_triangle_ratio"), InUnitSquareRatio);
    Resp->SetNumberField(TEXT("approx_unique_islands"), ApproxUniqueIslands);
    if (!PerTriError.IsEmpty())
    {
        Resp->SetStringField(TEXT("per_triangle_iteration_error"), PerTriError);
    }
    return mcp_common::MakeOkReply(Resp);
}

// ===========================================================================
// Write-side UV ops (static <-> dynamic mesh round-trip)
// Pattern (mirrors GeometryScript handlers):
//   1. LoadObject<UStaticMesh>(static_mesh_path)
//   2. NewObject<UDynamicMesh>(GetTransientPackage()) -> CopyMeshFromStaticMesh
//   3. UGeometryScriptLibrary_MeshUVFunctions::<Op>(DynamicMesh, ...)
//   4. CopyMeshToStaticMesh (writes back to the UStaticMesh asset)
//   5. SaveAssetPackage
// ===========================================================================

// MCP:clear_uv_channel - Drop every UV from one UV channel of LOD 0 of a UStaticMesh asset: LOD 0 (its source model, or its render data when it has none) is copied to a transient dynamic mesh, the channel's UVs are cleared there, and the copy is written back over LOD 0 in one undo step and the package saved. The channel itself stays, so the channel count does not change, and an already-empty channel is still written and saved. A uv_channel LOD 0 lacks, a mesh with no source model, a path that does not load or a mesh under /Engine is an error and nothing changes; a failed save makes success false. Inventory first: list_uv_channels; one triangle: set_triangle_uvs; channel count of a DynamicMeshActor: set_num_uv_channels.
//   @param static_mesh_path - Asset path to the UStaticMesh
//   @param uv_channel - Index of the UV channel to clear (0-based; default 0); one LOD 0 does not have is an error
//   @returns { static_mesh_path, uv_channel, uv_channel_count_before, uv_channel_count_after, cleared, saved }
TSharedPtr<FJsonObject> HandleClearUVChannel(const TSharedPtr<FJsonObject>& Params)
{
    FString MeshPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("static_mesh_path"), MeshPath)) { return Missing; }
    int32 UVChannel = 0;
    { double D = 0.0; if (Params->TryGetNumberField(TEXT("uv_channel"), D)) { UVChannel = static_cast<int32>(D); } }

    UStaticMesh* StaticMesh = nullptr;
    if (TSharedPtr<FJsonObject> LoadError = UVMapping_LoadMesh(MeshPath, TEXT("Cannot load static mesh"), StaticMesh)) { return LoadError; }
    const int32 UVCountBefore = StaticMesh->GetNumUVChannels(0);

    UDynamicMesh* DynMesh = nullptr;
    if (TSharedPtr<FJsonObject> CopyError = UVMapping_CopyToDynamicMesh(StaticMesh, DynMesh)) { return CopyError; }
    // ClearUVChannel leaves a channel the copy does not have untouched and says so only in its debug object, so such a
    // channel is refused here, before the copy would be written back and saved unchanged.
    const int32 CopyUVChannels = UGeometryScriptLibrary_MeshQueryFunctions::GetNumUVSets(DynMesh);
    if (UVChannel < 0 || UVChannel >= CopyUVChannels)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("uv_channel %d does not exist: LOD 0 of %s has %d UV channels; nothing was changed"),
            UVChannel, *StaticMesh->GetPathName(), CopyUVChannels));
    }
    UGeometryScriptLibrary_MeshUVFunctions::ClearUVChannel(DynMesh, UVChannel, /*Debug*/ nullptr);

    if (TSharedPtr<FJsonObject> WriteError = UVMapping_CopyToStaticMesh(DynMesh, StaticMesh)) { return WriteError; }
    StaticMesh->Modify();
    const bool bSaved = mcp_common::SaveAssetPackage(StaticMesh->GetPackage(), StaticMesh);
    const int32 UVCountAfter = StaticMesh->GetNumUVChannels(0);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("static_mesh_path"), StaticMesh->GetPathName());
    Data->SetNumberField(TEXT("uv_channel"), UVChannel);
    Data->SetNumberField(TEXT("uv_channel_count_before"), UVCountBefore);
    Data->SetNumberField(TEXT("uv_channel_count_after"), UVCountAfter);
    Data->SetBoolField(TEXT("cleared"), true);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("clear_uv_channel"));
}

// MCP:set_triangle_uvs - Set the three UV corners of one triangle in one UV channel of LOD 0 of a UStaticMesh asset, a granular edit without a repack: LOD 0 (its source model, or its render data when it has none) is copied to a transient dynamic mesh, the triangle gets three new UV elements there, so it becomes its own UV island, and the copy is written back over LOD 0 in one undo step and the package saved. triangle_id counts the copy's triangles from 0. A negative uv_channel, a triangle or channel the copy lacks, a missing corner, a path that does not load or a mesh under /Engine is an error and nothing changes; a failed save makes success false. Whole channel: clear_uv_channel; check the result: get_uv_health_report.
//   @param static_mesh_path - Asset path to the UStaticMesh
//   @param uv_channel - UV channel index (default 0); a negative one, or one LOD 0 does not have, is an error
//   @param triangle_id - Triangle ID to update (0-based; default 0)
//   @param uv0 [array] - First triangle corner as a [u, v] pair of numbers (required).
//   @param uv1 [array] - Second corner as [u, v] (required).
//   @param uv2 [array] - Third corner as [u, v] (required).
//   @returns { static_mesh_path, uv_channel, triangle_id, is_valid_triangle, modified, saved }
TSharedPtr<FJsonObject> HandleSetTriangleUVs(const TSharedPtr<FJsonObject>& Params)
{
    FString MeshPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("static_mesh_path"), MeshPath)) { return Missing; }
    int32 UVChannel = 0;
    { double D = 0.0; if (Params->TryGetNumberField(TEXT("uv_channel"), D)) { UVChannel = static_cast<int32>(D); } }
    int32 TriangleID = 0;
    { double D = 0.0; if (Params->TryGetNumberField(TEXT("triangle_id"), D)) { TriangleID = static_cast<int32>(D); } }
    // SetMeshTriangleUVs checks only the upper bound of the channel, so a negative one would index UV layer -1.
    if (UVChannel < 0)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("uv_channel must be >= 0 (got %d); nothing was changed"), UVChannel));
    }

    // Parse the 3 UVs. Accept either separate uv0/uv1/uv2 fields (arrays) or flat u0/v0/u1/v1/u2/v2 numbers.
    auto ParseUV = [&](const TCHAR* FieldName, FVector2D& Out) -> bool
    {
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (Params->TryGetArrayField(FString(FieldName), Arr) && Arr && Arr->Num() == 2)
        {
            double U = 0.0, V = 0.0;
            (*Arr)[0]->TryGetNumber(U);
            (*Arr)[1]->TryGetNumber(V);
            Out = FVector2D(U, V);
            return true;
        }
        return false;
    };
    FVector2D UV0, UV1, UV2;
    if (!ParseUV(TEXT("uv0"), UV0) || !ParseUV(TEXT("uv1"), UV1) || !ParseUV(TEXT("uv2"), UV2))
    {
        return mcp_common::MakeErrorReply(TEXT(
            "uv0, uv1, uv2 are required (each an [u, v] array of two numbers)"));
    }

    UStaticMesh* StaticMesh = nullptr;
    if (TSharedPtr<FJsonObject> LoadError = UVMapping_LoadMesh(MeshPath, TEXT("Cannot load static mesh"), StaticMesh)) { return LoadError; }

    UDynamicMesh* DynMesh = nullptr;
    if (TSharedPtr<FJsonObject> CopyError = UVMapping_CopyToDynamicMesh(StaticMesh, DynMesh)) { return CopyError; }

    const FGeometryScriptUVTriangle UVs{ UV0, UV1, UV2 };
    bool bIsValidTriangle = false;
    UGeometryScriptLibrary_MeshUVFunctions::SetMeshTriangleUVs(
        DynMesh, UVChannel, TriangleID, UVs, bIsValidTriangle, /*bDeferChangeNotifications=*/false);

    if (!bIsValidTriangle)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("SetMeshTriangleUVs reported triangle %d as invalid in channel %d (out of range or topology rejected)"),
            TriangleID, UVChannel));
    }

    if (TSharedPtr<FJsonObject> WriteError = UVMapping_CopyToStaticMesh(DynMesh, StaticMesh)) { return WriteError; }
    StaticMesh->Modify();
    const bool bSaved = mcp_common::SaveAssetPackage(StaticMesh->GetPackage(), StaticMesh);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("static_mesh_path"), StaticMesh->GetPathName());
    Data->SetNumberField(TEXT("uv_channel"), UVChannel);
    Data->SetNumberField(TEXT("triangle_id"), TriangleID);
    Data->SetBoolField(TEXT("is_valid_triangle"), bIsValidTriangle);
    Data->SetBoolField(TEXT("modified"), true);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_triangle_uvs"));
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("clear_uv_channel"), &HandleClearUVChannel },
    { TEXT("get_uv_health_report"), &HandleGetUVHealthReport },
    { TEXT("list_uv_channels"), &HandleListUVChannels },
    { TEXT("set_triangle_uvs"), &HandleSetTriangleUVs },
};
} // namespace UEAgentUVMappingCommands
