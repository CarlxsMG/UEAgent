// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Data/DataAuthoringUtils.h"

#include "WorldPartition/HLOD/HLODLayer.h"          // UHLODLayer, EHLODLayerType, Get/SetLayerType

#include "Editor.h"                                  // GEditor
#include "Engine/World.h"                            // UWorld::IsPartitionedWorld (inline)
#include "EngineUtils.h"                             // TActorIterator<AActor>

#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"                       // FIntProperty / FDoubleProperty / FBoolProperty
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"

/**
 * World Partition HLOD commands.
 *
 * Covers UHLODLayer asset authoring (a plain /Game UObject asset in the Engine module) and actor <-> layer
 * assignment. Create/get/set are real create->read->verify operations; delete a layer with delete_asset.
 * UHLODLayer is MinimalAPI: LayerType has usable inline GetLayerType/SetLayerType accessors, but
 * CellSize/LoadingRange/bIsSpatiallyLoaded are private and UE_DEPRECATED(5.7) (moved to partition settings),
 * so they are read/written through the reflection system (FProperty) rather than the deprecated accessors.
 */
namespace UEAgentHLODCommands
{

// MCP-DOMAIN: HLOD
// Categorized under HLOD (not Optimization).

namespace
{
    // EHLODLayerType <-> string. Mirrors the UENUM DisplayName-ish identifiers but
    // keeps the stable C++ enum names so payloads are deterministic.
    const TCHAR* HLODLayerTypeToString(EHLODLayerType InType)
    {
        switch (InType)
        {
            case EHLODLayerType::Instancing:        return TEXT("Instancing");
            case EHLODLayerType::MeshMerge:         return TEXT("MeshMerge");
            case EHLODLayerType::MeshSimplify:      return TEXT("MeshSimplify");
            case EHLODLayerType::MeshApproximate:   return TEXT("MeshApproximate");
            case EHLODLayerType::Custom:            return TEXT("Custom");
            case EHLODLayerType::CustomHLODActor:   return TEXT("CustomHLODActor");
            default:                                return TEXT("Instancing");
        }
    }

    // Parse a layer type string (case-insensitive). Defaults to Instancing.
    EHLODLayerType ParseHLODLayerType(const FString& In)
    {
        if (In.Equals(TEXT("MeshMerge"), ESearchCase::IgnoreCase) ||
            In.Equals(TEXT("Merged Mesh"), ESearchCase::IgnoreCase) ||
            In.Equals(TEXT("MergedMesh"), ESearchCase::IgnoreCase))
        {
            return EHLODLayerType::MeshMerge;
        }
        if (In.Equals(TEXT("MeshSimplify"), ESearchCase::IgnoreCase) ||
            In.Equals(TEXT("Simplified Mesh"), ESearchCase::IgnoreCase) ||
            In.Equals(TEXT("SimplifiedMesh"), ESearchCase::IgnoreCase))
        {
            return EHLODLayerType::MeshSimplify;
        }
        if (In.Equals(TEXT("MeshApproximate"), ESearchCase::IgnoreCase) ||
            In.Equals(TEXT("Approximated Mesh"), ESearchCase::IgnoreCase) ||
            In.Equals(TEXT("ApproximatedMesh"), ESearchCase::IgnoreCase))
        {
            return EHLODLayerType::MeshApproximate;
        }
        if (In.Equals(TEXT("Custom"), ESearchCase::IgnoreCase))
        {
            return EHLODLayerType::Custom;
        }
        if (In.Equals(TEXT("CustomHLODActor"), ESearchCase::IgnoreCase))
        {
            return EHLODLayerType::CustomHLODActor;
        }
        return EHLODLayerType::Instancing;
    }

    // Load a UHLODLayer from a /Game path. Returns nullptr on failure.
    UHLODLayer* LoadHLODLayerAt(const FString& AssetPath)
    {
        return Cast<UHLODLayer>(StaticLoadObject(UHLODLayer::StaticClass(), nullptr, *AssetPath));
    }

    // CellSize (int32), LoadingRange (double) and bIsSpatiallyLoaded (uint32:1) are
    // private + UE_DEPRECATED(5.7) on UHLODLayer. Read/write them through the
    // reflection system so we avoid the deprecated inline accessors (which also gate
    // their return on bIsSpatiallyLoaded). These properties only exist under
    // WITH_EDITORONLY_DATA, so the FindFProperty lookups are editor-only.

    // Writes Value into the UHLODLayer property Name of Layer when UHLODLayer has it as a PropertyType.
    template <typename PropertyType, typename ValueType>
    void SetHLODLayerProperty(UHLODLayer* Layer, const TCHAR* Name, const ValueType& Value)
    {
        if (PropertyType* Prop = CastField<PropertyType>(UHLODLayer::StaticClass()->FindPropertyByName(Name)))
        {
            Prop->SetPropertyValue_InContainer(Layer, Value);
        }
    }

    // The UHLODLayer property Name read as a PropertyType; ValueType() when there is no such property.
    template <typename PropertyType, typename ValueType>
    ValueType GetHLODLayerProperty(const UHLODLayer* Layer, const TCHAR* Name)
    {
        if (PropertyType* Prop = CastField<PropertyType>(UHLODLayer::StaticClass()->FindPropertyByName(Name)))
        {
            return Prop->GetPropertyValue_InContainer(Layer);
        }
        return ValueType();
    }

    void SetSpatiallyLoaded(UHLODLayer* Layer, bool bValue)
    {
        SetHLODLayerProperty<FBoolProperty>(Layer, TEXT("bIsSpatiallyLoaded"), bValue);
    }

    bool GetSpatiallyLoaded(const UHLODLayer* Layer)
    {
        return GetHLODLayerProperty<FBoolProperty, bool>(Layer, TEXT("bIsSpatiallyLoaded"));
    }

    void SetCellSizeReflected(UHLODLayer* Layer, int32 Value)
    {
        SetHLODLayerProperty<FIntProperty>(Layer, TEXT("CellSize"), Value);
    }

    int32 GetCellSizeReflected(const UHLODLayer* Layer)
    {
        return GetHLODLayerProperty<FIntProperty, int32>(Layer, TEXT("CellSize"));
    }

    void SetLoadingRangeReflected(UHLODLayer* Layer, double Value)
    {
        SetHLODLayerProperty<FDoubleProperty>(Layer, TEXT("LoadingRange"), Value);
    }

    double GetLoadingRangeReflected(const UHLODLayer* Layer)
    {
        return GetHLODLayerProperty<FDoubleProperty, double>(Layer, TEXT("LoadingRange"));
    }

    // The reply of create_hlod_layer before its outcome: the request, then the layer the path holds.
    TSharedPtr<FJsonObject> HLOD_CreatedReply(const FString& Name, const FString& PackagePath, const UHLODLayer* Layer)
    {
        TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
        Reply->SetStringField(TEXT("name"), Name);
        Reply->SetStringField(TEXT("package_path"), PackagePath);
        Reply->SetStringField(TEXT("asset_path"), Layer->GetPathName());
        Reply->SetStringField(TEXT("layer_type"), HLODLayerTypeToString(Layer->GetLayerType()));
        Reply->SetNumberField(TEXT("cell_size"), GetCellSizeReflected(Layer));
        Reply->SetNumberField(TEXT("loading_range"), GetLoadingRangeReflected(Layer));
        return Reply;
    }
}

// MCP:create_hlod_layer - Creates and saves a new UHLODLayer (World Partition HLOD layer) asset.  Idempotent on re-run: an HLOD layer already at the path is returned unchanged with created=false, already_exists=true and saved=false, layer_type not applied and nothing saved; an asset of another class there is an error. Sets default streaming-grid cell_size=25600, loading_range=51200. Layer type accepts 'Instancing', 'MeshApproximate' (also 'Custom'/'CustomHLODActor'). Defaults to 'Instancing'. A failed save makes success false with saved=false. Change it later with set_properties.
//   @param name - Name of the new HLOD layer asset
//   @param package_path - Content folder; defaults to /Game/HLOD
//   @param layer_type - Optional EHLODLayerType string; defaults to "Instancing"
//   @returns { name: string, package_path: string, asset_path: string, layer_type: string, cell_size: int, loading_range: number, created: bool, already_exists?: bool, success: bool, saved: bool, message: string }

TSharedPtr<FJsonObject> HandleCreateHLODLayer(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetName;
    FString PackagePath;
    FString LayerTypeStr;

    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), AssetName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (!Params->TryGetStringField(TEXT("package_path"), PackagePath))
    {
        PackagePath = TEXT("/Game/HLOD");
    }
    if (!Params->TryGetStringField(TEXT("layer_type"), LayerTypeStr))
    {
        LayerTypeStr = TEXT("Instancing");
    }

    const EHLODLayerType DesiredType = ParseHLODLayerType(LayerTypeStr);
    const FString FullPath = FString::Printf(TEXT("%s/%s"), *PackagePath, *AssetName);
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(FullPath)) { return Invalid; }

    // Default streaming-grid params for the new layer (mirrors the engine default
    // HLOD layer setup). Stored on the (deprecated) CellSize/LoadingRange UPROPERTYs.
    const int32 DefaultCellSize = 25600;
    const double DefaultLoadingRange = 51200.0;

    // Idempotent: if it already exists, load and return a STABLE payload. Resolve via LoadHLODLayerAt
    // (StaticLoadObject; registry-independent) — NOT UEditorAssetLibrary::DoesAssetExist, which is
    // unreliable in headless for an asset created earlier this session (asset registry not synced)
    // and would break idempotency under accumulated state. The layer found is returned unchanged and nothing is
    // saved; a subclass of UHLODLayer is not the asset asked for. An asset of an unrelated class does not load
    // here, and NewAssetInPackage below refuses it.
    if (UHLODLayer* Existing = LoadHLODLayerAt(FullPath))
    {
        if (Existing->GetClass() != UHLODLayer::StaticClass())
        {
            return DataAuthoringUtils::OtherClassAtPathReply(FullPath, Existing, UHLODLayer::StaticClass());
        }
        TSharedPtr<FJsonObject> ExistObj = HLOD_CreatedReply(AssetName, PackagePath, Existing);
        DataAuthoringUtils::AddAlreadyExistsFlags(ExistObj, TEXT("HLOD layer"));
        return mcp_common::MakeOkReply(ExistObj);
    }

    // Create a real /Game package + UHLODLayer inside it, then save. The asset is a
    // plain UObject (the inner UHLODBuilderSettings is instanced by the constructor),
    // so direct NewObject is correct (avoids any modal factory).
    FString CreateError;
    UHLODLayer* NewLayer = mcp_common::NewAssetInPackage<UHLODLayer>(FullPath, AssetName, TEXT("Failed to create UHLODLayer"), CreateError);
    if (!NewLayer)
    {
        return mcp_common::MakeErrorReply(CreateError);
    }

    // SetLayerType is an inline editor accessor; CellSize/LoadingRange/bIsSpatiallyLoaded
    // are set via reflection (private + deprecated). Mark spatially-loaded so the
    // grid params are meaningful.
    NewLayer->SetLayerType(DesiredType);
    SetSpatiallyLoaded(NewLayer, true);
    SetCellSizeReflected(NewLayer, DefaultCellSize);
    SetLoadingRangeReflected(NewLayer, DefaultLoadingRange);

    FAssetRegistryModule::AssetCreated(NewLayer);

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(NewLayer->GetOutermost(), NewLayer, SaveError);

    // Verify by readback.
    TSharedPtr<FJsonObject> ResultObj = HLOD_CreatedReply(AssetName, PackagePath, NewLayer);
    DataAuthoringUtils::AddCreatedOutcome(ResultObj, bSaved, TEXT("HLOD layer"), SaveError);

    // The verdict measured above is the verdict EMITTED. A hardcoded success:true
    // from MakeOkReply would hide it, and callers only check that flag.
    return mcp_common::MakeOutcomeReply(ResultObj, bSaved,
        bSaved ? FString() : mcp_common::MeasuredOutcomeError(TEXT("create_hlod_layer"), TEXT("bSaved")));
}

// MCP:set_hlod_layer_settings - Sets streaming-grid settings / layer type on a UHLODLayer and saves it.  Partial update: only the fields you send are touched. Sending cell_size or loading_range also enables the layer spatially loaded. Layer type accepts 'Instancing'|'MeshApproximate'|'Custom'|'CustomHLODActor'.
//   @retired set_properties - object_path=<HLOD layer>, properties={"bIsSpatiallyLoaded":true,"CellSize":25600,"LoadingRange":51200,"LayerType":"Instancing"}, save=true
//   @param hlod_layer_path - Full /Game path to the HLOD layer asset
//   @param cell_size - Optional streaming-grid cell size
//   @param loading_range - Optional streaming-grid loading range
//   @param layer_type - Optional EHLODLayerType string
//   @returns { hlod_layer_path: string, asset_path: string, layer_type: string, cell_size: int, loading_range: number, changed_cell_size: bool, changed_loading_range: bool, changed_layer_type: bool, settings_set: bool, saved: bool }

TSharedPtr<FJsonObject> HandleSetHLODLayerSettings(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("hlod_layer_path"), AssetPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UHLODLayer* Layer = LoadHLODLayerAt(AssetPath);
    if (!Layer)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("HLOD layer not found: %s"), *AssetPath));
    }

    const FScopedTransaction Transaction(FText::FromString(TEXT("MCP: Set HLOD Layer Settings")));
    Layer->Modify();

    bool bChangedCell = false;
    bool bChangedRange = false;
    bool bChangedType = false;

    double CellSizeIn = 0.0;
    if (Params->TryGetNumberField(TEXT("cell_size"), CellSizeIn))
    {
        SetSpatiallyLoaded(Layer, true);
        SetCellSizeReflected(Layer, (int32)FMath::RoundToInt(CellSizeIn));
        bChangedCell = true;
    }

    double LoadingRangeIn = 0.0;
    if (Params->TryGetNumberField(TEXT("loading_range"), LoadingRangeIn))
    {
        SetSpatiallyLoaded(Layer, true);
        SetLoadingRangeReflected(Layer, LoadingRangeIn);
        bChangedRange = true;
    }

    FString LayerTypeStr;
    if (Params->TryGetStringField(TEXT("layer_type"), LayerTypeStr))
    {
        Layer->SetLayerType(ParseHLODLayerType(LayerTypeStr));
        bChangedType = true;
    }

    Layer->MarkPackageDirty();

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Layer->GetOutermost(), Layer, SaveError);

    // Verify by readback.
    const int32 ReadCell = GetCellSizeReflected(Layer);
    const double ReadRange = GetLoadingRangeReflected(Layer);
    const EHLODLayerType ReadType = Layer->GetLayerType();

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("hlod_layer_path"), AssetPath);
    ResultObj->SetStringField(TEXT("asset_path"), Layer->GetPathName());
    ResultObj->SetStringField(TEXT("layer_type"), HLODLayerTypeToString(ReadType));
    ResultObj->SetNumberField(TEXT("cell_size"), ReadCell);
    ResultObj->SetNumberField(TEXT("loading_range"), ReadRange);
    ResultObj->SetBoolField(TEXT("changed_cell_size"), bChangedCell);
    ResultObj->SetBoolField(TEXT("changed_loading_range"), bChangedRange);
    ResultObj->SetBoolField(TEXT("changed_layer_type"), bChangedType);
    ResultObj->SetBoolField(TEXT("settings_set"), true);

    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("set_hlod_layer_settings"), SaveError);
}

// MCP:set_hlod_layer_parent - Sets (or clears) the parent UHLODLayer of a UHLODLayer and saves it.  The parent chain drives nested HLOD generation (a layer's HLOD becomes an input to its parent's HLOD). Send parent_layer_path="" to clear the parent to null; omit parent_layer_path to leave it untouched. Idempotent: if the parent is already the requested value, nothing is modified/saved (changed_parent=false, saved=false). Validates against self-parent and circular parent chains (the engine's guard is editor-only and does not run headless).
//   @param hlod_layer_path - Full /Game path to the child HLOD layer asset
//   @param parent_layer_path - Optional full /Game path to the parent HLOD layer; "" clears to null; absent leaves it untouched
//   @returns { hlod_layer_path: string, asset_path: string, parent_layer_path: string, has_parent: bool, changed_parent: bool, settings_set: bool, saved: bool, note: string? }

TSharedPtr<FJsonObject> HandleSetHLODLayerParent(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("hlod_layer_path"), AssetPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UHLODLayer* Layer = LoadHLODLayerAt(AssetPath);
    if (!Layer)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("HLOD layer not found: %s"), *AssetPath));
    }

    // Presence of parent_layer_path is the intent to write. Absent => leave untouched
    // (resolve target to the current parent so idempotency short-circuits). Empty
    // string => clear to nullptr. Non-empty => resolve the asset (error if missing).
    FString ParentPath;
    const bool bHasParent = Params->TryGetStringField(TEXT("parent_layer_path"), ParentPath);

    UHLODLayer* Parent = Layer->GetParentLayer();
    if (bHasParent)
    {
        Parent = ParentPath.IsEmpty() ? nullptr : LoadHLODLayerAt(ParentPath);
        if (!ParentPath.IsEmpty() && !Parent)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Parent HLOD layer not found: %s"), *ParentPath));
        }

        // Engine self-parent / cycle guards are editor-only (they live in the details
        // customization), so they do NOT run headless — validate here to keep the asset
        // consistent. Self-parent first, then walk the parent chain for a cycle.
        if (Parent == Layer)
        {
            return mcp_common::MakeErrorReply(TEXT("Invalid parent: a HLOD layer cannot be its own parent"));
        }

        TSet<UHLODLayer*> Visited;
        for (UHLODLayer* Cursor = Parent; Cursor != nullptr; Cursor = Cursor->GetParentLayer())
        {
            if (Cursor == Layer)
            {
                return mcp_common::MakeErrorReply(TEXT("Invalid parent: setting this parent would create a circular parent chain"));
            }
            bool bAlreadyVisited = false;
            Visited.Add(Cursor, &bAlreadyVisited);
            if (bAlreadyVisited)
            {
                // Pre-existing cycle among ancestors (defensive) — stop walking.
                break;
            }
        }
    }

    // Idempotent: if the parent is already the requested value, do not Modify/save.
    const bool bChangedParent = (Layer->GetParentLayer() != Parent);
    bool bSaved = false;
    if (bChangedParent)
    {
        const FScopedTransaction Transaction(FText::FromString(TEXT("MCP: Set HLOD Layer Parent")));
        Layer->Modify();
        Layer->SetParentLayer(Parent);
        Layer->MarkPackageDirty();

        FString SaveError;
        bSaved = mcp_common::SaveAssetPackage(Layer->GetOutermost(), Layer, SaveError);
    }

    // Verify by readback.
    UHLODLayer* ReadParent = Layer->GetParentLayer();

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("hlod_layer_path"), AssetPath);
    ResultObj->SetStringField(TEXT("asset_path"), Layer->GetPathName());
    ResultObj->SetStringField(TEXT("parent_layer_path"), ReadParent ? ReadParent->GetPathName() : FString());
    ResultObj->SetBoolField(TEXT("has_parent"), ReadParent != nullptr);
    ResultObj->SetBoolField(TEXT("changed_parent"), bChangedParent);
    ResultObj->SetBoolField(TEXT("settings_set"), true);
    if (!bChangedParent)
    {
        ResultObj->SetBoolField(TEXT("saved"), false);
        ResultObj->SetStringField(TEXT("note"), TEXT("the parent already has the requested value; nothing changed, nothing saved"));
        return mcp_common::MakeOkReply(ResultObj);
    }

    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("set_hlod_layer_parent"));
}

namespace UEAgentHLODImpl
{
    // Registry query for UHLODLayer assets (subclasses included) anywhere below ContentRoot.
    TArray<FAssetData> FindLayerAssets(const FString& ContentRoot)
    {
        IAssetRegistry& Registry = mcp_common::AssetRegistry();

        FARFilter LayerQuery;
        LayerQuery.ClassPaths.Add(UHLODLayer::StaticClass()->GetClassPathName());
        LayerQuery.bRecursiveClasses = true;
        LayerQuery.PackagePaths.Add(FName(*ContentRoot));
        LayerQuery.bRecursivePaths = true;

        TArray<FAssetData> Matches;
        Registry.GetAssets(LayerQuery, Matches);
        return Matches;
    }

    // { name, path, layer_type } entry for one layer asset. The layer type is a plain UPROPERTY rather
    // than a registry tag, so the asset is loaded to read it (editor builds only; "Unknown" otherwise).
    TSharedPtr<FJsonObject> DescribeLayerAsset(const FAssetData& LayerAsset)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), LayerAsset.AssetName.ToString());
        Entry->SetStringField(TEXT("path"), LayerAsset.GetObjectPathString());

        FString LayerType = TEXT("Unknown");
        if (UHLODLayer* Layer = Cast<UHLODLayer>(LayerAsset.GetAsset()))
        {
            LayerType = HLODLayerTypeToString(Layer->GetLayerType());
        }
        Entry->SetStringField(TEXT("layer_type"), LayerType);
        return Entry;
    }
}

// MCP:get_hlod_layers - List every World Partition HLOD layer asset (UHLODLayer and subclasses) under a content folder and its subfolders, found through the Asset Registry, with each one's name, object path and layer type (Instancing, MeshMerge, MeshSimplify, MeshApproximate, Custom or CustomHLODActor). Each layer asset is loaded to read its type, but nothing is changed or saved. A folder with no layers, or one that does not exist, gives an empty list and count 0, not an error. Use it to find a layer path, then read one layer's grid settings with get_hlod_layer_info or its assigned actors with get_hlod_layer_actors; create_hlod_layer makes a new one and set_properties changes one.
//   @param path - Optional content folder searched recursively, as a long package path, e.g. '/Game/HLOD'; default /Game.
//   @returns { path: string, hlod_layers: [{ name, path, layer_type }], count: int }

TSharedPtr<FJsonObject> HandleGetHLODLayers(const TSharedPtr<FJsonObject>& Params)
{
    // Search root; when no usable string is supplied the project content root is listed.
    FString RootPath;
    if (!Params->TryGetStringField(TEXT("path"), RootPath))
    {
        RootPath = TEXT("/Game");
    }

    TArray<TSharedPtr<FJsonValue>> Layers;
    for (const FAssetData& LayerAsset : UEAgentHLODImpl::FindLayerAssets(RootPath))
    {
        Layers.Add(MakeShared<FJsonValueObject>(UEAgentHLODImpl::DescribeLayerAsset(LayerAsset)));
    }

    TSharedPtr<FJsonObject> Listing = MakeShared<FJsonObject>();
    Listing->SetStringField(TEXT("path"), RootPath);
    Listing->SetArrayField(TEXT("hlod_layers"), Layers);
    Listing->SetNumberField(TEXT("count"), Layers.Num());

    return mcp_common::MakeOkReply(Listing);
}

// MCP:get_hlod_layer_info - Read one World Partition HLOD layer asset (UHLODLayer): its layer type (Instancing, MeshMerge, MeshSimplify, MeshApproximate, Custom or CustomHLODActor) and its streaming-grid settings, cell_size and loading_range in centimetres and is_spatially_loaded. Read-only: it loads the asset and changes nothing. A path that does not load as an HLOD layer is an error. The grid fields are read by reflection because the engine deprecated them in 5.7; on a build without them they read 0 and false. Find layer paths with get_hlod_layers; change these values with set_properties, the parent with set_hlod_layer_parent, and list the actors using the layer with get_hlod_layer_actors.
//   @param hlod_layer_path - Object or package path of the HLOD layer asset, e.g. '/Game/HLOD/HLOD_Instanced' (as get_hlod_layers lists it).
//   @returns { hlod_layer_path: string, asset_path: string, name: string, layer_type: string, cell_size: int, loading_range: number, is_spatially_loaded: bool }

TSharedPtr<FJsonObject> HandleGetHLODLayerInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("hlod_layer_path"), AssetPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UHLODLayer* Layer = LoadHLODLayerAt(AssetPath);
    if (!Layer)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("HLOD layer not found: %s"), *AssetPath));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("hlod_layer_path"), AssetPath);
    ResultObj->SetStringField(TEXT("asset_path"), Layer->GetPathName());
    ResultObj->SetStringField(TEXT("name"), Layer->GetName());
    ResultObj->SetStringField(TEXT("layer_type"), HLODLayerTypeToString(Layer->GetLayerType()));
    ResultObj->SetNumberField(TEXT("cell_size"), GetCellSizeReflected(Layer));
    ResultObj->SetNumberField(TEXT("loading_range"), GetLoadingRangeReflected(Layer));
    ResultObj->SetBoolField(TEXT("is_spatially_loaded"), GetSpatiallyLoaded(Layer));

    return mcp_common::MakeOkReply(ResultObj);
}

// ---------------------------------------------------------------------------
// Actor <-> HLOD layer assignment.
//
// The canonical store is AActor::HLODLayer (TObjectPtr<UHLODLayer>, UPROPERTY
// at Actor.h) with public GetHLODLayer/SetHLODLayer accessors (Actor.h).
// World Partition mirrors this on FWorldPartitionActorDesc::HLODLayer at actor-desc
// construction time (WorldPartitionActorDesc.cpp), so writing the actor-side
// value is the right way to set the assignment.
//
// Strategy: iterate loaded actors via TActorIterator<AActor> and call the public
// AActor::SetHLODLayer API. This is honest: only loaded actors are reachable,
// and SetHLODLayer on a non-WP world still updates AActor::HLODLayer even though
// the engine doesn't make use of it.
//
// Non-WP worlds: the API is callable but the assignment has no engine-side
// effect (no actor desc store). We return not_applicable=true with a clear
// note so the caller knows the assignment won't be persisted to a WP desc.
// ---------------------------------------------------------------------------

namespace
{
    /** Honest gate shared by both handlers: the assignment only persists in WP worlds. */
    bool BuildNonWorldPartitionResponse(const TSharedPtr<FJsonObject>& Resp)
    {
        Resp->SetBoolField(TEXT("not_applicable"), true);
        TArray<TSharedPtr<FJsonValue>> EmptyArr;
        Resp->SetArrayField(TEXT("actors"), EmptyArr);
        Resp->SetArrayField(TEXT("not_found"), EmptyArr);
        Resp->SetArrayField(TEXT("changed_actors"), EmptyArr);
        Resp->SetNumberField(TEXT("total"), 0);
        Resp->SetNumberField(TEXT("changed"), 0);
        Resp->SetNumberField(TEXT("unchanged"), 0);
        Resp->SetNumberField(TEXT("cleared"), 0);
        Resp->SetStringField(TEXT("note"),
            TEXT("Actor <-> HLOD layer assignment only persists on World Partition worlds; current editor world is not partitioned. Set HLODLayer on the actor is still applied in-memory via AActor::SetHLODLayer but the engine will not bake it into FWorldPartitionActorDesc::HLODLayer"));
        return true;
    }
}

// MCP:get_hlod_layer_actors - Enumerate which LOADED actors in the current editor world are assigned to a given UHLODLayer (read-side of set_hlod_layer_actors). Two limits worth knowing before you trust an empty result: it only sees actors currently loaded, and it is WORLD PARTITION ONLY -- on a non-WP world it returns not_applicable=true with a note instead of a list, because there the assignment would not survive a Generate anyway.
//   @param hlod_layer_path - Full /Game path of the UHLODLayer asset (matches against AActor::HLODLayer::GetPathName()).
//   @returns { hlod_layer_path, world, is_world_partition, not_applicable, note?, total: int, actors: [{actor_name, actor_path, actor_label}] }
//   NOTE (impl): non-WP worlds have no FWorldPartitionActorDesc mirror; on WP worlds the engine re-syncs the desc
//   from the actor value at construction time.

TSharedPtr<FJsonObject> HandleGetHLODLayerActors(const TSharedPtr<FJsonObject>& Params)
{
    FString LayerPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("hlod_layer_path"), LayerPath)) { return Missing; }

    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World)) { return NoWorld; }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("hlod_layer_path"), LayerPath);
    Resp->SetStringField(TEXT("world"), World->GetName());
    Resp->SetBoolField(TEXT("is_world_partition"), World->IsPartitionedWorld());

    if (!World->IsPartitionedWorld())
    {
        BuildNonWorldPartitionResponse(Resp);
        return mcp_common::MakeOkReply(Resp);
    }

    // WP path: enumerate loaded actors and filter by AActor::HLODLayer.
    UHLODLayer* TargetLayer = LoadHLODLayerAt(LayerPath);
    if (!TargetLayer)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("get_hlod_layer_actors: could not load UHLODLayer at %s"), *LayerPath));
    }

    TArray<TSharedPtr<FJsonValue>> ActorsArr;
    int32 MatchCount = 0;

    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* Actor = *It;
        if (!Actor || !Actor->IsValidLowLevel()) { continue; }
        if (Actor->GetHLODLayer() != TargetLayer) { continue; }

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("actor_name"), Actor->GetName());
        Entry->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
        Entry->SetStringField(TEXT("actor_path"), Actor->GetPathName());
        ActorsArr.Add(MakeShared<FJsonValueObject>(Entry));
        ++MatchCount;
    }

    Resp->SetBoolField(TEXT("not_applicable"), false);
    Resp->SetNumberField(TEXT("total"), MatchCount);
    Resp->SetArrayField(TEXT("actors"), ActorsArr);
    return mcp_common::MakeOkReply(Resp);
}

// MCP:set_hlod_layer_actors - Assign a UHLODLayer to a BATCH of loaded actors in the current editor world -- or clear it from them by passing hlod_layer_path="". WORLD PARTITION ONLY: on a non-WP world it returns not_applicable=true and changes nothing. Idempotent per actor: one that already has that layer is not touched at all and comes back counted as unchanged, so the changed/unchanged/cleared split tells you exactly what really moved.
//   @param hlod_layer_path - Full /Game path of the UHLODLayer asset. Pass empty string ("") to clear.
//   @param actor_paths - JSON array of actor paths (long package paths or object paths) to assign.
//   @returns { hlod_layer_path, world, is_world_partition, not_applicable, note?, requested: int, changed: int, unchanged: int, cleared: int, not_found: [paths], changed_actors: [{actor_path, old_hlod_layer, new_hlod_layer}] }

TSharedPtr<FJsonObject> HandleSetHLODLayerActors(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return mcp_common::MakeErrorReply(TEXT("set_hlod_layer_actors: params are required"));
    }
    FString LayerPath;
    Params->TryGetStringField(TEXT("hlod_layer_path"), LayerPath);

    const TArray<TSharedPtr<FJsonValue>>* ActorPathsArr = nullptr;
    if (!Params->TryGetArrayField(TEXT("actor_paths"), ActorPathsArr) || !ActorPathsArr)
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: actor_paths (array of strings)"));
    }

    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World)) { return NoWorld; }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("hlod_layer_path"), LayerPath);
    Resp->SetStringField(TEXT("world"), World->GetName());
    Resp->SetBoolField(TEXT("is_world_partition"), World->IsPartitionedWorld());
    Resp->SetNumberField(TEXT("requested"), ActorPathsArr->Num());

    if (!World->IsPartitionedWorld())
    {
        BuildNonWorldPartitionResponse(Resp);
        return mcp_common::MakeOkReply(Resp);
    }

    // Empty layer_path = clear operation; otherwise resolve the asset.
    const bool bIsClear = LayerPath.IsEmpty();
    UHLODLayer* TargetLayer = nullptr;
    if (!bIsClear)
    {
        TargetLayer = LoadHLODLayerAt(LayerPath);
        if (!TargetLayer)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("set_hlod_layer_actors: could not load UHLODLayer at %s"), *LayerPath));
        }
    }

    int32 ChangedCount = 0;
    int32 UnchangedCount = 0;
    int32 ClearedCount = 0;
    TArray<TSharedPtr<FJsonValue>> NotFoundArr;
    TArray<TSharedPtr<FJsonValue>> ChangedActorsArr;

    for (const TSharedPtr<FJsonValue>& V : *ActorPathsArr)
    {
        if (!V.IsValid() || V->Type != EJson::String) { continue; }
        const FString ActorPath = V->AsString();
        if (ActorPath.IsEmpty()) { continue; }

        // Try by path (object path or long package path).
        AActor* Actor = LoadObject<AActor>(nullptr, *ActorPath);
        if (!Actor)
        {
            NotFoundArr.Add(MakeShared<FJsonValueString>(ActorPath));
            continue;
        }

        const UHLODLayer* OldHLOD = Actor->GetHLODLayer();
        if (OldHLOD == TargetLayer)
        {
            ++UnchangedCount;
            continue;
        }

        const FString OldLayerStr = OldHLOD ? OldHLOD->GetPathName() : FString();

        // AActor::SetHLODLayer is the canonical public API (Actor.h). On a WP world,
        // the next actor-desc refresh will mirror this into FWorldPartitionActorDesc::HLODLayer
        // (see WorldPartitionActorDesc.cpp where the desc re-reads from the actor).
        Actor->SetHLODLayer(TargetLayer);
        Actor->Modify();
        ++ChangedCount;
        if (bIsClear) { ++ClearedCount; }

        TSharedPtr<FJsonObject> ChangedEntry = MakeShared<FJsonObject>();
        ChangedEntry->SetStringField(TEXT("actor_path"), Actor->GetPathName());
        ChangedEntry->SetStringField(TEXT("old_hlod_layer"), OldLayerStr);
        ChangedEntry->SetStringField(TEXT("new_hlod_layer"), TargetLayer ? TargetLayer->GetPathName() : FString());
        ChangedActorsArr.Add(MakeShared<FJsonValueObject>(ChangedEntry));
    }

    Resp->SetBoolField(TEXT("not_applicable"), false);
    Resp->SetNumberField(TEXT("changed"), ChangedCount);
    Resp->SetNumberField(TEXT("unchanged"), UnchangedCount);
    Resp->SetNumberField(TEXT("cleared"), ClearedCount);
    Resp->SetArrayField(TEXT("not_found"), NotFoundArr);
    Resp->SetArrayField(TEXT("changed_actors"), ChangedActorsArr);
    return mcp_common::MakeOkReply(Resp);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("create_hlod_layer"), &HandleCreateHLODLayer },
    { TEXT("get_hlod_layer_actors"), &HandleGetHLODLayerActors },
    { TEXT("get_hlod_layer_info"), &HandleGetHLODLayerInfo },
    { TEXT("get_hlod_layers"), &HandleGetHLODLayers },
    { TEXT("set_hlod_layer_actors"), &HandleSetHLODLayerActors },
    { TEXT("set_hlod_layer_parent"), &HandleSetHLODLayerParent },
    { TEXT("set_hlod_layer_settings"), &HandleSetHLODLayerSettings },
};
} // namespace UEAgentHLODCommands
