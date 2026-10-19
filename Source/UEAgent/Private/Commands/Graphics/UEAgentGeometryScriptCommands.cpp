#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "JsonObjectConverter.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"

// GeometryFramework (engine Runtime module): dynamic mesh actor / component / mesh.
#include "DynamicMeshActor.h"                       // ADynamicMeshActor
#include "Components/DynamicMeshComponent.h"        // UDynamicMeshComponent::GetDynamicMesh()/NotifyMeshUpdated()
#include "UDynamicMesh.h"                           // UDynamicMesh::GetTriangleCount()
#include "Changes/MeshReplacementChange.h"          // FMeshReplacementChange: the undo record of a dynamic mesh edit
#include "Misc/ITransaction.h"                      // ITransaction::StoreUndo (GUndo)

// GeometryScriptingCore (plugin GeometryScripting): blueprint function libraries.
#include "GeometryScript/MeshPrimitiveFunctions.h"  // UGeometryScriptLibrary_MeshPrimitiveFunctions + FGeometryScriptPrimitiveOptions
#include "GeometryScript/MeshQueryFunctions.h"      // UGeometryScriptLibrary_MeshQueryFunctions::GetVertexCount
#include "GeometryScript/MeshBooleanFunctions.h"    // UGeometryScriptLibrary_MeshBooleanFunctions::ApplyMeshBoolean
#include "GeometryScript/MeshSpatialFunctions.h"    // RebuildBVHForMesh (BVH lifecycle)
#include "GeometryScript/MeshUVFunctions.h"         // RotateMeshUVs, SetMeshUVElementPosition
#include "GeometryScript/MeshBasicEditFunctions.h"  // UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMesh + FGeometryScriptAppendMeshOptions
#include "GeometryScript/MeshTransformFunctions.h"  // UGeometryScriptLibrary_MeshTransformFunctions::TransformMesh / TranslateMesh / RotateMesh / ScaleMesh
#include "GeometryScript/MeshNormalsFunctions.h"    // UGeometryScriptLibrary_MeshNormalsFunctions::RecomputeNormals + FGeometryScriptCalculateNormalsOptions
#include "GeometryScript/MeshSimplifyFunctions.h"   // UGeometryScriptLibrary_MeshSimplifyFunctions::ApplySimplifyToTriangleCount + FGeometryScriptSimplifyMeshOptions
#include "GeometryScript/MeshRepairFunctions.h"    // UGeometryScriptLibrary_MeshRepairFunctions::WeldMeshEdges / RemoveUnusedVertices / CompactMesh + FGeometryScriptWeldEdgesOptions
#include "GeometryScript/MeshRemeshFunctions.h"    // UGeometryScriptLibrary_RemeshingFunctions::ApplyUniformRemesh / ApplyAdaptiveRemesh + FGeometryScriptRemeshOptions / FGeometryScriptUniformRemeshOptions / FGeometryScriptAdaptiveRemeshOptions + EGeometryScriptUniformRemeshTargetType / EGeometryScriptRemeshSmoothingType (GeometryScriptingCore module already in Build.cs)
#include "GeometryScript/MeshVoxelFunctions.h"    // UGeometryScriptLibrary_MeshVoxelFunctions + FGeometryScriptSolidifyOptions + FGeometryScriptMorphologyOptions + EGeometryScriptMorphologicalOpType
// StaticMesh -> UDynamicMesh bridge for read-only queries
#include "GeometryScript/MeshAssetFunctions.h"   // UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromStaticMesh (signature: returns UDynamicMesh*, takes FGeometryScriptCopyMeshFromAssetOptions + FGeometryScriptMeshReadLOD + EGeometryScriptOutcomePins)
#include "GeometryScript/GeometryScriptTypes.h"   // FGeometryScriptCopyMeshFromAssetOptions / FGeometryScriptMeshReadLOD / EGeometryScriptLODType / EGeometryScriptOutcomePins + FGeometryScriptCopyMeshToAssetOptions / FGeometryScriptMeshWriteLOD (DynamicMesh -> StaticMesh)
#include "GeometryScript/MeshPolygroupFunctions.h" // UGeometryScriptLibrary_MeshPolygroupFunctions::SetNumExtendedPolygroupLayers
// Per-triangle MaterialIDs (the write half).
#include "GeometryScript/MeshMaterialFunctions.h"  // UGeometryScriptLibrary_MeshMaterialFunctions::{EnableMaterialIDs,ClearMaterialIDs,SetMaterialIDOnTriangles,SetPolygroupMaterialID,SetMaterialIDForMeshSelection,GetMaxMaterialID,GetAllTriangleMaterialIDs,GetTrianglesByMaterialID,RemapMaterialIDs,CompactMaterialIDs}
#include "GeometryScript/MeshSelectionFunctions.h" // UGeometryScriptLibrary_MeshSelectionFunctions::SelectMeshElements{InBox,InSphere,WithPlane,ByNormalAngle,ByMaterialID,ByPolygroup}
#include "GeometryScript/GeometryScriptSelectionTypes.h" // FGeometryScriptMeshSelection::{GetNumSelected,ConvertToMeshIndexArray}
#include "DynamicMesh/DynamicMesh3.h"             // FDynamicMesh3 (GeometryCore) — needed by ConvertToMeshIndexArray via UDynamicMesh::ProcessMesh
#include "VectorTypes.h"                          // UE::Geometry::Normalized / IsNormalized (GeometryCore): the ray direction check of find_nearest_ray_intersection_with_mesh
// Mesh map ("what is this piece?") + UV unwrapping.
#include "Selections/MeshConnectedComponents.h"   // UE::Geometry::FMeshConnectedComponents::FindConnectedTriangles (per-island triangle lists; READ-ONLY, unlike SplitMeshByComponents)
#include "GeometryScript/MeshSelectionQueryFunctions.h" // UGeometryScriptLibrary_MeshSelectionQueryFunctions::GetMeshSelectionBoundingBox
#include "Engine/StaticMesh.h"                   // UStaticMesh (loaded by path -> CopyMeshFromStaticMesh / CopyMeshToStaticMesh)
#include "StaticMeshResources.h"                 // MAX_STATIC_MESH_LODS (the LOD range copy_dynamic_mesh_to_static_mesh accepts)
// Material slot set on UDynamicMeshComponent.
#include "Materials/MaterialInterface.h"        // UMaterialInterface (loaded by path for set_dynamic_mesh_material)

#define LOCTEXT_NAMESPACE "UEAgentGeometryScript"

/**
 * Geometry Script dynamic-mesh and static-mesh commands.
 *
 * Spawns ADynamicMeshActor instances into the editor world and procedurally builds geometry on their
 * UDynamicMeshComponent->GetDynamicMesh() (UDynamicMesh) using the GeometryScripting plugin's blueprint
 * function libraries (UGeometryScriptLibrary_*).
 *
 * The family also exposes read-only static-mesh asset queries: CopyMeshFromStaticMesh
 * (UGeometryScriptLibrary_StaticMeshFunctions) extracts a transient UDynamicMesh from a UStaticMesh asset and
 * the query runs on that copy -- headless-safe, no save, no asset mutation.
 *
 * Modules: GeometryScriptingCore (plugin GeometryScripting) + GeometryFramework
 * (engine Runtime module: ADynamicMeshActor / UDynamicMeshComponent / UDynamicMesh).
 */
namespace UEAgentGeometryScriptCommands
{

// MCP-DOMAIN: GeometryScript

namespace
{
    /**
     * Append the requested primitive ("Box"|"Sphere"|"Cylinder"|"Cone") to TargetMesh
     * at the given local Transform, using Size as the dominant dimension.
     * Returns false (with OutError set) if the primitive type is unknown.
     *
     * MaterialID tags the appended triangles so a later
     * copy_dynamic_mesh_to_static_mesh (which already runs with bUseSectionMaterials=true)
     * emits them as their own section/slot. 0 = no separate section.
     */
    bool AppendPrimitiveToMesh(UDynamicMesh* TargetMesh, const FString& Primitive,
                               const FTransform& Transform, float Size, FString& OutError,
                               int32 MaterialID = 0)
    {
        if (!TargetMesh)
        {
            OutError = TEXT("Null target mesh");
            return false;
        }

        FGeometryScriptPrimitiveOptions Options; // defaults: PerFace polygroups, MaterialID 0
        Options.MaterialID = MaterialID;

        const FString Prim = Primitive.ToLower();
        if (Prim == TEXT("box"))
        {
            UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
                TargetMesh, Options, Transform,
                /*DimensionX*/ Size, /*DimensionY*/ Size, /*DimensionZ*/ Size,
                /*StepsX*/ 0, /*StepsY*/ 0, /*StepsZ*/ 0,
                EGeometryScriptPrimitiveOriginMode::Center);
            return true;
        }
        if (Prim == TEXT("sphere"))
        {
            UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendSphereLatLong(
                TargetMesh, Options, Transform,
                /*Radius*/ Size * 0.5f,
                /*StepsPhi*/ 12, /*StepsTheta*/ 16,
                EGeometryScriptPrimitiveOriginMode::Center);
            return true;
        }
        if (Prim == TEXT("cylinder"))
        {
            UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendCylinder(
                TargetMesh, Options, Transform,
                /*Radius*/ Size * 0.5f, /*Height*/ Size,
                /*RadialSteps*/ 12, /*HeightSteps*/ 0, /*bCapped*/ true,
                EGeometryScriptPrimitiveOriginMode::Center);
            return true;
        }
        if (Prim == TEXT("cone"))
        {
            UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendCone(
                TargetMesh, Options, Transform,
                /*BaseRadius*/ Size * 0.5f, /*TopRadius*/ 0.0f, /*Height*/ Size,
                /*RadialSteps*/ 12, /*HeightSteps*/ 4, /*bCapped*/ true,
                EGeometryScriptPrimitiveOriginMode::Center);
            return true;
        }

        OutError = FString::Printf(TEXT("Unknown primitive '%s' (expected Box|Sphere|Cylinder|Cone)"), *Primitive);
        return false;
    }

    /**
     * Extract a transient UDynamicMesh from a UStaticMesh
     * asset via UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromStaticMesh.
     *
     * Returns the populated UDynamicMesh* (caller-owned, transient package lifetime)
     * or nullptr with OutError set on failure.
     *
     * Reads via MaxAvailable, which resets RequestedLODIndex to 0 and reads the
     * hi-res source model when there is one (GeometryScript_ResolveStaticMeshReadLOD
     * names the LOD read). UV channel selection is left to the
     * caller (no UV traversal here — query cmds accept their own UVChannel arg).
     *
     * Build options:
     *   bApplyBuildSettings = true   — respect StaticMesh build settings (normal
     *                                  generation, smoothing groups) so the queried
     *                                  geometry matches what renders at runtime.
     *   bRequestTangents = false     — tangents not needed for queries; cheaper.
     *   bIgnoreRemoveDegenerates = true — keep all triangles for accurate counts
     *                                  even if they're degenerate (a query that
     *                                  silently drops tris would be dishonest).
     *   bUseBuildScale = true        — respect Build Scale so volume/area are
     *                                  in the StaticMesh's authored units.
     */
    UDynamicMesh* BuildDynamicMeshFromStatic(UStaticMesh* StaticMesh, int32 RequestedLODIndex, FString& OutError)
    {
        if (!StaticMesh)
        {
            OutError = TEXT("StaticMesh is null");
            return nullptr;
        }

        FGeometryScriptCopyMeshFromAssetOptions Options;
        Options.bApplyBuildSettings = true;
        Options.bRequestTangents = false;
        Options.bIgnoreRemoveDegenerates = true;
        Options.bUseBuildScale = true;

        FGeometryScriptMeshReadLOD RequestedLOD;
        RequestedLOD.LODType = EGeometryScriptLODType::MaxAvailable;
        RequestedLOD.LODIndex = FMath::Max(0, RequestedLODIndex);

        EGeometryScriptOutcomePins Outcome = EGeometryScriptOutcomePins::Failure;
        UDynamicMesh* DynMesh = NewObject<UDynamicMesh>(GetTransientPackage());
        UDynamicMesh* Result = UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromStaticMesh(
            StaticMesh, DynMesh, Options, RequestedLOD, Outcome);
        if (Outcome != EGeometryScriptOutcomePins::Success || !Result)
        {
            OutError = FString::Printf(
                TEXT("CopyMeshFromStaticMesh failed for '%s' (LOD %d, outcome=%d)"),
                *StaticMesh->GetPathName(), RequestedLOD.LODIndex, static_cast<int32>(Outcome));
            return nullptr;
        }
        return Result;
    }
}

// ---------------------------------------------------------------------------------------------
// Lookups shared by the dynamic-mesh actor commands.
// ---------------------------------------------------------------------------------------------
namespace UEAgentGeometryScriptCommandsImpl
{

    /** The ADynamicMeshActor labelled or named ActorName in World, or nullptr with OutFailure set. */
    ADynamicMeshActor* FindDynamicMeshActor(UWorld* World, const FString& ActorName, TSharedPtr<FJsonObject>& OutFailure)
    {
        ADynamicMeshActor* MeshActor = Cast<ADynamicMeshActor>(mcp_common::FindActorByName(World, ActorName));
        if (MeshActor == nullptr)
        {
            OutFailure = mcp_common::MakeErrorReply(
                FString::Printf(TEXT("DynamicMeshActor '%s' not found in the editor world"), *ActorName));
        }
        return MeshActor;
    }

    /** A material by object path; when the plain load fails, a valid soft path is tried once more. */
    UMaterialInterface* LoadSlotMaterial(const FString& Path)
    {
        if (UMaterialInterface* Direct = LoadObject<UMaterialInterface>(nullptr, *Path))
        {
            return Direct;
        }
        const FSoftObjectPath AsSoftPath(Path);
        return AsSoftPath.IsValid() ? Cast<UMaterialInterface>(AsSoftPath.TryLoad()) : nullptr;
    }

    /**
     * Vertex count of a dynamic mesh. UDynamicMesh only exposes a triangle count, so the vertices
     * are counted on the underlying FDynamicMesh3.
     */
    int32 VertexCountOf(UDynamicMesh* Mesh)
    {
        const UE::Geometry::FDynamicMesh3* Raw = Mesh->GetMeshPtr();
        return Raw != nullptr ? Raw->VertexCount() : 0;
    }
}

namespace
{
    // A DynamicMeshActor a mesh command works on: the actor, its component and the component's live mesh, with
    // the "actor_name" param in Name when a resolver read it.
    struct FGeometryScriptMeshActor
    {
        FString Name;
        ADynamicMeshActor* Actor = nullptr;
        UDynamicMeshComponent* Component = nullptr;
        UDynamicMesh* Mesh = nullptr;
    };

    // The DynamicMeshActor ActorName names in World, its component and live mesh (all non-null on success),
    // into Out; null on success, else the error reply.
    TSharedPtr<FJsonObject> GeometryScript_FindMeshActor(UWorld* World, const FString& ActorName, FGeometryScriptMeshActor& Out)
    {
        TSharedPtr<FJsonObject> Failure;
        Out.Actor = UEAgentGeometryScriptCommandsImpl::FindDynamicMeshActor(World, ActorName, Failure);
        if (Out.Actor == nullptr)
        {
            return Failure;
        }
        Out.Component = Out.Actor->GetDynamicMeshComponent();
        Out.Mesh = Out.Component ? Out.Component->GetDynamicMesh() : nullptr;
        if (!Out.Mesh)
        {
            return mcp_common::MakeErrorReply(TEXT("DynamicMeshActor has no dynamic mesh"));
        }
        return nullptr;
    }

    // The editor world, the required "actor_name" param and, when RequiredField is given, the presence of that
    // param, checked in that order; then GeometryScript_FindMeshActor for that name.
    TSharedPtr<FJsonObject> GeometryScript_ResolveMeshActor(const TSharedPtr<FJsonObject>& Params, FGeometryScriptMeshActor& Out,
        const TCHAR* RequiredField = nullptr)
    {
        UWorld* World = nullptr;
        if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World, TEXT("Failed to get editor world"))) { return NoWorld; }
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), Out.Name, mcp_common::EAllowEmpty::Yes))
        {
            return Missing;
        }
        if (RequiredField != nullptr && !Params->HasField(RequiredField))
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Missing '%s'"), RequiredField));
        }
        return GeometryScript_FindMeshActor(World, Out.Name, Out);
    }

    // A reply payload whose actor_name is Actor's label.
    TSharedPtr<FJsonObject> GeometryScript_NewActorData(const ADynamicMeshActor* Actor)
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("actor_name"), Actor->GetActorLabel());
        return Data;
    }

    // Null when the vector param Field is absent or a list starting with three numbers (what the vector reader
    // reads); otherwise the error reply. The reader turns any other value into 0,0,0, which as a scale collapses
    // the whole mesh onto one point.
    TSharedPtr<FJsonObject> GeometryScript_CheckVectorParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field)
    {
        if (!Params->HasField(Field))
        {
            return nullptr;
        }
        const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
        if (Params->TryGetArrayField(Field, Items) && Items != nullptr && Items->Num() >= 3
            && (*Items)[0]->Type == EJson::Number && (*Items)[1]->Type == EJson::Number && (*Items)[2]->Type == EJson::Number)
        {
            return nullptr;
        }
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("'%s' must be a list of three numbers [x, y, z]"), Field));
    }

    // Null when Target's mesh has vertices; otherwise the error reply, since a whole-mesh transform has nothing to move.
    TSharedPtr<FJsonObject> GeometryScript_RequireVertices(const FGeometryScriptMeshActor& Target)
    {
        if (UGeometryScriptLibrary_MeshQueryFunctions::GetVertexCount(Target.Mesh) > 0)
        {
            return nullptr;
        }
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("DynamicMeshActor '%s' has an empty mesh: there are no vertices to move"), *Target.Name));
    }

    // Whether Transform leaves every vertex where it is. The MeshTransformFunctions apply the transform to every
    // vertex and report nothing, so on a mesh with vertices this is exactly the case in which nothing moves.
    bool GeometryScript_IsIdentityTransform(const FTransform& Transform)
    {
        return Transform.Equals(FTransform::Identity, UE_SMALL_NUMBER);
    }

    // The reply of a whole-mesh transform: applied says whether it moved the vertices. An identity transform is a
    // valid request that changes nothing, so it succeeds with applied false and a note.
    TSharedPtr<FJsonObject> GeometryScript_TransformReply(const TSharedPtr<FJsonObject>& Data, bool bApplied)
    {
        Data->SetBoolField(TEXT("applied"), bApplied);
        if (!bApplied)
        {
            Data->SetStringField(TEXT("note"), TEXT("identity transform: the mesh is unchanged"));
        }
        return mcp_common::MakeOkReply(Data);
    }

    // Triangle and vertex counts of a dynamic mesh.
    struct FGeometryScriptMeshCounts
    {
        int32 Triangles = 0;
        int32 Vertices = 0;
    };

    // Pushes an edit of Mesh to Component's render data (when there is a component) and reads back the mesh's counts.
    FGeometryScriptMeshCounts GeometryScript_NotifyAndCount(UDynamicMeshComponent* Component, UDynamicMesh* Mesh)
    {
        if (Component)
        {
            Component->NotifyMeshUpdated();
        }
        FGeometryScriptMeshCounts Counts;
        Counts.Triangles = Mesh->GetTriangleCount();
        Counts.Vertices = UGeometryScriptLibrary_MeshQueryFunctions::GetVertexCount(Mesh);
        return Counts;
    }

    // One undoable edit of a live dynamic mesh. UDynamicMesh::Serialize leaves the geometry out of a transaction,
    // so Modify() on the mesh records none of it. The engine undoes a mesh edit with an FMeshReplacementChange that
    // holds the mesh before and after the edit, stored on the open transaction, as
    // UDynamicMeshComponentToolTarget::CommitDynamicMeshChange does; undo and redo copy one of the two back through
    // UDynamicMesh::ApplyChange, whose change event refreshes the component. The step holds two full copies of the
    // mesh for as long as the undo buffer keeps it. Construct it inside the call's transaction before the first
    // write, then Commit after the edit, or Revert when the call fails.
    class FGeometryScriptMeshEdit
    {
    public:
        explicit FGeometryScriptMeshEdit(UDynamicMesh* InMesh)
            : Mesh(InMesh)
        {
            Mesh->ProcessMesh([this](const UE::Geometry::FDynamicMesh3& ReadMesh)
            {
                Before = MakeShared<UE::Geometry::FDynamicMesh3>(ReadMesh);
            });
        }

        // Stores the edit on the open transaction, so one undo restores the mesh and redo applies the edit again.
        // An edit that changed nothing stores nothing and leaves no undo entry. The copy taken before the edit is
        // what undo restores, so Modify() runs here, once the mesh is known to have changed: a no-op leaves the
        // package clean. Returns whether the mesh changed.
        bool Commit()
        {
            if (!HasChanged())
            {
                return false;
            }
            Mesh->Modify();
            if (GUndo != nullptr)
            {
                TSharedPtr<UE::Geometry::FDynamicMesh3> After;
                Mesh->ProcessMesh([&After](const UE::Geometry::FDynamicMesh3& ReadMesh)
                {
                    After = MakeShared<UE::Geometry::FDynamicMesh3>(ReadMesh);
                });
                GUndo->StoreUndo(Mesh, MakeUnique<FMeshReplacementChange>(Before, After));
            }
            return true;
        }

        // Puts the mesh back as it was before the edit, so a failed call leaves nothing half-written.
        void Revert()
        {
            if (HasChanged())
            {
                Mesh->SetMesh(*Before);
            }
        }

    private:
        // Whether the mesh differs from the copy taken before the edit: positions and attributes compared exactly,
        // and the element IDs, including the free ID slots that a compaction trims.
        bool HasChanged() const
        {
            UE::Geometry::FDynamicMesh3::FSameAsOptions Exact;
            Exact.bCheckConnectivity = true;
            Exact.bCheckEdgeIDs = true;
            Exact.bCheckNormals = true;
            Exact.bCheckColors = true;
            Exact.bCheckUVs = true;
            Exact.bCheckGroups = true;
            Exact.bCheckAttributes = true;
            Exact.bIgnoreDataLayout = false;
            Exact.EpsilonFloat = 0.0f;
            Exact.EpsilonDouble = 0.0;
            bool bSame = false;
            Mesh->ProcessMesh([this, &Exact, &bSame](const UE::Geometry::FDynamicMesh3& Current)
            {
                bSame = Current.MaxVertexID() == Before->MaxVertexID()
                    && Current.MaxTriangleID() == Before->MaxTriangleID()
                    && Current.MaxEdgeID() == Before->MaxEdgeID()
                    && Before->IsSameAs(Current, Exact);
            });
            return !bSame;
        }

        UDynamicMesh* Mesh = nullptr;
        TSharedPtr<UE::Geometry::FDynamicMesh3> Before;
    };

    // The "actor_name" and "source_actor_name" DynamicMeshActors; null on success, else the error reply.
    TSharedPtr<FJsonObject> GeometryScript_ResolveActorPair(const TSharedPtr<FJsonObject>& Params, FString& OutActorName,
        FString& OutSourceName, ADynamicMeshActor*& OutActor, ADynamicMeshActor*& OutSource)
    {
        UWorld* World = nullptr;
        if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World, TEXT("Failed to get editor world"))) { return NoWorld; }
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), OutActorName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("source_actor_name"), OutSourceName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
        TSharedPtr<FJsonObject> Failure;
        OutActor = UEAgentGeometryScriptCommandsImpl::FindDynamicMeshActor(World, OutActorName, Failure);
        if (OutActor == nullptr)
        {
            return Failure;
        }
        OutSource = Cast<ADynamicMeshActor>(mcp_common::FindActorByName(World, OutSourceName));
        if (OutSource == nullptr)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Source DynamicMeshActor '%s' not found in the editor world"), *OutSourceName));
        }
        return nullptr;
    }

    // The "operation" boolean (Subtract when absent); null on success, else the error reply.
    TSharedPtr<FJsonObject> GeometryScript_ReadBooleanOperation(const TSharedPtr<FJsonObject>& Params, FString& OutOpName,
        EGeometryScriptBooleanOperation& OutOp)
    {
        OutOpName = TEXT("Subtract");
        Params->TryGetStringField(TEXT("operation"), OutOpName);
        const FString OpLower = OutOpName.ToLower();
        if (OpLower == TEXT("union"))                                        { OutOp = EGeometryScriptBooleanOperation::Union; }
        else if (OpLower == TEXT("subtract") || OpLower == TEXT("difference")) { OutOp = EGeometryScriptBooleanOperation::Subtract; }
        else if (OpLower == TEXT("intersect") || OpLower == TEXT("intersection")) { OutOp = EGeometryScriptBooleanOperation::Intersection; }
        else
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Unknown operation '%s' (expected Union|Subtract|Intersect)"), *OutOpName));
        }
        return nullptr;
    }

    // The LOD a static-mesh read was asked for, and the one it reads.
    struct FGeometryScriptStaticMeshLOD
    {
        int32 Requested = 0;        // lod_index as sent (0 when absent)
        int32 Read = 0;             // index of the LOD read; 0 for the hi-res source model
        bool bHiResSource = false;  // the hi-res source model was read
    };

    // The LOD CopyMeshFromStaticMesh reads for an EGeometryScriptLODType::MaxAvailable request, the type every
    // static-mesh read here passes. It mirrors the engine's resolution in the editor (UE::Conversion's
    // StaticMeshToDynamicMesh with bUseClosestLOD, which GeometryScript passes): MaxAvailable resets the requested
    // index to 0, then reads the hi-res source model when the asset has a valid one, else source model 0, else
    // (no source geometry there) render-data LOD 0. A mesh with no source model, or with neither source geometry nor
    // a render LOD, would make the engine index LOD -1 and kill the editor: OutError then says why it is refused.
    FGeometryScriptStaticMeshLOD GeometryScript_ResolveStaticMeshReadLOD(const UStaticMesh* StaticMesh, int32 RequestedLODIndex,
        FString& OutError)
    {
        FGeometryScriptStaticMeshLOD LOD;
        LOD.Requested = RequestedLODIndex;
        if (StaticMesh->IsHiResMeshDescriptionValid())
        {
            LOD.bHiResSource = true;
        }
        else if (StaticMesh->GetNumSourceModels() == 0
            || (!StaticMesh->GetSourceModel(0).IsSourceModelInitialized() && StaticMesh->GetNumLODs() == 0))
        {
            OutError = FString::Printf(
                TEXT("Static mesh '%s' has no source geometry and no render LOD to read; nothing was read"),
                *StaticMesh->GetPathName());
        }
        return LOD;
    }

    // The "static_mesh_path" mesh and the LOD it is read at (the optional "lod_index" is only recorded as
    // requested); null on success, else the error reply.
    TSharedPtr<FJsonObject> GeometryScript_LoadStaticMeshParam(const TSharedPtr<FJsonObject>& Params, FString& OutMeshPath,
        FGeometryScriptStaticMeshLOD& OutLOD, UStaticMesh*& OutStaticMesh)
    {
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("static_mesh_path"), OutMeshPath)) { return Missing; }
        OutStaticMesh = mcp_common::LoadAssetOrSoftPath<UStaticMesh>(OutMeshPath);
        if (!OutStaticMesh)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Cannot load static mesh: %s"), *OutMeshPath));
        }
        int32 RequestedLODIndex = 0;
        Params->TryGetNumberField(TEXT("lod_index"), RequestedLODIndex);
        FString LODError;
        OutLOD = GeometryScript_ResolveStaticMeshReadLOD(OutStaticMesh, RequestedLODIndex, LODError);
        if (!LODError.IsEmpty())
        {
            return mcp_common::MakeErrorReply(LODError);
        }
        return nullptr;
    }

    // GeometryScript_LoadStaticMeshParam, then a transient dynamic-mesh copy of that LOD.
    TSharedPtr<FJsonObject> GeometryScript_CopyStaticMeshParam(const TSharedPtr<FJsonObject>& Params, FString& OutMeshPath,
        FGeometryScriptStaticMeshLOD& OutLOD, UDynamicMesh*& OutMesh)
    {
        UStaticMesh* StaticMesh = nullptr;
        if (TSharedPtr<FJsonObject> Failure = GeometryScript_LoadStaticMeshParam(Params, OutMeshPath, OutLOD, StaticMesh))
        {
            return Failure;
        }
        FString DynError;
        OutMesh = BuildDynamicMeshFromStatic(StaticMesh, OutLOD.Requested, DynError);
        if (!OutMesh)
        {
            return mcp_common::MakeErrorReply(DynError);
        }
        return nullptr;
    }

    // The optional "lod_index" param clamped to >= 0; 0 when absent.
    int32 GeometryScript_ReadClampedLODIndex(const TSharedPtr<FJsonObject>& Params)
    {
        int32 LODIndex = 0;
        double LODDouble = 0.0;
        if (Params->TryGetNumberField(TEXT("lod_index"), LODDouble))
        {
            LODIndex = FMath::Max(0, static_cast<int32>(LODDouble));
        }
        return LODIndex;
    }

    // Data["lod_index"] = the LOD read, Data["requested_lod_index"] = the one asked for, and
    // Data["read_hi_res_source"] = whether the hi-res source model was read instead of an LOD.
    void GeometryScript_AddStaticMeshLOD(const TSharedPtr<FJsonObject>& Data, const FGeometryScriptStaticMeshLOD& LOD)
    {
        Data->SetNumberField(TEXT("lod_index"), LOD.Read);
        Data->SetNumberField(TEXT("requested_lod_index"), LOD.Requested);
        Data->SetBoolField(TEXT("read_hi_res_source"), LOD.bHiResSource);
    }

    // A reply payload naming the queried static mesh: static_mesh_path and the LOD fields of
    // GeometryScript_AddStaticMeshLOD.
    TSharedPtr<FJsonObject> GeometryScript_NewStaticMeshData(const FString& MeshPath, const FGeometryScriptStaticMeshLOD& LOD)
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("static_mesh_path"), MeshPath);
        GeometryScript_AddStaticMeshLOD(Data, LOD);
        return Data;
    }

    // A reply payload whose actor_name is the name of DynMesh's outer.
    TSharedPtr<FJsonObject> GeometryScript_NewMeshData(const UDynamicMesh* DynMesh)
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("actor_name"), DynMesh->GetOuter()->GetName());
        return Data;
    }

    // The required non-negative "uv_channel" param; null on success, else the error reply.
    TSharedPtr<FJsonObject> GeometryScript_ReadUVChannel(const TSharedPtr<FJsonObject>& Params, int32& OutUVChannel)
    {
        double UVChannelD = 0.0;
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("uv_channel"), UVChannelD)) { return Missing; }
        OutUVChannel = static_cast<int32>(UVChannelD);
        if (OutUVChannel < 0)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("uv_channel must be >= 0 (got %d)"), OutUVChannel));
        }
        return nullptr;
    }

    // The optional non-negative "uv_channel" param; null on success, else the error reply.
    TSharedPtr<FJsonObject> GeometryScript_ReadOptionalUVChannel(const TSharedPtr<FJsonObject>& Params, int32& OutUVChannel)
    {
        double UVChannelD = 0.0;
        if (Params->TryGetNumberField(TEXT("uv_channel"), UVChannelD))
        {
            OutUVChannel = static_cast<int32>(UVChannelD);
            if (OutUVChannel < 0)
            {
                return mcp_common::MakeErrorReply(
                    FString::Printf(TEXT("uv_channel must be >= 0 (got %d)"), OutUVChannel));
            }
        }
        return nullptr;
    }

    // Null when DynMesh has the UV channel UVChannel; otherwise the out-of-range error reply.
    TSharedPtr<FJsonObject> GeometryScript_CheckUVChannelExists(UDynamicMesh* DynMesh, int32 UVChannel)
    {
        const int32 NumChannels = UGeometryScriptLibrary_MeshQueryFunctions::GetNumUVSets(DynMesh);
        if (UVChannel >= NumChannels)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("uv_channel %d out of range (mesh has %d UV channels)"),
                    UVChannel, NumChannels));
        }
        return nullptr;
    }

    // The most UV channels a dynamic mesh can carry: UGeometryScriptLibrary_MeshUVFunctions::SetNumUVSets
    // refuses any larger count and leaves the mesh as it was.
    constexpr int32 GeometryScript_MaxUVChannels = 8;

    // A transient sink for what a GeometryScript library call refuses or fails: those calls return the
    // target mesh whatever happened and report a failure only by appending an error to this object.
    UGeometryScriptDebug* GeometryScript_NewDebug()
    {
        return NewObject<UGeometryScriptDebug>(GetTransientPackage());
    }

    // The first error Debug collected, or an empty string when the call reported none (warnings do not count).
    FString GeometryScript_FirstError(const UGeometryScriptDebug* Debug)
    {
        if (Debug != nullptr)
        {
            for (const FGeometryScriptDebugMessage& Message : Debug->Messages)
            {
                if (Message.MessageType == EGeometryScriptDebugMessageType::ErrorMessage)
                {
                    return Message.Message.ToString();
                }
            }
        }
        return FString();
    }

    // The reply of a mesh edit whose outcome flag was measured: success carries the flag, and a false one
    // names the flag and why it is false.
    TSharedPtr<FJsonObject> GeometryScript_OutcomeReply(const TSharedPtr<FJsonObject>& Data, bool bOutcome,
        const TCHAR* Command, const TCHAR* OutcomeField, const FString& Reason)
    {
        return mcp_common::MakeOutcomeReply(Data, bOutcome, bOutcome ? FString()
            : FString::Printf(TEXT("%s: %s is false: %s"), Command, OutcomeField, *Reason));
    }

    // How many triangles of DynMesh carry MaterialID. A mesh without the MaterialID attribute reads as all 0, which
    // is what EnableMaterialIDs gives it.
    int32 GeometryScript_CountMaterialID(UDynamicMesh* DynMesh, int32 MaterialID)
    {
        FGeometryScriptIndexList AllIDs;
        bool bHasMaterialIDs = false;
        UGeometryScriptLibrary_MeshMaterialFunctions::GetAllTriangleMaterialIDs(DynMesh, AllIDs, bHasMaterialIDs);
        if (!bHasMaterialIDs)
        {
            return MaterialID == 0 ? DynMesh->GetTriangleCount() : 0;
        }
        int32 Count = 0;
        if (AllIDs.List.IsValid())
        {
            for (const int32 ID : *AllIDs.List)
            {
                Count += (ID == MaterialID) ? 1 : 0;
            }
        }
        return Count;
    }

    // Reads back the mesh's max MaterialID and how many distinct MaterialIDs its triangles carry.
    void GeometryScript_ReadMaterialIDs(UDynamicMesh* DynMesh, int32& OutMaxMaterialID, int32& OutDistinctCount)
    {
        bool bHasMaterialIDs = false;
        OutMaxMaterialID = UGeometryScriptLibrary_MeshMaterialFunctions::GetMaxMaterialID(DynMesh, bHasMaterialIDs);
        FGeometryScriptIndexList AllIDs;
        bool bHasIDsAfter = false;
        UGeometryScriptLibrary_MeshMaterialFunctions::GetAllTriangleMaterialIDs(DynMesh, AllIDs, bHasIDsAfter);
        TSet<int32> DistinctIDs;
        if (AllIDs.List.IsValid()) { for (const int32 ID : *AllIDs.List) { DistinctIDs.Add(ID); } }
        OutDistinctCount = DistinctIDs.Num();
    }

    // A local-space box as { min_x, min_y, min_z, max_x, max_y, max_z, center_x.., size_x.. }.
    TSharedPtr<FJsonObject> GeometryScript_BoundsToJson(const FBox& Bounds, const FVector& Center, const FVector& Size)
    {
        TSharedPtr<FJsonObject> BoundsJson = MakeShared<FJsonObject>();
        BoundsJson->SetNumberField(TEXT("min_x"), Bounds.Min.X);
        BoundsJson->SetNumberField(TEXT("min_y"), Bounds.Min.Y);
        BoundsJson->SetNumberField(TEXT("min_z"), Bounds.Min.Z);
        BoundsJson->SetNumberField(TEXT("max_x"), Bounds.Max.X);
        BoundsJson->SetNumberField(TEXT("max_y"), Bounds.Max.Y);
        BoundsJson->SetNumberField(TEXT("max_z"), Bounds.Max.Z);
        BoundsJson->SetNumberField(TEXT("center_x"), Center.X);
        BoundsJson->SetNumberField(TEXT("center_y"), Center.Y);
        BoundsJson->SetNumberField(TEXT("center_z"), Center.Z);
        BoundsJson->SetNumberField(TEXT("size_x"), Size.X);
        BoundsJson->SetNumberField(TEXT("size_y"), Size.Y);
        BoundsJson->SetNumberField(TEXT("size_z"), Size.Z);
        return BoundsJson;
    }
}

// Dynamic-mesh actor commands: spawn an ADynamicMeshActor, then build and edit its live UDynamicMesh. Every
// mutation here is verified by reading the triangle / vertex count back off the live mesh.

// MCP:spawn_dynamic_mesh_actor - Spawn an empty ADynamicMeshActor into the editor world (not PIE) at an optional location and rotation, ignoring collisions, in an undoable transaction. Returns the label of the new actor as actor_name, its internal name and triangle_count (0). Remove it with remove_dynamic_mesh_actor.
//   @param location - World location [x, y, z] in cm (default the world origin).
//   @param rotation - World rotation in degrees as [pitch, yaw, roll] (default no rotation).
//   @returns { actor_name: string, actor_internal_name: string, actor_class: string, has_dynamic_mesh_component: bool, has_dynamic_mesh: bool, triangle_count: int, spawned: bool }
TSharedPtr<FJsonObject> HandleSpawnDynamicMeshActor(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World, TEXT("Failed to get editor world"))) { return NoWorld; }

    // location is an [x, y, z] array (the origin when absent). rotation is only parsed when present,
    // through the shared rotator reader (object form, or the ambiguous [pitch, yaw, roll] array).
    const FVector SpawnLocation = mcp_common::ReadJsonVector(Params, TEXT("location"));
    const FRotator SpawnRotation = Params->HasField(TEXT("rotation"))
        ? mcp_common::ReadJsonRotator(Params, TEXT("rotation"))
        : FRotator::ZeroRotator;

    const FScopedTransaction Transaction(LOCTEXT("SpawnDynamicMeshActor", "MCP: Spawn Dynamic Mesh Actor"));

    FActorSpawnParameters SpawnOptions;
    SpawnOptions.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    ADynamicMeshActor* Spawned = World->SpawnActor<ADynamicMeshActor>(SpawnLocation, SpawnRotation, SpawnOptions);
    if (Spawned == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to spawn DynamicMeshActor"));
    }
    Spawned->Modify();

    // A fresh actor normally carries a component with an empty mesh; both are reported as found.
    UDynamicMeshComponent* MeshComp = Spawned->GetDynamicMeshComponent();
    UDynamicMesh* DynMesh = MeshComp != nullptr ? MeshComp->GetDynamicMesh() : nullptr;

    TSharedPtr<FJsonObject> Result = GeometryScript_NewActorData(Spawned);
    Result->SetStringField(TEXT("actor_internal_name"), Spawned->GetName());
    Result->SetStringField(TEXT("actor_class"), Spawned->GetClass()->GetName());
    Result->SetBoolField(TEXT("has_dynamic_mesh_component"), MeshComp != nullptr);
    Result->SetBoolField(TEXT("has_dynamic_mesh"), DynMesh != nullptr);
    Result->SetNumberField(TEXT("triangle_count"), DynMesh != nullptr ? DynMesh->GetTriangleCount() : 0);
    Result->SetBoolField(TEXT("spawned"), true);
    return mcp_common::MakeOkReply(Result);
}

// MCP:append_mesh_primitive - Append a Box, Sphere, Cylinder or Cone to the live dynamic mesh of a DynamicMeshActor in the editor world, keeping the geometry already there: size is the box edge or the diameter and height of the round shapes, centred at location in mesh-local space. Each call adds another copy as one undo step: undo puts the mesh back as it was. Nothing is saved to an asset. An unknown actor or primitive is an error that leaves the mesh as it was; appended is true when the triangle count grew. To merge another actor's mesh use append_mesh_to_dynamic_mesh; to persist the result, copy_dynamic_mesh_to_static_mesh.
//   @param actor_name - Label or name of the target DynamicMeshActor
//   @param primitive - Box, Sphere, Cylinder, or Cone, default Box
//   @param size - Dominant dimension in cm, default 100
//   @param location - Optional local [x, y, z] offset for the primitive
//   @param dimensions - Fallback alias for size when size is omitted (cm)
//   @param material_id - Optional MaterialID stamped on the appended triangles (default 0). Non-zero enables the mesh MaterialID attribute first, so a later copy_dynamic_mesh_to_static_mesh (bUseSectionMaterials=true) emits this piece as its own section/material slot. This is how one dynamic mesh becomes a multi-slot static mesh.
//   @returns { actor_name: string, primitive: string, size: number, material_id: int, triangle_count: int, triangles_added: int, vertex_count: int, appended: bool }
TSharedPtr<FJsonObject> HandleAppendMeshPrimitive(const TSharedPtr<FJsonObject>& Params)
{
    FGeometryScriptMeshActor Target;
    if (TSharedPtr<FJsonObject> Failure = GeometryScript_ResolveMeshActor(Params, Target))
    {
        return Failure;
    }

    FString Primitive = TEXT("Box");
    Params->TryGetStringField(TEXT("primitive"), Primitive);

    double Size = 100.0;
    if (!Params->TryGetNumberField(TEXT("size"), Size))
    {
        Params->TryGetNumberField(TEXT("dimensions"), Size);
    }

    // Optional local offset for the appended primitive.
    FVector Offset = FVector::ZeroVector;
    if (Params->HasField(TEXT("location")))
    {
        Offset = mcp_common::ReadJsonVector(Params, TEXT("location"));
    }
    const FTransform LocalTransform(FRotator::ZeroRotator, Offset, FVector::OneVector);

    // Optional per-piece MaterialID. Absent => 0.
    int32 MaterialID = 0;
    {
        double MaterialIDDouble = 0.0;
        if (Params->TryGetNumberField(TEXT("material_id"), MaterialIDDouble))
        {
            MaterialID = static_cast<int32>(MaterialIDDouble);
            if (MaterialID < 0)
                return mcp_common::MakeErrorReply(
                    FString::Printf(TEXT("material_id must be >= 0 (got %d)"), MaterialID));
        }
    }

    const int32 TrisBefore = Target.Mesh->GetTriangleCount();

    const FScopedTransaction Transaction(LOCTEXT("AppendMeshPrimitive", "MCP: Append Mesh Primitive"));
    Target.Actor->Modify();
    FGeometryScriptMeshEdit MeshEdit(Target.Mesh);

    // The MaterialID attribute must exist BEFORE the append or the ID is silently dropped
    // (the engine reports no error). Only touched when a non-zero ID was actually asked for.
    if (MaterialID != 0)
    {
        UGeometryScriptLibrary_MeshMaterialFunctions::EnableMaterialIDs(Target.Mesh, /*Debug*/ nullptr);
    }

    FString AppendError;
    if (!AppendPrimitiveToMesh(Target.Mesh, Primitive, LocalTransform, static_cast<float>(Size), AppendError, MaterialID))
    {
        MeshEdit.Revert();
        return mcp_common::MakeErrorReply(AppendError);
    }
    MeshEdit.Commit();

    // Refresh render data so the appended geometry is visible in the editor.
    const FGeometryScriptMeshCounts After = GeometryScript_NotifyAndCount(Target.Component, Target.Mesh);

    TSharedPtr<FJsonObject> Result = GeometryScript_NewActorData(Target.Actor);
    Result->SetStringField(TEXT("primitive"), Primitive);
    Result->SetNumberField(TEXT("size"), Size);
    Result->SetNumberField(TEXT("material_id"), MaterialID);
    Result->SetNumberField(TEXT("triangle_count"), After.Triangles);
    Result->SetNumberField(TEXT("triangles_added"), After.Triangles - TrisBefore);
    Result->SetNumberField(TEXT("vertex_count"), After.Vertices);
    Result->SetBoolField(TEXT("appended"), After.Triangles > TrisBefore);
    return mcp_common::MakeOkReply(Result);
}

// MCP:apply_mesh_boolean - Apply a CSG boolean (Subtract by default, Union or Intersect) between the live dynamic mesh of a DynamicMeshActor in the editor world and a temporary primitive tool: other_primitive (Sphere by default) of size cm, offset by location in mesh-local space. The target is changed in place as one undo step that restores it, nothing is saved, and each call cuts or merges again. An unknown actor, operation or primitive is an error. When the engine refuses the result (an empty one, such as subtracting from an empty mesh) the mesh is left unchanged, boolean_applied is false and the call fails. To use another actor's mesh as the tool call apply_mesh_boolean_actor_to_actor.
//   @param actor_name - Label or name of the target DynamicMeshActor
//   @param operation - Union, Subtract, or Intersect, default Subtract
//   @param other_primitive - Tool primitive shape, default Sphere
//   @param size - Tool primitive size in cm, default 100
//   @param location - Optional tool [x, y, z] offset
//   @param dimensions - Fallback alias for size when size is omitted (cm)
//   @returns { actor_name: string, operation: string, other_primitive: string, triangle_count: int, triangle_count_before: int, vertex_count: int, boolean_applied: bool }
TSharedPtr<FJsonObject> HandleApplyMeshBoolean(const TSharedPtr<FJsonObject>& Params)
{
    FGeometryScriptMeshActor Target;
    if (TSharedPtr<FJsonObject> Failure = GeometryScript_ResolveMeshActor(Params, Target))
    {
        return Failure;
    }

    // Operation.
    FString OpName;
    EGeometryScriptBooleanOperation Op = EGeometryScriptBooleanOperation::Subtract;
    if (TSharedPtr<FJsonObject> Failure = GeometryScript_ReadBooleanOperation(Params, OpName, Op))
        return Failure;

    // Build the tool mesh as a transient UDynamicMesh and append the requested primitive.
    FString ToolPrimitive = TEXT("Sphere");
    Params->TryGetStringField(TEXT("other_primitive"), ToolPrimitive);

    double ToolSize = 100.0;
    if (!Params->TryGetNumberField(TEXT("size"), ToolSize))
    {
        Params->TryGetNumberField(TEXT("dimensions"), ToolSize);
    }

    FVector ToolOffset = FVector::ZeroVector;
    if (Params->HasField(TEXT("location")))
    {
        ToolOffset = mcp_common::ReadJsonVector(Params, TEXT("location"));
    }

    UDynamicMesh* ToolMesh = NewObject<UDynamicMesh>(GetTransientPackage());
    if (!ToolMesh)
        return mcp_common::MakeErrorReply(TEXT("Failed to allocate tool dynamic mesh"));

    FString AppendError;
    if (!AppendPrimitiveToMesh(ToolMesh, ToolPrimitive, FTransform::Identity, static_cast<float>(ToolSize), AppendError))
    {
        return mcp_common::MakeErrorReply(AppendError);
    }

    const int32 TrisBefore = Target.Mesh->GetTriangleCount();

    const FScopedTransaction Transaction(LOCTEXT("ApplyMeshBoolean", "MCP: Apply Mesh Boolean"));
    Target.Actor->Modify();
    FGeometryScriptMeshEdit MeshEdit(Target.Mesh);

    FGeometryScriptMeshBooleanOptions BoolOptions; // defaults: fill holes, simplify output, no empty result
    const FTransform ToolTransform(FRotator::ZeroRotator, ToolOffset, FVector::OneVector);

    // The engine replaces the target with the result unless it refuses it (an empty result), and says so only
    // through Debug; a refused boolean leaves the target as it was.
    UGeometryScriptDebug* Debug = GeometryScript_NewDebug();
    UGeometryScriptLibrary_MeshBooleanFunctions::ApplyMeshBoolean(
        Target.Mesh, FTransform::Identity,
        ToolMesh, ToolTransform,
        Op, BoolOptions, Debug);
    const FString BooleanError = GeometryScript_FirstError(Debug);
    if (BooleanError.IsEmpty())
    {
        MeshEdit.Commit();
    }
    else
    {
        MeshEdit.Revert();
    }

    const FGeometryScriptMeshCounts After = GeometryScript_NotifyAndCount(Target.Component, Target.Mesh);

    TSharedPtr<FJsonObject> Result = GeometryScript_NewActorData(Target.Actor);
    Result->SetStringField(TEXT("operation"), OpName);
    Result->SetStringField(TEXT("other_primitive"), ToolPrimitive);
    Result->SetNumberField(TEXT("triangle_count"), After.Triangles);
    Result->SetNumberField(TEXT("triangle_count_before"), TrisBefore);
    Result->SetNumberField(TEXT("vertex_count"), After.Vertices);
    Result->SetBoolField(TEXT("boolean_applied"), BooleanError.IsEmpty());
    return GeometryScript_OutcomeReply(Result, BooleanError.IsEmpty(), TEXT("apply_mesh_boolean"), TEXT("boolean_applied"),
        FString::Printf(TEXT("the engine refused the boolean and left the mesh unchanged (%s)"), *BooleanError));
}

// MCP:apply_mesh_boolean_actor_to_actor - Apply a CSG boolean using a second DynamicMeshActor as the cutting tool. Unlike apply_mesh_boolean (which builds a transient primitive tool), the tool geometry is the live dynamic mesh of source_actor_name. The target mesh is mutated in place as one undo step that restores it; the source mesh is left untouched. NOT idempotent: re-running keeps cutting/merging the same tool against the already-modified target. When the engine refuses the result (an empty one) the target is left unchanged, boolean_applied is false and the call fails.
//   @param actor_name - Label or name of the target DynamicMeshActor (the receiver, mutated in place)
//   @param source_actor_name - Label or name of the DynamicMeshActor whose live dynamic mesh is used as the boolean tool
//   @param operation - Union, Subtract, or Intersect, default Subtract
//   @param use_world_transforms - Optional bool, default true. When true the tool is positioned by the relative world transform (source relative to target) so the boolean respects the actors' placement; when false the tool is applied at identity (co-located).
//   @param fill_holes - Optional bool override for FGeometryScriptMeshBooleanOptions.bFillHoles (default true)
//   @param simplify_output - Optional bool override for FGeometryScriptMeshBooleanOptions.bSimplifyOutput (default true)
//   @returns { actor_name: string, source_actor_name: string, operation: string, triangle_count: int, triangle_count_before: int, vertex_count: int, boolean_applied: bool }
TSharedPtr<FJsonObject> HandleApplyMeshBooleanActorToActor(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName, SourceName;
    ADynamicMeshActor* DMActor = nullptr;
    ADynamicMeshActor* DMSource = nullptr;
    if (TSharedPtr<FJsonObject> Failure = GeometryScript_ResolveActorPair(Params, ActorName, SourceName, DMActor, DMSource))
        return Failure;

    // A boolean of an actor against itself is degenerate here — use repair_mesh_self_intersections for that.
    if (DMSource == DMActor)
        return mcp_common::MakeErrorReply(TEXT("source_actor_name must differ from actor_name (use repair_mesh_self_intersections to boolean a mesh with itself)"));

    UDynamicMeshComponent* MeshComp = DMActor->GetDynamicMeshComponent();
    UDynamicMesh* TargetMesh = MeshComp ? MeshComp->GetDynamicMesh() : nullptr;
    if (!TargetMesh)
        return mcp_common::MakeErrorReply(TEXT("Target DynamicMeshActor has no dynamic mesh"));

    UDynamicMeshComponent* SourceComp = DMSource->GetDynamicMeshComponent();
    UDynamicMesh* SourceMesh = SourceComp ? SourceComp->GetDynamicMesh() : nullptr;
    if (!SourceMesh)
        return mcp_common::MakeErrorReply(TEXT("Source DynamicMeshActor has no dynamic mesh"));

    // Operation.
    FString OpName;
    EGeometryScriptBooleanOperation Op = EGeometryScriptBooleanOperation::Subtract;
    if (TSharedPtr<FJsonObject> Failure = GeometryScript_ReadBooleanOperation(Params, OpName, Op))
        return Failure;

    // Position the tool mesh. The boolean runs with the target at identity, so the tool
    // transform is the source's world transform expressed relative to the target's.
    bool bUseWorldTransforms = true;
    Params->TryGetBoolField(TEXT("use_world_transforms"), bUseWorldTransforms);
    const FTransform ToolTransform = bUseWorldTransforms
        ? DMSource->GetActorTransform().GetRelativeTransform(DMActor->GetActorTransform())
        : FTransform::Identity;

    FGeometryScriptMeshBooleanOptions BoolOptions; // defaults: fill holes, simplify output
    Params->TryGetBoolField(TEXT("fill_holes"), BoolOptions.bFillHoles);
    Params->TryGetBoolField(TEXT("simplify_output"), BoolOptions.bSimplifyOutput);

    const int32 TrisBefore = TargetMesh->GetTriangleCount();

    const FScopedTransaction Transaction(LOCTEXT("ApplyMeshBooleanActorToActor", "MCP: Apply Mesh Boolean (Actor to Actor)"));
    DMActor->Modify();
    DMSource->Modify();
    FGeometryScriptMeshEdit MeshEdit(TargetMesh);

    // A refused boolean (an empty result) is reported only through Debug and leaves the target unchanged.
    UGeometryScriptDebug* Debug = GeometryScript_NewDebug();
    UGeometryScriptLibrary_MeshBooleanFunctions::ApplyMeshBoolean(
        TargetMesh, FTransform::Identity,
        SourceMesh, ToolTransform,
        Op, BoolOptions, Debug);
    const FString BooleanError = GeometryScript_FirstError(Debug);
    if (BooleanError.IsEmpty())
    {
        MeshEdit.Commit();
    }
    else
    {
        MeshEdit.Revert();
    }

    const FGeometryScriptMeshCounts After = GeometryScript_NotifyAndCount(MeshComp, TargetMesh);

    TSharedPtr<FJsonObject> Result = GeometryScript_NewActorData(DMActor);
    Result->SetStringField(TEXT("source_actor_name"), DMSource->GetActorLabel());
    Result->SetStringField(TEXT("operation"), OpName);
    Result->SetNumberField(TEXT("triangle_count"), After.Triangles);
    Result->SetNumberField(TEXT("triangle_count_before"), TrisBefore);
    Result->SetNumberField(TEXT("vertex_count"), After.Vertices);
    Result->SetBoolField(TEXT("boolean_applied"), BooleanError.IsEmpty());
    return GeometryScript_OutcomeReply(Result, BooleanError.IsEmpty(), TEXT("apply_mesh_boolean_actor_to_actor"),
        TEXT("boolean_applied"), FString::Printf(TEXT("the engine refused the boolean and left the mesh unchanged (%s)"), *BooleanError));
}

// MCP:apply_mesh_solidify - Voxel-wrap the live dynamic mesh of a DynamicMeshActor in the editor world, found by label or name: the mesh is replaced by a voxelised-and-meshed approximation on a grid of 64 cells along its longest side, as one undo step that restores the original mesh; nothing is saved. An unknown actor is an error. When the engine reports an error (a result with no triangles, as from an empty mesh), solidified is false, the call fails and the mesh is left as it was. For an offset surface use apply_mesh_morphology; to only fix self-intersections, repair_mesh_self_intersections; persist the result with copy_dynamic_mesh_to_static_mesh.
//   @param actor_name - Label or name of the target DynamicMeshActor
//   @param winding_threshold - Optional [0..1], default 0.5. Space with generalised winding number above this is considered inside.
//   @param extend_bounds - Optional positive float, default 1.0. Bounds extension applied to the default input-mesh-based bounds.
//   @param surface_search_steps - Optional int >= 1, default 3. Marching-cubes vertex search steps.
//   @param thicken_shells - Optional bool, default false. Thicken open shells into closed solids (expensive on large meshes).
//   @param shell_thickness - Optional positive float, default 1.0. Offset distance for shell thickening.
//   @returns { actor_name, triangle_count, triangle_count_before, vertex_count, solidified }
TSharedPtr<FJsonObject> HandleApplyMeshSolidify(const TSharedPtr<FJsonObject>& Params)
{
    FGeometryScriptMeshActor Target;
    if (TSharedPtr<FJsonObject> Failure = GeometryScript_ResolveMeshActor(Params, Target))
    {
        return Failure;
    }

    FGeometryScriptSolidifyOptions Options; // defaults are documented in MeshVoxelFunctions.h
    double D = 0.0;
    if (Params->TryGetNumberField(TEXT("winding_threshold"), D))
    {
        Options.WindingThreshold = static_cast<float>(D);
    }
    if (Params->TryGetNumberField(TEXT("extend_bounds"), D))
    {
        Options.ExtendBounds = static_cast<float>(D);
    }
    int32 I = 0;
    if (Params->TryGetNumberField(TEXT("surface_search_steps"), I))
    {
        Options.SurfaceSearchSteps = I;
    }
    bool B = false;
    if (Params->TryGetBoolField(TEXT("thicken_shells"), B))
    {
        Options.bThickenShells = B;
    }
    D = 0.0;
    if (Params->TryGetNumberField(TEXT("shell_thickness"), D))
    {
        Options.ShellThickness = D;
    }

    const int32 TrisBefore = Target.Mesh->GetTriangleCount();

    const FScopedTransaction Transaction(LOCTEXT("ApplyMeshSolidify", "MCP: Apply Mesh Solidify"));
    Target.Actor->Modify();
    FGeometryScriptMeshEdit MeshEdit(Target.Mesh);

    // ApplyMeshSolidify returns the target mesh whatever happened and reports a failure (a result with no
    // triangles, which it leaves in the mesh) only as an error in Debug: the mesh is then put back as it was and
    // the call fails with the engine's message.
    UGeometryScriptDebug* Debug = GeometryScript_NewDebug();
    UGeometryScriptLibrary_MeshVoxelFunctions::ApplyMeshSolidify(Target.Mesh, Options, Debug);
    const FString SolidifyError = GeometryScript_FirstError(Debug);
    const bool bSucceeded = SolidifyError.IsEmpty();
    if (bSucceeded)
    {
        MeshEdit.Commit();
    }
    else
    {
        MeshEdit.Revert();
    }
    const FGeometryScriptMeshCounts After = GeometryScript_NotifyAndCount(Target.Component, Target.Mesh);

    TSharedPtr<FJsonObject> Result = GeometryScript_NewActorData(Target.Actor);
    Result->SetStringField(TEXT("operation"), TEXT("solidify"));
    Result->SetNumberField(TEXT("triangle_count"), After.Triangles);
    Result->SetNumberField(TEXT("triangle_count_before"), TrisBefore);
    Result->SetNumberField(TEXT("vertex_count"), After.Vertices);
    Result->SetBoolField(TEXT("solidified"), bSucceeded);
    return GeometryScript_OutcomeReply(Result, bSucceeded, TEXT("apply_mesh_solidify"), TEXT("solidified"), SolidifyError);
}

// MCP:repair_mesh_self_intersections - Boolean-union a DynamicMeshActor's dynamic mesh with itself to repair self-intersections, remove internal/floating geometry and resolve non-manifold overlaps (UGeometryScriptLibrary_MeshBooleanFunctions::ApplyMeshSelfUnion). Convergent: the first pass mutates the mesh, but a second pass on the already-cleaned mesh is a no-op (repaired reports false). One undo step restores the mesh, and a pass that changes nothing adds no undo step. NOTE: the engine impl ignores its internal success flag and always returns the target mesh, so success is measured by comparing the triangle count, not by the return value.
//   @param actor_name - Label or name of the target DynamicMeshActor (mutated in place)
//   @param fill_holes - Optional bool override for FGeometryScriptMeshSelfUnionOptions.bFillHoles (default true)
//   @param trim_flaps - Optional bool override for FGeometryScriptMeshSelfUnionOptions.bTrimFlaps (default true)
//   @param simplify_output - Optional bool override for FGeometryScriptMeshSelfUnionOptions.bSimplifyOutput (default true)
//   @param winding_threshold - Optional [0..1] float override for FGeometryScriptMeshSelfUnionOptions.WindingThreshold (default 0.5). Space with generalised winding number above this is considered inside.
//   @returns { actor_name: string, triangle_count: int, triangle_count_before: int, vertex_count: int, is_closed: bool, repaired: bool }
TSharedPtr<FJsonObject> HandleRepairMeshSelfIntersections(const TSharedPtr<FJsonObject>& Params)
{
    FGeometryScriptMeshActor Target;
    if (TSharedPtr<FJsonObject> Failure = GeometryScript_ResolveMeshActor(Params, Target))
    {
        return Failure;
    }

    FGeometryScriptMeshSelfUnionOptions Options; // defaults documented in MeshBooleanFunctions.h
    Params->TryGetBoolField(TEXT("fill_holes"), Options.bFillHoles);
    Params->TryGetBoolField(TEXT("trim_flaps"), Options.bTrimFlaps);
    Params->TryGetBoolField(TEXT("simplify_output"), Options.bSimplifyOutput);
    double WindingThreshold = 0.0;
    if (Params->TryGetNumberField(TEXT("winding_threshold"), WindingThreshold))
    {
        Options.WindingThreshold = static_cast<float>(WindingThreshold);
    }

    const int32 TrisBefore = Target.Mesh->GetTriangleCount();

    const FScopedTransaction Transaction(LOCTEXT("RepairMeshSelfIntersections", "MCP: Repair Mesh Self-Intersections"));
    Target.Actor->Modify();
    FGeometryScriptMeshEdit MeshEdit(Target.Mesh);

    // NOTE: ApplyMeshSelfUnion always returns TargetMesh (the impl ignores its internal
    // bSuccess), so we cannot use the return value as a success signal — measure by tris.
    UGeometryScriptLibrary_MeshBooleanFunctions::ApplyMeshSelfUnion(
        Target.Mesh, Options, /*Debug*/ nullptr);
    MeshEdit.Commit();

    const FGeometryScriptMeshCounts After = GeometryScript_NotifyAndCount(Target.Component, Target.Mesh);
    const bool bIsClosed = UGeometryScriptLibrary_MeshQueryFunctions::GetIsClosedMesh(Target.Mesh);

    TSharedPtr<FJsonObject> Result = GeometryScript_NewActorData(Target.Actor);
    Result->SetNumberField(TEXT("triangle_count"), After.Triangles);
    Result->SetNumberField(TEXT("triangle_count_before"), TrisBefore);
    Result->SetNumberField(TEXT("vertex_count"), After.Vertices);
    Result->SetBoolField(TEXT("is_closed"), bIsClosed);
    Result->SetBoolField(TEXT("repaired"), After.Triangles != TrisBefore);
    return mcp_common::MakeOkReply(Result);
}

// MCP:apply_mesh_morphology - Apply an SDF-based morphology op (dilate/contract/close/open) to the live dynamic mesh of a DynamicMeshActor in the editor world, found by label or name, replacing the mesh with an SDF-based offset mesh approximation as one undo step that restores the original mesh; nothing is saved. distance is the offset in cm and must be > 0; a missing or unknown operation, a missing distance or an unknown actor is an error checked before anything changes. When the engine reports an error (a result with no triangles, as when a contract erases the whole mesh, or from an empty mesh), morphed is false, the call fails and the mesh is left as it was. Wrap into a solid instead with apply_mesh_solidify; persist the result with copy_dynamic_mesh_to_static_mesh.
//   @param actor_name - Label or name of the target DynamicMeshActor
//   @param operation - Required. One of: dilate, contract, close, open.
//   @param distance - Required positive float. Offset distance in cm (SDF-based).
//   @returns { actor_name, operation, distance, triangle_count, triangle_count_before, vertex_count, morphed }
TSharedPtr<FJsonObject> HandleApplyMeshMorphology(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World, TEXT("Failed to get editor world"))) { return NoWorld; }

    FString ActorName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    FString OpName;
    if (!Params->TryGetStringField(TEXT("operation"), OpName) || OpName.IsEmpty())
        return mcp_common::MakeErrorReply(TEXT("Missing 'operation' (dilate|contract|close|open)"));

    double DistanceD = 0.0;
    if (!Params->TryGetNumberField(TEXT("distance"), DistanceD))
        return mcp_common::MakeErrorReply(TEXT("Missing 'distance' (float, > 0)"));
    if (DistanceD <= 0.0)
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("distance must be > 0 (got %f)"), DistanceD));

    const FString OpLower = OpName.ToLower();
    EGeometryScriptMorphologicalOpType Op = EGeometryScriptMorphologicalOpType::Dilate;
    if      (OpLower == TEXT("dilate"))   { Op = EGeometryScriptMorphologicalOpType::Dilate;   }
    else if (OpLower == TEXT("contract")) { Op = EGeometryScriptMorphologicalOpType::Contract; }
    else if (OpLower == TEXT("close"))    { Op = EGeometryScriptMorphologicalOpType::Close;    }
    else if (OpLower == TEXT("open"))     { Op = EGeometryScriptMorphologicalOpType::Open;     }
    else
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Unknown operation '%s' (expected dilate|contract|close|open)"), *OpName));

    FGeometryScriptMeshActor Target;
    if (TSharedPtr<FJsonObject> Failure = GeometryScript_FindMeshActor(World, ActorName, Target))
    {
        return Failure;
    }

    FGeometryScriptMorphologyOptions Options;
    Options.Operation = Op;
    Options.Distance = static_cast<float>(DistanceD);

    const int32 TrisBefore = Target.Mesh->GetTriangleCount();

    const FScopedTransaction Transaction(LOCTEXT("ApplyMeshMorphology", "MCP: Apply Mesh Morphology"));
    Target.Actor->Modify();
    FGeometryScriptMeshEdit MeshEdit(Target.Mesh);

    // ApplyMeshMorphology returns the target mesh whatever happened and reports a failure (a result with no
    // triangles, which it leaves in the mesh) only as an error in Debug: the mesh is then put back as it was and
    // the call fails with the engine's message.
    UGeometryScriptDebug* Debug = GeometryScript_NewDebug();
    UGeometryScriptLibrary_MeshVoxelFunctions::ApplyMeshMorphology(Target.Mesh, Options, Debug);
    const FString MorphologyError = GeometryScript_FirstError(Debug);
    const bool bSucceeded = MorphologyError.IsEmpty();
    if (bSucceeded)
    {
        MeshEdit.Commit();
    }
    else
    {
        MeshEdit.Revert();
    }
    const FGeometryScriptMeshCounts After = GeometryScript_NotifyAndCount(Target.Component, Target.Mesh);

    TSharedPtr<FJsonObject> Result = GeometryScript_NewActorData(Target.Actor);
    Result->SetStringField(TEXT("operation"), OpName);
    Result->SetNumberField(TEXT("distance"), DistanceD);
    Result->SetNumberField(TEXT("triangle_count"), After.Triangles);
    Result->SetNumberField(TEXT("triangle_count_before"), TrisBefore);
    Result->SetNumberField(TEXT("vertex_count"), After.Vertices);
    Result->SetBoolField(TEXT("morphed"), bSucceeded);
    return GeometryScript_OutcomeReply(Result, bSucceeded, TEXT("apply_mesh_morphology"), TEXT("morphed"), MorphologyError);
}

// MCP:get_dynamic_mesh_info - Read geometry stats off a DynamicMeshActor's live dynamic mesh
//   @param actor_name - Label or name of the target DynamicMeshActor
//   @returns { actor_name: string, actor_internal_name: string, triangle_count: int, vertex_count: int, is_closed: bool, is_empty: bool }
TSharedPtr<FJsonObject> HandleGetDynamicMeshInfo(const TSharedPtr<FJsonObject>& Params)
{
    FGeometryScriptMeshActor Target;
    if (TSharedPtr<FJsonObject> Failure = GeometryScript_ResolveMeshActor(Params, Target))
    {
        return Failure;
    }

    const int32 TriCount = Target.Mesh->GetTriangleCount();
    const int32 VertCount = UGeometryScriptLibrary_MeshQueryFunctions::GetVertexCount(Target.Mesh);
    const bool bIsClosed = UGeometryScriptLibrary_MeshQueryFunctions::GetIsClosedMesh(Target.Mesh);

    TSharedPtr<FJsonObject> Result = GeometryScript_NewActorData(Target.Actor);
    Result->SetStringField(TEXT("actor_internal_name"), Target.Actor->GetName());
    Result->SetNumberField(TEXT("triangle_count"), TriCount);
    Result->SetNumberField(TEXT("vertex_count"), VertCount);
    Result->SetBoolField(TEXT("is_closed"), bIsClosed);
    Result->SetBoolField(TEXT("is_empty"), Target.Mesh->IsEmpty());
    return mcp_common::MakeOkReply(Result);
}

// MCP:recompute_mesh_normals - Recompute the normals of the live dynamic mesh of a DynamicMeshActor in the editor world, overwriting the values of its existing normal overlay while keeping its hard-edge splits; a mesh without normals first gets per-vertex normals. Face contributions are weighted by area and by corner angle (both on by default; both off gives a plain average). Idempotent, nothing saved: one undo step restores the previous normals, and a recompute that changes nothing adds no undo step. An empty mesh is an error, and recomputed is false (the call fails, leaving the mesh as it was) when the engine reports an error. Use it when shading looks wrong after geometry edits; copy_dynamic_mesh_to_static_mesh persists the result.
//   @param actor_name [!] - Label or name of the target DynamicMeshActor
//   @param angle_weighted [bool] - Weight contributing face normals by corner angle, default true
//   @param area_weighted [bool] - Weight contributing face normals by face area, default true
//   @returns { actor_name: string, angle_weighted: bool, area_weighted: bool, triangle_count: int, vertex_count: int, recomputed: bool }
TSharedPtr<FJsonObject> HandleRecomputeMeshNormals(const TSharedPtr<FJsonObject>& Params)
{
    FGeometryScriptMeshActor Target;
    if (TSharedPtr<FJsonObject> Failure = GeometryScript_ResolveMeshActor(Params, Target))
    {
        return Failure;
    }

    // Per-field init (not positional brace): both fields are bool and adjacent, so a
    // positional literal could silently swap angle/area. UE-canonical idiom.
    FGeometryScriptCalculateNormalsOptions NormalsOptions;
    bool bAngleWeighted = true;
    bool bAreaWeighted = true;
    Params->TryGetBoolField(TEXT("angle_weighted"), bAngleWeighted);
    Params->TryGetBoolField(TEXT("area_weighted"), bAreaWeighted);
    NormalsOptions.bAngleWeighted = bAngleWeighted;
    NormalsOptions.bAreaWeighted = bAreaWeighted;

    // A mesh without triangles has no normals to compute; refused before anything is touched.
    if (Target.Mesh->GetTriangleCount() == 0)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("DynamicMeshActor '%s' has an empty mesh: there are no normals to recompute"), *Target.Name));
    }

    const FScopedTransaction Transaction(LOCTEXT("RecomputeMeshNormals", "MCP: Recompute Mesh Normals"));
    Target.Actor->Modify();
    FGeometryScriptMeshEdit MeshEdit(Target.Mesh);

    // Headless CPU recompute. With no prior normal overlay this falls back to per-vertex normals (a warning,
    // not an error); an error in Debug means the engine did not recompute.
    UGeometryScriptDebug* Debug = GeometryScript_NewDebug();
    UGeometryScriptLibrary_MeshNormalsFunctions::RecomputeNormals(
        Target.Mesh, NormalsOptions, /*bDeferChangeNotifications*/ false, Debug);
    const FString NormalsError = GeometryScript_FirstError(Debug);
    if (NormalsError.IsEmpty())
    {
        MeshEdit.Commit();
    }
    else
    {
        MeshEdit.Revert();
    }

    const FGeometryScriptMeshCounts After = GeometryScript_NotifyAndCount(Target.Component, Target.Mesh);

    UE_LOG(LogTemp, Verbose, TEXT("recompute_mesh_normals: actor='%s' angle=%d area=%d -> tris=%d verts=%d"),
        *Target.Name, bAngleWeighted ? 1 : 0, bAreaWeighted ? 1 : 0, After.Triangles, After.Vertices);

    TSharedPtr<FJsonObject> Result = GeometryScript_NewActorData(Target.Actor);
    Result->SetBoolField(TEXT("angle_weighted"), bAngleWeighted);
    Result->SetBoolField(TEXT("area_weighted"), bAreaWeighted);
    Result->SetNumberField(TEXT("triangle_count"), After.Triangles);
    Result->SetNumberField(TEXT("vertex_count"), After.Vertices);
    Result->SetBoolField(TEXT("recomputed"), NormalsError.IsEmpty());
    return GeometryScript_OutcomeReply(Result, NormalsError.IsEmpty(), TEXT("recompute_mesh_normals"), TEXT("recomputed"),
        NormalsError);
}

// MCP:generate_mesh_uvs - Planar-project UVs for the whole live dynamic mesh of a DynamicMeshActor in the editor world into channel uv_channel, overwriting it: the projection runs down mesh-local Z with 1 UV unit per cm, not fitted to 0-1. A missing channel is added first (the channel count only grows, so higher channels such as a lightmap UV1 are kept). uv_channel 8 or more, or an empty mesh, is an error checked before anything changes; uvs_generated is false (the call fails, leaving the mesh as it was) when the engine reports an error. Idempotent, nothing saved: one undo step restores the previous UVs and channel count. For box, cylinder or region projections use set_mesh_uvs_from_projection; for a real unwrap, auto_generate_mesh_uvs.
//   @param actor_name [!] - Label or name of the target DynamicMeshActor
//   @param uv_channel [int] - UV set index to write, 0 to 7, default 0
//   @returns { actor_name: string, uv_channel: int, uv_channel_count: int, triangle_count: int, vertex_count: int, uvs_generated: bool }
TSharedPtr<FJsonObject> HandleGenerateMeshUVs(const TSharedPtr<FJsonObject>& Params)
{
    FGeometryScriptMeshActor Target;
    if (TSharedPtr<FJsonObject> Failure = GeometryScript_ResolveMeshActor(Params, Target))
    {
        return Failure;
    }

    // Read as a double so an out-of-range number is refused instead of falling back to channel 0.
    double UVChannelD = 0.0;
    Params->TryGetNumberField(TEXT("uv_channel"), UVChannelD);
    if (UVChannelD < 0.0 || UVChannelD >= GeometryScript_MaxUVChannels)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("'uv_channel' must be between 0 and %d (got %g): a dynamic mesh has at most %d UV channels"),
            GeometryScript_MaxUVChannels - 1, UVChannelD, GeometryScript_MaxUVChannels));
    const int32 UVChannel = static_cast<int32>(UVChannelD);

    // A mesh without triangles has nothing to project onto; refused before any channel is added.
    if (Target.Mesh->GetTriangleCount() == 0)
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("DynamicMeshActor '%s' has an empty mesh: there are no triangles to project UVs onto"), *Target.Name));

    const int32 ChannelsBefore = UGeometryScriptLibrary_MeshQueryFunctions::GetNumUVSets(Target.Mesh);

    const FScopedTransaction Transaction(LOCTEXT("GenerateMeshUVs", "MCP: Generate Mesh UVs"));
    Target.Actor->Modify();
    FGeometryScriptMeshEdit MeshEdit(Target.Mesh);

    // SetMeshUVsFromPlanarProjection writes nothing to a channel the mesh lacks, so a missing channel is added
    // first. The count only ever grows: SetNumUVSets sets an exact count, and shrinking it would delete every
    // channel above uv_channel (a lightmap UV1, say).
    if (UVChannel >= ChannelsBefore)
    {
        UGeometryScriptLibrary_MeshUVFunctions::SetNumUVSets(Target.Mesh, UVChannel + 1, /*Debug*/ nullptr);
    }

    // Empty FGeometryScriptMeshSelection => whole mesh. Identity plane transform.
    UGeometryScriptDebug* Debug = GeometryScript_NewDebug();
    UGeometryScriptLibrary_MeshUVFunctions::SetMeshUVsFromPlanarProjection(
        Target.Mesh, UVChannel, FTransform::Identity, FGeometryScriptMeshSelection(), Debug);
    const FString ProjectionError = GeometryScript_FirstError(Debug);
    const bool bGenerated = ProjectionError.IsEmpty() && UVChannel < UGeometryScriptLibrary_MeshQueryFunctions::GetNumUVSets(Target.Mesh);
    if (bGenerated)
    {
        MeshEdit.Commit();
    }
    else
    {
        MeshEdit.Revert();
    }

    const FGeometryScriptMeshCounts After = GeometryScript_NotifyAndCount(Target.Component, Target.Mesh);
    const int32 ChannelsAfter = UGeometryScriptLibrary_MeshQueryFunctions::GetNumUVSets(Target.Mesh);

    UE_LOG(LogTemp, Verbose, TEXT("generate_mesh_uvs: actor='%s' uv_channel=%d channels=%d->%d -> tris=%d verts=%d"),
        *Target.Name, UVChannel, ChannelsBefore, ChannelsAfter, After.Triangles, After.Vertices);

    TSharedPtr<FJsonObject> Result = GeometryScript_NewActorData(Target.Actor);
    Result->SetNumberField(TEXT("uv_channel"), UVChannel);
    Result->SetNumberField(TEXT("uv_channel_count"), ChannelsAfter);
    Result->SetNumberField(TEXT("triangle_count"), After.Triangles);
    Result->SetNumberField(TEXT("vertex_count"), After.Vertices);
    Result->SetBoolField(TEXT("uvs_generated"), bGenerated);
    return GeometryScript_OutcomeReply(Result, bGenerated, TEXT("generate_mesh_uvs"), TEXT("uvs_generated"),
        ProjectionError.IsEmpty() ? FString::Printf(TEXT("the mesh has %d UV channels, so channel %d does not exist"), ChannelsAfter, UVChannel)
                                  : ProjectionError);
}

// MCP:simplify_mesh - QEM-simplify a DynamicMeshActor's dynamic mesh to a target triangle count. One undo step restores the mesh; a call that changes nothing adds no undo step.
//   @param actor_name [!] - Label or name of the target DynamicMeshActor
//   @param target_triangle_count [int!] - Target triangle count to simplify down to (must be > 0)
//   @returns { actor_name: string, target_triangle_count: int, triangle_count_before: int, triangle_count_after: int, vertex_count: int, simplified: bool }
TSharedPtr<FJsonObject> HandleSimplifyMesh(const TSharedPtr<FJsonObject>& Params)
{
    FGeometryScriptMeshActor Target;
    if (TSharedPtr<FJsonObject> Failure = GeometryScript_ResolveMeshActor(Params, Target))
    {
        return Failure;
    }

    int32 TargetTriangleCount = 0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("target_triangle_count"), TargetTriangleCount)) { return Missing; }
    if (TargetTriangleCount <= 0)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("'target_triangle_count' must be > 0 (got %d)"), TargetTriangleCount));

    const int32 TrisBefore = Target.Mesh->GetTriangleCount();

    const FScopedTransaction Transaction(LOCTEXT("SimplifyMesh", "MCP: Simplify Mesh"));
    Target.Actor->Modify();
    FGeometryScriptMeshEdit MeshEdit(Target.Mesh);

    // Pure-CPU QEM (default options = attribute-aware QEM, headless-safe; NOT the
    // WITH_EDITOR ApplyEditorSimplify* path). If target >= current the mesh does not
    // grow (no-op); an empty mesh is a silent no-op.
    FGeometryScriptSimplifyMeshOptions SimplifyOptions;
    UGeometryScriptLibrary_MeshSimplifyFunctions::ApplySimplifyToTriangleCount(
        Target.Mesh, TargetTriangleCount, SimplifyOptions, /*Debug*/ nullptr);
    MeshEdit.Commit();

    const FGeometryScriptMeshCounts After = GeometryScript_NotifyAndCount(Target.Component, Target.Mesh);

    UE_LOG(LogTemp, Verbose, TEXT("simplify_mesh: actor='%s' target=%d -> before=%d after=%d verts=%d"),
        *Target.Name, TargetTriangleCount, TrisBefore, After.Triangles, After.Vertices);

    TSharedPtr<FJsonObject> Result = GeometryScript_NewActorData(Target.Actor);
    Result->SetNumberField(TEXT("target_triangle_count"), TargetTriangleCount);
    Result->SetNumberField(TEXT("triangle_count_before"), TrisBefore);
    Result->SetNumberField(TEXT("triangle_count_after"), After.Triangles);
    Result->SetNumberField(TEXT("vertex_count"), After.Vertices);
    Result->SetBoolField(TEXT("simplified"), After.Triangles < TrisBefore);
    return mcp_common::MakeOkReply(Result);
}

// MCP:weld_mesh_edges - Weld the coincident open boundary edges of a DynamicMeshActor's live dynamic mesh in the editor world (actor found by label or object name), merging boundary edge pairs whose endpoints lie within tolerance so cracks and duplicated seams close. The mesh changes in place as one undo step that restores it, and nothing is saved; a mesh with nothing to weld (closed, or no pair within tolerance) is a success with welded=false and adds no undo step. welded is true when the vertex or triangle count dropped; a failure the engine's welder reports makes success false and leaves the mesh as it was. A negative tolerance or an unknown actor is an error. Count open edges first with get_num_open_border_edges; self-intersections: repair_mesh_self_intersections; orphan vertices: remove_unused_vertices; persist the result with copy_dynamic_mesh_to_static_mesh.
//   @param actor_name [!] - Label or object name of the DynamicMeshActor in the editor world (case-insensitive).
//   @param tolerance [float] - Largest distance, in the mesh's local units (cm before actor scale), between the endpoints of two boundary edges for them to be welded; must be >= 0 (default 1e-06).
//   @param only_unique_pairs [bool] - When true (default), weld only edges that have exactly one coincident partner; false also merges ambiguous matches.
//   @returns { actor_name: string, tolerance: number, only_unique_pairs: bool, triangle_count: int, vertex_count: int, welded: bool }
TSharedPtr<FJsonObject> HandleWeldMeshEdges(const TSharedPtr<FJsonObject>& Params)
{
    FGeometryScriptMeshActor Target;
    if (TSharedPtr<FJsonObject> Failure = GeometryScript_ResolveMeshActor(Params, Target))
    {
        return Failure;
    }

    // FGeometryScriptWeldEdgesOptions has only 2 fields (Tolerance + bOnlyUniquePairs);
    // per-field init keeps it simple and matches the recompute_mesh_normals idiom.
    FGeometryScriptWeldEdgesOptions WeldOptions; // defaults: Tolerance 1e-06, bOnlyUniquePairs true
    double Tolerance = WeldOptions.Tolerance;
    if (Params->TryGetNumberField(TEXT("tolerance"), Tolerance))
    {
        if (Tolerance < 0.0)
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("'tolerance' must be >= 0 (got %f)"), Tolerance));
        WeldOptions.Tolerance = static_cast<float>(Tolerance);
    }
    bool bOnlyUniquePairs = WeldOptions.bOnlyUniquePairs;
    Params->TryGetBoolField(TEXT("only_unique_pairs"), bOnlyUniquePairs);
    WeldOptions.bOnlyUniquePairs = bOnlyUniquePairs;

    const FScopedTransaction Transaction(LOCTEXT("WeldMeshEdges", "MCP: Weld Mesh Edges"));
    Target.Actor->Modify();
    FGeometryScriptMeshEdit MeshEdit(Target.Mesh);

    const int32 TriCountBefore = Target.Mesh->GetTriangleCount();
    const int32 VertCountBefore = UGeometryScriptLibrary_MeshQueryFunctions::GetVertexCount(Target.Mesh);

    // Pure-CPU edge weld. Closed meshes are a no-op (no open boundary edges to weld);
    // re-applying on a clean mesh is idempotent. The welder reports a failure only as an error in Debug: the mesh is
    // then put back as it was and the call fails with the engine's message.
    UGeometryScriptDebug* Debug = GeometryScript_NewDebug();
    UGeometryScriptLibrary_MeshRepairFunctions::WeldMeshEdges(
        Target.Mesh, WeldOptions, Debug);
    const FString WeldError = GeometryScript_FirstError(Debug);
    if (WeldError.IsEmpty())
    {
        MeshEdit.Commit();
    }
    else
    {
        MeshEdit.Revert();
    }

    const FGeometryScriptMeshCounts After = GeometryScript_NotifyAndCount(Target.Component, Target.Mesh);

    UE_LOG(LogTemp, Verbose, TEXT("weld_mesh_edges: actor='%s' tolerance=%f unique=%d -> tris=%d->%d verts=%d->%d"),
        *Target.Name, WeldOptions.Tolerance, WeldOptions.bOnlyUniquePairs ? 1 : 0,
        TriCountBefore, After.Triangles, VertCountBefore, After.Vertices);

    TSharedPtr<FJsonObject> Result = GeometryScript_NewActorData(Target.Actor);
    Result->SetNumberField(TEXT("tolerance"), WeldOptions.Tolerance);
    Result->SetBoolField(TEXT("only_unique_pairs"), WeldOptions.bOnlyUniquePairs);
    Result->SetNumberField(TEXT("triangle_count"), After.Triangles);
    Result->SetNumberField(TEXT("vertex_count"), After.Vertices);
    Result->SetBoolField(TEXT("welded"), WeldError.IsEmpty()
        && (After.Triangles < TriCountBefore || After.Vertices < VertCountBefore));
    if (!WeldError.IsEmpty())
    {
        return GeometryScript_OutcomeReply(Result, false, TEXT("weld_mesh_edges"), TEXT("welded"), WeldError);
    }
    // A mesh with nothing to weld is a valid no-op: success, with welded false.
    return mcp_common::MakeOkReply(Result);
}

// MCP:remove_unused_vertices - Remove the vertices no triangle references from a DynamicMeshActor's live dynamic mesh in the editor world and refresh its render data. Remaining vertex IDs are not renumbered and a clean mesh is a no-op that adds no undo step; otherwise one undo step restores the mesh. Returns vertex_count_before, vertex_count, vertices_removed and removed.
//   @param actor_name [!] - Label or object name (case-insensitive) of the target DynamicMeshActor in the editor world.
//   @returns { actor_name: string, triangle_count: int, vertex_count: int, vertex_count_before: int, vertices_removed: int, removed: bool }
TSharedPtr<FJsonObject> HandleRemoveUnusedVertices(const TSharedPtr<FJsonObject>& Params)
{
    FGeometryScriptMeshActor Target;
    if (TSharedPtr<FJsonObject> Failure = GeometryScript_ResolveMeshActor(Params, Target))
    {
        return Failure;
    }

    const int32 VertCountBefore = UGeometryScriptLibrary_MeshQueryFunctions::GetVertexCount(Target.Mesh);

    const FScopedTransaction Transaction(LOCTEXT("RemoveUnusedVertices", "MCP: Remove Unused Vertices"));
    Target.Actor->Modify();
    FGeometryScriptMeshEdit MeshEdit(Target.Mesh);

    // Pure-CPU vertex compaction. On a clean mesh it's a no-op (idempotent).
    // Note: does NOT renumber remaining vertex IDs — that's CompactMesh's job.
    UGeometryScriptLibrary_MeshRepairFunctions::RemoveUnusedVertices(
        Target.Mesh, /*Debug*/ nullptr);
    MeshEdit.Commit();

    const FGeometryScriptMeshCounts After = GeometryScript_NotifyAndCount(Target.Component, Target.Mesh);

    UE_LOG(LogTemp, Verbose, TEXT("remove_unused_vertices: actor='%s' -> verts=%d->%d tris=%d"),
        *Target.Name, VertCountBefore, After.Vertices, After.Triangles);

    TSharedPtr<FJsonObject> Result = GeometryScript_NewActorData(Target.Actor);
    Result->SetNumberField(TEXT("triangle_count"), After.Triangles);
    Result->SetNumberField(TEXT("vertex_count"), After.Vertices);
    Result->SetNumberField(TEXT("vertex_count_before"), VertCountBefore);
    Result->SetNumberField(TEXT("vertices_removed"), VertCountBefore - After.Vertices);
    Result->SetBoolField(TEXT("removed"), After.Vertices < VertCountBefore);
    return mcp_common::MakeOkReply(Result);
}

// MCP:compact_mesh - Compact a DynamicMeshActor's live dynamic mesh in the editor world (actor found by label or object name): renumber its vertex, triangle and edge IDs into dense ranges, dropping the free ID slots that deletions leave. Geometry and the vertex and triangle counts stay the same, but IDs move, so vertex or triangle IDs read before (select_mesh_triangles, get_mesh_components_detail) are stale afterwards. Idempotent: compacted is true when any ID moved, and one undo step restores the old IDs; an already compact mesh is a success with compacted=false and adds no undo step. Nothing is saved; an unknown actor is an error. Run it after delete_selected_triangles_from_mesh or remove_unused_vertices, and before the xatlas method of auto_generate_mesh_uvs.
//   @param actor_name [!] - Label or object name of the DynamicMeshActor in the editor world (case-insensitive).
//   @returns { actor_name: string, triangle_count: int, vertex_count: int, compacted: bool }
TSharedPtr<FJsonObject> HandleCompactMesh(const TSharedPtr<FJsonObject>& Params)
{
    FGeometryScriptMeshActor Target;
    if (TSharedPtr<FJsonObject> Failure = GeometryScript_ResolveMeshActor(Params, Target))
    {
        return Failure;
    }

    const int32 TriCountBefore = Target.Mesh->GetTriangleCount();
    const int32 VertCountBefore = UGeometryScriptLibrary_MeshQueryFunctions::GetVertexCount(Target.Mesh);

    const FScopedTransaction Transaction(LOCTEXT("CompactMesh", "MCP: Compact Mesh"));
    Target.Actor->Modify();
    FGeometryScriptMeshEdit MeshEdit(Target.Mesh);

    // Pure-CPU ID renumbering. On a clean mesh (no holes in ID lists) it's a no-op
    // (idempotent). This is the canonical step after RemoveUnusedVertices.
    UGeometryScriptLibrary_MeshRepairFunctions::CompactMesh(
        Target.Mesh, /*Debug*/ nullptr);
    // Compaction renumbers IDs and keeps the live vertex and triangle counts, so only the mesh comparison of the
    // commit tells whether any ID moved.
    const bool bCompacted = MeshEdit.Commit();

    const FGeometryScriptMeshCounts After = GeometryScript_NotifyAndCount(Target.Component, Target.Mesh);

    UE_LOG(LogTemp, Verbose, TEXT("compact_mesh: actor='%s' -> verts=%d->%d tris=%d->%d"),
        *Target.Name, VertCountBefore, After.Vertices, TriCountBefore, After.Triangles);

    TSharedPtr<FJsonObject> Result = GeometryScript_NewActorData(Target.Actor);
    Result->SetNumberField(TEXT("triangle_count"), After.Triangles);
    Result->SetNumberField(TEXT("vertex_count"), After.Vertices);
    Result->SetBoolField(TEXT("compacted"), bCompacted);
    return mcp_common::MakeOkReply(Result);
}

// MCP:apply_uniform_remesh - Remesh the live dynamic mesh of a DynamicMeshActor in the editor world into evenly sized triangles, aiming at target_edge_length (cm) or target_tri_count: exactly one is required, else an error. The mesh changes in place as one undo step that restores it, and the actor's package is marked dirty, not saved. smoothing_type picks uniform, UV-preserving or mixed smoothing; an unknown value is an error. The reply echoes the settings the engine was given. remeshed is true when the triangle or vertex count changed. For variable density use apply_adaptive_remesh; to only reduce triangles, simplify_mesh.
//   @param actor_name [!] - Label or name of the target DynamicMeshActor
//   @param target_edge_length [float] - Target uniform edge length in mesh units (sets TargetType=TargetEdgeLength). Mutually exclusive with target_tri_count; at least one is required.
//   @param target_tri_count [int] - Approximate target triangle count (sets TargetType=TriangleCount). Mutually exclusive with target_edge_length.
//   @param preserve_sharp_edges [bool] - Approximate 'preserve sharp edges' by enabling bPreventNormalFlips on the shared RemeshOptions (default true; UE's FGeometryScriptUniformRemeshOptions has no explicit preserve-sharp flag, so the closest equivalent is the shared RemeshOptions.bPreventNormalFlips).
//   @param smoothing_type [string] - One of 'uniform' | 'uv_preserving' | 'mixed' (default 'mixed'); maps to EGeometryScriptRemeshSmoothingType on the shared RemeshOptions.
//   @param auto_compact [bool] - Auto-compact the index space after remesh on the shared RemeshOptions (default true).
//   @returns { actor_name: string, target_edge_length: number, target_tri_count: int, preserve_sharp_edges: bool, smoothing_type: string, auto_compact: bool, target_type: string, triangle_count: int, triangle_count_before: int, vertex_count: int, remeshed: bool }
TSharedPtr<FJsonObject> HandleApplyUniformRemesh(const TSharedPtr<FJsonObject>& Params)
{
    FGeometryScriptMeshActor Target;
    if (TSharedPtr<FJsonObject> Failure = GeometryScript_ResolveMeshActor(Params, Target))
    {
        return Failure;
    }

    // Resolve TargetType + target value from the (mutually-exclusive) JSON fields.
    // FGeometryScriptUniformRemeshOptions has exactly one target field actually
    // consulted (depending on TargetType); we populate both to keep the echo honest
    // and to make the unused one visible to callers via target_tri_count/target_edge_length.
    double TargetEdgeLength = 0.0;
    double TargetTriCount = 0.0;
    const bool bHasEdgeLength = Params->TryGetNumberField(TEXT("target_edge_length"), TargetEdgeLength);
    const bool bHasTriCount   = Params->TryGetNumberField(TEXT("target_tri_count"), TargetTriCount);
    if (!bHasEdgeLength && !bHasTriCount)
        return mcp_common::MakeErrorReply(TEXT("Uniform remesh requires 'target_edge_length' or 'target_tri_count'"));
    if (bHasEdgeLength && bHasTriCount)
        return mcp_common::MakeErrorReply(TEXT("Uniform remesh 'target_edge_length' and 'target_tri_count' are mutually exclusive"));
    if (bHasEdgeLength && TargetEdgeLength <= 0.0)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("'target_edge_length' must be > 0 (got %f)"), TargetEdgeLength));
    if (bHasTriCount && TargetTriCount <= 0.0)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("'target_tri_count' must be > 0 (got %f)"), TargetTriCount));

    // smoothing_type: the published snake_case values map to EGeometryScriptRemeshSmoothingType here, case-insensitively
    // (the engine name UVPreserving is accepted too). Anything else is refused before the mesh is touched.
    FString SmoothingTypeStr = TEXT("mixed");
    Params->TryGetStringField(TEXT("smoothing_type"), SmoothingTypeStr);
    EGeometryScriptRemeshSmoothingType SmoothingType = EGeometryScriptRemeshSmoothingType::Mixed;
    if (SmoothingTypeStr.Equals(TEXT("uniform"), ESearchCase::IgnoreCase))
    {
        SmoothingType = EGeometryScriptRemeshSmoothingType::Uniform;
    }
    else if (SmoothingTypeStr.Equals(TEXT("uv_preserving"), ESearchCase::IgnoreCase)
        || SmoothingTypeStr.Equals(TEXT("uvpreserving"), ESearchCase::IgnoreCase))
    {
        SmoothingType = EGeometryScriptRemeshSmoothingType::UVPreserving;
    }
    else if (!SmoothingTypeStr.Equals(TEXT("mixed"), ESearchCase::IgnoreCase))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Unknown smoothing_type '%s' (expected uniform|uv_preserving|mixed)"), *SmoothingTypeStr));
    }

    // preserve_sharp_edges -> bPreventNormalFlips (closest UE equivalent on the shared struct), default true.
    bool bPreserveSharpEdges = true;
    Params->TryGetBoolField(TEXT("preserve_sharp_edges"), bPreserveSharpEdges);

    // auto_compact -> bAutoCompact, default true.
    bool bAutoCompact = true;
    Params->TryGetBoolField(TEXT("auto_compact"), bAutoCompact);

    // The three requested fields are written directly; every other field keeps the engine default
    // (0.25 smoothing rate, Free boundary constraints, all ops allowed, 20 iterations, reprojection on).
    FGeometryScriptRemeshOptions RemeshOptions;
    RemeshOptions.SmoothingType = SmoothingType;
    RemeshOptions.bPreventNormalFlips = bPreserveSharpEdges;
    RemeshOptions.bAutoCompact = bAutoCompact;

    // Build UniformOptions with the resolved TargetType + target value.
    FGeometryScriptUniformRemeshOptions UniformOptions; // defaults: TriangleCount=5000, TargetEdgeLength=1.0.
    {
        TSharedRef<FJsonObject> OptJson = MakeShared<FJsonObject>();
        if (bHasEdgeLength)
        {
            OptJson->SetStringField(TEXT("TargetType"), TEXT("TargetEdgeLength"));
            OptJson->SetNumberField(TEXT("TargetEdgeLength"), TargetEdgeLength);
        }
        else
        {
            OptJson->SetStringField(TEXT("TargetType"), TEXT("TriangleCount"));
            OptJson->SetNumberField(TEXT("TargetTriangleCount"), TargetTriCount);
        }
        FJsonObjectConverter::JsonObjectToUStruct(OptJson, FGeometryScriptUniformRemeshOptions::StaticStruct(), &UniformOptions, /*CheckFlags*/ 0, /*SkipFlags*/ 0);
    }

    const int32 TrisBefore = Target.Mesh->GetTriangleCount();
    const int32 VertCountBefore = UGeometryScriptLibrary_MeshQueryFunctions::GetVertexCount(Target.Mesh);

    const FScopedTransaction Transaction(LOCTEXT("ApplyUniformRemesh", "MCP: Apply Uniform Remesh"));
    Target.Actor->Modify();
    FGeometryScriptMeshEdit MeshEdit(Target.Mesh);

    // Pure-CPU isotropic remesh (default options are headless-safe; Debug=nullptr).
    // Re-applying with the same params is convergent (target-driven, not query-driven) —
    // not strictly idempotent on intermediate states, but converges to the same target.
    UGeometryScriptLibrary_RemeshingFunctions::ApplyUniformRemesh(
        Target.Mesh, RemeshOptions, UniformOptions, /*Debug*/ nullptr);
    MeshEdit.Commit();

    const FGeometryScriptMeshCounts After = GeometryScript_NotifyAndCount(Target.Component, Target.Mesh);
    if (UPackage* Pkg = Target.Actor->GetPackage())
    {
        Pkg->MarkPackageDirty();
    }

    const FString TargetTypeStr = bHasEdgeLength ? TEXT("TargetEdgeLength") : TEXT("TriangleCount");

    UE_LOG(LogTemp, Verbose, TEXT("apply_uniform_remesh: actor='%s' target_type=%s target=%s -> tris=%d->%d verts=%d->%d"),
        *Target.Name, *TargetTypeStr,
        bHasEdgeLength ? *FString::Printf(TEXT("edge=%.6f"), TargetEdgeLength)
                       : *FString::Printf(TEXT("tris=%.0f"), TargetTriCount),
        TrisBefore, After.Triangles, VertCountBefore, After.Vertices);

    TSharedPtr<FJsonObject> Result = GeometryScript_NewActorData(Target.Actor);
    Result->SetNumberField(TEXT("target_edge_length"), bHasEdgeLength ? TargetEdgeLength : 0.0);
    Result->SetNumberField(TEXT("target_tri_count"), bHasTriCount ? TargetTriCount : 0);
    Result->SetBoolField(TEXT("preserve_sharp_edges"), RemeshOptions.bPreventNormalFlips);
    Result->SetStringField(TEXT("smoothing_type"),
        StaticEnum<EGeometryScriptRemeshSmoothingType>()->GetNameStringByValue(static_cast<int64>(RemeshOptions.SmoothingType)));
    Result->SetBoolField(TEXT("auto_compact"), RemeshOptions.bAutoCompact);
    Result->SetStringField(TEXT("target_type"), TargetTypeStr);
    Result->SetNumberField(TEXT("triangle_count"), After.Triangles);
    Result->SetNumberField(TEXT("triangle_count_before"), TrisBefore);
    Result->SetNumberField(TEXT("vertex_count"), After.Vertices);
    Result->SetBoolField(TEXT("remeshed"), After.Triangles != TrisBefore || After.Vertices != VertCountBefore);
    return mcp_common::MakeOkReply(Result);
}

// MCP:apply_adaptive_remesh - Adaptive-remesh the live dynamic mesh of a DynamicMeshActor in the editor world, found by label or name, toward max_edge_length, with the edge length scaled per vertex by a weight map and relative_density, as one undo step that restores the mesh; nothing is saved. The engine needs that weight map: without weight_map_name, or with a name the mesh has no weight layer for, it reports an error, remeshed is false and the call fails with the mesh unchanged (weight_map_resolved says whether the name was found). A missing or non-positive max_edge_length or an unknown actor is an error. remeshed is true when the triangle or vertex count changed. For uniform density use apply_uniform_remesh; to only reduce triangles, simplify_mesh.
//   @param actor_name [!] - Label or name of the target DynamicMeshActor
//   @param min_edge_length [float] - Soft floor for adaptive edge length (default 1.0). NOTE: FGeometryScriptAdaptiveRemeshOptions has no explicit min field — this is echoed back in the result for traceability but is NOT consulted by the engine call.
//   @param max_edge_length [float!] - Target edge length for adaptive remesh (sets TargetType=TargetEdgeLength + TargetEdgeLength). Required.
//   @param relative_density [float] - Density adjustment in [-2.0, 2.0] passed to FGeometryScriptAdaptiveRemeshOptions::RelativeDensity (default 0.0).
//   @param weight_map_name [string] - Name of an existing vertex weight map on the mesh (resolved via UGeometryScriptLibrary_MeshQueryFunctions::GetWeightMapHandle); the engine's adaptive remesh needs one, so an omitted name or one the mesh has no layer for makes the call fail with the mesh unchanged.
//   @returns { actor_name: string, min_edge_length: number, max_edge_length: number, relative_density: number, weight_map_name: string, weight_map_resolved: bool, target_type: string, triangle_count: int, triangle_count_before: int, vertex_count: int, remeshed: bool }
TSharedPtr<FJsonObject> HandleApplyAdaptiveRemesh(const TSharedPtr<FJsonObject>& Params)
{
    FGeometryScriptMeshActor Target;
    if (TSharedPtr<FJsonObject> Failure = GeometryScript_ResolveMeshActor(Params, Target))
    {
        return Failure;
    }

    // Adaptive remesh target edge length (only a single TargetEdgeLength is exposed on
    // FGeometryScriptAdaptiveRemeshOptions; the min_edge_length field is echoed but not used).
    double MaxEdgeLength = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("max_edge_length"), MaxEdgeLength)) { return Missing; }
    if (MaxEdgeLength <= 0.0)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("'max_edge_length' must be > 0 (got %f)"), MaxEdgeLength));

    double MinEdgeLength = 1.0;
    Params->TryGetNumberField(TEXT("min_edge_length"), MinEdgeLength);

    double RelativeDensity = 0.0;
    Params->TryGetNumberField(TEXT("relative_density"), RelativeDensity);

    // Weight-map resolution. GetWeightMapHandle returns an invalid handle (-1) if the named layer does not exist;
    // ApplyAdaptiveRemesh refuses an invalid handle (an error in Debug, the mesh untouched), which is reported below.
    FString WeightMapName;
    Params->TryGetStringField(TEXT("weight_map_name"), WeightMapName);
    bool bWeightMapResolved = false;
    FGeometryScriptWeightMapHandle WeightMapHandle;
    if (!WeightMapName.IsEmpty())
    {
        EGeometryScriptSearchOutcomePins Outcome = EGeometryScriptSearchOutcomePins::NotFound;
        WeightMapHandle = UGeometryScriptLibrary_MeshQueryFunctions::GetWeightMapHandle(
            Target.Mesh, FName(*WeightMapName), Outcome);
        bWeightMapResolved = (Outcome == EGeometryScriptSearchOutcomePins::Found) && WeightMapHandle.IsValid();
        if (!bWeightMapResolved)
        {
            UE_LOG(LogTemp, Verbose, TEXT("apply_adaptive_remesh: weight_map '%s' not found on actor '%s'"),
                *WeightMapName, *Target.Name);
            WeightMapHandle = FGeometryScriptWeightMapHandle(); // reset to invalid (-1)
        }
    }

    // Adaptive remesh reuses the same shared FGeometryScriptRemeshOptions as Uniform
    // (same struct: smoothing, boundary constraints, iterations, auto-compact). We use
    // the struct's UE-canonical defaults — no params exposed for them in this command.
    FGeometryScriptRemeshOptions RemeshOptions;

    // Build AdaptiveOptions with TargetEdgeLength + RelativeDensity.
    FGeometryScriptAdaptiveRemeshOptions AdaptiveOptions; // defaults: TriangleCount=5000, TargetEdgeLength=1.0, RelativeDensity=0.
    {
        TSharedRef<FJsonObject> OptJson = MakeShared<FJsonObject>();
        OptJson->SetStringField(TEXT("TargetType"), TEXT("TargetEdgeLength"));
        OptJson->SetNumberField(TEXT("TargetEdgeLength"), MaxEdgeLength);
        OptJson->SetNumberField(TEXT("RelativeDensity"), RelativeDensity);
        FJsonObjectConverter::JsonObjectToUStruct(OptJson, FGeometryScriptAdaptiveRemeshOptions::StaticStruct(), &AdaptiveOptions, /*CheckFlags*/ 0, /*SkipFlags*/ 0);
    }

    const int32 TrisBefore = Target.Mesh->GetTriangleCount();
    const int32 VertCountBefore = UGeometryScriptLibrary_MeshQueryFunctions::GetVertexCount(Target.Mesh);

    const FScopedTransaction Transaction(LOCTEXT("ApplyAdaptiveRemesh", "MCP: Apply Adaptive Remesh"));
    Target.Actor->Modify();
    FGeometryScriptMeshEdit MeshEdit(Target.Mesh);

    // Pure-CPU anisotropic remesh (default RemeshOptions are headless-safe). The engine reports a failure only as an
    // error in Debug, as it does for an invalid weight-map handle (refused before the mesh is touched): the mesh is
    // then put back as it was and the call fails with the engine's message.
    UGeometryScriptDebug* Debug = GeometryScript_NewDebug();
    UGeometryScriptLibrary_RemeshingFunctions::ApplyAdaptiveRemesh(
        Target.Mesh, RemeshOptions, AdaptiveOptions, WeightMapHandle, Debug);
    const FString RemeshError = GeometryScript_FirstError(Debug);
    if (RemeshError.IsEmpty())
    {
        MeshEdit.Commit();
    }
    else
    {
        MeshEdit.Revert();
    }

    const FGeometryScriptMeshCounts After = GeometryScript_NotifyAndCount(Target.Component, Target.Mesh);
    if (UPackage* Pkg = Target.Actor->GetPackage())
    {
        Pkg->MarkPackageDirty();
    }

    UE_LOG(LogTemp, Verbose, TEXT("apply_adaptive_remesh: actor='%s' min=%f max=%f rel=%f weight='%s' resolved=%d -> tris=%d->%d verts=%d->%d"),
        *Target.Name, MinEdgeLength, MaxEdgeLength, RelativeDensity,
        *WeightMapName, bWeightMapResolved ? 1 : 0,
        TrisBefore, After.Triangles, VertCountBefore, After.Vertices);

    TSharedPtr<FJsonObject> Result = GeometryScript_NewActorData(Target.Actor);
    Result->SetNumberField(TEXT("min_edge_length"), MinEdgeLength);
    Result->SetNumberField(TEXT("max_edge_length"), MaxEdgeLength);
    Result->SetNumberField(TEXT("relative_density"), AdaptiveOptions.RelativeDensity);
    Result->SetStringField(TEXT("weight_map_name"), WeightMapName);
    Result->SetBoolField(TEXT("weight_map_resolved"), bWeightMapResolved);
    Result->SetStringField(TEXT("target_type"), TEXT("TargetEdgeLength"));
    Result->SetNumberField(TEXT("triangle_count"), After.Triangles);
    Result->SetNumberField(TEXT("triangle_count_before"), TrisBefore);
    Result->SetNumberField(TEXT("vertex_count"), After.Vertices);
    Result->SetBoolField(TEXT("remeshed"), RemeshError.IsEmpty()
        && (After.Triangles != TrisBefore || After.Vertices != VertCountBefore));
    if (!RemeshError.IsEmpty())
    {
        return GeometryScript_OutcomeReply(Result, false, TEXT("apply_adaptive_remesh"), TEXT("remeshed"), RemeshError);
    }
    return mcp_common::MakeOkReply(Result);
}

// ===========================================================================
// StaticMesh asset queries.
// All read-only on a UStaticMesh via UGeometryScriptLibrary_StaticMeshFunctions
// + UGeometryScriptLibrary_MeshQueryFunctions. Headless-safe. No save.
// ===========================================================================

// MCP:get_mesh_bounding_box - Read the axis-aligned bounding box of a UStaticMesh asset in mesh-local space (cm, no actor transform), measured on a transient dynamic-mesh copy with the asset's build settings and build scale applied: min, max, center, and extent as the half-size on each axis. Read-only: the asset is loaded, not changed. lod_index does not pick the geometry: the engine reads the hi-res source model when there is one, else LOD 0, and lod_index and read_hi_res_source say which. A path that does not load as a static mesh, or a mesh with no geometry to read, is an error. The bounds stored on the asset, with any bounds extension and a sphere radius: get_static_mesh_info; a placed actor in world space: get_actor_bounds; part of a DynamicMeshActor: get_mesh_selection_bounds.
//   @param static_mesh_path - Content path to the UStaticMesh asset (e.g. "/Game/Meshes/SM_Cube")
//   @param lod_index - LOD asked for (default 0), echoed as requested_lod_index; it does not select the geometry, which is the hi-res source model or else LOD 0.
//   @returns { static_mesh_path, lod_index, requested_lod_index, read_hi_res_source, min_x, min_y, min_z, max_x, max_y, max_z, extent_x, extent_y, extent_z, center_x, center_y, center_z }
TSharedPtr<FJsonObject> HandleGetMeshBoundingBox(
    const TSharedPtr<FJsonObject>& Params)
{
    FString MeshPath;
    FGeometryScriptStaticMeshLOD LOD;
    UDynamicMesh* DynMesh = nullptr;
    if (TSharedPtr<FJsonObject> Failure = GeometryScript_CopyStaticMeshParam(Params, MeshPath, LOD, DynMesh))
        return Failure;

    const FBox Box = UGeometryScriptLibrary_MeshQueryFunctions::GetMeshBoundingBox(DynMesh);

    TSharedPtr<FJsonObject> Data = GeometryScript_NewStaticMeshData(MeshPath, LOD);
    Data->SetNumberField(TEXT("min_x"), Box.Min.X);
    Data->SetNumberField(TEXT("min_y"), Box.Min.Y);
    Data->SetNumberField(TEXT("min_z"), Box.Min.Z);
    Data->SetNumberField(TEXT("max_x"), Box.Max.X);
    Data->SetNumberField(TEXT("max_y"), Box.Max.Y);
    Data->SetNumberField(TEXT("max_z"), Box.Max.Z);
    Data->SetNumberField(TEXT("extent_x"), Box.GetExtent().X);
    Data->SetNumberField(TEXT("extent_y"), Box.GetExtent().Y);
    Data->SetNumberField(TEXT("extent_z"), Box.GetExtent().Z);
    Data->SetNumberField(TEXT("center_x"), Box.GetCenter().X);
    Data->SetNumberField(TEXT("center_y"), Box.GetCenter().Y);
    Data->SetNumberField(TEXT("center_z"), Box.GetCenter().Z);
    return mcp_common::MakeOkReply(Data);
}

// MCP:get_mesh_volume_area - Read the surface area (cm^2) and signed volume (cm^3) of one LOD of a UStaticMesh asset, measured on a transient copy with the asset's build settings and build scale applied, in mesh-local space (no actor transform). The volume is an enclosed volume only when is_closed is true; on an open mesh it is not one, and reversed winding makes it negative. Read-only: the asset is loaded, not changed. lod_index does not choose the geometry: the engine reads the hi-res source model when the asset has one, else LOD 0, and lod_index and read_hi_res_source in the reply say which was read. A path that does not load as a static mesh, or a mesh with no geometry to read, is an error. Center of mass too: get_mesh_volume_area_center; bounds: get_mesh_bounding_box; watertightness alone: get_mesh_is_closed.
//   @param static_mesh_path - Asset path of the UStaticMesh, e.g. "/Game/Meshes/SM_Rock" (the object path form also works).
//   @param lod_index - LOD asked for (default 0), echoed as requested_lod_index; it does not select the geometry, which is the hi-res source model or else LOD 0.
//   @returns { static_mesh_path, lod_index, requested_lod_index, read_hi_res_source, surface_area, volume, is_closed }
TSharedPtr<FJsonObject> HandleGetMeshVolumeArea(
    const TSharedPtr<FJsonObject>& Params)
{
    FString MeshPath;
    FGeometryScriptStaticMeshLOD LOD;
    UDynamicMesh* DynMesh = nullptr;
    if (TSharedPtr<FJsonObject> Failure = GeometryScript_CopyStaticMeshParam(Params, MeshPath, LOD, DynMesh))
        return Failure;

    float SurfaceArea = 0.f;
    float Volume = 0.f;
    UGeometryScriptLibrary_MeshQueryFunctions::GetMeshVolumeArea(DynMesh, SurfaceArea, Volume);
    const bool bClosed = UGeometryScriptLibrary_MeshQueryFunctions::GetIsClosedMesh(DynMesh);

    TSharedPtr<FJsonObject> Data = GeometryScript_NewStaticMeshData(MeshPath, LOD);
    Data->SetNumberField(TEXT("surface_area"), SurfaceArea);
    Data->SetNumberField(TEXT("volume"), Volume);
    Data->SetBoolField(TEXT("is_closed"), bClosed);
    return mcp_common::MakeOkReply(Data);
}

// MCP:get_mesh_volume_area_center - Read the surface area (cm^2), signed volume (cm^3) and center of mass of a UStaticMesh asset, measured on a transient dynamic-mesh copy with the asset's build settings and build scale applied, in mesh-local space with no actor transform. The center is the centroid of the enclosed solid at uniform density; it and the volume mean something only when is_closed is true, and reversed winding makes the volume negative. Read-only: the asset is not changed. lod_index does not pick the geometry: the engine reads the hi-res source model when there is one, else LOD 0, and lod_index and read_hi_res_source say which. A path that does not load as a static mesh, or a mesh with no geometry to read, is an error. Area and volume alone: get_mesh_volume_area; bounds: get_mesh_bounding_box.
//   @param static_mesh_path - Content path to the UStaticMesh asset
//   @param lod_index - LOD asked for (default 0), echoed as requested_lod_index; it does not select the geometry, which is the hi-res source model or else LOD 0.
//   @returns { static_mesh_path, lod_index, requested_lod_index, read_hi_res_source, surface_area, volume, center_of_mass_x, center_of_mass_y, center_of_mass_z, is_closed }
TSharedPtr<FJsonObject> HandleGetMeshVolumeAreaCenter(
    const TSharedPtr<FJsonObject>& Params)
{
    FString MeshPath;
    FGeometryScriptStaticMeshLOD LOD;
    UDynamicMesh* DynMesh = nullptr;
    if (TSharedPtr<FJsonObject> Failure = GeometryScript_CopyStaticMeshParam(Params, MeshPath, LOD, DynMesh))
        return Failure;

    float SurfaceArea = 0.f;
    float Volume = 0.f;
    FVector CenterOfMass = FVector::ZeroVector;
    UGeometryScriptLibrary_MeshQueryFunctions::GetMeshVolumeAreaCenter(DynMesh, SurfaceArea, Volume, CenterOfMass);
    const bool bClosed = UGeometryScriptLibrary_MeshQueryFunctions::GetIsClosedMesh(DynMesh);

    TSharedPtr<FJsonObject> Data = GeometryScript_NewStaticMeshData(MeshPath, LOD);
    Data->SetNumberField(TEXT("surface_area"), SurfaceArea);
    Data->SetNumberField(TEXT("volume"), Volume);
    Data->SetNumberField(TEXT("center_of_mass_x"), CenterOfMass.X);
    Data->SetNumberField(TEXT("center_of_mass_y"), CenterOfMass.Y);
    Data->SetNumberField(TEXT("center_of_mass_z"), CenterOfMass.Z);
    Data->SetBoolField(TEXT("is_closed"), bClosed);
    return mcp_common::MakeOkReply(Data);
}

// MCP:get_mesh_is_closed - Test whether one LOD of a UStaticMesh asset is closed (watertight): is_closed is true when the mesh, copied to a transient dynamic mesh with build settings applied, has triangles and no boundary edge, that is no edge with a single adjacent triangle. An empty mesh reports false. Read-only: the asset is loaded, not changed. lod_index does not choose the geometry: the engine reads the hi-res source model when the asset has one, else LOD 0, and lod_index and read_hi_res_source in the reply say which was read. A path that does not load as a static mesh, or a mesh with no geometry to read, is an error. To locate the holes use get_mesh_open_border_loops; area and volume (with is_closed): get_mesh_volume_area; a DynamicMeshActor in the level: get_num_open_border_edges or get_dynamic_mesh_info.
//   @param static_mesh_path - Asset path of the UStaticMesh, e.g. "/Game/Meshes/SM_Rock" (the object path form also works).
//   @param lod_index - LOD asked for (default 0), echoed as requested_lod_index; it does not select the geometry, which is the hi-res source model or else LOD 0.
//   @returns { static_mesh_path, lod_index, requested_lod_index, read_hi_res_source, is_closed }
TSharedPtr<FJsonObject> HandleGetMeshIsClosed(
    const TSharedPtr<FJsonObject>& Params)
{
    FString MeshPath;
    FGeometryScriptStaticMeshLOD LOD;
    UDynamicMesh* DynMesh = nullptr;
    if (TSharedPtr<FJsonObject> Failure = GeometryScript_CopyStaticMeshParam(Params, MeshPath, LOD, DynMesh))
        return Failure;

    const bool bClosed = UGeometryScriptLibrary_MeshQueryFunctions::GetIsClosedMesh(DynMesh);

    TSharedPtr<FJsonObject> Data = GeometryScript_NewStaticMeshData(MeshPath, LOD);
    Data->SetBoolField(TEXT("is_closed"), bClosed);
    return mcp_common::MakeOkReply(Data);
}

// MCP:get_mesh_open_border_loops - Count the open border loops of a UStaticMesh asset: closed chains of boundary edges (edges with one adjacent triangle), so each hole or open rim counts once and a closed mesh gives 0. Computed on a transient dynamic-mesh copy with build settings applied; read-only. A chain the engine cannot close into a loop, at a bowtie vertex, is left out of the count, and ambiguous_topology does not flag it: with the engine's default loop settings it stays false. lod_index does not pick the geometry: the engine reads the hi-res source model when there is one, else LOD 0, and lod_index and read_hi_res_source say which. A path that is not a static mesh, or one with no geometry, is an error. Yes or no: get_mesh_is_closed; a DynamicMeshActor in the level: get_num_open_border_edges.
//   @param static_mesh_path - Content path to the UStaticMesh asset
//   @param lod_index - LOD asked for (default 0), echoed as requested_lod_index; it does not select the geometry, which is the hi-res source model or else LOD 0.
//   @returns { static_mesh_path, lod_index, requested_lod_index, read_hi_res_source, open_border_loops, ambiguous_topology }
TSharedPtr<FJsonObject> HandleGetMeshOpenBorderLoops(
    const TSharedPtr<FJsonObject>& Params)
{
    FString MeshPath;
    FGeometryScriptStaticMeshLOD LOD;
    UDynamicMesh* DynMesh = nullptr;
    if (TSharedPtr<FJsonObject> Failure = GeometryScript_CopyStaticMeshParam(Params, MeshPath, LOD, DynMesh))
        return Failure;

    bool bAmbiguous = false;
    const int32 NumLoops = UGeometryScriptLibrary_MeshQueryFunctions::GetNumOpenBorderLoops(DynMesh, bAmbiguous);

    TSharedPtr<FJsonObject> Data = GeometryScript_NewStaticMeshData(MeshPath, LOD);
    Data->SetNumberField(TEXT("open_border_loops"), NumLoops);
    Data->SetBoolField(TEXT("ambiguous_topology"), bAmbiguous);
    return mcp_common::MakeOkReply(Data);
}

// MCP:get_mesh_connected_components - Count the connected components (islands: sets of triangles joined through shared edges) of one LOD of a UStaticMesh asset, computed on a transient dynamic mesh copy with build settings applied. One solid piece gives 1 and an empty mesh 0; parts that only touch or overlap without sharing edges count separately. Read-only: the asset is loaded, not changed. lod_index does not choose the geometry: the engine reads the hi-res source model when the asset has one, else LOD 0, and lod_index and read_hi_res_source in the reply say which was read. A path that does not load as a static mesh, or a mesh with no geometry to read, is an error. For a DynamicMeshActor in the level use get_num_connected_islands, or get_mesh_components_detail for each island's triangle count, bounds and material IDs.
//   @param static_mesh_path - Asset path of the UStaticMesh, e.g. "/Game/Meshes/SM_Rock" (the object path form also works).
//   @param lod_index - LOD asked for (default 0), echoed as requested_lod_index; it does not select the geometry, which is the hi-res source model or else LOD 0.
//   @returns { static_mesh_path, lod_index, requested_lod_index, read_hi_res_source, connected_components }
TSharedPtr<FJsonObject> HandleGetMeshConnectedComponents(
    const TSharedPtr<FJsonObject>& Params)
{
    FString MeshPath;
    FGeometryScriptStaticMeshLOD LOD;
    UDynamicMesh* DynMesh = nullptr;
    if (TSharedPtr<FJsonObject> Failure = GeometryScript_CopyStaticMeshParam(Params, MeshPath, LOD, DynMesh))
        return Failure;

    const int32 NumComponents = UGeometryScriptLibrary_MeshQueryFunctions::GetNumConnectedComponents(DynMesh);

    TSharedPtr<FJsonObject> Data = GeometryScript_NewStaticMeshData(MeshPath, LOD);
    Data->SetNumberField(TEXT("connected_components"), NumComponents);
    return mcp_common::MakeOkReply(Data);
}

// MCP:get_mesh_vertex_count - Count the vertices of a UStaticMesh asset on a transient dynamic-mesh copy with build settings applied and degenerate triangles kept: vertex_count is the live vertices, vertex_id_count the size of the ID space, and has_vertex_id_gaps is true when they differ. The copy shares one vertex across UV and normal seams, so the count is usually below the render vertex counts of list_uv_channels (per LOD) and get_static_mesh_info (LOD 0). Read-only: the asset is not changed. lod_index does not pick the geometry: the engine reads the hi-res source model when there is one, else LOD 0, and lod_index and read_hi_res_source say which. A path that does not load as a static mesh, or a mesh with no geometry, is an error. Triangles: get_mesh_triangle_count; a DynamicMeshActor in the level: get_dynamic_mesh_info.
//   @param static_mesh_path - Content path to the UStaticMesh asset
//   @param lod_index - LOD asked for (default 0), echoed as requested_lod_index; it does not select the geometry, which is the hi-res source model or else LOD 0.
//   @returns { static_mesh_path, lod_index, requested_lod_index, read_hi_res_source, vertex_count, vertex_id_count, has_vertex_id_gaps }
TSharedPtr<FJsonObject> HandleGetMeshVertexCount(
    const TSharedPtr<FJsonObject>& Params)
{
    FString MeshPath;
    FGeometryScriptStaticMeshLOD LOD;
    UDynamicMesh* DynMesh = nullptr;
    if (TSharedPtr<FJsonObject> Failure = GeometryScript_CopyStaticMeshParam(Params, MeshPath, LOD, DynMesh))
        return Failure;

    const int32 VertexCount = UGeometryScriptLibrary_MeshQueryFunctions::GetVertexCount(DynMesh);
    const int32 NumVertexIDs = UGeometryScriptLibrary_MeshQueryFunctions::GetNumVertexIDs(DynMesh);
    const bool bHasGaps = UGeometryScriptLibrary_MeshQueryFunctions::GetHasVertexIDGaps(DynMesh);

    TSharedPtr<FJsonObject> Data = GeometryScript_NewStaticMeshData(MeshPath, LOD);
    Data->SetNumberField(TEXT("vertex_count"), VertexCount);
    Data->SetNumberField(TEXT("vertex_id_count"), NumVertexIDs);
    Data->SetBoolField(TEXT("has_vertex_id_gaps"), bHasGaps);
    return mcp_common::MakeOkReply(Data);
}

// MCP:get_mesh_triangle_count - Count the triangles of one LOD of a UStaticMesh asset, copied to a transient dynamic mesh with build settings applied and degenerate triangles kept. triangle_count is the number of live triangles, triangle_id_count the size of the triangle ID space, and has_triangle_id_gaps is true when they differ (free ID slots, which a fresh copy does not normally have). Read-only: the asset is loaded, not changed. lod_index does not choose the geometry: the engine reads the hi-res source model when the asset has one, else LOD 0, and lod_index and read_hi_res_source in the reply say which was read. A path that does not load as a static mesh, or a mesh with no geometry to read, is an error. Vertices: get_mesh_vertex_count; LOD count and materials: get_static_mesh_info; a DynamicMeshActor in the level: get_dynamic_mesh_info.
//   @param static_mesh_path - Asset path of the UStaticMesh, e.g. "/Game/Meshes/SM_Rock" (the object path form also works).
//   @param lod_index - LOD asked for (default 0), echoed as requested_lod_index; it does not select the geometry, which is the hi-res source model or else LOD 0.
//   @returns { static_mesh_path, lod_index, requested_lod_index, read_hi_res_source, triangle_count, triangle_id_count, has_triangle_id_gaps }
TSharedPtr<FJsonObject> HandleGetMeshTriangleCount(
    const TSharedPtr<FJsonObject>& Params)
{
    FString MeshPath;
    FGeometryScriptStaticMeshLOD LOD;
    UDynamicMesh* DynMesh = nullptr;
    if (TSharedPtr<FJsonObject> Failure = GeometryScript_CopyStaticMeshParam(Params, MeshPath, LOD, DynMesh))
        return Failure;

    const int32 NumTriangleIDs = UGeometryScriptLibrary_MeshQueryFunctions::GetNumTriangleIDs(DynMesh);
    const int32 TriangleCount = DynMesh->GetTriangleCount();
    const bool bHasGaps = UGeometryScriptLibrary_MeshQueryFunctions::GetHasTriangleIDGaps(DynMesh);

    TSharedPtr<FJsonObject> Data = GeometryScript_NewStaticMeshData(MeshPath, LOD);
    Data->SetNumberField(TEXT("triangle_count"), TriangleCount);
    Data->SetNumberField(TEXT("triangle_id_count"), NumTriangleIDs);
    Data->SetBoolField(TEXT("has_triangle_id_gaps"), bHasGaps);
    return mcp_common::MakeOkReply(Data);
}

// MCP:get_mesh_uv_channel_count - Count the UV channels of a UStaticMesh asset as GeometryScript reads them: the UV layers of a transient dynamic-mesh copy of its geometry, 0 when the copy carries no attributes. Read-only: the asset is not changed. lod_index does not pick the geometry: the engine reads the hi-res source model when there is one, else LOD 0, and lod_index and read_hi_res_source say which. A lower LOD can carry a different number. A path that does not load as a static mesh, or a mesh with no geometry, is an error. Use it to pick a valid uv_channel for get_mesh_uv_area; every LOD's count and the lightmap channel index: list_uv_channels; overlaps and out-of-square UVs: get_uv_health_report; empty a channel: clear_uv_channel.
//   @param static_mesh_path - Content path to the UStaticMesh asset
//   @param lod_index - LOD asked for (default 0), echoed as requested_lod_index; it does not select the geometry, which is the hi-res source model or else LOD 0.
//   @returns { static_mesh_path, lod_index, requested_lod_index, read_hi_res_source, uv_channel_count }
TSharedPtr<FJsonObject> HandleGetMeshUVChannelCount(
    const TSharedPtr<FJsonObject>& Params)
{
    FString MeshPath;
    FGeometryScriptStaticMeshLOD LOD;
    UDynamicMesh* DynMesh = nullptr;
    if (TSharedPtr<FJsonObject> Failure = GeometryScript_CopyStaticMeshParam(Params, MeshPath, LOD, DynMesh))
        return Failure;

    const int32 NumUVChannels = UGeometryScriptLibrary_MeshQueryFunctions::GetNumUVSets(DynMesh);

    TSharedPtr<FJsonObject> Data = GeometryScript_NewStaticMeshData(MeshPath, LOD);
    Data->SetNumberField(TEXT("uv_channel_count"), NumUVChannels);
    return mcp_common::MakeOkReply(Data);
}

// MCP:get_mesh_uv_area - Sum the UV-space area of one UV channel of a UStaticMesh asset on a transient dynamic-mesh copy: each triangle with UVs set adds its unsigned area and one without UVs adds nothing, so UVs that fill the 0-1 square once sum to 1.0, and overlapping or stacked islands all count, which can push the sum past 1. uv_channel defaults to 0; a negative one is an error, but a channel the mesh lacks is a success with is_valid_uv_channel=false and uv_area 0. Read-only. lod_index does not pick the geometry: the engine reads the hi-res source model when there is one, else LOD 0, and lod_index and read_hi_res_source say which. A path that is not a static mesh, or one with no geometry, is an error. Channel count: get_mesh_uv_channel_count; overlaps: get_uv_health_report.
//   @param static_mesh_path - Content path to the UStaticMesh asset
//   @param uv_channel - UV channel index (default 0)
//   @param lod_index - LOD asked for (default 0), echoed as requested_lod_index; it does not select the geometry, which is the hi-res source model or else LOD 0.
//   @returns { static_mesh_path, lod_index, requested_lod_index, read_hi_res_source, uv_channel, uv_area, is_valid_uv_channel }
TSharedPtr<FJsonObject> HandleGetMeshUVArea(
    const TSharedPtr<FJsonObject>& Params)
{
    FString MeshPath;
    FGeometryScriptStaticMeshLOD LOD;
    UStaticMesh* StaticMesh = nullptr;
    if (TSharedPtr<FJsonObject> Failure = GeometryScript_LoadStaticMeshParam(Params, MeshPath, LOD, StaticMesh))
        return Failure;

    int32 UVChannel = 0;
    Params->TryGetNumberField(TEXT("uv_channel"), UVChannel);
    if (UVChannel < 0)
        return mcp_common::MakeErrorReply(TEXT("uv_channel must be >= 0"));

    FString DynError;
    UDynamicMesh* DynMesh = BuildDynamicMeshFromStatic(StaticMesh, LOD.Requested, DynError);
    if (!DynMesh)
        return mcp_common::MakeErrorReply(DynError);

    bool bIsValidUVChannel = false;
    const double UVArea = UGeometryScriptLibrary_MeshQueryFunctions::GetMeshUVArea(DynMesh, UVChannel, bIsValidUVChannel);

    TSharedPtr<FJsonObject> Data = GeometryScript_NewStaticMeshData(MeshPath, LOD);
    Data->SetNumberField(TEXT("uv_channel"), UVChannel);
    Data->SetNumberField(TEXT("uv_area"), UVArea);
    Data->SetBoolField(TEXT("is_valid_uv_channel"), bIsValidUVChannel);
    return mcp_common::MakeOkReply(Data);
}

// ===========================================================================
// Dynamic-mesh basic edits + transforms.
// All operate on the live UDynamicMesh* of an ADynamicMeshActor (mutations).
// ===========================================================================

// MCP:append_mesh_to_dynamic_mesh - Merge another UDynamicMesh into the live dynamic mesh of a DynamicMeshActor via UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMesh. One undo step puts the target mesh back as it was; the source mesh is not changed.
//   @param actor_name - Label or name of the target DynamicMeshActor (the receiver)
//   @param source_actor_name - Label or name of the DynamicMeshActor whose dynamic mesh will be merged in
//   @param append_transform - Optional [x,y,z] world-space transform applied to the source mesh before merging (default identity)
//   @returns { actor_name, source_actor_name, triangles_before, triangles_after, triangles_added, vertex_count_after, appended: bool } -- appended is false when the source mesh has no triangles
TSharedPtr<FJsonObject> HandleAppendMeshToDynamicMesh(
    const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName, SourceName;
    ADynamicMeshActor* DMActor = nullptr;
    ADynamicMeshActor* DMSource = nullptr;
    if (TSharedPtr<FJsonObject> Failure = GeometryScript_ResolveActorPair(Params, ActorName, SourceName, DMActor, DMSource))
        return Failure;

    UDynamicMeshComponent* MeshComp = DMActor->GetDynamicMeshComponent();
    UDynamicMesh* TargetMesh = MeshComp ? MeshComp->GetDynamicMesh() : nullptr;
    if (!TargetMesh)
        return mcp_common::MakeErrorReply(TEXT("Target DynamicMeshActor has no dynamic mesh"));

    UDynamicMeshComponent* SourceComp = DMSource->GetDynamicMeshComponent();
    UDynamicMesh* SourceMesh = SourceComp ? SourceComp->GetDynamicMesh() : nullptr;
    if (!SourceMesh)
        return mcp_common::MakeErrorReply(TEXT("Source DynamicMeshActor has no dynamic mesh"));

    // Optional world-space transform applied to the source mesh before appending.
    FVector Offset = FVector::ZeroVector;
    if (Params->HasField(TEXT("append_transform")))
    {
        Offset = mcp_common::ReadJsonVector(Params, TEXT("append_transform"));
    }
    const FTransform AppendTransform(FRotator::ZeroRotator, Offset, FVector::OneVector);

    const int32 TrisBefore = TargetMesh->GetTriangleCount();
    const int32 SourceTris = SourceMesh->GetTriangleCount();

    if (SourceTris == 0)
    {
        // Honest no-op: source is empty so nothing to merge. Return success with zeros.
        TSharedPtr<FJsonObject> Result = GeometryScript_NewActorData(DMActor);
        Result->SetStringField(TEXT("source_actor_name"), DMSource->GetActorLabel());
        Result->SetNumberField(TEXT("triangles_before"), TrisBefore);
        Result->SetNumberField(TEXT("triangles_after"), TrisBefore);
        Result->SetNumberField(TEXT("triangles_added"), 0);
        Result->SetNumberField(TEXT("vertex_count_after"), UGeometryScriptLibrary_MeshQueryFunctions::GetVertexCount(TargetMesh));
        Result->SetBoolField(TEXT("appended"), false);
        Result->SetStringField(TEXT("note"), TEXT("source mesh is empty (no triangles); nothing to merge"));
        return mcp_common::MakeOkReply(Result);
    }

    const FScopedTransaction Transaction(LOCTEXT("AppendMeshToDynamicMesh", "MCP: Append Mesh To Dynamic Mesh"));
    DMActor->Modify();
    DMSource->Modify();
    FGeometryScriptMeshEdit MeshEdit(TargetMesh);

    UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMesh(
        TargetMesh, SourceMesh, AppendTransform, /*bDeferChangeNotifications*/ false,
        FGeometryScriptAppendMeshOptions(), /*Debug*/ nullptr);
    MeshEdit.Commit();

    const FGeometryScriptMeshCounts After = GeometryScript_NotifyAndCount(MeshComp, TargetMesh);

    UE_LOG(LogTemp, Verbose, TEXT("append_mesh_to_dynamic_mesh: '%s' <- '%s' offset=%s -> tris=%d->%d verts=%d"),
        *ActorName, *SourceName, *Offset.ToString(),
        TrisBefore, After.Triangles, After.Vertices);

    TSharedPtr<FJsonObject> Result = GeometryScript_NewActorData(DMActor);
    Result->SetStringField(TEXT("source_actor_name"), DMSource->GetActorLabel());
    Result->SetNumberField(TEXT("triangles_before"), TrisBefore);
    Result->SetNumberField(TEXT("triangles_after"), After.Triangles);
    Result->SetNumberField(TEXT("triangles_added"), After.Triangles - TrisBefore);
    Result->SetNumberField(TEXT("vertex_count_after"), After.Vertices);
    Result->SetBoolField(TEXT("appended"), After.Triangles > TrisBefore);
    return mcp_common::MakeOkReply(Result);
}

// MCP:transform_dynamic_mesh - Apply a full FTransform (translation + rotation + scale) to the live dynamic mesh of a DynamicMeshActor via UGeometryScriptLibrary_MeshTransformFunctions::TransformMesh, as one undo step that restores the mesh. A location or scale that is not three numbers, or an empty mesh, is an error. An identity transform succeeds with applied false and a note, leaving the mesh unchanged.
//   @param actor_name - Label or name of the target DynamicMeshActor
//   @param location - Optional [x, y, z] translation; default (0,0,0)
//   @param rotation - Optional [pitch, yaw, roll] rotation; default (0,0,0)
//   @param scale - Optional [x, y, z] scale; default (1,1,1)
//   @returns { actor_name: string, location_x: number, location_y: number, location_z: number, rotation_pitch: number, rotation_yaw: number, rotation_roll: number, scale_x: number, scale_y: number, scale_z: number, applied: bool, note?: string }
TSharedPtr<FJsonObject> HandleTransformDynamicMesh(
    const TSharedPtr<FJsonObject>& Params)
{
    FGeometryScriptMeshActor Target;
    if (TSharedPtr<FJsonObject> Failure = GeometryScript_ResolveMeshActor(Params, Target))
    {
        return Failure;
    }
    if (TSharedPtr<FJsonObject> Malformed = GeometryScript_CheckVectorParam(Params, TEXT("location")))
    {
        return Malformed;
    }
    if (TSharedPtr<FJsonObject> Malformed = GeometryScript_CheckVectorParam(Params, TEXT("scale")))
    {
        return Malformed;
    }
    if (TSharedPtr<FJsonObject> Empty = GeometryScript_RequireVertices(Target))
    {
        return Empty;
    }

    const FVector Location = Params->HasField(TEXT("location"))
        ? mcp_common::ReadJsonVector(Params, TEXT("location"))
        : FVector::ZeroVector;
    const FRotator Rotation = Params->HasField(TEXT("rotation"))
        ? mcp_common::ReadJsonRotator(Params, TEXT("rotation"))
        : FRotator::ZeroRotator;
    FVector Scale = FVector::OneVector;
    if (Params->HasField(TEXT("scale")))
    {
        Scale = mcp_common::ReadJsonVector(Params, TEXT("scale"));
    }

    const FTransform Transform(Rotation, Location, Scale);
    const bool bApplied = !GeometryScript_IsIdentityTransform(Transform);
    if (bApplied)
    {
        const FScopedTransaction Transaction(LOCTEXT("TransformDynamicMesh", "MCP: Transform Dynamic Mesh"));
        Target.Actor->Modify();
        FGeometryScriptMeshEdit MeshEdit(Target.Mesh);

        UGeometryScriptLibrary_MeshTransformFunctions::TransformMesh(Target.Mesh, Transform, /*bFixOrientationForNegativeScale*/ false, /*Debug*/ nullptr);
        MeshEdit.Commit();

        if (Target.Component)
        {
            Target.Component->NotifyMeshUpdated();
        }
    }

    UE_LOG(LogTemp, Verbose, TEXT("transform_dynamic_mesh: '%s' transform=%s applied=%d"),
        *Target.Name, *Transform.ToString(), bApplied ? 1 : 0);

    TSharedPtr<FJsonObject> Result = GeometryScript_NewActorData(Target.Actor);
    Result->SetNumberField(TEXT("location_x"), Location.X);
    Result->SetNumberField(TEXT("location_y"), Location.Y);
    Result->SetNumberField(TEXT("location_z"), Location.Z);
    Result->SetNumberField(TEXT("rotation_pitch"), Rotation.Pitch);
    Result->SetNumberField(TEXT("rotation_yaw"), Rotation.Yaw);
    Result->SetNumberField(TEXT("rotation_roll"), Rotation.Roll);
    Result->SetNumberField(TEXT("scale_x"), Scale.X);
    Result->SetNumberField(TEXT("scale_y"), Scale.Y);
    Result->SetNumberField(TEXT("scale_z"), Scale.Z);
    return GeometryScript_TransformReply(Result, bApplied);
}

// MCP:translate_dynamic_mesh - Translate the live dynamic mesh of a DynamicMeshActor via UGeometryScriptLibrary_MeshTransformFunctions::TranslateMesh, as one undo step that restores the mesh. A translation that is not three numbers, or an empty mesh, is an error. A zero translation succeeds with applied false and a note, leaving the mesh unchanged.
//   @param actor_name - Label or name of the target DynamicMeshActor
//   @param translation - [x, y, z] translation delta in local space
//   @returns { actor_name: string, translation_x: number, translation_y: number, translation_z: number, applied: bool, note?: string }
TSharedPtr<FJsonObject> HandleTranslateDynamicMesh(
    const TSharedPtr<FJsonObject>& Params)
{
    FGeometryScriptMeshActor Target;
    if (TSharedPtr<FJsonObject> Failure = GeometryScript_ResolveMeshActor(Params, Target, TEXT("translation")))
    {
        return Failure;
    }
    if (TSharedPtr<FJsonObject> Malformed = GeometryScript_CheckVectorParam(Params, TEXT("translation")))
    {
        return Malformed;
    }
    if (TSharedPtr<FJsonObject> Empty = GeometryScript_RequireVertices(Target))
    {
        return Empty;
    }

    const FVector Translation = mcp_common::ReadJsonVector(Params, TEXT("translation"));
    const bool bApplied = !GeometryScript_IsIdentityTransform(FTransform(Translation));
    if (bApplied)
    {
        const FScopedTransaction Transaction(LOCTEXT("TranslateDynamicMesh", "MCP: Translate Dynamic Mesh"));
        Target.Actor->Modify();
        FGeometryScriptMeshEdit MeshEdit(Target.Mesh);

        UGeometryScriptLibrary_MeshTransformFunctions::TranslateMesh(Target.Mesh, Translation, /*Debug*/ nullptr);
        MeshEdit.Commit();

        if (Target.Component)
        {
            Target.Component->NotifyMeshUpdated();
        }
    }

    TSharedPtr<FJsonObject> Result = GeometryScript_NewActorData(Target.Actor);
    Result->SetNumberField(TEXT("translation_x"), Translation.X);
    Result->SetNumberField(TEXT("translation_y"), Translation.Y);
    Result->SetNumberField(TEXT("translation_z"), Translation.Z);
    return GeometryScript_TransformReply(Result, bApplied);
}

// MCP:rotate_dynamic_mesh - Rotate every vertex of the live dynamic mesh of a DynamicMeshActor in the editor world about the mesh-local origin, baking the rotation into the geometry; the actor transform is not touched. Rotations accumulate: each call turns the current mesh again, as one undo step that restores it; nothing saved. An empty mesh is an error; a zero rotation succeeds with applied false and a note, leaving the mesh unchanged. Use transform_dynamic_mesh to move, rotate and scale in one call, set_actor_transform to turn the actor itself, rotate_mesh_uvs to rotate UVs.
//   @param actor_name - Label or name of the target DynamicMeshActor
//   @param rotation [array!] - [pitch, yaw, roll] Euler rotation in degrees, applied on top of the current mesh
//   @returns { actor_name: string, rotation_pitch: number, rotation_yaw: number, rotation_roll: number, applied: bool, note?: string }
TSharedPtr<FJsonObject> HandleRotateDynamicMesh(
    const TSharedPtr<FJsonObject>& Params)
{
    FGeometryScriptMeshActor Target;
    if (TSharedPtr<FJsonObject> Failure = GeometryScript_ResolveMeshActor(Params, Target))
    {
        return Failure;
    }

    // rotation is required, as a [pitch, yaw, roll] list of at least three numbers or a {pitch, yaw, roll} object:
    // the rotator reader turns anything else into no rotation at all.
    const TArray<TSharedPtr<FJsonValue>>* RotationItems = nullptr;
    if (!Params->TryGetArrayField(TEXT("rotation"), RotationItems) && !Params->HasTypedField<EJson::Object>(TEXT("rotation")))
    {
        return mcp_common::MakeErrorReply(TEXT("Missing 'rotation': pass [pitch, yaw, roll] in degrees or a {pitch, yaw, roll} object"));
    }
    if (RotationItems != nullptr && RotationItems->Num() < 3)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("'rotation' must hold three numbers [pitch, yaw, roll] (got %d)"), RotationItems->Num()));
    }
    if (TSharedPtr<FJsonObject> Empty = GeometryScript_RequireVertices(Target))
    {
        return Empty;
    }

    const FRotator Rotation = mcp_common::ReadJsonRotator(Params, TEXT("rotation"));
    const bool bApplied = !GeometryScript_IsIdentityTransform(FTransform(Rotation));
    if (bApplied)
    {
        const FScopedTransaction Transaction(LOCTEXT("RotateDynamicMesh", "MCP: Rotate Dynamic Mesh"));
        Target.Actor->Modify();
        FGeometryScriptMeshEdit MeshEdit(Target.Mesh);

        UGeometryScriptLibrary_MeshTransformFunctions::RotateMesh(Target.Mesh, Rotation, /*RotationOrigin*/ FVector::ZeroVector, /*Debug*/ nullptr);
        MeshEdit.Commit();

        if (Target.Component)
        {
            Target.Component->NotifyMeshUpdated();
        }
    }

    TSharedPtr<FJsonObject> Result = GeometryScript_NewActorData(Target.Actor);
    Result->SetNumberField(TEXT("rotation_pitch"), Rotation.Pitch);
    Result->SetNumberField(TEXT("rotation_yaw"), Rotation.Yaw);
    Result->SetNumberField(TEXT("rotation_roll"), Rotation.Roll);
    return GeometryScript_TransformReply(Result, bApplied);
}

// MCP:scale_dynamic_mesh - Scale the live dynamic mesh of a DynamicMeshActor via UGeometryScriptLibrary_MeshTransformFunctions::ScaleMesh, as one undo step that restores the mesh. A scale that is not three numbers, or an empty mesh, is an error. A scale of 1,1,1 succeeds with applied false and a note, leaving the mesh unchanged.
//   @param actor_name - Label or name of the target DynamicMeshActor
//   @param scale - [x, y, z] scale factor (1.0 = no change)
//   @returns { actor_name: string, scale_x: number, scale_y: number, scale_z: number, applied: bool, note?: string }
TSharedPtr<FJsonObject> HandleScaleDynamicMesh(
    const TSharedPtr<FJsonObject>& Params)
{
    FGeometryScriptMeshActor Target;
    if (TSharedPtr<FJsonObject> Failure = GeometryScript_ResolveMeshActor(Params, Target, TEXT("scale")))
    {
        return Failure;
    }
    if (TSharedPtr<FJsonObject> Malformed = GeometryScript_CheckVectorParam(Params, TEXT("scale")))
    {
        return Malformed;
    }
    if (TSharedPtr<FJsonObject> Empty = GeometryScript_RequireVertices(Target))
    {
        return Empty;
    }

    const FVector Scale = mcp_common::ReadJsonVector(Params, TEXT("scale"));
    const bool bApplied = !GeometryScript_IsIdentityTransform(FTransform(FQuat::Identity, FVector::ZeroVector, Scale));
    if (bApplied)
    {
        const FScopedTransaction Transaction(LOCTEXT("ScaleDynamicMesh", "MCP: Scale Dynamic Mesh"));
        Target.Actor->Modify();
        FGeometryScriptMeshEdit MeshEdit(Target.Mesh);

        UGeometryScriptLibrary_MeshTransformFunctions::ScaleMesh(Target.Mesh, Scale, /*ScaleOrigin*/ FVector::ZeroVector, /*bFixOrientationForNegativeScale*/ false, /*Debug*/ nullptr);
        MeshEdit.Commit();

        if (Target.Component)
        {
            Target.Component->NotifyMeshUpdated();
        }
    }

    TSharedPtr<FJsonObject> Result = GeometryScript_NewActorData(Target.Actor);
    Result->SetNumberField(TEXT("scale_x"), Scale.X);
    Result->SetNumberField(TEXT("scale_y"), Scale.Y);
    Result->SetNumberField(TEXT("scale_z"), Scale.Z);
    return GeometryScript_TransformReply(Result, bApplied);
}

// ===========================================================================
// Mesh spatial queries (read-only)
// ===========================================================================

namespace
{
    // The required, non-empty "actor_name" param as a DynamicMeshActor of the editor world, with its component and
    // live mesh (all non-null on success), into Out; false, with OutError set, on failure.
    bool GeometryScript_ResolveLiveMesh(const TSharedPtr<FJsonObject>& Params, FGeometryScriptMeshActor& Out, FString& OutError)
    {
        if (!Params->TryGetStringField(TEXT("actor_name"), Out.Name) || Out.Name.IsEmpty())
        {
            OutError = TEXT("actor_name is required");
            return false;
        }
        UWorld* World = mcp_common::GetEditorWorld();
        if (!World)
        {
            OutError = TEXT("Failed to get editor world");
            return false;
        }
        Out.Actor = Cast<ADynamicMeshActor>(mcp_common::FindActorByName(World, Out.Name));
        if (!Out.Actor)
        {
            OutError = FString::Printf(TEXT("DynamicMeshActor '%s' not found in the editor world"), *Out.Name);
            return false;
        }
        Out.Component = Out.Actor->GetDynamicMeshComponent();
        Out.Mesh = Out.Component ? Out.Component->GetDynamicMesh() : nullptr;
        if (!Out.Mesh)
        {
            OutError = TEXT("DynamicMeshActor has no dynamic mesh");
            return false;
        }
        return true;
    }

    // The live mesh of GeometryScript_ResolveLiveMesh, or nullptr with OutError set. The BVH-out-parameter
    // pattern requires callers to keep the FGeometryScriptDynamicMeshBVH value alive while making queries;
    // each handler declares it locally since the queries are read-only and complete synchronously.
    UDynamicMesh* ResolveDynamicMeshForSpatial(const TSharedPtr<FJsonObject>& Params, FString& OutError)
    {
        FGeometryScriptMeshActor Target;
        return GeometryScript_ResolveLiveMesh(Params, Target, OutError) ? Target.Mesh : nullptr;
    }

    // Common spatial-query options: optional max_distance / unsafe-modified / winding threshold.
    FGeometryScriptSpatialQueryOptions ReadSpatialOptions(const TSharedPtr<FJsonObject>& Params)
    {
        FGeometryScriptSpatialQueryOptions Options;  // defaults: MaxDistance=0 (unlimited), bAllowUnsafe=false, WindingIsoThreshold=0.5
        double MaxDistance = 0.0;
        if (Params->TryGetNumberField(TEXT("max_distance"), MaxDistance))
        {
            Options.MaxDistance = static_cast<float>(MaxDistance);
        }
        bool bUnsafe = false;
        if (Params->HasField(TEXT("allow_unsafe_modified")))
        {
            Params->TryGetBoolField(TEXT("allow_unsafe_modified"), bUnsafe);
            Options.bAllowUnsafeModifiedQueries = bUnsafe;
        }
        return Options;
    }
}

// MCP:build_mesh_bvh - Build a BVH (bounding volume hierarchy) for the live dynamic mesh of a DynamicMeshActor in the editor world with BuildBVHForMesh, and read it back with IsBVHValidForMesh. The plugin keeps no BVH between calls: this one is dropped on return, and the spatial-query tools (find_nearest_point_on_mesh, find_nearest_ray_intersection_with_mesh, is_point_inside_mesh) build their own on every call, so they never need this first. It checks that a BVH can be built for the mesh as it is now: built is true when the engine reported no error and the BVH reads back as valid; the call fails otherwise. The mesh is not changed.
//   @param actor_name - Label or name of the target DynamicMeshActor.
//   @param b_only_if_invalid - Optional bool, default true; it has no effect, since no earlier BVH is kept and every call builds a new one.
//   @returns { actor_name, built: bool, b_only_if_invalid: bool, bvh_valid: bool, vertex_count, triangle_count } -- actor_name is the dynamic mesh component's name, not the actor label; b_only_if_invalid echoes the request; the counts are those of the mesh the BVH was built for.
//   @hints read-only
TSharedPtr<FJsonObject> HandleBuildMeshBVH(
    const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UDynamicMesh* DynMesh = ResolveDynamicMeshForSpatial(Params, Error);
    if (!DynMesh)
        return mcp_common::MakeErrorReply(Error);

    // A GeometryScript BVH is a value its caller holds: it points into the mesh it was built for, and nothing tracks
    // that mesh's lifetime or edits for it. No tool of the plugin reads a kept one, so this BVH is built from the
    // mesh as it is now and dropped on return; b_only_if_invalid could only skip rebuilding an earlier BVH, and
    // there is none, so it is only echoed. A refusal is reported only through Debug, and the result is read back
    // with IsBVHValidForMesh.
    bool bOnlyIfInvalid = true;
    Params->TryGetBoolField(TEXT("b_only_if_invalid"), bOnlyIfInvalid);
    FGeometryScriptDynamicMeshBVH BVH;
    UGeometryScriptDebug* Debug = GeometryScript_NewDebug();
    UGeometryScriptLibrary_MeshSpatial::BuildBVHForMesh(DynMesh, BVH, Debug);
    const FString BuildError = GeometryScript_FirstError(Debug);
    bool bIsValidAfter = false;
    UGeometryScriptLibrary_MeshSpatial::IsBVHValidForMesh(DynMesh, BVH, bIsValidAfter, /*Debug*/ nullptr);
    const bool bBuilt = BuildError.IsEmpty() && bIsValidAfter;

    TSharedPtr<FJsonObject> Data = GeometryScript_NewMeshData(DynMesh);
    Data->SetBoolField(TEXT("built"), bBuilt);
    Data->SetBoolField(TEXT("b_only_if_invalid"), bOnlyIfInvalid);
    Data->SetBoolField(TEXT("bvh_valid"), bIsValidAfter);
    Data->SetNumberField(TEXT("vertex_count"),
        UGeometryScriptLibrary_MeshQueryFunctions::GetVertexCount(DynMesh));
    Data->SetNumberField(TEXT("triangle_count"), DynMesh->GetTriangleCount());
    return GeometryScript_OutcomeReply(Data, bBuilt, TEXT("build_mesh_bvh"), TEXT("built"),
        BuildError.IsEmpty() ? FString(TEXT("the built BVH does not read back as valid for the mesh")) : BuildError);
}

// MCP:is_bvh_valid_for_mesh - Report whether a BVH built for the live dynamic mesh of a DynamicMeshActor in the editor world is valid for it, via BuildBVHForMesh then UGeometryScriptLibrary_MeshSpatial::IsBVHValidForMesh. The plugin keeps no BVH between calls (build_mesh_bvh and rebuild_mesh_bvh drop theirs, and the spatial-query tools build their own on every call), so no kept BVH can go stale after a mesh edit and that staleness is not tracked: this builds a BVH for the mesh as it is now and checks it, as each spatial query does. bvh_valid false means no valid BVH could be built for the mesh. The mesh is not changed.
//   @param actor_name - Label or name of the target DynamicMeshActor.
//   @returns { actor_name, bvh_valid: bool } -- actor_name is the dynamic mesh component's name, not the actor label.
TSharedPtr<FJsonObject> HandleIsBVHValidForMesh(
    const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UDynamicMesh* DynMesh = ResolveDynamicMeshForSpatial(Params, Error);
    if (!DynMesh)
        return mcp_common::MakeErrorReply(Error);

    // IsBVHValidForMesh is only true for a BVH built for this very mesh and unchanged since, and the plugin keeps none
    // (see build_mesh_bvh), so an empty BVH would always read back false. The answer is measured on a BVH built now.
    FGeometryScriptDynamicMeshBVH BVH;
    UGeometryScriptLibrary_MeshSpatial::BuildBVHForMesh(DynMesh, BVH, /*Debug*/ nullptr);
    bool bValid = false;
    UGeometryScriptLibrary_MeshSpatial::IsBVHValidForMesh(DynMesh, BVH, bValid, /*Debug*/ nullptr);

    TSharedPtr<FJsonObject> Data = GeometryScript_NewMeshData(DynMesh);
    Data->SetBoolField(TEXT("bvh_valid"), bValid);
    return mcp_common::MakeOkReply(Data);
}

// MCP:find_nearest_point_on_mesh - Find the nearest point on the live dynamic mesh of a DynamicMeshActor to a 3D query point via UGeometryScriptLibrary_MeshSpatial::FindNearestPointOnMesh. Every call builds a fresh BVH from the mesh as it is now (the plugin keeps none), then queries it, so the answer always reflects the latest mesh edits. found is false when no triangle lies within max_distance, or the mesh is empty. The mesh is not changed.
//   @param actor_name - Label or name of the target DynamicMeshActor.
//   @param query_point - [x, y, z] query point in the mesh's local space.
//   @param max_distance - Optional float; max search distance (0 = unlimited).
//   @param allow_unsafe_modified [bool] - Optional; maps to bAllowUnsafeModifiedQueries (default false). It has no effect: the BVH is built fresh on every call, so it always matches the mesh.
//   @returns { actor_name, found: bool, valid: bool, triangle_id, position_x/y/z, bary_coords_x/y/z } -- actor_name is the dynamic mesh component's name, not the actor label.
TSharedPtr<FJsonObject> HandleFindNearestPointOnMesh(
    const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UDynamicMesh* DynMesh = ResolveDynamicMeshForSpatial(Params, Error);
    if (!DynMesh)
        return mcp_common::MakeErrorReply(Error);
    if (!Params->HasField(TEXT("query_point")))
        return mcp_common::MakeErrorReply(TEXT("query_point is required"));

    const FVector QueryPoint = mcp_common::ReadJsonVector(Params, TEXT("query_point"));
    const FGeometryScriptSpatialQueryOptions Options = ReadSpatialOptions(Params);

    FGeometryScriptDynamicMeshBVH BVH;
    UGeometryScriptLibrary_MeshSpatial::BuildBVHForMesh(DynMesh, BVH, /*Debug*/ nullptr);

    FGeometryScriptTrianglePoint Nearest;
    EGeometryScriptSearchOutcomePins Outcome = EGeometryScriptSearchOutcomePins::NotFound;
    UGeometryScriptLibrary_MeshSpatial::FindNearestPointOnMesh(
        DynMesh, BVH, QueryPoint, Options, Nearest, Outcome, /*Debug*/ nullptr);

    TSharedPtr<FJsonObject> Data = GeometryScript_NewMeshData(DynMesh);
    Data->SetBoolField(TEXT("found"), Outcome == EGeometryScriptSearchOutcomePins::Found);
    Data->SetBoolField(TEXT("valid"), Nearest.bValid);
    Data->SetNumberField(TEXT("triangle_id"), Nearest.TriangleID);
    Data->SetNumberField(TEXT("position_x"), Nearest.Position.X);
    Data->SetNumberField(TEXT("position_y"), Nearest.Position.Y);
    Data->SetNumberField(TEXT("position_z"), Nearest.Position.Z);
    Data->SetNumberField(TEXT("bary_coords_x"), Nearest.BaryCoords.X);
    Data->SetNumberField(TEXT("bary_coords_y"), Nearest.BaryCoords.Y);
    Data->SetNumberField(TEXT("bary_coords_z"), Nearest.BaryCoords.Z);
    return mcp_common::MakeOkReply(Data);
}

// MCP:find_nearest_ray_intersection_with_mesh - Cast a ray against the live dynamic mesh of a DynamicMeshActor and return the nearest hit via UGeometryScriptLibrary_MeshSpatial::FindNearestRayIntersectionWithMesh. Every call builds a fresh BVH from the mesh as it is now (the plugin keeps none), so the answer always reflects the latest mesh edits. The engine normalizes ray_direction, so ray_parameter is the distance from ray_origin to the hit. A ray_direction that cannot be normalized (all zeros, a non-finite value, or not an array of three numbers) is refused before anything else runs. hit is false when nothing is hit within max_distance. The mesh is not changed.
//   @param actor_name - Label or name of the target DynamicMeshActor.
//   @param ray_origin - [x, y, z] origin point of the ray in mesh-local space.
//   @param ray_direction - [x, y, z] direction of the ray; the engine normalizes it, so its length does not matter, but a zero or non-finite direction is an error.
//   @param max_distance - Optional float; max search distance (0 = unlimited).
//   @param allow_unsafe_modified [bool] - Optional; maps to bAllowUnsafeModifiedQueries (default false). It has no effect: the BVH is built fresh on every call, so it always matches the mesh.
//   @returns { actor_name, hit: bool, ray_parameter, hit_triangle_id, hit_position_x/y/z, hit_bary_coords_x/y/z } -- actor_name is the dynamic mesh component's name, not the actor label; ray_parameter is a distance along the normalized direction.
TSharedPtr<FJsonObject> HandleFindNearestRayIntersectionWithMesh(
    const TSharedPtr<FJsonObject>& Params)
{
    if (!Params->HasField(TEXT("ray_origin")))
        return mcp_common::MakeErrorReply(TEXT("ray_origin is required"));
    if (!Params->HasField(TEXT("ray_direction")))
        return mcp_common::MakeErrorReply(TEXT("ray_direction is required"));

    const FVector RayOrigin = mcp_common::ReadJsonVector(Params, TEXT("ray_origin"));
    const FVector RayDirection = mcp_common::ReadJsonVector(Params, TEXT("ray_direction"));

    // The engine builds its ray with UE::Geometry::Normalized(RayDirection), which yields the zero vector for a zero
    // length and NaN for an infinite one; that ray hits nothing, so the query would answer hit:false for a request
    // that is no ray at all. The same normalization is measured here and refused before anything else runs. A
    // malformed array reads as the zero vector (ReadJsonVector), so it is refused too.
    if (!UE::Geometry::IsNormalized(UE::Geometry::Normalized(FVector3d(RayDirection))))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("ray_direction (%s) cannot be normalized: pass an [x, y, z] array of finite numbers that are not all zero"),
            *RayDirection.ToString()));
    }

    FString Error;
    UDynamicMesh* DynMesh = ResolveDynamicMeshForSpatial(Params, Error);
    if (!DynMesh)
        return mcp_common::MakeErrorReply(Error);

    const FGeometryScriptSpatialQueryOptions Options = ReadSpatialOptions(Params);

    FGeometryScriptDynamicMeshBVH BVH;
    UGeometryScriptLibrary_MeshSpatial::BuildBVHForMesh(DynMesh, BVH, /*Debug*/ nullptr);

    FGeometryScriptRayHitResult Hit;
    EGeometryScriptSearchOutcomePins Outcome = EGeometryScriptSearchOutcomePins::NotFound;
    UGeometryScriptLibrary_MeshSpatial::FindNearestRayIntersectionWithMesh(
        DynMesh, BVH, RayOrigin, RayDirection, Options, Hit, Outcome, /*Debug*/ nullptr);

    TSharedPtr<FJsonObject> Data = GeometryScript_NewMeshData(DynMesh);
    Data->SetBoolField(TEXT("hit"), Hit.bHit);
    Data->SetNumberField(TEXT("ray_parameter"), Hit.RayParameter);
    Data->SetNumberField(TEXT("hit_triangle_id"), Hit.HitTriangleID);
    Data->SetNumberField(TEXT("hit_position_x"), Hit.HitPosition.X);
    Data->SetNumberField(TEXT("hit_position_y"), Hit.HitPosition.Y);
    Data->SetNumberField(TEXT("hit_position_z"), Hit.HitPosition.Z);
    Data->SetNumberField(TEXT("hit_bary_coords_x"), Hit.HitBaryCoords.X);
    Data->SetNumberField(TEXT("hit_bary_coords_y"), Hit.HitBaryCoords.Y);
    Data->SetNumberField(TEXT("hit_bary_coords_z"), Hit.HitBaryCoords.Z);
    return mcp_common::MakeOkReply(Data);
}

// MCP:is_point_inside_mesh - Test whether a 3D point is inside the live dynamic mesh of a DynamicMeshActor using Fast Winding Number via UGeometryScriptLibrary_MeshSpatial::IsPointInsideMesh. Every call builds a fresh BVH and fast winding tree from the mesh as it is now (the plugin keeps none), so the answer always reflects the latest mesh edits. The mesh is not changed.
//   @param actor_name - Label or name of the target DynamicMeshActor.
//   @param query_point - [x, y, z] point in the mesh's local space.
//   @param winding_threshold - Optional float; default 0.5 (standard parity test). Higher values shrink the "inside" region.
//   @param max_distance - Optional float; it has no effect on this query, whose winding-number test uses no search distance.
//   @param allow_unsafe_modified [bool] - Optional; maps to bAllowUnsafeModifiedQueries (default false). It has no effect: the BVH is built fresh on every call, so it always matches the mesh.
//   @returns { actor_name, is_inside: bool, containment_outcome: string } -- actor_name is the dynamic mesh component's name, not the actor label.
TSharedPtr<FJsonObject> HandleIsPointInsideMesh(
    const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UDynamicMesh* DynMesh = ResolveDynamicMeshForSpatial(Params, Error);
    if (!DynMesh)
        return mcp_common::MakeErrorReply(Error);
    if (!Params->HasField(TEXT("query_point")))
        return mcp_common::MakeErrorReply(TEXT("query_point is required"));

    const FVector QueryPoint = mcp_common::ReadJsonVector(Params, TEXT("query_point"));
    FGeometryScriptSpatialQueryOptions Options = ReadSpatialOptions(Params);

    double WindingThreshold = 0.5;
    if (Params->TryGetNumberField(TEXT("winding_threshold"), WindingThreshold))
    {
        Options.WindingIsoThreshold = static_cast<float>(WindingThreshold);
    }

    // IsPointInsideMesh needs BOTH a BVH and a FastWindingTree. BuildBVHForMesh
    // populates both internally; no separate build needed here.
    FGeometryScriptDynamicMeshBVH BVH;
    UGeometryScriptLibrary_MeshSpatial::BuildBVHForMesh(DynMesh, BVH, /*Debug*/ nullptr);

    bool bIsInside = false;
    EGeometryScriptContainmentOutcomePins Outcome = EGeometryScriptContainmentOutcomePins::Outside;
    UGeometryScriptLibrary_MeshSpatial::IsPointInsideMesh(
        DynMesh, BVH, QueryPoint, Options, bIsInside, Outcome, /*Debug*/ nullptr);

    TSharedPtr<FJsonObject> Data = GeometryScript_NewMeshData(DynMesh);
    Data->SetBoolField(TEXT("is_inside"), bIsInside);
    Data->SetStringField(TEXT("containment_outcome"),
        Outcome == EGeometryScriptContainmentOutcomePins::Inside ? TEXT("Inside") : TEXT("Outside"));
    return mcp_common::MakeOkReply(Data);
}

// ============================================================================
// UV setter siblings of generate_mesh_uvs.
// Setter wrappers around UGeometryScriptLibrary_MeshUVFunctions: SetNumUVSets /
// TranslateMeshUVs / ScaleMeshUVs. Idempotent by value (no-op when the operation
// matches the current state). All operate on a UDynamicMesh transient (no
// bIsValidUVSet bool& out-param for the UV-set index API; we pre-read NumUVSets
// and reject OOR upfront).
// ============================================================================

// MCP:set_num_uv_channels - Set how many UV channels the live dynamic mesh of a DynamicMeshActor in the editor world has: added channels start without UVs, and channels from num_uv_channels upward are discarded with their UVs (note says so). The current count is a no-op (changed false). The engine allows at most 8, so a count above 8 is an error; num_uv_channels is the count read back after the call, and the call fails, leaving the mesh as it was, if it differs from the request. One undo step restores the previous channels with their UVs, nothing saved; reply actor_name is the component name, not the actor label. Fill a channel with copy_mesh_uv_channel or generate_mesh_uvs; clear_uv_channel works on static mesh assets.
//   @param actor_name [string!] - DynamicMeshActor name (required)
//   @param num_uv_channels [number!] - Desired count, 0 to 8 (required)
//   @returns { actor_name: string, num_uv_channels: int, previous_num_uv_channels: int, changed: bool, note?: string } -- actor_name is the dynamic mesh component's name, not the actor label; num_uv_channels is read back; note when channels were removed, whose UV data is dropped.
TSharedPtr<FJsonObject> HandleSetNumUVChannels(
    const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UDynamicMesh* DynMesh = ResolveDynamicMeshForSpatial(Params, Error);
    if (!DynMesh)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    double NumChannelsD = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("num_uv_channels"), NumChannelsD)) { return Missing; }
    // SetNumUVSets refuses a count above the maximum and leaves the mesh as it was, so it is refused here first.
    if (NumChannelsD < 0.0 || NumChannelsD > GeometryScript_MaxUVChannels)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("num_uv_channels must be between 0 and %d (got %g): a dynamic mesh has at most %d UV channels"),
            GeometryScript_MaxUVChannels, NumChannelsD, GeometryScript_MaxUVChannels));
    }
    const int32 NumChannels = static_cast<int32>(NumChannelsD);

    // Read current count BEFORE mutating. GetNumUVSets takes a single
    // UDynamicMesh* arg (no bIsValidUVSet out-param -- unlike GetMeshUVArea).
    const int32 Previous = UGeometryScriptLibrary_MeshQueryFunctions::GetNumUVSets(DynMesh);

    if (Previous == NumChannels)
    {
        TSharedPtr<FJsonObject> ResultObj = GeometryScript_NewMeshData(DynMesh);
        ResultObj->SetNumberField(TEXT("num_uv_channels"), NumChannels);
        ResultObj->SetNumberField(TEXT("previous_num_uv_channels"), Previous);
        ResultObj->SetBoolField(TEXT("changed"), false);
        return mcp_common::MakeOkReply(ResultObj);
    }

    const FScopedTransaction Transaction(LOCTEXT("SetNumUVChannels", "MCP: Set Num UV Channels"));
    FGeometryScriptMeshEdit MeshEdit(DynMesh);
    UGeometryScriptLibrary_MeshUVFunctions::SetNumUVSets(DynMesh, NumChannels, /*Debug*/ nullptr);
    if (UGeometryScriptLibrary_MeshQueryFunctions::GetNumUVSets(DynMesh) == NumChannels)
    {
        MeshEdit.Commit();
    }
    else
    {
        MeshEdit.Revert();
    }

    // The count the mesh has now, read back: the reply reports it, not the request.
    const int32 Current = UGeometryScriptLibrary_MeshQueryFunctions::GetNumUVSets(DynMesh);

    TSharedPtr<FJsonObject> ResultObj = GeometryScript_NewMeshData(DynMesh);
    ResultObj->SetNumberField(TEXT("num_uv_channels"), Current);
    ResultObj->SetNumberField(TEXT("previous_num_uv_channels"), Previous);
    ResultObj->SetBoolField(TEXT("changed"), Current != Previous);
    if (Current < Previous)
    {
        ResultObj->SetStringField(TEXT("note"),
            TEXT("Reduced UV channel count; existing UV data on discarded channels is dropped (UE API behavior)."));
    }
    return mcp_common::MakeOutcomeReply(ResultObj, Current == NumChannels, Current == NumChannels ? FString()
        : FString::Printf(TEXT("set_num_uv_channels: the mesh has %d UV channels after the call, not the %d requested"), Current, NumChannels));
}

// MCP:translate_mesh_uvs - Offset all UVs of one channel on a DynamicMeshActor's live dynamic mesh in the editor world by (translate_u, translate_v); a zero offset changes nothing (translated false) and adds no undo step. One undo step restores the UVs; reply actor_name is the component name, not the actor label.
//   @param actor_name [string!] - Label or object name (case-insensitive) of the target DynamicMeshActor in the editor world.
//   @param uv_channel [number!] - 0-based UV channel index; a channel the mesh does not have fails the call.
//   @param translate_u [number!] - Offset added to every U coordinate (1.0 shifts by one full tile).
//   @param translate_v [number!] - Offset added to every V coordinate (1.0 shifts by one full tile).
//   @returns { actor_name: string, uv_channel: int, translate_u: number, translate_v: number, translated: bool }
TSharedPtr<FJsonObject> HandleTranslateMeshUVs(
    const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UDynamicMesh* DynMesh = ResolveDynamicMeshForSpatial(Params, Error);
    if (!DynMesh)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    int32 UVChannel = 0;
    if (TSharedPtr<FJsonObject> Failure = GeometryScript_ReadUVChannel(Params, UVChannel))
    {
        return Failure;
    }
    double TU = 0.0, TV = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("translate_u"), TU)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("translate_v"), TV)) { return Missing; }

    // Validate the channel exists (engine returns silently otherwise).
    if (TSharedPtr<FJsonObject> Failure = GeometryScript_CheckUVChannelExists(DynMesh, UVChannel))
    {
        return Failure;
    }

    const bool Translated = (TU != 0.0) || (TV != 0.0);
    if (Translated)
    {
        const FScopedTransaction Transaction(LOCTEXT("TranslateMeshUVs", "MCP: Translate Mesh UVs"));
        FGeometryScriptMeshEdit MeshEdit(DynMesh);
        UGeometryScriptLibrary_MeshUVFunctions::TranslateMeshUVs(
            DynMesh, UVChannel, FVector2D(TU, TV),
            FGeometryScriptMeshSelection(),
            /*Debug*/ nullptr);
        MeshEdit.Commit();
    }

    TSharedPtr<FJsonObject> ResultObj = GeometryScript_NewMeshData(DynMesh);
    ResultObj->SetNumberField(TEXT("uv_channel"), UVChannel);
    ResultObj->SetNumberField(TEXT("translate_u"), TU);
    ResultObj->SetNumberField(TEXT("translate_v"), TV);
    ResultObj->SetBoolField(TEXT("translated"), Translated);
    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:scale_mesh_uvs - Scale all UVs of one channel on a DynamicMeshActor's live dynamic mesh in the editor world by separate U and V factors; zero factors are refused and 1, 1 changes nothing (scaled false) and adds no undo step. One undo step restores the UVs; reply actor_name is the component name, not the actor label.
//   @param actor_name [string!] - Label or object name (case-insensitive) of the target DynamicMeshActor in the editor world.
//   @param uv_channel [number!] - 0-based UV channel index; a channel the mesh does not have fails the call.
//   @param scale_u [number!] - Scale factor along U; must be non-zero (1 keeps U unchanged).
//   @param scale_v [number!] - Scale factor along V; must be non-zero (1 keeps V unchanged).
//   @param scale_origin_u [number] - U coordinate of the point the scaling is centred on (default 0).
//   @param scale_origin_v [number] - V coordinate of the point the scaling is centred on (default 0).
//   @returns { actor_name: string, uv_channel: int, scale_u: number, scale_v: number, scale_origin: { u, v }, scaled: bool }
TSharedPtr<FJsonObject> HandleScaleMeshUVs(
    const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UDynamicMesh* DynMesh = ResolveDynamicMeshForSpatial(Params, Error);
    if (!DynMesh)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    int32 UVChannel = 0;
    if (TSharedPtr<FJsonObject> Failure = GeometryScript_ReadUVChannel(Params, UVChannel))
    {
        return Failure;
    }
    double SU = 1.0, SV = 1.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("scale_u"), SU)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("scale_v"), SV)) { return Missing; }
    if (SU == 0.0 || SV == 0.0)
    {
        return mcp_common::MakeErrorReply(
            TEXT("scale_u and scale_v must be non-zero"));
    }

    double SOU = 0.0, SOV = 0.0;
    Params->TryGetNumberField(TEXT("scale_origin_u"), SOU);
    Params->TryGetNumberField(TEXT("scale_origin_v"), SOV);

    // Validate the channel exists.
    if (TSharedPtr<FJsonObject> Failure = GeometryScript_CheckUVChannelExists(DynMesh, UVChannel))
    {
        return Failure;
    }

    const bool Scaled = (SU != 1.0) || (SV != 1.0);
    if (Scaled)
    {
        const FScopedTransaction Transaction(LOCTEXT("ScaleMeshUVs", "MCP: Scale Mesh UVs"));
        FGeometryScriptMeshEdit MeshEdit(DynMesh);
        UGeometryScriptLibrary_MeshUVFunctions::ScaleMeshUVs(
            DynMesh, UVChannel, FVector2D(SU, SV), FVector2D(SOU, SOV),
            FGeometryScriptMeshSelection(),
            /*Debug*/ nullptr);
        MeshEdit.Commit();
    }

    TSharedPtr<FJsonObject> ResultObj = GeometryScript_NewMeshData(DynMesh);
    ResultObj->SetNumberField(TEXT("uv_channel"), UVChannel);
    ResultObj->SetNumberField(TEXT("scale_u"), SU);
    ResultObj->SetNumberField(TEXT("scale_v"), SV);
    TSharedPtr<FJsonObject> OriginObj = MakeShared<FJsonObject>();
    OriginObj->SetNumberField(TEXT("u"), SOU);
    OriginObj->SetNumberField(TEXT("v"), SOV);
    ResultObj->SetObjectField(TEXT("scale_origin"), OriginObj);
    ResultObj->SetBoolField(TEXT("scaled"), Scaled);
    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:copy_mesh_uv_channel - Copy UV data from one UV channel to another on the same mesh. A dest_channel the mesh does not have yet is added first (the channel count only grows, and channels added in between have no UVs set); a source_channel the mesh does not have, or a dest_channel of 8 or more, is an error checked before anything changes. copied is false, and the call fails, when the engine reports an error; the same channel for both is a no-op that succeeds with copied false and a note. One undo step restores the channels, a failed copy leaves the mesh as it was, and nothing is saved.
//   @param actor_name [string!] - DynamicMeshActor name (required)
//   @param source_channel [number!] - Source UV channel index (required)
//   @param dest_channel [number!] - Destination UV channel index, 0 to 7 (required, added to the mesh when it does not have it yet)
//   @returns { actor_name: string, source_channel: int, dest_channel: int, copied: bool, num_uv_channels_after: int, note?: string } -- actor_name is the dynamic mesh component's name, not the actor label; copied is false when source_channel equals dest_channel, and note says so or that the mesh grew new channels to reach dest_channel.
TSharedPtr<FJsonObject> HandleCopyMeshUVChannel(
    const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UDynamicMesh* DynMesh = ResolveDynamicMeshForSpatial(Params, Error);
    if (!DynMesh)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    double SourceD = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("source_channel"), SourceD)) { return Missing; }
    const int32 SourceChannel = static_cast<int32>(SourceD);
    if (SourceChannel < 0)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("source_channel must be >= 0 (got %d)"), SourceChannel));
    }
    // Read as a double so an out-of-range number is refused instead of wrapping into a valid channel.
    double DestD = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("dest_channel"), DestD)) { return Missing; }
    if (DestD < 0.0 || DestD >= GeometryScript_MaxUVChannels)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("dest_channel must be between 0 and %d (got %g): a dynamic mesh has at most %d UV channels"),
            GeometryScript_MaxUVChannels - 1, DestD, GeometryScript_MaxUVChannels));
    }
    const int32 DestChannel = static_cast<int32>(DestD);

    // Validate source exists.
    const int32 NumChannelsBefore = UGeometryScriptLibrary_MeshQueryFunctions::GetNumUVSets(DynMesh);
    if (SourceChannel >= NumChannelsBefore)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("source_channel %d out of range (mesh has %d UV channels)"),
                SourceChannel, NumChannelsBefore));
    }

    TSharedPtr<FJsonObject> ResultObj = GeometryScript_NewMeshData(DynMesh);
    ResultObj->SetNumberField(TEXT("source_channel"), SourceChannel);
    ResultObj->SetNumberField(TEXT("dest_channel"), DestChannel);

    // Copying a channel onto itself changes nothing (CopyUVSet refuses it), so the mesh is left alone.
    if (SourceChannel == DestChannel)
    {
        ResultObj->SetBoolField(TEXT("copied"), false);
        ResultObj->SetNumberField(TEXT("num_uv_channels_after"), NumChannelsBefore);
        ResultObj->SetStringField(TEXT("note"),
            TEXT("source_channel == dest_channel; copy is identity and the mesh is unchanged. "
                 "Use distinct channels to actually duplicate UV data."));
        return mcp_common::MakeOkReply(ResultObj);
    }

    // CopyUVSet copies only into a channel the mesh already has: for a missing one it reports an error through
    // Debug and copies nothing. So a missing destination is added first; the count only ever grows.
    const FScopedTransaction Transaction(LOCTEXT("CopyMeshUVChannel", "MCP: Copy Mesh UV Channel"));
    FGeometryScriptMeshEdit MeshEdit(DynMesh);
    if (DestChannel >= NumChannelsBefore)
    {
        UGeometryScriptLibrary_MeshUVFunctions::SetNumUVSets(DynMesh, DestChannel + 1, /*Debug*/ nullptr);
    }

    UGeometryScriptDebug* Debug = GeometryScript_NewDebug();
    UGeometryScriptLibrary_MeshUVFunctions::CopyUVSet(DynMesh, SourceChannel, DestChannel, Debug);
    const FString CopyError = GeometryScript_FirstError(Debug);
    const bool bCopied = CopyError.IsEmpty();
    if (bCopied)
    {
        MeshEdit.Commit();
    }
    else
    {
        MeshEdit.Revert();
    }

    const int32 NumChannelsAfter = UGeometryScriptLibrary_MeshQueryFunctions::GetNumUVSets(DynMesh);

    ResultObj->SetBoolField(TEXT("copied"), bCopied);
    ResultObj->SetNumberField(TEXT("num_uv_channels_after"), NumChannelsAfter);
    if (NumChannelsAfter > NumChannelsBefore)
    {
        ResultObj->SetStringField(TEXT("note"),
            FString::Printf(TEXT("dest_channel was >= NumUVSets; the mesh grew UV channels %d -> %d before the copy."),
                NumChannelsBefore, NumChannelsAfter));
    }
    return GeometryScript_OutcomeReply(ResultObj, bCopied, TEXT("copy_mesh_uv_channel"), TEXT("copied"), CopyError);
}

// ===========================================================================
// Typed actor remove + material set + clear mesh
// ===========================================================================

// MCP:remove_dynamic_mesh_actor - Destroy a DynamicMeshActor in the editor world, found by label or object name, inside an undoable transaction. Returns the label, internal name and triangle_count_before of the destroyed actor; fails if the name matches no DynamicMeshActor. Create one with spawn_dynamic_mesh_actor.
//   @param actor_name [string!] - Label or object name (case-insensitive) of the DynamicMeshActor to destroy.
//   @returns { actor_name: string, actor_internal_name: string, destroyed: bool, triangle_count_before: int, note?: string }
TSharedPtr<FJsonObject> HandleRemoveDynamicMeshActor(const TSharedPtr<FJsonObject>& Params)
{
    FString TargetLabel;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), TargetLabel)) { return Missing; }

    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World, TEXT("Failed to get editor world"))) { return NoWorld; }
    TSharedPtr<FJsonObject> Failure;
    ADynamicMeshActor* MeshActor = UEAgentGeometryScriptCommandsImpl::FindDynamicMeshActor(World, TargetLabel, Failure);
    if (MeshActor == nullptr)
    {
        return Failure;
    }

    // Everything reported is captured before the actor goes away.
    UDynamicMeshComponent* MeshComp = MeshActor->GetDynamicMeshComponent();
    UDynamicMesh* DynMesh = MeshComp != nullptr ? MeshComp->GetDynamicMesh() : nullptr;
    const int32 TrianglesBefore = DynMesh != nullptr ? DynMesh->GetTriangleCount() : 0;
    const FString InternalName = MeshActor->GetName();
    const FString LabelName = MeshActor->GetActorLabel();

    const FScopedTransaction Transaction(LOCTEXT("RemoveDynamicMeshActor", "MCP: Remove Dynamic Mesh Actor"));
    MeshActor->Modify();
    if (!World->DestroyActor(MeshActor))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("World->DestroyActor returned false for DynamicMeshActor '%s'"), *TargetLabel));
    }

    TSharedPtr<FJsonObject> Removal = MakeShared<FJsonObject>();
    Removal->SetStringField(TEXT("actor_name"), LabelName);
    Removal->SetStringField(TEXT("actor_internal_name"), InternalName);
    Removal->SetBoolField(TEXT("destroyed"), true);
    Removal->SetNumberField(TEXT("triangle_count_before"), TrianglesBefore);
    Removal->SetStringField(TEXT("note"), TEXT("DynamicMeshActor removed from editor world; underlying UDynamicMesh was GC-eligible."));
    return mcp_common::MakeOkReply(Removal);
}

// MCP:set_dynamic_mesh_material - Assign a UMaterialInterface asset to a slot on a DynamicMeshActor's UDynamicMeshComponent, in an editor transaction. applied is the slot read back after the assignment; the call fails when it does not hold the material.
//   @param actor_name [string!] - Label or name of the target DynamicMeshActor (required).
//   @param material_path [string!] - Asset path of the UMaterialInterface (e.g. /Engine/EngineMaterials/DefaultMaterial) (required).
//   @param slot_index [number] - Material slot to assign. Default 0 (the dynamic mesh only uses slot 0).
//   @returns { actor_name: string, material_path: string, material_name: string, slot_index: int, applied: bool, note?: string }
TSharedPtr<FJsonObject> HandleSetDynamicMeshMaterial(const TSharedPtr<FJsonObject>& Params)
{
    FString TargetLabel;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), TargetLabel)) { return Missing; }
    FString MaterialAssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_path"), MaterialAssetPath)) { return Missing; }

    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World, TEXT("Failed to get editor world"))) { return NoWorld; }
    TSharedPtr<FJsonObject> Failure;
    ADynamicMeshActor* MeshActor = UEAgentGeometryScriptCommandsImpl::FindDynamicMeshActor(World, TargetLabel, Failure);
    if (MeshActor == nullptr)
    {
        return Failure;
    }
    UDynamicMeshComponent* MeshComp = MeshActor->GetDynamicMeshComponent();
    if (MeshComp == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("DynamicMeshActor has no DynamicMeshComponent"));
    }

    UMaterialInterface* SlotMaterial = UEAgentGeometryScriptCommandsImpl::LoadSlotMaterial(MaterialAssetPath);
    if (SlotMaterial == nullptr)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Could not load UMaterialInterface at '%s'"), *MaterialAssetPath));
    }

    // slot_index is optional (slot 0) and must name an existing slot; a component that reports no
    // slots at all still accepts slot 0.
    double RequestedSlot = 0.0;
    int32 SlotIndex = 0;
    if (Params->TryGetNumberField(TEXT("slot_index"), RequestedSlot))
    {
        SlotIndex = static_cast<int32>(RequestedSlot);
    }
    const int32 SlotLimit = FMath::Max(MeshComp->GetNumMaterials(), 1);
    if (SlotIndex < 0 || SlotIndex >= SlotLimit)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("slot_index %d out of range [0, %d) for DynamicMeshActor '%s'"),
                SlotIndex, SlotLimit, *TargetLabel));
    }

    const FScopedTransaction Transaction(LOCTEXT("SetDynamicMeshMaterial", "MCP: Set Dynamic Mesh Material"));
    MeshComp->Modify();
    MeshComp->SetMaterial(SlotIndex, SlotMaterial);
    MeshComp->MarkRenderStateDirty();

    // Read the slot back: applied says the component now holds this material there.
    const bool bApplied = MeshComp->GetMaterial(SlotIndex) == SlotMaterial;

    TSharedPtr<FJsonObject> Assignment = GeometryScript_NewActorData(MeshActor);
    Assignment->SetStringField(TEXT("material_path"), MaterialAssetPath);
    Assignment->SetStringField(TEXT("material_name"), SlotMaterial->GetName());
    Assignment->SetNumberField(TEXT("slot_index"), SlotIndex);
    Assignment->SetBoolField(TEXT("applied"), bApplied);
    if (bApplied)
    {
        Assignment->SetStringField(TEXT("note"), TEXT("Material applied to slot; render state marked dirty for next viewport refresh."));
    }
    return GeometryScript_OutcomeReply(Assignment, bApplied, TEXT("set_dynamic_mesh_material"), TEXT("applied"),
        TEXT("the slot does not hold the material after SetMaterial"));
}

// MCP:clear_dynamic_mesh - Empty a DynamicMeshActor's live UDynamicMesh in place (keep the actor, drop the geometry). One undo step restores the geometry; an already empty mesh adds no undo step.
//   @param actor_name [string!] - Label or name of the DynamicMeshActor to clear (required).
//   @returns { actor_name: string, triangle_count_before: int, triangle_count_after: int, vertex_count_before: int, vertex_count_after: int, cleared: bool }
TSharedPtr<FJsonObject> HandleClearDynamicMesh(const TSharedPtr<FJsonObject>& Params)
{
    FString TargetLabel;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), TargetLabel)) { return Missing; }

    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World, TEXT("Failed to get editor world"))) { return NoWorld; }
    FGeometryScriptMeshActor Target;
    TSharedPtr<FJsonObject> Failure = GeometryScript_FindMeshActor(World, TargetLabel, Target);
    if (Failure.IsValid())
    {
        return Failure;
    }

    const int32 TrianglesBefore = Target.Mesh->GetTriangleCount();
    const int32 VerticesBefore = UEAgentGeometryScriptCommandsImpl::VertexCountOf(Target.Mesh);

    // Empty the mesh in place (the actor and its component stay) and tell the component to rebuild.
    const FScopedTransaction Transaction(LOCTEXT("ClearDynamicMesh", "MCP: Clear Dynamic Mesh"));
    Target.Component->Modify();
    FGeometryScriptMeshEdit MeshEdit(Target.Mesh);
    Target.Mesh->Reset();
    MeshEdit.Commit();
    Target.Component->NotifyMeshUpdated();

    const int32 TrianglesAfter = Target.Mesh->GetTriangleCount();
    const int32 VerticesAfter = UEAgentGeometryScriptCommandsImpl::VertexCountOf(Target.Mesh);

    TSharedPtr<FJsonObject> Cleared = GeometryScript_NewActorData(Target.Actor);
    Cleared->SetNumberField(TEXT("triangle_count_before"), TrianglesBefore);
    Cleared->SetNumberField(TEXT("triangle_count_after"), TrianglesAfter);
    Cleared->SetNumberField(TEXT("vertex_count_before"), VerticesBefore);
    Cleared->SetNumberField(TEXT("vertex_count_after"), VerticesAfter);
    Cleared->SetBoolField(TEXT("cleared"), TrianglesAfter == 0 && VerticesAfter == 0);
    return mcp_common::MakeOkReply(Cleared);
}

// ---------------------------------------------------------------------------
// UV / query / spatial commands: reads, or single-property mutations, on the live UDynamicMesh*
// of an ADynamicMeshActor (same resolution path as the UV setters and spatial queries).
// ---------------------------------------------------------------------------

// MCP:rotate_mesh_uvs - Rotate all UVs of one channel on a DynamicMeshActor's live dynamic mesh in the editor world by an angle in degrees around a UV-space pivot; 0 degrees changes nothing (rotated false) and adds no undo step. One undo step restores the UVs; reply actor_name is the component name, not the actor label.
//   @param actor_name [string!] - Label or object name (case-insensitive) of the target DynamicMeshActor in the editor world.
//   @param uv_channel [number!] - 0-based UV channel index; a channel the mesh does not have fails the call.
//   @param rotation_angle_deg [number!] - Rotation angle in degrees; 0 leaves the UVs untouched.
//   @param rotation_origin_u [number] - U coordinate of the rotation pivot (default 0).
//   @param rotation_origin_v [number] - V coordinate of the rotation pivot (default 0).
//   @returns { actor_name, uv_channel, rotation_angle_deg, rotation_origin: {u, v}, rotated: bool }
TSharedPtr<FJsonObject> HandleRotateMeshUVs(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UDynamicMesh* DynMesh = ResolveDynamicMeshForSpatial(Params, Error);
    if (!DynMesh) { return mcp_common::MakeErrorReply(Error); }

    int32 UVChannel = 0;
    if (TSharedPtr<FJsonObject> Failure = GeometryScript_ReadUVChannel(Params, UVChannel))
    {
        return Failure;
    }
    double AngleDeg = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("rotation_angle_deg"), AngleDeg)) { return Missing; }
    double OriginU = 0.0, OriginV = 0.0;
    Params->TryGetNumberField(TEXT("rotation_origin_u"), OriginU);
    Params->TryGetNumberField(TEXT("rotation_origin_v"), OriginV);

    if (TSharedPtr<FJsonObject> Failure = GeometryScript_CheckUVChannelExists(DynMesh, UVChannel))
    {
        return Failure;
    }

    const bool Rotated = AngleDeg != 0.0;
    if (Rotated)
    {
        const FScopedTransaction Transaction(LOCTEXT("RotateMeshUVs", "MCP: Rotate Mesh UVs"));
        FGeometryScriptMeshEdit MeshEdit(DynMesh);
        UGeometryScriptLibrary_MeshUVFunctions::RotateMeshUVs(
            DynMesh, UVChannel, static_cast<float>(AngleDeg),
            FVector2D(OriginU, OriginV),
            FGeometryScriptMeshSelection(),
            /*Debug*/ nullptr);
        MeshEdit.Commit();
    }

    TSharedPtr<FJsonObject> ResultObj = GeometryScript_NewMeshData(DynMesh);
    ResultObj->SetNumberField(TEXT("uv_channel"), UVChannel);
    ResultObj->SetNumberField(TEXT("rotation_angle_deg"), AngleDeg);
    TSharedPtr<FJsonObject> OriginObj = MakeShared<FJsonObject>();
    OriginObj->SetNumberField(TEXT("u"), OriginU);
    OriginObj->SetNumberField(TEXT("v"), OriginV);
    ResultObj->SetObjectField(TEXT("rotation_origin"), OriginObj);
    ResultObj->SetBoolField(TEXT("rotated"), Rotated);
    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:set_mesh_uv_element_position - Set the (u, v) of one element of a UV channel overlay on a DynamicMeshActor's live dynamic mesh in the editor world; an invalid element ID writes nothing, adds no undo step and returns is_valid_element false, not an error. One undo step restores the element; reply actor_name is the component name, not the actor label.
//   @param actor_name [string!] - Label or object name (case-insensitive) of the target DynamicMeshActor in the editor world.
//   @param uv_channel [number!] - 0-based UV channel index; a channel the mesh does not have fails the call.
//   @param element_id [number!] - ID of the element in the UV overlay of that channel, not a vertex, edge or triangle ID.
//   @param u [number!] - New U coordinate of the element.
//   @param v [number!] - New V coordinate of the element.
//   @returns { actor_name, uv_channel, element_id, new_position: {u, v}, is_valid_element, set: bool }
TSharedPtr<FJsonObject> HandleSetMeshUVElementPosition(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UDynamicMesh* DynMesh = ResolveDynamicMeshForSpatial(Params, Error);
    if (!DynMesh) { return mcp_common::MakeErrorReply(Error); }

    int32 UVChannel = 0;
    if (TSharedPtr<FJsonObject> Failure = GeometryScript_ReadUVChannel(Params, UVChannel))
    {
        return Failure;
    }
    double ElementIdD = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("element_id"), ElementIdD)) { return Missing; }
    const int32 ElementId = static_cast<int32>(ElementIdD);
    double U = 0.0, V = 0.0;
    if (!Params->TryGetNumberField(TEXT("u"), U) || !Params->TryGetNumberField(TEXT("v"), V))
    {
        return mcp_common::MakeErrorReply(
            TEXT("Missing required parameter: u and v (both required)"));
    }

    if (TSharedPtr<FJsonObject> Failure = GeometryScript_CheckUVChannelExists(DynMesh, UVChannel))
    {
        return Failure;
    }

    const FScopedTransaction Transaction(LOCTEXT("SetMeshUVElementPosition", "MCP: Set Mesh UV Element Position"));
    FGeometryScriptMeshEdit MeshEdit(DynMesh);
    bool bIsValidElement = false;
    UGeometryScriptLibrary_MeshUVFunctions::SetMeshUVElementPosition(
        DynMesh, UVChannel, ElementId, FVector2D(U, V), bIsValidElement, /*bDeferChangeNotifications*/ false);
    MeshEdit.Commit();

    TSharedPtr<FJsonObject> ResultObj = GeometryScript_NewMeshData(DynMesh);
    ResultObj->SetNumberField(TEXT("uv_channel"), UVChannel);
    ResultObj->SetNumberField(TEXT("element_id"), ElementId);
    TSharedPtr<FJsonObject> PosObj = MakeShared<FJsonObject>();
    PosObj->SetNumberField(TEXT("u"), U);
    PosObj->SetNumberField(TEXT("v"), V);
    ResultObj->SetObjectField(TEXT("new_position"), PosObj);
    ResultObj->SetBoolField(TEXT("is_valid_element"), bIsValidElement);
    ResultObj->SetBoolField(TEXT("set"), bIsValidElement);
    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:rebuild_mesh_bvh - Build a BVH for the live dynamic mesh of a DynamicMeshActor with RebuildBVHForMesh and read it back with IsBVHValidForMesh. The BVH is a temporary that is not kept: the spatial-query tools build their own on every call, so this only checks that a BVH can be built for the mesh as it is now. bvh_rebuilt is true when the engine reported no error and the BVH reads back as valid; the call fails otherwise. The mesh is not changed.
//   @param actor_name [string!] - DynamicMeshActor name (required)
//   @param b_only_if_invalid [bool] - Optional, default true; it has no effect, since every call starts from an empty temporary BVH that the engine always rebuilds
//   @returns { actor_name, bvh_rebuilt: bool, b_only_if_invalid, bvh_valid } -- actor_name is the dynamic mesh component's name, not the actor label.
//   @hints read-only
TSharedPtr<FJsonObject> HandleRebuildMeshBVH(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UDynamicMesh* DynMesh = ResolveDynamicMeshForSpatial(Params, Error);
    if (!DynMesh) { return mcp_common::MakeErrorReply(Error); }

    bool bOnlyIfInvalid = true;
    if (Params->HasField(TEXT("b_only_if_invalid")))
    {
        Params->TryGetBoolField(TEXT("b_only_if_invalid"), bOnlyIfInvalid);
    }

    // RebuildBVHForMesh re-fills the BVH it is given. This one is a local that starts empty, so the engine always
    // rebuilds it (bOnlyIfInvalid only skips a BVH already built for this mesh) and it is dropped on return. A
    // refusal is reported only through Debug, and the result is read back with IsBVHValidForMesh.
    FGeometryScriptDynamicMeshBVH BVH;
    UGeometryScriptDebug* Debug = GeometryScript_NewDebug();
    UGeometryScriptLibrary_MeshSpatial::RebuildBVHForMesh(DynMesh, BVH, bOnlyIfInvalid, Debug);
    const FString RebuildError = GeometryScript_FirstError(Debug);
    bool bIsValidAfter = false;
    UGeometryScriptLibrary_MeshSpatial::IsBVHValidForMesh(DynMesh, BVH, bIsValidAfter, /*Debug*/ nullptr);
    const bool bRebuilt = RebuildError.IsEmpty() && bIsValidAfter;

    TSharedPtr<FJsonObject> ResultObj = GeometryScript_NewMeshData(DynMesh);
    ResultObj->SetBoolField(TEXT("bvh_rebuilt"), bRebuilt);
    ResultObj->SetBoolField(TEXT("b_only_if_invalid"), bOnlyIfInvalid);
    ResultObj->SetBoolField(TEXT("bvh_valid"), bIsValidAfter);
    return GeometryScript_OutcomeReply(ResultObj, bRebuilt, TEXT("rebuild_mesh_bvh"), TEXT("bvh_rebuilt"),
        RebuildError.IsEmpty() ? FString(TEXT("the rebuilt BVH does not read back as valid for the mesh")) : RebuildError);
}

// MCP:get_num_uv_islands - Get the number of UV islands in a UV channel.
//   @param actor_name [string!] - DynamicMeshActor name (required)
//   @param uv_channel [number!] - UV channel index (required)
//   @returns { actor_name, uv_channel, num_uv_islands, is_valid_uv_channel }
TSharedPtr<FJsonObject> HandleGetNumUVIslands(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UDynamicMesh* DynMesh = ResolveDynamicMeshForSpatial(Params, Error);
    if (!DynMesh) { return mcp_common::MakeErrorReply(Error); }

    int32 UVChannel = 0;
    if (TSharedPtr<FJsonObject> Failure = GeometryScript_ReadUVChannel(Params, UVChannel))
    {
        return Failure;
    }

    bool bIsValidChannel = false;
    const int32 NumIslands = UGeometryScriptLibrary_MeshQueryFunctions::GetNumUVIslands(
        DynMesh, UVChannel, bIsValidChannel);

    TSharedPtr<FJsonObject> ResultObj = GeometryScript_NewMeshData(DynMesh);
    ResultObj->SetNumberField(TEXT("uv_channel"), UVChannel);
    ResultObj->SetNumberField(TEXT("num_uv_islands"), NumIslands);
    ResultObj->SetBoolField(TEXT("is_valid_uv_channel"), bIsValidChannel);
    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:get_num_connected_islands - Get the number of separate connected components in the mesh (alias of GetNumConnectedComponents).
//   @param actor_name [string!] - DynamicMeshActor name (required)
//   @returns { actor_name, num_connected_islands }
TSharedPtr<FJsonObject> HandleGetNumConnectedIslands(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UDynamicMesh* DynMesh = ResolveDynamicMeshForSpatial(Params, Error);
    if (!DynMesh) { return mcp_common::MakeErrorReply(Error); }

    const int32 NumIslands = UGeometryScriptLibrary_MeshQueryFunctions::GetNumConnectedComponents(DynMesh);

    TSharedPtr<FJsonObject> ResultObj = GeometryScript_NewMeshData(DynMesh);
    ResultObj->SetNumberField(TEXT("num_connected_islands"), NumIslands);
    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:get_is_dense_mesh - Return true if the mesh's triangle IDs are dense (no gaps after deletions).
//   @param actor_name [string!] - DynamicMeshActor name (required)
//   @returns { actor_name, is_dense }
TSharedPtr<FJsonObject> HandleGetIsDenseMesh(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UDynamicMesh* DynMesh = ResolveDynamicMeshForSpatial(Params, Error);
    if (!DynMesh) { return mcp_common::MakeErrorReply(Error); }

    const bool bIsDense = UGeometryScriptLibrary_MeshQueryFunctions::GetIsDenseMesh(DynMesh);

    TSharedPtr<FJsonObject> ResultObj = GeometryScript_NewMeshData(DynMesh);
    ResultObj->SetBoolField(TEXT("is_dense"), bIsDense);
    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:get_mesh_has_attribute_set - Return true if the mesh has at least one attribute set registered.
//   @param actor_name [string!] - DynamicMeshActor name (required)
//   @returns { actor_name, has_attribute_set }
TSharedPtr<FJsonObject> HandleGetMeshHasAttributeSet(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UDynamicMesh* DynMesh = ResolveDynamicMeshForSpatial(Params, Error);
    if (!DynMesh) { return mcp_common::MakeErrorReply(Error); }

    const bool bHas = UGeometryScriptLibrary_MeshQueryFunctions::GetMeshHasAttributeSet(DynMesh);

    TSharedPtr<FJsonObject> ResultObj = GeometryScript_NewMeshData(DynMesh);
    ResultObj->SetBoolField(TEXT("has_attribute_set"), bHas);
    return mcp_common::MakeOkReply(ResultObj);
}

// ============================================================================
// UV / Cut / Query commands not covered by the UV setters, basic edits,
//  spatial queries and the block above:
//   * RecomputeMeshUVs          (per-channel UV recovery using islands/polygroups/selection)
//   * RepackMeshUVs             (per-channel UV pack into standard space)
//   * DeleteTriangleFromMesh    (single-triangle delete by ID; complement of apply_mesh_boolean)
//   * DeleteSelectedTrianglesFromMesh (selection-driven delete; uses FGeometryScriptMeshSelection)
//   * GetNumOpenBorderEdges     (open boundary EDGES count, complement of get_mesh_open_border_loops)
//   * GetNumExtendedPolygroupLayers (extended polygroup layer count)
// ============================================================================

// MCP:recompute_mesh_uvs - Recompute UVs on a UV channel using the configured flatten method. A channel the mesh does not have, or an empty mesh, is an error; recomputed is false, and the call fails, when the engine refuses (for example polygroups on a mesh without polygroups); a failed call leaves the UVs as they were. One undo step restores the previous UVs.
//   @param actor_name [string!] - DynamicMeshActor name (required)
//   @param uv_channel [number]  - UV channel index (default 0)
//   @param island_source [string] - "uv_islands" | "polygroups" | "existing_groups" | "selection" (default "uv_islands")
//   @param method [string]       - flatten method: "spectral_conformal" (default), "conformal" or "exp_map"
//   @returns { actor_name, uv_channel, island_source, method, recomputed }
TSharedPtr<FJsonObject> HandleRecomputeMeshUVs(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UDynamicMesh* DynMesh = ResolveDynamicMeshForSpatial(Params, Error);
    if (!DynMesh) { return mcp_common::MakeErrorReply(Error); }

    int32 UVChannel = 0;
    if (TSharedPtr<FJsonObject> Failure = GeometryScript_ReadOptionalUVChannel(Params, UVChannel))
    {
        return Failure;
    }

    FString IslandSourceStr;
    Params->TryGetStringField(TEXT("island_source"), IslandSourceStr);
    if (IslandSourceStr.IsEmpty()) { IslandSourceStr = TEXT("uv_islands"); }

    FString MethodStr;
    Params->TryGetStringField(TEXT("method"), MethodStr);
    if (MethodStr.IsEmpty()) { MethodStr = TEXT("spectral_conformal"); }

    FGeometryScriptRecomputeUVsOptions Options;
    if (IslandSourceStr.Equals(TEXT("uv_islands"), ESearchCase::IgnoreCase))
    {
        Options.IslandSource = EGeometryScriptUVIslandSource::UVIslands;
    }
    else if (IslandSourceStr.Equals(TEXT("polygroups"), ESearchCase::IgnoreCase))
    {
        Options.IslandSource = EGeometryScriptUVIslandSource::PolyGroups;
    }
    else
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Unknown island_source '%s' (uv_islands|polygroups)"), *IslandSourceStr));
    }

    // The engine ships three flatten methods; accept all three instead of rejecting two of them
    // as if they did not exist.
    if (MethodStr.Equals(TEXT("spectral_conformal"), ESearchCase::IgnoreCase))
    {
        Options.Method = EGeometryScriptUVFlattenMethod::SpectralConformal;
    }
    else if (MethodStr.Equals(TEXT("conformal"), ESearchCase::IgnoreCase))
    {
        Options.Method = EGeometryScriptUVFlattenMethod::Conformal;
    }
    else if (MethodStr.Equals(TEXT("exp_map"), ESearchCase::IgnoreCase))
    {
        Options.Method = EGeometryScriptUVFlattenMethod::ExpMap;
    }
    else
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Unknown method '%s' (spectral_conformal|conformal|exp_map)"), *MethodStr));
    }

    // The engine writes nothing to a channel the mesh lacks, and an empty mesh has nothing to unwrap.
    if (TSharedPtr<FJsonObject> Failure = GeometryScript_CheckUVChannelExists(DynMesh, UVChannel))
    {
        return Failure;
    }
    if (DynMesh->GetTriangleCount() == 0)
    {
        return mcp_common::MakeErrorReply(TEXT("The dynamic mesh is empty: there are no triangles to recompute UVs for"));
    }

    // Empty selection = entire mesh. A refusal (such as polygroups on a mesh without a polygroup layer) is
    // reported only through Debug and leaves the UVs unchanged.
    const FGeometryScriptMeshSelection EmptySelection;
    const FScopedTransaction Transaction(LOCTEXT("RecomputeMeshUVs", "MCP: Recompute Mesh UVs"));
    FGeometryScriptMeshEdit MeshEdit(DynMesh);
    UGeometryScriptDebug* Debug = GeometryScript_NewDebug();
    UGeometryScriptLibrary_MeshUVFunctions::RecomputeMeshUVs(
        DynMesh, UVChannel, Options, EmptySelection, Debug);
    const FString RecomputeError = GeometryScript_FirstError(Debug);
    if (RecomputeError.IsEmpty())
    {
        MeshEdit.Commit();
    }
    else
    {
        MeshEdit.Revert();
    }

    TSharedPtr<FJsonObject> ResultObj = GeometryScript_NewMeshData(DynMesh);
    ResultObj->SetNumberField(TEXT("uv_channel"), UVChannel);
    ResultObj->SetStringField(TEXT("island_source"), IslandSourceStr);
    ResultObj->SetStringField(TEXT("method"), MethodStr);
    ResultObj->SetBoolField(TEXT("recomputed"), RecomputeError.IsEmpty());
    return GeometryScript_OutcomeReply(ResultObj, RecomputeError.IsEmpty(), TEXT("recompute_mesh_uvs"), TEXT("recomputed"),
        RecomputeError);
}

// MCP:repack_mesh_uvs - Pack existing UV islands on a UV channel into standard UV space. A channel the mesh does not have, or an empty mesh, is an error checked before anything changes. A channel with no UVs set has no islands to pack: the call succeeds with repacked false and a note, leaving the mesh unchanged. Otherwise repacked is false, and the call fails, leaving the UVs as they were, when the engine reports an error. One undo step restores the previous UVs, nothing saved.
//   @param actor_name [string!] - DynamicMeshActor name (required)
//   @param uv_channel [number]  - UV channel index (default 0)
//   @param target_image_width [number] - target image width in pixels (default 512)
//   @param optimize_island_rotation [bool] - whether to optimize island rotation (default true)
//   @returns { actor_name, uv_channel, target_image_width, optimize_island_rotation, num_uv_islands, repacked, note? }
TSharedPtr<FJsonObject> HandleRepackMeshUVs(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UDynamicMesh* DynMesh = ResolveDynamicMeshForSpatial(Params, Error);
    if (!DynMesh) { return mcp_common::MakeErrorReply(Error); }

    int32 UVChannel = 0;
    if (TSharedPtr<FJsonObject> Failure = GeometryScript_ReadOptionalUVChannel(Params, UVChannel))
    {
        return Failure;
    }

    FGeometryScriptRepackUVsOptions Options;
    double WidthD = 0.0;
    if (Params->TryGetNumberField(TEXT("target_image_width"), WidthD) && WidthD > 0.0)
    {
        Options.TargetImageWidth = static_cast<int32>(WidthD);
    }
    bool bOptimize = true;
    if (Params->TryGetBoolField(TEXT("optimize_island_rotation"), bOptimize))
    {
        Options.bOptimizeIslandRotation = bOptimize;
    }

    // The engine writes nothing to a channel the mesh lacks, and an empty mesh has nothing to pack.
    if (TSharedPtr<FJsonObject> Failure = GeometryScript_CheckUVChannelExists(DynMesh, UVChannel))
    {
        return Failure;
    }
    if (DynMesh->GetTriangleCount() == 0)
    {
        return mcp_common::MakeErrorReply(TEXT("The dynamic mesh is empty: there are no UV islands to repack"));
    }

    // Islands are counted over the triangles that have UVs set; a channel with none (one just added, say) gives
    // the packer nothing to move, so that valid request succeeds with repacked false and the mesh untouched.
    bool bIsValidChannel = false;
    const int32 NumIslands = UGeometryScriptLibrary_MeshQueryFunctions::GetNumUVIslands(DynMesh, UVChannel, bIsValidChannel);

    TSharedPtr<FJsonObject> ResultObj = GeometryScript_NewMeshData(DynMesh);
    ResultObj->SetNumberField(TEXT("uv_channel"), UVChannel);
    ResultObj->SetNumberField(TEXT("target_image_width"), Options.TargetImageWidth);
    ResultObj->SetBoolField(TEXT("optimize_island_rotation"), Options.bOptimizeIslandRotation);
    ResultObj->SetNumberField(TEXT("num_uv_islands"), FMath::Max(0, NumIslands));
    if (NumIslands <= 0)
    {
        ResultObj->SetBoolField(TEXT("repacked"), false);
        ResultObj->SetStringField(TEXT("note"), FString::Printf(
            TEXT("UV channel %d has no UVs set: there are no islands to pack, and the mesh is unchanged"), UVChannel));
        return mcp_common::MakeOkReply(ResultObj);
    }

    // A refusal is reported only through Debug.
    const FScopedTransaction Transaction(LOCTEXT("RepackMeshUVs", "MCP: Repack Mesh UVs"));
    FGeometryScriptMeshEdit MeshEdit(DynMesh);
    UGeometryScriptDebug* Debug = GeometryScript_NewDebug();
    UGeometryScriptLibrary_MeshUVFunctions::RepackMeshUVs(
        DynMesh, UVChannel, Options, Debug);
    const FString RepackError = GeometryScript_FirstError(Debug);
    if (RepackError.IsEmpty())
    {
        MeshEdit.Commit();
    }
    else
    {
        MeshEdit.Revert();
    }

    ResultObj->SetBoolField(TEXT("repacked"), RepackError.IsEmpty());
    return GeometryScript_OutcomeReply(ResultObj, RepackError.IsEmpty(), TEXT("repack_mesh_uvs"), TEXT("repacked"),
        RepackError);
}

// MCP:delete_triangle_from_mesh - Delete one triangle by ID from a DynamicMeshActor's live dynamic mesh in the editor world; a missing ID is not an error, deleted just comes back false and no undo step is added. One undo step restores a deleted triangle; reply actor_name is the component name, not the actor label.
//   @param actor_name [string!] - Label or object name (case-insensitive) of the target DynamicMeshActor in the editor world.
//   @param triangle_id [number!] - Non-negative integer ID of the triangle to delete.
//   @returns { actor_name, triangle_id, deleted, triangle_count, previous_triangle_count }
TSharedPtr<FJsonObject> HandleDeleteTriangleFromMesh(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UDynamicMesh* DynMesh = ResolveDynamicMeshForSpatial(Params, Error);
    if (!DynMesh) { return mcp_common::MakeErrorReply(Error); }

    double TriangleIdD = 0.0;
    if (!Params->TryGetNumberField(TEXT("triangle_id"), TriangleIdD) || TriangleIdD < 0.0)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Missing required parameter: triangle_id (non-negative integer)"));
    }
    const int32 TriangleId = static_cast<int32>(TriangleIdD);

    // GetTriangleCount, not GetNumTriangleIDs: that is MaxTriangleID, the ID space, which a delete never shrinks.
    const int32 PreviousCount = DynMesh->GetTriangleCount();

    const FScopedTransaction Transaction(LOCTEXT("DeleteTriangleFromMesh", "MCP: Delete Triangle From Mesh"));
    FGeometryScriptMeshEdit MeshEdit(DynMesh);
    bool bWasDeleted = false;
    UGeometryScriptLibrary_MeshBasicEditFunctions::DeleteTriangleFromMesh(
        DynMesh, TriangleId, bWasDeleted, /*bDeferChangeNotifications*/ false);
    MeshEdit.Commit();

    const int32 NewCount = DynMesh->GetTriangleCount();

    TSharedPtr<FJsonObject> ResultObj = GeometryScript_NewMeshData(DynMesh);
    ResultObj->SetNumberField(TEXT("triangle_id"), TriangleId);
    ResultObj->SetBoolField(TEXT("deleted"), bWasDeleted);
    ResultObj->SetNumberField(TEXT("triangle_count"), NewCount);
    ResultObj->SetNumberField(TEXT("previous_triangle_count"), PreviousCount);
    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:delete_selected_triangles_from_mesh - Delete the listed triangle IDs from a DynamicMeshActor's live dynamic mesh in the editor world; IDs that do not exist are skipped, so compare num_deleted with requested_count. One undo step restores the triangles, and a call that deletes none adds no undo step; reply actor_name is the component name, not the actor label.
//   @param actor_name [string!] - Label or object name (case-insensitive) of the target DynamicMeshActor in the editor world.
//   @param triangle_ids [array!] - Non-empty array of non-negative integer triangle IDs, e.g. [0, 5, 12]; any negative or non-numeric entry fails the call.
//   @returns { actor_name, num_deleted, requested_count, triangle_count, previous_triangle_count }
TSharedPtr<FJsonObject> HandleDeleteSelectedTrianglesFromMesh(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UDynamicMesh* DynMesh = ResolveDynamicMeshForSpatial(Params, Error);
    if (!DynMesh) { return mcp_common::MakeErrorReply(Error); }

    const TArray<TSharedPtr<FJsonValue>>* IdList = nullptr;
    if (!Params->TryGetArrayField(TEXT("triangle_ids"), IdList) || !IdList || IdList->Num() == 0)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Missing required parameter: triangle_ids (non-empty array of triangle IDs)"));
    }

    FGeometryScriptIndexList Indices;
    Indices.Reset(EGeometryScriptIndexType::Triangle, IdList->Num());
    for (const TSharedPtr<FJsonValue>& Val : *IdList)
    {
        double IdD = 0.0;
        if (!Val->TryGetNumber(IdD) || IdD < 0.0)
        {
            return mcp_common::MakeErrorReply(
                TEXT("triangle_ids must be a non-empty array of non-negative integers"));
        }
        Indices.List->Add(static_cast<int32>(IdD));
    }

    // GetTriangleCount, not GetNumTriangleIDs: that is MaxTriangleID, the ID space, which a delete never shrinks.
    const int32 PreviousCount = DynMesh->GetTriangleCount();

    const FScopedTransaction Transaction(LOCTEXT("DeleteSelectedTrianglesFromMesh", "MCP: Delete Selected Triangles From Mesh"));
    FGeometryScriptMeshEdit MeshEdit(DynMesh);
    int32 NumDeleted = 0;
    UGeometryScriptLibrary_MeshBasicEditFunctions::DeleteTrianglesFromMesh(
        DynMesh, Indices, NumDeleted, /*bDeferChangeNotifications*/ false);
    MeshEdit.Commit();

    const int32 NewCount = DynMesh->GetTriangleCount();

    TSharedPtr<FJsonObject> ResultObj = GeometryScript_NewMeshData(DynMesh);
    ResultObj->SetNumberField(TEXT("requested_count"), Indices.List->Num());
    ResultObj->SetNumberField(TEXT("num_deleted"), NumDeleted);
    ResultObj->SetNumberField(TEXT("triangle_count"), NewCount);
    ResultObj->SetNumberField(TEXT("previous_triangle_count"), PreviousCount);
    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:get_num_open_border_edges - Count the open boundary edges (edges with a single adjacent triangle) of a DynamicMeshActor's live dynamic mesh in the editor world; 0 means the mesh is closed. Read-only; get the loops with get_mesh_open_border_loops. Reply actor_name is the component name, not the actor label.
//   @param actor_name [string!] - Label or object name (case-insensitive) of the target DynamicMeshActor in the editor world.
//   @returns { actor_name, num_open_border_edges }
TSharedPtr<FJsonObject> HandleGetNumOpenBorderEdges(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UDynamicMesh* DynMesh = ResolveDynamicMeshForSpatial(Params, Error);
    if (!DynMesh) { return mcp_common::MakeErrorReply(Error); }

    const int32 NumEdges = UGeometryScriptLibrary_MeshQueryFunctions::GetNumOpenBorderEdges(DynMesh);

    TSharedPtr<FJsonObject> ResultObj = GeometryScript_NewMeshData(DynMesh);
    ResultObj->SetNumberField(TEXT("num_open_border_edges"), NumEdges);
    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:get_num_extended_polygroup_layers - Count the extended polygroup layers (those beyond the default polygroup layer) on a DynamicMeshActor's live dynamic mesh in the editor world, as num_polygroup_layers. Read-only; reply actor_name is the component name, not the actor label. Change the count with set_num_extended_polygroup_layers.
//   @param actor_name [string!] - Label or object name (case-insensitive) of the target DynamicMeshActor in the editor world.
//   @returns { actor_name, num_polygroup_layers }
TSharedPtr<FJsonObject> HandleGetNumExtendedPolygroupLayers(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UDynamicMesh* DynMesh = ResolveDynamicMeshForSpatial(Params, Error);
    if (!DynMesh) { return mcp_common::MakeErrorReply(Error); }

    const int32 NumLayers = UGeometryScriptLibrary_MeshQueryFunctions::GetNumExtendedPolygroupLayers(DynMesh);

    TSharedPtr<FJsonObject> ResultObj = GeometryScript_NewMeshData(DynMesh);
    ResultObj->SetNumberField(TEXT("num_polygroup_layers"), NumLayers);
    return mcp_common::MakeOkReply(ResultObj);
}

// ===========================================================================
// StaticMesh <-> DynamicMesh asset bridge + polygroup layers.
//
// Bridges a UStaticMesh asset and the live UDynamicMesh of an ADynamicMeshActor
// in both directions, plus a polygroup-layer count setter. They run on the
// bridge's game thread (FTSTicker), which is mandatory because
// CopyMeshToStaticMesh calls FlushRenderingCommands() internally
// (MeshAssetFunctions.cpp) — that must not be issued off the game thread.
// ===========================================================================

// MCP:copy_static_mesh_to_dynamic_mesh - Copy the geometry of a UStaticMesh asset INTO the live dynamic mesh of a DynamicMeshActor via UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromStaticMesh (engine-default options: build settings applied, tangents on). Destructive replace of the actor's current mesh. Idempotent-ish: re-running re-extracts the same source geometry. It reads the hi-res source model when the asset has one, else LOD 0, whatever lod_index says, and lod_index and read_hi_res_source in the reply say which was read; a mesh with no geometry to read is an error. One undo step restores the previous mesh, and a failed copy leaves it as it was.
//   @param source_static_mesh_path [string!] - Content path of the source UStaticMesh (e.g. "/Engine/BasicShapes/Cube").
//   @param actor_name [string!] - Label or name of the target DynamicMeshActor whose GetDynamicMesh() receives the geometry.
//   @param lod_index [int] - Optional source LOD index (default 0, a negative one read as 0), echoed as requested_lod_index; it does not select the geometry, which is the hi-res source model or else LOD 0.
//   @returns { source_static_mesh_path, actor_name, lod_index, requested_lod_index, read_hi_res_source, outcome_success: bool, triangle_count, vertex_count, copied: bool }
TSharedPtr<FJsonObject> HandleCopyStaticMeshToDynamicMesh(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World, TEXT("Failed to get editor world"))) { return NoWorld; }

    FString SourcePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("source_static_mesh_path"), SourcePath)) { return Missing; }

    FString ActorName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName)) { return Missing; }

    const int32 LODIndex = GeometryScript_ReadClampedLODIndex(Params);

    // Resolve the source static-mesh asset.
    UStaticMesh* SourceMesh = mcp_common::LoadAssetOrSoftPath<UStaticMesh>(SourcePath);
    if (!SourceMesh)
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Source static mesh not found: %s"), *SourcePath));
    // The LOD the engine reads (MaxAvailable ignores LODIndex); a mesh it cannot read without killing the editor is
    // refused before the actor is touched.
    FString LODError;
    const FGeometryScriptStaticMeshLOD SourceLOD = GeometryScript_ResolveStaticMeshReadLOD(SourceMesh, LODIndex, LODError);
    if (!LODError.IsEmpty())
    {
        return mcp_common::MakeErrorReply(LODError);
    }

    // Resolve the destination DynamicMeshActor's live UDynamicMesh.
    FString ResolveError;
    FGeometryScriptMeshActor Target;
    if (!GeometryScript_ResolveLiveMesh(Params, Target, ResolveError))
    {
        return mcp_common::MakeErrorReply(ResolveError);
    }

    UE_LOG(LogTemp, Verbose,
        TEXT("[MCP] copy_static_mesh_to_dynamic_mesh: source='%s' actor='%s' lod=%d"),
        *SourcePath, *ActorName, LODIndex);

    // Engine-default read options (bApplyBuildSettings=true, bRequestTangents=true,
    // bIgnoreRemoveDegenerates=true, bUseBuildScale=true).
    FGeometryScriptCopyMeshFromAssetOptions AssetOptions;

    FGeometryScriptMeshReadLOD RequestedLOD;
    RequestedLOD.LODType = EGeometryScriptLODType::MaxAvailable; // the hi-res source model, else LOD 0, whatever LODIndex is
    RequestedLOD.LODIndex = LODIndex;

    const FScopedTransaction Transaction(LOCTEXT("CopyStaticMeshToDynamicMesh", "MCP: Copy Static Mesh To Dynamic Mesh"));
    Target.Actor->Modify();
    FGeometryScriptMeshEdit MeshEdit(Target.Mesh);

    // The engine says why a copy failed only in Debug; the reply carries that message.
    UGeometryScriptDebug* Debug = GeometryScript_NewDebug();
    EGeometryScriptOutcomePins Outcome = EGeometryScriptOutcomePins::Failure;
    UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromStaticMesh(
        SourceMesh, Target.Mesh, AssetOptions, RequestedLOD, Outcome, Debug);

    const bool bSuccess = (Outcome == EGeometryScriptOutcomePins::Success);
    if (!bSuccess)
    {
        MeshEdit.Revert();
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("CopyMeshFromStaticMesh failed for source '%s' (LOD %d, outcome=%d): %s; the mesh was left as it was"),
            *SourcePath, LODIndex, static_cast<int32>(Outcome), *GeometryScript_FirstError(Debug)));
    }
    MeshEdit.Commit();

    // Push the new geometry to the render proxy.
    const FGeometryScriptMeshCounts After = GeometryScript_NotifyAndCount(Target.Component, Target.Mesh);

    UE_LOG(LogTemp, Verbose,
        TEXT("[MCP] copy_static_mesh_to_dynamic_mesh: OK tris=%d verts=%d"),
        After.Triangles, After.Vertices);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("source_static_mesh_path"), SourcePath);
    ResultObj->SetStringField(TEXT("actor_name"), Target.Actor->GetActorLabel());
    GeometryScript_AddStaticMeshLOD(ResultObj, SourceLOD);
    ResultObj->SetBoolField(TEXT("outcome_success"), bSuccess);
    ResultObj->SetNumberField(TEXT("triangle_count"), After.Triangles);
    ResultObj->SetNumberField(TEXT("vertex_count"), After.Vertices);
    ResultObj->SetBoolField(TEXT("copied"), true);
    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:copy_dynamic_mesh_to_static_mesh - Write the live dynamic mesh of a DynamicMeshActor INTO an EXISTING /Game UStaticMesh asset's LOD via UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshToStaticMesh (7-arg V2, bUseSectionMaterials=true; engine-default options). Editor-only (the engine function is WITH_EDITOR-gated and touches GEditor + FlushRenderingCommands). Idempotent: overwrites the target LOD in place, no duplication. HONEST GATES: the target asset must already exist (create it first, e.g. via create_editable_static_mesh) and must NOT live under /Engine/ (the engine refuses to modify built-in engine assets). A lod_index outside 0 to 7 (a static mesh has at most 8 LODs) is an error checked before anything changes; one past the asset's last LOD adds the LODs in between, which the engine sets up to be reduced from an earlier LOD.
//   @param actor_name [string!] - Label or name of the source DynamicMeshActor.
//   @param target_static_mesh_path [string!] - Content path of an EXISTING /Game UStaticMesh to overwrite.
//   @param lod_index [int] - Optional target LOD index, 0 to 7 (default 0); a negative index or one of 8 or more is refused.
//   @param materials [array] - Optional ordered list of UMaterialInterface content paths, one per MaterialID (index 0 = MaterialID 0). Supplying it flips bReplaceMaterials on, so the target asset gets exactly these material slots. WITHOUT this the asset keeps its existing slots and a multi-MaterialID mesh collapses into whatever slots already existed — this is the parameter that makes per-triangle MaterialIDs visible on the saved asset.
//   @param material_slot_names [array] - Optional slot names matching materials 1:1 (default: the engine names them). Ignored when materials is absent.
//   @returns { actor_name, target_static_mesh_path, lod_index, outcome_success: bool, source_triangle_count, material_count: int, replaced_materials: bool, wrote: bool }. NOTE: this marks the target package dirty (transaction) but does NOT save to disk — persistence requires a separate save (e.g. via save_asset).
TSharedPtr<FJsonObject> HandleCopyDynamicMeshToStaticMesh(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World, TEXT("Failed to get editor world"))) { return NoWorld; }

    FString ActorName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName)) { return Missing; }

    FString TargetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("target_static_mesh_path"), TargetPath)) { return Missing; }

    // CopyMeshToStaticMesh clamps the LOD index to [0, MAX_STATIC_MESH_LODS], one past the last LOD a static mesh can
    // have, and grows the source models to reach it: 8 or more gives the asset 9 LODs, after which UStaticMesh::CanBuild
    // refuses every build of it. A negative index would be clamped to 0 and overwrite LOD 0. Both are refused before
    // anything changes; the value is read as a double so a number outside int32 is refused too.
    double LODIndexD = 0.0;
    Params->TryGetNumberField(TEXT("lod_index"), LODIndexD);
    if (LODIndexD < 0.0 || LODIndexD >= MAX_STATIC_MESH_LODS)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("lod_index must be between 0 and %d (got %g): a static mesh has at most %d LODs; nothing was changed"),
            MAX_STATIC_MESH_LODS - 1, LODIndexD, MAX_STATIC_MESH_LODS));
    }
    const int32 LODIndex = static_cast<int32>(LODIndexD);

    // HONEST GATE: the engine rejects built-in /Engine/ assets (MeshAssetFunctions.cpp).
    // Fail early with a clean, actionable message instead of surfacing the engine's generic outcome.
    if (TargetPath.StartsWith(TEXT("/Engine/")))
        return mcp_common::MakeErrorReply(
            TEXT("target_static_mesh_path under /Engine/ is not writable; the engine rejects built-in engine assets. Duplicate it into /Game first (e.g. create_editable_static_mesh)."));

    // Resolve the source DynamicMeshActor and its live UDynamicMesh.
    FString ResolveError;
    FGeometryScriptMeshActor Source;
    if (!GeometryScript_ResolveLiveMesh(Params, Source, ResolveError))
    {
        return mcp_common::MakeErrorReply(ResolveError);
    }

    // HONEST GATE: the target asset must already exist. LoadObject (not
    // UEditorAssetLibrary) per headless-asset gotchas — the latter is unreliable headless.
    UStaticMesh* ToStaticMeshAsset = LoadObject<UStaticMesh>(nullptr, *TargetPath);
    if (!ToStaticMeshAsset)
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("target static mesh asset not found: %s; create it first (e.g. create_editable_static_mesh)"), *TargetPath));

    UE_LOG(LogTemp, Verbose,
        TEXT("[MCP] copy_dynamic_mesh_to_static_mesh: actor='%s' target='%s' lod=%d"),
        *ActorName, *TargetPath, LODIndex);

    const int32 SourceTriCount = Source.Mesh->GetTriangleCount();

    // Engine-default write options (bReplaceMaterials=false, bUseBuildScale=true,
    // bEmitTransaction=true, no recompute). The engine emits its own transaction;
    // we still Modify() the asset before the write per the wiring contract.
    FGeometryScriptCopyMeshToAssetOptions Options;

    // Optional material list -> bReplaceMaterials. Without it the write runs with
    // engine defaults (bReplaceMaterials=false) and the target keeps whatever slots it had, so a
    // mesh painted with several MaterialIDs still lands on a single-slot asset.
    const TArray<TSharedPtr<FJsonValue>>* MaterialsArray = nullptr;
    if (Params->TryGetArrayField(TEXT("materials"), MaterialsArray) && MaterialsArray && MaterialsArray->Num() > 0)
    {
        for (int32 Index = 0; Index < MaterialsArray->Num(); ++Index)
        {
            const FString MaterialPath = (*MaterialsArray)[Index]->AsString();
            if (MaterialPath.IsEmpty())
                return mcp_common::MakeErrorReply(
                    FString::Printf(TEXT("materials[%d] is empty; every entry must be a UMaterialInterface content path"), Index));

            UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
            if (!Material)
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("materials[%d]: material not found: %s"), Index, *MaterialPath));

            Options.NewMaterials.Add(Material);
        }

        // Slot names are optional; when given they must line up 1:1 or the mapping is a guess.
        const TArray<TSharedPtr<FJsonValue>>* SlotNamesArray = nullptr;
        if (Params->TryGetArrayField(TEXT("material_slot_names"), SlotNamesArray) && SlotNamesArray)
        {
            if (SlotNamesArray->Num() != MaterialsArray->Num())
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("material_slot_names has %d entries but materials has %d; they must match 1:1"),
                    SlotNamesArray->Num(), MaterialsArray->Num()));

            for (const TSharedPtr<FJsonValue>& SlotNameValue : *SlotNamesArray)
            {
                Options.NewMaterialSlotNames.Add(FName(*SlotNameValue->AsString()));
            }
        }

        Options.bReplaceMaterials = true;
    }
    const int32 MaterialCount = Options.NewMaterials.Num();

    FGeometryScriptMeshWriteLOD TargetLOD; // bWriteHiResSource=false
    TargetLOD.LODIndex = LODIndex;

    const FScopedTransaction Transaction(LOCTEXT("CopyDynamicMeshToStaticMesh", "MCP: Copy Dynamic Mesh To Static Mesh"));
    ToStaticMeshAsset->Modify();

    // The engine says why a write failed only in Debug; the reply carries that message.
    UGeometryScriptDebug* Debug = GeometryScript_NewDebug();
    EGeometryScriptOutcomePins Outcome = EGeometryScriptOutcomePins::Failure;
    // 7-arg V2 (bUseSectionMaterials passed explicitly to avoid the deprecated 6-arg overload).
    UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshToStaticMesh(
        Source.Mesh, ToStaticMeshAsset, Options, TargetLOD, Outcome,
        /*bUseSectionMaterials*/ true, Debug);

    const bool bSuccess = (Outcome == EGeometryScriptOutcomePins::Success);
    if (!bSuccess)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("CopyMeshToStaticMesh failed for target '%s' (LOD %d, outcome=%d): %s"),
            *TargetPath, LODIndex, static_cast<int32>(Outcome), *GeometryScript_FirstError(Debug)));
    }

    UE_LOG(LogTemp, Verbose,
        TEXT("[MCP] copy_dynamic_mesh_to_static_mesh: OK wrote %d source tris into '%s' LOD %d (package dirty; not saved)"),
        SourceTriCount, *TargetPath, LODIndex);

    TSharedPtr<FJsonObject> ResultObj = GeometryScript_NewActorData(Source.Actor);
    ResultObj->SetStringField(TEXT("target_static_mesh_path"), TargetPath);
    ResultObj->SetNumberField(TEXT("lod_index"), LODIndex);
    ResultObj->SetBoolField(TEXT("outcome_success"), bSuccess);
    ResultObj->SetNumberField(TEXT("source_triangle_count"), SourceTriCount);
    ResultObj->SetNumberField(TEXT("material_count"), MaterialCount);
    ResultObj->SetBoolField(TEXT("replaced_materials"), Options.bReplaceMaterials);
    ResultObj->SetBoolField(TEXT("wrote"), true);
    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:set_num_extended_polygroup_layers - Set the number of extended polygroup layers on the live dynamic mesh of a DynamicMeshActor via UGeometryScriptLibrary_MeshPolygroupFunctions::SetNumExtendedPolygroupLayers. Idempotent: the engine dedups (only reshapes attributes when the count actually differs from the current one), so re-setting the same count is a no-op. The layer count is read back AFTER the set via GetNumExtendedPolygroupLayers: applied is true when it equals the request, and the call fails, leaving the mesh as it was, when it does not. One undo step restores the previous layers.
//   @param actor_name [string!] - Label or name of the target DynamicMeshActor.
//   @param num_layers [int!] - Desired number of extended polygroup layers (>= 0).
//   @returns { actor_name, requested_num_layers, num_polygroup_layers, applied: bool }
TSharedPtr<FJsonObject> HandleSetNumExtendedPolygroupLayers(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World, TEXT("Failed to get editor world"))) { return NoWorld; }

    FString ActorName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName)) { return Missing; }

    if (!Params->HasField(TEXT("num_layers")))
        return mcp_common::MakeErrorReply(TEXT("num_layers is required"));
    double NumLayersDouble = 0.0;
    Params->TryGetNumberField(TEXT("num_layers"), NumLayersDouble);
    const int32 NumLayers = static_cast<int32>(NumLayersDouble);
    if (NumLayers < 0)
        return mcp_common::MakeErrorReply(TEXT("num_layers must be >= 0"));

    // Resolve the target DynamicMeshActor's live UDynamicMesh (+ component for NotifyMeshUpdated).
    FString ResolveError;
    FGeometryScriptMeshActor Target;
    if (!GeometryScript_ResolveLiveMesh(Params, Target, ResolveError))
    {
        return mcp_common::MakeErrorReply(ResolveError);
    }

    UE_LOG(LogTemp, Verbose,
        TEXT("[MCP] set_num_extended_polygroup_layers: actor='%s' num_layers=%d"),
        *ActorName, NumLayers);

    const FScopedTransaction Transaction(LOCTEXT("SetNumExtendedPolygroupLayers", "MCP: Set Num Extended Polygroup Layers"));
    Target.Actor->Modify();
    FGeometryScriptMeshEdit MeshEdit(Target.Mesh);

    // Engine self-dedups: only reshapes attribute layers when NumLayers != current.
    UGeometryScriptLibrary_MeshPolygroupFunctions::SetNumExtendedPolygroupLayers(
        Target.Mesh, NumLayers, /*Debug*/ nullptr);
    if (UGeometryScriptLibrary_MeshQueryFunctions::GetNumExtendedPolygroupLayers(Target.Mesh) == NumLayers)
    {
        MeshEdit.Commit();
    }
    else
    {
        MeshEdit.Revert();
    }

    Target.Component->NotifyMeshUpdated();

    // Read back the layer count AFTER the set to confirm (same getter path as
    // HandleGetNumExtendedPolygroupLayers). GetNumExtendedPolygroupLayers takes no Debug arg.
    const int32 ActualLayers = UGeometryScriptLibrary_MeshQueryFunctions::GetNumExtendedPolygroupLayers(Target.Mesh);

    UE_LOG(LogTemp, Verbose,
        TEXT("[MCP] set_num_extended_polygroup_layers: OK requested=%d actual=%d"),
        NumLayers, ActualLayers);

    TSharedPtr<FJsonObject> ResultObj = GeometryScript_NewActorData(Target.Actor);
    ResultObj->SetNumberField(TEXT("requested_num_layers"), NumLayers);
    ResultObj->SetNumberField(TEXT("num_polygroup_layers"), ActualLayers);
    ResultObj->SetBoolField(TEXT("applied"), ActualLayers == NumLayers);
    return GeometryScript_OutcomeReply(ResultObj, ActualLayers == NumLayers, TEXT("set_num_extended_polygroup_layers"),
        TEXT("applied"), FString::Printf(TEXT("the mesh has %d extended polygroup layers after the call, not the %d requested"), ActualLayers, NumLayers));
}

// ============================================================================
// Per-triangle MaterialIDs.
//
// WHY THIS BLOCK EXISTS: a dynamic mesh baked into a UStaticMesh with every triangle at
// MaterialID 0 comes out single-slot — "one mesh, one material". The bake side is not the
// problem: copy_dynamic_mesh_to_static_mesh calls CopyMeshToStaticMesh with
// bUseSectionMaterials=true, which already emits one section per distinct MaterialID.
// What these writers supply is the INPUT: an ID other than 0.
//
// THE TRAP, and why every writer below opens the same way: on a mesh whose MaterialID
// attribute is not enabled, the engine drops the write and reports SUCCESS. Reading back
// then shows max_material_id = 0 with no error anywhere. So each writer calls
// EnableMaterialIDs first, and get_mesh_material_ids exists to make the result falsifiable
// with a read instead of trusting success:true.
// ============================================================================

namespace
{
    /**
     * Resolve a JSON region description into a triangle selection.
     *
     * Regions: "all" | "box" | "sphere" | "plane" | "normal_angle" | "material_id" | "polygroup".
     * The spatial ones mirror the engine's SelectMeshElements* family 1:1; "material_id" and
     * "polygroup" select what is already tagged, which is what makes repaint-by-part possible
     * after a boolean has fused the original polygroups.
     *
     * Returns false with OutError set on an unknown region or a missing required field.
     */
    bool ResolveMeshRegionSelection(UDynamicMesh* TargetMesh, const TSharedPtr<FJsonObject>& Params,
                                    FGeometryScriptMeshSelection& OutSelection,
                                    FString& OutRegion, FString& OutError)
    {
        FString Region;
        Params->TryGetStringField(TEXT("region"), Region);
        if (Region.IsEmpty()) { Region = TEXT("all"); }
        OutRegion = Region;

        bool bInvert = false;
        Params->TryGetBoolField(TEXT("invert"), bInvert);

        constexpr EGeometryScriptMeshSelectionType SelectionType = EGeometryScriptMeshSelectionType::Triangles;

        // A triangle counts as inside only when this many of its points are; 3 = fully contained.
        int32 MinTrianglePoints = 3;
        {
            double MinPointsDouble = 0.0;
            if (Params->TryGetNumberField(TEXT("min_triangle_points"), MinPointsDouble))
            {
                MinTrianglePoints = FMath::Clamp(static_cast<int32>(MinPointsDouble), 1, 3);
            }
        }

        if (Region.Equals(TEXT("all"), ESearchCase::IgnoreCase))
        {
            UGeometryScriptLibrary_MeshSelectionFunctions::CreateSelectAllMeshSelection(
                TargetMesh, OutSelection, SelectionType);
            return true;
        }

        if (Region.Equals(TEXT("box"), ESearchCase::IgnoreCase))
        {
            const FVector Center = mcp_common::ReadJsonVector(Params, TEXT("center"));
            const FVector Extent = mcp_common::ReadJsonVector(Params, TEXT("extent"));
            if (Extent.IsNearlyZero())
            {
                OutError = TEXT("region 'box' requires a non-zero 'extent' [x,y,z] (half-size in cm)");
                return false;
            }
            const FBox Box(Center - Extent, Center + Extent);
            UGeometryScriptLibrary_MeshSelectionFunctions::SelectMeshElementsInBox(
                TargetMesh, OutSelection, Box, SelectionType, bInvert, MinTrianglePoints);
            return true;
        }

        if (Region.Equals(TEXT("sphere"), ESearchCase::IgnoreCase))
        {
            const FVector Center = mcp_common::ReadJsonVector(Params, TEXT("center"));
            double Radius = 0.0;
            Params->TryGetNumberField(TEXT("radius"), Radius);
            if (Radius <= 0.0)
            {
                OutError = TEXT("region 'sphere' requires a positive 'radius' (cm)");
                return false;
            }
            UGeometryScriptLibrary_MeshSelectionFunctions::SelectMeshElementsInSphere(
                TargetMesh, OutSelection, Center, Radius, SelectionType, bInvert, MinTrianglePoints);
            return true;
        }

        if (Region.Equals(TEXT("plane"), ESearchCase::IgnoreCase))
        {
            const FVector Origin = mcp_common::ReadJsonVector(Params, TEXT("center"));
            FVector Normal = FVector::UpVector;
            if (Params->HasField(TEXT("normal")))
            {
                Normal = mcp_common::ReadJsonVector(Params, TEXT("normal"));
                if (Normal.IsNearlyZero())
                {
                    OutError = TEXT("region 'plane' requires a non-zero 'normal' [x,y,z]");
                    return false;
                }
                Normal.Normalize();
            }
            // Selects the half-space on the normal's side; 'invert' takes the other half.
            UGeometryScriptLibrary_MeshSelectionFunctions::SelectMeshElementsWithPlane(
                TargetMesh, OutSelection, Origin, Normal, SelectionType, bInvert, MinTrianglePoints);
            return true;
        }

        if (Region.Equals(TEXT("normal_angle"), ESearchCase::IgnoreCase))
        {
            FVector Normal = FVector::UpVector;
            if (Params->HasField(TEXT("normal")))
            {
                Normal = mcp_common::ReadJsonVector(Params, TEXT("normal"));
                if (Normal.IsNearlyZero())
                {
                    OutError = TEXT("region 'normal_angle' requires a non-zero 'normal' [x,y,z]");
                    return false;
                }
                Normal.Normalize();
            }
            double MaxAngleDeg = 15.0;
            Params->TryGetNumberField(TEXT("max_angle_deg"), MaxAngleDeg);
            if (MaxAngleDeg <= 0.0)
            {
                OutError = TEXT("region 'normal_angle' requires a positive 'max_angle_deg'");
                return false;
            }
            UGeometryScriptLibrary_MeshSelectionFunctions::SelectMeshElementsByNormalAngle(
                TargetMesh, OutSelection, Normal, MaxAngleDeg, SelectionType, bInvert, MinTrianglePoints);
            return true;
        }

        if (Region.Equals(TEXT("material_id"), ESearchCase::IgnoreCase))
        {
            // Deliberately NOT 'material_id': in the painting tool that key is the ID being
            // WRITTEN. Reusing it would make "repaint 1 as 2" unexpressible.
            if (!Params->HasField(TEXT("source_material_id")))
            {
                OutError = TEXT("region 'material_id' requires 'source_material_id' (the ID to select; 'material_id' is the ID being written)");
                return false;
            }
            double SourceIDDouble = 0.0;
            Params->TryGetNumberField(TEXT("source_material_id"), SourceIDDouble);
            UGeometryScriptLibrary_MeshSelectionFunctions::SelectMeshElementsByMaterialID(
                TargetMesh, static_cast<int32>(SourceIDDouble), OutSelection, SelectionType);
            return true;
        }

        if (Region.Equals(TEXT("polygroup"), ESearchCase::IgnoreCase))
        {
            if (!Params->HasField(TEXT("polygroup_id")))
            {
                OutError = TEXT("region 'polygroup' requires 'polygroup_id'");
                return false;
            }
            double PolygroupIDDouble = 0.0;
            Params->TryGetNumberField(TEXT("polygroup_id"), PolygroupIDDouble);

            FGeometryScriptGroupLayer GroupLayer; // bDefaultLayer=true
            double LayerIndexDouble = 0.0;
            if (Params->TryGetNumberField(TEXT("polygroup_layer"), LayerIndexDouble))
            {
                GroupLayer.bDefaultLayer = false;
                GroupLayer.ExtendedLayerIndex = static_cast<int32>(LayerIndexDouble);
            }
            UGeometryScriptLibrary_MeshSelectionFunctions::SelectMeshElementsByPolygroup(
                TargetMesh, GroupLayer, static_cast<int32>(PolygroupIDDouble), OutSelection, SelectionType);
            return true;
        }

        if (Region.Equals(TEXT("connected_component"), ESearchCase::IgnoreCase))
        {
            // Address one physical PIECE by its island index, which is what
            // get_mesh_components_detail enumerates. Without this, painting a piece means knowing
            // its triangle_ids by heart -- and guessing those is error-prone.
            if (!Params->HasField(TEXT("component_index")))
            {
                OutError = TEXT("region 'connected_component' requires 'component_index' (see get_mesh_components_detail)");
                return false;
            }
            double ComponentIndexDouble = 0.0;
            Params->TryGetNumberField(TEXT("component_index"), ComponentIndexDouble);
            const int32 ComponentIndex = static_cast<int32>(ComponentIndexDouble);
            if (ComponentIndex < 0)
            {
                OutError = FString::Printf(TEXT("component_index must be >= 0 (got %d)"), ComponentIndex);
                return false;
            }

            // Islands are indexed in the same order get_mesh_components_detail reports them: both
            // read FindConnectedTriangles on the same mesh, so index N means the same piece in both.
            TArray<int32> ComponentTriangles;
            int32 NumComponents = 0;
            TargetMesh->ProcessMesh([&](const UE::Geometry::FDynamicMesh3& Mesh)
            {
                UE::Geometry::FMeshConnectedComponents Components(&Mesh);
                Components.FindConnectedTriangles();
                NumComponents = Components.Components.Num();
                if (ComponentIndex < NumComponents)
                {
                    ComponentTriangles = Components.Components[ComponentIndex].Indices;
                }
            });

            if (ComponentIndex >= NumComponents)
            {
                OutError = FString::Printf(
                    TEXT("component_index %d is out of range: the mesh has %d connected components (0..%d)"),
                    ComponentIndex, NumComponents, FMath::Max(0, NumComponents - 1));
                return false;
            }

            FGeometryScriptIndexList TriangleIDList;
            TriangleIDList.Reset(EGeometryScriptIndexType::Triangle, ComponentTriangles.Num());
            for (const int32 TriangleID : ComponentTriangles)
            {
                TriangleIDList.List->Add(TriangleID);
            }
            UGeometryScriptLibrary_MeshSelectionFunctions::ConvertIndexListToMeshSelection(
                TargetMesh, TriangleIDList, SelectionType, OutSelection);
            return true;
        }

        OutError = FString::Printf(
            TEXT("Unknown region '%s' (all|box|sphere|plane|normal_angle|material_id|polygroup|connected_component)"), *Region);
        return false;
    }

    /** Read a JSON int array (e.g. triangle_ids) into a GeometryScript index list. */
    bool BuildIndexListFromJson(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName,
                                EGeometryScriptIndexType IndexType,
                                FGeometryScriptIndexList& OutList, FString& OutError)
    {
        const TArray<TSharedPtr<FJsonValue>>* JsonArray = nullptr;
        if (!Params->TryGetArrayField(FieldName, JsonArray) || !JsonArray)
        {
            OutError = FString::Printf(TEXT("'%s' is required and must be an array of integers"), FieldName);
            return false;
        }
        if (JsonArray->Num() == 0)
        {
            OutError = FString::Printf(TEXT("'%s' is empty; nothing to do"), FieldName);
            return false;
        }

        OutList.Reset(IndexType, JsonArray->Num());
        for (const TSharedPtr<FJsonValue>& Value : *JsonArray)
        {
            OutList.List->Add(static_cast<int32>(Value->AsNumber()));
        }
        return true;
    }
}

// MCP:get_mesh_material_ids - Read the per-triangle MaterialID distribution of a DynamicMeshActor's live dynamic mesh via UGeometryScriptLibrary_MeshMaterialFunctions::GetMaxMaterialID + GetAllTriangleMaterialIDs. Read-only. This is the ORACLE for every MaterialID write: it returns a per-ID triangle histogram, so "did the paint actually land?" is answerable with a read instead of trusting a success flag. IMPORTANT: has_material_ids=false means the mesh has no MaterialID attribute at all, which is exactly the state in which writes are silently dropped by the engine.
//   @param actor_name [string!] - Label or name of the target DynamicMeshActor.
//   @returns { actor_name, has_material_ids: bool, max_material_id: int, triangle_count: int, distinct_material_ids: int, histogram: [ { material_id: int, triangle_count: int } ] }
TSharedPtr<FJsonObject> HandleGetMeshMaterialIDs(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UDynamicMesh* DynMesh = ResolveDynamicMeshForSpatial(Params, Error);
    if (!DynMesh) { return mcp_common::MakeErrorReply(Error); }

    bool bHasMaterialIDs = false;
    const int32 MaxMaterialID = UGeometryScriptLibrary_MeshMaterialFunctions::GetMaxMaterialID(DynMesh, bHasMaterialIDs);

    // One entry per triangle, in triangle order.
    FGeometryScriptIndexList MaterialIDList;
    bool bHasIDsFromList = false;
    UGeometryScriptLibrary_MeshMaterialFunctions::GetAllTriangleMaterialIDs(DynMesh, MaterialIDList, bHasIDsFromList);

    TMap<int32, int32> Histogram;
    if (MaterialIDList.List.IsValid())
    {
        for (const int32 MaterialID : *MaterialIDList.List)
        {
            Histogram.FindOrAdd(MaterialID)++;
        }
    }

    TArray<int32> SortedIDs;
    Histogram.GetKeys(SortedIDs);
    SortedIDs.Sort();

    TArray<TSharedPtr<FJsonValue>> HistogramJson;
    for (const int32 MaterialID : SortedIDs)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetNumberField(TEXT("material_id"), MaterialID);
        Entry->SetNumberField(TEXT("triangle_count"), Histogram[MaterialID]);
        HistogramJson.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> ResultObj = GeometryScript_NewMeshData(DynMesh);
    ResultObj->SetBoolField(TEXT("has_material_ids"), bHasMaterialIDs || bHasIDsFromList);
    ResultObj->SetNumberField(TEXT("max_material_id"), MaxMaterialID);
    ResultObj->SetNumberField(TEXT("triangle_count"), DynMesh->GetTriangleCount());
    ResultObj->SetNumberField(TEXT("distinct_material_ids"), SortedIDs.Num());
    ResultObj->SetArrayField(TEXT("histogram"), HistogramJson);
    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:set_mesh_material_id - Stamp a MaterialID onto triangles of a DynamicMeshActor's live dynamic mesh. scope='all' repaints every triangle (UGeometryScriptLibrary_MeshMaterialFunctions::ClearMaterialIDs); scope='triangles' repaints the listed triangle IDs (SetMaterialIDOnTriangles). Calls EnableMaterialIDs first, because on a mesh without that attribute the engine drops the write. An ID in triangle_ids that is not a triangle, or an empty mesh, is an error checked before anything is painted. The targeted triangles are read back: applied is true when all of them carry material_id, and the call fails otherwise, leaving the MaterialIDs as they were. One undo step restores the previous MaterialIDs.
//   @param actor_name [string!] - Label or name of the target DynamicMeshActor.
//   @param material_id [int!] - MaterialID to write (>= 0). Index into the material list later passed to copy_dynamic_mesh_to_static_mesh.
//   @param scope [string] - 'all' (default) or 'triangles'.
//   @param triangle_ids [array] - Required when scope='triangles': array of triangle IDs to repaint (e.g. from select_mesh_triangles).
//   @returns { actor_name, material_id, scope, triangles_targeted: int, triangles_painted: int, max_material_id_after: int, distinct_material_ids_after: int, applied: bool }
TSharedPtr<FJsonObject> HandleSetMeshMaterialID(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params->HasField(TEXT("material_id")))
        return mcp_common::MakeErrorReply(TEXT("material_id is required"));
    double MaterialIDDouble = 0.0;
    Params->TryGetNumberField(TEXT("material_id"), MaterialIDDouble);
    const int32 MaterialID = static_cast<int32>(MaterialIDDouble);
    if (MaterialID < 0)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("material_id must be >= 0 (got %d)"), MaterialID));

    FString Scope;
    Params->TryGetStringField(TEXT("scope"), Scope);
    if (Scope.IsEmpty()) { Scope = TEXT("all"); }

    FString Error;
    FGeometryScriptMeshActor Target;
    if (!GeometryScript_ResolveLiveMesh(Params, Target, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }

    // Parse the triangle list BEFORE opening the transaction so a bad payload changes nothing.
    FGeometryScriptIndexList TriangleIDList;
    const bool bScopeTriangles = Scope.Equals(TEXT("triangles"), ESearchCase::IgnoreCase);
    if (bScopeTriangles)
    {
        if (!BuildIndexListFromJson(Params, TEXT("triangle_ids"), EGeometryScriptIndexType::Triangle, TriangleIDList, Error))
            return mcp_common::MakeErrorReply(Error);
    }
    else if (!Scope.Equals(TEXT("all"), ESearchCase::IgnoreCase))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Unknown scope '%s' (all|triangles)"), *Scope));
    }

    // SetMaterialIDOnTriangles stops at the first ID that is not a triangle, after painting the ones before it,
    // so every ID is checked first; an empty mesh has nothing to paint.
    if (bScopeTriangles)
    {
        for (const int32 TriangleID : *TriangleIDList.List)
        {
            if (!UGeometryScriptLibrary_MeshQueryFunctions::IsValidTriangleID(Target.Mesh, TriangleID))
            {
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("triangle_ids: %d is not a triangle of the mesh (it has %d triangles); nothing was painted"),
                    TriangleID, Target.Mesh->GetTriangleCount()));
            }
        }
    }
    else if (Target.Mesh->GetTriangleCount() == 0)
    {
        return mcp_common::MakeErrorReply(TEXT("The dynamic mesh is empty: there are no triangles to paint"));
    }

    const FScopedTransaction Transaction(LOCTEXT("SetMeshMaterialID", "MCP: Set Mesh Material ID"));
    Target.Actor->Modify();
    FGeometryScriptMeshEdit MeshEdit(Target.Mesh);

    // Mandatory: without the attribute the writes below are silent no-ops.
    UGeometryScriptLibrary_MeshMaterialFunctions::EnableMaterialIDs(Target.Mesh, /*Debug*/ nullptr);

    int32 TrianglesTargeted = 0;
    if (bScopeTriangles)
    {
        TrianglesTargeted = TriangleIDList.List->Num();
        UGeometryScriptLibrary_MeshMaterialFunctions::SetMaterialIDOnTriangles(
            Target.Mesh, TriangleIDList, MaterialID, /*bDeferChangeNotifications*/ false, /*Debug*/ nullptr);
    }
    else
    {
        TrianglesTargeted = Target.Mesh->GetTriangleCount();
        UGeometryScriptLibrary_MeshMaterialFunctions::ClearMaterialIDs(Target.Mesh, MaterialID, /*Debug*/ nullptr);
    }

    Target.Component->NotifyMeshUpdated();

    // Read back so the response carries evidence, not just an echo of the request: how many of the targeted
    // triangles now carry MaterialID.
    int32 TrianglesPainted = 0;
    if (bScopeTriangles)
    {
        for (const int32 TriangleID : *TriangleIDList.List)
        {
            bool bIsValidTriangle = false;
            const int32 ReadID = UGeometryScriptLibrary_MeshMaterialFunctions::GetTriangleMaterialID(Target.Mesh, TriangleID, bIsValidTriangle);
            TrianglesPainted += (bIsValidTriangle && ReadID == MaterialID) ? 1 : 0;
        }
    }
    else
    {
        TrianglesPainted = GeometryScript_CountMaterialID(Target.Mesh, MaterialID);
    }
    const bool bApplied = TrianglesTargeted > 0 && TrianglesPainted == TrianglesTargeted;
    if (bApplied)
    {
        MeshEdit.Commit();
    }
    else
    {
        MeshEdit.Revert();
    }

    int32 MaxAfter = 0;
    int32 DistinctAfter = 0;
    GeometryScript_ReadMaterialIDs(Target.Mesh, MaxAfter, DistinctAfter);

    UE_LOG(LogTemp, Verbose,
        TEXT("[MCP] set_mesh_material_id: actor='%s' id=%d scope=%s targeted=%d max_after=%d distinct_after=%d"),
        *Target.Actor->GetActorLabel(), MaterialID, *Scope, TrianglesTargeted, MaxAfter, DistinctAfter);

    TSharedPtr<FJsonObject> ResultObj = GeometryScript_NewActorData(Target.Actor);
    ResultObj->SetNumberField(TEXT("material_id"), MaterialID);
    ResultObj->SetStringField(TEXT("scope"), Scope);
    ResultObj->SetNumberField(TEXT("triangles_targeted"), TrianglesTargeted);
    ResultObj->SetNumberField(TEXT("triangles_painted"), TrianglesPainted);
    ResultObj->SetNumberField(TEXT("max_material_id_after"), MaxAfter);
    ResultObj->SetNumberField(TEXT("distinct_material_ids_after"), DistinctAfter);
    ResultObj->SetBoolField(TEXT("applied"), bApplied);
    return GeometryScript_OutcomeReply(ResultObj, bApplied, TEXT("set_mesh_material_id"), TEXT("applied"),
        FString::Printf(TEXT("%d of the %d targeted triangles carry MaterialID %d after the write"), TrianglesPainted, TrianglesTargeted, MaterialID));
}

// MCP:set_polygroup_material_id - Stamp a MaterialID onto every triangle of one polygroup via UGeometryScriptLibrary_MeshMaterialFunctions::SetPolygroupMaterialID. This is usually the cheapest way to paint a part: append_mesh_primitive lays primitives down with PerFace polygroups, so each appended piece is already its own group. HONEST GATE: the engine reports whether the polygroup ID actually existed, and that flag is surfaced as polygroup_id_valid: a false there means nothing was painted, even though the call succeeded, and no undo step is added. A polygroup layer the mesh does not have (a polygroup_layer past its extended layers, or a mesh without default polygroups) is an engine error: applied is false, the call fails and the mesh is left as it was. One undo step restores the previous MaterialIDs.
//   @param actor_name [string!] - Label or name of the target DynamicMeshActor.
//   @param polygroup_id [int!] - Polygroup ID to repaint.
//   @param material_id [int!] - MaterialID to write (>= 0).
//   @param polygroup_layer [int] - Optional extended polygroup layer index. Omit for the default layer.
//   @returns { actor_name, polygroup_id, material_id, polygroup_id_valid: bool, max_material_id_after: int, distinct_material_ids_after: int, applied: bool }
TSharedPtr<FJsonObject> HandleSetPolygroupMaterialID(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params->HasField(TEXT("polygroup_id")))
        return mcp_common::MakeErrorReply(TEXT("polygroup_id is required"));
    if (!Params->HasField(TEXT("material_id")))
        return mcp_common::MakeErrorReply(TEXT("material_id is required"));

    double PolygroupIDDouble = 0.0;
    Params->TryGetNumberField(TEXT("polygroup_id"), PolygroupIDDouble);
    const int32 PolygroupID = static_cast<int32>(PolygroupIDDouble);

    double MaterialIDDouble = 0.0;
    Params->TryGetNumberField(TEXT("material_id"), MaterialIDDouble);
    const int32 MaterialID = static_cast<int32>(MaterialIDDouble);
    if (MaterialID < 0)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("material_id must be >= 0 (got %d)"), MaterialID));

    FString Error;
    FGeometryScriptMeshActor Target;
    if (!GeometryScript_ResolveLiveMesh(Params, Target, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }

    FGeometryScriptGroupLayer GroupLayer; // bDefaultLayer=true
    double LayerIndexDouble = 0.0;
    if (Params->TryGetNumberField(TEXT("polygroup_layer"), LayerIndexDouble))
    {
        GroupLayer.bDefaultLayer = false;
        GroupLayer.ExtendedLayerIndex = static_cast<int32>(LayerIndexDouble);
    }

    const FScopedTransaction Transaction(LOCTEXT("SetPolygroupMaterialID", "MCP: Set Polygroup Material ID"));
    Target.Actor->Modify();
    FGeometryScriptMeshEdit MeshEdit(Target.Mesh);

    UGeometryScriptLibrary_MeshMaterialFunctions::EnableMaterialIDs(Target.Mesh, /*Debug*/ nullptr);

    // SetPolygroupMaterialID reports a polygroup layer the mesh does not have only as an error in Debug.
    UGeometryScriptDebug* Debug = GeometryScript_NewDebug();
    bool bIsValidPolygroupID = false;
    UGeometryScriptLibrary_MeshMaterialFunctions::SetPolygroupMaterialID(
        Target.Mesh, GroupLayer, PolygroupID, MaterialID, bIsValidPolygroupID,
        /*bDeferChangeNotifications*/ false, Debug);
    const FString PaintError = GeometryScript_FirstError(Debug);
    // Nothing painted (an engine error, or no triangle in that polygroup): the mesh is put back as it was, which also
    // drops a MaterialID attribute EnableMaterialIDs just added, so the call leaves no undo step.
    if (PaintError.IsEmpty() && bIsValidPolygroupID)
    {
        MeshEdit.Commit();
    }
    else
    {
        MeshEdit.Revert();
    }

    Target.Component->NotifyMeshUpdated();

    int32 MaxAfter = 0;
    int32 DistinctAfter = 0;
    GeometryScript_ReadMaterialIDs(Target.Mesh, MaxAfter, DistinctAfter);

    TSharedPtr<FJsonObject> ResultObj = GeometryScript_NewActorData(Target.Actor);
    ResultObj->SetNumberField(TEXT("polygroup_id"), PolygroupID);
    ResultObj->SetNumberField(TEXT("material_id"), MaterialID);
    ResultObj->SetBoolField(TEXT("polygroup_id_valid"), bIsValidPolygroupID);
    ResultObj->SetNumberField(TEXT("max_material_id_after"), MaxAfter);
    ResultObj->SetNumberField(TEXT("distinct_material_ids_after"), DistinctAfter);
    ResultObj->SetBoolField(TEXT("applied"), PaintError.IsEmpty() && bIsValidPolygroupID);
    if (!PaintError.IsEmpty())
    {
        return GeometryScript_OutcomeReply(ResultObj, false, TEXT("set_polygroup_material_id"), TEXT("applied"), PaintError);
    }
    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:set_mesh_material_id_in_region - Stamp a MaterialID onto every triangle inside a region, via the engine's SelectMeshElements* family + SetMaterialIDForMeshSelection. Use this when polygroups are not usable: after a boolean, appended pieces are fused and set_polygroup_material_id cannot address them separately. Coordinates are LOCAL to the dynamic mesh, not world. Preview any region with select_mesh_triangles before painting: a region that selects 0 triangles still returns success, and adds no undo step. One undo step restores the previous MaterialIDs.
//   @param actor_name [string!] - Label or name of the target DynamicMeshActor.
//   @param material_id [int!] - MaterialID to write (>= 0).
//   @param region [string] - 'all' (default) | 'box' | 'sphere' | 'plane' | 'normal_angle' | 'material_id' | 'polygroup' | 'connected_component' (one physical PIECE, by the island index that get_mesh_components_detail reports).
//   @param center [array] - Local [x,y,z] centre. Used by box/sphere, and as the plane origin.
//   @param extent [array] - Local [x,y,z] HALF-size in cm. Required by region='box'.
//   @param radius [number] - Radius in cm. Required by region='sphere'.
//   @param normal [array] - [x,y,z] direction. Plane normal for region='plane', facing direction for 'normal_angle'. Default [0,0,1].
//   @param max_angle_deg [number] - Max angle from normal in degrees for region='normal_angle' (default 15).
//   @param source_material_id [int] - The MaterialID to SELECT when region='material_id'. Separate from material_id, which is the one being written, so 'repaint 1 as 2' is expressible.
//   @param polygroup_id [int] - Polygroup to select when region='polygroup'.
//   @param polygroup_layer [int] - Optional extended polygroup layer index for region='polygroup'.
//   @param component_index [int] - Island index when region='connected_component'. Get it from get_mesh_components_detail, which reports each piece's size and bounding box.
//   @param min_triangle_points [int] - How many of a triangle's 3 points must be inside for it to count, 1-3 (default 3 = fully contained). Only used by the spatial regions.
//   @param invert [bool] - Invert the spatial selection (default false).
//   @returns { actor_name, material_id, region, triangles_selected: int, max_material_id_after: int, distinct_material_ids_after: int, applied: bool }
TSharedPtr<FJsonObject> HandleSetMeshMaterialIDInRegion(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params->HasField(TEXT("material_id")))
        return mcp_common::MakeErrorReply(TEXT("material_id is required"));
    double MaterialIDDouble = 0.0;
    Params->TryGetNumberField(TEXT("material_id"), MaterialIDDouble);
    const int32 MaterialID = static_cast<int32>(MaterialIDDouble);
    if (MaterialID < 0)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("material_id must be >= 0 (got %d)"), MaterialID));

    FString Error;
    FGeometryScriptMeshActor Target;
    if (!GeometryScript_ResolveLiveMesh(Params, Target, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }

    const FScopedTransaction Transaction(LOCTEXT("SetMeshMaterialIDInRegion", "MCP: Set Mesh Material ID In Region"));
    Target.Actor->Modify();
    FGeometryScriptMeshEdit MeshEdit(Target.Mesh);

    UGeometryScriptLibrary_MeshMaterialFunctions::EnableMaterialIDs(Target.Mesh, /*Debug*/ nullptr);

    FGeometryScriptMeshSelection Selection;
    FString Region;
    if (!ResolveMeshRegionSelection(Target.Mesh, Params, Selection, Region, Error))
    {
        MeshEdit.Revert();
        return mcp_common::MakeErrorReply(Error);
    }

    const int32 TrianglesSelected = Selection.GetNumSelected();

    UGeometryScriptLibrary_MeshMaterialFunctions::SetMaterialIDForMeshSelection(
        Target.Mesh, Selection, MaterialID, /*bDeferChangeNotifications*/ false, /*Debug*/ nullptr);
    MeshEdit.Commit();

    Target.Component->NotifyMeshUpdated();

    int32 MaxAfter = 0;
    int32 DistinctAfter = 0;
    GeometryScript_ReadMaterialIDs(Target.Mesh, MaxAfter, DistinctAfter);

    UE_LOG(LogTemp, Verbose,
        TEXT("[MCP] set_mesh_material_id_in_region: actor='%s' id=%d region=%s selected=%d distinct_after=%d"),
        *Target.Actor->GetActorLabel(), MaterialID, *Region, TrianglesSelected, DistinctAfter);

    TSharedPtr<FJsonObject> ResultObj = GeometryScript_NewActorData(Target.Actor);
    ResultObj->SetNumberField(TEXT("material_id"), MaterialID);
    ResultObj->SetStringField(TEXT("region"), Region);
    ResultObj->SetNumberField(TEXT("triangles_selected"), TrianglesSelected);
    ResultObj->SetNumberField(TEXT("max_material_id_after"), MaxAfter);
    ResultObj->SetNumberField(TEXT("distinct_material_ids_after"), DistinctAfter);
    ResultObj->SetBoolField(TEXT("applied"), TrianglesSelected > 0);
    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:select_mesh_triangles - Resolve a region into a triangle selection and REPORT it, without modifying anything. Read-only companion of set_mesh_material_id_in_region: same region parameters, same resolver, so a region previewed here paints exactly those triangles there. Use it to avoid the failure mode where a mis-specified box selects 0 triangles and the paint call still returns success. Coordinates are LOCAL to the dynamic mesh, not world.
//   @param actor_name [string!] - Label or name of the target DynamicMeshActor.
//   @param region [string] - 'all' (default) | 'box' | 'sphere' | 'plane' | 'normal_angle' | 'material_id' | 'polygroup' | 'connected_component' (one physical PIECE, by the island index that get_mesh_components_detail reports).
//   @param center [array] - Local [x,y,z] centre. Used by box/sphere, and as the plane origin.
//   @param extent [array] - Local [x,y,z] HALF-size in cm. Required by region='box'.
//   @param radius [number] - Radius in cm. Required by region='sphere'.
//   @param normal [array] - [x,y,z] direction. Plane normal for region='plane', facing direction for 'normal_angle'. Default [0,0,1].
//   @param max_angle_deg [number] - Max angle from normal in degrees for region='normal_angle' (default 15).
//   @param source_material_id [int] - MaterialID to select when region='material_id'.
//   @param polygroup_id [int] - Polygroup to select when region='polygroup'.
//   @param polygroup_layer [int] - Optional extended polygroup layer index for region='polygroup'.
//   @param component_index [int] - Island index when region='connected_component'. Get it from get_mesh_components_detail, which reports each piece's size and bounding box.
//   @param min_triangle_points [int] - How many of a triangle's 3 points must be inside for it to count, 1-3 (default 3 = fully contained).
//   @param invert [bool] - Invert the spatial selection (default false).
//   @param include_triangle_ids [bool] - Also return the resolved triangle IDs (default false; they can be thousands). The returned array is what set_mesh_material_id scope='triangles' consumes.
//   @param max_triangle_ids [int] - Cap on how many IDs to return when include_triangle_ids is on (default 1000). When the cap truncates, triangle_ids_truncated is true — the count in triangles_selected is still the real one.
//   @returns { actor_name, region, triangles_selected: int, mesh_triangle_count: int, is_empty: bool, triangle_ids: [int], triangle_ids_truncated: bool }
TSharedPtr<FJsonObject> HandleSelectMeshTriangles(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UDynamicMesh* DynMesh = ResolveDynamicMeshForSpatial(Params, Error);
    if (!DynMesh) { return mcp_common::MakeErrorReply(Error); }

    FGeometryScriptMeshSelection Selection;
    FString Region;
    if (!ResolveMeshRegionSelection(DynMesh, Params, Selection, Region, Error))
        return mcp_common::MakeErrorReply(Error);

    const int32 TrianglesSelected = Selection.GetNumSelected();

    bool bIncludeTriangleIDs = false;
    Params->TryGetBoolField(TEXT("include_triangle_ids"), bIncludeTriangleIDs);

    int32 MaxTriangleIDs = 1000;
    {
        double MaxIDsDouble = 0.0;
        if (Params->TryGetNumberField(TEXT("max_triangle_ids"), MaxIDsDouble))
        {
            MaxTriangleIDs = FMath::Max(0, static_cast<int32>(MaxIDsDouble));
        }
    }

    TArray<TSharedPtr<FJsonValue>> TriangleIDsJson;
    bool bTruncated = false;
    if (bIncludeTriangleIDs && TrianglesSelected > 0)
    {
        TArray<int32> TriangleIDs;
        // ConvertToMeshIndexArray needs the underlying FDynamicMesh3; ProcessMesh is the
        // sanctioned read-only accessor (takes the mesh lock for us).
        DynMesh->ProcessMesh([&Selection, &TriangleIDs](const UE::Geometry::FDynamicMesh3& Mesh)
        {
            Selection.ConvertToMeshIndexArray(Mesh, TriangleIDs, EGeometryScriptIndexType::Triangle);
        });

        const int32 EmitCount = FMath::Min(TriangleIDs.Num(), MaxTriangleIDs);
        bTruncated = EmitCount < TriangleIDs.Num();
        TriangleIDsJson.Reserve(EmitCount);
        for (int32 Index = 0; Index < EmitCount; ++Index)
        {
            TriangleIDsJson.Add(MakeShared<FJsonValueNumber>(TriangleIDs[Index]));
        }
    }

    TSharedPtr<FJsonObject> ResultObj = GeometryScript_NewMeshData(DynMesh);
    ResultObj->SetStringField(TEXT("region"), Region);
    ResultObj->SetNumberField(TEXT("triangles_selected"), TrianglesSelected);
    ResultObj->SetNumberField(TEXT("mesh_triangle_count"), DynMesh->GetTriangleCount());
    ResultObj->SetBoolField(TEXT("is_empty"), Selection.IsEmpty());
    ResultObj->SetArrayField(TEXT("triangle_ids"), TriangleIDsJson);
    ResultObj->SetBoolField(TEXT("triangle_ids_truncated"), bTruncated);
    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:remap_material_ids - Renumber MaterialIDs on a DynamicMeshActor's live dynamic mesh. Two modes: pass from_material_id + to_material_id to merge one ID into another (UGeometryScriptLibrary_MeshMaterialFunctions::RemapMaterialIDs), or pass compact=true with the material list to close gaps left by deleted/merged IDs (CompactMaterialIDs) and get back the compacted list to hand to copy_dynamic_mesh_to_static_mesh. Gaps matter: MaterialIDs index into that list positionally, so an unused ID in the middle costs a wasted material slot on the baked asset. The materials list may be shorter than the IDs in use (or absent): the missing entries count as no material. When no triangle carries from_material_id, or from and to are the same ID, the mesh is left unchanged and the call succeeds with applied false, triangles_remapped 0 and a note. Otherwise applied is measured (remap: the triangles that carried from_material_id no longer do; compact: the engine reported no error) and the call fails when it is false, leaving the MaterialIDs as they were. One undo step restores the previous MaterialIDs.
//   @param actor_name [string!] - Label or name of the target DynamicMeshActor.
//   @param compact [bool] - Compact mode: renumber IDs to be contiguous from 0 (default false).
//   @param from_material_id [int] - Remap mode: the MaterialID to replace. Required unless compact is true.
//   @param to_material_id [int] - Remap mode: the MaterialID to replace it with. Required unless compact is true.
//   @param materials [array] - Compact mode: ordered UMaterialInterface content paths, one per current MaterialID. The compacted list is returned in compacted_materials, in the new ID order.
//   @param remove_duplicate_materials [bool] - Compact mode: also merge IDs that point at the SAME material asset (default false).
//   @returns { actor_name, mode: string, from_material_id: int, to_material_id: int, triangles_remapped?: int, max_material_id_after: int, distinct_material_ids_after: int, compacted_materials: [string], applied: bool, note?: string }
TSharedPtr<FJsonObject> HandleRemapMaterialIDs(const TSharedPtr<FJsonObject>& Params)
{
    bool bCompact = false;
    Params->TryGetBoolField(TEXT("compact"), bCompact);

    FString Error;
    FGeometryScriptMeshActor Target;
    if (!GeometryScript_ResolveLiveMesh(Params, Target, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }

    int32 FromMaterialID = -1;
    int32 ToMaterialID = -1;
    TArray<UMaterialInterface*> SourceMaterialList;

    if (bCompact)
    {
        // The material list is optional: without it the IDs are still compacted, there is just
        // no list to report back. With it, the response is directly usable as the `materials`
        // argument of copy_dynamic_mesh_to_static_mesh.
        const TArray<TSharedPtr<FJsonValue>>* MaterialsArray = nullptr;
        if (Params->TryGetArrayField(TEXT("materials"), MaterialsArray) && MaterialsArray)
        {
            for (int32 Index = 0; Index < MaterialsArray->Num(); ++Index)
            {
                const FString MaterialPath = (*MaterialsArray)[Index]->AsString();
                UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
                if (!Material)
                    return mcp_common::MakeErrorReply(FString::Printf(
                        TEXT("materials[%d]: material not found: %s"), Index, *MaterialPath));
                SourceMaterialList.Add(Material);
            }
        }
    }
    else
    {
        if (!Params->HasField(TEXT("from_material_id")) || !Params->HasField(TEXT("to_material_id")))
            return mcp_common::MakeErrorReply(
                TEXT("from_material_id and to_material_id are both required unless compact=true"));

        double FromDouble = 0.0, ToDouble = 0.0;
        Params->TryGetNumberField(TEXT("from_material_id"), FromDouble);
        Params->TryGetNumberField(TEXT("to_material_id"), ToDouble);
        FromMaterialID = static_cast<int32>(FromDouble);
        ToMaterialID = static_cast<int32>(ToDouble);
        if (FromMaterialID < 0 || ToMaterialID < 0)
            return mcp_common::MakeErrorReply(TEXT("from_material_id and to_material_id must both be >= 0"));
    }

    // Remap mode, before anything is written: a valid request with nothing to do (no triangle carries
    // from_material_id, or from and to are the same ID) leaves the mesh alone and succeeds with applied false.
    int32 TrianglesRemapped = 0;
    FString NothingToDoNote;
    if (!bCompact)
    {
        TrianglesRemapped = GeometryScript_CountMaterialID(Target.Mesh, FromMaterialID);
        if (TrianglesRemapped == 0)
        {
            NothingToDoNote = FString::Printf(TEXT("no triangle carries MaterialID %d: the mesh is unchanged"), FromMaterialID);
        }
        else if (FromMaterialID == ToMaterialID)
        {
            TrianglesRemapped = 0;
            NothingToDoNote = FString::Printf(TEXT("from_material_id and to_material_id are both %d: the mesh is unchanged"), FromMaterialID);
        }
    }

    TArray<TSharedPtr<FJsonValue>> CompactedMaterialsJson;
    bool bApplied = false;
    FString NotAppliedReason;
    if (NothingToDoNote.IsEmpty())
    {
        const FScopedTransaction Transaction(LOCTEXT("RemapMaterialIDs", "MCP: Remap Material IDs"));
        Target.Actor->Modify();
        FGeometryScriptMeshEdit MeshEdit(Target.Mesh);

        UGeometryScriptLibrary_MeshMaterialFunctions::EnableMaterialIDs(Target.Mesh, /*Debug*/ nullptr);

        if (bCompact)
        {
            bool bRemoveDuplicateMaterials = false;
            Params->TryGetBoolField(TEXT("remove_duplicate_materials"), bRemoveDuplicateMaterials);

            // CompactMaterialIDs indexes its per-ID tables by the size of the material list, so a MaterialID in use
            // at or past the end of the list reads out of bounds and crashes the editor. The list is padded with
            // null entries up to the highest ID, which is what the engine means for an ID without a material.
            const bool bMaterialsGiven = SourceMaterialList.Num() > 0;
            bool bHasMaterialIDs = false;
            const int32 MaxBefore = UGeometryScriptLibrary_MeshMaterialFunctions::GetMaxMaterialID(Target.Mesh, bHasMaterialIDs);
            if (SourceMaterialList.Num() <= MaxBefore)
            {
                SourceMaterialList.SetNumZeroed(MaxBefore + 1);
            }

            UGeometryScriptDebug* Debug = GeometryScript_NewDebug();
            TArray<UMaterialInterface*> CompactedMaterialList;
            UGeometryScriptLibrary_MeshMaterialFunctions::CompactMaterialIDs(
                Target.Mesh, SourceMaterialList, CompactedMaterialList, bRemoveDuplicateMaterials, Debug);
            NotAppliedReason = GeometryScript_FirstError(Debug);
            bApplied = NotAppliedReason.IsEmpty();

            // Without a material list from the caller there is no list to hand back.
            if (bMaterialsGiven)
            {
                for (UMaterialInterface* Material : CompactedMaterialList)
                {
                    CompactedMaterialsJson.Add(MakeShared<FJsonValueString>(
                        Material ? Material->GetPathName() : FString()));
                }
            }
        }
        else
        {
            // Measured: none of the triangles counted above carries from_material_id afterwards.
            UGeometryScriptLibrary_MeshMaterialFunctions::RemapMaterialIDs(
                Target.Mesh, FromMaterialID, ToMaterialID, /*Debug*/ nullptr);
            const int32 FromLeft = GeometryScript_CountMaterialID(Target.Mesh, FromMaterialID);
            bApplied = FromLeft == 0;
            NotAppliedReason = FString::Printf(TEXT("%d triangles still carry MaterialID %d after the remap"), FromLeft, FromMaterialID);
        }
        if (bApplied)
        {
            MeshEdit.Commit();
        }
        else
        {
            MeshEdit.Revert();
        }

        Target.Component->NotifyMeshUpdated();
    }

    int32 MaxAfter = 0;
    int32 DistinctAfter = 0;
    GeometryScript_ReadMaterialIDs(Target.Mesh, MaxAfter, DistinctAfter);

    TSharedPtr<FJsonObject> ResultObj = GeometryScript_NewActorData(Target.Actor);
    ResultObj->SetStringField(TEXT("mode"), bCompact ? TEXT("compact") : TEXT("remap"));
    ResultObj->SetNumberField(TEXT("from_material_id"), FromMaterialID);
    ResultObj->SetNumberField(TEXT("to_material_id"), ToMaterialID);
    if (!bCompact)
    {
        ResultObj->SetNumberField(TEXT("triangles_remapped"), TrianglesRemapped);
    }
    ResultObj->SetNumberField(TEXT("max_material_id_after"), MaxAfter);
    ResultObj->SetNumberField(TEXT("distinct_material_ids_after"), DistinctAfter);
    ResultObj->SetArrayField(TEXT("compacted_materials"), CompactedMaterialsJson);
    ResultObj->SetBoolField(TEXT("applied"), bApplied);
    if (!NothingToDoNote.IsEmpty())
    {
        ResultObj->SetStringField(TEXT("note"), NothingToDoNote);
        return mcp_common::MakeOkReply(ResultObj);
    }
    return GeometryScript_OutcomeReply(ResultObj, bApplied, TEXT("remap_material_ids"), TEXT("applied"), NotAppliedReason);
}

// ============================================================================
// Mesh map -- "WHAT is this piece?"
//
// WHY: the MaterialID block above can paint any set of triangles, but nothing says what a
// given set of triangles IS. A histogram says WHERE something is, never what it is, so a label
// taken from outside the measurement (the order the parts were listed in, an intuition about
// where a part sits) can be false without a single number disagreeing -- the totals add up
// under every false hypothesis.
//
// A typical false premise: "a box is 12 triangles". A SUBDIVIDED box can be 48, so every derived
// map silently desyncs from the first such block onwards. get_mesh_components_detail reports the
// REAL triangle count per piece, which refutes that in one call.
// ============================================================================

// MCP:get_mesh_components_detail - Enumerate the connected components (physical pieces) of a DynamicMeshActor's live dynamic mesh via UE::Geometry::FMeshConnectedComponents, reporting for EACH island its triangle count, its local-space bounding box, its triangle_id range and which MaterialIDs it carries. Read-only (unlike the engine's SplitMeshByComponents, which consumes the mesh). This is the map of a mesh in ONE call, and the tool to reach for BEFORE painting anything: get_mesh_material_ids says how many triangles carry each material, and this says what those triangles physically ARE. HONEST GATE: it names nothing — it gives you sizes and positions, and which island is which part is still a human call; what it prevents is deriving that from a triangle-count assumption.
//   @param actor_name [string!] - Label or name of the target DynamicMeshActor.
//   @param max_components [int] - Cap on how many islands to return (default 64). components_truncated says whether the cap cut the list; component_count is always the real total.
//   @param include_triangle_ids [bool] - Also return each island's full triangle ID list (default false; use the reported id_min/id_max range instead unless the island is non-contiguous).
//   @returns { actor_name, component_count: int, components_truncated: bool, components: [ { index, triangle_count, id_min, id_max, id_contiguous: bool, material_ids: [int], bounds: { min_x, min_y, min_z, max_x, max_y, max_z, center_x, center_y, center_z, size_x, size_y, size_z }, triangle_ids: [int] } ] }
TSharedPtr<FJsonObject> HandleGetMeshComponentsDetail(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UDynamicMesh* DynMesh = ResolveDynamicMeshForSpatial(Params, Error);
    if (!DynMesh) { return mcp_common::MakeErrorReply(Error); }

    int32 MaxComponents = 64;
    {
        double MaxComponentsDouble = 0.0;
        if (Params->TryGetNumberField(TEXT("max_components"), MaxComponentsDouble))
        {
            MaxComponents = FMath::Max(1, static_cast<int32>(MaxComponentsDouble));
        }
    }

    bool bIncludeTriangleIDs = false;
    Params->TryGetBoolField(TEXT("include_triangle_ids"), bIncludeTriangleIDs);

    // Everything is gathered inside a single ProcessMesh (one read lock): islands, their bounds,
    // and their MaterialIDs. Doing it per-island through GeometryScript would re-walk the mesh N times.
    int32 ComponentCount = 0;
    TArray<TSharedPtr<FJsonValue>> ComponentsJson;

    DynMesh->ProcessMesh([&](const UE::Geometry::FDynamicMesh3& Mesh)
    {
        UE::Geometry::FMeshConnectedComponents Components(&Mesh);
        Components.FindConnectedTriangles();
        ComponentCount = Components.Components.Num();

        const UE::Geometry::FDynamicMeshMaterialAttribute* MaterialAttrib =
            Mesh.HasAttributes() ? Mesh.Attributes()->GetMaterialID() : nullptr;

        const int32 EmitCount = FMath::Min(ComponentCount, MaxComponents);
        for (int32 ComponentIndex = 0; ComponentIndex < EmitCount; ++ComponentIndex)
        {
            const TArray<int32>& Triangles = Components.Components[ComponentIndex].Indices;

            FBox Bounds(ForceInit);
            int32 IdMin = MAX_int32;
            int32 IdMax = -1;
            TSet<int32> MaterialIDs;

            for (const int32 TriangleID : Triangles)
            {
                IdMin = FMath::Min(IdMin, TriangleID);
                IdMax = FMath::Max(IdMax, TriangleID);

                if (MaterialAttrib)
                {
                    MaterialIDs.Add(MaterialAttrib->GetValue(TriangleID));
                }

                const UE::Geometry::FIndex3i Triangle = Mesh.GetTriangle(TriangleID);
                Bounds += (FVector)Mesh.GetVertex(Triangle.A);
                Bounds += (FVector)Mesh.GetVertex(Triangle.B);
                Bounds += (FVector)Mesh.GetVertex(Triangle.C);
            }

            // A contiguous range means id_min..id_max IS the piece, so it can be fed straight to
            // set_mesh_material_id scope='triangles' without asking for the full list.
            const bool bContiguous = (Triangles.Num() > 0) && ((IdMax - IdMin + 1) == Triangles.Num());

            TSharedPtr<FJsonObject> BoundsJson = GeometryScript_BoundsToJson(Bounds, Bounds.GetCenter(), Bounds.GetSize());

            TArray<int32> SortedMaterialIDs = MaterialIDs.Array();
            SortedMaterialIDs.Sort();
            TArray<TSharedPtr<FJsonValue>> MaterialIDsJson;
            for (const int32 MaterialID : SortedMaterialIDs)
            {
                MaterialIDsJson.Add(MakeShared<FJsonValueNumber>(MaterialID));
            }

            TArray<TSharedPtr<FJsonValue>> TriangleIDsJson;
            if (bIncludeTriangleIDs)
            {
                TriangleIDsJson.Reserve(Triangles.Num());
                for (const int32 TriangleID : Triangles)
                {
                    TriangleIDsJson.Add(MakeShared<FJsonValueNumber>(TriangleID));
                }
            }

            TSharedPtr<FJsonObject> ComponentJson = MakeShared<FJsonObject>();
            ComponentJson->SetNumberField(TEXT("index"), ComponentIndex);
            ComponentJson->SetNumberField(TEXT("triangle_count"), Triangles.Num());
            ComponentJson->SetNumberField(TEXT("id_min"), (IdMax >= 0) ? IdMin : -1);
            ComponentJson->SetNumberField(TEXT("id_max"), IdMax);
            ComponentJson->SetBoolField(TEXT("id_contiguous"), bContiguous);
            ComponentJson->SetArrayField(TEXT("material_ids"), MaterialIDsJson);
            ComponentJson->SetObjectField(TEXT("bounds"), BoundsJson);
            ComponentJson->SetArrayField(TEXT("triangle_ids"), TriangleIDsJson);
            ComponentsJson.Add(MakeShared<FJsonValueObject>(ComponentJson));
        }
    });

    UE_LOG(LogTemp, Verbose,
        TEXT("[MCP] get_mesh_components_detail: %d island(s), %d reported"),
        ComponentCount, ComponentsJson.Num());

    TSharedPtr<FJsonObject> ResultObj = GeometryScript_NewMeshData(DynMesh);
    ResultObj->SetNumberField(TEXT("component_count"), ComponentCount);
    ResultObj->SetBoolField(TEXT("components_truncated"), ComponentsJson.Num() < ComponentCount);
    ResultObj->SetArrayField(TEXT("components"), ComponentsJson);
    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:get_mesh_selection_bounds - Resolve a region into a selection and report its local-space BOUNDING BOX via UGeometryScriptLibrary_MeshSelectionQueryFunctions::GetMeshSelectionBoundingBox. Read-only. Takes exactly the same region parameters as select_mesh_triangles and set_mesh_material_id_in_region, so a region measured here is the same one painted there. This is what turns a bisection answer ("the piece is somewhere in X>0") into actual numbers: centre and size, which is what lets you tell a 150x100x40 slab from a 14x230x75 rail without opening the editor.
//   @param actor_name [string!] - Label or name of the target DynamicMeshActor.
//   @param region [string] - 'all' (default) | 'box' | 'sphere' | 'plane' | 'normal_angle' | 'material_id' | 'polygroup' | 'connected_component'.
//   @param component_index [int] - Island index when region='connected_component' (see get_mesh_components_detail).
//   @param center [array] - Local [x,y,z] centre. Used by box/sphere, and as the plane origin.
//   @param extent [array] - Local [x,y,z] HALF-size in cm. Required by region='box'.
//   @param radius [number] - Radius in cm. Required by region='sphere'.
//   @param normal [array] - [x,y,z] direction. Plane normal for region='plane', facing direction for 'normal_angle'. Default [0,0,1].
//   @param max_angle_deg [number] - Max angle from normal in degrees for region='normal_angle' (default 15).
//   @param source_material_id [int] - MaterialID to select when region='material_id'.
//   @param polygroup_id [int] - Polygroup to select when region='polygroup'.
//   @param polygroup_layer [int] - Optional extended polygroup layer index for region='polygroup'.
//   @param component_index [int] - Island index when region='connected_component'. Get it from get_mesh_components_detail, which reports each piece's size and bounding box.
//   @param min_triangle_points [int] - How many of a triangle's 3 points must be inside for it to count, 1-3 (default 3).
//   @param invert [bool] - Invert the spatial selection (default false).
//   @returns { actor_name, region, triangles_selected: int, is_empty: bool, bounds: { min_x, min_y, min_z, max_x, max_y, max_z, center_x, center_y, center_z, size_x, size_y, size_z } }
TSharedPtr<FJsonObject> HandleGetMeshSelectionBounds(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UDynamicMesh* DynMesh = ResolveDynamicMeshForSpatial(Params, Error);
    if (!DynMesh) { return mcp_common::MakeErrorReply(Error); }

    FGeometryScriptMeshSelection Selection;
    FString Region;
    if (!ResolveMeshRegionSelection(DynMesh, Params, Selection, Region, Error))
        return mcp_common::MakeErrorReply(Error);

    FBox SelectionBounds(ForceInit);
    bool bIsEmpty = true;
    UGeometryScriptLibrary_MeshSelectionQueryFunctions::GetMeshSelectionBoundingBox(
        DynMesh, Selection, SelectionBounds, bIsEmpty, /*Debug*/ nullptr);

    const FVector Center = bIsEmpty ? FVector::ZeroVector : SelectionBounds.GetCenter();
    const FVector Size = bIsEmpty ? FVector::ZeroVector : SelectionBounds.GetSize();
    TSharedPtr<FJsonObject> BoundsJson = GeometryScript_BoundsToJson(SelectionBounds, Center, Size);

    TSharedPtr<FJsonObject> ResultObj = GeometryScript_NewMeshData(DynMesh);
    ResultObj->SetStringField(TEXT("region"), Region);
    ResultObj->SetNumberField(TEXT("triangles_selected"), Selection.GetNumSelected());
    ResultObj->SetBoolField(TEXT("is_empty"), bIsEmpty);
    ResultObj->SetObjectField(TEXT("bounds"), BoundsJson);
    return mcp_common::MakeOkReply(ResultObj);
}

// ============================================================================
// UV unwrapping.
//
// WHY: meshes built by append_mesh_primitive + generate_mesh_uvs come out with a PLANAR projection,
// which is not an unwrap: a tileable material reads STRETCHED on every face that is not facing the
// projection plane. The engine ships three projections and two auto-unwrappers; these expose them.
// ============================================================================

// MCP:set_mesh_uvs_from_projection - Replace the UVs of one channel on a DynamicMeshActor's live dynamic mesh in the editor world with a box, plane or cylinder projection, optionally only inside a region. Box suits plank-and-post geometry (each face takes the axis it faces), plane only flat pieces, cylinder wrapped shapes. One undo step restores the previous UVs. Returns triangles_affected and applied. A uv_channel the mesh does not have is an engine error: applied is false, the call fails and the UVs are left as they were (add channels with set_num_uv_channels).
//   @param actor_name [string!] - Label or name of the target DynamicMeshActor.
//   @param projection [string] - 'box' (default) | 'plane' | 'cylinder'.
//   @param uv_channel [int] - UV channel to write (default 0).
//   @param location [array] - Local [x,y,z] origin of the projection shape (default the mesh origin).
//   @param rotation [array] - [pitch, yaw, roll] of the projection shape (default none). For 'plane', this is what chooses which way the plane faces.
//   @param scale [array] - [x,y,z] size of the projection shape (default [100,100,100]). THIS IS THE UV DENSITY KNOB: bigger scale = texture repeats fewer times. A tileable material that reads too big or too small is fixed here, not in the material.
//   @param min_island_tri_count [int] - box only: islands smaller than this are merged into a neighbour (default 2).
//   @param split_angle [number] - cylinder only: angle in degrees at which the wrap is split (default 45).
//   @param region [string] - Optional region to limit the projection to; same vocabulary as select_mesh_triangles (default 'all').
//   @param component_index [int] - Island index when region='connected_component'.
//   @param center [array] - Region parameter (box/sphere centre, plane origin).
//   @param extent [array] - Region parameter: box HALF-size.
//   @param radius [number] - Region parameter: sphere radius.
//   @param normal [array] - Region parameter: plane/normal_angle direction.
//   @param max_angle_deg [number] - Region parameter for region='normal_angle'.
//   @param source_material_id [int] - Region parameter for region='material_id'.
//   @param polygroup_id [int] - Region parameter for region='polygroup'.
//   @param polygroup_layer [int] - Region parameter for region='polygroup': extended polygroup layer index (default the mesh's default polygroup layer).
//   @param min_triangle_points [int] - Region parameter, 1-3.
//   @param invert [bool] - Select the complement of a box, sphere, plane or normal_angle region (default false); the other regions ignore it.
//   @returns { actor_name, projection, uv_channel, region, triangles_affected: int, uv_channel_count: int, applied: bool }
TSharedPtr<FJsonObject> HandleSetMeshUVsFromProjection(const TSharedPtr<FJsonObject>& Params)
{
    FString Projection;
    Params->TryGetStringField(TEXT("projection"), Projection);
    if (Projection.IsEmpty()) { Projection = TEXT("box"); }

    const bool bIsBox = Projection.Equals(TEXT("box"), ESearchCase::IgnoreCase);
    const bool bIsPlane = Projection.Equals(TEXT("plane"), ESearchCase::IgnoreCase)
                       || Projection.Equals(TEXT("planar"), ESearchCase::IgnoreCase);
    const bool bIsCylinder = Projection.Equals(TEXT("cylinder"), ESearchCase::IgnoreCase);
    if (!bIsBox && !bIsPlane && !bIsCylinder)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Unknown projection '%s' (box|plane|cylinder)"), *Projection));
    }

    int32 UVChannel = 0;
    if (TSharedPtr<FJsonObject> Failure = GeometryScript_ReadOptionalUVChannel(Params, UVChannel))
        return Failure;

    FString Error;
    FGeometryScriptMeshActor Target;
    if (!GeometryScript_ResolveLiveMesh(Params, Target, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }

    // The projection shape. Scale is the UV density knob, so it defaults to something usable
    // (100 uu) rather than to 1, which would tile a 200 uu plank a hundred times.
    const FVector ShapeLocation = Params->HasField(TEXT("location"))
        ? mcp_common::ReadJsonVector(Params, TEXT("location")) : FVector::ZeroVector;
    const FRotator ShapeRotation = Params->HasField(TEXT("rotation"))
        ? mcp_common::ReadJsonRotator(Params, TEXT("rotation")) : FRotator::ZeroRotator;
    FVector ShapeScale = FVector(100.0, 100.0, 100.0);
    if (Params->HasField(TEXT("scale")))
    {
        ShapeScale = mcp_common::ReadJsonVector(Params, TEXT("scale"));
        if (ShapeScale.IsNearlyZero())
            return mcp_common::MakeErrorReply(TEXT("scale must be non-zero: it is the projection shape's size, and a zero shape produces degenerate UVs"));
    }
    const FTransform ShapeTransform(ShapeRotation, ShapeLocation, ShapeScale);

    FGeometryScriptMeshSelection Selection;
    FString Region;
    if (!ResolveMeshRegionSelection(Target.Mesh, Params, Selection, Region, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }
    // An empty selection means "whole mesh" to the engine, which is what region='all' resolves to.
    const int32 TrianglesAffected = Region.Equals(TEXT("all"), ESearchCase::IgnoreCase)
        ? Target.Mesh->GetTriangleCount() : Selection.GetNumSelected();

    const FScopedTransaction Transaction(LOCTEXT("SetMeshUVsFromProjection", "MCP: Set Mesh UVs From Projection"));
    Target.Actor->Modify();
    FGeometryScriptMeshEdit MeshEdit(Target.Mesh);

    // The projections report a UV channel the mesh does not have only as an error in Debug, writing nothing: the
    // mesh is then put back as it was and the call fails with the engine's message.
    UGeometryScriptDebug* Debug = GeometryScript_NewDebug();
    if (bIsBox)
    {
        int32 MinIslandTriCount = 2;
        double MinIslandTriCountDouble = 0.0;
        if (Params->TryGetNumberField(TEXT("min_island_tri_count"), MinIslandTriCountDouble))
        {
            MinIslandTriCount = FMath::Max(1, static_cast<int32>(MinIslandTriCountDouble));
        }
        UGeometryScriptLibrary_MeshUVFunctions::SetMeshUVsFromBoxProjection(
            Target.Mesh, UVChannel, ShapeTransform, Selection, MinIslandTriCount, Debug);
    }
    else if (bIsCylinder)
    {
        double SplitAngle = 45.0;
        Params->TryGetNumberField(TEXT("split_angle"), SplitAngle);
        UGeometryScriptLibrary_MeshUVFunctions::SetMeshUVsFromCylinderProjection(
            Target.Mesh, UVChannel, ShapeTransform, Selection, static_cast<float>(SplitAngle), Debug);
    }
    else
    {
        UGeometryScriptLibrary_MeshUVFunctions::SetMeshUVsFromPlanarProjection(
            Target.Mesh, UVChannel, ShapeTransform, Selection, Debug);
    }
    const FString ProjectionError = GeometryScript_FirstError(Debug);
    if (ProjectionError.IsEmpty())
    {
        MeshEdit.Commit();
    }
    else
    {
        MeshEdit.Revert();
    }

    Target.Component->NotifyMeshUpdated();

    const int32 UVChannelCount = UGeometryScriptLibrary_MeshQueryFunctions::GetNumUVSets(Target.Mesh);

    UE_LOG(LogTemp, Verbose,
        TEXT("[MCP] set_mesh_uvs_from_projection: actor='%s' projection=%s channel=%d region=%s tris=%d"),
        *Target.Actor->GetActorLabel(), *Projection, UVChannel, *Region, TrianglesAffected);

    TSharedPtr<FJsonObject> ResultObj = GeometryScript_NewActorData(Target.Actor);
    ResultObj->SetStringField(TEXT("projection"), Projection);
    ResultObj->SetNumberField(TEXT("uv_channel"), UVChannel);
    ResultObj->SetStringField(TEXT("region"), Region);
    ResultObj->SetNumberField(TEXT("triangles_affected"), TrianglesAffected);
    ResultObj->SetNumberField(TEXT("uv_channel_count"), UVChannelCount);
    ResultObj->SetBoolField(TEXT("applied"), ProjectionError.IsEmpty() && TrianglesAffected > 0);
    if (!ProjectionError.IsEmpty())
    {
        return GeometryScript_OutcomeReply(ResultObj, false, TEXT("set_mesh_uvs_from_projection"), TEXT("applied"), ProjectionError);
    }
    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:auto_generate_mesh_uvs - Compute a real UV UNWRAP for the whole mesh via UGeometryScriptLibrary_MeshUVFunctions::AutoGeneratePatchBuilderMeshUVs or AutoGenerateXAtlasMeshUVs. This is what sits behind the editor's "Auto UV" button. Use it when a projection is not enough (organic or mixed-orientation geometry); for plank-and-post shapes set_mesh_uvs_from_projection with box is usually cleaner and far cheaper. Both methods PACK the result into standard UV space by default, so island layout is handled too. SLOW on dense meshes: this is a solver, not a projection. A UV channel the mesh does not have, or an empty mesh, is an error; applied is false, and the call fails, leaving the UVs as they were, when the solver reports an error (xatlas needs a compact mesh: run compact_mesh first). One undo step restores the previous UVs.
//   @param actor_name [string!] - Label or name of the target DynamicMeshActor.
//   @param method [string] - 'patch_builder' (default, the editor's Auto UV) or 'xatlas'.
//   @param uv_channel [int] - UV channel to write (default 0).
//   @param initial_patch_count [int] - patch_builder: how many patches to seed (default 100). More patches = less distortion, more seams.
//   @param min_patch_size [int] - patch_builder: minimum triangles per patch (default 2).
//   @param merging_threshold [number] - patch_builder: patch merging metric threshold (default 1.5). Higher merges more aggressively, giving fewer islands.
//   @param max_angle_deg [number] - patch_builder: max angle in degrees between merged patches (default 45).
//   @param respect_polygroups [bool] - patch_builder: seed patches from the existing polygroups instead of curvature (default false). Useful right after append_mesh_primitive, where each appended face is already its own group.
//   @param auto_pack [bool] - Pack the resulting islands into standard UV space (default true).
//   @param max_iterations [int] - xatlas: solver iterations (default 2). Higher is slower and slightly tighter.
//   @returns { actor_name, method, uv_channel, triangle_count: int, uv_channel_count: int, applied: bool }
TSharedPtr<FJsonObject> HandleAutoGenerateMeshUVs(const TSharedPtr<FJsonObject>& Params)
{
    FString Method;
    Params->TryGetStringField(TEXT("method"), Method);
    if (Method.IsEmpty()) { Method = TEXT("patch_builder"); }

    const bool bIsPatchBuilder = Method.Equals(TEXT("patch_builder"), ESearchCase::IgnoreCase);
    const bool bIsXAtlas = Method.Equals(TEXT("xatlas"), ESearchCase::IgnoreCase);
    if (!bIsPatchBuilder && !bIsXAtlas)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Unknown method '%s' (patch_builder|xatlas)"), *Method));
    }

    int32 UVChannel = 0;
    if (TSharedPtr<FJsonObject> Failure = GeometryScript_ReadOptionalUVChannel(Params, UVChannel))
        return Failure;

    FString Error;
    FGeometryScriptMeshActor Target;
    if (!GeometryScript_ResolveLiveMesh(Params, Target, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }

    // The engine writes nothing to a channel the mesh lacks, and an empty mesh has nothing to unwrap.
    if (TSharedPtr<FJsonObject> Failure = GeometryScript_CheckUVChannelExists(Target.Mesh, UVChannel))
    {
        return Failure;
    }
    if (Target.Mesh->GetTriangleCount() == 0)
    {
        return mcp_common::MakeErrorReply(TEXT("The dynamic mesh is empty: there are no triangles to unwrap"));
    }

    bool bAutoPack = true;
    Params->TryGetBoolField(TEXT("auto_pack"), bAutoPack);

    const FScopedTransaction Transaction(LOCTEXT("AutoGenerateMeshUVs", "MCP: Auto Generate Mesh UVs"));
    Target.Actor->Modify();
    FGeometryScriptMeshEdit MeshEdit(Target.Mesh);

    // Both solvers report a refusal or a failed unwrap only through Debug.
    UGeometryScriptDebug* Debug = GeometryScript_NewDebug();
    if (bIsPatchBuilder)
    {
        FGeometryScriptPatchBuilderOptions Options; // engine defaults, then only what was asked for
        double InitialPatchCountDouble = 0.0;
        if (Params->TryGetNumberField(TEXT("initial_patch_count"), InitialPatchCountDouble))
        {
            Options.InitialPatchCount = FMath::Max(1, static_cast<int32>(InitialPatchCountDouble));
        }
        double MinPatchSizeDouble = 0.0;
        if (Params->TryGetNumberField(TEXT("min_patch_size"), MinPatchSizeDouble))
        {
            Options.MinPatchSize = FMath::Max(1, static_cast<int32>(MinPatchSizeDouble));
        }
        double MergingThreshold = 0.0;
        if (Params->TryGetNumberField(TEXT("merging_threshold"), MergingThreshold))
        {
            Options.PatchMergingMetricThresh = static_cast<float>(MergingThreshold);
        }
        double MaxAngleDeg = 0.0;
        if (Params->TryGetNumberField(TEXT("max_angle_deg"), MaxAngleDeg))
        {
            Options.PatchMergingAngleThresh = static_cast<float>(MaxAngleDeg);
        }
        bool bRespectPolygroups = false;
        if (Params->TryGetBoolField(TEXT("respect_polygroups"), bRespectPolygroups))
        {
            Options.bRespectInputGroups = bRespectPolygroups;
        }
        Options.bAutoPack = bAutoPack;

        UGeometryScriptLibrary_MeshUVFunctions::AutoGeneratePatchBuilderMeshUVs(
            Target.Mesh, UVChannel, Options, Debug);
    }
    else
    {
        FGeometryScriptXAtlasOptions Options;
        double MaxIterationsDouble = 0.0;
        if (Params->TryGetNumberField(TEXT("max_iterations"), MaxIterationsDouble))
        {
            Options.MaxIterations = FMath::Max(1, static_cast<int32>(MaxIterationsDouble));
        }
        UGeometryScriptLibrary_MeshUVFunctions::AutoGenerateXAtlasMeshUVs(
            Target.Mesh, UVChannel, Options, Debug);
    }
    const FString UnwrapError = GeometryScript_FirstError(Debug);
    if (UnwrapError.IsEmpty())
    {
        MeshEdit.Commit();
    }
    else
    {
        MeshEdit.Revert();
    }

    Target.Component->NotifyMeshUpdated();

    const int32 UVChannelCount = UGeometryScriptLibrary_MeshQueryFunctions::GetNumUVSets(Target.Mesh);

    UE_LOG(LogTemp, Verbose,
        TEXT("[MCP] auto_generate_mesh_uvs: actor='%s' method=%s channel=%d tris=%d"),
        *Target.Actor->GetActorLabel(), *Method, UVChannel, Target.Mesh->GetTriangleCount());

    TSharedPtr<FJsonObject> ResultObj = GeometryScript_NewActorData(Target.Actor);
    ResultObj->SetStringField(TEXT("method"), Method);
    ResultObj->SetNumberField(TEXT("uv_channel"), UVChannel);
    ResultObj->SetNumberField(TEXT("triangle_count"), Target.Mesh->GetTriangleCount());
    ResultObj->SetNumberField(TEXT("uv_channel_count"), UVChannelCount);
    ResultObj->SetBoolField(TEXT("applied"), UnwrapError.IsEmpty());
    return GeometryScript_OutcomeReply(ResultObj, UnwrapError.IsEmpty(), TEXT("auto_generate_mesh_uvs"), TEXT("applied"), UnwrapError);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("append_mesh_primitive"), &HandleAppendMeshPrimitive },
    { TEXT("append_mesh_to_dynamic_mesh"), &HandleAppendMeshToDynamicMesh },
    { TEXT("apply_adaptive_remesh"), &HandleApplyAdaptiveRemesh },
    { TEXT("apply_mesh_boolean"), &HandleApplyMeshBoolean },
    { TEXT("apply_mesh_boolean_actor_to_actor"), &HandleApplyMeshBooleanActorToActor },
    { TEXT("apply_mesh_morphology"), &HandleApplyMeshMorphology },
    { TEXT("apply_mesh_solidify"), &HandleApplyMeshSolidify },
    { TEXT("apply_uniform_remesh"), &HandleApplyUniformRemesh },
    { TEXT("auto_generate_mesh_uvs"), &HandleAutoGenerateMeshUVs },
    { TEXT("build_mesh_bvh"), &HandleBuildMeshBVH },
    { TEXT("clear_dynamic_mesh"), &HandleClearDynamicMesh },
    { TEXT("compact_mesh"), &HandleCompactMesh },
    { TEXT("copy_dynamic_mesh_to_static_mesh"), &HandleCopyDynamicMeshToStaticMesh },
    { TEXT("copy_mesh_uv_channel"), &HandleCopyMeshUVChannel },
    { TEXT("copy_static_mesh_to_dynamic_mesh"), &HandleCopyStaticMeshToDynamicMesh },
    { TEXT("delete_selected_triangles_from_mesh"), &HandleDeleteSelectedTrianglesFromMesh },
    { TEXT("delete_triangle_from_mesh"), &HandleDeleteTriangleFromMesh },
    { TEXT("find_nearest_point_on_mesh"), &HandleFindNearestPointOnMesh },
    { TEXT("find_nearest_ray_intersection_with_mesh"), &HandleFindNearestRayIntersectionWithMesh },
    { TEXT("generate_mesh_uvs"), &HandleGenerateMeshUVs },
    { TEXT("get_dynamic_mesh_info"), &HandleGetDynamicMeshInfo },
    { TEXT("get_is_dense_mesh"), &HandleGetIsDenseMesh },
    { TEXT("get_mesh_bounding_box"), &HandleGetMeshBoundingBox },
    { TEXT("get_mesh_components_detail"), &HandleGetMeshComponentsDetail },
    { TEXT("get_mesh_connected_components"), &HandleGetMeshConnectedComponents },
    { TEXT("get_mesh_has_attribute_set"), &HandleGetMeshHasAttributeSet },
    { TEXT("get_mesh_is_closed"), &HandleGetMeshIsClosed },
    { TEXT("get_mesh_material_ids"), &HandleGetMeshMaterialIDs },
    { TEXT("get_mesh_open_border_loops"), &HandleGetMeshOpenBorderLoops },
    { TEXT("get_mesh_selection_bounds"), &HandleGetMeshSelectionBounds },
    { TEXT("get_mesh_triangle_count"), &HandleGetMeshTriangleCount },
    { TEXT("get_mesh_uv_area"), &HandleGetMeshUVArea },
    { TEXT("get_mesh_uv_channel_count"), &HandleGetMeshUVChannelCount },
    { TEXT("get_mesh_vertex_count"), &HandleGetMeshVertexCount },
    { TEXT("get_mesh_volume_area"), &HandleGetMeshVolumeArea },
    { TEXT("get_mesh_volume_area_center"), &HandleGetMeshVolumeAreaCenter },
    { TEXT("get_num_connected_islands"), &HandleGetNumConnectedIslands },
    { TEXT("get_num_extended_polygroup_layers"), &HandleGetNumExtendedPolygroupLayers },
    { TEXT("get_num_open_border_edges"), &HandleGetNumOpenBorderEdges },
    { TEXT("get_num_uv_islands"), &HandleGetNumUVIslands },
    { TEXT("is_bvh_valid_for_mesh"), &HandleIsBVHValidForMesh },
    { TEXT("is_point_inside_mesh"), &HandleIsPointInsideMesh },
    { TEXT("rebuild_mesh_bvh"), &HandleRebuildMeshBVH },
    { TEXT("recompute_mesh_normals"), &HandleRecomputeMeshNormals },
    { TEXT("recompute_mesh_uvs"), &HandleRecomputeMeshUVs },
    { TEXT("remap_material_ids"), &HandleRemapMaterialIDs },
    { TEXT("remove_dynamic_mesh_actor"), &HandleRemoveDynamicMeshActor },
    { TEXT("remove_unused_vertices"), &HandleRemoveUnusedVertices },
    { TEXT("repack_mesh_uvs"), &HandleRepackMeshUVs },
    { TEXT("repair_mesh_self_intersections"), &HandleRepairMeshSelfIntersections },
    { TEXT("rotate_dynamic_mesh"), &HandleRotateDynamicMesh },
    { TEXT("rotate_mesh_uvs"), &HandleRotateMeshUVs },
    { TEXT("scale_dynamic_mesh"), &HandleScaleDynamicMesh },
    { TEXT("scale_mesh_uvs"), &HandleScaleMeshUVs },
    { TEXT("select_mesh_triangles"), &HandleSelectMeshTriangles },
    { TEXT("set_dynamic_mesh_material"), &HandleSetDynamicMeshMaterial },
    { TEXT("set_mesh_material_id"), &HandleSetMeshMaterialID },
    { TEXT("set_mesh_material_id_in_region"), &HandleSetMeshMaterialIDInRegion },
    { TEXT("set_mesh_uv_element_position"), &HandleSetMeshUVElementPosition },
    { TEXT("set_mesh_uvs_from_projection"), &HandleSetMeshUVsFromProjection },
    { TEXT("set_num_extended_polygroup_layers"), &HandleSetNumExtendedPolygroupLayers },
    { TEXT("set_num_uv_channels"), &HandleSetNumUVChannels },
    { TEXT("set_polygroup_material_id"), &HandleSetPolygroupMaterialID },
    { TEXT("simplify_mesh"), &HandleSimplifyMesh },
    { TEXT("spawn_dynamic_mesh_actor"), &HandleSpawnDynamicMeshActor },
    { TEXT("transform_dynamic_mesh"), &HandleTransformDynamicMesh },
    { TEXT("translate_dynamic_mesh"), &HandleTranslateDynamicMesh },
    { TEXT("translate_mesh_uvs"), &HandleTranslateMeshUVs },
    { TEXT("weld_mesh_edges"), &HandleWeldMeshEdges },
};
} // namespace UEAgentGeometryScriptCommands

#undef LOCTEXT_NAMESPACE
