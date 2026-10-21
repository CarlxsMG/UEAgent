// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

#include "Engine/AssetManager.h"
#include "Engine/AssetManagerSettings.h"
#include "Engine/AssetManagerTypes.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetBundleData.h"
#include "AssetRegistry/IAssetRegistry.h"

#include "UObject/Class.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/PrimaryAssetId.h"
#include "UObject/TopLevelAssetPath.h"

/**
 * Asset Manager commands.
 *
 * Reads + mutates the project's Asset Management framework configuration and
 * runtime directory, all headless-safe:
 *   - UAssetManagerSettings (UDeveloperSettings, config=Game) holds the
 *     PrimaryAssetTypesToScan list. We mutate the mutable CDO and persist via
 *     TryUpdateDefaultConfigFile() (writes DefaultGame.ini), then read back.
 *   - UAssetManager::Get() is the runtime singleton. GetPrimaryAssetIdForPath,
 *     GetPrimaryAssetData, GetPrimaryAssetTypeInfo are real reads;
 *     ScanPathsForPrimaryAssets / RefreshPrimaryAssetDirectory build the
 *     directory synchronously in the editor.
 *   - Asset bundle data is read straight off the asset registry's FAssetData
 *     (TaggedAssetBundles), which is the AssetBundles tag for the object.
 */
namespace UEAgentAssetManagerCommands
{

// MCP-DOMAIN: Management

namespace
{
    /** Strip a trailing slash from a content folder path. */
    FString AMNormalizeFolder(const FString& In)
    {
        FString Out = In;
        Out.RemoveFromEnd(TEXT("/"));
        return Out;
    }

    /** The directories a primary asset type scans, as JSON strings. */
    TArray<TSharedPtr<FJsonValue>> AssetManager_DirectoriesToJson(const FPrimaryAssetTypeInfo& Info)
    {
        TArray<TSharedPtr<FJsonValue>> Dirs;
        // AsConst selects the read-only GetDirectories() overload; the non-const one
        // ensure()s CanModifyConfigData(), false once Info holds runtime data.
        for (const FDirectoryPath& Dir : AsConst(Info).GetDirectories())
        {
            Dirs.Add(MakeShared<FJsonValueString>(Dir.Path));
        }
        return Dirs;
    }

    /** The short name of a stored cook rule, as get_primary_asset_types and set_primary_asset_cook_rule report it. */
    FString CookRuleToString(EPrimaryAssetCookRule In)
    {
        // Return user-friendly aliases (not the raw enum names) so callers
        // can pass them back to set_primary_asset_cook_rule.
        switch (In)
        {
        case EPrimaryAssetCookRule::Unknown:                                       return TEXT("Unknown");
        case EPrimaryAssetCookRule::NeverCook:                                     return TEXT("Never");
        case EPrimaryAssetCookRule::ProductionNeverCook:                           return TEXT("ProductionNeverCook");
        case EPrimaryAssetCookRule::DevelopmentAlwaysProductionNeverCook:          return TEXT("DevelopmentAlways");
        case EPrimaryAssetCookRule::DevelopmentAlwaysProductionUnknownCook:       return TEXT("DevelopmentAlwaysProductionUnknownCook");
        case EPrimaryAssetCookRule::AlwaysCook:                                    return TEXT("Always");
        default:                                                                    return TEXT("Unknown");
        }
    }

    /** Build the JSON view of a single FPrimaryAssetTypeInfo config entry, its cook rules included. */
    TSharedPtr<FJsonObject> PrimaryAssetTypeInfoToJson(const FPrimaryAssetTypeInfo& Info)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("type"), Info.PrimaryAssetType.ToString());
        Obj->SetStringField(TEXT("base_class"), Info.GetAssetBaseClass().ToString());
        Obj->SetBoolField(TEXT("has_blueprint_classes"), Info.bHasBlueprintClasses);
        Obj->SetBoolField(TEXT("is_editor_only"), Info.bIsEditorOnly);
        Obj->SetArrayField(TEXT("directories"), AssetManager_DirectoriesToJson(Info));
        Obj->SetNumberField(TEXT("num_specific_assets"), Info.GetSpecificAssets().Num());
        Obj->SetStringField(TEXT("cook_rule"), CookRuleToString(Info.Rules.CookRule));
        Obj->SetNumberField(TEXT("priority"), Info.Rules.Priority);
        Obj->SetNumberField(TEXT("chunk_id"), Info.Rules.ChunkId);
        Obj->SetBoolField(TEXT("apply_recursively"), Info.Rules.bApplyRecursively);
        return Obj;
    }
}

// ---------------------------------------------------------------------------
// add_primary_asset_rule
//   Mutate UAssetManagerSettings::PrimaryAssetTypesToScan (update-or-add the
//   entry for the given type), persist to DefaultGame.ini, then read back.
// ---------------------------------------------------------------------------
// MCP:add_primary_asset_rule - Add a PrimaryAssetTypesToScan rule to the Asset Manager project settings, or update the rule of that type, then write DefaultGame.ini (no undo); a failed write makes success false. An update always resets the base class (to UObject when base_class_path is omitted), appends directory unless already listed and sets priority only when given; idempotent. Nothing is rescanned: run scan_primary_assets so the running Asset Manager registers the assets. Cook rules: set_primary_asset_cook_rule; inverse: remove_primary_asset_rule; list rules: get_primary_asset_types.
//   @param primary_asset_type - Logical type name, e.g. "Item"
//   @param base_class_path - Asset base class path (defaults to UObject)
//   @param directory - Content directory to scan for this type
//   @param priority - Scan rule priority (optional)
//   @returns { type, base_class, saved, updated_existing, directory_count, type_count }
TSharedPtr<FJsonObject> HandleAddPrimaryAssetRule(const TSharedPtr<FJsonObject>& Params)
{
    FString PrimaryAssetType;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("primary_asset_type"), PrimaryAssetType)) { return Missing; }

    FString BaseClassPath;
    if (!Params->TryGetStringField(TEXT("base_class_path"), BaseClassPath) || BaseClassPath.IsEmpty())
    {
        // Default to UObject if none specified.
        BaseClassPath = TEXT("/Script/CoreUObject.Object");
    }

    FString Directory;
    Params->TryGetStringField(TEXT("directory"), Directory);

    double PriorityNum = 0.0;
    const bool bHasPriority = Params->TryGetNumberField(TEXT("priority"), PriorityNum);

    UAssetManagerSettings* Settings = GetMutableDefault<UAssetManagerSettings>();
    if (!Settings)
        return mcp_common::MakeErrorReply(TEXT("Could not get mutable UAssetManagerSettings"));

    const FName TypeName(*PrimaryAssetType);
    const FSoftObjectPath BaseClassObjectPath(BaseClassPath);
    const TSoftClassPtr<UObject> BaseClassPtr(BaseClassObjectPath);

    // Find an existing entry for this type (update in place to keep idempotent).
    FPrimaryAssetTypeInfo* Existing = nullptr;
    for (FPrimaryAssetTypeInfo& Entry : Settings->PrimaryAssetTypesToScan)
    {
        if (Entry.PrimaryAssetType == TypeName)
        {
            Existing = &Entry;
            break;
        }
    }

    bool bUpdated = false;
    if (Existing)
    {
        Existing->SetAssetBaseClass(BaseClassPtr);
        if (!Directory.IsEmpty())
        {
            TArray<FDirectoryPath>& Dirs = Existing->GetDirectories();
            const bool bDirPresent = Dirs.ContainsByPredicate(
                [&Directory](const FDirectoryPath& D) { return D.Path == Directory; });
            if (!bDirPresent)
            {
                FDirectoryPath DirPath;
                DirPath.Path = Directory;
                Dirs.Add(DirPath);
            }
        }
        if (bHasPriority)
        {
            Existing->Rules.Priority = static_cast<int32>(PriorityNum);
        }
        bUpdated = true;
    }
    else
    {
        FPrimaryAssetTypeInfo NewInfo;
        NewInfo.PrimaryAssetType = TypeName;
        NewInfo.SetAssetBaseClass(BaseClassPtr);
        NewInfo.bHasBlueprintClasses = false;
        NewInfo.bIsEditorOnly = false;
        if (!Directory.IsEmpty())
        {
            FDirectoryPath DirPath;
            DirPath.Path = Directory;
            NewInfo.GetDirectories().Add(DirPath);
        }
        if (bHasPriority)
        {
            NewInfo.Rules.Priority = static_cast<int32>(PriorityNum);
        }
        Settings->PrimaryAssetTypesToScan.Add(NewInfo);
    }

    // Persist to DefaultGame.ini (UDeveloperSettings -> config=Game, defaultconfig).
    const bool bSaved = Settings->TryUpdateDefaultConfigFile();

    // Read back from the (now-updated) settings array for honest verification.
    FString SavedBaseClass;
    int32 SavedDirCount = 0;
    for (const FPrimaryAssetTypeInfo& Entry : Settings->PrimaryAssetTypesToScan)
    {
        if (Entry.PrimaryAssetType == TypeName)
        {
            SavedBaseClass = Entry.GetAssetBaseClass().ToString();
            SavedDirCount = Entry.GetDirectories().Num();
            break;
        }
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("type"), PrimaryAssetType);
    Resp->SetStringField(TEXT("base_class"), SavedBaseClass.IsEmpty() ? BaseClassPath : SavedBaseClass);
    Resp->SetBoolField(TEXT("updated_existing"), bUpdated);
    Resp->SetNumberField(TEXT("directory_count"), SavedDirCount);
    Resp->SetNumberField(TEXT("type_count"), Settings->PrimaryAssetTypesToScan.Num());
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("add_primary_asset_rule"));
}

// ---------------------------------------------------------------------------
// get_primary_asset_types
//   Read UAssetManagerSettings::PrimaryAssetTypesToScan -> types[] + count,
//   optionally only the entries of one type.
// ---------------------------------------------------------------------------
// MCP:get_primary_asset_types - Read the PrimaryAssetTypesToScan rules configured in the Asset Manager project settings, read-only: each entry with its type, base_class, has_blueprint_classes, is_editor_only, directories, num_specific_assets and its cook rules (cook_rule, priority, chunk_id, apply_recursively), optionally only the entries of one primary_asset_type. The running Asset Manager is not consulted, so a type only registered by game code is not listed: get_all_primary_asset_types reads the live registry. cook_rule comes back as Unknown, Never, ProductionNeverCook, DevelopmentAlways, DevelopmentAlwaysProductionUnknownCook or Always, each accepted back by set_primary_asset_cook_rule.
//   @param primary_asset_type - Optional type name (e.g. "Item", matched without case): list only the rules of that type; a type with no configured rule is an error.
//   @returns { types: [{ type, base_class, has_blueprint_classes, is_editor_only, directories: [string], num_specific_assets, cook_rule, priority, chunk_id, apply_recursively }], type_count, primary_asset_type? }
TSharedPtr<FJsonObject> HandleGetPrimaryAssetTypes(const TSharedPtr<FJsonObject>& Params)
{
    FString FilterType;
    Params->TryGetStringField(TEXT("primary_asset_type"), FilterType);
    const bool bHasFilter = !FilterType.IsEmpty();
    const FName FilterTypeName(*FilterType);

    const UAssetManagerSettings* Settings = GetDefault<UAssetManagerSettings>();
    if (!Settings)
        return mcp_common::MakeErrorReply(TEXT("Could not get UAssetManagerSettings"));

    TArray<TSharedPtr<FJsonValue>> Types;
    for (const FPrimaryAssetTypeInfo& Info : Settings->PrimaryAssetTypesToScan)
    {
        if (bHasFilter && Info.PrimaryAssetType != FilterTypeName)
        {
            continue;
        }
        Types.Add(MakeShared<FJsonValueObject>(PrimaryAssetTypeInfoToJson(Info)));
    }
    if (bHasFilter && Types.Num() == 0)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Primary asset type '%s' not found among the configured PrimaryAssetTypesToScan rules"), *FilterType));
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetArrayField(TEXT("types"), Types);
    // The entries listed: every configured rule, or the rules of primary_asset_type.
    Resp->SetNumberField(TEXT("type_count"), Types.Num());
    if (bHasFilter)
    {
        Resp->SetStringField(TEXT("primary_asset_type"), FilterType);
    }
    return mcp_common::MakeOkReply(Resp);
}

// ---------------------------------------------------------------------------
// get_primary_asset_id
//   UAssetManager::Get().GetPrimaryAssetIdForPath(FSoftObjectPath(path)).
//   Invalid is a real, correct answer for a non-primary asset.
// ---------------------------------------------------------------------------
// MCP:get_primary_asset_id - Look up the registered Primary Asset Id ("Type:Name") of an asset path: primary_asset_id, is_valid and, when valid, primary_asset_type and primary_asset_name. A non-primary or unscanned asset gives is_valid=false, not an error; an uninitialized Asset Manager is an error. All ids of one type: get_primary_asset_ids_by_type; to make a type scannable, add_primary_asset_rule then scan_primary_assets.
//   @param asset_path - Full object path including the object name, e.g. /Game/MyGame/M_Example.M_Example; a package-only path does not match.
//   @returns { asset_path, primary_asset_id, is_valid, primary_asset_type, primary_asset_name }
TSharedPtr<FJsonObject> HandleGetPrimaryAssetId(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_path"), AssetPath)) { return Missing; }

    if (!UAssetManager::IsInitialized())
        return mcp_common::MakeErrorReply(TEXT("AssetManager singleton is not initialized"));

    UAssetManager& Manager = UAssetManager::Get();
    const FSoftObjectPath ObjectPath(AssetPath);
    const FPrimaryAssetId PrimaryId = Manager.GetPrimaryAssetIdForPath(ObjectPath);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("asset_path"), AssetPath);
    Resp->SetStringField(TEXT("primary_asset_id"), PrimaryId.ToString());
    Resp->SetBoolField(TEXT("is_valid"), PrimaryId.IsValid());
    if (PrimaryId.IsValid())
    {
        Resp->SetStringField(TEXT("primary_asset_type"), PrimaryId.PrimaryAssetType.ToString());
        Resp->SetStringField(TEXT("primary_asset_name"), PrimaryId.PrimaryAssetName.ToString());
    }
    return mcp_common::MakeOkReply(Resp);
}

// ---------------------------------------------------------------------------
// get_asset_bundle_data
//   Read the AssetBundles tag (FAssetData::TaggedAssetBundles) for an asset.
//   An asset with no bundles -> empty bundles[] is the real, correct result.
// ---------------------------------------------------------------------------
// MCP:get_asset_bundle_data - Read the AssetBundles tag of one asset from the Asset Registry without loading it: each bundle with bundle_name, asset_paths and num_assets, plus bundle_count and asset_found. An asset with no bundles, or not found, returns an empty list rather than an error. Read-only.
//   @param asset_path - Full object path including the object name, e.g. /Game/MyGame/M_Example.M_Example; a package-only path does not resolve to the asset.
//   @returns { bundle_name, asset_paths, num_assets, asset_path, asset_found, bundles, bundle_count }
TSharedPtr<FJsonObject> HandleGetAssetBundleData(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_path"), AssetPath)) { return Missing; }

    if (!UAssetManager::IsInitialized())
        return mcp_common::MakeErrorReply(TEXT("AssetManager singleton is not initialized"));

    UAssetManager& Manager = UAssetManager::Get();
    IAssetRegistry& AssetRegistry = Manager.GetAssetRegistry();

    const FSoftObjectPath ObjectPath(AssetPath);
    const FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(ObjectPath, /*bIncludeOnlyOnDiskAssets*/ false);

    TArray<TSharedPtr<FJsonValue>> Bundles;
    if (AssetData.TaggedAssetBundles.IsValid())
    {
        for (const FAssetBundleEntry& Entry : AssetData.TaggedAssetBundles->Bundles)
        {
            TSharedPtr<FJsonObject> BundleObj = MakeShared<FJsonObject>();
            BundleObj->SetStringField(TEXT("bundle_name"), Entry.BundleName.ToString());

            TArray<TSharedPtr<FJsonValue>> AssetPaths;
            for (const FTopLevelAssetPath& Path : Entry.AssetPaths)
            {
                AssetPaths.Add(MakeShared<FJsonValueString>(Path.ToString()));
            }
            BundleObj->SetArrayField(TEXT("asset_paths"), AssetPaths);
            BundleObj->SetNumberField(TEXT("num_assets"), Entry.AssetPaths.Num());
            Bundles.Add(MakeShared<FJsonValueObject>(BundleObj));
        }
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("asset_path"), AssetPath);
    Resp->SetBoolField(TEXT("asset_found"), AssetData.IsValid());
    Resp->SetArrayField(TEXT("bundles"), Bundles);
    Resp->SetNumberField(TEXT("bundle_count"), Bundles.Num());
    return mcp_common::MakeOkReply(Resp);
}

// ---------------------------------------------------------------------------
// scan_primary_assets
//   Scan the configured types' directories (or one named type) synchronously.
//   A fresh project may legitimately find 0 assets -> 0 is real.
// ---------------------------------------------------------------------------
// MCP:scan_primary_assets - Synchronously rescan the directories and specific assets of every PrimaryAssetTypesToScan rule (or only primary_asset_type) so the Asset Manager registers their primary assets; rules without paths are skipped. Returns num_assets_found, types_scanned and per-type scanned_types. Run it after add_primary_asset_rule.
//   @param primary_asset_type - Configured type name to scan alone, e.g. "Item"; omitted or empty scans every rule, and an unknown name scans nothing (types_scanned=0).
//   @returns { type, num_assets_found, num_paths, scanned, types_scanned, scanned_types }
TSharedPtr<FJsonObject> HandleScanPrimaryAssets(const TSharedPtr<FJsonObject>& Params)
{
    if (!UAssetManager::IsInitialized())
        return mcp_common::MakeErrorReply(TEXT("AssetManager singleton is not initialized"));

    UAssetManager& Manager = UAssetManager::Get();
    const UAssetManagerSettings* Settings = GetDefault<UAssetManagerSettings>();
    if (!Settings)
        return mcp_common::MakeErrorReply(TEXT("Could not get UAssetManagerSettings"));

    FString FilterType;
    Params->TryGetStringField(TEXT("primary_asset_type"), FilterType);
    const FName FilterTypeName(*FilterType);
    const bool bHasFilter = !FilterType.IsEmpty();

    int32 NumFound = 0;
    int32 TypesScanned = 0;
    TArray<TSharedPtr<FJsonValue>> ScannedTypes;

    for (const FPrimaryAssetTypeInfo& Info : Settings->PrimaryAssetTypesToScan)
    {
        if (bHasFilter && Info.PrimaryAssetType != FilterTypeName)
        {
            continue;
        }

        // Build the scan paths from the configured directories.
        TArray<FString> Paths;
        // AsConst selects the read-only GetDirectories() overload; the non-const one
        // ensure()s CanModifyConfigData(), false once Info holds runtime data.
        for (const FDirectoryPath& Dir : AsConst(Info).GetDirectories())
        {
            if (!Dir.Path.IsEmpty())
            {
                Paths.Add(AMNormalizeFolder(Dir.Path));
            }
        }
        for (const FSoftObjectPath& Specific : Info.GetSpecificAssets())
        {
            Paths.Add(Specific.ToString());
        }

        if (Paths.Num() == 0)
        {
            continue;
        }

        UClass* BaseClass = Info.GetAssetBaseClass().LoadSynchronous();
        if (!BaseClass)
        {
            BaseClass = UObject::StaticClass();
        }

        const int32 Found = Manager.ScanPathsForPrimaryAssets(
            Info.PrimaryAssetType, Paths, BaseClass,
            Info.bHasBlueprintClasses, Info.bIsEditorOnly, /*bForceSynchronousScan*/ true);
        NumFound += Found;
        ++TypesScanned;

        TSharedPtr<FJsonObject> TypeObj = MakeShared<FJsonObject>();
        TypeObj->SetStringField(TEXT("type"), Info.PrimaryAssetType.ToString());
        TypeObj->SetNumberField(TEXT("num_assets_found"), Found);
        TypeObj->SetNumberField(TEXT("num_paths"), Paths.Num());
        ScannedTypes.Add(MakeShared<FJsonValueObject>(TypeObj));
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("scanned"), true);
    Resp->SetNumberField(TEXT("types_scanned"), TypesScanned);
    Resp->SetNumberField(TEXT("num_assets_found"), NumFound);
    Resp->SetArrayField(TEXT("scanned_types"), ScannedTypes);
    return mcp_common::MakeOkReply(Resp);
}

// ---------------------------------------------------------------------------
// Asset Manager advanced commands

// ---------------------------------------------------------------------------
// get_primary_asset_ids_by_type
//   UAssetManager::Get().GetPrimaryAssetIdList(Type, Ids, Filter).
//   Returns all primary asset IDs of a given type.
// ---------------------------------------------------------------------------
// MCP:get_primary_asset_ids_by_type - List the Primary Asset Ids the running Asset Manager has registered for one type: asset_ids entries with type, name and full_id, plus count; success is false when none are found, unknown type included. Only scanned assets appear (see scan_primary_assets). Read-only.
//   @param asset_type_name - Name of the primary asset type, e.g. "Item".
//   @param filter - "unloaded_only" keeps only assets with no current or pending bundle state (not loaded through the Asset Manager); omitted or any other value lists all.
//   @returns { type, name, full_id, asset_type_name, success, asset_ids, count }
TSharedPtr<FJsonObject> HandleGetPrimaryAssetIdsByType(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetTypeName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_type_name"), AssetTypeName)) { return Missing; }

    if (!UAssetManager::IsInitialized())
        return mcp_common::MakeErrorReply(TEXT("AssetManager singleton is not initialized"));

    FString FilterStr;
    Params->TryGetStringField(TEXT("filter"), FilterStr);
    
    EAssetManagerFilter Filter = EAssetManagerFilter::Default;
    if (FilterStr == TEXT("unloaded_only"))
    {
        Filter = EAssetManagerFilter::UnloadedOnly;
    }

    UAssetManager& Manager = UAssetManager::Get();
    const FPrimaryAssetType TypeName(*AssetTypeName);
    
    TArray<FPrimaryAssetId> IdList;
    const bool bSuccess = Manager.GetPrimaryAssetIdList(TypeName, IdList, Filter);

    TArray<TSharedPtr<FJsonValue>> AssetIds;
    for (const FPrimaryAssetId& Id : IdList)
    {
        TSharedPtr<FJsonObject> IdObj = MakeShared<FJsonObject>();
        IdObj->SetStringField(TEXT("type"), Id.PrimaryAssetType.ToString());
        IdObj->SetStringField(TEXT("name"), Id.PrimaryAssetName.ToString());
        IdObj->SetStringField(TEXT("full_id"), Id.ToString());
        AssetIds.Add(MakeShared<FJsonValueObject>(IdObj));
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("asset_type_name"), AssetTypeName);
    Resp->SetBoolField(TEXT("success"), bSuccess);
    Resp->SetArrayField(TEXT("asset_ids"), AssetIds);
    Resp->SetNumberField(TEXT("count"), IdList.Num());
    return mcp_common::MakeOkReply(Resp);
}

// ---------------------------------------------------------------------------
// get_asset_bundle_entry
//   UAssetManager::Get().GetAssetBundleEntry(PrimaryAssetId, BundleName).
//   Returns the bundle entry for a specific primary asset and bundle name.
// ---------------------------------------------------------------------------
// MCP:get_asset_bundle_entry - Read one named asset bundle of a primary asset from the running Asset Manager's bundle cache (UAssetManager::GetAssetBundleEntry), loading nothing: bundle_objects lists the asset paths in it. Only scanned primary assets are cached, so an unscanned asset, an unknown bundle, a malformed id or an empty bundle all give found=false with an empty list, not an error. For every bundle of one asset by object path, read from the Asset Registry, use get_asset_bundle_data; get_primary_asset_id turns a path into the id.
//   @param primary_asset_id - Primary asset ID in "Type:Name" format
//   @param bundle_name - Name of the asset bundle
//   @returns { primary_asset_id, bundle_name, bundle_objects, found }
TSharedPtr<FJsonObject> HandleGetAssetBundleEntry(const TSharedPtr<FJsonObject>& Params)
{
    FString PrimaryAssetIdStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("primary_asset_id"), PrimaryAssetIdStr)) { return Missing; }

    FString BundleNameStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("bundle_name"), BundleNameStr)) { return Missing; }

    if (!UAssetManager::IsInitialized())
        return mcp_common::MakeErrorReply(TEXT("AssetManager singleton is not initialized"));

    UAssetManager& Manager = UAssetManager::Get();
    const FPrimaryAssetId PrimaryId(PrimaryAssetIdStr);
    const FName BundleName(*BundleNameStr);
    
    const FAssetBundleEntry Entry = Manager.GetAssetBundleEntry(PrimaryId, BundleName);
    
    const bool bFound = !Entry.BundleName.IsNone() && Entry.AssetPaths.Num() > 0;

    TArray<TSharedPtr<FJsonValue>> BundleObjects;
    for (const FTopLevelAssetPath& Path : Entry.AssetPaths)
    {
        BundleObjects.Add(MakeShared<FJsonValueString>(Path.ToString()));
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("primary_asset_id"), PrimaryAssetIdStr);
    Resp->SetStringField(TEXT("bundle_name"), BundleNameStr);
    Resp->SetArrayField(TEXT("bundle_objects"), BundleObjects);
    Resp->SetBoolField(TEXT("found"), bFound);
    return mcp_common::MakeOkReply(Resp);
}

// ---------------------------------------------------------------------------
// get_all_primary_asset_types
//   UAssetManager::Get().GetPrimaryAssetTypeInfoList(InfoList).
//   Returns all registered primary asset types.
// ---------------------------------------------------------------------------
// MCP:get_all_primary_asset_types - List the primary asset types the running Asset Manager has registered, read-only: each entry of types holds the type name, its asset_class (base class path), num_assets from the last scan, has_blueprint_classes, is_editor_only and its directories, and count says how many entries there are. asset_type_name narrows it to one type, with found false (and an empty list) for a type the manager does not know, not an error. This is the live registry, not the project settings, so it includes types registered by game code and its counts cover scanned assets only (rescan with scan_primary_assets); the configured PrimaryAssetTypesToScan rules with their cook rules are get_primary_asset_types. An uninitialized Asset Manager is an error. A type's asset ids: get_primary_asset_ids_by_type.
//   @param asset_type_name - Optional name of one primary asset type (e.g. "Item") to read alone; omitted lists every registered type.
//   @returns { types: [{ name, asset_class, num_assets, has_blueprint_classes, is_editor_only, directories: [string] }], count, asset_type_name?, found? }
TSharedPtr<FJsonObject> HandleGetAllPrimaryAssetTypes(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetTypeName;
    Params->TryGetStringField(TEXT("asset_type_name"), AssetTypeName);
    const bool bHasFilter = !AssetTypeName.IsEmpty();

    if (!UAssetManager::IsInitialized())
        return mcp_common::MakeErrorReply(TEXT("AssetManager singleton is not initialized"));

    UAssetManager& Manager = UAssetManager::Get();

    TArray<FPrimaryAssetTypeInfo> InfoList;
    bool bFound = false;
    if (bHasFilter)
    {
        FPrimaryAssetTypeInfo Info;
        bFound = Manager.GetPrimaryAssetTypeInfo(FPrimaryAssetType(*AssetTypeName), Info);
        if (bFound)
        {
            InfoList.Add(Info);
        }
    }
    else
    {
        Manager.GetPrimaryAssetTypeInfoList(InfoList);
    }

    TArray<TSharedPtr<FJsonValue>> Types;
    for (const FPrimaryAssetTypeInfo& Info : InfoList)
    {
        TSharedPtr<FJsonObject> TypeObj = MakeShared<FJsonObject>();
        TypeObj->SetStringField(TEXT("name"), Info.PrimaryAssetType.ToString());
        TypeObj->SetStringField(TEXT("asset_class"), Info.GetAssetBaseClass().ToString());
        TypeObj->SetNumberField(TEXT("num_assets"), Info.NumberOfAssets);
        TypeObj->SetBoolField(TEXT("has_blueprint_classes"), Info.bHasBlueprintClasses);
        TypeObj->SetBoolField(TEXT("is_editor_only"), Info.bIsEditorOnly);
        TypeObj->SetArrayField(TEXT("directories"), AssetManager_DirectoriesToJson(Info));
        Types.Add(MakeShared<FJsonValueObject>(TypeObj));
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetArrayField(TEXT("types"), Types);
    Resp->SetNumberField(TEXT("count"), InfoList.Num());
    if (bHasFilter)
    {
        Resp->SetStringField(TEXT("asset_type_name"), AssetTypeName);
        Resp->SetBoolField(TEXT("found"), bFound);
    }
    return mcp_common::MakeOkReply(Resp);
}

// ===========================================================================
// Asset Manager cook rules + per-rule mutators, all headless. They mutate the
// persisted UAssetManagerSettings::PrimaryAssetTypesToScan array; get_primary_asset_types reads it.
//   1. set_primary_asset_cook_rule -- set CookRule (and optional ChunkId /
//      bApplyRecursively) on an existing type entry.
//   2. remove_primary_asset_rule -- drop a type entry from the persisted
//      PrimaryAssetTypesToScan (writes DefaultGame.ini).
// Engine APIs used:
//   FPrimaryAssetTypeInfo::Rules                      AssetManagerTypes.h
//   FPrimaryAssetRules::Priority/etc.                 AssetManagerTypes.h
//   UDeveloperSettings::TryUpdateDefaultConfigFile()  DeveloperSettings.h
// ===========================================================================

namespace
{
    // Map a user-friendly CookRule name to the real EPrimaryAssetCookRule
    // enum value (5.7: Unknown | NeverCook | ProductionNeverCook |
    // DevelopmentAlwaysProductionNeverCook | DevelopmentAlwaysProductionUnknownCook |
    // AlwaysCook). Returns true on hit.
    bool ParseCookRuleName(const FString& In, EPrimaryAssetCookRule& Out)
    {
        const FString L = In.ToLower();
        if (L == TEXT("unknown"))                                                 { Out = EPrimaryAssetCookRule::Unknown;   return true; }
        if (L == TEXT("never")     || L == TEXT("nevercook"))                     { Out = EPrimaryAssetCookRule::NeverCook; return true; }
        if (L == TEXT("development") || L == TEXT("developmentcook") || L == TEXT("productionnevercook"))
                                                                                   { Out = EPrimaryAssetCookRule::ProductionNeverCook; return true; }
        if (L == TEXT("developmentalways") || L == TEXT("developmentalwayscook")
         || L == TEXT("developmentalwaysproductionnevercook"))
                                                                                   { Out = EPrimaryAssetCookRule::DevelopmentAlwaysProductionNeverCook; return true; }
        if (L == TEXT("developmentalwaysproductionunknowncook"))                  { Out = EPrimaryAssetCookRule::DevelopmentAlwaysProductionUnknownCook; return true; }
        if (L == TEXT("always")     || L == TEXT("alwayscook"))                   { Out = EPrimaryAssetCookRule::AlwaysCook; return true; }
        return false;
    }

    /** The cook rules of PrimaryAssetType as set_primary_asset_cook_rule reports them. */
    TSharedPtr<FJsonObject> AssetManager_CookRulesReply(const FString& PrimaryAssetType, const FPrimaryAssetRules& Rules)
    {
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetStringField(TEXT("primary_asset_type"), PrimaryAssetType);
        Resp->SetStringField(TEXT("cook_rule"), CookRuleToString(Rules.CookRule));
        Resp->SetNumberField(TEXT("priority"), Rules.Priority);
        Resp->SetNumberField(TEXT("chunk_id"), Rules.ChunkId);
        Resp->SetBoolField(TEXT("apply_recursively"), Rules.bApplyRecursively);
        return Resp;
    }
}

// MCP:set_primary_asset_cook_rule - Set the CookRule of an existing PrimaryAssetTypesToScan rule in the Asset Manager project settings, plus ChunkId and bApplyRecursively when given, then write DefaultGame.ini (no undo); a failed write makes success false. An unknown type (add it with add_primary_asset_rule) or cook_rule is an error before any change. Priority is not touched here; add_primary_asset_rule sets it. Nothing is rescanned. Read the rules back with get_primary_asset_types (primary_asset_type); the reply's cook_rule is the short name of the stored rule.
//   @param primary_asset_type - Name of the primary asset type (e.g. "Item").
//   @param cook_rule - One of "Unknown" / "Never" / "Development" / "DevelopmentAlways" / "Always", case-insensitive; "Development" stores ProductionNeverCook, and the EPrimaryAssetCookRule names (e.g. "AlwaysCook", "DevelopmentAlwaysProductionUnknownCook") are accepted too.
//   @param chunk_id - Optional chunk ID override for the rule
//   @param apply_recursively - Optional flag to apply the rule recursively
//   @returns { primary_asset_type, cook_rule, priority, chunk_id, apply_recursively, saved }
TSharedPtr<FJsonObject> HandleSetPrimaryAssetCookRule(const TSharedPtr<FJsonObject>& Params)
{
    FString PrimaryAssetType, CookRuleStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("primary_asset_type"), PrimaryAssetType)) { return Missing; }
    if (!Params->TryGetStringField(TEXT("cook_rule"), CookRuleStr) || CookRuleStr.IsEmpty())
    {
        return mcp_common::MakeErrorReply(
            TEXT("Missing required parameter: cook_rule (Unknown|Never|Development|DevelopmentAlways|Always)"));
    }
    EPrimaryAssetCookRule NewRule;
    if (!ParseCookRuleName(CookRuleStr, NewRule))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Invalid cook_rule '%s' (use Unknown|Never|Development|DevelopmentAlways|Always, or an EPrimaryAssetCookRule name such as AlwaysCook)"), *CookRuleStr));
    }

    UAssetManagerSettings* Settings = GetMutableDefault<UAssetManagerSettings>();
    if (!Settings)
    {
        return mcp_common::MakeErrorReply(TEXT("Could not get UAssetManagerSettings"));
    }

    const FName TypeName(*PrimaryAssetType);
    FPrimaryAssetTypeInfo* Existing = Settings->PrimaryAssetTypesToScan.FindByPredicate(
        [&TypeName](const FPrimaryAssetTypeInfo& I) { return I.PrimaryAssetType == TypeName; });
    if (!Existing)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Primary asset type '%s' not found (use add_primary_asset_rule first)"), *PrimaryAssetType));
    }

    // Optional ChunkId / bApplyRecursively overrides (only when caller passes them).
    int32 NewChunkId = -1;
    const bool bHasChunk = Params->TryGetNumberField(TEXT("chunk_id"), NewChunkId);
    bool bApplyRec = Existing->Rules.bApplyRecursively;
    const bool bHasApply = Params->TryGetBoolField(TEXT("apply_recursively"), bApplyRec);

    Settings->Modify();
    Existing->Rules.CookRule = NewRule;
    if (bHasChunk) { Existing->Rules.ChunkId = NewChunkId; }
    if (bHasApply) { Existing->Rules.bApplyRecursively = bApplyRec; }
    const bool bSaved = Settings->TryUpdateDefaultConfigFile();

    return mcp_common::MakeSavedReply(AssetManager_CookRulesReply(PrimaryAssetType, Existing->Rules), bSaved,
        TEXT("set_primary_asset_cook_rule"));
}

// MCP:remove_primary_asset_rule - Remove every PrimaryAssetTypesToScan entry of one type from the Asset Manager project settings and write DefaultGame.ini; an unknown type is an error. Returns removed_count, type_count_after and saved. The running Asset Manager is not refreshed, so its scanned data for the type remains.
//   @param primary_asset_type - Name of the primary asset type to remove, e.g. "Item".
//   @returns { primary_asset_type, removed_count, type_count_after, saved }
TSharedPtr<FJsonObject> HandleRemovePrimaryAssetRule(const TSharedPtr<FJsonObject>& Params)
{
    FString PrimaryAssetType;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("primary_asset_type"), PrimaryAssetType)) { return Missing; }
    UAssetManagerSettings* Settings = GetMutableDefault<UAssetManagerSettings>();
    if (!Settings)
    {
        return mcp_common::MakeErrorReply(TEXT("Could not get UAssetManagerSettings"));
    }
    const FName TypeName(*PrimaryAssetType);
    const int32 RemovedCount = Settings->PrimaryAssetTypesToScan.RemoveAll(
        [&TypeName](const FPrimaryAssetTypeInfo& I) { return I.PrimaryAssetType == TypeName; });
    if (RemovedCount == 0)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Primary asset type '%s' not found (nothing to remove)"), *PrimaryAssetType));
    }
    Settings->Modify();
    const bool bSaved = Settings->TryUpdateDefaultConfigFile();

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("primary_asset_type"), PrimaryAssetType);
    Resp->SetNumberField(TEXT("removed_count"), RemovedCount);
    Resp->SetNumberField(TEXT("type_count_after"), Settings->PrimaryAssetTypesToScan.Num());
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("remove_primary_asset_rule"));
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_primary_asset_rule"), &HandleAddPrimaryAssetRule },
    { TEXT("get_all_primary_asset_types"), &HandleGetAllPrimaryAssetTypes },
    { TEXT("get_asset_bundle_data"), &HandleGetAssetBundleData },
    { TEXT("get_asset_bundle_entry"), &HandleGetAssetBundleEntry },
    { TEXT("get_primary_asset_id"), &HandleGetPrimaryAssetId },
    { TEXT("get_primary_asset_ids_by_type"), &HandleGetPrimaryAssetIdsByType },
    { TEXT("get_primary_asset_types"), &HandleGetPrimaryAssetTypes },
    { TEXT("remove_primary_asset_rule"), &HandleRemovePrimaryAssetRule },
    { TEXT("scan_primary_assets"), &HandleScanPrimaryAssets },
    { TEXT("set_primary_asset_cook_rule"), &HandleSetPrimaryAssetCookRule },
};
} // namespace UEAgentAssetManagerCommands
