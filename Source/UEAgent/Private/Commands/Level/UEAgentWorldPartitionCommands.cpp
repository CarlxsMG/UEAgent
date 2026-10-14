#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Common/UEAgentGraphUtils.h"             // FUEAgentGraphUtils::ResolveClass (filter_class resolution)
#include "Commands/Data/DataAuthoringUtils.h"
#include "UObject/TopLevelAssetPath.h"                           // FTopLevelAssetPath (native/base class of an actor descriptor)

#include "WorldPartition/WorldPartition.h"                       // UWorldPartition, RuntimeHash, IsStreamingEnabled, GetDataLayerManager, CreateEditorLoaderAdapter
#include "WorldPartition/WorldPartitionRuntimeHash.h"            // UWorldPartitionRuntimeHash (complete type for RuntimeHash->GetClass())
#include "WorldPartition/WorldPartitionRuntimeSpatialHash.h"     // UWorldPartitionRuntimeSpatialHash::GetNumGrids (generated streaming grids)
#include "WorldPartition/ActorDescContainerInstanceCollection.h" // FActorDescContainerInstanceCollection::TIterator<> (walk descriptors without loading)
#include "WorldPartition/WorldPartitionActorDescInstance.h"      // FWorldPartitionActorDescInstance (GetActorSoftPath/GetNativeClass/GetBaseClass/GetEditorBounds/GetRuntimeBounds/GetIsSpatiallyLoaded/GetDataLayers)
#include "WorldPartition/WorldPartitionEditorLoaderAdapter.h"    // UWorldPartitionEditorLoaderAdapter (region pin holder)
#include "WorldPartition/WorldPartitionActorLoaderInterface.h"   // IWorldPartitionActorLoaderInterface::ILoaderAdapter::Load/IsLoaded
#include "WorldPartition/LoaderAdapter/LoaderAdapterShape.h"     // FLoaderAdapterShape (spatial box loader adapter)
#include "WorldPartition/DataLayer/DataLayerManager.h"           // UDataLayerManager::GetDataLayerManager / ForEachDataLayerInstance
#include "WorldPartition/DataLayer/DataLayerInstance.h"          // UDataLayerInstance (GetDataLayerShortName/FullName/FName/GetType/IsRuntime/GetAsset)
#include "WorldPartition/DataLayer/DataLayerType.h"              // EDataLayerType
#include "WorldPartition/DataLayer/DataLayerAsset.h"             // UDataLayerAsset
#include "WorldPartition/WorldPartitionMiniMap.h"                // AWorldPartitionMiniMap (read-only already_has_minimap probe)

#include "DataLayer/DataLayerEditorSubsystem.h"                  // UDataLayerEditorSubsystem + FDataLayerCreationParameters (DataLayerEditor module)

#include "Editor.h"                                              // GEditor
#include "Editor/EditorEngine.h"                                 // UEditorEngine::InitBuilderBrush
#include "Engine/World.h"                                        // UWorld, UWorld::CreateWorld, GetWorldPartition
#include "Engine/WorldInitializationValues.h"                    // FWorldInitializationValues
#include "EngineUtils.h"                                         // TActorIterator
#include "GameFramework/Actor.h"
#include "ThumbnailRendering/WorldThumbnailInfo.h"               // UWorldThumbnailInfo (mirror UWorldFactory)

#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
// list/remove_orphaned_ofpa_actors
#include "Engine/Level.h"                                        // ULevel::GetExternalActorsPath
#include "HAL/FileManager.h"                                     // IFileManager::FindFilesRecursive/Delete (raw filesystem scan + deletion fallback)
#include "Misc/Paths.h"                                          // FPaths::ChangeExtension/FileExists (sibling .uexp/.ubulk handling)

/**
 * World Partition and Data Layer instance commands.
 *
 * UEAgentDataLayerCommands authors the UDataLayerAsset assets; this family works on the world-level
 * structures of the editor world: its UWorldPartition, its UDataLayerManager and the UDataLayerInstance
 * objects instanced into it, plus the __ExternalActors__ (OFPA) packages of a level.
 *
 * The editor world is never swapped. On a map without World Partition the reads report it as disabled (or
 * return an empty list), and the commands that need it fail with an explicit error instead of faking
 * success. convert_map_to_world_partition, build_minimap and set_world_partition_runtime_hash only inspect:
 * the real operation is commandlet-only or happens at cook/build time, so they mutate nothing.
 */
namespace UEAgentWorldPartitionCommands
{

// MCP-DOMAIN: Level

namespace
{
    // Serialize an FBox as { valid, origin:[x,y,z], extent:[x,y,z] }. origin/extent are only
    // meaningful when the box is valid; GetCenter/GetExtent on an invalid box would report
    // zeros, so we emit them only when IsValid.
    TSharedPtr<FJsonObject> WPBoxToJson(const FBox& Box)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        const bool bValid = (Box.IsValid != 0);
        Obj->SetBoolField(TEXT("valid"), bValid);
        if (bValid)
        {
            Obj->SetArrayField(TEXT("origin"), mcp_common::VectorToJsonArray(Box.GetCenter()));
            Obj->SetArrayField(TEXT("extent"), mcp_common::VectorToJsonArray(Box.GetExtent()));
        }
        return Obj;
    }

    // Serialize one data layer instance into a JSON object.
    TSharedPtr<FJsonObject> DataLayerInstanceToJson(UDataLayerInstance* Instance)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        if (!Instance)
        {
            return Obj;
        }
        Obj->SetStringField(TEXT("instance_name"), Instance->GetFName().ToString());
        Obj->SetStringField(TEXT("short_name"), Instance->GetDataLayerShortName());
        Obj->SetStringField(TEXT("full_name"), Instance->GetDataLayerFullName());
        Obj->SetStringField(TEXT("data_layer_type"), DataAuthoringUtils::DataLayerTypeToString(Instance->GetType()));
        Obj->SetBoolField(TEXT("is_runtime"), Instance->IsRuntime());
        if (const UDataLayerAsset* Asset = Instance->GetAsset())
        {
            Obj->SetStringField(TEXT("asset_path"), Asset->GetPathName());
        }
        return Obj;
    }
}

// Steps the World Partition commands share.
namespace UEAgentWorldPartitionCommandsImpl
{
    TSharedPtr<FJsonObject> ActorNotInEditorWorldReply(const FString& NameOrLabel)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Actor '%s' not found in the editor world"), *NameOrLabel));
    }

    // What assign/remove_actor_from_data_layer_instance act on.
    struct FActorLayerTarget
    {
        FString AssetPath;
        AActor* Actor = nullptr;
        UDataLayerEditorSubsystem* Subsystem = nullptr;
        UDataLayerInstance* Instance = nullptr;
    };

    // Reads actor_name and data_layer_asset_path and resolves them in the editor world; the error reply of the first step
    // that fails (NoManagerError without World Partition, "...; <NoInstanceAdvice>" when no instance uses the asset).
    TSharedPtr<FJsonObject> ResolveActorLayerTarget(const TSharedPtr<FJsonObject>& Params, const TCHAR* NoManagerError,
        const TCHAR* NoInstanceAdvice, FActorLayerTarget& Out)
    {
        FString Wanted;
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), Wanted, mcp_common::EAllowEmpty::Yes)) { return Missing; }
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("data_layer_asset_path"), Out.AssetPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

        UDataLayerAsset* Asset = nullptr;
        if (TSharedPtr<FJsonObject> Failure = DataAuthoringUtils::OpenDataLayerAsset(Out.AssetPath, Asset))
        {
            return Failure;
        }

        UWorld* World = nullptr;
        if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World)) { return NoWorld; }

        Out.Actor = mcp_common::FindActorByName(World, Wanted);
        if (Out.Actor == nullptr)
        {
            return ActorNotInEditorWorldReply(Wanted);
        }

        // HONEST gate: binding an actor to a data layer instance requires a World Partition world.
        if (UDataLayerManager::GetDataLayerManager(World) == nullptr)
        {
            return mcp_common::MakeErrorReply(NoManagerError);
        }

        Out.Subsystem = UDataLayerEditorSubsystem::Get();
        if (!Out.Subsystem)
        {
            return mcp_common::MakeErrorReply(TEXT("Data layer editor subsystem unavailable"));
        }

        Out.Instance = Out.Subsystem->GetDataLayerInstance(Asset);
        if (!Out.Instance)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("No data layer instance references asset '%s' in this world; %s"), *Out.AssetPath, NoInstanceAdvice));
        }
        return nullptr;
    }

    // The actor, by its LABEL as the outliner shows it, the data layer asset and the instance referencing it.
    TSharedPtr<FJsonObject> ActorLayerReply(const AActor* Actor, const FString& AssetPath, const UDataLayerInstance* Instance)
    {
        TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
        Reply->SetStringField(TEXT("actor_name"), Actor->GetActorLabel());
        Reply->SetStringField(TEXT("data_layer_asset_path"), AssetPath);
        Reply->SetStringField(TEXT("instance_name"), Instance->GetFName().ToString());
        Reply->SetStringField(TEXT("short_name"), Instance->GetDataLayerShortName());
        return Reply;
    }

    // The class name of the runtime hash of WorldPartition, "None" when there is none.
    FString RuntimeHashTypeName(const UWorldPartition* WorldPartition)
    {
        if (WorldPartition == nullptr || WorldPartition->RuntimeHash == nullptr)
        {
            return TEXT("None");
        }
        return WorldPartition->RuntimeHash->GetClass()->GetName();
    }
}

// MCP:get_world_partition_info - Reads the editor world's World Partition state.  REAL read: works on any map. Returns is_world_partition_enabled=false on the default (non-WP) editor map, which is the correct result.
//   @returns { world_name: string, is_world_partition_enabled: bool, num_data_layers: int, runtime_hash_type: string, is_streaming_enabled: bool, note: string }

TSharedPtr<FJsonObject> HandleGetWorldPartitionInfo(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World)) { return NoWorld; }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("world_name"), World->GetName());

    UWorldPartition* WorldPartition = World->GetWorldPartition();
    const bool bEnabled = (WorldPartition != nullptr);
    ResultObj->SetBoolField(TEXT("is_world_partition_enabled"), bEnabled);

    int32 NumDataLayers = 0;
    bool bStreamingEnabled = false;

    if (WorldPartition)
    {
        bStreamingEnabled = WorldPartition->IsStreamingEnabled();
        if (UDataLayerManager* Manager = WorldPartition->GetDataLayerManager())
        {
            Manager->ForEachDataLayerInstance([&NumDataLayers](UDataLayerInstance* /*Instance*/)
            {
                ++NumDataLayers;
                return true; // continue
            });
        }
    }

    ResultObj->SetNumberField(TEXT("num_data_layers"), NumDataLayers);
    ResultObj->SetStringField(TEXT("runtime_hash_type"), UEAgentWorldPartitionCommandsImpl::RuntimeHashTypeName(WorldPartition));
    ResultObj->SetBoolField(TEXT("is_streaming_enabled"), bStreamingEnabled);
    ResultObj->SetStringField(TEXT("note"), bEnabled
        ? TEXT("Editor world is World Partition enabled")
        : TEXT("Editor world is NOT World Partition (default map); enabled=false is the correct read"));

    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:create_world_partition_map - Creates and saves a new World Partition-enabled UWorld map asset on disk.  The editor world is NOT swapped: a new Inactive UWorld is created inside a /Game package with World Partition enabled (mirroring UWorldFactory's headless creation path) and saved as a .umap.
//   @param name - Name of the new map.
//   @param package_path - Destination /Game folder (default /Game/Maps).
//   @param enable_streaming - bool, enable World Partition streaming (default true).
//   @returns { name: string, package_path: string, map_path: string, world_partition_enabled: bool, runtime_hash_type: string, created: bool, already_exists: bool, saved: bool, message: string, note: string }

TSharedPtr<FJsonObject> HandleCreateWorldPartitionMap(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath;
    FString MapName;

    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), MapName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (!Params->TryGetStringField(TEXT("package_path"), PackagePath))
    {
        PackagePath = TEXT("/Game/Maps");
    }

    bool bEnableStreaming = true;
    Params->TryGetBoolField(TEXT("enable_streaming"), bEnableStreaming);

    const FString FullPath = FString::Printf(TEXT("%s/%s"), *PackagePath, *MapName);
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(FullPath)) { return Invalid; }

    // Idempotent: if the map already exists, load it and return a STABLE payload.
    if (UEditorAssetLibrary::DoesAssetExist(FullPath))
    {
        UWorld* ExistingWorld = Cast<UWorld>(StaticLoadObject(UWorld::StaticClass(), nullptr, *FullPath));
        const bool bExistingWP = ExistingWorld && (ExistingWorld->GetWorldPartition() != nullptr);

        TSharedPtr<FJsonObject> ExistObj = MakeShared<FJsonObject>();
        ExistObj->SetStringField(TEXT("name"), MapName);
        ExistObj->SetStringField(TEXT("package_path"), PackagePath);
        ExistObj->SetStringField(TEXT("map_path"), FullPath);
        ExistObj->SetBoolField(TEXT("world_partition_enabled"), bExistingWP);
        DataAuthoringUtils::AddAlreadyExistsFlags(ExistObj);
        ExistObj->SetStringField(TEXT("note"), TEXT("Map already exists; returned stable payload"));
        return mcp_common::MakeOkReply(ExistObj);
    }

    // Create a real /Game package to host the new UWorld map asset.
    UPackage* Package = CreatePackage(*FullPath);
    if (!Package)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Failed to create package: %s"), *FullPath));
    }
    Package->FullyLoad();

    // Headless-safe WP world creation, mirroring UWorldFactory::FactoryCreateNew:
    // build an Inactive UWorld inside the package with CreateWorldPartition(true),
    // WITHOUT informing the engine or swapping the editor world.
    FWorldInitializationValues InitValues = FWorldInitializationValues()
        .ShouldSimulatePhysics(false)
        .EnableTraceCollision(true)
        .CreateNavigation(false)
        .CreateAISystem(false)
        .CreateWorldPartition(true)
        .EnableWorldPartitionStreaming(bEnableStreaming);

    UWorld* NewWorld = UWorld::CreateWorld(
        EWorldType::Inactive,
        /*bInformEngineOfWorld=*/false,
        FName(*MapName),
        Package,
        /*bAddToRoot=*/false,
        ERHIFeatureLevel::Num,
        &InitValues);

    if (!NewWorld)
    {
        return mcp_common::MakeErrorReply(TEXT("UWorld::CreateWorld returned null"));
    }

    if (GEditor)
    {
        GEditor->InitBuilderBrush(NewWorld);
    }
    NewWorld->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
    NewWorld->ThumbnailInfo = NewObject<UWorldThumbnailInfo>(NewWorld, NAME_None, RF_Transactional);

    // Verify by readback: GetWorldPartition() is non-null when WP creation succeeded.
    UWorldPartition* WorldPartition = NewWorld->GetWorldPartition();
    const bool bWorldPartitionEnabled = (WorldPartition != nullptr);

    FAssetRegistryModule::AssetCreated(NewWorld);

    // Persist as a .umap (map package extension) on disk.
    FString FileName;
    FString SaveError;
    bool bSaved = false;
    if (FPackageName::TryConvertLongPackageNameToFilename(
            Package->GetName(), FileName, FPackageName::GetMapPackageExtension()))
    {
        Package->MarkPackageDirty();

        FSavePackageArgs SaveArgs;
        SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
        SaveArgs.SaveFlags = SAVE_NoError;
        SaveArgs.Error = GError;

        bSaved = UPackage::SavePackage(Package, NewWorld, *FileName, SaveArgs);
        if (!bSaved)
        {
            SaveError = FString::Printf(TEXT("Failed to save map package to disk: %s"), *FileName);
        }
    }
    else
    {
        SaveError = FString::Printf(TEXT("Could not convert package name to map filename: %s"), *Package->GetName());
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("name"), MapName);
    ResultObj->SetStringField(TEXT("package_path"), PackagePath);
    ResultObj->SetStringField(TEXT("map_path"), FullPath);
    ResultObj->SetBoolField(TEXT("world_partition_enabled"), bWorldPartitionEnabled);
    ResultObj->SetStringField(TEXT("runtime_hash_type"), UEAgentWorldPartitionCommandsImpl::RuntimeHashTypeName(WorldPartition));
    ResultObj->SetBoolField(TEXT("created"), true);
    ResultObj->SetBoolField(TEXT("already_exists"), false);
    ResultObj->SetStringField(TEXT("message"), bSaved
        ? (bWorldPartitionEnabled
            ? TEXT("World Partition map created and saved (editor world NOT swapped)")
            : TEXT("Map created and saved but World Partition was not enabled on it"))
        : FString::Printf(TEXT("Map created but save failed: %s"), *SaveError));

    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("create_world_partition_map"));
}

// MCP:create_data_layer_instance - Instances a UDataLayerAsset into the editor world's World Partition.  Uses UDataLayerEditorSubsystem::CreateDataLayerInstance. Requires the editor world to be a World Partition world; on the default non-WP map this returns an honest "requires world partition" error (no editor-world swap is done).
//   @param data_layer_asset_path - Full /Game path to the UDataLayerAsset to instance.
//   @returns { data_layer_asset_path: string, instance_name: string, short_name: string, full_name: string, data_layer_type: string, is_runtime: bool, asset_path: string, created: bool, already_exists: bool }

TSharedPtr<FJsonObject> HandleCreateDataLayerInstance(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("data_layer_asset_path"), AssetPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UDataLayerAsset* Asset = nullptr;
    if (TSharedPtr<FJsonObject> Failure = DataAuthoringUtils::OpenDataLayerAsset(AssetPath, Asset))
    {
        return Failure;
    }

    // HONEST gate: instancing a data layer requires the editor world to be a World
    // Partition world (GetDataLayerManager() walks UWorld::GetWorldPartition(), which
    // is null on the default non-WP map). We never swap the editor world, so on the
    // default map this returns an honest error rather than faking success.
    UWorld* World = nullptr;
    UDataLayerManager* Manager = nullptr;
    if (TSharedPtr<FJsonObject> Failure = DataAuthoringUtils::OpenEditorDataLayerManager(
            TEXT("This world has no World Partition; data layer instance creation requires a world partition map (no data layer manager)"), World, Manager))
    {
        return Failure;
    }

    UDataLayerEditorSubsystem* Subsystem = UDataLayerEditorSubsystem::Get();
    if (!Subsystem)
    {
        return mcp_common::MakeErrorReply(TEXT("Data layer editor subsystem unavailable"));
    }

    // Idempotent: if an instance already references this asset in the world, return it.
    if (UDataLayerInstance* Existing = Subsystem->GetDataLayerInstance(Asset))
    {
        TSharedPtr<FJsonObject> ExistObj = DataLayerInstanceToJson(Existing);
        ExistObj->SetStringField(TEXT("data_layer_asset_path"), AssetPath);
        ExistObj->SetBoolField(TEXT("created"), false);
        ExistObj->SetBoolField(TEXT("already_exists"), true);
        return mcp_common::MakeOkReply(ExistObj);
    }

    const FScopedTransaction Transaction(FText::FromString(TEXT("MCP: Create Data Layer Instance")));

    FDataLayerCreationParameters CreationParams;
    CreationParams.DataLayerAsset = Asset;        // required: instance references this asset
    CreationParams.WorldDataLayers = nullptr;     // default to the level WorldDataLayers
    CreationParams.bIsPrivate = false;

    UDataLayerInstance* NewInstance = Subsystem->CreateDataLayerInstance(CreationParams);
    if (!NewInstance)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("CreateDataLayerInstance failed for asset '%s' in this world"), *AssetPath));
    }

    TSharedPtr<FJsonObject> ResultObj = DataLayerInstanceToJson(NewInstance);
    ResultObj->SetStringField(TEXT("data_layer_asset_path"), AssetPath);
    ResultObj->SetBoolField(TEXT("created"), true);
    ResultObj->SetBoolField(TEXT("already_exists"), false);

    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:assign_actor_to_data_layer_instance - Assigns an editor-world actor to a data layer instance.  Uses UDataLayerEditorSubsystem::AddActorToDataLayer against the data layer instance referencing the given asset. Requires a World Partition world; on a non-WP (default) map this returns an honest error (no editor-world swap).
//   @param actor_name - Label or name of the actor in the editor world.
//   @param data_layer_asset_path - Full /Game path to the UDataLayerAsset (must be instanced in the world first via create_data_layer_instance).
//   @returns { actor_name: string, data_layer_asset_path: string, instance_name: string, short_name: string, assigned: bool, message: string }

TSharedPtr<FJsonObject> HandleAssignActorToDataLayerInstance(const TSharedPtr<FJsonObject>& Params)
{
    UEAgentWorldPartitionCommandsImpl::FActorLayerTarget Target;
    if (TSharedPtr<FJsonObject> Failure = UEAgentWorldPartitionCommandsImpl::ResolveActorLayerTarget(Params,
            TEXT("This world has no World Partition; actor data layer instance assignment requires a world partition map (no data layer manager)"),
            TEXT("create the instance first"), Target))
    {
        return Failure;
    }

    const FScopedTransaction Transaction(FText::FromString(TEXT("MCP: Assign Actor To Data Layer Instance")));
    // false means the actor was already in the layer: reported, not an error.
    const bool bAdded = Target.Subsystem->AddActorToDataLayer(Target.Actor, Target.Instance);

    TSharedPtr<FJsonObject> Reply = UEAgentWorldPartitionCommandsImpl::ActorLayerReply(Target.Actor, Target.AssetPath, Target.Instance);
    Reply->SetBoolField(TEXT("assigned"), bAdded);
    Reply->SetStringField(TEXT("message"), bAdded
        ? TEXT("Actor assigned to data layer instance")
        : TEXT("Actor already belonged to the data layer instance"));
    return mcp_common::MakeOkReply(Reply);
}

// MCP:remove_actor_from_data_layer_instance - Removes an editor-world actor from a data layer instance (inverse of assign_actor_to_data_layer_instance).  Uses UDataLayerEditorSubsystem::RemoveActorFromDataLayer against the data layer instance referencing the given asset. Requires a World Partition world; on a non-WP (default) map this returns an honest error (no editor-world swap). Idempotent: removed=false means the actor was not in the layer (not an error).
//   @param actor_name - Label or name of the actor in the editor world.
//   @param data_layer_asset_path - Full /Game path to the UDataLayerAsset (the instance referencing it must exist in the world).
//   @returns { actor_name: string, data_layer_asset_path: string, instance_name: string, short_name: string, removed: bool, message: string }

TSharedPtr<FJsonObject> HandleRemoveActorFromDataLayerInstance(const TSharedPtr<FJsonObject>& Params)
{
    UEAgentWorldPartitionCommandsImpl::FActorLayerTarget Target;
    if (TSharedPtr<FJsonObject> Failure = UEAgentWorldPartitionCommandsImpl::ResolveActorLayerTarget(Params,
            TEXT("This world has no World Partition; actor data layer instance removal requires a world partition map (no data layer manager)"),
            TEXT("nothing to remove the actor from"), Target))
    {
        return Failure;
    }

    const FScopedTransaction Transaction(FText::FromString(TEXT("MCP: Remove Actor From Data Layer Instance")));
    // RemoveActorFromDataLayer returns false when the actor was not in the layer; that is
    // an idempotent signal (already-removed), NOT an error.
    const bool bRemoved = Target.Subsystem->RemoveActorFromDataLayer(Target.Actor, Target.Instance);

    TSharedPtr<FJsonObject> Reply = UEAgentWorldPartitionCommandsImpl::ActorLayerReply(Target.Actor, Target.AssetPath, Target.Instance);
    Reply->SetBoolField(TEXT("removed"), bRemoved);
    Reply->SetStringField(TEXT("message"), bRemoved
        ? TEXT("Actor removed from data layer instance")
        : TEXT("Actor was not in the layer"));
    return mcp_common::MakeOkReply(Reply);
}

// MCP:remove_data_layer_instance - Removes a data layer instance from the editor world's World Partition (inverse of create_data_layer_instance).  Uses UDataLayerEditorSubsystem::DeleteDataLayer (void), verified by read-back: after delete GetDataLayerInstance(asset) must be null. Requires a World Partition world; on a non-WP (default) map this returns an honest error (no editor-world swap). Idempotent: already_deleted=true when no instance referenced the asset before the call.
//   @param data_layer_asset_path - Full /Game path to the UDataLayerAsset whose instance should be removed from the world.
//   @returns { data_layer_asset_path: string, deleted: bool, already_deleted: bool, instance_name?: string, note?: string }

TSharedPtr<FJsonObject> HandleRemoveDataLayerInstance(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("data_layer_asset_path"), AssetPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UDataLayerAsset* Asset = nullptr;
    if (TSharedPtr<FJsonObject> Failure = DataAuthoringUtils::OpenDataLayerAsset(AssetPath, Asset))
    {
        return Failure;
    }

    // HONEST gate: removing a data layer instance requires the editor world to be a World
    // Partition world (same reasoning as create_data_layer_instance). We never swap the
    // editor world, so on the default non-WP map this returns an honest error.
    UWorld* World = nullptr;
    UDataLayerManager* Manager = nullptr;
    if (TSharedPtr<FJsonObject> Failure = DataAuthoringUtils::OpenEditorDataLayerManager(
            TEXT("This world has no World Partition; data layer instance removal requires a world partition map (no data layer manager)"), World, Manager))
    {
        return Failure;
    }

    UDataLayerEditorSubsystem* Subsystem = UDataLayerEditorSubsystem::Get();
    if (!Subsystem)
    {
        return mcp_common::MakeErrorReply(TEXT("Data layer editor subsystem unavailable"));
    }

    // Idempotent: if no instance references this asset, it is already absent from the world.
    UDataLayerInstance* Instance = Subsystem->GetDataLayerInstance(Asset);
    if (!Instance)
    {
        TSharedPtr<FJsonObject> AbsentObj = MakeShared<FJsonObject>();
        AbsentObj->SetStringField(TEXT("data_layer_asset_path"), AssetPath);
        AbsentObj->SetBoolField(TEXT("deleted"), false);
        AbsentObj->SetBoolField(TEXT("already_deleted"), true);
        AbsentObj->SetStringField(TEXT("note"), TEXT("No data layer instance referenced this asset in the world; nothing to remove"));
        return mcp_common::MakeOkReply(AbsentObj);
    }

    const FString InstanceName = Instance->GetFName().ToString();

    const FScopedTransaction Transaction(FText::FromString(TEXT("MCP: Remove Data Layer Instance")));
    // DeleteDataLayer returns void, so success is VERIFIED by read-back below. It also
    // silently no-ops when Instance->CanBeRemoved() is false (only true for external data
    // layers; the standard UDataLayerInstanceWithAsset from create_data_layer_instance
    // returns true).
    const bool bCanBeRemoved = Instance->CanBeRemoved();
    Subsystem->DeleteDataLayer(Instance);

    // Read-back verification: the instance must be gone for the asset.
    const bool bDeleted = (Subsystem->GetDataLayerInstance(Asset) == nullptr);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("data_layer_asset_path"), AssetPath);
    ResultObj->SetStringField(TEXT("instance_name"), InstanceName);
    ResultObj->SetBoolField(TEXT("deleted"), bDeleted);
    ResultObj->SetBoolField(TEXT("already_deleted"), false);
    if (!bDeleted)
    {
        ResultObj->SetStringField(TEXT("note"), bCanBeRemoved
            ? TEXT("DeleteDataLayer did not remove the instance (still present on read-back)")
            : TEXT("DeleteDataLayer silently no-ops because CanBeRemoved()==false for this instance (only external data layers can be removed); the instance is still present"));
    }

    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:set_data_layer_parent - Sets (or clears) the parent of a data layer instance in the editor world's World Partition (twin of create_data_layer_instance).  Uses UDataLayerEditorSubsystem::SetParentDataLayer, verified by read-back (GetParent() must equal the requested parent). Both instances are resolved from their UDataLayerAsset paths (create the instances first via create_data_layer_instance). Pass parent_data_layer_asset_path empty/omitted to detach (reparent to root). Requires a World Partition world; on a non-WP (default) map this returns an honest error (no editor-world swap). Idempotent: already_parented=true when the instance already had the requested parent. Note: SetParentDataLayer returns false (parented=false) when the engine rejects the relationship (CanBeChildOf()==false, e.g. a runtime child under an editor parent or a cycle); the read-back reports the honest result.
//   @param data_layer_asset_path - Full /Game path to the child UDataLayerAsset (its instance must exist in the world).
//   @param parent_data_layer_asset_path - Full /Game path to the parent UDataLayerAsset (its instance must exist in the world). Omit or pass empty to clear the parent (reparent to root).
//   @returns { data_layer_asset_path: string, parent_data_layer_asset_path: string, instance_name: string, parent_instance_name: string, parented: bool, already_parented: bool, note?: string }

TSharedPtr<FJsonObject> HandleSetDataLayerParent(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("data_layer_asset_path"), AssetPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    // Parent is optional: empty/omitted means detach (reparent to root).
    FString ParentAssetPath;
    Params->TryGetStringField(TEXT("parent_data_layer_asset_path"), ParentAssetPath);
    const bool bDetach = ParentAssetPath.IsEmpty();

    UDataLayerAsset* Asset = nullptr;
    if (TSharedPtr<FJsonObject> Failure = DataAuthoringUtils::OpenDataLayerAsset(AssetPath, Asset))
    {
        return Failure;
    }

    UDataLayerAsset* ParentAsset = nullptr;
    if (!bDetach)
    {
        ParentAsset = DataAuthoringUtils::LoadDataLayerAsset(ParentAssetPath);
        if (!ParentAsset)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Parent data layer asset not found: %s"), *ParentAssetPath));
        }
    }

    // HONEST gate: reparenting a data layer instance requires the editor world to be a World
    // Partition world (same reasoning as create_data_layer_instance). We never swap the editor
    // world, so on the default non-WP map this returns an honest error.
    UWorld* World = nullptr;
    UDataLayerManager* Manager = nullptr;
    if (TSharedPtr<FJsonObject> Failure = DataAuthoringUtils::OpenEditorDataLayerManager(
            TEXT("This world has no World Partition; data layer reparenting requires a world partition map (no data layer manager)"), World, Manager))
    {
        return Failure;
    }

    UDataLayerEditorSubsystem* Subsystem = UDataLayerEditorSubsystem::Get();
    if (!Subsystem)
    {
        return mcp_common::MakeErrorReply(TEXT("Data layer editor subsystem unavailable"));
    }

    UDataLayerInstance* Instance = Subsystem->GetDataLayerInstance(Asset);
    if (!Instance)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("No data layer instance references asset '%s' in this world; create the instance first"), *AssetPath));
    }

    UDataLayerInstance* ParentInstance = nullptr;
    if (!bDetach)
    {
        ParentInstance = Subsystem->GetDataLayerInstance(ParentAsset);
        if (!ParentInstance)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("No data layer instance references parent asset '%s' in this world; create the parent instance first"), *ParentAssetPath));
        }
    }

    // Idempotent: if the instance already has the requested parent, return a stable payload.
    if (Instance->GetParent() == ParentInstance)
    {
        TSharedPtr<FJsonObject> ExistObj = MakeShared<FJsonObject>();
        ExistObj->SetStringField(TEXT("data_layer_asset_path"), AssetPath);
        ExistObj->SetStringField(TEXT("parent_data_layer_asset_path"), ParentAssetPath);
        ExistObj->SetStringField(TEXT("instance_name"), Instance->GetFName().ToString());
        ExistObj->SetStringField(TEXT("parent_instance_name"), ParentInstance ? ParentInstance->GetFName().ToString() : FString());
        ExistObj->SetBoolField(TEXT("parented"), true);
        ExistObj->SetBoolField(TEXT("already_parented"), true);
        return mcp_common::MakeOkReply(ExistObj);
    }

    const FScopedTransaction Transaction(FText::FromString(TEXT("MCP: Set Data Layer Parent")));
    // SetParentDataLayer returns false when the engine rejects the relationship
    // (CanBeChildOf()==false: e.g. a runtime child under an editor parent, or a cycle).
    // We do not trust the return value alone; the actual parent is VERIFIED by read-back.
    Subsystem->SetParentDataLayer(Instance, ParentInstance);

    const bool bParented = (Instance->GetParent() == ParentInstance);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("data_layer_asset_path"), AssetPath);
    ResultObj->SetStringField(TEXT("parent_data_layer_asset_path"), ParentAssetPath);
    ResultObj->SetStringField(TEXT("instance_name"), Instance->GetFName().ToString());
    ResultObj->SetStringField(TEXT("parent_instance_name"), ParentInstance ? ParentInstance->GetFName().ToString() : FString());
    ResultObj->SetBoolField(TEXT("parented"), bParented);
    ResultObj->SetBoolField(TEXT("already_parented"), false);
    if (!bParented)
    {
        ResultObj->SetStringField(TEXT("note"), bDetach
            ? TEXT("SetParentDataLayer did not clear the parent (still present on read-back)")
            : TEXT("SetParentDataLayer rejected the relationship (CanBeChildOf()==false: a runtime data layer cannot be a child of an editor one, and cycles are disallowed); the parent is unchanged on read-back"));
    }

    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:get_data_layer_instances - Lists the editor world's data layer instances.  REAL read via UDataLayerManager::ForEachDataLayerInstance. Returns an empty list on a non-WP (default) map, which is the correct result.
//   @returns { world_name: string, is_world_partition_enabled: bool, data_layer_instances: [{instance_name, short_name, full_name, data_layer_type, is_runtime, asset_path}], count: int, note: string }

TSharedPtr<FJsonObject> HandleGetDataLayerInstances(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World)) { return NoWorld; }

    TArray<TSharedPtr<FJsonValue>> InstanceArray;
    bool bWorldPartition = false;

    if (UDataLayerManager* Manager = UDataLayerManager::GetDataLayerManager(World))
    {
        bWorldPartition = true;
        Manager->ForEachDataLayerInstance([&InstanceArray](UDataLayerInstance* Instance)
        {
            if (Instance)
            {
                InstanceArray.Add(MakeShared<FJsonValueObject>(DataLayerInstanceToJson(Instance)));
            }
            return true; // continue
        });
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("world_name"), World->GetName());
    ResultObj->SetBoolField(TEXT("is_world_partition_enabled"), bWorldPartition);
    ResultObj->SetArrayField(TEXT("data_layer_instances"), InstanceArray);
    ResultObj->SetNumberField(TEXT("count"), InstanceArray.Num());
    ResultObj->SetStringField(TEXT("note"), bWorldPartition
        ? TEXT("Listed the editor world's data layer instances")
        : TEXT("Editor world is not World Partition; empty list is the correct read"));

    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:convert_map_to_world_partition - Reports whether the current editor world can be converted to World Partition.  HONEST gate: the only complete conversion path is the UWorldPartitionConvertCommandlet (commandlet-only, needs -run=WorldPartitionConvert -AllowCommandletRendering as a separate process), and the runtime FWorldPartitionConverter::Convert mutates the level in place WITHOUT saving, so it cannot be driven safely or synchronously from this request. The tool does the headless-safe part for real (resolves the editor world, detects World Partition) and returns converted=false with a note explaining why (already WP, or commandlet-only).
//   @param map_path - Optional informational path of the map to convert (the world converted is always the current editor world).
//   @returns { world: string, is_world_partition: bool, converted: bool, already_converted: bool, success: bool, note: string, message: string }

TSharedPtr<FJsonObject> HandleConvertMapToWorldPartition(const TSharedPtr<FJsonObject>& Params)
{
    // HONEST gate (mirrors HandleBuildHLOD): a real conversion of an existing level to
    // World Partition is only available through the UWorldPartitionConvertCommandlet
    // (a separate -run=WorldPartitionConvert -AllowCommandletRendering process). The one
    // runtime API, FWorldPartitionConverter::Convert, is DESTRUCTIVE in place
    // (ConvertAllActorsToPackaging / EditorDestroyActor / RemoveLevelFromWorld) and does
    // not save, so using it would corrupt the editor world and break idempotency. We
    // therefore never drive a conversion here; we only do the headless-safe part for
    // real (resolve the editor world, detect WP) and report honestly.
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World)) { return NoWorld; }

    // Informational only: the world actually inspected is the current editor world.
    FString MapPath;
    Params->TryGetStringField(TEXT("map_path"), MapPath);

    const bool bIsPartitioned = World->IsPartitionedWorld();

    TSharedPtr<FJsonObject> ResultObj = DataAuthoringUtils::WorldPartitionGateReply(*World, TEXT("converted"));

    if (bIsPartitioned)
    {
        // Already a World Partition world: nothing to convert. Honest, stable payload.
        ResultObj->SetBoolField(TEXT("already_converted"), true);
        ResultObj->SetStringField(TEXT("note"),
            TEXT("Editor world is already World Partition; nothing to convert"));
        ResultObj->SetStringField(TEXT("message"),
            TEXT("No conversion performed: editor world is already a World Partition map"));
        return mcp_common::MakeOkReply(ResultObj);
    }

    // Normal case: a non-WP editor world. Conversion is commandlet-only / destructive
    // runtime-only, so we honestly perform nothing and point at the from-scratch path.
    ResultObj->SetBoolField(TEXT("already_converted"), false);
    ResultObj->SetStringField(TEXT("note"),
        TEXT("Converting an existing level to World Partition is commandlet-only (UWorldPartitionConvertCommandlet, requires running -run=WorldPartitionConvert -AllowCommandletRendering as a separate process). The runtime FWorldPartitionConverter::Convert mutates the level in place and does not save, so it cannot be driven safely or synchronously from this request; no conversion was performed. To create a World Partition map from scratch use create_world_partition_map."));
    ResultObj->SetStringField(TEXT("message"),
        TEXT("Map-to-WorldPartition conversion is commandlet-only; no conversion was performed. Use create_world_partition_map to author a WP map from scratch."));
    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:build_minimap - Reports whether a World Partition minimap can be / has been built for the current editor world.  HONEST gate: minimap generation is a GPU scene-capture / commandlet-rendering operation (FWorldPartitionMiniMapHelper::CaptureBoundsMiniMapToTexture via ASceneCapture2D, or UWorldPartitionMiniMapBuilder whose RequiresCommandletRendering()==true, both gated by -AllowCommandletRendering) and cannot be driven synchronously from a single request. The tool does the headless-safe part for real (resolves the editor world, detects World Partition, and on a WP world reads back whether a minimap texture already exists) and returns built=false with a note.
//   @returns { world: string, is_world_partition: bool, built: bool, already_has_minimap?: bool, success: bool, note: string, message: string }

TSharedPtr<FJsonObject> HandleBuildMinimap(const TSharedPtr<FJsonObject>& Params)
{
    // HONEST gate (mirrors HandleBuildHLOD): generating a World Partition minimap is a
    // real GPU scene-capture (FWorldPartitionMiniMapHelper::CaptureBoundsMiniMapToTexture
    // -> ASceneCapture2D::CaptureScene + FakeEngineTick) and/or the
    // UWorldPartitionMiniMapBuilder whose RequiresCommandletRendering()==true (blocked by
    // IsAllowCommandletRendering()==false). Neither can run synchronously inside this
    // single request. We do the headless-safe part for real: resolve the editor world,
    // detect World Partition, and (read-only) report whether a minimap texture already
    // exists, then report honestly that no build was performed.
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World)) { return NoWorld; }

    const bool bIsPartitioned = World->IsPartitionedWorld();

    TSharedPtr<FJsonObject> ResultObj = DataAuthoringUtils::WorldPartitionGateReply(*World, TEXT("built"));

    if (!bIsPartitioned)
    {
        // Minimap is meaningless without World Partition: honest, stable payload.
        ResultObj->SetStringField(TEXT("note"),
            TEXT("Minimap build requires a World Partition world; current editor world is not partitioned, so no minimap was built"));
        ResultObj->SetStringField(TEXT("message"),
            TEXT("Minimap build not applicable: editor world is not a World Partition map"));
        return mcp_common::MakeOkReply(ResultObj);
    }

    // Read-only probe: an AWorldPartitionMiniMap with a non-null MiniMapTexture means a
    // minimap was already generated for this world. This is a pure read (TActorIterator
    // + null-check of a TObjectPtr<UTexture2D>); it spawns nothing and renders nothing.
    bool bAlreadyHasMinimap = false;
    for (TActorIterator<AWorldPartitionMiniMap> It(World); It; ++It)
    {
        AWorldPartitionMiniMap* MiniMap = *It;
        if (MiniMap && MiniMap->MiniMapTexture != nullptr)
        {
            bAlreadyHasMinimap = true;
            break;
        }
    }
    ResultObj->SetBoolField(TEXT("already_has_minimap"), bAlreadyHasMinimap);

    // World Partition is present, but a real minimap build is GPU/commandlet-rendering
    // only and cannot be driven synchronously here, so we report honestly.
    ResultObj->SetStringField(TEXT("note"),
        TEXT("World Partition detected; minimap generation is a GPU scene-capture / commandlet-rendering operation (UWorldPartitionMiniMapBuilder / FWorldPartitionMiniMapHelper::CaptureBoundsMiniMapToTexture, requires -AllowCommandletRendering) and cannot be driven synchronously from a single request, so no minimap was built."));
    ResultObj->SetStringField(TEXT("message"),
        TEXT("Minimap build is GPU/commandlet-rendering only; run the WorldPartitionMiniMapBuilder commandlet (with -AllowCommandletRendering) or Build > Build Minimap to generate"));
    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:get_world_partition_runtime_hash - Reads the editor world's World Partition runtime hash.  REAL read: reports the runtime hash class and the number of GENERATED streaming grids (UWorldPartitionRuntimeSpatialHash::GetNumGrids), which is 0 until a cook/build generates the runtime grids. On the default non-WP editor map it honestly returns is_world_partition=false (no swap is done).
//   @returns { is_world_partition: bool, runtime_hash_type?: string, num_streaming_grids?: int, success: bool, note: string }

TSharedPtr<FJsonObject> HandleGetWorldPartitionRuntimeHash(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World)) { return NoWorld; }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();

    // Honest read: the default editor map is not partitioned. We never swap the editor
    // world, so is_world_partition=false is the correct result (success stays true).
    if (!World->IsPartitionedWorld())
    {
        ResultObj->SetBoolField(TEXT("is_world_partition"), false);
        ResultObj->SetBoolField(TEXT("success"), true);
        ResultObj->SetStringField(TEXT("note"), TEXT("Editor world is not World Partition."));
        return mcp_common::MakeOkReply(ResultObj);
    }

    // REAL read of the World Partition runtime hash type + generated streaming grids.
    UWorldPartition* WP = World->GetWorldPartition();

    int32 NumGrids = 0;
    if (UWorldPartitionRuntimeSpatialHash* SpatialHash = Cast<UWorldPartitionRuntimeSpatialHash>(WP->RuntimeHash))
    {
        NumGrids = static_cast<int32>(SpatialHash->GetNumGrids());
    }

    ResultObj->SetBoolField(TEXT("is_world_partition"), true);
    ResultObj->SetStringField(TEXT("runtime_hash_type"), UEAgentWorldPartitionCommandsImpl::RuntimeHashTypeName(WP));
    ResultObj->SetNumberField(TEXT("num_streaming_grids"), NumGrids);
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetStringField(TEXT("note"),
        TEXT("num_streaming_grids reads generated runtime grids (0 when not cooked); edit-time grid settings live in editor-only data."));

    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:set_world_partition_runtime_hash - HONEST gate for editing the World Partition runtime hash grid settings.  Mirrors HandleBuildMinimap: the WP runtime grid settings (CellSize/LoadingRange) live in UWorldPartitionRuntimeSpatialHash::Grids, an editor-only private UPROPERTY with NO public setter, and the streaming grid is generated during cook/build (GenerateStreaming is WITH_EDITOR), so it cannot be set synchronously headless. The tool resolves the editor world + detects WP for real and reports applied=false; nothing is mutated. On the non-WP default map it honestly returns is_world_partition=false.
//   @param cell_size - Informative only (accepted, not applied): desired streaming cell size.
//   @param loading_range - Informative only (accepted, not applied): desired loading range.
//   @param grid_name - Informative only (accepted, not applied): target grid name.
//   @returns { is_world_partition: bool, runtime_hash_type?: string, applied: bool, runtime_regenerated?: bool, success: bool, note: string }

TSharedPtr<FJsonObject> HandleSetWorldPartitionRuntimeHash(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World)) { return NoWorld; }

    // Informative-only params: accepted and parsed so callers get honest validation, but
    // never applied (the grid settings have no public setter and the streaming grid is
    // generated at cook/build, so no synchronous headless mutation is possible).
    int32 CellSize = 0;
    Params->TryGetNumberField(TEXT("cell_size"), CellSize);
    double LoadingRange = 0.0;
    Params->TryGetNumberField(TEXT("loading_range"), LoadingRange);
    FString GridName;
    Params->TryGetStringField(TEXT("grid_name"), GridName);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();

    // HONEST gate: setting grid settings requires a World Partition map. The default
    // editor map is not partitioned and we never swap the editor world.
    if (!World->IsPartitionedWorld())
    {
        ResultObj->SetBoolField(TEXT("is_world_partition"), false);
        ResultObj->SetBoolField(TEXT("applied"), false);
        ResultObj->SetBoolField(TEXT("success"), true);
        ResultObj->SetStringField(TEXT("note"), TEXT("Requires a World Partition map."));
        return mcp_common::MakeOkReply(ResultObj);
    }

    // World Partition present: read back the runtime hash type for real, but DO NOT MUTATE.
    ResultObj->SetBoolField(TEXT("is_world_partition"), true);
    ResultObj->SetStringField(TEXT("runtime_hash_type"), UEAgentWorldPartitionCommandsImpl::RuntimeHashTypeName(World->GetWorldPartition()));
    ResultObj->SetBoolField(TEXT("applied"), false);
    ResultObj->SetBoolField(TEXT("runtime_regenerated"), false);
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetStringField(TEXT("note"),
        TEXT("WP runtime grid settings (CellSize/LoadingRange) live in UWorldPartitionRuntimeSpatialHash::Grids, editor-only private UPROPERTY with no public setter; the streaming grid is generated during cook/build (GenerateStreaming is WITH_EDITOR), so it cannot be set synchronously headless. No runtime change applied."));

    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:list_world_partition_actors - Lists the editor world's World Partition actors by walking their DESCRIPTORS without loading any actor package.  Unlike get_actors_by_class (which only sees LOADED AActors via TActorIterator), this reads FWorldPartitionActorDescInstance for every actor known to World Partition, INCLUDING spatially-unloaded / streamed-out ones (solving "get_actors_by_class only sees loaded actors"). Editor-world read only (WP actor descriptors are editor-only data); honest empty list on a non-WP (default) map.
//   @param filter_class - Optional. Class filter: resolved to a UClass (full path, /Script name, Blueprint asset path, or bare A/U name) and matched against each descriptor's native class via IsChildOf; ALSO matched as a case-insensitive substring against the descriptor's native and base class paths (so Blueprint class names like 'BP_Foo_C' or partial names work).
//   @param name_contains - Optional. Case-insensitive substring matched against the actor's label, object name, and soft path.
//   @param folder - Optional. Case-insensitive substring matched against the actor's editor folder path.
//   @param limit - Optional int cap on the number of actors returned (default 2000); total_matched reports the full match count regardless of the cap.
//   @returns { world: string, is_world_partition: bool, is_initialized: bool, count: int, total_matched: int, truncated: bool, actors: [{ path: string, name: string, label: string, native_class: string, base_class: string, display_class: string, is_spatially_loaded: bool, is_loaded: bool, editor_only: bool, folder: string, runtime_grid: string, editor_bounds: {valid,origin,extent}, runtime_bounds: {valid,origin,extent}, data_layers: [string] }], note: string }

TSharedPtr<FJsonObject> HandleListWorldPartitionActors(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World)) { return NoWorld; }

    // Optional filters.
    FString FilterClassName;
    Params->TryGetStringField(TEXT("filter_class"), FilterClassName);
    FString NameContains;
    Params->TryGetStringField(TEXT("name_contains"), NameContains);
    FString Folder;
    Params->TryGetStringField(TEXT("folder"), Folder);

    int32 Limit = 2000;
    Params->TryGetNumberField(TEXT("limit"), Limit);
    if (Limit <= 0)
    {
        Limit = 2000;
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("world"), World->GetName());

    UWorldPartition* WP = World->GetWorldPartition();
    if (!WP)
    {
        // Honest read: the default editor map is not partitioned, so it has no actor
        // descriptors. Empty list is the correct result (mirrors get_data_layer_instances).
        ResultObj->SetBoolField(TEXT("is_world_partition"), false);
        ResultObj->SetBoolField(TEXT("is_initialized"), false);
        ResultObj->SetArrayField(TEXT("actors"), TArray<TSharedPtr<FJsonValue>>());
        ResultObj->SetNumberField(TEXT("count"), 0);
        ResultObj->SetNumberField(TEXT("total_matched"), 0);
        ResultObj->SetBoolField(TEXT("truncated"), false);
        ResultObj->SetStringField(TEXT("note"), TEXT("Editor world is not World Partition; empty list is the correct read"));
        return mcp_common::MakeOkReply(ResultObj);
    }

    ResultObj->SetBoolField(TEXT("is_world_partition"), true);
    ResultObj->SetBoolField(TEXT("is_initialized"), WP->IsInitialized());

    // Resolve filter_class once to a UClass (nullptr if the string is not a resolvable class;
    // the substring fallback below still applies for Blueprint / partial class names).
    UClass* FilterClass = nullptr;
    if (!FilterClassName.IsEmpty())
    {
        FilterClass = FUEAgentGraphUtils::ResolveClass(FilterClassName);
    }

    TArray<TSharedPtr<FJsonValue>> ActorsArray;
    int32 TotalMatched = 0;

    // Walk the descriptors WITHOUT loading actors (same idiom the WP editor builders use:
    // FActorDescContainerInstanceCollection::TIterator over UWorldPartition, which IS an
    // FActorDescContainerInstanceCollection). Covers content-bundle / level-instance
    // sub-containers too.
    for (FActorDescContainerInstanceCollection::TIterator<> It(WP); It; ++It)
    {
        FWorldPartitionActorDescInstance* Desc = *It;
        if (!Desc)
        {
            continue;
        }

        const FString NativeClassPath = Desc->GetNativeClass().ToString();
        const FString BaseClassPath   = Desc->GetBaseClass().ToString();

        // filter_class: match if the resolved class is a native parent, OR either class path
        // contains the identifier (case-insensitive; handles Blueprint / partial names).
        if (!FilterClassName.IsEmpty())
        {
            bool bClassMatch = false;
            if (FilterClass)
            {
                UClass* NativeClass = Desc->GetActorNativeClass();
                bClassMatch = (NativeClass != nullptr) && NativeClass->IsChildOf(FilterClass);
            }
            if (!bClassMatch)
            {
                bClassMatch = NativeClassPath.Contains(FilterClassName, ESearchCase::IgnoreCase)
                           || BaseClassPath.Contains(FilterClassName, ESearchCase::IgnoreCase);
            }
            if (!bClassMatch)
            {
                continue;
            }
        }

        const FString Label      = Desc->GetActorLabel().ToString();
        const FString Name       = Desc->GetActorName().ToString();
        const FString Path       = Desc->GetActorSoftPath().ToString();
        const FString FolderPath = Desc->GetFolderPath().ToString();

        if (!NameContains.IsEmpty())
        {
            const bool bNameMatch = Label.Contains(NameContains, ESearchCase::IgnoreCase)
                                 || Name.Contains(NameContains, ESearchCase::IgnoreCase)
                                 || Path.Contains(NameContains, ESearchCase::IgnoreCase);
            if (!bNameMatch)
            {
                continue;
            }
        }

        if (!Folder.IsEmpty() && !FolderPath.Contains(Folder, ESearchCase::IgnoreCase))
        {
            continue;
        }

        ++TotalMatched;
        if (ActorsArray.Num() >= Limit)
        {
            continue; // count every match but cap the JSON payload
        }

        TSharedPtr<FJsonObject> ActorObj = MakeShared<FJsonObject>();
        ActorObj->SetStringField(TEXT("path"), Path);
        ActorObj->SetStringField(TEXT("name"), Name);
        ActorObj->SetStringField(TEXT("label"), Label);
        ActorObj->SetStringField(TEXT("native_class"), NativeClassPath);
        ActorObj->SetStringField(TEXT("base_class"), BaseClassPath);
        ActorObj->SetStringField(TEXT("display_class"), Desc->GetDisplayClassName().ToString());
        ActorObj->SetBoolField(TEXT("is_spatially_loaded"), Desc->GetIsSpatiallyLoaded());
        ActorObj->SetBoolField(TEXT("is_loaded"), Desc->IsLoaded());
        ActorObj->SetBoolField(TEXT("editor_only"), Desc->GetActorIsEditorOnly());
        ActorObj->SetStringField(TEXT("folder"), FolderPath);
        ActorObj->SetStringField(TEXT("runtime_grid"), Desc->GetRuntimeGrid().ToString());
        ActorObj->SetObjectField(TEXT("editor_bounds"), WPBoxToJson(Desc->GetEditorBounds()));
        ActorObj->SetObjectField(TEXT("runtime_bounds"), WPBoxToJson(Desc->GetRuntimeBounds()));

        TArray<TSharedPtr<FJsonValue>> DataLayersArray;
        for (const FName& DL : Desc->GetDataLayers())
        {
            DataLayersArray.Add(MakeShared<FJsonValueString>(DL.ToString()));
        }
        ActorObj->SetArrayField(TEXT("data_layers"), DataLayersArray);

        ActorsArray.Add(MakeShared<FJsonValueObject>(ActorObj));
    }

    const bool bTruncated = (TotalMatched > ActorsArray.Num());
    ResultObj->SetArrayField(TEXT("actors"), ActorsArray);
    ResultObj->SetNumberField(TEXT("count"), ActorsArray.Num());
    ResultObj->SetNumberField(TEXT("total_matched"), TotalMatched);
    ResultObj->SetBoolField(TEXT("truncated"), bTruncated);
    ResultObj->SetStringField(TEXT("note"), bTruncated
        ? FString::Printf(TEXT("Listed %d of %d matching World Partition actor descriptors (payload capped at limit=%d); descriptors are read WITHOUT loading actors"), ActorsArray.Num(), TotalMatched, Limit)
        : TEXT("Listed World Partition actor descriptors WITHOUT loading actors (sees spatially-unloaded / streamed-out actors that get_actors_by_class cannot)"));

    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:load_world_partition_region - Pins the World Partition cells intersecting an axis-aligned region [min,max] into memory for inspection (editor world).  Creates a UWorldPartitionEditorLoaderAdapter backed by FLoaderAdapterShape over the box and calls ILoaderAdapter::Load(); the adapter stays REGISTERED so the region remains resident (mirrors the editor's "Load Region from Here"), which then lets get_actors_by_class / actor tools see the now-loaded actors. Honest error on a non-WP (default) map or an uninitialized World Partition. Non-destructive: it loads (pins) actors, it never spawns/edits/saves.
//   @param min - [x,y,z] minimum corner of the region box in world units.
//   @param max - [x,y,z] maximum corner of the region box in world units.
//   @param label - Optional label for the loader adapter (default "MCP Load Region").
//   @returns { world: string, is_world_partition: bool, loaded: bool, region_min: [x,y,z], region_max: [x,y,z], label: string, num_loaded_actors: int, note: string }

TSharedPtr<FJsonObject> HandleLoadWorldPartitionRegion(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params->HasField(TEXT("min")) || !Params->HasField(TEXT("max")))
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameters: min and max ([x,y,z] each)"));
    }
    const FVector MinIn = mcp_common::ReadJsonVector(Params, TEXT("min"));
    const FVector MaxIn = mcp_common::ReadJsonVector(Params, TEXT("max"));

    // Normalize so min<=max per component; a degenerate (zero-volume) box loads nothing useful.
    const FVector RegionMin = FVector::Min(MinIn, MaxIn);
    const FVector RegionMax = FVector::Max(MinIn, MaxIn);
    if ((RegionMax - RegionMin).IsNearlyZero())
    {
        return mcp_common::MakeErrorReply(TEXT("Degenerate region: min and max describe a zero-volume box; provide a real [min,max] extent"));
    }

    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World)) { return NoWorld; }

    UWorldPartition* WP = World->GetWorldPartition();
    if (!WP)
    {
        return mcp_common::MakeErrorReply(TEXT("This world has no World Partition; region loading requires a world partition map"));
    }

    if (!WP->IsInitialized())
    {
        return mcp_common::MakeErrorReply(TEXT("World Partition is not initialized; cannot create a region loader adapter"));
    }

    FString Label;
    if (!Params->TryGetStringField(TEXT("label"), Label) || Label.IsEmpty())
    {
        Label = TEXT("MCP Load Region");
    }

    const FBox Region(RegionMin, RegionMax);

    // Create a spatial box loader adapter and LOAD (pin) the intersecting cells. The adapter is
    // registered in UWorldPartition::RegisteredEditorLoaderAdapters, so we deliberately DO NOT
    // release it: releasing would immediately Unload() the region. This mirrors the editor's
    // "Load Region from Here" and keeps the region resident for subsequent inspection.
    UWorldPartitionEditorLoaderAdapter* Adapter = WP->CreateEditorLoaderAdapter<FLoaderAdapterShape>(World, Region, Label);
    bool bLoaded = false;
    if (Adapter)
    {
        if (IWorldPartitionActorLoaderInterface::ILoaderAdapter* LoaderAdapter = Adapter->GetLoaderAdapter())
        {
            LoaderAdapter->Load();
            bLoaded = LoaderAdapter->IsLoaded();
        }
    }

    // Read-back verification: count actor descriptors now resident (loaded). This is a
    // world-wide count (an honest signal that loading happened), not a per-region count.
    int32 NumLoadedActors = 0;
    for (FActorDescContainerInstanceCollection::TIterator<> It(WP); It; ++It)
    {
        if (FWorldPartitionActorDescInstance* Desc = *It)
        {
            if (Desc->IsLoaded())
            {
                ++NumLoadedActors;
            }
        }
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("world"), World->GetName());
    ResultObj->SetBoolField(TEXT("is_world_partition"), true);
    ResultObj->SetBoolField(TEXT("loaded"), bLoaded);
    ResultObj->SetArrayField(TEXT("region_min"), mcp_common::VectorToJsonArray(RegionMin));
    ResultObj->SetArrayField(TEXT("region_max"), mcp_common::VectorToJsonArray(RegionMax));
    ResultObj->SetStringField(TEXT("label"), Label);
    ResultObj->SetNumberField(TEXT("num_loaded_actors"), NumLoadedActors);
    ResultObj->SetStringField(TEXT("note"), bLoaded
        ? TEXT("Region pinned via editor loader adapter (FLoaderAdapterShape); the adapter stays registered so the region remains resident for inspection. num_loaded_actors is the world-wide loaded-descriptor count after loading.")
        : TEXT("Loader adapter created but reported not loaded (the region may intersect no World Partition cells, or the matching actors were already resident)."));

    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:set_actor_spatially_loaded - Toggles an editor-world actor's World Partition spatial-loading flag (AActor::SetIsSpatiallyLoaded).  Guarded by AActor::CanChangeIsSpatiallyLoadedFlag() because the setter check()s that precondition (same baked guard as spawn_box_volume) -- so an actor that forbids the change never crashes the editor. Wrapped in FScopedTransaction + Actor->Modify() so the flag change marks the actor's (OFPA) package dirty and survives save_level. applied=false with can_change=false is the honest result for an actor that forbids the change (e.g. a non-partitioned / always-loaded actor); nothing is mutated in that case.
//   @param actor_name - Label or name of the actor in the editor world.
//   @param is_spatially_loaded - bool desired spatial-loading flag.
//   @returns { actor_name: string, actor_path: string, requested: bool, is_spatially_loaded: bool, applied: bool, can_change: bool, changed: bool, note: string }

TSharedPtr<FJsonObject> HandleSetActorSpatiallyLoaded(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    bool bDesired = true;
    if (!Params->TryGetBoolField(TEXT("is_spatially_loaded"), bDesired))
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: is_spatially_loaded (bool)"));
    }

    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World)) { return NoWorld; }

    AActor* Actor = mcp_common::FindActorByName(World, ActorName);
    if (!Actor)
    {
        return UEAgentWorldPartitionCommandsImpl::ActorNotInEditorWorldReply(ActorName);
    }

    const bool bBefore    = Actor->GetIsSpatiallyLoaded();
    const bool bCanChange = Actor->CanChangeIsSpatiallyLoadedFlag();

    bool bApplied = false;
    if (bCanChange)
    {
        // Transaction + Modify() so the flag change dirties the actor's package (OFPA-safe;
        // mirrors set_actor_transform), otherwise save_level would drop it.
        const FScopedTransaction Transaction(FText::FromString(TEXT("MCP: Set Actor Spatially Loaded")));
        Actor->Modify();
        Actor->SetIsSpatiallyLoaded(bDesired);
        bApplied = true;
    }

    const bool bAfter = Actor->GetIsSpatiallyLoaded();

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("actor_name"), Actor->GetActorLabel());
    ResultObj->SetStringField(TEXT("actor_path"), Actor->GetPathName());
    ResultObj->SetBoolField(TEXT("requested"), bDesired);
    ResultObj->SetBoolField(TEXT("is_spatially_loaded"), bAfter);
    ResultObj->SetBoolField(TEXT("applied"), bApplied);
    ResultObj->SetBoolField(TEXT("can_change"), bCanChange);
    ResultObj->SetBoolField(TEXT("changed"), bAfter != bBefore);
    ResultObj->SetStringField(TEXT("note"), bCanChange
        ? (bAfter != bBefore
            ? TEXT("Spatial-loading flag updated (package marked dirty; save the level to persist)")
            : TEXT("Spatial-loading flag already had the requested value; no change"))
        : TEXT("Actor forbids changing the spatial-loading flag (CanChangeIsSpatiallyLoadedFlag()==false, e.g. a non-partitioned / always-loaded actor); nothing was changed"));

    return mcp_common::MakeOkReply(ResultObj);
}

// ==========================================================================
// list_orphaned_ofpa_actors / remove_orphaned_ofpa_actors.
//
// __ExternalActors__ packages can fall out of sync with the World Partition actor-descriptor set
// (e.g. after a bad delete+save): the package stays on disk (and sometimes even in the
// AssetRegistry) but is not part of the level's registered actor set, making it invisible to
// get_actors_by_class (loaded-actor iteration) AND to list_world_partition_actors (descriptor
// walk over the CURRENTLY REGISTERED set) alike. These two tools find and (honest-gated) remove
// such orphans WITHOUT ever loading the orphaned package or swapping/opening map_path.
// ==========================================================================

namespace
{
    // Result of the 3-layer orphan scan for one level's external-actors folder. Layer 3 (the live
    // WP descriptor cross-check) is the only DEFINITIVE signal and is only available when map_path
    // is the CURRENTLY OPEN editor world with an initialized World Partition -- the recommended
    // cleanup procedure is "clean up orphans with the editor on a DIFFERENT map", so callers
    // should expect bLiveCrossCheckAvailable=false in the common case.
    struct FOfpaOrphanScan
    {
        FString ExternalActorsPath;
        TSet<FName> DiskPackages;                                  // Layer 1: raw filesystem scan (ground truth)
        TMap<FName, FTopLevelAssetPath> RegistryClassByPackage;    // Layer 2: AssetRegistry (zero-load class info)
        TSet<FName> LiveRegisteredPackages;                        // Layer 3: live WP actor-descriptor set (join key = GetActorPackage(), NOT GetActorSoftPath())
        bool bLiveCrossCheckAvailable = false;
    };

    FOfpaOrphanScan ScanOfpaOrphanCandidates(const FString& MapPath)
    {
        FOfpaOrphanScan Scan;
        // __ExternalActors__ is inserted at the /Game ROOT followed by the level's own path (e.g.
        // /Game/__ExternalActors__/MyGame/Maps/MainMap for map_path=/Game/MyGame/Maps/MainMap),
        // NOT a sibling folder next to the .umap -- do not hand-roll this string.
        Scan.ExternalActorsPath = ULevel::GetExternalActorsPath(MapPath);

        // Layer 1: raw filesystem scan. Catches packages the AssetRegistry has forgotten entirely
        // -- the exact "merge/registry lost data silently" failure mode.
        {
            FString DiskFolder;
            if (FPackageName::TryConvertLongPackageNameToFilename(Scan.ExternalActorsPath, DiskFolder))
            {
                TArray<FString> FoundFiles;
                IFileManager::Get().FindFilesRecursive(FoundFiles, *DiskFolder, TEXT("*.uasset"), /*Files*/ true, /*Directories*/ false);
                for (const FString& FilePath : FoundFiles)
                {
                    FString PackageName;
                    if (FPackageName::TryConvertFilenameToLongPackageName(FilePath, PackageName))
                    {
                        Scan.DiskPackages.Add(FName(*PackageName));
                    }
                }
            }
        }

        // Layer 2: AssetRegistry scan (bIncludeOnlyOnDiskAssets=true; AssetClassPath gives the
        // GeneratedClass with ZERO loading -- for an external actor package this IS the actor's
        // real UClass).
        {
            TArray<FAssetData> Assets;
            mcp_common::AssetRegistry().GetAssetsByPath(FName(*Scan.ExternalActorsPath), Assets, /*bRecursive*/ true, /*bIncludeOnlyOnDiskAssets*/ true);
            for (const FAssetData& Asset : Assets)
            {
                Scan.RegistryClassByPackage.Add(Asset.PackageName, Asset.AssetClassPath);
            }
        }

        // Layer 3: live WP actor-descriptor cross-check -- ONLY when map_path IS the currently
        // open editor world (never loads/swaps to a different map to answer this). Reuses the
        // EXACT same walk as list_world_partition_actors (FActorDescContainerInstanceCollection::
        // TIterator over UWorld::GetWorldPartition()), joined on GetActorPackage() (the external
        // UPackage name) -- NOT GetActorSoftPath() (an in-level OBJECT path that would never
        // string-match an AssetRegistry package name).
        if (UWorld* EditorWorld = mcp_common::GetEditorWorld())
        {
            if (EditorWorld->GetOutermost()->GetName() == MapPath)
            {
                if (UWorldPartition* WP = EditorWorld->GetWorldPartition())
                {
                    if (WP->IsInitialized())
                    {
                        Scan.bLiveCrossCheckAvailable = true;
                        for (FActorDescContainerInstanceCollection::TIterator<> It(WP); It; ++It)
                        {
                            if (FWorldPartitionActorDescInstance* Desc = *It)
                            {
                                Scan.LiveRegisteredPackages.Add(Desc->GetActorPackage());
                            }
                        }
                    }
                }
            }
        }

        return Scan;
    }

    // Classifies ONE package against a completed scan. Optionally writes its registry class path
    // (FTopLevelAssetPath{}/empty if not in the registry) to OutClass.
    //   confirmed_orphan    - on disk but the AssetRegistry has NO record at all (strongest
    //                         possible signal), OR a live cross-check is available and the
    //                         descriptor set does not include it despite disk+registry presence.
    //   candidate_unconfirmed - on disk AND in the AssetRegistry, but no live cross-check was
    //                         available to confirm either way (the common real-world case, since
    //                         the recommended procedure is to clean up orphans from a DIFFERENT map).
    //   registered          - live cross-check available AND the package IS in the descriptor set;
    //                         not an orphan.
    //   registered_no_file  - AssetRegistry references the package but no file remains on disk (a
    //                         different, also-interesting anomaly; not what these tools clean up).
    //   unknown             - seen by neither disk nor registry (e.g. a typo'd / unrelated path).
    FString ClassifyOfpaPackage(const FOfpaOrphanScan& Scan, FName PackageName, FTopLevelAssetPath* OutClass = nullptr)
    {
        const bool bOnDisk = Scan.DiskPackages.Contains(PackageName);
        const FTopLevelAssetPath* ClassPath = Scan.RegistryClassByPackage.Find(PackageName);
        const bool bInRegistry = (ClassPath != nullptr);
        if (OutClass && ClassPath)
        {
            *OutClass = *ClassPath;
        }

        if (!bOnDisk && !bInRegistry) { return TEXT("unknown"); }
        if (bOnDisk && !bInRegistry)  { return TEXT("confirmed_orphan"); }
        if (!bOnDisk && bInRegistry)  { return TEXT("registered_no_file"); }
        // bOnDisk && bInRegistry:
        if (Scan.bLiveCrossCheckAvailable)
        {
            return Scan.LiveRegisteredPackages.Contains(PackageName) ? TEXT("registered") : TEXT("confirmed_orphan");
        }
        return TEXT("candidate_unconfirmed");
    }
}

// MCP:list_orphaned_ofpa_actors - Finds __ExternalActors__ (OFPA) packages with no registered World Partition descriptor -- actors invisible to get_actors_by_class/list_world_partition_actors because their package fell out of sync with the level's registered actor set (e.g. after a bad delete+save). Three enumeration layers, most-to-least authoritative: (1) raw filesystem scan of the level's external-actors folder (catches AssetRegistry blind spots); (2) AssetRegistry GetAssetsByPath (bIncludeOnlyOnDiskAssets=true, zero-load class info via AssetClassPath); (3) IF map_path is the CURRENTLY OPEN editor world with an initialized World Partition, a live cross-check against the real descriptor set. When map_path is a DIFFERENT map (the recommended procedure: clean up orphans with the editor on another map), layer 3 is skipped and disk-and-registry-both-present candidates are reported as candidate_unconfirmed rather than a false-confident confirmed_orphan. Read-only: never loads a package, never opens/swaps to map_path.
//   @param map_path - Long package name of the level to inspect (e.g. /Game/MyGame/Maps/MainMap); does NOT need to be the open editor world (that only enables the stronger layer-3 cross-check).
//   @param filter_generated_class - Optional substring (case-insensitive) matched against each candidate's generated_class_name (the AssetClassPath, resolved with zero loading); also tried as a UClass IsChildOf when the string resolves to a real class.
//   @returns { map_path, external_actors_path, disk_count, asset_registry_count, live_registered_count (nullable), live_cross_check_available, confirmed_orphan_count, candidate_unconfirmed_count, candidates: [{ path, generated_class_name, is_loaded, on_disk, in_asset_registry, classification }], note }
TSharedPtr<FJsonObject> HandleListOrphanedOfpaActors(const TSharedPtr<FJsonObject>& Params)
{
    FString MapPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("map_path"), MapPath)) { return Missing; }
    FString FilterGeneratedClass;
    Params->TryGetStringField(TEXT("filter_generated_class"), FilterGeneratedClass);

    const FOfpaOrphanScan Scan = ScanOfpaOrphanCandidates(MapPath);

    UClass* FilterClass = nullptr;
    if (!FilterGeneratedClass.IsEmpty())
    {
        FilterClass = FUEAgentGraphUtils::ResolveClass(FilterGeneratedClass);
    }

    // Union of every package known to disk OR the registry.
    TSet<FName> AllPackages = Scan.DiskPackages;
    for (const TPair<FName, FTopLevelAssetPath>& KV : Scan.RegistryClassByPackage)
    {
        AllPackages.Add(KV.Key);
    }

    TArray<TSharedPtr<FJsonValue>> Candidates;
    int32 ConfirmedOrphanCount = 0;
    int32 CandidateUnconfirmedCount = 0;

    for (const FName& PackageName : AllPackages)
    {
        FTopLevelAssetPath ClassPath;
        const FString Classification = ClassifyOfpaPackage(Scan, PackageName, &ClassPath);
        if (Classification == TEXT("registered"))
        {
            continue; // not an orphan; omit from the candidate list
        }

        const FString GeneratedClassName = ClassPath.IsValid() ? ClassPath.ToString() : FString();
        if (!FilterGeneratedClass.IsEmpty())
        {
            bool bMatch = GeneratedClassName.Contains(FilterGeneratedClass, ESearchCase::IgnoreCase);
            if (!bMatch && FilterClass && !GeneratedClassName.IsEmpty())
            {
                if (UObject* ClassObj = FSoftObjectPath(GeneratedClassName).ResolveObject())
                {
                    if (UClass* AsClass = Cast<UClass>(ClassObj))
                    {
                        bMatch = AsClass->IsChildOf(FilterClass);
                    }
                }
            }
            if (!bMatch)
            {
                continue;
            }
        }

        if (Classification == TEXT("confirmed_orphan")) { ++ConfirmedOrphanCount; }
        else if (Classification == TEXT("candidate_unconfirmed")) { ++CandidateUnconfirmedCount; }

        const bool bIsLoaded = (FindPackage(nullptr, *PackageName.ToString()) != nullptr);

        TSharedPtr<FJsonObject> CandidateObj = MakeShared<FJsonObject>();
        CandidateObj->SetStringField(TEXT("path"), PackageName.ToString());
        CandidateObj->SetStringField(TEXT("generated_class_name"), GeneratedClassName);
        CandidateObj->SetBoolField(TEXT("is_loaded"), bIsLoaded);
        CandidateObj->SetBoolField(TEXT("on_disk"), Scan.DiskPackages.Contains(PackageName));
        CandidateObj->SetBoolField(TEXT("in_asset_registry"), Scan.RegistryClassByPackage.Contains(PackageName));
        CandidateObj->SetStringField(TEXT("classification"), Classification);
        Candidates.Add(MakeShared<FJsonValueObject>(CandidateObj));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("map_path"), MapPath);
    ResultObj->SetStringField(TEXT("external_actors_path"), Scan.ExternalActorsPath);
    ResultObj->SetNumberField(TEXT("disk_count"), Scan.DiskPackages.Num());
    ResultObj->SetNumberField(TEXT("asset_registry_count"), Scan.RegistryClassByPackage.Num());
    if (Scan.bLiveCrossCheckAvailable)
    {
        ResultObj->SetNumberField(TEXT("live_registered_count"), Scan.LiveRegisteredPackages.Num());
    }
    else
    {
        ResultObj->SetField(TEXT("live_registered_count"), MakeShared<FJsonValueNull>());
    }
    ResultObj->SetBoolField(TEXT("live_cross_check_available"), Scan.bLiveCrossCheckAvailable);
    ResultObj->SetNumberField(TEXT("confirmed_orphan_count"), ConfirmedOrphanCount);
    ResultObj->SetNumberField(TEXT("candidate_unconfirmed_count"), CandidateUnconfirmedCount);
    ResultObj->SetArrayField(TEXT("candidates"), Candidates);
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetStringField(TEXT("note"), Scan.bLiveCrossCheckAvailable
        ? TEXT("Live World Partition descriptor cross-check available (map_path is the open editor world): confirmed_orphan is a definitive result.")
        : TEXT("map_path is NOT the currently open editor world, so there is no live World Partition descriptor cross-check available; a disk/registry mismatch (registry has no record at all) is still reported as confirmed_orphan (the strongest possible signal short of loading the level), while disk-and-registry-both-present candidates are reported as candidate_unconfirmed -- open map_path read-only and re-run, or call list_world_partition_actors after doing so, for a definitive answer."));

    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:remove_orphaned_ofpa_actors - Honest-gated deletion of __ExternalActors__ (OFPA) orphan packages. Every entry in actor_paths is RE-VERIFIED against a fresh 3-layer scan (never trusts the caller's list, e.g. from a stale list_orphaned_ofpa_actors call) -- only a definitive classification='confirmed_orphan' is eligible for deletion; anything else (candidate_unconfirmed / registered / registered_no_file / unknown) is refused regardless of dry_run/force_save. dry_run DEFAULTS TO TRUE (deliberate deviation from a delete-by-default signature: editor crashes around OFPA + World Partition actor deletion are documented, and this tool is unexercised) and is ALWAYS forced to true -- no override parameter exists for this -- when map_path is the CURRENTLY OPEN editor world; open a different map first to actually delete (the recommended cleanup procedure). Deletion (when dry_run=false) is raw file removal of the .uasset (+ sibling .uexp/.ubulk), NOT ObjectTools::ForceDeleteObjects on a bare AActor package -- that path is untested here for a standalone-loaded actor with no owning level, and is plausibly part of why in-editor OFPA actor deletion crashes; a package still loaded in memory is refused unless force_save=true.
//   @param map_path - Long package name of the level whose orphans to remove (e.g. /Game/MyGame/Maps/MainMap).
//   @param actor_paths - Array of external-actor PACKAGE path strings to remove (the 'path' field from list_orphaned_ofpa_actors).
//   @param dry_run - Default true. When true (or forced true by the active-world gate), nothing is deleted; would_delete reports what WOULD be removed. Pass false to actually delete.
//   @param force_save - Default false. Only meaningful when dry_run=false: permits deleting a candidate that is currently loaded in memory (is_loaded=true). Never overrides the confirmed_orphan re-verification requirement or the active-world gate.
//   @returns { map_path, requested_count, refused_active_world, dry_run, would_delete: [{path, generated_class_name, is_loaded}], deleted: [{path, deleted, error?}], refused_not_orphan: [{path, classification, reason}], refused_loaded: [{path, reason}], deleted_count, note }
TSharedPtr<FJsonObject> HandleRemoveOrphanedOfpaActors(const TSharedPtr<FJsonObject>& Params)
{
    FString MapPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("map_path"), MapPath)) { return Missing; }
    const TArray<TSharedPtr<FJsonValue>>* ActorPathsArray = nullptr;
    if (!Params->TryGetArrayField(TEXT("actor_paths"), ActorPathsArray) || !ActorPathsArray || ActorPathsArray->Num() == 0)
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: actor_paths (non-empty array of package path strings, e.g. the 'path' field from list_orphaned_ofpa_actors)"));
    }

    bool bDryRun = true;
    Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
    bool bForceSave = false;
    Params->TryGetBoolField(TEXT("force_save"), bForceSave);

    // Hard active-world gate -- deliberately NO override/force parameter for this specific check
    // (OFPA deletion is a documented crash zone). If map_path is the CURRENTLY OPEN
    // editor world, this tool ALWAYS behaves as dry_run=true regardless of what was requested.
    bool bRefusedActiveWorld = false;
    if (UWorld* EditorWorld = mcp_common::GetEditorWorld())
    {
        if (EditorWorld->GetOutermost()->GetName() == MapPath)
        {
            bRefusedActiveWorld = true;
            bDryRun = true;
        }
    }

    const FOfpaOrphanScan Scan = ScanOfpaOrphanCandidates(MapPath);

    TArray<TSharedPtr<FJsonValue>> WouldDelete;
    TArray<TSharedPtr<FJsonValue>> Deleted;
    TArray<TSharedPtr<FJsonValue>> RefusedNotOrphan;
    TArray<TSharedPtr<FJsonValue>> RefusedLoaded;
    int32 DeletedCount = 0;

    for (const TSharedPtr<FJsonValue>& Item : *ActorPathsArray)
    {
        const FString PathStr = Item.IsValid() ? Item->AsString() : FString();
        if (PathStr.IsEmpty())
        {
            continue;
        }
        const FName PackageName(*PathStr);

        FTopLevelAssetPath ClassPath;
        const FString Classification = ClassifyOfpaPackage(Scan, PackageName, &ClassPath);

        // Never trust the caller's list -- only a FRESH, definitive confirmed_orphan is eligible
        // for deletion. Everything else is refused regardless of force_save/dry_run.
        if (Classification != TEXT("confirmed_orphan"))
        {
            TSharedPtr<FJsonObject> RefusedObj = MakeShared<FJsonObject>();
            RefusedObj->SetStringField(TEXT("path"), PathStr);
            RefusedObj->SetStringField(TEXT("classification"), Classification);
            RefusedObj->SetStringField(TEXT("reason"), FString::Printf(
                TEXT("Re-verification found classification='%s', not 'confirmed_orphan'; refusing to delete"), *Classification));
            RefusedNotOrphan.Add(MakeShared<FJsonValueObject>(RefusedObj));
            continue;
        }

        const bool bIsLoaded = (FindPackage(nullptr, *PathStr) != nullptr);
        if (bIsLoaded && !bForceSave)
        {
            TSharedPtr<FJsonObject> RefusedObj = MakeShared<FJsonObject>();
            RefusedObj->SetStringField(TEXT("path"), PathStr);
            RefusedObj->SetStringField(TEXT("reason"), TEXT("Package is currently loaded in memory; pass force_save=true to proceed anyway"));
            RefusedLoaded.Add(MakeShared<FJsonValueObject>(RefusedObj));
            continue;
        }

        TSharedPtr<FJsonObject> CandidateObj = MakeShared<FJsonObject>();
        CandidateObj->SetStringField(TEXT("path"), PathStr);
        CandidateObj->SetStringField(TEXT("generated_class_name"), ClassPath.IsValid() ? ClassPath.ToString() : FString());
        CandidateObj->SetBoolField(TEXT("is_loaded"), bIsLoaded);
        WouldDelete.Add(MakeShared<FJsonValueObject>(CandidateObj));

        if (!bDryRun)
        {
            // Raw file delete -- NEVER ObjectTools::ForceDeleteObjects on a bare, standalone-loaded
            // AActor (untested in this codebase; sidesteps ever instantiating the actor object,
            // which is plausibly part of why in-editor OFPA actor deletion crashes). The
            // AssetRegistry cache may keep a stale entry until the next scan/restart.
            FString DiskFile;
            bool bDeleted = false;
            FString DeleteError;
            if (FPackageName::TryConvertLongPackageNameToFilename(PathStr, DiskFile, TEXT(".uasset")))
            {
                bDeleted = IFileManager::Get().Delete(*DiskFile, /*RequireExists*/ false, /*EvenReadOnly*/ true);
                if (!bDeleted)
                {
                    DeleteError = FString::Printf(TEXT("Failed to delete %s"), *DiskFile);
                }
                else
                {
                    for (const TCHAR* SiblingExt : { TEXT("uexp"), TEXT("ubulk") })
                    {
                        const FString SiblingFile = FPaths::ChangeExtension(DiskFile, SiblingExt);
                        if (FPaths::FileExists(SiblingFile))
                        {
                            IFileManager::Get().Delete(*SiblingFile, /*RequireExists*/ false, /*EvenReadOnly*/ true);
                        }
                    }
                }
            }
            else
            {
                DeleteError = TEXT("Could not resolve package path to a disk filename");
            }

            TSharedPtr<FJsonObject> DeletedObj = MakeShared<FJsonObject>();
            DeletedObj->SetStringField(TEXT("path"), PathStr);
            DeletedObj->SetBoolField(TEXT("deleted"), bDeleted);
            if (!bDeleted)
            {
                DeletedObj->SetStringField(TEXT("error"), DeleteError);
            }
            Deleted.Add(MakeShared<FJsonValueObject>(DeletedObj));
            if (bDeleted)
            {
                ++DeletedCount;
            }
        }
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("map_path"), MapPath);
    ResultObj->SetNumberField(TEXT("requested_count"), ActorPathsArray->Num());
    ResultObj->SetBoolField(TEXT("refused_active_world"), bRefusedActiveWorld);
    ResultObj->SetBoolField(TEXT("dry_run"), bDryRun);
    ResultObj->SetArrayField(TEXT("would_delete"), WouldDelete);
    ResultObj->SetArrayField(TEXT("deleted"), Deleted);
    ResultObj->SetArrayField(TEXT("refused_not_orphan"), RefusedNotOrphan);
    ResultObj->SetArrayField(TEXT("refused_loaded"), RefusedLoaded);
    ResultObj->SetNumberField(TEXT("deleted_count"), DeletedCount);
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetStringField(TEXT("note"), bRefusedActiveWorld
        ? TEXT("map_path is the CURRENTLY OPEN editor world; this tool refuses to ever delete while that is true (documented OFPA-deletion crash history) -- forced to dry_run=true regardless of the dry_run parameter. Open a different map first, then re-run with dry_run=false.")
        : (bDryRun
            ? TEXT("dry_run=true (the default): nothing was deleted. would_delete lists the packages that WOULD be removed; re-run with dry_run=false to actually delete them.")
            : TEXT("Deleted via raw file removal (.uasset/.uexp/.ubulk), NOT ObjectTools::ForceDeleteObjects (untested on bare OFPA actor packages) -- the AssetRegistry cache may still show a stale entry until the next scan/restart.")));

    return mcp_common::MakeOkReply(ResultObj);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("assign_actor_to_data_layer_instance"), &HandleAssignActorToDataLayerInstance },
    { TEXT("build_minimap"), &HandleBuildMinimap },
    { TEXT("convert_map_to_world_partition"), &HandleConvertMapToWorldPartition },
    { TEXT("create_data_layer_instance"), &HandleCreateDataLayerInstance },
    { TEXT("create_world_partition_map"), &HandleCreateWorldPartitionMap },
    { TEXT("get_data_layer_instances"), &HandleGetDataLayerInstances },
    { TEXT("get_world_partition_info"), &HandleGetWorldPartitionInfo },
    { TEXT("get_world_partition_runtime_hash"), &HandleGetWorldPartitionRuntimeHash },
    { TEXT("list_orphaned_ofpa_actors"), &HandleListOrphanedOfpaActors },
    { TEXT("list_world_partition_actors"), &HandleListWorldPartitionActors },
    { TEXT("load_world_partition_region"), &HandleLoadWorldPartitionRegion },
    { TEXT("remove_actor_from_data_layer_instance"), &HandleRemoveActorFromDataLayerInstance },
    { TEXT("remove_data_layer_instance"), &HandleRemoveDataLayerInstance },
    { TEXT("remove_orphaned_ofpa_actors"), &HandleRemoveOrphanedOfpaActors },
    { TEXT("set_actor_spatially_loaded"), &HandleSetActorSpatiallyLoaded },
    { TEXT("set_data_layer_parent"), &HandleSetDataLayerParent },
    { TEXT("set_world_partition_runtime_hash"), &HandleSetWorldPartitionRuntimeHash },
};
} // namespace UEAgentWorldPartitionCommands
