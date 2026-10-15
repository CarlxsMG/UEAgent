#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

#include "EditorAssetLibrary.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/NameTypes.h"

// Collection manager (Developer/CollectionManager module).
#include "CollectionManagerModule.h"
#include "CollectionManagerTypes.h"
#include "ICollectionManager.h"
#include "ICollectionContainer.h"

/**
 * Asset metadata tag and Content Browser collection commands.
 *
 * Metadata tags use UEditorAssetLibrary::SetMetadataTag/GetMetadataTag/GetMetadataTagValues (editor-only,
 * WITH_EDITOR). Collections use the ICollectionContainer API obtained from
 * FCollectionManagerModule::GetModule().Get().GetProjectCollectionContainer(); the legacy
 * ICollectionManager collection methods are UE_DEPRECATED(5.6).
 */
namespace UEAgentAssetMetaCommands
{

// MCP-DOMAIN: Asset

namespace
{
    /** Resolve an ECollectionShareType from a user string; defaults to Local (not under SCC). */
    ECollectionShareType::Type ShareTypeFromString(const FString& In)
    {
        // ECollectionShareType::FromString understands "Local"/"Private"/"Shared"/"System"/"All".
        ECollectionShareType::Type Type = ECollectionShareType::FromString(*In, ECollectionShareType::CST_Local);
        // Never let an unrecognised value collapse to CST_All (which is not a real storage bucket).
        if (Type == ECollectionShareType::CST_All)
        {
            Type = ECollectionShareType::CST_Local;
        }
        return Type;
    }

    /** Get the project's collection container (UE 5.7 top-level collections object). */
    TSharedPtr<ICollectionContainer> GetProjectContainer()
    {
        if (!FCollectionManagerModule::IsModuleAvailable())
        {
            return nullptr;
        }
        ICollectionManager& Manager = FCollectionManagerModule::GetModule().Get();
        return Manager.GetProjectCollectionContainer();
    }

    /** The collection a command names: the project container, the collection's name and its share type. */
    struct FAssetMetaCollection
    {
        TSharedPtr<ICollectionContainer> Container;
        FName Name;
        FString ShareTypeStr;
        ECollectionShareType::Type ShareType = ECollectionShareType::CST_Local;
    };

    /** Reads share_type and opens the project container for CollectionName: null, or the error reply. */
    TSharedPtr<FJsonObject> AssetMeta_OpenCollection(const TSharedPtr<FJsonObject>& Params, const FString& CollectionName,
        FAssetMetaCollection& Out)
    {
        Out.ShareTypeStr = TEXT("Local");
        Params->TryGetStringField(TEXT("share_type"), Out.ShareTypeStr);
        Out.ShareType = ShareTypeFromString(Out.ShareTypeStr);

        Out.Container = GetProjectContainer();
        if (!Out.Container.IsValid())
        {
            return mcp_common::MakeErrorReply(TEXT("Collection manager / project collection container unavailable"));
        }
        Out.Name = FName(*CollectionName);
        return nullptr;
    }

    /** AssetMeta_OpenCollection for a collection that must already exist: null, or the error reply. */
    TSharedPtr<FJsonObject> AssetMeta_OpenExistingCollection(const TSharedPtr<FJsonObject>& Params,
        const FString& CollectionName, FAssetMetaCollection& Out)
    {
        if (TSharedPtr<FJsonObject> Error = AssetMeta_OpenCollection(Params, CollectionName, Out))
        {
            return Error;
        }
        if (!Out.Container->CollectionExists(Out.Name, Out.ShareType))
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Collection not found: %s (%s)"), *CollectionName, *Out.ShareTypeStr));
        }
        return nullptr;
    }

    /** A reply that starts with the collection's name and share type. */
    TSharedPtr<FJsonObject> AssetMeta_StartCollectionReply(const FString& CollectionName, const FAssetMetaCollection& Collection)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("collection_name"), CollectionName);
        Result->SetStringField(TEXT("share_type"), ECollectionShareType::ToString(Collection.ShareType));
        return Result;
    }
}

// -------------------------------------------------------------------------- //
// Metadata tags
// -------------------------------------------------------------------------- //

// MCP:set_asset_metadata_tag - Sets (or overwrites) a string metadata tag on an asset and saves it.  The asset is loaded, UEditorAssetLibrary::SetMetadataTag is applied, the owning package is saved, and the value is read back to verify.
//   @param asset_path - /Game path to the asset (e.g. "/Game/MyGame/M_Example").
//   @param key - Metadata tag name (FName), e.g. "Author".
//   @param value - String value to associate with the tag.
//   @returns { asset_path: string, key: string, value: string, read_back: string, tag_set: bool, verified: bool, saved: bool }

TSharedPtr<FJsonObject> HandleSetAssetMetadataTag(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath, Key, Value;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_path"), AssetPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("key"), Key, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("value"), Value, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    if (!UEditorAssetLibrary::DoesAssetExist(AssetPath))
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Asset does not exist: %s"), *AssetPath));

    UObject* Asset = UEditorAssetLibrary::LoadAsset(AssetPath);
    if (!Asset)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Failed to load asset: %s"), *AssetPath));

    const FName Tag(*Key);
    UEditorAssetLibrary::SetMetadataTag(Asset, Tag, Value);

    // Persist the change.
    const bool bSaved = UEditorAssetLibrary::SaveLoadedAsset(Asset, /*bOnlyIfIsDirty=*/false);

    // Verify by reading it back.
    const FString ReadBack = UEditorAssetLibrary::GetMetadataTag(Asset, Tag);
    const bool bVerified = ReadBack.Equals(Value);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("asset_path"), AssetPath);
    Result->SetStringField(TEXT("key"), Key);
    Result->SetStringField(TEXT("value"), Value);
    Result->SetStringField(TEXT("read_back"), ReadBack);
    Result->SetBoolField(TEXT("tag_set"), true);
    Result->SetBoolField(TEXT("verified"), bVerified);
    return mcp_common::MakeSavedReply(Result, bSaved, TEXT("set_asset_metadata_tag"));
}

// MCP:get_asset_metadata_tags - Reads all editor metadata tags (UMetaData) of an asset.  Wraps UEditorAssetLibrary::GetMetadataTagValues and returns each tag as a {key, value} pair. (Distinct from get_asset_metadata, which reads asset registry data instead of the asset's stored metadata tags.)
//   @param asset_path - /Game path to the asset.
//   @returns { asset_path: string, tags: [{ key: string, value: string }], tag_count: number }

TSharedPtr<FJsonObject> HandleGetAssetMetadata(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_path"), AssetPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    if (!UEditorAssetLibrary::DoesAssetExist(AssetPath))
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Asset does not exist: %s"), *AssetPath));

    UObject* Asset = UEditorAssetLibrary::LoadAsset(AssetPath);
    if (!Asset)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Failed to load asset: %s"), *AssetPath));

    const TMap<FName, FString> TagValues = UEditorAssetLibrary::GetMetadataTagValues(Asset);

    TArray<TSharedPtr<FJsonValue>> TagsArray;
    for (const TPair<FName, FString>& Pair : TagValues)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("key"), Pair.Key.ToString());
        Entry->SetStringField(TEXT("value"), Pair.Value);
        TagsArray.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("asset_path"), AssetPath);
    Result->SetArrayField(TEXT("tags"), TagsArray);
    Result->SetNumberField(TEXT("tag_count"), TagsArray.Num());
    return mcp_common::MakeOkReply(Result);
}

// -------------------------------------------------------------------------- //
// Collections
// -------------------------------------------------------------------------- //

// MCP:create_collection - Creates a static Content Browser collection (idempotent).  Uses the UE 5.7 project ICollectionContainer. If the collection already exists a stable payload is returned (already_existed=True).          "Private", or "Shared".
//   @param collection_name - Name of the collection.
//   @param share_type - ECollectionShareType: "Local", "Private", or "Shared" (optional, default "Local")
//   @returns { collection_name: string, share_type: string, created: bool, already_existed: bool, exists: bool }

TSharedPtr<FJsonObject> HandleCreateCollection(const TSharedPtr<FJsonObject>& Params)
{
    FString CollectionName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("collection_name"), CollectionName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    FAssetMetaCollection Collection;
    if (TSharedPtr<FJsonObject> OpenError = AssetMeta_OpenCollection(Params, CollectionName, Collection)) { return OpenError; }

    // Idempotent: if it already exists, return a stable payload.
    const bool bAlready = Collection.Container->CollectionExists(Collection.Name, Collection.ShareType);

    bool bCreated = false;
    if (!bAlready)
    {
        FText Error;
        bCreated = Collection.Container->CreateCollection(Collection.Name, Collection.ShareType, ECollectionStorageMode::Static, &Error);
        if (!bCreated)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Failed to create collection '%s' (%s): %s"),
                    *CollectionName, *Collection.ShareTypeStr, *Error.ToString()));
        }
    }

    const bool bExistsNow = Collection.Container->CollectionExists(Collection.Name, Collection.ShareType);

    TSharedPtr<FJsonObject> Result = AssetMeta_StartCollectionReply(CollectionName, Collection);
    Result->SetBoolField(TEXT("created"), bCreated);
    Result->SetBoolField(TEXT("already_existed"), bAlready);
    Result->SetBoolField(TEXT("exists"), bExistsNow);
    return mcp_common::MakeOkReply(Result);
}

// MCP:delete_collection - Destroys a Content Browser collection (idempotent; inverse of create_collection).  Uses the UE 5.7 project ICollectionContainer::DestroyCollection. If the collection is already absent a stable payload is returned (already_absent=True).          "Private", or "Shared".
//   @param collection_name - Name of the collection to destroy.
//   @param share_type - ECollectionShareType: "Local", "Private", or "Shared" (optional, default "Local")
//   @returns { collection_name: string, share_type: string, destroyed: bool, already_absent: bool, exists: bool }

TSharedPtr<FJsonObject> HandleDeleteCollection(const TSharedPtr<FJsonObject>& Params)
{
    FString CollectionName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("collection_name"), CollectionName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    FAssetMetaCollection Collection;
    if (TSharedPtr<FJsonObject> OpenError = AssetMeta_OpenCollection(Params, CollectionName, Collection)) { return OpenError; }

    // Idempotent: DestroyCollection returns false when the collection is already
    // absent, which is not an error. Detect already-absent BEFORE calling.
    const bool bExisted = Collection.Container->CollectionExists(Collection.Name, Collection.ShareType);

    bool bDestroyed = false;
    if (bExisted)
    {
        FText Error;
        bDestroyed = Collection.Container->DestroyCollection(Collection.Name, Collection.ShareType, &Error);
        if (!bDestroyed)
        {
            // The collection WAS present, so a false return is a real failure.
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Failed to destroy collection '%s' (%s): %s"),
                    *CollectionName, *Collection.ShareTypeStr, *Error.ToString()));
        }
    }

    const bool bExistsNow = Collection.Container->CollectionExists(Collection.Name, Collection.ShareType);

    TSharedPtr<FJsonObject> Result = AssetMeta_StartCollectionReply(CollectionName, Collection);
    Result->SetBoolField(TEXT("destroyed"), bDestroyed);
    Result->SetBoolField(TEXT("already_absent"), !bExisted);
    Result->SetBoolField(TEXT("exists"), bExistsNow);
    return mcp_common::MakeOkReply(Result);
}

// MCP:add_asset_to_collection - Adds an asset to an existing collection and reads back its object count.          must match the share type the collection was created with.
//   @param collection_name - Name of the (existing) collection.
//   @param asset_path - /Game path / object path of the asset to add.
//   @param share_type - ECollectionShareType: "Local", "Private", or "Shared" (optional, default "Local")
//   @returns { collection_name: string, share_type: string, asset_path: string, added: bool, verified: bool, object_count: number }

TSharedPtr<FJsonObject> HandleAddAssetToCollection(const TSharedPtr<FJsonObject>& Params)
{
    FString CollectionName, AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("collection_name"), CollectionName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_path"), AssetPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    FAssetMetaCollection Collection;
    if (TSharedPtr<FJsonObject> OpenError = AssetMeta_OpenExistingCollection(Params, CollectionName, Collection)) { return OpenError; }

    const FSoftObjectPath ObjectPath(AssetPath);

    FText Error;
    const bool bAdded = Collection.Container->AddToCollection(Collection.Name, Collection.ShareType, ObjectPath, &Error);
    if (!bAdded)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Failed to add '%s' to collection '%s': %s"),
                *AssetPath, *CollectionName, *Error.ToString()));
    }

    // Read back the collection's object count (verifies the add landed).
    TArray<FSoftObjectPath> AssetsInCollection;
    Collection.Container->GetAssetsInCollection(Collection.Name, Collection.ShareType, AssetsInCollection, ECollectionRecursionFlags::Self);
    const bool bContains = Collection.Container->IsObjectInCollection(ObjectPath, Collection.Name, Collection.ShareType);

    TSharedPtr<FJsonObject> Result = AssetMeta_StartCollectionReply(CollectionName, Collection);
    Result->SetStringField(TEXT("asset_path"), AssetPath);
    Result->SetBoolField(TEXT("added"), bAdded);
    Result->SetBoolField(TEXT("verified"), bContains);
    Result->SetNumberField(TEXT("object_count"), AssetsInCollection.Num());
    return mcp_common::MakeOkReply(Result);
}

// MCP:remove_asset_from_collection - Removes an asset from a collection and reads back its object count (idempotent; inverse of add_asset_to_collection).  If the asset is already absent from the collection a stable payload is returned (already_absent=True).          must match the share type the collection was created with.
//   @param collection_name - Name of the (existing) collection.
//   @param asset_path - /Game path / object path of the asset to remove.
//   @param share_type - ECollectionShareType: "Local", "Private", or "Shared" (optional, default "Local")
//   @returns { collection_name: string, share_type: string, asset_path: string, removed: bool, already_absent: bool, verified: bool, object_count: number }

TSharedPtr<FJsonObject> HandleRemoveAssetFromCollection(const TSharedPtr<FJsonObject>& Params)
{
    FString CollectionName, AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("collection_name"), CollectionName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_path"), AssetPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    FAssetMetaCollection Collection;
    if (TSharedPtr<FJsonObject> OpenError = AssetMeta_OpenExistingCollection(Params, CollectionName, Collection)) { return OpenError; }

    const FSoftObjectPath ObjectPath(AssetPath);

    // Idempotent: RemoveFromCollection returns false when the asset is already
    // absent, which is not an error. Detect already-absent BEFORE calling.
    const bool bWasPresent = Collection.Container->IsObjectInCollection(ObjectPath, Collection.Name, Collection.ShareType);

    bool bRemoved = false;
    if (bWasPresent)
    {
        FText Error;
        bRemoved = Collection.Container->RemoveFromCollection(Collection.Name, Collection.ShareType, ObjectPath, &Error);
        if (!bRemoved)
        {
            // The asset WAS a member, so a false return is a real failure.
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Failed to remove '%s' from collection '%s': %s"),
                    *AssetPath, *CollectionName, *Error.ToString()));
        }
    }

    // Read back the collection's object count (verifies the remove landed).
    TArray<FSoftObjectPath> AssetsInCollection;
    Collection.Container->GetAssetsInCollection(Collection.Name, Collection.ShareType, AssetsInCollection, ECollectionRecursionFlags::Self);
    const bool bStillContains = Collection.Container->IsObjectInCollection(ObjectPath, Collection.Name, Collection.ShareType);

    TSharedPtr<FJsonObject> Result = AssetMeta_StartCollectionReply(CollectionName, Collection);
    Result->SetStringField(TEXT("asset_path"), AssetPath);
    Result->SetBoolField(TEXT("removed"), bRemoved);
    Result->SetBoolField(TEXT("already_absent"), !bWasPresent);
    Result->SetBoolField(TEXT("verified"), !bStillContains);
    Result->SetNumberField(TEXT("object_count"), AssetsInCollection.Num());
    return mcp_common::MakeOkReply(Result);
}

// MCP:get_collections - List every Content Browser collection of the project container, of every share type, without changing anything: name and share_type of each (the share_type that get_collection_assets and the other collection tools need), plus collection_count. Errors when the collection manager is unavailable.
//   @returns { collections: [{ name: string, share_type: string }], collection_count: number }

TSharedPtr<FJsonObject> HandleGetCollections(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<ICollectionContainer> Container = GetProjectContainer();
    if (!Container.IsValid())
        return mcp_common::MakeErrorReply(TEXT("Collection manager / project collection container unavailable"));

    TArray<FCollectionNameType> Collections;
    Container->GetCollections(Collections);

    TArray<TSharedPtr<FJsonValue>> CollectionsArray;
    for (const FCollectionNameType& Coll : Collections)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Coll.Name.ToString());
        Entry->SetStringField(TEXT("share_type"), ECollectionShareType::ToString(Coll.Type));
        CollectionsArray.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("collections"), CollectionsArray);
    Result->SetNumberField(TEXT("collection_count"), CollectionsArray.Num());
    return mcp_common::MakeOkReply(Result);
}

// MCP:get_collection_assets - Lists the asset object paths contained in a collection.          must match the collection's share type.
//   @param collection_name - Name of the (existing) collection.
//   @param share_type - ECollectionShareType: "Local", "Private", or "Shared" (optional, default "Local")
//   @returns { collection_name: string, share_type: string, assets: [string], asset_count: number }

TSharedPtr<FJsonObject> HandleGetCollectionAssets(const TSharedPtr<FJsonObject>& Params)
{
    FString CollectionName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("collection_name"), CollectionName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    FAssetMetaCollection Collection;
    if (TSharedPtr<FJsonObject> OpenError = AssetMeta_OpenExistingCollection(Params, CollectionName, Collection)) { return OpenError; }

    TArray<FSoftObjectPath> AssetPaths;
    Collection.Container->GetAssetsInCollection(Collection.Name, Collection.ShareType, AssetPaths, ECollectionRecursionFlags::Self);

    TArray<TSharedPtr<FJsonValue>> AssetsArray;
    for (const FSoftObjectPath& Path : AssetPaths)
    {
        AssetsArray.Add(MakeShared<FJsonValueString>(Path.ToString()));
    }

    TSharedPtr<FJsonObject> Result = AssetMeta_StartCollectionReply(CollectionName, Collection);
    Result->SetArrayField(TEXT("assets"), AssetsArray);
    Result->SetNumberField(TEXT("asset_count"), AssetsArray.Num());
    return mcp_common::MakeOkReply(Result);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_asset_to_collection"), &HandleAddAssetToCollection },
    { TEXT("create_collection"), &HandleCreateCollection },
    { TEXT("delete_collection"), &HandleDeleteCollection },
    { TEXT("get_asset_metadata_tags"), &HandleGetAssetMetadata },
    { TEXT("get_collection_assets"), &HandleGetCollectionAssets },
    { TEXT("get_collections"), &HandleGetCollections },
    { TEXT("remove_asset_from_collection"), &HandleRemoveAssetFromCollection },
    { TEXT("set_asset_metadata_tag"), &HandleSetAssetMetadataTag },
};
} // namespace UEAgentAssetMetaCommands
