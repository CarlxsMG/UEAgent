// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

#include "Editor.h"
#include "Engine/World.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "AssetRegistry/AssetRegistryModule.h"

// GeometryCollectionEngine (engine Runtime/Experimental): UGeometryCollection + conversion utils.
#include "GeometryCollection/GeometryCollectionObject.h"           // UGeometryCollection
#include "GeometryCollection/GeometryCollectionEngineConversion.h" // FGeometryCollectionEngineConversion::AppendStaticMesh

// Chaos (engine Runtime/Experimental): FGeometryCollection data + simulation types (enums).
#include "GeometryCollection/GeometryCollection.h"                 // FGeometryCollection, TransformGroup
#include "GeometryCollection/GeometryCollectionSimulationTypes.h"  // EDamageModelTypeEnum

// FractureEngine (plugin Fracture): fracturing + clustering.
#include "FractureEngineFracturing.h"  // FFractureEngineFracturing::VoronoiFracture
#include "FractureEngineClustering.h"  // FFractureEngineClustering::AutoCluster

// Dataflow selection (for Voronoi fracture transform selection).
#include "Dataflow/DataflowSelection.h"  // FDataflowTransformSelection

// Proximity + hierarchy (Chaos), connection-graph enum (ChaosSolverEngine).
#include "GeometryCollection/GeometryCollectionProximityUtility.h"   // FGeometryCollectionProximityUtility
#include "GeometryCollection/Facades/CollectionHierarchyFacade.h"    // Chaos::Facades::FCollectionHierarchyFacade
#include "GeometryCollection/TransformCollection.h"                  // BoneName/Parent arrays
// EClusterConnectionTypeEnum is exposed transitively via GeometryCollectionObject.h
// (UGeometryCollection::ClusterConnectionType is of this type) -> no extra Build.cs module needed.

// Engine types for loading source meshes.
#include "Engine/StaticMesh.h"

#define LOCTEXT_NAMESPACE "UEAgentChaos"

/**
 * Chaos / Destruction commands.
 *
 * Creates and manipulates UGeometryCollection assets: creation from static meshes and their geometry
 * sources, info, hierarchy and bone queries, Voronoi / plane-cut / slice fracturing, auto-clustering,
 * proximity and connection settings, and damage model and mass configuration.
 *
 * Modules: GeometryCollectionEngine (engine Runtime/Experimental) +
 * FractureEngine (plugin Fracture).
 *
 * Plugins required: GeometryCollectionPlugin, Fracture.
 */
namespace UEAgentChaosCommands
{

// MCP-DOMAIN: Physics

namespace
{
    /** Load a UGeometryCollection by asset path (e.g. "/Game/Path/GC_Name.GC_Name"). */
    UGeometryCollection* LoadGeometryCollection(const FString& AssetPath)
    {
        UGeometryCollection* GC = LoadObject<UGeometryCollection>(nullptr, *AssetPath);
        if (!GC)
        {
            // Try with the asset path + ".GC_Name" suffix pattern.
            const FString DotName = AssetPath + TEXT(".") + FPackageName::GetShortName(AssetPath);
            GC = LoadObject<UGeometryCollection>(nullptr, *DotName);
        }
        return GC;
    }

    /** Generate random Voronoi sites within a bounding box. */
    TArray<FVector> GenerateRandomSites(const FBox& InBox, int32 NumSites, int32 Seed)
    {
        TArray<FVector> Sites;
        Sites.Reserve(NumSites);

        FRandomStream RandomStream(Seed);
        const FVector Extent = InBox.GetExtent();
        const FVector Center = InBox.GetCenter();

        for (int32 i = 0; i < NumSites; ++i)
        {
            const float X = Center.X + RandomStream.FRandRange(-Extent.X, Extent.X);
            const float Y = Center.Y + RandomStream.FRandRange(-Extent.Y, Extent.Y);
            const float Z = Center.Z + RandomStream.FRandRange(-Extent.Z, Extent.Z);
            Sites.Add(FVector(X, Y, Z));
        }
        return Sites;
    }

    /** Map a damage model string to the enum. */
    EDamageModelTypeEnum ParseDamageModel(const FString& InName)
    {
        const FString Lower = InName.ToLower();
        if (Lower == TEXT("threshold") || Lower == TEXT("userdefined"))
        {
            return EDamageModelTypeEnum::Chaos_Damage_Model_UserDefined_Damage_Threshold;
        }
        if (Lower == TEXT("simple") || Lower == TEXT("material"))
        {
            return EDamageModelTypeEnum::Chaos_Damage_Model_Material_Strength_And_Connectivity_DamageThreshold;
        }
        // Default to threshold model.
        return EDamageModelTypeEnum::Chaos_Damage_Model_UserDefined_Damage_Threshold;
    }

    /** Map a damage model enum to a readable string. */
    FString DamageModelToString(EDamageModelTypeEnum InModel)
    {
        switch (InModel)
        {
        case EDamageModelTypeEnum::Chaos_Damage_Model_UserDefined_Damage_Threshold:
            return TEXT("Threshold");
        case EDamageModelTypeEnum::Chaos_Damage_Model_Material_Strength_And_Connectivity_DamageThreshold:
            return TEXT("MaterialStrength");
        default:
            return TEXT("Unknown");
        }
    }

    /** Map a cluster connection type string to the enum (case-insensitive, ignores Chaos_ prefix). */
    EClusterConnectionTypeEnum ParseClusterConnectionType(const FString& InName)
    {
        FString Lower = InName.ToLower();
        Lower.ReplaceInline(TEXT("chaos_"), TEXT(""));
        if (Lower == TEXT("minimalspanningsubsetdelaunaytriangulation") || Lower == TEXT("minimalspanning"))
        {
            return EClusterConnectionTypeEnum::Chaos_MinimalSpanningSubsetDelaunayTriangulation;
        }
        if (Lower == TEXT("pointimplicitaugmentedwithminimaldelaunay"))
        {
            return EClusterConnectionTypeEnum::Chaos_PointImplicitAugmentedWithMinimalDelaunay;
        }
        if (Lower == TEXT("boundsoverlapfiltereddelaunaytriangulation") || Lower == TEXT("boundsoverlap"))
        {
            return EClusterConnectionTypeEnum::Chaos_BoundsOverlapFilteredDelaunayTriangulation;
        }
        if (Lower == TEXT("delaunaytriangulation") || Lower == TEXT("delaunay"))
        {
            return EClusterConnectionTypeEnum::Chaos_DelaunayTriangulation;
        }
        if (Lower == TEXT("none"))
        {
            return EClusterConnectionTypeEnum::Chaos_None;
        }
        // Default: PointImplicit.
        return EClusterConnectionTypeEnum::Chaos_PointImplicit;
    }

    /** Map a cluster connection enum to a readable string. */
    FString ClusterConnectionTypeToString(EClusterConnectionTypeEnum InType)
    {
        switch (InType)
        {
        case EClusterConnectionTypeEnum::Chaos_PointImplicit:
            return TEXT("PointImplicit");
        case EClusterConnectionTypeEnum::Chaos_DelaunayTriangulation:
            return TEXT("DelaunayTriangulation");
        case EClusterConnectionTypeEnum::Chaos_MinimalSpanningSubsetDelaunayTriangulation:
            return TEXT("MinimalSpanningSubsetDelaunayTriangulation");
        case EClusterConnectionTypeEnum::Chaos_PointImplicitAugmentedWithMinimalDelaunay:
            return TEXT("PointImplicitAugmentedWithMinimalDelaunay");
        case EClusterConnectionTypeEnum::Chaos_BoundsOverlapFilteredDelaunayTriangulation:
            return TEXT("BoundsOverlapFilteredDelaunayTriangulation");
        case EClusterConnectionTypeEnum::Chaos_None:
            return TEXT("None");
        default:
            return TEXT("Unknown");
        }
    }

    /** Reads "geometry_collection_path" and loads that collection; null on success, else the error reply. */
    TSharedPtr<FJsonObject> Chaos_ReadCollection(const TSharedPtr<FJsonObject>& Params, FString& OutPath, UGeometryCollection*& OutCollection)
    {
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("geometry_collection_path"), OutPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
        OutCollection = LoadGeometryCollection(OutPath);
        if (!OutCollection)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("GeometryCollection not found: %s"), *OutPath));
        }
        return nullptr;
    }

    /** Chaos_ReadCollection, then the collection's FGeometryCollection data, which must exist. */
    TSharedPtr<FJsonObject> Chaos_ReadCollectionData(const TSharedPtr<FJsonObject>& Params, FString& OutPath, UGeometryCollection*& OutCollection,
        TSharedPtr<FGeometryCollection, ESPMode::ThreadSafe>& OutData)
    {
        if (TSharedPtr<FJsonObject> Error = Chaos_ReadCollection(Params, OutPath, OutCollection))
        {
            return Error;
        }
        OutData = OutCollection->GetGeometryCollection();
        if (!OutData.IsValid())
        {
            return mcp_common::MakeErrorReply(TEXT("GeometryCollection has no internal FGeometryCollection data"));
        }
        return nullptr;
    }

    /** A reply holding geometry_collection_path, the first field of every collection command's reply. */
    TSharedPtr<FJsonObject> Chaos_StartReply(const FString& CollectionPath)
    {
        TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
        Reply->SetStringField(TEXT("geometry_collection_path"), CollectionPath);
        return Reply;
    }

    /**
     * The reply of a command that cuts or clusters a collection and then saves it. The engine calls
     * return nothing that counts what they made (the cutters return the index of the first new piece),
     * so what was made is measured: the transforms added, one per new piece or cluster. Success needs
     * at least one AND the package on disk; Result's own "success" carries the same verdict.
     * NothingMade says why a call that added nothing changed nothing.
     */
    TSharedPtr<FJsonObject> Chaos_EditReply(const TSharedPtr<FJsonObject>& Result, int32 TransformsAdded, bool bSaved,
        const FString& SaveError, const TCHAR* Command, const FString& NothingMade)
    {
        const bool bMadeSomething = TransformsAdded > 0;
        Result->SetBoolField(TEXT("success"), bMadeSomething && bSaved);
        mcp_common::AddSaveOutcome(Result, bSaved, SaveError);
        if (!bMadeSomething)
        {
            return mcp_common::MakeOutcomeReply(Result, false, FString::Printf(TEXT("%s: %s"), Command, *NothingMade));
        }
        return mcp_common::MakeSavedReply(Result, bSaved, Command);
    }

    /** "Rigid", "Clustered" or "None": the simulation type of transform Index ("None" also for an index out of range). */
    FString Chaos_SimulationTypeName(const TManagedArray<int32>& SimTypes, int32 Index)
    {
        if (!SimTypes.IsValidIndex(Index))
        {
            return TEXT("None");
        }
        switch (SimTypes[Index])
        {
        case FGeometryCollection::ESimulationTypes::FST_Rigid:
            return TEXT("Rigid");
        case FGeometryCollection::ESimulationTypes::FST_Clustered:
            return TEXT("Clustered");
        default:
            return TEXT("None");
        }
    }
}

// ---------------------------------------------------------------------------
// 1. create_geometry_collection
// ---------------------------------------------------------------------------
// MCP:create_geometry_collection - Creates a UGeometryCollection asset at package_path/name, optionally filled from one StaticMesh, and saves it. An existing collection there is RESET first (all geometry and sources discarded; created false, rebuilt true). A source_mesh_path that does not load as a StaticMesh is an error, checked before anything is touched. Succeeds only when the package reaches disk; a failed save leaves the collection in memory only (saved false, save_error).
//   @param package_path - Content folder for the asset, e.g. /Game/Chaos/Collections; a path without a leading slash is placed under /Game/
//   @param name - Name of the geometry collection asset, e.g. GC_Wall
//   @param source_mesh_path - Optional StaticMesh to convert and append, e.g. /Engine/BasicShapes/Cube
//   @returns { asset_path: string, geometry_collection_name: string, num_materials: int, num_transforms: int, created: bool, rebuilt: bool, saved: bool, save_error: string? }
TSharedPtr<FJsonObject> HandleCreateGeometryCollection(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("package_path"), PackagePath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    FString Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    // Normalize the package path: ensure it starts with /Game/ and doesn't end with /.
    if (!PackagePath.StartsWith(TEXT("/")))
    {
        PackagePath = TEXT("/Game/") + PackagePath;
    }
    if (PackagePath.EndsWith(TEXT("/")))
    {
        PackagePath.LeftChopInline(1);
    }

    const FString FullPath = PackagePath + TEXT("/") + Name;
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(FullPath)) { return Invalid; }

    // The optional source mesh is loaded before anything is touched: a path that does not load would
    // otherwise leave an empty collection behind, and reset an existing one for nothing.
    FString SourceMeshPath;
    UStaticMesh* SourceMesh = nullptr;
    if (Params->TryGetStringField(TEXT("source_mesh_path"), SourceMeshPath) && !SourceMeshPath.IsEmpty())
    {
        SourceMesh = LoadObject<UStaticMesh>(nullptr, *SourceMeshPath);
        if (!SourceMesh)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("source_mesh_path does not load as a StaticMesh: %s (nothing was created or reset)"), *SourceMeshPath));
        }
    }

    // Check if the asset already exists.
    UGeometryCollection* Existing = LoadObject<UGeometryCollection>(nullptr, *FullPath);
    if (!Existing)
    {
        const FString DotName = FullPath + TEXT(".") + Name;
        Existing = LoadObject<UGeometryCollection>(nullptr, *DotName);
    }

    const FScopedTransaction Transaction(LOCTEXT("CreateGeometryCollection", "MCP: Create Geometry Collection"));

    UGeometryCollection* GC = nullptr;
    UPackage* Package = nullptr;
    bool bRebuilt = false;

    if (Existing)
    {
        // Rebuild an existing asset fresh rather than returning it as-is.
        // fracture_geometry_collection mutates the collection in place and selects
        // ALL transforms, so re-fracturing an already-fractured collection grows it
        // geometrically (observed: a 153 MB GC that hung VoronoiFracture for >60s).
        // Resetting on create guarantees every create yields a single-piece collection,
        // so a later fracture always starts clean — and it self-heals a bloated asset.
        GC = Existing;
        Package = GC->GetOutermost();
        GC->Modify();
        GC->Reset();
        GC->GeometrySource.Empty();
        bRebuilt = true;
    }
    else
    {
        // Create the package and the UGeometryCollection asset.
        FString CreateError;
        GC = mcp_common::NewAssetInPackage<UGeometryCollection>(FullPath, Name, TEXT("Failed to create UGeometryCollection"), CreateError);
        if (!GC)
        {
            return mcp_common::MakeErrorReply(CreateError);
        }
        Package = GC->GetOutermost();
    }

    // Optionally append the source static mesh.
    if (SourceMesh)
    {
        // Build a materials array from the static mesh.
        TArray<UMaterialInterface*> MeshMaterials;
        const TArray<FStaticMaterial>& StaticMats = SourceMesh->GetStaticMaterials();
        for (int32 i = 0; i < StaticMats.Num(); ++i)
        {
            MeshMaterials.Add(StaticMats[i].MaterialInterface);
        }

        FGeometryCollectionEngineConversion::AppendStaticMesh(
            SourceMesh, MeshMaterials, FTransform::Identity, GC,
            /*bReindexMaterials=*/true, /*bAddInternalMaterials=*/true,
            /*bSplitComponents=*/false, /*bSetInternalFromMaterialIndex=*/false);

        // Also record the source in the editor-only GeometrySource array.
        FGeometryCollectionSource Src;
        Src.SourceGeometryObject = FSoftObjectPath(SourceMeshPath);
        Src.LocalTransform = FTransform::Identity;
        Src.SourceMaterial = MeshMaterials;
        GC->GeometrySource.Add(Src);
    }

    // Register the asset in the asset registry (only for a brand-new asset;
    // a rebuilt existing asset is already registered).
    if (!bRebuilt)
    {
        FAssetRegistryModule::AssetCreated(GC);
    }

    // Rebuild derived collection state (RootIndex, render data) before saving. Every other GC
    // mutator InvalidateCollection()s pre-save; the create path must too, else the serialize
    // harvests an unmapped "RootIndex" attribute name (non-deterministic-serialize LinkerSave ensure).
    GC->InvalidateCollection();

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Package, GC, SaveError);

    const TSharedPtr<FGeometryCollection, ESPMode::ThreadSafe> Collection = GC->GetGeometryCollection();
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("asset_path"), FullPath);
    Result->SetStringField(TEXT("geometry_collection_name"), Name);
    Result->SetNumberField(TEXT("num_materials"), GC->Materials.Num());
    Result->SetNumberField(TEXT("num_transforms"), Collection.IsValid() ? Collection->NumElements(FGeometryCollection::TransformGroup) : 0);
    Result->SetBoolField(TEXT("created"), !bRebuilt);
    Result->SetBoolField(TEXT("rebuilt"), bRebuilt);
    return mcp_common::MakeSavedReply(Result, bSaved, TEXT("create_geometry_collection"), SaveError);
}

// ---------------------------------------------------------------------------
// 2. get_geometry_collection_info
// ---------------------------------------------------------------------------
// MCP:get_geometry_collection_info - Reads a UGeometryCollection asset without changing it: material, source and damage-threshold counts, damage model ("Threshold", "MaterialStrength" or "Unknown"), mass settings and element counts (transforms, vertices, faces, geometry; 0 without collection data). Use get_geometry_collection_hierarchy for the bone tree.
//   @param geometry_collection_path - Full asset path, e.g. /Game/Chaos/GC_Wall
//   @returns { asset_path: string, num_materials: int, num_geometry_sources: int, damage_model: string, num_damage_thresholds: int, is_empty: bool, num_transforms: int, num_vertices: int, num_faces: int, num_geometry: int, mass: float, mass_as_density: bool, minimum_mass_clamp: float, density_from_physics_material: bool, has_physics_material: bool }
TSharedPtr<FJsonObject> HandleGetGeometryCollectionInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString GCPath;
    UGeometryCollection* GC = nullptr;
    if (TSharedPtr<FJsonObject> Error = Chaos_ReadCollection(Params, GCPath, GC))
    {
        return Error;
    }

    const bool bIsEmpty = GC->IsEmpty();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("asset_path"), GCPath);
    Result->SetNumberField(TEXT("num_materials"), GC->Materials.Num());
    Result->SetNumberField(TEXT("num_geometry_sources"), GC->GeometrySource.Num());
    Result->SetStringField(TEXT("damage_model"), DamageModelToString(GC->DamageModel));
    Result->SetNumberField(TEXT("num_damage_thresholds"), GC->DamageThreshold.Num());
    Result->SetBoolField(TEXT("is_empty"), bIsEmpty);

    // Mass / density configuration (top-level "Collisions" fields). Round-trips
    // the values written by set_geometry_collection_mass.
    Result->SetNumberField(TEXT("mass"), GC->Mass);
    Result->SetBoolField(TEXT("mass_as_density"), GC->bMassAsDensity);
    Result->SetNumberField(TEXT("minimum_mass_clamp"), GC->MinimumMassClamp);
    Result->SetBoolField(TEXT("density_from_physics_material"), GC->bDensityFromPhysicsMaterial);
    Result->SetBoolField(TEXT("has_physics_material"), GC->PhysicsMaterial != nullptr);

    // If the internal FGeometryCollection exists, report transform count.
    const TSharedPtr<FGeometryCollection, ESPMode::ThreadSafe> FGC = GC->GetGeometryCollection();
    if (FGC.IsValid())
    {
        Result->SetNumberField(TEXT("num_transforms"), FGC->NumElements(FGeometryCollection::TransformGroup));
        Result->SetNumberField(TEXT("num_vertices"), FGC->NumElements(FGeometryCollection::VerticesGroup));
        Result->SetNumberField(TEXT("num_faces"), FGC->NumElements(FGeometryCollection::FacesGroup));
        Result->SetNumberField(TEXT("num_geometry"), FGC->NumElements(FGeometryCollection::GeometryGroup));
    }
    else
    {
        Result->SetNumberField(TEXT("num_transforms"), 0);
        Result->SetNumberField(TEXT("num_vertices"), 0);
        Result->SetNumberField(TEXT("num_faces"), 0);
        Result->SetNumberField(TEXT("num_geometry"), 0);
    }

    return mcp_common::MakeOkReply(Result);
}

// ---------------------------------------------------------------------------
// 3. fracture_geometry_collection
// ---------------------------------------------------------------------------
// MCP:fracture_geometry_collection - Applies Voronoi fracturing to all transforms of a UGeometryCollection asset, using num_sites random sites in its bounds, then saves it. num_fractures is the number of new pieces, measured as the transforms the fracture added; the call fails when it added none (an empty collection, or a chance_to_fracture that skipped every piece) or when the save fails. Re-fracturing grows a collection geometrically and can stall the editor; fracture a fresh one from create_geometry_collection.
//   @param geometry_collection_path - Full asset path to the UGeometryCollection
//   @param num_sites - Number of random Voronoi sites to generate, minimum 1, default 20
//   @param random_seed - Seed for the random site positions and the per-transform fracture chance, default 0
//   @param chance_to_fracture - Chance from 0 to 1 that each selected transform is fractured, default 1
//   @param grout - Width of the gap left between cut pieces, in cm (default 0)
//   @param split_islands - Optional, split resulting islands, default false
//   @returns { geometry_collection_path: string, num_fractures: int, transform_count: int, num_sites: int, random_seed: int, success: bool, saved: bool, save_error: string? }
TSharedPtr<FJsonObject> HandleFractureGeometryCollection(const TSharedPtr<FJsonObject>& Params)
{
    FString GCPath;
    UGeometryCollection* GC = nullptr;
    TSharedPtr<FGeometryCollection, ESPMode::ThreadSafe> FGC;
    if (TSharedPtr<FJsonObject> Error = Chaos_ReadCollectionData(Params, GCPath, GC, FGC))
    {
        return Error;
    }

    // Parameters.
    int32 NumSites = 20;
    Params->TryGetNumberField(TEXT("num_sites"), NumSites);
    if (NumSites < 1) NumSites = 1;

    int32 RandomSeed = 0;
    Params->TryGetNumberField(TEXT("random_seed"), RandomSeed);

    float ChanceToFracture = 1.0f;
    if (Params->HasField(TEXT("chance_to_fracture")))
    {
        Params->TryGetNumberField(TEXT("chance_to_fracture"), ChanceToFracture);
    }

    float Grout = 0.0f;
    if (Params->HasField(TEXT("grout")))
    {
        Params->TryGetNumberField(TEXT("grout"), Grout);
    }

    // Island splitting; exposed as an explicit param (default OFF). NOTE: the
    // dominant cost of VoronoiFracture is the per-piece boolean cut, so cost
    // scales with the collection's existing transform/piece count. Re-fracturing
    // an already-fractured collection many times grows it geometrically and can
    // appear to hang — fracture a freshly-created collection instead.
    bool bSplitIslands = false;
    Params->TryGetBoolField(TEXT("split_islands"), bSplitIslands);

    const FScopedTransaction Transaction(LOCTEXT("FractureGeometryCollection", "MCP: Fracture Geometry Collection"));
    GC->Modify();

    // Compute bounding box for site generation.
    const FBoxSphereBounds Bounds = FGC->GetBoundingBox();
    const FBox BBox = Bounds.GetBox();

    // Generate random Voronoi sites.
    TArray<FVector> Sites = GenerateRandomSites(BBox, NumSites, RandomSeed);

    // Build a transform selection covering all transforms.
    FDataflowTransformSelection TransformSelection;
    TransformSelection.InitializeFromCollection(*FGC, true);

    const int32 NumTransformsBefore = FGC->NumElements(FGeometryCollection::TransformGroup);

    // Call VoronoiFracture with default noise parameters.
    FFractureEngineFracturing::VoronoiFracture(
        *FGC,
        TransformSelection,
        Sites,
        FTransform::Identity,
        RandomSeed,
        ChanceToFracture,
        /*InSplitIslands=*/bSplitIslands,
        Grout,
        /*InAmplitude=*/0.0f,
        /*InFrequency=*/0.0f,
        /*InPersistence=*/0.0f,
        /*InLacunarity=*/0.0f,
        /*InOctaveNumber=*/1,
        /*InPointSpacing=*/1.0f,
        /*InAddSamplesForCollision=*/false,
        /*InCollisionSampleSpacing=*/0.0f);

    // Mark simulation data dirty so it gets regenerated.
    GC->InvalidateCollection();

    // Save the modified asset.
    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(GC->GetOutermost(), GC, SaveError);

    const int32 NumTransformsAfter = FGC->NumElements(FGeometryCollection::TransformGroup);
    TSharedPtr<FJsonObject> Result = Chaos_StartReply(GCPath);
    Result->SetNumberField(TEXT("num_fractures"), NumTransformsAfter - NumTransformsBefore);
    Result->SetNumberField(TEXT("transform_count"), NumTransformsAfter);
    Result->SetNumberField(TEXT("num_sites"), NumSites);
    Result->SetNumberField(TEXT("random_seed"), RandomSeed);
    return Chaos_EditReply(Result, NumTransformsAfter - NumTransformsBefore, bSaved, SaveError, TEXT("fracture_geometry_collection"),
        FString::Printf(TEXT("no fracture was made (the collection kept its %d transforms): an empty collection, or a ")
            TEXT("chance_to_fracture that skipped every piece, fractures nothing."), NumTransformsBefore));
}

// ---------------------------------------------------------------------------
// 4. cluster_geometry_collection
// ---------------------------------------------------------------------------
// MCP:cluster_geometry_collection - Runs auto-clustering on all transforms of a UGeometryCollection asset, targeting site_count clusters ("Small" doubles it, "Large" halves it, any other method keeps it) with connectivity enforced, then saves it. Returns the cluster count used and the transform counts before and after; the call fails when no cluster was added (a collection that is not fractured into several pieces has nothing to group) or when the save fails.
//   @param geometry_collection_path - Full asset path to the UGeometryCollection
//   @param cluster_size_method - Small, Medium, or Large, default Medium
//   @param site_count - Target cluster count before cluster_size_method scales it, minimum 1, default 10
//   @returns { geometry_collection_path: string, cluster_size_method: string, site_count: int, num_transforms_before: int, num_transforms_after: int, success: bool, saved: bool, save_error: string? }
TSharedPtr<FJsonObject> HandleClusterGeometryCollection(const TSharedPtr<FJsonObject>& Params)
{
    FString GCPath;
    UGeometryCollection* GC = nullptr;
    TSharedPtr<FGeometryCollection, ESPMode::ThreadSafe> FGC;
    if (TSharedPtr<FJsonObject> Error = Chaos_ReadCollectionData(Params, GCPath, GC, FGC))
    {
        return Error;
    }

    // Parse cluster size method.
    FString MethodStr = TEXT("Medium");
    Params->TryGetStringField(TEXT("cluster_size_method"), MethodStr);

    int32 SiteCount = 10;
    Params->TryGetNumberField(TEXT("site_count"), SiteCount);
    if (SiteCount < 1) SiteCount = 1;

    const FScopedTransaction Transaction(LOCTEXT("ClusterGeometryCollection", "MCP: Cluster Geometry Collection"));
    GC->Modify();

    // Build the bone indices array: all leaf-level transform indices.
    const int32 NumTransforms = FGC->NumElements(FGeometryCollection::TransformGroup);
    TArray<int32> BoneIndices;
    BoneIndices.Reserve(NumTransforms);
    for (int32 i = 0; i < NumTransforms; ++i)
    {
        BoneIndices.Add(i);
    }

    // Map the user-friendly "Small"/"Medium"/"Large" to EFractureEngineClusterSizeMethod + parameters.
    EFractureEngineClusterSizeMethod ClusterMethod;
    uint32 EffectiveSiteCount = static_cast<uint32>(SiteCount);
    float SiteCountFraction = 0.5f;
    float SiteSize = 50.0f;

    const FString Lower = MethodStr.ToLower();
    if (Lower == TEXT("small"))
    {
        // Many small clusters.
        ClusterMethod = EFractureEngineClusterSizeMethod::ByNumber;
        EffectiveSiteCount = static_cast<uint32>(SiteCount * 2);
    }
    else if (Lower == TEXT("large"))
    {
        // Few large clusters.
        ClusterMethod = EFractureEngineClusterSizeMethod::ByNumber;
        EffectiveSiteCount = FMath::Max(1u, static_cast<uint32>(SiteCount / 2));
    }
    else
    {
        // "Medium" or any unrecognized value -> ByNumber with the given count.
        ClusterMethod = EFractureEngineClusterSizeMethod::ByNumber;
        EffectiveSiteCount = static_cast<uint32>(SiteCount);
    }

    FFractureEngineClustering::AutoCluster(
        *FGC,
        BoneIndices,
        ClusterMethod,
        EffectiveSiteCount,
        SiteCountFraction,
        SiteSize,
        /*bEnforceConnectivity=*/true,
        /*bAvoidIsolated=*/true,
        /*bEnforceSiteParameters=*/false);

    // Mark simulation data dirty.
    GC->InvalidateCollection();

    // Save the modified asset.
    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(GC->GetOutermost(), GC, SaveError);

    // AutoCluster returns nothing; every cluster it makes is a new transform.
    const int32 NumTransformsAfter = FGC->NumElements(FGeometryCollection::TransformGroup);

    TSharedPtr<FJsonObject> Result = Chaos_StartReply(GCPath);
    Result->SetStringField(TEXT("cluster_size_method"), MethodStr);
    Result->SetNumberField(TEXT("site_count"), EffectiveSiteCount);
    Result->SetNumberField(TEXT("num_transforms_before"), NumTransforms);
    Result->SetNumberField(TEXT("num_transforms_after"), NumTransformsAfter);
    return Chaos_EditReply(Result, NumTransformsAfter - NumTransforms, bSaved, SaveError, TEXT("cluster_geometry_collection"),
        FString::Printf(TEXT("no cluster was added (the collection kept its %d transforms): auto-clustering groups the pieces of ")
            TEXT("a fractured collection, so fracture it first."), NumTransforms));
}

// ---------------------------------------------------------------------------
// 5. set_geometry_collection_damage
// ---------------------------------------------------------------------------
// MCP:set_geometry_collection_damage - Set the damage model and/or the per-level damage thresholds of a UGeometryCollection asset in an undo transaction, then save it. Only the fields given change; damage_thresholds overwrites the whole threshold list, skipping entries that are not numbers. Succeeds only when the package reaches disk; a failed save leaves the change in memory only (saved false, save_error). The reply reads back damage_model ("Threshold", "MaterialStrength" or "Unknown") and num_thresholds; get_geometry_collection_info shows them later. Mass: set_geometry_collection_mass; clustering: set_geometry_collection_connection_settings.
//   @param geometry_collection_path - Full asset path to the UGeometryCollection
//   @param damage_model - Threshold (alias UserDefined) or Simple (alias Material, the material-strength model), case-insensitive; any other text means Threshold, and leaving it out keeps the current model.
//   @param damage_thresholds - Optional array of per-level damage threshold floats
//   @returns { geometry_collection_path: string, damage_model: string, num_thresholds: int, success: bool, saved: bool, save_error: string? }
TSharedPtr<FJsonObject> HandleSetGeometryCollectionDamage(const TSharedPtr<FJsonObject>& Params)
{
    FString GCPath;
    UGeometryCollection* GC = nullptr;
    if (TSharedPtr<FJsonObject> Error = Chaos_ReadCollection(Params, GCPath, GC))
    {
        return Error;
    }

    const FScopedTransaction Transaction(LOCTEXT("SetGeometryCollectionDamage", "MCP: Set Geometry Collection Damage"));
    GC->Modify();

    // Set damage model.
    FString DamageModelStr;
    if (Params->TryGetStringField(TEXT("damage_model"), DamageModelStr))
    {
        GC->DamageModel = ParseDamageModel(DamageModelStr);
    }

    // Set damage thresholds.
    const TArray<TSharedPtr<FJsonValue>>* ThresholdArray = nullptr;
    if (Params->TryGetArrayField(TEXT("damage_thresholds"), ThresholdArray))
    {
        GC->DamageThreshold.Empty();
        for (const TSharedPtr<FJsonValue>& Val : *ThresholdArray)
        {
            double V = 0.0;
            if (Val->TryGetNumber(V))
            {
                GC->DamageThreshold.Add(static_cast<float>(V));
            }
        }
    }

    // Save the modified asset; the reply succeeds only when the package reached disk.
    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(GC->GetOutermost(), GC, SaveError);

    TSharedPtr<FJsonObject> Result = Chaos_StartReply(GCPath);
    Result->SetStringField(TEXT("damage_model"), DamageModelToString(GC->DamageModel));
    Result->SetNumberField(TEXT("num_thresholds"), GC->DamageThreshold.Num());
    Result->SetBoolField(TEXT("success"), bSaved);
    return mcp_common::MakeSavedReply(Result, bSaved, TEXT("set_geometry_collection_damage"), SaveError);
}

// ---------------------------------------------------------------------------
// 5b. set_geometry_collection_mass
// ---------------------------------------------------------------------------
// MCP:set_geometry_collection_mass - Writes the mass settings of a UGeometryCollection asset (mass, mass_as_density, minimum_mass_clamp, density_from_physics_material), only for the fields given, then saves it; undoable. Returns the four values as stored, success, saved; fails when the save fails. Read them back with get_geometry_collection_info.
//   @param geometry_collection_path - Full asset path to the UGeometryCollection
//   @param mass - Optional total mass (kg), or density (kg/m^3) when mass_as_density is true
//   @param mass_as_density - Optional, interpret 'mass' as a density in kg/m^3, default unchanged
//   @param minimum_mass_clamp - Optional smallest allowable mass, default unchanged
//   @param density_from_physics_material - Optional, derive density from the physics material, default unchanged
//   @returns { geometry_collection_path: string, mass: float, mass_as_density: bool, minimum_mass_clamp: float, density_from_physics_material: bool, success: bool, saved: bool }
TSharedPtr<FJsonObject> HandleSetGeometryCollectionMass(const TSharedPtr<FJsonObject>& Params)
{
    FString GCPath;
    UGeometryCollection* GC = nullptr;
    if (TSharedPtr<FJsonObject> Error = Chaos_ReadCollection(Params, GCPath, GC))
    {
        return Error;
    }

    const FScopedTransaction Transaction(LOCTEXT("SetGeometryCollectionMass", "MCP: Set Geometry Collection Mass"));
    GC->Modify();

    // Each field is set-to-value: only the keys present in the request are written
    // (so callers can tweak a single dimension without resetting the others).
    if (Params->HasField(TEXT("density_from_physics_material")))
    {
        bool bDensityFromMaterial = GC->bDensityFromPhysicsMaterial;
        Params->TryGetBoolField(TEXT("density_from_physics_material"), bDensityFromMaterial);
        GC->bDensityFromPhysicsMaterial = bDensityFromMaterial;
    }

    if (Params->HasField(TEXT("mass_as_density")))
    {
        bool bMassAsDensity = GC->bMassAsDensity;
        Params->TryGetBoolField(TEXT("mass_as_density"), bMassAsDensity);
        GC->bMassAsDensity = bMassAsDensity;
    }

    if (Params->HasField(TEXT("mass")))
    {
        double MassVal = GC->Mass;
        Params->TryGetNumberField(TEXT("mass"), MassVal);
        GC->Mass = static_cast<float>(MassVal);
    }

    if (Params->HasField(TEXT("minimum_mass_clamp")))
    {
        double MinClamp = GC->MinimumMassClamp;
        Params->TryGetNumberField(TEXT("minimum_mass_clamp"), MinClamp);
        GC->MinimumMassClamp = static_cast<float>(MinClamp);
    }

    GC->InvalidateCollection();

    const bool bSaved = mcp_common::SaveAssetPackage(GC->GetOutermost(), GC);

    TSharedPtr<FJsonObject> Result = Chaos_StartReply(GCPath);
    Result->SetNumberField(TEXT("mass"), GC->Mass);
    Result->SetBoolField(TEXT("mass_as_density"), GC->bMassAsDensity);
    Result->SetNumberField(TEXT("minimum_mass_clamp"), GC->MinimumMassClamp);
    Result->SetBoolField(TEXT("density_from_physics_material"), GC->bDensityFromPhysicsMaterial);
    Result->SetBoolField(TEXT("success"), true);
    return mcp_common::MakeSavedReply(Result, bSaved, TEXT("set_geometry_collection_mass"));
}

// ---------------------------------------------------------------------------
// 6. add_geometry_collection_source
// ---------------------------------------------------------------------------
// MCP:add_geometry_collection_source - Append an additional StaticMesh source to an existing geometry collection, then save it. Succeeds only when the package reaches disk; a failed save leaves the change in memory only (saved false, save_error).
//   @param geometry_collection_path - Full asset path to the UGeometryCollection
//   @param source_mesh_path - Asset path to the UStaticMesh to append
//   @param transform - Optional {location, rotation, scale} for the source mesh placement
//   @returns { geometry_collection_path: string, source_mesh_path: string, num_sources: int, num_materials: int, success: bool, saved: bool, save_error: string? }
TSharedPtr<FJsonObject> HandleAddGeometryCollectionSource(const TSharedPtr<FJsonObject>& Params)
{
    FString GCPath;
    UGeometryCollection* GC = nullptr;
    if (TSharedPtr<FJsonObject> Error = Chaos_ReadCollection(Params, GCPath, GC))
    {
        return Error;
    }

    FString SourceMeshPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("source_mesh_path"), SourceMeshPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UStaticMesh* SourceMesh = LoadObject<UStaticMesh>(nullptr, *SourceMeshPath);
    if (!SourceMesh)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("StaticMesh not found: %s"), *SourceMeshPath));

    // Parse optional transform.
    FTransform SourceTransform = FTransform::Identity;
    if (Params->HasField(TEXT("transform")))
    {
        const TSharedPtr<FJsonObject>* TransformObj = nullptr;
        if (Params->TryGetObjectField(TEXT("transform"), TransformObj) && TransformObj)
        {
            FVector Location = mcp_common::ReadJsonVector(*TransformObj, TEXT("location"));
            FRotator Rotation = mcp_common::ReadJsonRotator(*TransformObj, TEXT("rotation"));

            FVector Scale(1.0f);
            if ((*TransformObj)->HasField(TEXT("scale")))
            {
                const TArray<TSharedPtr<FJsonValue>>* ScaleArr = nullptr;
                if ((*TransformObj)->TryGetArrayField(TEXT("scale"), ScaleArr) && ScaleArr && ScaleArr->Num() >= 3)
                {
                    double SX = 1.0, SY = 1.0, SZ = 1.0;
                    (*ScaleArr)[0]->TryGetNumber(SX);
                    (*ScaleArr)[1]->TryGetNumber(SY);
                    (*ScaleArr)[2]->TryGetNumber(SZ);
                    Scale = FVector(SX, SY, SZ);
                }
            }

            SourceTransform = FTransform(Rotation, Location, Scale);
        }
    }

    const FScopedTransaction Transaction(LOCTEXT("AddGeometryCollectionSource", "MCP: Add Geometry Collection Source"));
    GC->Modify();

    // Build materials array from the source mesh.
    TArray<UMaterialInterface*> MeshMaterials;
    const TArray<FStaticMaterial>& StaticMats = SourceMesh->GetStaticMaterials();
    for (int32 i = 0; i < StaticMats.Num(); ++i)
    {
        MeshMaterials.Add(StaticMats[i].MaterialInterface);
    }

    const bool bAppended = FGeometryCollectionEngineConversion::AppendStaticMesh(
        SourceMesh, MeshMaterials, SourceTransform, GC,
        /*bReindexMaterials=*/true, /*bAddInternalMaterials=*/true,
        /*bSplitComponents=*/false, /*bSetInternalFromMaterialIndex=*/false);

    if (!bAppended)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to append static mesh to geometry collection"));
    }

    // Record the source in the editor-only GeometrySource array.
    FGeometryCollectionSource Src;
    Src.SourceGeometryObject = FSoftObjectPath(SourceMeshPath);
    Src.LocalTransform = SourceTransform;
    Src.SourceMaterial = MeshMaterials;
    GC->GeometrySource.Add(Src);

    // Mark simulation data dirty.
    GC->InvalidateCollection();

    // Save the modified asset; the reply succeeds only when the package reached disk.
    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(GC->GetOutermost(), GC, SaveError);

    TSharedPtr<FJsonObject> Result = Chaos_StartReply(GCPath);
    Result->SetStringField(TEXT("source_mesh_path"), SourceMeshPath);
    Result->SetNumberField(TEXT("num_sources"), GC->GeometrySource.Num());
    Result->SetNumberField(TEXT("num_materials"), GC->Materials.Num());
    Result->SetBoolField(TEXT("success"), bSaved);
    return mcp_common::MakeSavedReply(Result, bSaved, TEXT("add_geometry_collection_source"), SaveError);
}

// ---------------------------------------------------------------------------
// 7. remove_geometry_collection_source
// ---------------------------------------------------------------------------
// MCP:remove_geometry_collection_source - Remove a StaticMesh source reference from an existing geometry collection (inverse of add_geometry_collection_source; drops the GeometrySource entry only, does NOT re-fracture the core geometry)
//   @param geometry_collection_path - Full asset path to the UGeometryCollection
//   @param index - Optional 0-based index into the GeometrySource array (exactly one of index / source_mesh_path)
//   @param source_mesh_path - Optional asset path of the source mesh to match against SourceGeometryObject (exactly one of index / source_mesh_path)
//   @returns { geometry_collection_path: string, removed_index: int, removed_source_object: string, removed: bool, already_absent: bool, num_sources: int, success: bool, saved: bool }
TSharedPtr<FJsonObject> HandleRemoveGeometryCollectionSource(const TSharedPtr<FJsonObject>& Params)
{
    FString GCPath;
    UGeometryCollection* GC = nullptr;
    if (TSharedPtr<FJsonObject> Error = Chaos_ReadCollection(Params, GCPath, GC))
    {
        return Error;
    }

    const FScopedTransaction Transaction(LOCTEXT("RemoveGeometryCollectionSource", "MCP: Remove Geometry Collection Source"));
    GC->Modify();

    // Selection: EXACTLY one of {index, source_mesh_path}.
    int32 IndexParam = 0;
    const bool bHasIndex = Params->TryGetNumberField(TEXT("index"), IndexParam);
    FString SourceMeshPath;
    const bool bHasMeshPath = Params->TryGetStringField(TEXT("source_mesh_path"), SourceMeshPath);

    if (bHasIndex == bHasMeshPath)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Provide exactly one of 'index' (int) or 'source_mesh_path' (string) to select the source to remove"));
    }

    // Resolve the index to remove.
    int32 RemoveIdx = INDEX_NONE;
    if (bHasIndex)
    {
        if (IndexParam >= 0 && IndexParam < GC->GeometrySource.Num())
        {
            RemoveIdx = IndexParam;
        }
    }
    else
    {
        const FSoftObjectPath TargetPath(SourceMeshPath);
        for (int32 i = 0; i < GC->GeometrySource.Num(); ++i)
        {
            if (GC->GeometrySource[i].SourceGeometryObject == TargetPath)
            {
                RemoveIdx = i;
                break;
            }
        }
    }

    // No-op honest: index out of range / no match -> report already_absent, no save.
    if (RemoveIdx == INDEX_NONE)
    {
        TSharedPtr<FJsonObject> AbsentResult = Chaos_StartReply(GCPath);
        AbsentResult->SetBoolField(TEXT("removed"), false);
        AbsentResult->SetBoolField(TEXT("already_absent"), true);
        AbsentResult->SetNumberField(TEXT("num_sources"), GC->GeometrySource.Num());
        AbsentResult->SetBoolField(TEXT("success"), true);
        return mcp_common::MakeOkReply(AbsentResult);
    }

    // Capture the removed object before dropping the entry.
    const FString RemovedSourceObject = GC->GeometrySource[RemoveIdx].SourceGeometryObject.ToString();
    GC->GeometrySource.RemoveAt(RemoveIdx);

    // Regenerate StateGuid + reindex to reflect the dropped source (no re-fracture, no re-sim).
    GC->InvalidateCollection();

    // Save the modified asset.
    const bool bSaved = mcp_common::SaveAssetPackage(GC->GetOutermost(), GC);

    TSharedPtr<FJsonObject> Result = Chaos_StartReply(GCPath);
    Result->SetNumberField(TEXT("removed_index"), RemoveIdx);
    Result->SetStringField(TEXT("removed_source_object"), RemovedSourceObject);
    Result->SetBoolField(TEXT("removed"), true);
    Result->SetBoolField(TEXT("already_absent"), false);
    Result->SetNumberField(TEXT("num_sources"), GC->GeometrySource.Num());
    Result->SetBoolField(TEXT("success"), true);
    return mcp_common::MakeSavedReply(Result, bSaved, TEXT("remove_geometry_collection_source"));
}

// ---------------------------------------------------------------------------
// cut_geometry_collection_with_planes
// ---------------------------------------------------------------------------
// MCP:cut_geometry_collection_with_planes - Cuts all transforms of a UGeometryCollection asset with num_planes random planes inside its bounds (no noise, islands split by default), then saves it. new_fracture_count is the number of new pieces, measured as the transforms the cut added; the call fails when it added none (an empty collection, or planes that miss every piece) or when the save fails.
//   @param geometry_collection_path - Asset path of the UGeometryCollection
//   @param num_planes - Number of random cutting planes to generate, minimum 1, default 3
//   @param random_seed - Optional seed for plane generation, default 1337
//   @param grout - Width of the gap left between cut pieces, in cm (default 0)
//   @param split_islands - Optional, split resulting islands, default true
//   @returns { geometry_collection_path: string, num_planes: int, new_fracture_count: int, transform_count: int, success: bool, saved: bool, save_error: string? }
TSharedPtr<FJsonObject> HandlePlaneCutFracture(const TSharedPtr<FJsonObject>& Params)
{
    FString GCPath;
    UGeometryCollection* GC = nullptr;
    TSharedPtr<FGeometryCollection, ESPMode::ThreadSafe> FGC;
    if (TSharedPtr<FJsonObject> Error = Chaos_ReadCollectionData(Params, GCPath, GC, FGC))
    {
        return Error;
    }

    int32 NumPlanes = 3;
    Params->TryGetNumberField(TEXT("num_planes"), NumPlanes);
    if (NumPlanes < 1) NumPlanes = 1;

    int32 RandomSeed = 1337;
    Params->TryGetNumberField(TEXT("random_seed"), RandomSeed);

    float Grout = 0.0f;
    if (Params->HasField(TEXT("grout")))
    {
        Params->TryGetNumberField(TEXT("grout"), Grout);
    }

    bool bSplitIslands = true;
    Params->TryGetBoolField(TEXT("split_islands"), bSplitIslands);

    const FScopedTransaction Transaction(LOCTEXT("PlaneCutFracture", "MCP: Plane Cut Fracture"));
    GC->Modify();

    const int32 NumTransformsBefore = FGC->NumElements(FGeometryCollection::TransformGroup);

    const FBoxSphereBounds Bounds = FGC->GetBoundingBox();
    const FBox BBox = Bounds.GetBox();

    FDataflowTransformSelection TransformSelection;
    TransformSelection.InitializeFromCollection(*FGC, true);

    FFractureEngineFracturing::PlaneCutter(
        *FGC,
        TransformSelection,
        BBox,
        FTransform::Identity,
        NumPlanes,
        RandomSeed,
        /*InChanceToFracture=*/1.0f,
        bSplitIslands,
        Grout,
        /*InAmplitude=*/0.0f,
        /*InFrequency=*/0.1f,
        /*InPersistence=*/0.5f,
        /*InLacunarity=*/2.0f,
        /*InOctaveNumber=*/2,
        /*InPointSpacing=*/0.0f,
        /*InAddSamplesForCollision=*/false,
        /*InCollisionSampleSpacing=*/0.0f);

    GC->InvalidateCollection();

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(GC->GetOutermost(), GC, SaveError);

    const int32 NumTransformsAfter = FGC->NumElements(FGeometryCollection::TransformGroup);

    TSharedPtr<FJsonObject> Result = Chaos_StartReply(GCPath);
    Result->SetNumberField(TEXT("num_planes"), NumPlanes);
    Result->SetNumberField(TEXT("new_fracture_count"), NumTransformsAfter - NumTransformsBefore);
    Result->SetNumberField(TEXT("transform_count"), NumTransformsAfter);
    return Chaos_EditReply(Result, NumTransformsAfter - NumTransformsBefore, bSaved, SaveError, TEXT("cut_geometry_collection_with_planes"),
        FString::Printf(TEXT("nothing was cut (the collection kept its %d transforms): an empty collection, or planes that ")
            TEXT("miss every piece, cut nothing."), NumTransformsBefore));
}

// ---------------------------------------------------------------------------
// slice_geometry_collection
// ---------------------------------------------------------------------------
// MCP:slice_geometry_collection - Slices all transforms of a UGeometryCollection asset with a regular grid of slices_x by slices_y by slices_z cuts across its bounds (no angle or offset variation, islands split), then saves it. new_fracture_count is the number of new pieces, measured as the transforms the slicing added; the call fails when it added none (an empty collection, or all three slice counts 0) or when the save fails.
//   @param geometry_collection_path - Asset path of the UGeometryCollection
//   @param slices_x - Number of evenly spaced X slices, default 2
//   @param slices_y - Number of evenly spaced Y slices, default 2
//   @param slices_z - Number of evenly spaced Z slices, default 1
//   @param random_seed - Seed passed to the slicer, default 1337; angle and offset variation are fixed at 0, so it changes little
//   @param grout - Width of the gap left between cut pieces, in cm (default 0)
//   @returns { geometry_collection_path: string, slices_x: int, slices_y: int, slices_z: int, new_fracture_count: int, transform_count: int, success: bool, saved: bool, save_error: string? }
TSharedPtr<FJsonObject> HandleSliceFracture(const TSharedPtr<FJsonObject>& Params)
{
    FString GCPath;
    UGeometryCollection* GC = nullptr;
    TSharedPtr<FGeometryCollection, ESPMode::ThreadSafe> FGC;
    if (TSharedPtr<FJsonObject> Error = Chaos_ReadCollectionData(Params, GCPath, GC, FGC))
    {
        return Error;
    }

    int32 SlicesX = 2;
    Params->TryGetNumberField(TEXT("slices_x"), SlicesX);
    if (SlicesX < 0) SlicesX = 0;

    int32 SlicesY = 2;
    Params->TryGetNumberField(TEXT("slices_y"), SlicesY);
    if (SlicesY < 0) SlicesY = 0;

    int32 SlicesZ = 1;
    Params->TryGetNumberField(TEXT("slices_z"), SlicesZ);
    if (SlicesZ < 0) SlicesZ = 0;

    int32 RandomSeed = 1337;
    Params->TryGetNumberField(TEXT("random_seed"), RandomSeed);

    float Grout = 0.0f;
    if (Params->HasField(TEXT("grout")))
    {
        Params->TryGetNumberField(TEXT("grout"), Grout);
    }

    const FScopedTransaction Transaction(LOCTEXT("SliceFracture", "MCP: Slice Fracture"));
    GC->Modify();

    const int32 NumTransformsBefore = FGC->NumElements(FGeometryCollection::TransformGroup);

    const FBoxSphereBounds Bounds = FGC->GetBoundingBox();
    const FBox BBox = Bounds.GetBox();

    FDataflowTransformSelection TransformSelection;
    TransformSelection.InitializeFromCollection(*FGC, true);

    FFractureEngineFracturing::SliceCutter(
        *FGC,
        TransformSelection,
        BBox,
        SlicesX,
        SlicesY,
        SlicesZ,
        /*InSliceAngleVariation=*/0.0f,
        /*InSliceOffsetVariation=*/0.0f,
        RandomSeed,
        /*InChanceToFracture=*/1.0f,
        /*InSplitIslands=*/true,
        Grout,
        /*InAmplitude=*/0.0f,
        /*InFrequency=*/0.1f,
        /*InPersistence=*/0.5f,
        /*InLacunarity=*/2.0f,
        /*InOctaveNumber=*/2,
        /*InPointSpacing=*/0.0f,
        /*InAddSamplesForCollision=*/false,
        /*InCollisionSampleSpacing=*/0.0f);

    GC->InvalidateCollection();

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(GC->GetOutermost(), GC, SaveError);

    const int32 NumTransformsAfter = FGC->NumElements(FGeometryCollection::TransformGroup);

    TSharedPtr<FJsonObject> Result = Chaos_StartReply(GCPath);
    Result->SetNumberField(TEXT("slices_x"), SlicesX);
    Result->SetNumberField(TEXT("slices_y"), SlicesY);
    Result->SetNumberField(TEXT("slices_z"), SlicesZ);
    Result->SetNumberField(TEXT("new_fracture_count"), NumTransformsAfter - NumTransformsBefore);
    Result->SetNumberField(TEXT("transform_count"), NumTransformsAfter);
    return Chaos_EditReply(Result, NumTransformsAfter - NumTransformsBefore, bSaved, SaveError, TEXT("slice_geometry_collection"),
        FString::Printf(TEXT("nothing was sliced (the collection kept its %d transforms): an empty collection, or slices_x, ")
            TEXT("slices_y and slices_z all 0, cut nothing."), NumTransformsBefore));
}

// ---------------------------------------------------------------------------
// compute_geometry_collection_proximity
// ---------------------------------------------------------------------------
// MCP:compute_geometry_collection_proximity - Rebuilds the proximity (adjacency) graph of a UGeometryCollection asset in place and saves it; undoable. Returns proximity_pairs (unordered pairs of neighboring pieces, from a precise proximity pass), transform_count, success and saved; fails when the save fails. Afterwards get_geometry_collection_hierarchy reports proximity_computed true.
//   @param geometry_collection_path - Asset path of the UGeometryCollection
//   @returns { geometry_collection_path: string, proximity_pairs: int, transform_count: int, success: bool, saved: bool }
TSharedPtr<FJsonObject> HandleComputeGCProximity(const TSharedPtr<FJsonObject>& Params)
{
    FString GCPath;
    UGeometryCollection* GC = nullptr;
    TSharedPtr<FGeometryCollection, ESPMode::ThreadSafe> FGC;
    if (TSharedPtr<FJsonObject> Error = Chaos_ReadCollectionData(Params, GCPath, GC, FGC))
    {
        return Error;
    }

    const FScopedTransaction Transaction(LOCTEXT("ComputeGCProximity", "MCP: Compute Geometry Collection Proximity"));
    GC->Modify();

    // Build/refresh the proximity graph in-place.
    FGeometryCollectionProximityUtility ProximityUtil(FGC.Get());
    ProximityUtil.UpdateProximity();

    // Read precise proximity to report a deterministic pair count.
    const TArray<TSet<int32>> Precise = FGeometryCollectionProximityUtility::ComputePreciseProximity(*FGC);
    int64 TotalConnections = 0;
    for (const TSet<int32>& Neighbors : Precise)
    {
        TotalConnections += Neighbors.Num();
    }
    // Each connection is counted from both sides; report unordered pairs.
    const int32 ProximityPairs = static_cast<int32>(TotalConnections / 2);

    GC->InvalidateCollection();

    const bool bSaved = mcp_common::SaveAssetPackage(GC->GetOutermost(), GC);

    const int32 NumTransforms = FGC->NumElements(FGeometryCollection::TransformGroup);

    TSharedPtr<FJsonObject> Result = Chaos_StartReply(GCPath);
    Result->SetNumberField(TEXT("proximity_pairs"), ProximityPairs);
    Result->SetNumberField(TEXT("transform_count"), NumTransforms);
    Result->SetBoolField(TEXT("success"), true);
    return mcp_common::MakeSavedReply(Result, bSaved, TEXT("compute_geometry_collection_proximity"));
}

// ---------------------------------------------------------------------------
// set_geometry_collection_connection_settings
// ---------------------------------------------------------------------------
// MCP:set_geometry_collection_connection_settings - Write the five clustering and connection-graph properties of a UGeometryCollection asset (cluster connection type, EnableClustering, ClusterGroupIndex, MaxClusterLevel, bounds filtering margin) in an undo transaction, invalidate it and save it; a failed save makes success false. Every field is written on each call, so an omitted one goes back to its listed default, and an unrecognized connection type becomes PointImplicit. The bone hierarchy is not rebuilt: cluster_geometry_collection builds clusters and compute_geometry_collection_proximity the proximity graph. The reply reads back the type, enable_clustering and max_cluster_level.
//   @param geometry_collection_path - Asset path of the UGeometryCollection
//   @param cluster_connection_type - PointImplicit, DelaunayTriangulation, MinimalSpanningSubsetDelaunayTriangulation, BoundsOverlapFilteredDelaunayTriangulation, PointImplicitAugmentedWithMinimalDelaunay, or None, default PointImplicit
//   @param enable_clustering - If true, enable cluster graph generation, default true
//   @param cluster_group_index - Cluster group index, default 0
//   @param max_cluster_level - Maximum hierarchy level to cluster, default 100
//   @param bounds_filtering_margin - Connection graph bounds filtering margin, default 0
//   @returns { geometry_collection_path: string, cluster_connection_type: string, enable_clustering: bool, max_cluster_level: int, success: bool, saved: bool }
TSharedPtr<FJsonObject> HandleSetGCConnectionSettings(const TSharedPtr<FJsonObject>& Params)
{
    FString GCPath;
    UGeometryCollection* GC = nullptr;
    if (TSharedPtr<FJsonObject> Error = Chaos_ReadCollection(Params, GCPath, GC))
    {
        return Error;
    }

    const FScopedTransaction Transaction(LOCTEXT("SetGCConnectionSettings", "MCP: Set Geometry Collection Connection Settings"));
    GC->Modify();

    FString ConnTypeStr = TEXT("PointImplicit");
    Params->TryGetStringField(TEXT("cluster_connection_type"), ConnTypeStr);
    GC->ClusterConnectionType = ParseClusterConnectionType(ConnTypeStr);

    bool bEnableClustering = true;
    if (Params->HasField(TEXT("enable_clustering")))
    {
        Params->TryGetBoolField(TEXT("enable_clustering"), bEnableClustering);
    }
    GC->EnableClustering = bEnableClustering;

    int32 ClusterGroupIndex = 0;
    if (Params->HasField(TEXT("cluster_group_index")))
    {
        Params->TryGetNumberField(TEXT("cluster_group_index"), ClusterGroupIndex);
    }
    GC->ClusterGroupIndex = ClusterGroupIndex;

    int32 MaxClusterLevel = 100;
    if (Params->HasField(TEXT("max_cluster_level")))
    {
        Params->TryGetNumberField(TEXT("max_cluster_level"), MaxClusterLevel);
    }
    GC->MaxClusterLevel = MaxClusterLevel;

    float BoundsFilteringMargin = 0.0f;
    if (Params->HasField(TEXT("bounds_filtering_margin")))
    {
        Params->TryGetNumberField(TEXT("bounds_filtering_margin"), BoundsFilteringMargin);
    }
    GC->ConnectionGraphBoundsFilteringMargin = BoundsFilteringMargin;

    GC->InvalidateCollection();

    const bool bSaved = mcp_common::SaveAssetPackage(GC->GetOutermost(), GC);

    TSharedPtr<FJsonObject> Result = Chaos_StartReply(GCPath);
    Result->SetStringField(TEXT("cluster_connection_type"), ClusterConnectionTypeToString(GC->ClusterConnectionType));
    Result->SetBoolField(TEXT("enable_clustering"), GC->EnableClustering);
    Result->SetNumberField(TEXT("max_cluster_level"), GC->MaxClusterLevel);
    Result->SetBoolField(TEXT("success"), true);
    return mcp_common::MakeSavedReply(Result, bSaved, TEXT("set_geometry_collection_connection_settings"));
}

// ---------------------------------------------------------------------------
// get_geometry_collection_hierarchy
// ---------------------------------------------------------------------------
// MCP:get_geometry_collection_hierarchy - Reads the bone hierarchy of a UGeometryCollection asset: transform_count, root_indices and root_count, and per bone (parallel arrays in transform index order) its level, name, parent index (-1 for a root) and simulation type ("Rigid", "Clustered" or "None"), plus proximity_computed. Generates the Level attribute in memory if needed, without saving. A path that does not load or a collection without geometry data is an error. For damage, mass and source settings use get_geometry_collection_info.
//   @param geometry_collection_path - Asset path of the UGeometryCollection
//   @returns { geometry_collection_path: string, transform_count: int, root_indices: [int], root_count: int, levels: [int], bone_names: [string], parents: [int], simulation_types: [string], proximity_computed: bool, success: bool }
TSharedPtr<FJsonObject> HandleGetGCHierarchyInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString GCPath;
    UGeometryCollection* GC = nullptr;
    TSharedPtr<FGeometryCollection, ESPMode::ThreadSafe> FGC;
    if (TSharedPtr<FJsonObject> Error = Chaos_ReadCollectionData(Params, GCPath, GC, FGC))
    {
        return Error;
    }

    const int32 NumTransforms = FGC->NumElements(FGeometryCollection::TransformGroup);

    // Build the hierarchy facade and ensure the level attribute exists.
    Chaos::Facades::FCollectionHierarchyFacade Hierarchy(*FGC);
    if (Hierarchy.IsValid())
    {
        Hierarchy.GenerateLevelAttribute();
    }

    // Root indices.
    TArray<TSharedPtr<FJsonValue>> RootIndicesJson;
    if (Hierarchy.IsValid())
    {
        const TArray<int32> Roots = Hierarchy.GetRootIndices();
        for (int32 R : Roots)
        {
            RootIndicesJson.Add(MakeShared<FJsonValueNumber>(R));
        }
    }

    // Per-bone arrays: levels, bone names, simulation types.
    TArray<TSharedPtr<FJsonValue>> LevelsJson;
    TArray<TSharedPtr<FJsonValue>> BoneNamesJson;
    TArray<TSharedPtr<FJsonValue>> ParentsJson;
    TArray<TSharedPtr<FJsonValue>> SimTypesJson;

    const TManagedArray<FString>& BoneNames = FGC->BoneName;
    const TManagedArray<int32>& Parents = FGC->Parent;
    const TManagedArray<int32>& SimTypes = FGC->SimulationType;

    // A bone with no parent entry, or INDEX_NONE as parent, is a root.
    int32 RootCount = 0;
    for (int32 i = 0; i < NumTransforms; ++i)
    {
        const int32 Level = Hierarchy.IsValid() ? Hierarchy.GetInitialLevel(i) : INDEX_NONE;
        LevelsJson.Add(MakeShared<FJsonValueNumber>(Level));

        BoneNamesJson.Add(MakeShared<FJsonValueString>(BoneNames.IsValidIndex(i) ? BoneNames[i] : FString()));
        const int32 Parent = Parents.IsValidIndex(i) ? Parents[i] : INDEX_NONE;
        ParentsJson.Add(MakeShared<FJsonValueNumber>(Parent));
        if (Parent == INDEX_NONE)
        {
            ++RootCount;
        }
        SimTypesJson.Add(MakeShared<FJsonValueString>(Chaos_SimulationTypeName(SimTypes, i)));
    }

    const bool bProximityComputed = FGC->HasAttribute(TEXT("Proximity"), FGeometryCollection::GeometryGroup);

    TSharedPtr<FJsonObject> Result = Chaos_StartReply(GCPath);
    Result->SetNumberField(TEXT("transform_count"), NumTransforms);
    Result->SetArrayField(TEXT("root_indices"), RootIndicesJson);
    Result->SetNumberField(TEXT("root_count"), RootCount);
    Result->SetArrayField(TEXT("levels"), LevelsJson);
    Result->SetArrayField(TEXT("bone_names"), BoneNamesJson);
    Result->SetArrayField(TEXT("parents"), ParentsJson);
    Result->SetArrayField(TEXT("simulation_types"), SimTypesJson);
    Result->SetBoolField(TEXT("proximity_computed"), bProximityComputed);
    Result->SetBoolField(TEXT("success"), true);
    return mcp_common::MakeOkReply(Result);
}

// The state-mutation commands (set_state, crumble, recombine) were dropped: the
// state enum names changed in UE 5.8 and authoring bone state on the asset has
// no runtime effect (managed by UGeometryCollectionComponent at runtime).

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_geometry_collection_source"), &HandleAddGeometryCollectionSource },
    { TEXT("cluster_geometry_collection"), &HandleClusterGeometryCollection },
    { TEXT("compute_geometry_collection_proximity"), &HandleComputeGCProximity },
    { TEXT("create_geometry_collection"), &HandleCreateGeometryCollection },
    { TEXT("fracture_geometry_collection"), &HandleFractureGeometryCollection },
    { TEXT("get_geometry_collection_hierarchy"), &HandleGetGCHierarchyInfo },
    { TEXT("get_geometry_collection_info"), &HandleGetGeometryCollectionInfo },
    { TEXT("cut_geometry_collection_with_planes"), &HandlePlaneCutFracture },
    { TEXT("remove_geometry_collection_source"), &HandleRemoveGeometryCollectionSource },
    { TEXT("set_geometry_collection_connection_settings"), &HandleSetGCConnectionSettings },
    { TEXT("set_geometry_collection_damage"), &HandleSetGeometryCollectionDamage },
    { TEXT("set_geometry_collection_mass"), &HandleSetGeometryCollectionMass },
    { TEXT("slice_geometry_collection"), &HandleSliceFracture },
};
} // namespace UEAgentChaosCommands

#undef LOCTEXT_NAMESPACE
