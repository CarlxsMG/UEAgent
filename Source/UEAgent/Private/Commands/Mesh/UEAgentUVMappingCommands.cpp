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

    /** StaticMesh (highest available LOD, build settings applied) copied into a new transient dynamic mesh:
     *  null once OutDynMesh holds it, otherwise the error reply. */
    TSharedPtr<FJsonObject> UVMapping_CopyToDynamicMesh(UStaticMesh* StaticMesh, UDynamicMesh*& OutDynMesh)
    {
        OutDynMesh = NewObject<UDynamicMesh>(GetTransientPackage());
        FGeometryScriptCopyMeshFromAssetOptions Opts;
        Opts.bApplyBuildSettings = true;
        FGeometryScriptMeshReadLOD ReadLOD;
        ReadLOD.LODType = EGeometryScriptLODType::MaxAvailable;
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

// MCP:list_uv_channels - Per-LOD inventory of UV channels on a static mesh (read-only, no MarkDirty / no save).
//   @param mesh_path - Static mesh path (e.g. /Game/Meshes/Cube)
//   @param lod_index - Optional LOD index to inspect, or -1 to inventory every LOD (default -1)
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

// MCP:get_uv_health_report - Read-only diagnostic summary for one UV channel on a static mesh: counts, lightmap status, in-unit-square ratio, overlap count, and unique UVs.
//   @param mesh_path - Static mesh path (e.g. /Game/Meshes/Cube)
//   @param lod_index - LOD index to inspect (default 0)
//   @param uv_channel - UV channel index to inspect (default 0)
//   @returns { mesh_path, lod_index, uv_channel, vertex_count, triangle_count,
//             lightmap_coordinate_index, no_lightmap_configured, is_lightmap_hard_failing,
//             approx_overlap_count, in_unit_square_triangle_ratio, approx_unique_islands,
//             per_triangle_iteration_error?: string }
//
// Per-vertex UV iteration goes through the BAKED
// FStaticMeshVertexBuffer — same data the GPU rasterizer reads. The path is:
//   Mesh->GetRenderData()->LODResources[LODIndex] (IndexBuffer + VertexBuffers).
//   For each baked triangle, pull UV[uv_channel] from the vertex buffer and:
//     - count overlap (rounded 4-decimal duplicate via TSet),
//     - mark in-unit-square (all 3 UVs in [0,1]^2),
//     - publish unique-UVs after the pass (= approx_unique_islands).
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

    // -- Per-vertex UV iteration through the BAKED render-data vertex buffer.
    //    Two passes through every baked triangle vertex: in-unit-square + overlap
    //    count. UVs are rounded to 4-decimal precision (~1cm granularity on a
    //    [0,1] UV plane — coarser overlaps than that would not be a real
    //    "shared pixel" anyway).
    int32 TrianglesInUnitSquare = 0;
    int32 TrianglesTotal = 0;
    int32 OverlapCount = 0;
    int32 OverlapCheckedVertices = 0;
    int32 ApproxUniqueIslands = 0;
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
                    auto AddAndCheck = [&](const FVector2D& R)
                    {
                        FSetElementId Existing = RoundedUVs.FindId(R);
                        if (Existing.IsValidId())
                        {
                            ++OverlapCount;
                        }
                        else
                        {
                            RoundedUVs.Add(R);
                        }
                        ++OverlapCheckedVertices;
                    };
                    AddAndCheck(RoundUV(UV0));
                    AddAndCheck(RoundUV(UV1));
                    AddAndCheck(RoundUV(UV2));
                }
                // island approx = unique rounded UV positions post-overlap-pass
                ApproxUniqueIslands = RoundedUVs.Num();
            }
        }
    }

    const float InUnitSquareRatio = (TrianglesTotal > 0)
        ? static_cast<float>(TrianglesInUnitSquare) / static_cast<float>(TrianglesTotal)
        : 0.0f;

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("mesh_path"), Mesh->GetPathName());
    Resp->SetNumberField(TEXT("lod_index"), LodIndex);
    Resp->SetNumberField(TEXT("uv_channel"), UVChannel);
    Resp->SetNumberField(TEXT("vertex_count"), VertexCount);
    Resp->SetNumberField(TEXT("triangle_count"), TriangleCount);
    Resp->SetNumberField(TEXT("lightmap_coordinate_index"), LightmapIdx);
    Resp->SetBoolField(TEXT("no_lightmap_configured"), bNoLightmapConfigured);
    Resp->SetBoolField(TEXT("is_lightmap_hard_failing"), bIsLightmapHardFailing);
    Resp->SetNumberField(TEXT("approx_overlap_count"), OverlapCount);
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

// MCP:clear_uv_channel - Drop all UVs from one UV channel on a static mesh. Idempotent on already-empty channels. Resizes the UV-channel count if the cleared channel is the highest one (UE 5.8 doesn't auto-shrink channel count, so callers that want to "remove" a UV channel should follow up with set_num_uv_channels).
//   @param static_mesh_path - Asset path to the UStaticMesh
//   @param uv_channel - Index of the UV channel to clear (0-based; default 0)
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

// MCP:set_triangle_uvs - Set the 3 UV corners of a single triangle in one UV channel. Use this for granular UV edits without a full repack / regeneration. The triangle becomes an isolated UV island (the engine allocates fresh UV elements per call). Triangle IDs are 0-based into the mesh's triangle list. The bIsValidTriangle out-param surfaces the engine's validity verdict (false if TriangleID is out of range, the UV channel doesn't exist, or the resulting topology would be invalid).
//   @param static_mesh_path - Asset path to the UStaticMesh
//   @param uv_channel - UV channel index (default 0)
//   @param triangle_id - Triangle ID to update (0-based; default 0)
//   @param uv0 [array] - First triangle corner as a [u, v] pair. Read by the local ParseUV helper, which the extractor cannot follow, so this hint is what puts the param in the schema at all -- documenting the three on one line made all three unparseable and the tool had NO way to pass UVs.
//   @param uv1 [array] - Second corner as [u, v].
//   @param uv2 [array] - Third corner as [u, v]. Alternatively pass the six flat numbers u0/v0/u1/v1/u2/v2 instead of the three arrays.
//   @returns { static_mesh_path, uv_channel, triangle_id, is_valid_triangle, modified, saved }
TSharedPtr<FJsonObject> HandleSetTriangleUVs(const TSharedPtr<FJsonObject>& Params)
{
    FString MeshPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("static_mesh_path"), MeshPath)) { return Missing; }
    int32 UVChannel = 0;
    { double D = 0.0; if (Params->TryGetNumberField(TEXT("uv_channel"), D)) { UVChannel = static_cast<int32>(D); } }
    int32 TriangleID = 0;
    { double D = 0.0; if (Params->TryGetNumberField(TEXT("triangle_id"), D)) { TriangleID = static_cast<int32>(D); } }

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
