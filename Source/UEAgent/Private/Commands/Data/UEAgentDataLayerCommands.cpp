// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Data/DataAuthoringUtils.h"

#include "WorldPartition/DataLayer/DataLayerAsset.h"        // UDataLayerAsset, SetType/SetDebugColor/GetType/GetDebugColor/IsRuntime
#include "WorldPartition/DataLayer/DataLayerType.h"          // EDataLayerType (Runtime/Editor/Unknown/Size)
#include "WorldPartition/DataLayer/DataLayerManager.h"       // UDataLayerManager::GetDataLayerManager<T>()
#include "WorldPartition/DataLayer/DataLayerInstance.h"      // UDataLayerInstance

#include "DataLayer/DataLayerEditorSubsystem.h"              // UDataLayerEditorSubsystem (DataLayerEditor module)

#include "Editor.h"                                          // GEditor
#include "Engine/World.h"
#include "EngineUtils.h"                                     // TActorIterator
#include "GameFramework/Actor.h"

#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "EditorAssetLibrary.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"

/**
 * World Partition Data Layer commands.
 *
 * Covers UDataLayerAsset authoring (a plain /Game UDataAsset subclass in the Engine module, no plugin
 * required) and data layer runtime-state authoring/control: the initial state headless via the
 * UDataLayerEditorSubsystem, the live state in PIE via the runtime UDataLayerManager. Data layer instances
 * and actor assignment live in UEAgentWorldPartitionCommands.
 *
 * The runtime state lives on a data layer INSTANCE, which only exists in a World Partition world: on a
 * non-WP map the runtime-state commands return an honest "requires a World Partition map" error rather
 * than faking success.
 */
namespace UEAgentDataLayerCommands
{

// MCP-DOMAIN: Level

namespace
{
    // Parse "Editor"|"Runtime" (case-insensitive). Defaults to Editor.
    EDataLayerType ParseDataLayerType(const FString& In)
    {
        if (In.Equals(TEXT("Runtime"), ESearchCase::IgnoreCase))
        {
            return EDataLayerType::Runtime;
        }
        return EDataLayerType::Editor;
    }

    // Parse "Unloaded"|"Loaded"|"Activated" (case-insensitive) into
    // EDataLayerRuntimeState. Returns false when the string matches none of them so
    // the caller can reject the input with an honest error (no silent default).
    bool ParseDataLayerRuntimeState(const FString& In, EDataLayerRuntimeState& OutState)
    {
        if (In.Equals(TEXT("Unloaded"), ESearchCase::IgnoreCase))
        {
            OutState = EDataLayerRuntimeState::Unloaded;
            return true;
        }
        if (In.Equals(TEXT("Loaded"), ESearchCase::IgnoreCase))
        {
            OutState = EDataLayerRuntimeState::Loaded;
            return true;
        }
        if (In.Equals(TEXT("Activated"), ESearchCase::IgnoreCase))
        {
            OutState = EDataLayerRuntimeState::Activated;
            return true;
        }
        return false;
    }

    // Read a color array param [r,g,b,a] (0-255 ints, alpha optional -> 255) into
    // an FColor. Returns false if the param is missing or malformed.
    bool ReadColorParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, FColor& OutColor)
    {
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (!Params->TryGetArrayField(Field, Arr) || !Arr || Arr->Num() < 3)
        {
            return false;
        }
        auto Clamp255 = [](double V) -> uint8
        {
            return (uint8)FMath::Clamp((int32)FMath::RoundToInt(V), 0, 255);
        };
        OutColor.R = Clamp255((*Arr)[0]->AsNumber());
        OutColor.G = Clamp255((*Arr)[1]->AsNumber());
        OutColor.B = Clamp255((*Arr)[2]->AsNumber());
        OutColor.A = (Arr->Num() >= 4) ? Clamp255((*Arr)[3]->AsNumber()) : 255;
        return true;
    }

    TSharedPtr<FJsonObject> DataLayer_AssetReply(const FString& RequestedPath, const UDataLayerAsset* Asset)
    {
        TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
        Reply->SetStringField(TEXT("data_layer_asset_path"), RequestedPath);
        Reply->SetStringField(TEXT("asset_path"), Asset->GetPathName());
        return Reply;
    }

    // The reply of create_data_layer_asset before its outcome: the request, then the asset the path holds.
    TSharedPtr<FJsonObject> DataLayer_CreatedReply(const FString& Name, const FString& PackagePath, const UDataLayerAsset* Asset)
    {
        TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
        Reply->SetStringField(TEXT("name"), Name);
        Reply->SetStringField(TEXT("package_path"), PackagePath);
        Reply->SetStringField(TEXT("asset_path"), Asset->GetPathName());
        Reply->SetStringField(TEXT("data_layer_type"), DataAuthoringUtils::DataLayerTypeToString(Asset->GetType()));
        Reply->SetBoolField(TEXT("is_runtime"), Asset->IsRuntime());
        Reply->SetArrayField(TEXT("debug_color"), mcp_common::ColorToJsonArray(Asset->GetDebugColor()));
        return Reply;
    }
}

// Data Layer asset authoring.

// MCP:create_data_layer_asset - Create a UDataLayerAsset (the World Partition data layer definition) at package_path/name with its type and debug color, and save it. This is only the asset: put it in a map with create_data_layer_instance and add actors with assign_actor_to_data_layer_instance. data_layer_type is matched without case and anything but Runtime gives Editor; a debug_color missing or shorter than 3 numbers gives the default color. A data layer asset already at that path is returned unchanged (created=false, already_exists=true, saved=false; type and color are not applied) and nothing is saved, so repeat calls are idempotent; another asset class there, an external data layer asset included, or an invalid path, is an error. No undo step is recorded; a failed save, as during PIE, makes success false. Later changes: set_data_layer_type or set_data_layer_debug_color; read back: get_data_layer_assets with data_layer_asset_path; list them: get_data_layer_assets.
//   @param name - Name of the new data layer asset, also its package name under package_path.
//   @param package_path - Target /Game package path (optional, default "/Game/DataLayers")
//   @param data_layer_type - Data layer type: "Editor" or "Runtime" (optional, default "Editor"); matched without case, any other value gives Editor
//   @param debug_color - Optional [r, g, b] or [r, g, b, a] integers 0-255 (defaults to [0, 128, 255, 255]); rounded and clamped, alpha 255 when omitted, a shorter array is ignored
//   @returns { name: string, package_path: string, asset_path: string, data_layer_type: string, is_runtime: bool, debug_color: [number, number, number, number], created: bool, already_exists: bool, success: bool, saved: bool, message: string }

TSharedPtr<FJsonObject> HandleCreateDataLayerAsset(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetName;
    FString PackagePath;
    FString DataLayerTypeStr;

    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), AssetName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (!Params->TryGetStringField(TEXT("package_path"), PackagePath))
    {
        PackagePath = TEXT("/Game/DataLayers");
    }
    if (!Params->TryGetStringField(TEXT("data_layer_type"), DataLayerTypeStr))
    {
        DataLayerTypeStr = TEXT("Editor");
    }

    const EDataLayerType DesiredType = ParseDataLayerType(DataLayerTypeStr);
    const FString FullPath = FString::Printf(TEXT("%s/%s"), *PackagePath, *AssetName);
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(FullPath)) { return Invalid; }

    // Optional debug color (defaults to a stable value for deterministic re-reads).
    FColor DebugColor(0, 128, 255, 255);
    ReadColorParam(Params, TEXT("debug_color"), DebugColor);

    // Idempotent: a data layer asset already there is returned unchanged and nothing is saved. An external data
    // layer asset also loads as a UDataLayerAsset, but it is not the asset asked for, nor is any other class.
    if (UEditorAssetLibrary::DoesAssetExist(FullPath))
    {
        UDataLayerAsset* Existing = DataAuthoringUtils::LoadDataLayerAsset(FullPath);
        if (Existing && Existing->GetClass() == UDataLayerAsset::StaticClass())
        {
            TSharedPtr<FJsonObject> ExistObj = DataLayer_CreatedReply(AssetName, PackagePath, Existing);
            DataAuthoringUtils::AddAlreadyExistsFlags(ExistObj, TEXT("Data layer asset"));
            return mcp_common::MakeOkReply(ExistObj);
        }
        return DataAuthoringUtils::OtherClassAtPathReply(FullPath, Existing, UDataLayerAsset::StaticClass());
    }

    // Create a real /Game package + UDataLayerAsset inside it, then save. The asset
    // has no inner objects, so direct NewObject is correct (avoids any factory).
    FString CreateError;
    UDataLayerAsset* NewAsset = mcp_common::NewAssetInPackage<UDataLayerAsset>(FullPath, AssetName, TEXT("Failed to create UDataLayerAsset"), CreateError);
    if (!NewAsset)
    {
        return mcp_common::MakeErrorReply(CreateError);
    }

    // SetType / SetDebugColor are WITH_EDITOR only on UDataLayerAsset.
    NewAsset->SetType(DesiredType);
    NewAsset->SetDebugColor(DebugColor);

    FAssetRegistryModule::AssetCreated(NewAsset);

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(NewAsset->GetOutermost(), NewAsset, SaveError);

    // Read back from the live object, which reflects the setters even pre-save.
    TSharedPtr<FJsonObject> ResultObj = DataLayer_CreatedReply(AssetName, PackagePath, NewAsset);
    DataAuthoringUtils::AddCreatedOutcome(ResultObj, bSaved, TEXT("Data layer asset"), SaveError);

    // The verdict measured above is the verdict EMITTED. A hardcoded success:true
    // from MakeOkReply would hide it, and callers only check that flag.
    return mcp_common::MakeOutcomeReply(ResultObj, bSaved,
        bSaved ? FString() : mcp_common::MeasuredOutcomeError(TEXT("create_data_layer_asset"), TEXT("bSaved")));
}

// MCP:set_data_layer_debug_color - Set the debug color of a World Partition data layer asset (UDataLayerAsset), the color the Data Layers outliner and the World Partition debug views show for that layer, in one undo step, then save the asset and read the color back (color_set). Setting the color it already has writes and saves it again with the same result. A path that does not load as a data layer asset or a color that is not an array of 3 or 4 numbers is an error and nothing changes; a failed save is an error, the change kept in memory. Find layer assets with get_data_layer_assets; create one with its color in one call with create_data_layer_asset; change its Editor or Runtime type with set_data_layer_type.
//   @param data_layer_asset_path - Object or package path of the data layer asset, e.g. '/Game/DataLayers/DL_Interiors'.
//   @param color [array!] - [r, g, b] or [r, g, b, a] in 0-255, e.g. [255, 128, 0]; values are rounded and clamped to 0-255, and alpha defaults to 255.
//   @returns { data_layer_asset_path: string, asset_path: string, debug_color: [number, number, number, number], data_layer_type: string, color_set: bool, saved: bool }

TSharedPtr<FJsonObject> HandleSetDataLayerDebugColor(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("data_layer_asset_path"), AssetPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    FColor NewColor;
    if (!ReadColorParam(Params, TEXT("color"), NewColor))
    {
        return mcp_common::MakeErrorReply(
            TEXT("Missing or invalid required parameter: color (array of [r,g,b] or [r,g,b,a], 0-255)"));
    }

    UDataLayerAsset* Asset = nullptr;
    if (TSharedPtr<FJsonObject> Failure = DataAuthoringUtils::OpenDataLayerAsset(AssetPath, Asset))
    {
        return Failure;
    }

    const FScopedTransaction Transaction(FText::FromString(TEXT("MCP: Set Data Layer Debug Color")));
    Asset->Modify();
    Asset->SetDebugColor(NewColor);

    Asset->MarkPackageDirty();

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Asset->GetOutermost(), Asset, SaveError);

    // Verify by readback.
    const FColor ReadColor = Asset->GetDebugColor();

    TSharedPtr<FJsonObject> ResultObj = DataLayer_AssetReply(AssetPath, Asset);
    ResultObj->SetArrayField(TEXT("debug_color"), mcp_common::ColorToJsonArray(ReadColor));
    ResultObj->SetStringField(TEXT("data_layer_type"), DataAuthoringUtils::DataLayerTypeToString(Asset->GetType()));
    ResultObj->SetBoolField(TEXT("color_set"), ReadColor == NewColor);

    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("set_data_layer_debug_color"), SaveError);
}

// MCP:set_data_layer_type - Sets the EDataLayerType ("Editor" or "Runtime") of a UDataLayerAsset and saves it.  Editor-only authoring on the data asset itself (no world required); mirrors set_data_layer_debug_color.
//   @param data_layer_asset_path - Full /Game path to the data layer asset.
//   @param data_layer_type - "Editor" or "Runtime".
//   @returns { data_layer_asset_path: string, asset_path: string, data_layer_type: string, is_runtime: bool, type_set: bool, saved: bool }

TSharedPtr<FJsonObject> HandleSetDataLayerType(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("data_layer_asset_path"), AssetPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    FString TypeStr;
    if (!Params->TryGetStringField(TEXT("data_layer_type"), TypeStr))
    {
        return mcp_common::MakeErrorReply(
            TEXT("Missing required parameter: data_layer_type (\"Editor\" or \"Runtime\")"));
    }
    if (!TypeStr.Equals(TEXT("Editor"), ESearchCase::IgnoreCase) && !TypeStr.Equals(TEXT("Runtime"), ESearchCase::IgnoreCase))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Invalid data_layer_type '%s' (expected \"Editor\" or \"Runtime\")"), *TypeStr));
    }
    const EDataLayerType DesiredType = ParseDataLayerType(TypeStr);

    UDataLayerAsset* Asset = nullptr;
    if (TSharedPtr<FJsonObject> Failure = DataAuthoringUtils::OpenDataLayerAsset(AssetPath, Asset))
    {
        return Failure;
    }

    const FScopedTransaction Transaction(FText::FromString(TEXT("MCP: Set Data Layer Type")));
    Asset->Modify();
    Asset->SetType(DesiredType);

    Asset->MarkPackageDirty();

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Asset->GetOutermost(), Asset, SaveError);

    // Verify by readback (live object reflects the setter even pre-save).
    const EDataLayerType ReadType = Asset->GetType();

    TSharedPtr<FJsonObject> ResultObj = DataLayer_AssetReply(AssetPath, Asset);
    ResultObj->SetStringField(TEXT("data_layer_type"), DataAuthoringUtils::DataLayerTypeToString(ReadType));
    ResultObj->SetBoolField(TEXT("is_runtime"), Asset->IsRuntime());
    ResultObj->SetBoolField(TEXT("type_set"), ReadType == DesiredType);

    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("set_data_layer_type"), SaveError);
}

// MCP:get_data_layer_assets - Lists the UDataLayerAsset assets (subclasses included) under a content folder and all its sub-folders, found through the Asset Registry, or reads exactly one with data_layer_asset_path; each entry carries name, object path, type (Editor, Runtime or Unknown), the runtime flags and the debug color. Every match is loaded to read them; nothing is modified and the list is not paginated. These are layer assets, not the layers of the open World Partition map: list those with get_data_layer_instances.
//   @param path - Content path to search (optional, default "/Game", recursive). Ignored when data_layer_asset_path is given.
//   @param data_layer_asset_path - Optional full path of ONE data layer asset to read instead of searching; an asset that does not load as a UDataLayerAsset is an error.
//   @returns { path: string, data_layer_asset_path?: string (echo, single-asset mode), data_layer_assets: [{ name: string, path: string, type: string, is_runtime?: bool, is_private?: bool, supports_actor_filters?: bool, debug_color?: [number, number, number, number] }], count: number }

TSharedPtr<FJsonObject> HandleGetDataLayerAssets(const TSharedPtr<FJsonObject>& Params)
{
    // Content folder to search (sub-folders included); "/Game" when the key is absent.
    FString Root;
    if (!Params->TryGetStringField(TEXT("path"), Root))
    {
        Root = TEXT("/Game");
    }
    // Or exactly one asset, read by its path.
    FString SinglePath;
    const bool bSingle = Params->TryGetStringField(TEXT("data_layer_asset_path"), SinglePath);

    // One entry: its name, path and type, and, when the asset loads, its flags and debug color.
    auto DescribeLayer = [](const FString& Name, const FString& Path, const UDataLayerAsset* Layer)
    {
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("name"), Name);
        Row->SetStringField(TEXT("path"), Path);
        Row->SetStringField(TEXT("type"), Layer != nullptr ? DataAuthoringUtils::DataLayerTypeToString(Layer->GetType()) : TEXT("Unknown"));
        if (Layer != nullptr)
        {
            Row->SetBoolField(TEXT("is_runtime"), Layer->IsRuntime());
            Row->SetBoolField(TEXT("is_private"), Layer->IsPrivate());
            Row->SetBoolField(TEXT("supports_actor_filters"), Layer->SupportsActorFilters());
            Row->SetArrayField(TEXT("debug_color"), mcp_common::ColorToJsonArray(Layer->GetDebugColor()));
        }
        return MakeShared<FJsonValueObject>(Row);
    };

    TArray<TSharedPtr<FJsonValue>> Rows;
    if (bSingle)
    {
        UDataLayerAsset* Asset = nullptr;
        if (TSharedPtr<FJsonObject> Failure = DataAuthoringUtils::OpenDataLayerAsset(SinglePath, Asset))
        {
            return Failure;
        }
        Root = SinglePath;
        Rows.Add(DescribeLayer(Asset->GetName(), Asset->GetPathName(), Asset));
    }
    else
    {
        // Every UDataLayerAsset, subclasses included, under that folder tree.
        FARFilter Query;
        Query.ClassPaths.Add(UDataLayerAsset::StaticClass()->GetClassPathName());
        Query.PackagePaths.Add(FName(*Root));
        Query.bRecursivePaths = true;
        Query.bRecursiveClasses = true;

        TArray<FAssetData> Found;
        mcp_common::AssetRegistry().GetAssets(Query, Found);

        Rows.Reserve(Found.Num());
        for (const FAssetData& Entry : Found)
        {
            // The registry does not expose the layer type as a tag (it is a plain UPROPERTY),
            // so the asset is loaded to read it.
            Rows.Add(DescribeLayer(Entry.AssetName.ToString(), Entry.GetObjectPathString(), Cast<UDataLayerAsset>(Entry.GetAsset())));
        }
    }

    TSharedPtr<FJsonObject> Listing = MakeShared<FJsonObject>();
    Listing->SetStringField(TEXT("path"), Root);
    if (bSingle)
    {
        Listing->SetStringField(TEXT("data_layer_asset_path"), SinglePath);
    }
    Listing->SetArrayField(TEXT("data_layer_assets"), Rows);
    Listing->SetNumberField(TEXT("count"), Rows.Num());
    return mcp_common::MakeOkReply(Listing);
}

// Runtime-state authoring/control of World Partition data layer instances.

// MCP:set_data_layer_initial_runtime_state - Authors the initial runtime state ("Unloaded"|"Loaded"|"Activated") of a runtime data layer's instance in the current World Partition map, creating the instance from the asset if missing.  Headless authoring (no PIE); the state takes effect when the level streams in. Requires a World Partition map (honest error otherwise).
//   @param data_layer_asset_path - Full /Game path to the (runtime) data layer asset.
//   @param initial_runtime_state - "Unloaded", "Loaded" or "Activated".
//   @param create_instance_if_missing - Create the data layer instance from the asset if none exists yet (optional, default true).
//   @returns { data_layer_asset_path: string, initial_runtime_state: string, is_runtime: bool, already_existed: bool, state_set: bool, saved: bool }

TSharedPtr<FJsonObject> HandleSetDataLayerInitialRuntimeState(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("data_layer_asset_path"), AssetPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    FString StateStr;
    if (!Params->TryGetStringField(TEXT("initial_runtime_state"), StateStr))
    {
        return mcp_common::MakeErrorReply(
            TEXT("Missing required parameter: initial_runtime_state (\"Unloaded\", \"Loaded\" or \"Activated\")"));
    }
    EDataLayerRuntimeState DesiredState = EDataLayerRuntimeState::Unloaded;
    if (!ParseDataLayerRuntimeState(StateStr, DesiredState))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Invalid initial_runtime_state '%s' (expected \"Unloaded\", \"Loaded\" or \"Activated\")"), *StateStr));
    }

    bool bCreateIfMissing = true;
    Params->TryGetBoolField(TEXT("create_instance_if_missing"), bCreateIfMissing);

    UDataLayerAsset* Asset = nullptr;
    if (TSharedPtr<FJsonObject> Failure = DataAuthoringUtils::OpenDataLayerAsset(AssetPath, Asset))
    {
        return Failure;
    }

    // HONEST gate: the initial runtime state lives on a data layer INSTANCE, which
    // only exists inside a World Partition map. On a non-WP map there is no manager
    // and no instance to author, so refuse rather than fake success.
    UWorld* World = nullptr;
    UDataLayerManager* Manager = nullptr;
    if (TSharedPtr<FJsonObject> Failure = DataAuthoringUtils::OpenEditorDataLayerManager(
            TEXT("Setting the initial runtime state requires a World Partition map (no data layer manager in this world)"), World, Manager))
    {
        return Failure;
    }

    UDataLayerEditorSubsystem* Subsystem = UDataLayerEditorSubsystem::Get();
    if (!Subsystem)
    {
        return mcp_common::MakeErrorReply(TEXT("Data layer editor subsystem unavailable"));
    }

    const FScopedTransaction Transaction(FText::FromString(TEXT("MCP: Set Data Layer Initial Runtime State")));

    // Find (or, if requested, create) the instance that references this asset.
    UDataLayerInstance* Instance = Subsystem->GetDataLayerInstance(Asset);
    bool bAlreadyExisted = (Instance != nullptr);
    if (!Instance)
    {
        if (!bCreateIfMissing)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("No data layer instance references asset '%s' and create_instance_if_missing is false"), *AssetPath));
        }
        FDataLayerCreationParameters CreationParams;
        CreationParams.DataLayerAsset = Asset;
        Instance = Subsystem->CreateDataLayerInstance(CreationParams);
        if (!Instance)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Failed to create data layer instance for asset '%s'"), *AssetPath));
        }
    }

    // The initial runtime state is only meaningful for RUNTIME data layers. Refuse
    // honestly for an editor-type instance instead of silently no-op'ing.
    if (!Instance->IsRuntime())
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Data layer '%s' is not a runtime data layer; set its type to Runtime first"), *AssetPath));
    }

    // SetDataLayerInitialRuntimeState is void; verify by reading the state back.
    Subsystem->SetDataLayerInitialRuntimeState(Instance, DesiredState);
    const EDataLayerRuntimeState ReadState = Instance->GetInitialRuntimeState();

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(World->GetOutermost(), World, SaveError);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("data_layer_asset_path"), AssetPath);
    ResultObj->SetStringField(TEXT("initial_runtime_state"), GetDataLayerRuntimeStateName(ReadState));
    ResultObj->SetBoolField(TEXT("is_runtime"), Instance->IsRuntime());
    ResultObj->SetBoolField(TEXT("already_existed"), bAlreadyExisted);
    ResultObj->SetBoolField(TEXT("state_set"), ReadState == DesiredState);

    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("set_data_layer_initial_runtime_state"), SaveError);
}

// MCP:set_data_layer_runtime_state - Sets the LIVE runtime state ("Unloaded"|"Loaded"|"Activated") of a runtime data layer in a running World Partition game (PIE).  Requires a World Partition map; outside PIE this is an honest gate (applied:false + note pointing to set_data_layer_initial_runtime_state) rather than a failure.
//   @param data_layer_asset_path - Full /Game path to the (runtime) data layer asset.
//   @param runtime_state - "Unloaded", "Loaded" or "Activated".
//   @param is_recursive - Also apply to child data layers (optional, default false).
//   @returns { data_layer_asset_path: string, runtime_state: string, applied: bool, success: bool, note: string }

TSharedPtr<FJsonObject> HandleSetDataLayerRuntimeState(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("data_layer_asset_path"), AssetPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    FString StateStr;
    if (!Params->TryGetStringField(TEXT("runtime_state"), StateStr))
    {
        return mcp_common::MakeErrorReply(
            TEXT("Missing required parameter: runtime_state (\"Unloaded\", \"Loaded\" or \"Activated\")"));
    }
    EDataLayerRuntimeState DesiredState = EDataLayerRuntimeState::Unloaded;
    if (!ParseDataLayerRuntimeState(StateStr, DesiredState))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Invalid runtime_state '%s' (expected \"Unloaded\", \"Loaded\" or \"Activated\")"), *StateStr));
    }

    bool bRecursive = false;
    Params->TryGetBoolField(TEXT("is_recursive"), bRecursive);

    UDataLayerAsset* Asset = nullptr;
    if (TSharedPtr<FJsonObject> Failure = DataAuthoringUtils::OpenDataLayerAsset(AssetPath, Asset))
    {
        return Failure;
    }

    // HONEST gate #1: live runtime state requires a World Partition map (manager).
    UWorld* World = nullptr;
    UDataLayerManager* Manager = nullptr;
    if (TSharedPtr<FJsonObject> Failure = DataAuthoringUtils::OpenEditorDataLayerManager(
            TEXT("Setting the live runtime state requires a World Partition map (no data layer manager in this world)"), World, Manager))
    {
        return Failure;
    }

    // HONEST gate #2: the live runtime state only exists in a running game (PIE).
    // In the editor world (not a game world) there is nothing to stream, so we
    // return a non-fatal gate (applied:false + success:true + note) instead of
    // faking a state change. Authoring headless is done via the initial state.
    if (!World->IsGameWorld())
    {
        TSharedPtr<FJsonObject> GateObj = MakeShared<FJsonObject>();
        GateObj->SetStringField(TEXT("data_layer_asset_path"), AssetPath);
        GateObj->SetStringField(TEXT("runtime_state"), GetDataLayerRuntimeStateName(DesiredState));
        GateObj->SetBoolField(TEXT("applied"), false);
        GateObj->SetBoolField(TEXT("success"), true);
        GateObj->SetStringField(TEXT("note"),
            TEXT("Live data layer runtime state requires a running World Partition game (PIE). Use set_data_layer_initial_runtime_state to author the initial state headless."));
        return mcp_common::MakeOkReply(GateObj);
    }

    // Running PIE world: apply the live runtime state via the manager (asset overload).
    const bool bApplied = Manager->SetDataLayerRuntimeState(Asset, DesiredState, bRecursive);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("data_layer_asset_path"), AssetPath);
    ResultObj->SetStringField(TEXT("runtime_state"), GetDataLayerRuntimeStateName(DesiredState));
    ResultObj->SetBoolField(TEXT("applied"), bApplied);
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetStringField(TEXT("note"), bApplied
        ? TEXT("Live runtime state applied in the running game")
        : TEXT("No data layer instance for this asset in the running game; nothing applied"));

    return mcp_common::MakeOkReply(ResultObj);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("create_data_layer_asset"), &HandleCreateDataLayerAsset },
    { TEXT("get_data_layer_assets"), &HandleGetDataLayerAssets },
    { TEXT("set_data_layer_debug_color"), &HandleSetDataLayerDebugColor },
    { TEXT("set_data_layer_initial_runtime_state"), &HandleSetDataLayerInitialRuntimeState },
    { TEXT("set_data_layer_runtime_state"), &HandleSetDataLayerRuntimeState },
    { TEXT("set_data_layer_type"), &HandleSetDataLayerType },
};
} // namespace UEAgentDataLayerCommands
