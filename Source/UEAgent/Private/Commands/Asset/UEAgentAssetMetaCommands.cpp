// Copyright (c) 2026 CarlxsMG.

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

// MCP:set_asset_metadata_tag - Sets (or overwrites) a string metadata tag on an asset and saves it.  The asset is loaded, UEditorAssetLibrary::SetMetadataTag is applied, the owning package is saved, and the value is read back to verify. Inverse: remove_asset_metadata_tag.
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

// MCP:remove_asset_metadata_tag - Remove one string metadata tag (UMetaData) from an asset and save it (UEditorAssetLibrary::RemoveMetadataTag); no undo step, since package metadata is outside the undo history, and refused by the engine during PIE (the tag stays, reported as an error). Inverse of set_asset_metadata_tag. A key the asset has no tag for is an honest no-op (removed=false, nothing saved); an asset that does not exist is an error. The reply re-reads the tags (still_present, tag_count). Read: get_asset_metadata_tags.
//   @param asset_path - /Game path to the asset (e.g. "/Game/MyGame/M_Example").
//   @param key - Metadata tag name (FName) to remove, e.g. "Author".
//   @returns { asset_path: string, key: string, removed: bool, still_present: bool, tag_count: int, saved: bool }

TSharedPtr<FJsonObject> HandleRemoveAssetMetadataTag(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath, Key;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_path"), AssetPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("key"), Key)) { return Missing; }

    if (!UEditorAssetLibrary::DoesAssetExist(AssetPath))
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Asset does not exist: %s"), *AssetPath));
    UObject* Asset = UEditorAssetLibrary::LoadAsset(AssetPath);
    if (!Asset)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Failed to load asset: %s"), *AssetPath));

    const FName Tag(*Key);
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("asset_path"), AssetPath);
    Result->SetStringField(TEXT("key"), Key);
    if (!UEditorAssetLibrary::GetMetadataTagValues(Asset).Contains(Tag))
    {
        Result->SetBoolField(TEXT("removed"), false);
        Result->SetBoolField(TEXT("still_present"), false);
        Result->SetNumberField(TEXT("tag_count"), UEditorAssetLibrary::GetMetadataTagValues(Asset).Num());
        Result->SetBoolField(TEXT("saved"), false);
        return mcp_common::MakeOkReply(Result);
    }

    UEditorAssetLibrary::RemoveMetadataTag(Asset, Tag);
    const TMap<FName, FString> After = UEditorAssetLibrary::GetMetadataTagValues(Asset);
    const bool bStillPresent = After.Contains(Tag);
    if (bStillPresent)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Tag '%s' is still on the asset after RemoveMetadataTag; nothing was saved"), *Key));
    }
    const bool bSaved = UEditorAssetLibrary::SaveLoadedAsset(Asset, /*bOnlyIfIsDirty=*/false);

    Result->SetBoolField(TEXT("removed"), true);
    Result->SetBoolField(TEXT("still_present"), false);
    Result->SetNumberField(TEXT("tag_count"), After.Num());
    return mcp_common::MakeSavedReply(Result, bSaved, TEXT("remove_asset_metadata_tag"));
}

// MCP:get_asset_metadata_tags - Reads all editor metadata tags (UMetaData) of an asset.  Wraps UEditorAssetLibrary::GetMetadataTagValues and returns each tag as a {key, value} pair. (Distinct from get_asset_by_name with include_tags, which reads Asset Registry tags instead of the asset's stored metadata tags.)
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

// MCP:create_collection - Create a static Content Browser collection named collection_name in the project's collection container, under share_type (default Local; Private or Shared otherwise, and an unrecognised value falls back to Local). Idempotent: when that name already exists under that share type nothing is created and the call succeeds with created false and already_existed true; the check looks at that share type only. The collection manager writes the collection file itself, outside the undo history, and a creation it refuses is an error carrying its reason. exists is read back afterwards. Fill it with add_asset_to_collection, list collections with get_collections and members with get_collection_assets; the inverse is delete_collection. A Material Parameter Collection is a different asset: create_material_parameter_collection.
//   @param collection_name - Name of the collection to create.
//   @param share_type - ECollectionShareType: "Local", "Private", or "Shared" (optional, default "Local"); an unrecognised value falls back to Local
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

// MCP:delete_collection - Deletes a Content Browser collection of the project, looked up by name within share_type; only the collection goes, the assets it lists are not touched. The collection manager writes the change to its collection file itself, outside the undo history, so it cannot be undone. Idempotent: a name not found in that share type succeeds with already_absent true and nothing changes, so pass the share type the collection was created with (a wrong one reads as absent); get_collections lists names and share types. A collection that exists but that the manager fails to delete is an error. Inverse of create_collection; to take single assets out instead, use remove_asset_from_collection, and get_collection_assets to see the members first.
//   @param collection_name - Name of the collection to delete, as get_collections lists it (e.g. "Props_Review").
//   @param share_type - ECollectionShareType the collection was created with: "Local", "Private", or "Shared" (optional, default "Local"); an unrecognised value falls back to Local.
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

// MCP:add_asset_to_collection - Add one asset, by object path, to an existing static Content Browser collection of the project and read the membership back. The collection is looked up by collection_name within share_type (default Local; an unrecognised value also means Local), which must be the share type it was created with: a name not found there is an error. The asset itself is not loaded or checked. An asset that is already a member makes the collection manager refuse the add, which comes back as an error with its reason. The manager writes the collection file itself, outside the undo history. verified confirms membership and object_count counts the direct members. Create the collection with create_collection; the inverse is remove_asset_from_collection; list members with get_collection_assets and collections with get_collections.
//   @param collection_name - Name of the (existing) collection.
//   @param asset_path - Object path of the asset to add, e.g. "/Game/Props/SM_Chair.SM_Chair"; used as given, without loading or checking the asset.
//   @param share_type - ECollectionShareType: "Local", "Private", or "Shared" (optional, default "Local"); an unrecognised value falls back to Local
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

// MCP:get_collection_assets - List the object paths of the assets directly in one Content Browser collection (child collections are not included), without changing anything. The collection is looked up by collection_name within share_type, default Local; an unrecognized share_type also falls back to Local, and a name not found under that share type is an error. get_collections lists every collection with its share_type. Returns assets and asset_count; change membership with add_asset_to_collection or remove_asset_from_collection.
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
    { TEXT("remove_asset_metadata_tag"), &HandleRemoveAssetMetadataTag },
};
} // namespace UEAgentAssetMetaCommands
