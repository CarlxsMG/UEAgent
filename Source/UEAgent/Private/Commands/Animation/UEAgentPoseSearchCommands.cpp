#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Animation/AnimAssetUtils.h"

#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"   // FArrayProperty / FScriptArrayHelper / FObjectPropertyBase — reflection resize of the private Channels TArray in remove_pose_search_channel_from_schema

// PoseSearch runtime + editor (POSESEARCH_API / POSESEARCHEDITOR_API).
// All asset types derive from UDataAsset; their channels/index APIs are
// declared in the Runtime module's public headers (see below). The async
// build manager (FAsyncPoseSearchDatabasesManagement) is editor-only.
#include "PoseSearch/PoseSearchSchema.h"
#include "PoseSearch/PoseSearchDerivedData.h"   // FAsyncPoseSearchDatabasesManagement + ERequestAsyncBuildFlag + EAsyncBuildIndexResult
#include "PoseSearch/PoseSearchDatabase.h"
#include "PoseSearch/PoseSearchFeatureChannel.h"
#include "PoseSearch/PoseSearchFeatureChannel_Pose.h"
#include "PoseSearch/PoseSearchFeatureChannel_Trajectory.h"
#include "PoseSearch/PoseSearchFeatureChannel_Position.h"
#include "PoseSearch/PoseSearchFeatureChannel_Velocity.h"
#include "PoseSearch/PoseSearchFeatureChannel_Heading.h"
#include "Animation/MirrorDataTable.h"   // UMirrorDataTable fully defined for FPoseSearchRoledSkeleton::MirrorDataTable->GetPathName()
// Runtime motion-matching inspection (get_pose_search_runtime_state / evaluate_pose_search_query).
#include "PoseSearch/PoseSearchLibrary.h"   // UPoseSearchLibrary::MotionMatch/FindPoseHistoryNode + FPoseSearchBlueprintResult/ContinuingProperties/FutureProperties
#include "Animation/TrajectoryTypes.h"      // FTransformTrajectory / FTransformTrajectorySample (Engine module) — read a live actor's MotionTrajectory by reflection
#include "Components/SkeletalMeshComponent.h"
#include "Animation/AnimInstance.h"
#include "GameFramework/Actor.h"            // AActor::GetComponents / FindComponentByClass
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "Engine/Engine.h"

#define LOCTEXT_NAMESPACE "UEAgentPoseSearchCommands"

/**
 * PoseSearch commands: authoring of UPoseSearchSchema and UPoseSearchDatabase assets for the PoseSearch plugin
 * (Runtime module POSESEARCH_API, Editor module POSESEARCHEDITOR_API), plus live motion-matching inspection of
 * a PIE actor.
 *
 * GOTCHA (Finalize is private): UPoseSearchSchema::Finalize() does not compile from external modules. It runs
 *   inside AddDefaultChannels(), or through Schema->PostEditChange(), whose editor-only override calls it; the
 *   commands that change a schema's channels or settings therefore call PostEditChange() after the edit.
 */
namespace UEAgentPoseSearchCommands
{

// MCP-DOMAIN: Animation

namespace
{
    static const TCHAR* kDefaultPackagePath = mcp_common::DefaultAssetFolder();
    static const TCHAR* kDefaultSkeletonPath = TEXT("/Engine/EngineMeshes/SkeletalCube_Skeleton");

    // Resolve the channel UClass* from the user-facing channel_type string.
    // Returns nullptr for unknown strings (caller decides error wording).
    UClass* ResolveChannelClass(const FString& ChannelType)
    {
        if (ChannelType == TEXT("Pose"))       { return UPoseSearchFeatureChannel_Pose::StaticClass(); }
        if (ChannelType == TEXT("Trajectory")) { return UPoseSearchFeatureChannel_Trajectory::StaticClass(); }
        if (ChannelType == TEXT("Position"))   { return UPoseSearchFeatureChannel_Position::StaticClass(); }
        if (ChannelType == TEXT("Velocity"))   { return UPoseSearchFeatureChannel_Velocity::StaticClass(); }
        if (ChannelType == TEXT("Heading"))    { return UPoseSearchFeatureChannel_Heading::StaticClass(); }
        return nullptr;
    }

    // UPoseSearchSchema::FindFirstChannelOfType<T>() is templated, so we
    // can't call it dynamically. Instead we iterate the public
    // FinalizedChannels view and look for a cast match on ChannelClass.
    // This covers AddChannel + PostEditChange(Finalize) writes that have
    // already been finalized, AND AddDefaultChannels (which auto-finalizes
    // Trajectory + Pose on schema creation).
    const UPoseSearchFeatureChannel* FindChannelOfClass(
        const UPoseSearchSchema* Schema, const UClass* ChannelClass)
    {
        if (!Schema || !ChannelClass)
        {
            return nullptr;
        }
        for (const TObjectPtr<UPoseSearchFeatureChannel>& ChannelPtr : Schema->GetChannels())
        {
            if (ChannelPtr && ChannelPtr->IsA(ChannelClass))
            {
                return ChannelPtr.Get();
            }
        }
        return nullptr;
    }

    // The schema's finalized channels as [{ class, channel_name, weight }] (weight 1 without a Weight property).
    TArray<TSharedPtr<FJsonValue>> PoseSearch_ChannelsJson(const UPoseSearchSchema* Schema)
    {
        TArray<TSharedPtr<FJsonValue>> ChannelsJson;
        for (const TObjectPtr<UPoseSearchFeatureChannel>& ChannelPtr : Schema->GetChannels())
        {
            if (!ChannelPtr)
            {
                continue;
            }
            TSharedPtr<FJsonObject> ChanObj = MakeShared<FJsonObject>();
            ChanObj->SetStringField(TEXT("class"), ChannelPtr->GetClass()->GetName());
            ChanObj->SetStringField(TEXT("channel_name"),
                ChannelPtr->GetClass()->GetName().Replace(TEXT("PoseSearchFeatureChannel_"), TEXT("")));
            const FFloatProperty* WeightProp = FindFProperty<FFloatProperty>(ChannelPtr->GetClass(), TEXT("Weight"));
            ChanObj->SetNumberField(TEXT("weight"),
                WeightProp ? WeightProp->GetPropertyValue_InContainer(ChannelPtr) : 1.0f);
            ChannelsJson.Add(MakeShared<FJsonValueObject>(ChanObj));
        }
        return ChannelsJson;
    }

    // Deletes the asset at AssetPath only when it is a T: the payload of the outcome, or nullptr with
    // OutError set to the reply when the asset exists but does not load.
    template <typename T>
    TSharedPtr<FJsonObject> PoseSearch_DeleteTypedAsset(const FString& AssetPath, const TCHAR* PathField,
        const TCHAR* Command, const TCHAR* TypeLabel, TSharedPtr<FJsonObject>& OutError)
    {
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetStringField(PathField, AssetPath);
        if (!UEditorAssetLibrary::DoesAssetExist(AssetPath))
        {
            Resp->SetBoolField(TEXT("deleted"), false);
            Resp->SetBoolField(TEXT("already_existed"), false);
            Resp->SetStringField(TEXT("note"), FString::Printf(TEXT("%s does not exist; nothing to remove"), PathField));
            return Resp;
        }
        UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
        if (!Asset)
        {
            OutError = mcp_common::MakeErrorReply(
                FString::Printf(TEXT("%s: could not load asset at %s"), Command, *AssetPath));
            return nullptr;
        }
        const FString AssetClassName = Asset->GetClass()->GetName();
        Resp->SetStringField(TEXT("asset_class"), AssetClassName);
        if (!Cast<T>(Asset))
        {
            Resp->SetBoolField(TEXT("deleted"), false);
            Resp->SetStringField(TEXT("note"),
                FString::Printf(TEXT("asset is not a %s (got %s) - wrong type, refusing to delete"), TypeLabel, *AssetClassName));
            return Resp;
        }
        Resp->SetBoolField(TEXT("deleted"), UEditorAssetLibrary::DeleteAsset(AssetPath));
        Resp->SetBoolField(TEXT("already_existed"), true);
        return Resp;
    }
}

// ---------------------------------------------------------------------------
// create_pose_search_schema
// ---------------------------------------------------------------------------
// MCP:create_pose_search_schema - Create a UPoseSearchSchema data asset at package_path/name (package_path defaults to /Game/MCP) bound to the USkeleton at skeleton_path, with sample_rate clamped to 1..240 and the default Trajectory and Pose channels, then save it. Idempotent: a schema already at that path is returned unchanged with already_existed true and saved false, nothing saved, its skeleton_path and skeleton_paths read from it; the requested skeleton and sample_rate are not applied, and that skeleton is not even loaded. An invalid path, an asset of another class there, or for a new schema a skeleton that does not load, is an error; a failed save of a new schema makes success false. Next create a database with create_pose_search_database; add channels with add_pose_search_channel_to_schema, change settings with set_pose_search_schema_properties, and inspect it with inspect_pose_search_schema.
//   @param name - Asset name (e.g. 'PoseSearchSchema_Example').
//   @param package_path - Target content package path (default /Game/MCP); trailing slashes are stripped.
//   @param skeleton_path - Asset path of the USkeleton (default /Engine/EngineMeshes/SkeletalCube_Skeleton); loaded only for a schema this call creates.
//   @param sample_rate - Schema SampleRate (int, ClampMin=1, ClampMax=240; default 30); out-of-range values are clamped, and it is applied only to a schema this call creates.
//   @returns { name, package_path, asset_path, schema_path, skeleton_path, skeleton_paths, sample_rate, schema_cardinality, channel_count, created, already_existed, saved, save_error, message }
TSharedPtr<FJsonObject> HandleCreatePoseSearchSchema(const TSharedPtr<FJsonObject>& Params)
{
    FString Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name)) { return Missing; }

    FString PackagePath;
    if (!Params->TryGetStringField(TEXT("package_path"), PackagePath) || PackagePath.IsEmpty())
    {
        PackagePath = kDefaultPackagePath;
    }
    PackagePath = AnimAssetUtils::StripTrailingSlashes(PackagePath);

    FString SkeletonPath = kDefaultSkeletonPath;
    Params->TryGetStringField(TEXT("skeleton_path"), SkeletonPath);
    if (SkeletonPath.IsEmpty())
    {
        SkeletonPath = kDefaultSkeletonPath;
    }

    int32 SampleRate = 30;
    Params->TryGetNumberField(TEXT("sample_rate"), SampleRate);
    // Mirror the UPROPERTY meta (ClampMin=1, ClampMax=240) so we never write
    // out-of-range values that PostEditChange would log and clamp anyway.
    SampleRate = FMath::Clamp(SampleRate, 1, 240);

    const FString FullPath = FString::Printf(TEXT("%s/%s"), *PackagePath, *Name);
    if (TSharedPtr<FJsonObject> Error = mcp_common::InvalidPackageNameReply(FullPath)) { return Error; }

    auto BuildPayload = [&](UPoseSearchSchema* Schema, bool bCreated) -> TSharedPtr<FJsonObject>
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Name);
        Obj->SetStringField(TEXT("package_path"), PackagePath);
        Obj->SetStringField(TEXT("asset_path"), Schema->GetPathName());
        Obj->SetStringField(TEXT("schema_path"), FullPath);
        // The schema's own skeletons, read back from it rather than echoed from the request: skeleton_path is
        // the first one, skeleton_paths all of them.
        TArray<FString> SkeletonPaths;
        for (const FPoseSearchRoledSkeleton& RoledSkeleton : Schema->GetRoledSkeletons())
        {
            if (RoledSkeleton.Skeleton)
            {
                SkeletonPaths.Add(RoledSkeleton.Skeleton->GetPathName());
            }
        }
        Obj->SetStringField(TEXT("skeleton_path"), SkeletonPaths.IsEmpty() ? FString() : SkeletonPaths[0]);
        TArray<TSharedPtr<FJsonValue>> SkeletonPathsJson;
        for (const FString& Path : SkeletonPaths)
        {
            SkeletonPathsJson.Add(MakeShared<FJsonValueString>(Path));
        }
        Obj->SetArrayField(TEXT("skeleton_paths"), SkeletonPathsJson);
        Obj->SetNumberField(TEXT("sample_rate"), Schema->SampleRate);
        Obj->SetNumberField(TEXT("schema_cardinality"), Schema->SchemaCardinality);
        Obj->SetNumberField(TEXT("channel_count"), Schema->GetChannels().Num());
        Obj->SetBoolField(TEXT("created"), bCreated);
        return Obj;
    };

    // Idempotent: an existing schema is returned as it is. Nothing is applied to it, so nothing is saved, and
    // the requested skeleton is not needed.
    if (UPoseSearchSchema* Existing = AnimAssetUtils::FindExisting<UPoseSearchSchema>(FullPath))
    {
        TSharedPtr<FJsonObject> Obj = BuildPayload(Existing, /*bCreated=*/false);
        Obj->SetBoolField(TEXT("already_existed"), true);
        Obj->SetBoolField(TEXT("saved"), false);
        Obj->SetStringField(TEXT("message"),
            TEXT("UPoseSearchSchema already exists; loaded the existing asset unchanged, nothing saved"));
        return mcp_common::MakeOkReply(Obj);
    }
    // An asset of another class at that path is refused before the skeleton is loaded.
    if (const FString Conflict = mcp_common::AssetNameConflict(FullPath, Name, UPoseSearchSchema::StaticClass()); !Conflict.IsEmpty())
    {
        return mcp_common::MakeErrorReply(Conflict);
    }

    // Loading a path with "//" would create its package, and CreatePackage aborts the editor on such a name.
    if (TSharedPtr<FJsonObject> Error = mcp_common::InvalidPackageNameReply(SkeletonPath)) { return Error; }
    USkeleton* Skeleton = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(SkeletonPath, TEXT("Skeleton"), Skeleton)) { return Error; }

    // NewObject direct — UPoseSearchSchemaFactory opens a modal skeleton picker
    // via ConfigureProperties() which is not headless-safe. AddDefaultChannels()
    // internally AddChannel(UPoseSearchFeatureChannel_Trajectory) +
    // AddChannel(UPoseSearchFeatureChannel_Pose) + Finalize(), so the resulting
    // schema is FinalizedChannels-ready by the time we save.
    FString CreateError;
    UPoseSearchSchema* Schema = mcp_common::NewAssetInPackage<UPoseSearchSchema>(FullPath, Name,
        TEXT("Failed to create UPoseSearchSchema"), CreateError);
    if (!Schema)
    {
        return mcp_common::MakeErrorReply(CreateError);
    }

    Schema->SampleRate = SampleRate;
    Schema->AddSkeleton(Skeleton);

    // AddDefaultChannels appends Trajectory + Pose and calls Finalize()
    // (PoseSearchSchema.cpp) — SchemaCardinality lands > 0 with no extra
    // PostEditChange hop.
    Schema->AddDefaultChannels();

    FAssetRegistryModule::AssetCreated(Schema);

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Schema->GetPackage(), Schema, SaveError);

    TSharedPtr<FJsonObject> Obj = BuildPayload(Schema, /*bCreated=*/true);
    Obj->SetBoolField(TEXT("already_existed"), false);
    Obj->SetStringField(TEXT("message"), bSaved
        ? TEXT("UPoseSearchSchema created and saved to disk")
        : FString::Printf(TEXT("UPoseSearchSchema created but save failed: %s"), *SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("create_pose_search_schema"), SaveError);
}

// ---------------------------------------------------------------------------
// create_pose_search_database
// ---------------------------------------------------------------------------
// MCP:create_pose_search_database - Create a UPoseSearchDatabase data asset at package_path/name (package_path defaults to /Game/MCP) whose Schema is the UPoseSearchSchema at schema_path, with no animation assets yet, then save it. Idempotent: a database already at that path is returned unchanged with already_existed true and saved false, nothing saved, its schema_path read from it; the requested schema is not applied, and not even loaded. An invalid path, an asset of another class there, or for a new database a schema that does not load, is an error; a failed save of a new database makes success false. Create the schema with create_pose_search_schema; change the schema of an existing database with set_pose_search_database_schema; add animations with add_animation_asset_to_pose_search_database, then run build_pose_search_database_index.
//   @param name - Asset name (e.g. 'PoseSearchDatabase_Example').
//   @param package_path - Target content package path (default /Game/MCP).
//   @param schema_path - Asset path of an existing UPoseSearchSchema (required); loaded only for a database this call creates.
//   @returns { name, package_path, asset_path, database_path, schema_path, schema_cardinality, animation_asset_count, created, already_existed, saved, save_error, message }
TSharedPtr<FJsonObject> HandleCreatePoseSearchDatabase(const TSharedPtr<FJsonObject>& Params)
{
    FString Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name)) { return Missing; }

    FString PackagePath;
    if (!Params->TryGetStringField(TEXT("package_path"), PackagePath) || PackagePath.IsEmpty())
    {
        PackagePath = kDefaultPackagePath;
    }
    PackagePath = AnimAssetUtils::StripTrailingSlashes(PackagePath);

    FString SchemaPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("schema_path"), SchemaPath)) { return Missing; }

    const FString FullPath = FString::Printf(TEXT("%s/%s"), *PackagePath, *Name);
    if (TSharedPtr<FJsonObject> Error = mcp_common::InvalidPackageNameReply(FullPath)) { return Error; }

    auto BuildPayload = [&](UPoseSearchDatabase* Database, bool bCreated) -> TSharedPtr<FJsonObject>
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Name);
        Obj->SetStringField(TEXT("package_path"), PackagePath);
        Obj->SetStringField(TEXT("asset_path"), Database->GetPathName());
        Obj->SetStringField(TEXT("database_path"), FullPath);
        // The database's own schema, read back from it rather than echoed from the request.
        Obj->SetStringField(TEXT("schema_path"), Database->Schema ? Database->Schema->GetPathName() : FString());
        Obj->SetNumberField(TEXT("schema_cardinality"),
            Database->Schema ? Database->Schema->SchemaCardinality : 0);
        Obj->SetNumberField(TEXT("animation_asset_count"), Database->GetNumAnimationAssets());
        Obj->SetBoolField(TEXT("created"), bCreated);
        return Obj;
    };

    // Idempotent: an existing database is returned as it is. Nothing is applied to it, so nothing is saved, and
    // the requested schema is not needed.
    if (UPoseSearchDatabase* Existing = AnimAssetUtils::FindExisting<UPoseSearchDatabase>(FullPath))
    {
        TSharedPtr<FJsonObject> Obj = BuildPayload(Existing, /*bCreated=*/false);
        Obj->SetBoolField(TEXT("already_existed"), true);
        Obj->SetBoolField(TEXT("saved"), false);
        Obj->SetStringField(TEXT("message"),
            TEXT("UPoseSearchDatabase already exists; loaded the existing asset unchanged, nothing saved"));
        return mcp_common::MakeOkReply(Obj);
    }
    // An asset of another class at that path is refused before the schema is loaded.
    if (const FString Conflict = mcp_common::AssetNameConflict(FullPath, Name, UPoseSearchDatabase::StaticClass()); !Conflict.IsEmpty())
    {
        return mcp_common::MakeErrorReply(Conflict);
    }

    // Loading a path with "//" would create its package, and CreatePackage aborts the editor on such a name.
    if (TSharedPtr<FJsonObject> Error = mcp_common::InvalidPackageNameReply(SchemaPath)) { return Error; }
    UPoseSearchSchema* Schema = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(SchemaPath, TEXT("UPoseSearchSchema"), Schema)) { return Error; }

    FString CreateError;
    UPoseSearchDatabase* Database = mcp_common::NewAssetInPackage<UPoseSearchDatabase>(FullPath, Name,
        TEXT("Failed to create UPoseSearchDatabase"), CreateError);
    if (!Database)
    {
        return mcp_common::MakeErrorReply(CreateError);
    }

    // TObjectPtr<const UPoseSearchSchema> — assigning a non-const UPoseSearchSchema*
    // implicitly converts to const UPoseSearchSchema* via the TObjectPtr ctor.
    Database->Schema = Schema;

    FAssetRegistryModule::AssetCreated(Database);

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Database->GetPackage(), Database, SaveError);

    TSharedPtr<FJsonObject> Obj = BuildPayload(Database, /*bCreated=*/true);
    Obj->SetBoolField(TEXT("already_existed"), false);
    Obj->SetStringField(TEXT("message"), bSaved
        ? TEXT("UPoseSearchDatabase created and saved to disk")
        : FString::Printf(TEXT("UPoseSearchDatabase created but save failed: %s"), *SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("create_pose_search_database"), SaveError);
}

// ---------------------------------------------------------------------------
// add_pose_search_channel_to_schema
// ---------------------------------------------------------------------------
// MCP:add_pose_search_channel_to_schema - Add a feature channel of channel_type to a UPoseSearchSchema asset or, when a channel of that class is already there, overwrite its Weight instead (already_existed=true); then finalize via PostEditChange and save. Returns schema_cardinality and channel_count; success stays true if the save fails: read saved.
//   @param schema_path - Asset path of the UPoseSearchSchema (required).
//   @param channel_type - One of 'Pose', 'Trajectory', 'Position', 'Velocity', 'Heading' (required).
//   @param weight - Channel weight (float); default 1.0, which also resets the weight of an existing channel when omitted
//   @returns { schema_path, channel_type, channel_class, weight, schema_cardinality, channel_count, added, already_existed, weight_updated, saved, message }
TSharedPtr<FJsonObject> HandleAddPoseSearchChannelToSchema(const TSharedPtr<FJsonObject>& Params)
{
    FString SchemaPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("schema_path"), SchemaPath)) { return Missing; }

    FString ChannelType;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("channel_type"), ChannelType)) { return Missing; }

    double WeightD = 1.0;
    Params->TryGetNumberField(TEXT("weight"), WeightD);
    const float Weight = static_cast<float>(WeightD);

    UClass* ChannelClass = ResolveChannelClass(ChannelType);
    if (!ChannelClass)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Unknown channel_type '%s' (expected Pose|Trajectory|Position|Velocity|Heading)"),
                *ChannelType));
    }

    UPoseSearchSchema* Schema = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(SchemaPath, TEXT("UPoseSearchSchema"), Schema)) { return Error; }

    Schema->Modify();

    auto BuildPayload = [&](bool bAlreadyExisted, bool bWeightUpdated) -> TSharedPtr<FJsonObject>
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("schema_path"), SchemaPath);
        Obj->SetStringField(TEXT("channel_type"), ChannelType);
        Obj->SetStringField(TEXT("channel_class"), ChannelClass->GetName());
        Obj->SetNumberField(TEXT("weight"), Weight);
        Obj->SetNumberField(TEXT("schema_cardinality"), Schema->SchemaCardinality);
        Obj->SetNumberField(TEXT("channel_count"), Schema->GetChannels().Num());
        Obj->SetBoolField(TEXT("added"), !bAlreadyExisted);
        Obj->SetBoolField(TEXT("already_existed"), bAlreadyExisted);
        Obj->SetBoolField(TEXT("weight_updated"), bWeightUpdated);
        return Obj;
    };

    // Idempotent: if a channel of this exact class is already on the schema,
    // update its Weight and return already_existed=true (no append).
    if (const UPoseSearchFeatureChannel* Existing = FindChannelOfClass(Schema, ChannelClass))
    {
        // FinalizedChannels entries are typed const but Weight is a public float
        // UPROPERTY on the DERIVED classes (Pose/Trajectory/Position/etc.), NOT on
        // the base UPoseSearchFeatureChannel. Use reflection so this works for any
        // concrete channel subclass we add later. Modify() above keeps the txn clean.
        if (FFloatProperty* WeightProp = FindFProperty<FFloatProperty>(Existing->GetClass(), TEXT("Weight")))
        {
            WeightProp->SetPropertyValue_InContainer(
                const_cast<UPoseSearchFeatureChannel*>(Existing), Weight);
        }

        // PostEditChange triggers Schema's WITH_EDITOR PostEditChangeProperty
        // override, which calls Finalize() (PoseSearchSchema.cpp). This is
        // the canonical external-Finalize path — Schema::Finalize() is private.
        Schema->PostEditChange();

        FString SaveError;
        const bool bSaved = mcp_common::SaveAssetPackage(Schema->GetPackage(), Schema, SaveError);

        TSharedPtr<FJsonObject> Obj = BuildPayload(/*bAlreadyExisted=*/true, /*bWeightUpdated=*/true);
        Obj->SetBoolField(TEXT("saved"), bSaved);
        Obj->SetStringField(TEXT("message"), bSaved
            ? TEXT("Channel of requested type already existed; weight updated and saved")
            : FString::Printf(TEXT("Channel exists, weight updated, save failed: %s"), *SaveError));
        return mcp_common::MakeOkReply(Obj);
    }

    // Append a fresh channel instance and let the post-edit finalize it.
    UPoseSearchFeatureChannel* NewChannel = NewObject<UPoseSearchFeatureChannel>(Schema, ChannelClass,
        NAME_None, RF_Transactional);
    if (!NewChannel)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Failed to NewObject channel of class %s"), *ChannelClass->GetName()));
    }
    if (FFloatProperty* WeightProp = FindFProperty<FFloatProperty>(ChannelClass, TEXT("Weight")))
    {
        WeightProp->SetPropertyValue_InContainer(NewChannel, Weight);
    }
    Schema->AddChannel(NewChannel);

    // PostEditChange -> Schema's override -> Finalize() (private but reached via
    // the editor override). After this, SchemaCardinality reflects the new
    // channel and GetChannels() includes it.
    Schema->PostEditChange();

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Schema->GetPackage(), Schema, SaveError);

    TSharedPtr<FJsonObject> Obj = BuildPayload(/*bAlreadyExisted=*/false, /*bWeightUpdated=*/false);
    Obj->SetBoolField(TEXT("saved"), bSaved);
    Obj->SetStringField(TEXT("message"), bSaved
        ? TEXT("Channel added and saved")
        : FString::Printf(TEXT("Channel added but save failed: %s"), *SaveError));
    return mcp_common::MakeOkReply(Obj);
}

// ---------------------------------------------------------------------------
// remove_pose_search_channel_from_schema  (inverse of add_pose_search_channel_to_schema)
// ---------------------------------------------------------------------------
// MCP:remove_pose_search_channel_from_schema - Remove a UPoseSearchFeatureChannel_* from a UPoseSearchSchema by index or type (inverse of add_pose_search_channel_to_schema)
//   @param schema_path - Asset path of the UPoseSearchSchema (required).
//   @param channel_index - 0-based index into the schema's source Channels array (optional; takes precedence over channel_type).
//   @param channel_type - One of 'Pose', 'Trajectory', 'Position', 'Velocity', 'Heading'; removes the first channel of that class (optional; used only when channel_index is absent).
//   @returns { schema_path, channel_index, channel_type, channel_class, schema_cardinality, channel_count, previous_count, removed, already_absent, saved, message }
TSharedPtr<FJsonObject> HandleRemovePoseSearchChannelFromSchema(const TSharedPtr<FJsonObject>& Params)
{
    FString SchemaPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("schema_path"), SchemaPath)) { return Missing; }

    // Two addressing modes, mirroring the twin's channel_type + the by-index
    // idiom of remove_animation_asset_from_pose_search_database. channel_index
    // is unambiguous and takes precedence; channel_type resolves the first
    // channel of that UClass (same ResolveChannelClass the add path uses).
    int32 ChannelIndex = INDEX_NONE;
    const bool bHasIndex = Params->TryGetNumberField(TEXT("channel_index"), ChannelIndex);

    FString ChannelType;
    const bool bHasType = Params->TryGetStringField(TEXT("channel_type"), ChannelType) && !ChannelType.IsEmpty();

    if (!bHasIndex && !bHasType)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Provide channel_index (0-based) or channel_type (Pose|Trajectory|Position|Velocity|Heading)"));
    }

    UClass* ChannelClass = nullptr;
    if (!bHasIndex)
    {
        ChannelClass = ResolveChannelClass(ChannelType);
        if (!ChannelClass)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Unknown channel_type '%s' (expected Pose|Trajectory|Position|Velocity|Heading)"),
                    *ChannelType));
        }
    }

    UPoseSearchSchema* Schema = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(SchemaPath, TEXT("UPoseSearchSchema"), Schema)) { return Error; }

    // The source-of-truth Channels array (TArray<TObjectPtr<UPoseSearchFeatureChannel>>)
    // is a PRIVATE UPROPERTY (PoseSearchSchema.h) fed by AddChannel — GetChannels()
    // returns the DERIVED FinalizedChannels view, not this backing store. AddChannel
    // does Channels.Add (PoseSearchSchema.cpp) and Finalize() rebuilds
    // FinalizedChannels from Channels (PoseSearchSchema.cpp). There is no
    // public RemoveChannel, so we resize Channels by reflection via FScriptArrayHelper
    // (same private-array-resize idiom as remove_smart_object_slot) — this destructs
    // the removed TObjectPtr element through the property's reflection.
    FArrayProperty* ChannelsProp = FindFProperty<FArrayProperty>(
        UPoseSearchSchema::StaticClass(), TEXT("Channels"));
    if (!ChannelsProp)
    {
        return mcp_common::MakeErrorReply(
            TEXT("UPoseSearchSchema::Channels property not found (engine layout changed)"));
    }
    FScriptArrayHelper Helper(ChannelsProp, ChannelsProp->ContainerPtrToValuePtr<void>(Schema));
    const int32 PreviousCount = Helper.Num();

    // Resolve the concrete index to remove from the backing Channels array.
    // Index mode: use it directly. Type mode: find the first element whose
    // TObjectPtr resolves to an instance of ChannelClass. The element type is
    // FObjectPropertyBase (TObjectPtr), so we read the pointer via the inner
    // property to identify the class without instantiating anything.
    int32 RemoveIndex = INDEX_NONE;
    UClass* ResolvedClass = nullptr;
    if (bHasIndex)
    {
        if (ChannelIndex >= 0 && ChannelIndex < PreviousCount)
        {
            RemoveIndex = ChannelIndex;
            if (const FObjectPropertyBase* InnerObjProp = CastField<FObjectPropertyBase>(ChannelsProp->Inner))
            {
                if (const UObject* Elem = InnerObjProp->GetObjectPropertyValue(Helper.GetRawPtr(RemoveIndex)))
                {
                    ResolvedClass = Elem->GetClass();
                }
            }
        }
    }
    else if (const FObjectPropertyBase* InnerObjProp = CastField<FObjectPropertyBase>(ChannelsProp->Inner))
    {
        for (int32 Idx = 0; Idx < PreviousCount; ++Idx)
        {
            const UObject* Elem = InnerObjProp->GetObjectPropertyValue(Helper.GetRawPtr(Idx));
            if (Elem && Elem->IsA(ChannelClass))
            {
                RemoveIndex = Idx;
                ResolvedClass = Elem->GetClass();
                break;
            }
        }
    }

    auto BuildPayload = [&](bool bRemoved) -> TSharedPtr<FJsonObject>
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("schema_path"), SchemaPath);
        Obj->SetNumberField(TEXT("channel_index"), bHasIndex ? ChannelIndex : RemoveIndex);
        Obj->SetStringField(TEXT("channel_type"), bHasType ? ChannelType : FString());
        Obj->SetStringField(TEXT("channel_class"),
            ResolvedClass ? ResolvedClass->GetName()
                          : (ChannelClass ? ChannelClass->GetName() : FString()));
        Obj->SetNumberField(TEXT("schema_cardinality"), Schema->SchemaCardinality);
        Obj->SetNumberField(TEXT("channel_count"), Schema->GetChannels().Num());
        Obj->SetNumberField(TEXT("previous_count"), PreviousCount);
        Obj->SetBoolField(TEXT("removed"), bRemoved);
        Obj->SetBoolField(TEXT("already_absent"), !bRemoved);
        return Obj;
    };

    // Idempotent: no matching channel (out-of-range index, or no channel of the
    // requested type) -> removed=false + already_absent=true, no write/save. This
    // mirrors the out-of-range note idiom of remove_animation_asset_from_pose_search_database.
    if (RemoveIndex == INDEX_NONE)
    {
        TSharedPtr<FJsonObject> Obj = BuildPayload(/*bRemoved=*/false);
        Obj->SetBoolField(TEXT("saved"), true);
        Obj->SetStringField(TEXT("message"), bHasIndex
            ? FString::Printf(TEXT("channel_index %d out of range [0, %d); nothing removed"), ChannelIndex, PreviousCount)
            : FString::Printf(TEXT("No channel of type '%s' on schema; nothing removed"), *ChannelType));
        return mcp_common::MakeOkReply(Obj);
    }

    Schema->Modify();

    // Resize Channels via reflection (destructs the removed TObjectPtr element).
    Helper.RemoveValues(RemoveIndex, 1);

    // PostEditChange triggers the schema's WITH_EDITOR PostEditChangeProperty
    // override (PoseSearchSchema.cpp), which calls the private Finalize() — this
    // rebuilds FinalizedChannels from the now-shorter Channels and recomputes
    // SchemaCardinality. Same canonical external-Finalize path the add-twin uses.
    Schema->PostEditChange();

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Schema->GetPackage(), Schema, SaveError);

    TSharedPtr<FJsonObject> Obj = BuildPayload(/*bRemoved=*/true);
    Obj->SetStringField(TEXT("message"), bSaved
        ? TEXT("Channel removed and saved")
        : FString::Printf(TEXT("Channel removed but save failed: %s"), *SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("remove_pose_search_channel_from_schema"), SaveError);
}

// ---------------------------------------------------------------------------
// add_animation_asset_to_pose_search_database
// ---------------------------------------------------------------------------
// MCP:add_animation_asset_to_pose_search_database - Append an animation asset to a UPoseSearchDatabase as an enabled entry over its full range, then save the database; an asset already in it is left alone (already_existed=true). The asset type is not checked, and the search index is not rebuilt: run build_pose_search_database_index afterwards.
//   @param database_path - Asset path of the UPoseSearchDatabase (required).
//   @param animation_asset_path - Asset path of a UAnimSequenceBase / UAnimComposite / UAnimMontage / UBlendSpace (required).
//   @returns { database_path, animation_asset_path, animation_asset_count, added, already_existed, saved, message }
TSharedPtr<FJsonObject> HandleAddAnimationAssetToPoseSearchDatabase(const TSharedPtr<FJsonObject>& Params)
{
    FString DatabasePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("database_path"), DatabasePath)) { return Missing; }

    FString AnimationAssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("animation_asset_path"), AnimationAssetPath)) { return Missing; }

    UPoseSearchDatabase* Database = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(DatabasePath, TEXT("UPoseSearchDatabase"), Database)) { return Error; }

    // Loaded as a UObject: the engine's FPoseSearchDatabaseAnimationAsset
    // holds a UObject* (UPROPERTY TObjectPtr<UObject> AnimAsset). The static
    // type-erasure lets the database accept any animatable asset kind.
    UObject* AnimAsset = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(AnimationAssetPath, TEXT("Animation asset"), AnimAsset)) { return Error; }

    // Idempotent: UPoseSearchDatabase::Contains(const UObject*) const is the
    // canonical dedup check (PoseSearchDatabase.h) — checks CachedAssetMap.
    if (Database->Contains(AnimAsset))
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("database_path"), DatabasePath);
        Obj->SetStringField(TEXT("animation_asset_path"), AnimationAssetPath);
        Obj->SetNumberField(TEXT("animation_asset_count"), Database->GetNumAnimationAssets());
        Obj->SetBoolField(TEXT("added"), false);
        Obj->SetBoolField(TEXT("already_existed"), true);
        Obj->SetBoolField(TEXT("saved"), true);
        Obj->SetStringField(TEXT("message"), TEXT("Animation asset already in database"));
        return mcp_common::MakeOkReply(Obj);
    }

    Database->Modify();

    // Defaults match a freshly-authored asset entry: bEnabled=true,
    // SamplingRange={0,0} (entire frame range), MirrorOption=UnmirroredOnly.
    FPoseSearchDatabaseAnimationAsset NewAsset;
    NewAsset.AnimAsset = AnimAsset;
    NewAsset.bEnabled = true;
    NewAsset.SamplingRange = FFloatInterval(0.f, 0.f);
    Database->AddAnimationAsset(NewAsset);

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Database->GetPackage(), Database, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("database_path"), DatabasePath);
    Obj->SetStringField(TEXT("animation_asset_path"), AnimationAssetPath);
    Obj->SetNumberField(TEXT("animation_asset_count"), Database->GetNumAnimationAssets());
    Obj->SetBoolField(TEXT("added"), true);
    Obj->SetBoolField(TEXT("already_existed"), false);
    Obj->SetStringField(TEXT("message"), bSaved
        ? TEXT("Animation asset added and saved")
        : FString::Printf(TEXT("Animation asset added but save failed: %s"), *SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("add_animation_asset_to_pose_search_database"), SaveError);
}

// ---------------------------------------------------------------------------
// build_pose_search_database_index
// ---------------------------------------------------------------------------
// MCP:build_pose_search_database_index - Request an async build of the PoseSearch index for a database
//   @param database_path - Asset path of the UPoseSearchDatabase (required).
//   @param wait_for_completion - If true, block until the build completes (the engine's FTickableGameObject pumps during the wait).
//   @returns { database_path, schema_path, schema_cardinality, animation_asset_count, wait_for_completion, result, success, in_progress, failed, message }
TSharedPtr<FJsonObject> HandleBuildPoseSearchDatabaseIndex(const TSharedPtr<FJsonObject>& Params)
{
    FString DatabasePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("database_path"), DatabasePath)) { return Missing; }

    bool bWait = true;
    Params->TryGetBoolField(TEXT("wait_for_completion"), bWait);

    UPoseSearchDatabase* Database = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(DatabasePath, TEXT("UPoseSearchDatabase"), Database)) { return Error; }

    if (!Database->Schema)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("UPoseSearchDatabase '%s' has no Schema assigned"), *DatabasePath));
    }
    // SchemaCardinality is Transient (PoseSearchSchema.h) — a fresh load shows 0
    // even when the schema's Channels[] array is fully populated. Trigger
    // PostEditChange() to run the schema's PostEditChangeProperty override
    // (which calls the private Finalize() — PoseSearchSchema.cpp).
    // Database->Schema is `const TObjectPtr`, so we const_cast for the
    // non-const PostEditChange call.
    if (Database->Schema->SchemaCardinality <= 0)
    {
        const_cast<UPoseSearchSchema*>(Database->Schema.Get())->PostEditChange();
    }
    if (Database->Schema->SchemaCardinality <= 0)
    {
        // SchemaCardinality is only populated by Schema::Finalize(); if it's still 0
        // here, the schema's channel Finalize() reported an error or the skeleton's
        // root bone is missing. We refuse the build rather than spinning up an
        // async task that will fail at index time.
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("UPoseSearchSchema '%s' has no finalized channels (SchemaCardinality=0)"),
                *Database->Schema->GetPathName()));
    }

    using namespace UE::PoseSearch;

    // NewRequest kicks a fresh index build (cancels previous for this DB).
    // ContinueRequest ensures data is present without forcing a rebuild —
    // useful when the user just wants to ensure the DDC is warm. We always
    // pass NewRequest because build_pose_search_database_index is the
    // explicit "build now" verb; ContinueRequest is reserved for warm-on-demand.
    ERequestAsyncBuildFlag Flag = ERequestAsyncBuildFlag::NewRequest;
    if (bWait)
    {
        Flag |= ERequestAsyncBuildFlag::WaitForCompletion;
    }

    // When WaitForCompletion is set, this call drives the engine's
    // FTickableGameObject pumps inline (RequestAsyncBuildIndex spins the
    // task queue) — that's why the editor needs to be live. In the tests
    // they run on a fresh editor process so the pump is alive.
    const EAsyncBuildIndexResult Result = FAsyncPoseSearchDatabasesManagement::RequestAsyncBuildIndex(Database, Flag);

    const TCHAR* ResultStr = TEXT("Unknown");
    bool bSuccess = false;
    bool bInProgress = false;
    bool bFailed = false;
    switch (Result)
    {
    case EAsyncBuildIndexResult::Success:    ResultStr = TEXT("Success");    bSuccess    = true; break;
    case EAsyncBuildIndexResult::InProgress: ResultStr = TEXT("InProgress"); bInProgress = true; break;
    case EAsyncBuildIndexResult::Failed:     ResultStr = TEXT("Failed");     bFailed     = true; break;
    }

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("database_path"), DatabasePath);
    Obj->SetStringField(TEXT("schema_path"), Database->Schema->GetPathName());
    Obj->SetNumberField(TEXT("schema_cardinality"), Database->Schema->SchemaCardinality);
    Obj->SetNumberField(TEXT("animation_asset_count"), Database->GetNumAnimationAssets());
    Obj->SetBoolField(TEXT("wait_for_completion"), bWait);
    Obj->SetStringField(TEXT("result"), ResultStr);
    Obj->SetBoolField(TEXT("success"), bSuccess);
    Obj->SetBoolField(TEXT("in_progress"), bInProgress);
    Obj->SetBoolField(TEXT("failed"), bFailed);
    if (bSuccess)
    {
        Obj->SetStringField(TEXT("message"), TEXT("Index built successfully"));
    }
    else if (bInProgress)
    {
        Obj->SetStringField(TEXT("message"),
            TEXT("Index build still in progress (wait_for_completion was false)"));
    }
    else
    {
        Obj->SetStringField(TEXT("message"), TEXT("Index build failed (check LogPoseSearch)"));
    }
    // The verdict measured above is the verdict EMITTED.
    return mcp_common::MakeOutcomeReply(Obj, bSuccess,
        bSuccess ? FString() : mcp_common::MeasuredOutcomeError(TEXT("build_pose_search_database_index"), TEXT("bSuccess")));
}

// ---------------------------------------------------------------------------
// inspect_pose_search_database
// ---------------------------------------------------------------------------
// MCP:inspect_pose_search_database - Read-only walk of Schema, DatabaseAnimationAssets and Channels on a UPoseSearchDatabase
//   @param database_path - Asset path of the UPoseSearchDatabase (required).
//   @returns { database_path, schema_path, pose_search_mode, kd_tree_query_num_neighbors, number_of_principal_components, base_cost_bias, tags, schema_cardinality, animation_asset_count, animation_assets[], channel_count, channels[] }
TSharedPtr<FJsonObject> HandleInspectPoseSearchDatabase(const TSharedPtr<FJsonObject>& Params)
{
    FString DatabasePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("database_path"), DatabasePath)) { return Missing; }

    UPoseSearchDatabase* Database = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(DatabasePath, TEXT("UPoseSearchDatabase"), Database)) { return Error; }

    // EPoseSearchMode -> string. Values map to the UENUM declaration
    // (PoseSearchDatabase.h) so callers can decode them safely.
    const TCHAR* ModeStr = TEXT("Unknown");
    switch (Database->PoseSearchMode)
    {
    case EPoseSearchMode::BruteForce: ModeStr = TEXT("BruteForce"); break;
    case EPoseSearchMode::PCAKDTree:  ModeStr = TEXT("PCAKDTree"); break;
    case EPoseSearchMode::VPTree:     ModeStr = TEXT("VPTree"); break;
    case EPoseSearchMode::EventOnly:  ModeStr = TEXT("EventOnly"); break;
    }

    // Tags: TArray<FName>. Mirror them as a JSON string array.
    TArray<TSharedPtr<FJsonValue>> TagsJson;
    TagsJson.Reserve(Database->Tags.Num());
    for (const FName& Tag : Database->Tags)
    {
        TagsJson.Add(MakeShared<FJsonValueString>(Tag.ToString()));
    }

    // DatabaseAnimationAssets (private UPROPERTY) — walk via the public
    // GetNumAnimationAssets / GetAnimationAsset / GetDatabaseAnimationAsset
    // getters (PoseSearchDatabase.h). GetDatabaseAnimationAsset
    // returns a const FPoseSearchDatabaseAnimationAsset* that exposes the
    // base getters bEnabled / GetSamplingRange() / GetAnimationAsset().
    TArray<TSharedPtr<FJsonValue>> AnimAssetsJson;
    const int32 NumAnimAssets = Database->GetNumAnimationAssets();
    AnimAssetsJson.Reserve(NumAnimAssets);
    for (int32 Idx = 0; Idx < NumAnimAssets; ++Idx)
    {
        const FPoseSearchDatabaseAnimationAsset* Entry = Database->GetDatabaseAnimationAsset(Idx);
        if (!Entry)
        {
            continue;
        }
        TSharedPtr<FJsonObject> EntryObj = MakeShared<FJsonObject>();
        EntryObj->SetNumberField(TEXT("index"), Idx);
        EntryObj->SetBoolField(TEXT("b_enabled"), Entry->IsEnabled());
        const FFloatInterval Range = Entry->GetSamplingRange();
        TSharedPtr<FJsonObject> RangeObj = MakeShared<FJsonObject>();
        RangeObj->SetNumberField(TEXT("min"), Range.Min);
        RangeObj->SetNumberField(TEXT("max"), Range.Max);
        EntryObj->SetObjectField(TEXT("sampling_range"), RangeObj);
        UObject* AnimAsset = Entry->GetAnimationAsset();
        EntryObj->SetStringField(TEXT("animation_asset_path"),
            AnimAsset ? AnimAsset->GetPathName() : FString());
        AnimAssetsJson.Add(MakeShared<FJsonValueObject>(EntryObj));
    }

    // Schema-side channel walk. Database->Schema may be null — we still
    // surface a clean payload with channel_count=0 in that case (honest).
    TArray<TSharedPtr<FJsonValue>> ChannelsJson;
    int32 SchemaCardinality = 0;
    if (Database->Schema)
    {
        SchemaCardinality = Database->Schema->SchemaCardinality;
        ChannelsJson = PoseSearch_ChannelsJson(Database->Schema);
    }

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("database_path"), DatabasePath);
    Obj->SetStringField(TEXT("schema_path"),
        Database->Schema ? Database->Schema->GetPathName() : FString());
    Obj->SetStringField(TEXT("pose_search_mode"), ModeStr);
    Obj->SetNumberField(TEXT("kd_tree_query_num_neighbors"), Database->KDTreeQueryNumNeighbors);
    Obj->SetNumberField(TEXT("number_of_principal_components"), Database->NumberOfPrincipalComponents);
    Obj->SetNumberField(TEXT("base_cost_bias"), Database->BaseCostBias);
    Obj->SetArrayField(TEXT("tags"), TagsJson);
    Obj->SetNumberField(TEXT("schema_cardinality"), SchemaCardinality);
    Obj->SetNumberField(TEXT("animation_asset_count"), NumAnimAssets);
    Obj->SetArrayField(TEXT("animation_assets"), AnimAssetsJson);
    Obj->SetNumberField(TEXT("channel_count"), ChannelsJson.Num());
    Obj->SetArrayField(TEXT("channels"), ChannelsJson);
    return mcp_common::MakeOkReply(Obj);
}

// ---------------------------------------------------------------------------
// Typed delete inverses for schema/database/anim.
// ---------------------------------------------------------------------------

// MCP:remove_pose_search_schema - Destroy a UPoseSearchSchema asset (mirror of create_pose_search_schema). Typed-delete: it loads the asset and REFUSES to delete it if it is not a UPoseSearchSchema, so it cannot wipe the wrong asset the way the generic delete_asset can. Idempotent on missing assets.
//   @param schema_path - Full /Game/... asset path of the schema.
//   @returns { schema_path, deleted, already_existed, asset_class?, note? }
//   NOTE (impl): delegates the actual delete to UEditorAssetLibrary::DeleteAsset once the type check passes.

TSharedPtr<FJsonObject> HandleRemovePoseSearchSchema(const TSharedPtr<FJsonObject>& Params)
{
    FString SchemaPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("schema_path"), SchemaPath)) { return Missing; }
    TSharedPtr<FJsonObject> Err;
    TSharedPtr<FJsonObject> Resp = PoseSearch_DeleteTypedAsset<UPoseSearchSchema>(
        SchemaPath, TEXT("schema_path"), TEXT("remove_pose_search_schema"), TEXT("UPoseSearchSchema"), Err);
    if (!Resp) { return Err; }
    return mcp_common::MakeOkReply(Resp);
}

// MCP:remove_pose_search_database - Destroy a UPoseSearchDatabase asset (mirror of create_pose_search_database). Typed-delete: it loads the asset and REFUSES to delete it if it is not a UPoseSearchDatabase, so it cannot wipe the wrong asset the way the generic delete_asset can. Idempotent on missing assets.
//   @param database_path - Full /Game/... asset path of the database.
//   @returns { database_path, deleted, already_existed, asset_class?, note? }
//   NOTE (impl): delegates the actual delete to UEditorAssetLibrary::DeleteAsset once the type check passes.

TSharedPtr<FJsonObject> HandleRemovePoseSearchDatabase(const TSharedPtr<FJsonObject>& Params)
{
    FString DatabasePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("database_path"), DatabasePath)) { return Missing; }
    TSharedPtr<FJsonObject> Err;
    TSharedPtr<FJsonObject> Resp = PoseSearch_DeleteTypedAsset<UPoseSearchDatabase>(
        DatabasePath, TEXT("database_path"), TEXT("remove_pose_search_database"), TEXT("UPoseSearchDatabase"), Err);
    if (!Resp) { return Err; }
    return mcp_common::MakeOkReply(Resp);
}

// MCP:remove_animation_asset_from_pose_search_database - Remove one FPoseSearchDatabaseAnimationAsset entry from a UPoseSearchDatabase by 0-based index (inverse of add_animation_asset_to_pose_search_database). Idempotent: an out-of-range index returns removed:false + note, not an error. The database index is NOT rebuilt here -- run build_pose_search_database_index afterwards.
//   @param database_path - Full /Game/... asset path of the database.
//   @param animation_asset_index - 0-based index into DatabaseAnimationAssets[].
//   @returns { database_path, animation_asset_index, removed, previous_count, new_count }
//   NOTE (impl): wraps UPoseSearchDatabase::RemoveAnimationAssetAt(index).

TSharedPtr<FJsonObject> HandleRemoveAnimationAssetFromPoseSearchDatabase(const TSharedPtr<FJsonObject>& Params)
{
    FString DatabasePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("database_path"), DatabasePath)) { return Missing; }
    int32 AnimAssetIndex = INDEX_NONE;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("animation_asset_index"), AnimAssetIndex)) { return Missing; }
    UPoseSearchDatabase* Database = LoadObject<UPoseSearchDatabase>(nullptr, *DatabasePath);
    if (!Database)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("remove_animation_asset_from_pose_search_database: could not load UPoseSearchDatabase at %s"), *DatabasePath));
    }
    const int32 PrevCount = Database->GetNumAnimationAssets();
    if (AnimAssetIndex < 0 || AnimAssetIndex >= PrevCount)
    {
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetStringField(TEXT("database_path"), Database->GetPathName());
        Resp->SetNumberField(TEXT("animation_asset_index"), AnimAssetIndex);
        Resp->SetBoolField(TEXT("removed"), false);
        Resp->SetNumberField(TEXT("previous_count"), PrevCount);
        Resp->SetNumberField(TEXT("new_count"), PrevCount);
        Resp->SetStringField(TEXT("note"),
            FString::Printf(TEXT("animation_asset_index %d out of range [0, %d)"), AnimAssetIndex, PrevCount));
        return mcp_common::MakeOkReply(Resp);
    }
    Database->Modify();
    Database->RemoveAnimationAssetAt(AnimAssetIndex);
    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Database->GetPackage(), Database, SaveError);
    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("database_path"), Database->GetPathName());
    Resp->SetNumberField(TEXT("animation_asset_index"), AnimAssetIndex);
    Resp->SetBoolField(TEXT("removed"), true);
    Resp->SetNumberField(TEXT("previous_count"), PrevCount);
    Resp->SetNumberField(TEXT("new_count"), Database->GetNumAnimationAssets());
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("remove_animation_asset_from_pose_search_database"), SaveError);
}

// ---------------------------------------------------------------------------
// Read side of create_pose_search_schema + db<->schema bind.
// ---------------------------------------------------------------------------

// MCP:inspect_pose_search_schema - Read-only walk of a UPoseSearchSchema's public fields + FinalizedChannels. An empty finalized_channels / is_finalized:false is NOT a bug in this tool: the schema only gets finalized by AddDefaultChannels() or by a PostEditChange() on it, so a freshly authored schema reports none until something triggers that.
//   @param schema_path - Asset path of the UPoseSearchSchema (required).
//   @returns { schema_path, sample_rate, schema_cardinality, number_of_permutations, permutations_sample_rate, permutations_time_offset, add_data_padding: bool, inject_additional_debug_channels: bool, draw_inject_additional_debug_channels: bool, roled_skeletons: [{ role_name, skeleton_path, mirror_table_path }], roled_skeleton_count, is_finalized: bool, channel_count, finalized_channels: [{ class, channel_name, weight }], note?: string } -- note only when is_finalized is false.
//   NOTE (impl): Finalize() itself is private — external callers must go through PostEditChange (PoseSearchSchema.cpp).
TSharedPtr<FJsonObject> HandleInspectPoseSearchSchema(const TSharedPtr<FJsonObject>& Params)
{
    FString SchemaPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("schema_path"), SchemaPath)) { return Missing; }

    UPoseSearchSchema* Schema = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(SchemaPath, TEXT("UPoseSearchSchema"), Schema)) { return Error; }

    // Roled skeletons (read-side mirror of the FPoseSearchRoledSkeleton list).
    // FPoseSearchRoledSkeleton fields are public: TObjectPtr<USkeleton> Skeleton,
    // TObjectPtr<UMirrorDataTable> MirrorDataTable, FName Role. The pointer-truthy
    // check uses operator bool on TObjectPtr (true iff the pointer is non-null).
    TArray<TSharedPtr<FJsonValue>> RoledSkeletonsJson;
    for (const FPoseSearchRoledSkeleton& RoledSkeleton : Schema->GetRoledSkeletons())
    {
        TSharedPtr<FJsonObject> RsObj = MakeShared<FJsonObject>();
        RsObj->SetStringField(TEXT("role_name"),
            RoledSkeleton.Role.IsNone() ? FString() : RoledSkeleton.Role.ToString());
        RsObj->SetStringField(TEXT("skeleton_path"),
            RoledSkeleton.Skeleton ? RoledSkeleton.Skeleton->GetPathName() : FString());
        RsObj->SetStringField(TEXT("mirror_table_path"),
            RoledSkeleton.MirrorDataTable ? RoledSkeleton.MirrorDataTable->GetPathName() : FString());
        RoledSkeletonsJson.Add(MakeShared<FJsonValueObject>(RsObj));
    }

    // Finalized channels (empty until Finalize() runs via AddDefaultChannels or PostEditChange).
    const TArray<TSharedPtr<FJsonValue>> ChannelsJson = PoseSearch_ChannelsJson(Schema);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("schema_path"), SchemaPath);
    Obj->SetNumberField(TEXT("sample_rate"), Schema->SampleRate);
    Obj->SetNumberField(TEXT("schema_cardinality"), Schema->SchemaCardinality);
    Obj->SetNumberField(TEXT("number_of_permutations"), Schema->NumberOfPermutations);
    Obj->SetNumberField(TEXT("permutations_sample_rate"), Schema->PermutationsSampleRate);
    Obj->SetNumberField(TEXT("permutations_time_offset"), Schema->PermutationsTimeOffset);
    Obj->SetBoolField(TEXT("add_data_padding"), Schema->bAddDataPadding);
    Obj->SetBoolField(TEXT("inject_additional_debug_channels"), Schema->bInjectAdditionalDebugChannels);
    Obj->SetBoolField(TEXT("draw_inject_additional_debug_channels"), Schema->bDrawInjectAdditionalDebugChannels);
    Obj->SetArrayField(TEXT("roled_skeletons"), RoledSkeletonsJson);
    Obj->SetNumberField(TEXT("roled_skeleton_count"), RoledSkeletonsJson.Num());
    Obj->SetBoolField(TEXT("is_finalized"), ChannelsJson.Num() > 0 || Schema->SchemaCardinality > 0);
    Obj->SetNumberField(TEXT("channel_count"), ChannelsJson.Num());
    Obj->SetArrayField(TEXT("finalized_channels"), ChannelsJson);
    if (!Obj->GetBoolField(TEXT("is_finalized")))
    {
        Obj->SetStringField(TEXT("note"),
            TEXT("FinalizedChannels is empty; schema has not been Finalized yet (call add_pose_search_channel_to_schema or PostEditChange)."));
    }
    return mcp_common::MakeOkReply(Obj);
}

// MCP:set_pose_search_database_schema - Bind a UPoseSearchSchema to a UPoseSearchDatabase (assigns the Schema field). Use it right after creating a database with no schema, or to rebind an existing one; a database without a schema cannot be indexed.
//   @param database_path - Asset path of the UPoseSearchDatabase (required).
//   @param schema_path - Asset path of the UPoseSearchSchema to bind (required).
//   @returns { database_path, previous_schema_path, new_schema_path, changed: bool, saved: bool }
//   NOTE (impl): the Schema UPROPERTY on UPoseSearchDatabase is public (PoseSearchDatabase.h, TObjectPtr<const UPoseSearchSchema>), so a direct assignment compiles.
TSharedPtr<FJsonObject> HandleSetPoseSearchDatabaseSchema(const TSharedPtr<FJsonObject>& Params)
{
    FString DatabasePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("database_path"), DatabasePath)) { return Missing; }
    FString SchemaPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("schema_path"), SchemaPath)) { return Missing; }

    UPoseSearchDatabase* Database = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(DatabasePath, TEXT("UPoseSearchDatabase"), Database)) { return Error; }
    UPoseSearchSchema* Schema = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(SchemaPath, TEXT("UPoseSearchSchema"), Schema)) { return Error; }

    const FString PreviousSchemaPath = Database->Schema ? Database->Schema->GetPathName() : FString();
    const bool bChanged = PreviousSchemaPath != Schema->GetPathName();

    Database->Modify();
    Database->Schema = Schema;
    Database->MarkPackageDirty();

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Database->GetPackage(), Database, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("database_path"), DatabasePath);
    Obj->SetStringField(TEXT("previous_schema_path"), PreviousSchemaPath);
    Obj->SetStringField(TEXT("new_schema_path"), Schema->GetPathName());
    Obj->SetBoolField(TEXT("changed"), bChanged);
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("set_pose_search_database_schema"), SaveError);
}
// ---------------------------------------------------------------------------
// One reflective setter over the scalar UPROPERTY fields of UPoseSearchSchema:
// each is a flat UPROPERTY that JsonToStruct writes by reflection, so one
// partial-merge handler (model: HandleSetSoundWaveProperties) covers them all.
// The object-ref and keyed fields (skeleton / mirror_data_table / role) cannot
// go through the flat merge and have dedicated handlers below.
// ---------------------------------------------------------------------------

namespace
{
    // Loads the UPoseSearchSchema at the required "asset_path" into OutSchema: null when it loads, else the error
    // reply. The reflective setter and the object-ref / role setters all address the schema this way.
    TSharedPtr<FJsonObject> PoseSearch_ReadSchema(const TSharedPtr<FJsonObject>& Params, UPoseSearchSchema*& OutSchema)
    {
        FString SchemaPath;
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_path"), SchemaPath)) { return Missing; }
        return AnimAssetUtils::LoadOrError(SchemaPath, TEXT("UPoseSearchSchema"), OutSchema);
    }
}

// MCP:set_pose_search_schema_properties - Author UPoseSearchSchema scalar tunables via reflection (partial merge + clamp). Replaces the 8 former per-field set_pose_search_schema_* setters.
//   @param asset_path - Asset path of the target UPoseSearchSchema (required).
//   @param properties - Object of UPoseSearchSchema UPROPERTY fields to merge; keys are the C++ property NAMES (case-insensitive), absent keys untouched. Covers: SampleRate(int, ClampMin=1 ClampMax=240), bAddDataPadding(bool), bInjectAdditionalDebugChannels(bool), bDrawInjectAdditionalDebugChannels(bool), NumberOfPermutations(int, ClampMin=1), PermutationsSampleRate(int, ClampMin=1 ClampMax=240), PermutationsTimeOffset(float), DataPreprocessor(enum name: None/Normalize/NormalizeOnlyByDeviation/NormalizeWithCommonSchema). Object-refs (skeleton, mirror_data_table) and the role tag are set via their dedicated commands, not here.
//   @returns { asset_path, asset_class, settings, applied_keys, changed, saved }
TSharedPtr<FJsonObject> HandleSetPoseSearchSchemaProperties(const TSharedPtr<FJsonObject>& Params)
{
    UPoseSearchSchema* Schema = nullptr;
    if (TSharedPtr<FJsonObject> Error = PoseSearch_ReadSchema(Params, Schema)) { return Error; }

    const TSharedPtr<FJsonObject>* PropertiesJsonPtr = nullptr;
    if (!Params->TryGetObjectField(TEXT("properties"), PropertiesJsonPtr) || !PropertiesJsonPtr || !PropertiesJsonPtr->IsValid())
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: properties (object)"));
    }
    const TSharedPtr<FJsonObject>& PropertiesJson = *PropertiesJsonPtr;

    Schema->Modify();

    // Partial merge directly into the UPoseSearchSchema object: only the keys
    // present in PropertiesJson are written; every other property is untouched.
    // The 8 covered fields are all plain scalar/bool/enum UPROPERTYs (NOT
    // uint8:1 bitfields), so they round-trip through JsonToStruct cleanly, and
    // the DataPreprocessor enum decodes by name. Keys match the C++ property
    // names case-insensitively (FJsonObjectConverter::GetAuthoredNameForField).
    FString JsonErr;
    if (!mcp_common::JsonToStruct(
            PropertiesJson,
            UPoseSearchSchema::StaticClass(),
            Schema, JsonErr, 0, 0))
    {
        return mcp_common::MakeErrorReply(JsonErr);
    }

    // JsonToStruct ignores ClampMin/ClampMax meta, so re-apply it generically:
    // clamp SampleRate(1..240), NumberOfPermutations(>=1), PermutationsSampleRate(1..240).
    mcp_common::ClampNumericByMeta(UPoseSearchSchema::StaticClass(), Schema);

    // PostEditChange runs the schema's WITH_EDITOR PostEditChangeProperty
    // override (PoseSearchSchema.cpp), which calls the private Finalize().
    // Required after changing bAddDataPadding / DataPreprocessor / permutations
    // (the same external-Finalize path the create/add-channel handlers use).
    Schema->PostEditChange();

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Schema->GetPackage(), Schema, SaveError);

    // Echo the persisted properties back via reflection, plus the applied keys.
    TSharedPtr<FJsonObject> SettingsOut = mcp_common::StructToJson(
        UPoseSearchSchema::StaticClass(), Schema, 0, 0);

    TArray<TSharedPtr<FJsonValue>> AppliedKeys;
    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : PropertiesJson->Values)
    {
        AppliedKeys.Add(MakeShared<FJsonValueString>(Pair.Key));
    }

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("asset_path"), Schema->GetPathName());
    Obj->SetStringField(TEXT("asset_class"), Schema->GetClass()->GetName());
    Obj->SetObjectField(TEXT("settings"), SettingsOut);
    Obj->SetArrayField(TEXT("applied_keys"), AppliedKeys);
    Obj->SetBoolField(TEXT("changed"), AppliedKeys.Num() > 0);
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("set_pose_search_schema_properties"), SaveError);
}

// MCP:set_pose_search_schema_skeleton - Bind (or rebind) the USkeleton of the schema's FPoseSearchRoledSkeleton entry under `role`. Idempotent by role: if a skeleton is already bound under that role it is REPLACED; if no roled entry exists for that role yet, one is created. Use add_pose_search_roled_skeleton instead when you want to ADD a second character's role without disturbing the first.
//   @param asset_path - Asset path of the UPoseSearchSchema to edit (required).
//   @param skeleton_path - Asset path of the USkeleton to bind under the given role (required).
//   @param role - Role name (FName) identifying the FPoseSearchRoledSkeleton entry within a multi-character schema (optional; empty = default role).
//   @returns { asset_path, role, previous_skeleton_path, new_skeleton_path, changed, saved }
TSharedPtr<FJsonObject> HandleSetPoseSearchSchemaSkeleton(const TSharedPtr<FJsonObject>& Params)
{
    UPoseSearchSchema* Schema = nullptr;
    if (TSharedPtr<FJsonObject> Error = PoseSearch_ReadSchema(Params, Schema)) { return Error; }

    FString SkeletonPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("skeleton_path"), SkeletonPath)) { return Missing; }
    USkeleton* Skeleton = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(SkeletonPath, TEXT("USkeleton"), Skeleton)) { return Error; }

    FString RoleStr;
    Params->TryGetStringField(TEXT("role"), RoleStr);
    const FName Role(*RoleStr); // empty -> default role via AddSkeleton's default arg

    const FString PreviousSkeletonPath = Schema->GetSkeleton(Role) ? Schema->GetSkeleton(Role)->GetPathName() : FString();

    Schema->Modify();
    // AddSkeleton always APPENDS an entry (PoseSearchSchema.cpp), so the role's entry is edited in place.
    if (FPoseSearchRoledSkeleton* Entry = Schema->GetRoledSkeleton(Role))
    {
        Entry->Skeleton = Skeleton;
    }
    else
    {
        Schema->AddSkeleton(Skeleton, nullptr, Role);
    }
    Schema->PostEditChange();
    Schema->MarkPackageDirty();

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Schema->GetPackage(), Schema, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("asset_path"), Schema->GetPathName());
    Obj->SetStringField(TEXT("role"), RoleStr);
    Obj->SetStringField(TEXT("previous_skeleton_path"), PreviousSkeletonPath);
    Obj->SetStringField(TEXT("new_skeleton_path"), Skeleton->GetPathName());
    Obj->SetBoolField(TEXT("changed"), PreviousSkeletonPath != Skeleton->GetPathName());
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("set_pose_search_schema_skeleton"), SaveError);
}

// MCP:set_pose_search_schema_mirror_data_table - Bind (or rebind) the UMirrorDataTable of the schema's FPoseSearchRoledSkeleton entry under `role` (default role when omitted). Refused when no entry has that role.
//   @param asset_path - Asset path of the UPoseSearchSchema to edit (required).
//   @param mirror_data_table_path - Asset path of the UMirrorDataTable to bind under the given role (required).
//   @param role - Role name (FName) identifying the FPoseSearchRoledSkeleton entry within a multi-character schema (optional; empty = default role).
//   @returns { asset_path, role, previous_mirror_data_table_path, new_mirror_data_table_path, changed, saved }
TSharedPtr<FJsonObject> HandleSetPoseSearchSchemaMirrorDataTable(const TSharedPtr<FJsonObject>& Params)
{
    UPoseSearchSchema* Schema = nullptr;
    if (TSharedPtr<FJsonObject> Error = PoseSearch_ReadSchema(Params, Schema)) { return Error; }

    FString MirrorTablePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("mirror_data_table_path"), MirrorTablePath)) { return Missing; }

    FString RoleStr;
    Params->TryGetStringField(TEXT("role"), RoleStr);
    const FName Role(*RoleStr);
    FPoseSearchRoledSkeleton* Entry = Schema->GetRoledSkeleton(Role);
    if (!Entry)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("No roled skeleton under role '%s': bind one first (set_pose_search_schema_skeleton or add_pose_search_roled_skeleton)"),
            *RoleStr));
    }

    UMirrorDataTable* MirrorTable = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(MirrorTablePath, TEXT("UMirrorDataTable"), MirrorTable)) { return Error; }

    const FString PreviousPath = Entry->MirrorDataTable ? Entry->MirrorDataTable->GetPathName() : FString();

    // Edited in place: AddSkeleton always APPENDS an entry.
    Schema->Modify();
    Entry->MirrorDataTable = MirrorTable;
    Schema->PostEditChange();
    Schema->MarkPackageDirty();

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Schema->GetPackage(), Schema, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("asset_path"), Schema->GetPathName());
    Obj->SetStringField(TEXT("role"), RoleStr);
    Obj->SetStringField(TEXT("previous_mirror_data_table_path"), PreviousPath);
    Obj->SetStringField(TEXT("new_mirror_data_table_path"), MirrorTable->GetPathName());
    Obj->SetBoolField(TEXT("changed"), PreviousPath != MirrorTable->GetPathName());
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("set_pose_search_schema_mirror_data_table"), SaveError);
}

// MCP:set_pose_search_schema_role - Rename the role (FName) of the schema's FPoseSearchRoledSkeleton entry under role_from (default role when omitted). Refused when new_role is already taken by another entry.
//   @param asset_path - Asset path of the UPoseSearchSchema to edit (required).
//   @param new_role - New role name (FName) to assign to the entry, identifying it within a multi-character schema (required).
//   @param role_from - Existing role name (FName) of the FPoseSearchRoledSkeleton entry to rename (optional; empty = default role).
//   @returns { asset_path, previous_role, new_role, changed, saved }
TSharedPtr<FJsonObject> HandleSetPoseSearchSchemaRole(const TSharedPtr<FJsonObject>& Params)
{
    UPoseSearchSchema* Schema = nullptr;
    if (TSharedPtr<FJsonObject> Error = PoseSearch_ReadSchema(Params, Schema)) { return Error; }

    FString NewRoleStr;
    if (!Params->TryGetStringField(TEXT("new_role"), NewRoleStr) || NewRoleStr.IsEmpty())
    {
        return mcp_common::MakeErrorReply(
            TEXT("Missing required parameter: new_role (FName)"));
    }
    const FName NewRole(*NewRoleStr);

    FString RoleFromStr;
    Params->TryGetStringField(TEXT("role_from"), RoleFromStr);
    const FName RoleFrom(*RoleFromStr);

    FPoseSearchRoledSkeleton* Existing = Schema->GetRoledSkeleton(RoleFrom);
    if (!Existing)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("No FPoseSearchRoledSkeleton under role '%s' to rename"), *RoleFromStr));
    }
    if (RoleFrom != NewRole && Schema->GetRoledSkeleton(NewRole))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Role '%s' is already taken by another roled skeleton; remove it first (remove_pose_search_roled_skeleton)"),
            *NewRoleStr));
    }

    // Renamed in place: AddSkeleton always APPENDS an entry.
    Schema->Modify();
    Existing->Role = NewRole;
    Schema->PostEditChange();
    Schema->MarkPackageDirty();

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Schema->GetPackage(), Schema, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("asset_path"), Schema->GetPathName());
    Obj->SetStringField(TEXT("previous_role"), RoleFromStr);
    Obj->SetStringField(TEXT("new_role"), NewRoleStr);
    Obj->SetBoolField(TEXT("changed"), RoleFrom != NewRole);
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("set_pose_search_schema_role"), SaveError);
}

// ---------------------------------------------------------------------------
// Roled-skeleton list CRUD (multi-character schemas).
//
// A UPoseSearchSchema owns TArray<FPoseSearchRoledSkeleton> Skeletons (PRIVATE
// UPROPERTY). One entry per animation Role; single-character schemas have the
// lone default-role entry. add/remove below are the append/erase inverses on
// that list — the roled-skeleton peers of add/remove_pose_search_channel_from_schema.
//
//   FPoseSearchRoledSkeleton { TObjectPtr<USkeleton> Skeleton;
//       TObjectPtr<UMirrorDataTable> MirrorDataTable; FName Role; } (public fields).
//   Public read getters: GetRoledSkeleton(FName) -> const entry* (null if absent),
//   GetRoledSkeletons() -> const TArray&. Append: AddSkeleton(USkeleton*,
//   UMirrorDataTable* = nullptr, const FName& Role) (POSESEARCH_API).
//
// GOTCHA (AddSkeleton does NOT dedupe): it is a pure append (UE 5.8 source:
//   PoseSearchSchema.cpp::AddSkeleton emplaces unconditionally), so add guards on
//   GetRoledSkeleton(role) first and no-ops when the role is already present —
//   otherwise a second call would push a duplicate role entry.
// GOTCHA (no public RemoveSkeleton): remove resizes the private Skeletons array
//   by reflection (FArrayProperty + FScriptArrayHelper::RemoveValues), the same
//   private-array-resize idiom as remove_pose_search_channel_from_schema.
// Both re-Finalize via PostEditChange (schema cardinality/derived data depend on
//   the roled-skeleton set) and are WITH_EDITOR-gated (Finalize is editor-only).
// ---------------------------------------------------------------------------

// MCP:add_pose_search_roled_skeleton - Append a roled FPoseSearchRoledSkeleton (Skeleton + optional MirrorDataTable under a Role) to a UPoseSearchSchema for multi-character search. Idempotent on role: if an entry already exists under that role it no-ops (added=false, already_existed=true) instead of pushing a duplicate -- use set_pose_search_schema_skeleton if you actually want to REPLACE the skeleton bound to a role.
//   @param asset_path - Asset path of the target UPoseSearchSchema (required).
//   @param skeleton_path - Asset path of the USkeleton to bind under the role (required).
//   @param role - Role name (FName) identifying the entry within a multi-character schema (required; empty = default/None role).
//   @param mirror_data_table_path - Asset path of a UMirrorDataTable to bind under the role (optional; empty = none).
//   @returns { asset_path, role, skeleton_path, mirror_data_table_path, roled_skeleton_count, added, already_existed, saved }
//   NOTE (impl): the guard exists because UPoseSearchSchema::AddSkeleton does NOT dedupe -- it is a pure append.
TSharedPtr<FJsonObject> HandleAddPoseSearchRoledSkeleton(const TSharedPtr<FJsonObject>& Params)
{
    UPoseSearchSchema* Schema = nullptr;
    if (TSharedPtr<FJsonObject> Error = PoseSearch_ReadSchema(Params, Schema)) { return Error; }

    FString SkeletonPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("skeleton_path"), SkeletonPath)) { return Missing; }

    // role is required by contract but "" is a legal value (the default/None role).
    FString RoleStr;
    if (!Params->TryGetStringField(TEXT("role"), RoleStr))
    {
        return mcp_common::MakeErrorReply(
            TEXT("Missing required parameter: role (FName; empty string = default/None role)"));
    }
    const FName Role(*RoleStr);

    FString MirrorTablePath;
    Params->TryGetStringField(TEXT("mirror_data_table_path"), MirrorTablePath);

    // Guard: AddSkeleton is a pure append, so bail out (no write) when the role
    // already has an entry — re-running must not push a duplicate roled skeleton.
    if (Schema->GetRoledSkeleton(Role))
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("asset_path"), Schema->GetPathName());
        Obj->SetStringField(TEXT("role"), RoleStr);
        Obj->SetStringField(TEXT("skeleton_path"), SkeletonPath);
        Obj->SetStringField(TEXT("mirror_data_table_path"), MirrorTablePath);
        Obj->SetNumberField(TEXT("roled_skeleton_count"), Schema->GetRoledSkeletons().Num());
        Obj->SetBoolField(TEXT("added"), false);
        Obj->SetBoolField(TEXT("already_existed"), true);
        Obj->SetBoolField(TEXT("saved"), true);
        return mcp_common::MakeOkReply(Obj);
    }

    USkeleton* Skeleton = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(SkeletonPath, TEXT("USkeleton"), Skeleton)) { return Error; }

    // Optional mirror table: empty path => nullptr (AddSkeleton's default arg).
    // A non-empty path that fails to resolve is an honest error, not a silent skip.
    UMirrorDataTable* MirrorTable = nullptr;
    if (!MirrorTablePath.IsEmpty())
    {
        if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(MirrorTablePath, TEXT("UMirrorDataTable"), MirrorTable)) { return Error; }
    }

    Schema->Modify();
    Schema->AddSkeleton(Skeleton, MirrorTable, Role);

    // PostEditChange runs the schema's WITH_EDITOR PostEditChangeProperty override
    // (PoseSearchSchema.cpp -> private Finalize()), re-Finalizing against the
    // now-larger roled-skeleton set. Same canonical external-Finalize path the
    // channel/setter handlers use.
    Schema->PostEditChange();

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Schema->GetPackage(), Schema, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("asset_path"), Schema->GetPathName());
    Obj->SetStringField(TEXT("role"), RoleStr);
    Obj->SetStringField(TEXT("skeleton_path"), Skeleton->GetPathName());
    Obj->SetStringField(TEXT("mirror_data_table_path"), MirrorTable ? MirrorTable->GetPathName() : FString());
    Obj->SetNumberField(TEXT("roled_skeleton_count"), Schema->GetRoledSkeletons().Num());
    Obj->SetBoolField(TEXT("added"), true);
    Obj->SetBoolField(TEXT("already_existed"), false);
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("add_pose_search_roled_skeleton"), SaveError);
}

// MCP:remove_pose_search_roled_skeleton - Remove a roled FPoseSearchRoledSkeleton entry from a UPoseSearchSchema by role or by index (inverse of add_pose_search_roled_skeleton). Idempotent: no match = no write, reported as already_absent. The schema is re-finalized after the removal, so finalized_channels changes with it.
//   @param asset_path - Asset path of the target UPoseSearchSchema (required).
//   @param role - Role name (FName) of the FPoseSearchRoledSkeleton entry to remove (provide this OR skeleton_index; empty = default/None role).
//   @param skeleton_index - 0-based index into the roled-skeleton list of the entry to remove (provide this OR role).
//   @returns { asset_path, role, skeleton_index, removed_skeleton_path, roled_skeleton_count, previous_count, removed, already_absent, saved }
//   NOTE (impl): UPoseSearchSchema exposes no public RemoveSkeleton, so the private Skeletons TArray is resized by
//   reflection (FScriptArrayHelper::RemoveValues) — the same idiom as remove_pose_search_channel_from_schema —
//   then re-Finalized via PostEditChange.
TSharedPtr<FJsonObject> HandleRemovePoseSearchRoledSkeleton(const TSharedPtr<FJsonObject>& Params)
{
    UPoseSearchSchema* Schema = nullptr;
    if (TSharedPtr<FJsonObject> Error = PoseSearch_ReadSchema(Params, Schema)) { return Error; }

    // Selector: exactly one of role / skeleton_index. Empty-string role is legal
    // (the default/None role), so presence — not emptiness — decides role mode.
    FString RoleStr;
    const bool bHasRole = Params->TryGetStringField(TEXT("role"), RoleStr);
    int32 SkeletonIndex = INDEX_NONE;
    const bool bHasIndex = Params->TryGetNumberField(TEXT("skeleton_index"), SkeletonIndex);
    if (bHasRole == bHasIndex)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Provide exactly one of role (FName) or skeleton_index (0-based)"));
    }

    // The source-of-truth Skeletons array (TArray<FPoseSearchRoledSkeleton>) is a
    // PRIVATE UPROPERTY (PoseSearchSchema.h) fed by AddSkeleton; there is no public
    // RemoveSkeleton, so we resize it by reflection via FScriptArrayHelper — same
    // idiom as remove_pose_search_channel_from_schema. RemoveValues destructs the
    // erased struct element (its TObjectPtr members) through the property reflection.
    FArrayProperty* SkeletonsProp = FindFProperty<FArrayProperty>(
        UPoseSearchSchema::StaticClass(), TEXT("Skeletons"));
    if (!SkeletonsProp)
    {
        return mcp_common::MakeErrorReply(
            TEXT("UPoseSearchSchema::Skeletons property not found (engine layout changed)"));
    }
    FScriptArrayHelper Helper(SkeletonsProp, SkeletonsProp->ContainerPtrToValuePtr<void>(Schema));
    const int32 PreviousCount = Helper.Num();

    // Resolve the concrete index to remove. Index mode: use it directly (range
    // checked below). Role mode: scan GetRoledSkeletons() for the matching Role.
    int32 RemoveIndex = INDEX_NONE;
    const FName Role(*RoleStr);
    if (bHasIndex)
    {
        if (SkeletonIndex >= 0 && SkeletonIndex < PreviousCount)
        {
            RemoveIndex = SkeletonIndex;
        }
    }
    else
    {
        const TArray<FPoseSearchRoledSkeleton>& RoledSkeletons = Schema->GetRoledSkeletons();
        for (int32 Idx = 0; Idx < RoledSkeletons.Num(); ++Idx)
        {
            if (RoledSkeletons[Idx].Role == Role)
            {
                RemoveIndex = Idx;
                break;
            }
        }
    }

    // Idempotent: no matching entry (out-of-range index, or no entry under the
    // requested role) => removed=false + already_absent=true, no write/save.
    if (RemoveIndex == INDEX_NONE)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("asset_path"), Schema->GetPathName());
        Obj->SetStringField(TEXT("role"), bHasRole ? RoleStr : FString());
        Obj->SetNumberField(TEXT("skeleton_index"), bHasIndex ? SkeletonIndex : -1);
        Obj->SetStringField(TEXT("removed_skeleton_path"), FString());
        Obj->SetNumberField(TEXT("roled_skeleton_count"), PreviousCount);
        Obj->SetNumberField(TEXT("previous_count"), PreviousCount);
        Obj->SetBoolField(TEXT("removed"), false);
        Obj->SetBoolField(TEXT("already_absent"), true);
        Obj->SetBoolField(TEXT("saved"), true);
        return mcp_common::MakeOkReply(Obj);
    }

    // Capture the removed skeleton's path (via the public getter) before erasing.
    const TArray<FPoseSearchRoledSkeleton>& RoledSkeletonsView = Schema->GetRoledSkeletons();
    const FString RemovedSkeletonPath =
        RoledSkeletonsView[RemoveIndex].Skeleton
            ? RoledSkeletonsView[RemoveIndex].Skeleton->GetPathName()
            : FString();

    Schema->Modify();

    // Resize Skeletons via reflection (destructs the erased entry's TObjectPtrs).
    Helper.RemoveValues(RemoveIndex, 1);

    // PostEditChange -> WITH_EDITOR Finalize() rebuilds derived data against the
    // now-shorter roled-skeleton set (same external-Finalize path as the peers).
    Schema->PostEditChange();

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Schema->GetPackage(), Schema, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("asset_path"), Schema->GetPathName());
    Obj->SetStringField(TEXT("role"), bHasRole ? RoleStr : FString());
    Obj->SetNumberField(TEXT("skeleton_index"), RemoveIndex);
    Obj->SetStringField(TEXT("removed_skeleton_path"), RemovedSkeletonPath);
    Obj->SetNumberField(TEXT("roled_skeleton_count"), Schema->GetRoledSkeletons().Num());
    Obj->SetNumberField(TEXT("previous_count"), PreviousCount);
    Obj->SetBoolField(TEXT("removed"), true);
    Obj->SetBoolField(TEXT("already_absent"), false);
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("remove_pose_search_roled_skeleton"), SaveError);
}

// ---------------------------------------------------------------------------
// Runtime motion-matching inspection (live PIE) — get_pose_search_runtime_state /
// evaluate_pose_search_query.
//
// These two probe the LIVE running world (why does a Motion Matching chooser
// return null?). They resolve a PIE/Game actor, read its skeletal-mesh
// AnimInstance and its MotionTrajectory (the FTransformTrajectory that
// UCharacterTrajectoryComponent — or any custom component — stores), and
// (for evaluate) drive UPoseSearchLibrary::MotionMatch against a loaded
// UPoseSearchDatabase, reporting the best-match asset + search cost.
//
// GOTCHA (trajectory read is generic): rather than hard-link the Experimental
// MotionTrajectory module, we find the live FTransformTrajectory by reflection —
// the first FStructProperty on any of the actor's components whose UScriptStruct
// is FTransformTrajectory::StaticStruct() (FTransformTrajectory lives in the
// Engine module, already a dependency). This matches UCharacterTrajectoryComponent
// ("Trajectory") and any project component that stores one.
//
// GOTCHA (per-channel cost): FPoseSearchBlueprintResult (the BlueprintPure
// MotionMatch output) exposes only the scalar SearchCost, not a per-channel
// breakdown (that lives in the low-level UE::PoseSearch::FPoseSearchCost /
// FSearchContext path, editor/trace-gated). So per_channel_cost is returned
// empty with a note — evaluate is best-effort as scoped.
// ---------------------------------------------------------------------------
namespace
{
    // Resolve the target world honoring an optional 'world' param.
    //   world == "editor"           -> the editor world (mcp_common::GetEditorWorld).
    //   world == "pie" / "game"     -> the live PIE/Game world; nullptr (OutMode="none")
    //                                  when no such session is running.
    //   world omitted               -> prefer the live PIE/Game world, else fall back
    //                                  to the editor world.
    // CANONICAL WORLD RESOLUTION. Pose history / trajectory are read off a LIVE character, so
    // reading the client's copy of a server-driven pawn is exactly the kind of correct-but-
    // wrong-world answer that reads as a broken feature.
    //
    //   'editor'                      -> the editor world.
    //   'pie' / 'game'                -> the live world; nullptr (OutMode="none") when none.
    //   omitted                       -> prefer the live world, else the editor world
    //                                    (that IS mcp_common::GetActiveWorld's order).
    //   'server' / 'client' / 'pie:N' -> that world; OutMode reports the resolved world's real type.
    UWorld* ResolveWorld_PoseSearch(const TSharedPtr<FJsonObject>& Params, FString& OutMode)
    {
        FString WorldParam;
        if (Params.IsValid())
        {
            Params->TryGetStringField(TEXT("world"), WorldParam);
        }
        WorldParam = WorldParam.TrimStartAndEnd();

        // Label a resolved world by what it ACTUALLY is, not by what was asked for.
        auto ModeOf = [](const UWorld* W) -> FString
        {
            if (!W) { return TEXT("none"); }
            switch (W->WorldType)
            {
            case EWorldType::PIE:    return TEXT("pie");
            case EWorldType::Game:   return TEXT("game");
            case EWorldType::Editor: return TEXT("editor");
            default:                 return FString(LexToString(W->WorldType.GetValue())).ToLower();
            }
        };

        // Omitted -> the canonical "whatever is live, else the editor" resolver.
        if (WorldParam.IsEmpty())
        {
            UWorld* Active = mcp_common::GetActiveWorld();
            OutMode = ModeOf(Active);
            return Active;
        }

        // `world` is given from here on, so the default selector passed below never applies.
        FString Selector;
        FString UnusedError;
        if (UWorld* Resolved = mcp_common::ResolveWorldSelector(Params, TEXT("editor"), Selector, UnusedError))
        {
            OutMode = ModeOf(Resolved);
            return Resolved;
        }

        // Explicit live-world request with no PIE session: also accept an EWorldType::Game
        // world (a cooked or -game process).
        if (!Selector.Equals(TEXT("editor"), ESearchCase::IgnoreCase))
        {
            if (UWorld* Active = mcp_common::ActiveGameWorld())
            {
                OutMode = TEXT("game");
                return Active;
            }
        }

        OutMode = TEXT("none");
        return nullptr;
    }

    // Resolve the actor to inspect: explicit actor_name (by name or label), else the
    // first player controller's pawn (the locally-controlled character).
    AActor* ResolveActor_PoseSearch(UWorld* World, const TSharedPtr<FJsonObject>& Params, FString& OutHow)
    {
        if (!World)
        {
            return nullptr;
        }
        FString ActorName;
        if (Params->TryGetStringField(TEXT("actor_name"), ActorName) && !ActorName.IsEmpty())
        {
            OutHow = TEXT("actor_name");
            return mcp_common::FindActorByName(World, ActorName);
        }
        OutHow = TEXT("player_pawn");
        if (APlayerController* PC = World->GetFirstPlayerController())
        {
            return PC->GetPawn();
        }
        return nullptr;
    }

    // Find the first live FTransformTrajectory on any of the actor's components by
    // reflection. Returns the struct pointer (or nullptr) and fills the source names.
    const FTransformTrajectory* FindActorTrajectory_PoseSearch(AActor* Actor, FString& OutComponentName, FString& OutPropertyName)
    {
        if (!Actor)
        {
            return nullptr;
        }
        for (UActorComponent* Comp : Actor->GetComponents())
        {
            if (!Comp)
            {
                continue;
            }
            for (TFieldIterator<FStructProperty> It(Comp->GetClass()); It; ++It)
            {
                FStructProperty* StructProp = *It;
                if (StructProp->Struct == FTransformTrajectory::StaticStruct())
                {
                    OutComponentName = Comp->GetName();
                    OutPropertyName = StructProp->GetName();
                    return StructProp->ContainerPtrToValuePtr<FTransformTrajectory>(Comp);
                }
            }
        }
        return nullptr;
    }

    // Serialize a FTransformTrajectory's samples (Position / Facing quat / TimeInSeconds).
    // MaxSamples <= 0 emits every sample.
    void SerializeTrajectory_PoseSearch(const FTransformTrajectory* Trajectory, int32 MaxSamples, TArray<TSharedPtr<FJsonValue>>& OutSamples)
    {
        if (!Trajectory)
        {
            return;
        }
        for (const FTransformTrajectorySample& S : Trajectory->Samples)
        {
            if (MaxSamples > 0 && OutSamples.Num() >= MaxSamples)
            {
                break;
            }
            TSharedPtr<FJsonObject> SampleObj = MakeShared<FJsonObject>();
            SampleObj->SetNumberField(TEXT("time_in_seconds"), S.TimeInSeconds);

            SampleObj->SetObjectField(TEXT("position"), mcp_common::VectorToJsonObject(S.Position));

            TSharedPtr<FJsonObject> FacingObj = MakeShared<FJsonObject>();
            FacingObj->SetNumberField(TEXT("x"), S.Facing.X);
            FacingObj->SetNumberField(TEXT("y"), S.Facing.Y);
            FacingObj->SetNumberField(TEXT("z"), S.Facing.Z);
            FacingObj->SetNumberField(TEXT("w"), S.Facing.W);
            SampleObj->SetObjectField(TEXT("facing"), FacingObj);

            OutSamples.Add(MakeShared<FJsonValueObject>(SampleObj));
        }
    }

    // Read-only walk of a UPoseSearchDatabase's schema + channels + anim asset count
    // into an existing JSON object (mirrors inspect_pose_search_database's shape).
    void SerializeDatabaseSummary_PoseSearch(UPoseSearchDatabase* Database, const TSharedPtr<FJsonObject>& Out)
    {
        if (!Database || !Out.IsValid())
        {
            return;
        }
        Out->SetStringField(TEXT("schema_path"),
            Database->Schema ? Database->Schema->GetPathName() : FString());
        Out->SetNumberField(TEXT("animation_asset_count"), Database->GetNumAnimationAssets());

        TArray<TSharedPtr<FJsonValue>> ChannelsJson;
        int32 SchemaCardinality = 0;
        if (Database->Schema)
        {
            SchemaCardinality = Database->Schema->SchemaCardinality;
            ChannelsJson = PoseSearch_ChannelsJson(Database->Schema);
        }
        Out->SetNumberField(TEXT("schema_cardinality"), SchemaCardinality);
        Out->SetNumberField(TEXT("channel_count"), ChannelsJson.Num());
        Out->SetArrayField(TEXT("channels"), ChannelsJson);
    }
}

// MCP:get_pose_search_runtime_state - Read-only report of the live motion-matching state on a PIE/Game actor (why a chooser returns null): its AnimInstance, its MotionTrajectory samples, whether a PoseSearchHistoryCollector node named pose_history_name exists, and (optionally) the target database's schema/channels.
//   @param world - Which world to resolve: absent (default) = prefer the live PIE/Game world, else the editor world | 'server' (the PIE world with authority -- what a server-driven character lives in) | 'client' | 'pie:N' | 'pie'/'game' force the live session (errors if none running) | 'editor' forces the editor world.
//   @param actor_name - Actor name or label to inspect. Omitted = the first player controller's pawn (the locally-controlled character).
//   @param pose_history_name - Tag of the PoseSearchHistoryCollector anim-graph node to look up on the AnimInstance (default 'PoseHistory'). A missing node is the usual reason a query builds an empty pose portion.
//   @param database_path - Optional UPoseSearchDatabase asset path; when given, its schema_path/schema_cardinality/channels/animation_asset_count are included.
//   @param max_trajectory_samples - Cap on emitted trajectory samples (<=0 = all; default 0).
//   @returns { world_mode, actor_name, actor_resolved, actor_resolved_by, skeletal_mesh_component, anim_instance_class, pose_history_name, pose_history_found, trajectory:{found,component,property,sample_count,is_trajectory_empty,samples[]}, is_trajectory_empty, database?:{database_path,schema_path,schema_cardinality,channel_count,channels[],animation_asset_count}, note, world_name, world_package, world_type, net_mode, pie_instance }
TSharedPtr<FJsonObject> HandleGetPoseSearchRuntimeState(const TSharedPtr<FJsonObject>& Params)
{
    FString WorldMode;
    UWorld* World = ResolveWorld_PoseSearch(Params, WorldMode);
    if (!World)
    {
        return mcp_common::MakeErrorReply(
            TEXT("get_pose_search_runtime_state: no live world resolved (world='pie'/'game' requested but no Play-In-Editor / game session is running)"));
    }

    FString ResolvedBy;
    AActor* Actor = ResolveActor_PoseSearch(World, Params, ResolvedBy);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("world_mode"), WorldMode);
    // Pose history/trajectory only exist on the LIVE character; reading the client's copy
    // of a server-driven pawn gives an empty-but-correct answer, so say which world was read.
    mcp_common::AddWorldEvidence(Data, World);
    Data->SetStringField(TEXT("actor_resolved_by"), ResolvedBy);

    if (!Actor)
    {
        FString ActorName;
        Params->TryGetStringField(TEXT("actor_name"), ActorName);
        Data->SetBoolField(TEXT("actor_resolved"), false);
        Data->SetStringField(TEXT("actor_name"), ActorName);
        Data->SetStringField(TEXT("note"), ActorName.IsEmpty()
            ? TEXT("No actor_name given and no player pawn found in the resolved world")
            : FString::Printf(TEXT("Actor not found: %s (world=%s)"), *ActorName, *WorldMode));
        return mcp_common::MakeOkReply(Data);
    }

    Data->SetBoolField(TEXT("actor_resolved"), true);
    Data->SetStringField(TEXT("actor_name"), Actor->GetName());

    // AnimInstance off the actor's skeletal-mesh component.
    USkeletalMeshComponent* SkelComp = Actor->FindComponentByClass<USkeletalMeshComponent>();
    UAnimInstance* AnimInstance = SkelComp ? SkelComp->GetAnimInstance() : nullptr;
    Data->SetStringField(TEXT("skeletal_mesh_component"), SkelComp ? SkelComp->GetName() : FString());
    Data->SetStringField(TEXT("anim_instance_class"),
        AnimInstance ? AnimInstance->GetClass()->GetPathName() : FString());

    // Pose-history collector node lookup (a missing node = empty pose query).
    FString PoseHistoryName = TEXT("PoseHistory");
    Params->TryGetStringField(TEXT("pose_history_name"), PoseHistoryName);
    bool bPoseHistoryFound = false;
    if (AnimInstance)
    {
        bPoseHistoryFound = UPoseSearchLibrary::FindPoseHistoryNode(FName(*PoseHistoryName), AnimInstance) != nullptr;
    }
    Data->SetStringField(TEXT("pose_history_name"), PoseHistoryName);
    Data->SetBoolField(TEXT("pose_history_found"), bPoseHistoryFound);

    // Live MotionTrajectory (read by reflection — component-type-agnostic).
    int32 MaxSamples = 0;
    Params->TryGetNumberField(TEXT("max_trajectory_samples"), MaxSamples);
    FString TrajComp, TrajProp;
    const FTransformTrajectory* Trajectory = FindActorTrajectory_PoseSearch(Actor, TrajComp, TrajProp);
    TArray<TSharedPtr<FJsonValue>> SamplesJson;
    SerializeTrajectory_PoseSearch(Trajectory, MaxSamples, SamplesJson);
    const int32 TotalSamples = Trajectory ? Trajectory->Samples.Num() : 0;
    const bool bTrajectoryEmpty = (TotalSamples == 0);

    TSharedPtr<FJsonObject> TrajObj = MakeShared<FJsonObject>();
    TrajObj->SetBoolField(TEXT("found"), Trajectory != nullptr);
    TrajObj->SetStringField(TEXT("component"), TrajComp);
    TrajObj->SetStringField(TEXT("property"), TrajProp);
    TrajObj->SetNumberField(TEXT("sample_count"), TotalSamples);
    TrajObj->SetBoolField(TEXT("is_trajectory_empty"), bTrajectoryEmpty);
    TrajObj->SetArrayField(TEXT("samples"), SamplesJson);
    Data->SetObjectField(TEXT("trajectory"), TrajObj);
    Data->SetBoolField(TEXT("is_trajectory_empty"), bTrajectoryEmpty);

    // Optional database summary.
    FString DatabasePath;
    if (Params->TryGetStringField(TEXT("database_path"), DatabasePath) && !DatabasePath.IsEmpty())
    {
        UPoseSearchDatabase* Database = LoadObject<UPoseSearchDatabase>(nullptr, *DatabasePath);
        if (Database)
        {
            TSharedPtr<FJsonObject> DbObj = MakeShared<FJsonObject>();
            DbObj->SetStringField(TEXT("database_path"), Database->GetPathName());
            SerializeDatabaseSummary_PoseSearch(Database, DbObj);
            Data->SetObjectField(TEXT("database"), DbObj);
        }
        else
        {
            Data->SetStringField(TEXT("database_note"),
                FString::Printf(TEXT("UPoseSearchDatabase not found: %s"), *DatabasePath));
        }
    }

    Data->SetStringField(TEXT("note"), (!AnimInstance)
        ? TEXT("Actor has no skeletal-mesh AnimInstance; motion matching cannot run on it.")
        : (!bPoseHistoryFound
            ? TEXT("No PoseSearchHistoryCollector node found for pose_history_name; a MotionMatch query would have no pose history to build its query from (common cause of a null chooser result).")
            : (bTrajectoryEmpty
                ? TEXT("No MotionTrajectory samples on the actor; the trajectory portion of the query is empty.")
                : TEXT("AnimInstance + pose-history node + trajectory all present."))));
    return mcp_common::MakeOkReply(Data);
}

// MCP:evaluate_pose_search_query - Run a live motion-matching query on a PIE/Game actor against a loaded UPoseSearchDatabase (UPoseSearchLibrary::MotionMatch) and return the best-match asset + search cost, plus the query trajectory. Diagnoses why a chooser returns null. Best-effort: per-channel cost is not exposed by the BlueprintPure result.
//   @param world - Which world to resolve: absent (default) = prefer the live PIE/Game world | 'server' (the PIE world with authority) | 'client' | 'pie:N' | 'pie'/'game' force the live session (errors if none running) | 'editor' forces the editor world.
//   @param actor_name - Actor name or label whose AnimInstance drives the query. Omitted = the first player controller's pawn.
//   @param database_path - UPoseSearchDatabase asset path to search (required).
//   @param pose_history_name - Tag of the PoseSearchHistoryCollector anim-graph node used to build the query (default 'PoseHistory').
//   @param max_trajectory_samples - Cap on emitted query trajectory samples (<=0 = all; default 0).
//   @returns { world_mode, actor_name, actor_resolved, anim_instance_class, pose_history_name, pose_history_found, database_path, query_trajectory_samples[], trajectory_sample_count, is_trajectory_empty, has_match, best_match_asset, best_match_database, best_cost, selected_time, is_continuing_pose_search, wanted_play_rate, is_mirrored, is_looping, per_channel_cost[], note, world_name, world_package, world_type, net_mode, pie_instance }
TSharedPtr<FJsonObject> HandleEvaluatePoseSearchQuery(const TSharedPtr<FJsonObject>& Params)
{
    FString DatabasePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("database_path"), DatabasePath)) { return Missing; }

    FString WorldMode;
    UWorld* World = ResolveWorld_PoseSearch(Params, WorldMode);
    if (!World)
    {
        return mcp_common::MakeErrorReply(
            TEXT("evaluate_pose_search_query: no live world resolved (world='pie'/'game' requested but no Play-In-Editor / game session is running)"));
    }

    UPoseSearchDatabase* Database = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(DatabasePath, TEXT("UPoseSearchDatabase"), Database)) { return Error; }

    FString ResolvedBy;
    AActor* Actor = ResolveActor_PoseSearch(World, Params, ResolvedBy);
    if (!Actor)
    {
        FString ActorName;
        Params->TryGetStringField(TEXT("actor_name"), ActorName);
        return mcp_common::MakeErrorReply(ActorName.IsEmpty()
            ? TEXT("evaluate_pose_search_query: no actor_name given and no player pawn found in the resolved world")
            : FString::Printf(TEXT("evaluate_pose_search_query: actor not found: %s"), *ActorName));
    }

    USkeletalMeshComponent* SkelComp = Actor->FindComponentByClass<USkeletalMeshComponent>();
    UAnimInstance* AnimInstance = SkelComp ? SkelComp->GetAnimInstance() : nullptr;

    FString PoseHistoryName = TEXT("PoseHistory");
    Params->TryGetStringField(TEXT("pose_history_name"), PoseHistoryName);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("world_mode"), WorldMode);
    // Which world the queried character was read from (see get_pose_search_runtime_state).
    mcp_common::AddWorldEvidence(Data, World);
    Data->SetStringField(TEXT("actor_name"), Actor->GetName());
    Data->SetBoolField(TEXT("actor_resolved"), true);
    Data->SetStringField(TEXT("database_path"), Database->GetPathName());
    Data->SetStringField(TEXT("pose_history_name"), PoseHistoryName);
    Data->SetStringField(TEXT("anim_instance_class"),
        AnimInstance ? AnimInstance->GetClass()->GetPathName() : FString());

    // Query trajectory (same reflective read as the runtime-state report).
    int32 MaxSamples = 0;
    Params->TryGetNumberField(TEXT("max_trajectory_samples"), MaxSamples);
    FString TrajComp, TrajProp;
    const FTransformTrajectory* Trajectory = FindActorTrajectory_PoseSearch(Actor, TrajComp, TrajProp);
    TArray<TSharedPtr<FJsonValue>> SamplesJson;
    SerializeTrajectory_PoseSearch(Trajectory, MaxSamples, SamplesJson);
    const int32 TotalSamples = Trajectory ? Trajectory->Samples.Num() : 0;
    Data->SetArrayField(TEXT("query_trajectory_samples"), SamplesJson);
    Data->SetNumberField(TEXT("trajectory_sample_count"), TotalSamples);
    Data->SetBoolField(TEXT("is_trajectory_empty"), TotalSamples == 0);

    // per_channel_cost is not exposed by the BlueprintPure result path (see the per-channel cost
    // GOTCHA above).
    Data->SetArrayField(TEXT("per_channel_cost"), TArray<TSharedPtr<FJsonValue>>());

    bool bPoseHistoryFound = false;
    if (!AnimInstance)
    {
        Data->SetBoolField(TEXT("pose_history_found"), false);
        Data->SetBoolField(TEXT("has_match"), false);
        Data->SetStringField(TEXT("best_match_asset"), FString());
        Data->SetNumberField(TEXT("best_cost"), MAX_flt);
        Data->SetStringField(TEXT("note"),
            TEXT("Actor has no skeletal-mesh AnimInstance; MotionMatch requires one. Cannot evaluate."));
        return mcp_common::MakeOkReply(Data);
    }

    bPoseHistoryFound = UPoseSearchLibrary::FindPoseHistoryNode(FName(*PoseHistoryName), AnimInstance) != nullptr;
    Data->SetBoolField(TEXT("pose_history_found"), bPoseHistoryFound);

    // Drive the core motion-matching algorithm. MotionMatch builds an FSearchContext
    // internally from the AnimInstance + the named pose-history collector node, runs
    // the search against the given database, and fills FPoseSearchBlueprintResult.
    // A null SelectedAnim (SearchCost == MAX_flt) is the "chooser returns null"
    // symptom — usually a missing pose-history node or an empty query.
    FPoseSearchBlueprintResult Result;
    TArray<UObject*> AssetsToSearch;
    AssetsToSearch.Add(Database);
    UPoseSearchLibrary::MotionMatch(
        AnimInstance,
        AssetsToSearch,
        FName(*PoseHistoryName),
        FPoseSearchContinuingProperties(),
        FPoseSearchFutureProperties(),
        Result);

    const bool bHasMatch = Result.SelectedAnim != nullptr;
    Data->SetBoolField(TEXT("has_match"), bHasMatch);
    Data->SetStringField(TEXT("best_match_asset"),
        Result.SelectedAnim ? Result.SelectedAnim->GetPathName() : FString());
    Data->SetStringField(TEXT("best_match_database"),
        Result.SelectedDatabase ? Result.SelectedDatabase->GetPathName() : FString());
    Data->SetNumberField(TEXT("best_cost"), Result.SearchCost);
    Data->SetNumberField(TEXT("selected_time"), Result.SelectedTime);
    Data->SetBoolField(TEXT("is_continuing_pose_search"), Result.bIsContinuingPoseSearch);
    Data->SetNumberField(TEXT("wanted_play_rate"), Result.WantedPlayRate);
    Data->SetBoolField(TEXT("is_mirrored"), Result.bIsMirrored);
    Data->SetBoolField(TEXT("is_looping"), Result.bLoop);

    Data->SetStringField(TEXT("note"), bHasMatch
        ? TEXT("MotionMatch returned a valid best-match. per_channel_cost is empty (not exposed by the BlueprintPure result; use the low-level FSearchContext/FPoseSearchCost path for a channel breakdown).")
        : (!bPoseHistoryFound
            ? TEXT("MotionMatch returned no match and no PoseSearchHistoryCollector node named pose_history_name was found -- the query has no pose history, so the chooser resolves to null. Add/rename the collector node in the anim graph.")
            : TEXT("MotionMatch returned no match though a pose-history node exists -- check that the database index is built, its schema matches the skeleton, and the query trajectory is non-empty.")));
    return mcp_common::MakeOkReply(Data);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_animation_asset_to_pose_search_database"), &HandleAddAnimationAssetToPoseSearchDatabase },
    { TEXT("add_pose_search_channel_to_schema"), &HandleAddPoseSearchChannelToSchema },
    { TEXT("add_pose_search_roled_skeleton"), &HandleAddPoseSearchRoledSkeleton },
    { TEXT("build_pose_search_database_index"), &HandleBuildPoseSearchDatabaseIndex },
    { TEXT("create_pose_search_database"), &HandleCreatePoseSearchDatabase },
    { TEXT("create_pose_search_schema"), &HandleCreatePoseSearchSchema },
    { TEXT("evaluate_pose_search_query"), &HandleEvaluatePoseSearchQuery },
    { TEXT("get_pose_search_runtime_state"), &HandleGetPoseSearchRuntimeState },
    { TEXT("inspect_pose_search_database"), &HandleInspectPoseSearchDatabase },
    { TEXT("inspect_pose_search_schema"), &HandleInspectPoseSearchSchema },
    { TEXT("remove_animation_asset_from_pose_search_database"), &HandleRemoveAnimationAssetFromPoseSearchDatabase },
    { TEXT("remove_pose_search_channel_from_schema"), &HandleRemovePoseSearchChannelFromSchema },
    { TEXT("remove_pose_search_database"), &HandleRemovePoseSearchDatabase },
    { TEXT("remove_pose_search_roled_skeleton"), &HandleRemovePoseSearchRoledSkeleton },
    { TEXT("remove_pose_search_schema"), &HandleRemovePoseSearchSchema },
    { TEXT("set_pose_search_database_schema"), &HandleSetPoseSearchDatabaseSchema },
    { TEXT("set_pose_search_schema_mirror_data_table"), &HandleSetPoseSearchSchemaMirrorDataTable },
    { TEXT("set_pose_search_schema_properties"), &HandleSetPoseSearchSchemaProperties },
    { TEXT("set_pose_search_schema_role"), &HandleSetPoseSearchSchemaRole },
    { TEXT("set_pose_search_schema_skeleton"), &HandleSetPoseSearchSchemaSkeleton },
};
} // namespace UEAgentPoseSearchCommands
