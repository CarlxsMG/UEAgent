#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Data/DataAuthoringUtils.h"

#include "Internationalization/StringTable.h"        // UStringTable (Engine)
#include "Internationalization/StringTableCore.h"     // FStringTable / FStringTableRef (Core)
#include "Internationalization/TextKey.h"             // FTextKey (Core)
#include "Internationalization/Internationalization.h" // FInternationalization (Core) — get/set_active_culture
#include "Internationalization/Culture.h"              // FCulture (Core)

#include "LocalizationSettings.h"              // ULocalizationSettings (Localization)
#include "LocalizationTargetTypes.h"           // ULocalizationTarget/ULocalizationTargetSet (Localization)
#include "LocalizationConfigurationScript.h"   // LocalizationConfigurationScript:: namespace (Localization)
#include "Commandlets/GatherTextCommandlet.h"   // UGatherTextCommandlet (UnrealEd)

#include "UObject/UObjectGlobals.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"

/**
 * String table and localization commands.
 *
 * Authors UStringTable assets: a localization table of Key -> SourceString pairs under a single namespace,
 * each entry a localizable string keyed by an FTextKey, with optional per-entry metadata. Also reads and
 * sets the editor's active culture, and lists, gathers and compiles Localization Dashboard targets.
 *
 * UStringTable (Engine) only wraps the data: GetMutableStringTable() / GetStringTable() return the
 * FStringTable (Core) that holds the namespace, the source strings and their metadata. FStringTableRef is
 * TSharedRef<FStringTable, ThreadSafe>, and an FString converts implicitly to FTextKey.
 *
 * UStringTable is UCLASS(MinimalAPI), but every method used here is explicitly ENGINE_API, so they link.
 */
namespace UEAgentStringTableCommands
{

// MCP-DOMAIN: DataAsset

namespace
{
    UStringTable* LoadStringTableAt(const FString& Path)
    {
        return Cast<UStringTable>(StaticLoadObject(UStringTable::StaticClass(), nullptr, *Path));
    }

    int32 CountSourceStrings(const UStringTable* Table)
    {
        if (!Table)
        {
            return 0;
        }
        int32 Count = 0;
        Table->GetStringTable()->EnumerateSourceStrings(
            [&Count](const FString& /*Key*/, const FString& /*Source*/) -> bool
            {
                ++Count;
                return true; // continue
            });
        return Count;
    }

    // Enumerate an entry's metadata (id -> value) into a JSON object. Shared by the
    // metadata commands and the entries reader, so get_string_table_entries reports
    // each entry's metadata too.
    TSharedPtr<FJsonObject> EnumerateEntryMetadata(const UStringTable* Table, const FString& Key, int32& OutCount)
    {
        TSharedPtr<FJsonObject> Meta = MakeShared<FJsonObject>();
        OutCount = 0;
        if (Table)
        {
            int32* CountPtr = &OutCount;
            Table->GetStringTable()->EnumerateMetaData(Key,
                [&Meta, CountPtr](FName MetaId, const FString& Value) -> bool
                {
                    Meta->SetStringField(MetaId.ToString(), Value);
                    ++(*CountPtr);
                    return true; // continue
                });
        }
        return Meta;
    }

    TSharedPtr<FJsonObject> StringTable_NotFound(const FString& Path)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("StringTable not found: %s"), *Path));
    }

    TSharedPtr<FJsonObject> StringTable_EntryReply(const FString& TablePath, const FString& Key)
    {
        TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
        Reply->SetStringField(TEXT("string_table_path"), TablePath);
        Reply->SetStringField(TEXT("key"), Key);
        return Reply;
    }

    // When the save failed: message = "<Done> but save failed: <SaveError>".
    void StringTable_AddSaveFailure(const TSharedPtr<FJsonObject>& Reply, bool bSaved, const TCHAR* Done, const FString& SaveError)
    {
        if (!bSaved)
        {
            Reply->SetStringField(TEXT("message"), FString::Printf(TEXT("%s but save failed: %s"), Done, *SaveError));
        }
    }

    // The reply of create_string_table before its outcome: the request, then the table the path holds.
    TSharedPtr<FJsonObject> StringTable_CreatedReply(const FString& Name, const FString& PackagePath, const UStringTable* Table)
    {
        TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
        Reply->SetStringField(TEXT("name"), Name);
        Reply->SetStringField(TEXT("package_path"), PackagePath);
        Reply->SetStringField(TEXT("asset_path"), Table->GetPathName());
        Reply->SetStringField(TEXT("string_table_path"), Table->GetPathName());
        Reply->SetStringField(TEXT("string_table_id"), Table->GetStringTableId().ToString());
        Reply->SetStringField(TEXT("namespace"), Table->GetStringTable()->GetNamespace());
        Reply->SetNumberField(TEXT("entry_count"), CountSourceStrings(Table));
        return Reply;
    }

    // Shared upsert used by set_string_table_entry.
    // SetSourceString replaces any existing data for the key (true upsert).
    TSharedPtr<FJsonObject> UpsertStringTableEntry(const TSharedPtr<FJsonObject>& Params,
                                                   const TCHAR* TransactionName)
    {
        FString TablePath;
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("string_table_path"), TablePath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

        FString Key;
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("key"), Key, mcp_common::EAllowEmpty::Yes)) { return Missing; }

        FString SourceString;
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("source_string"), SourceString, mcp_common::EAllowEmpty::Yes)) { return Missing; }

        UStringTable* Table = LoadStringTableAt(TablePath);
        if (!Table)
        {
            return StringTable_NotFound(TablePath);
        }

        FScopedTransaction Transaction(FText::FromString(TransactionName));
        Table->Modify();

        // Was the key already present? (for an accurate created vs updated flag)
        FString PreviousValue;
        const bool bExistedBefore = Table->GetStringTable()->GetSourceString(Key, PreviousValue);

        Table->GetMutableStringTable()->SetSourceString(Key, SourceString, FString());

        Table->MarkPackageDirty();

        FString SaveError;
        const bool bSaved = mcp_common::SaveAssetPackage(Table->GetOutermost(), Table, SaveError);

        // Verify by read-back from the live table.
        FString ReadValue;
        const bool bReadOk = Table->GetStringTable()->GetSourceString(Key, ReadValue);
        const bool bMatches = bReadOk && (ReadValue == SourceString);

        TSharedPtr<FJsonObject> ResultObj = StringTable_EntryReply(TablePath, Key);
        ResultObj->SetStringField(TEXT("source_string"), ReadValue);
        ResultObj->SetBoolField(TEXT("entry_set"), bMatches);
        ResultObj->SetBoolField(TEXT("verified"), bMatches);
        ResultObj->SetBoolField(TEXT("created"), !bExistedBefore);
        ResultObj->SetBoolField(TEXT("updated"), bExistedBefore);
        ResultObj->SetStringField(TEXT("namespace"), Table->GetStringTable()->GetNamespace());
        ResultObj->SetNumberField(TEXT("entry_count"), CountSourceStrings(Table));
        ResultObj->SetBoolField(TEXT("saved"), bSaved);
        StringTable_AddSaveFailure(ResultObj, bSaved, TEXT("Entry set"), SaveError);

        return mcp_common::MakeOkReply(ResultObj);
    }

    // Resolve a ULocalizationTarget by name across the engine/game target sets (or a
    // single requested scope). Returns nullptr if not found; OutResolvedScope is only
    // meaningful when a target is returned.
    ULocalizationTarget* FindLocalizationTargetByName(const FString& TargetName, const FString& Scope, FString& OutResolvedScope)
    {
        auto SearchSet = [&TargetName](ULocalizationTargetSet* TargetSet) -> ULocalizationTarget*
        {
            if (!TargetSet)
            {
                return nullptr;
            }
            for (const TObjectPtr<ULocalizationTarget>& Target : TargetSet->TargetObjects)
            {
                if (Target && Target->Settings.Name == TargetName)
                {
                    return Target;
                }
            }
            return nullptr;
        };

        if (Scope != TEXT("game"))
        {
            if (ULocalizationTarget* Found = SearchSet(ULocalizationSettings::GetEngineTargetSet()))
            {
                OutResolvedScope = TEXT("engine");
                return Found;
            }
        }
        if (Scope != TEXT("engine"))
        {
            if (ULocalizationTarget* Found = SearchSet(ULocalizationSettings::GetGameTargetSet()))
            {
                OutResolvedScope = TEXT("game");
                return Found;
            }
        }
        return nullptr;
    }
}

// --------------------------------------------------------------------------- //
// create_string_table
// --------------------------------------------------------------------------- //
// MCP:create_string_table - Creates a UStringTable asset at package_path/name with the given namespace and no entries, then saves it. A string table already there is returned unchanged (namespace not applied, already_exists true). Returns asset_path, string_table_id, namespace, entry_count, created, saved. Add entries with set_string_table_entry.
//   @param name - Name of the new StringTable asset
//   @param package_path - Target content path, defaults to /Game/StringTables
//   @param namespace - Optional namespace for all entries, defaults to asset name
//   @returns { name: string, package_path: string, asset_path: string, string_table_path: string, string_table_id: string, namespace: string, entry_count: int, created: bool, already_exists: bool, saved: bool, message: string }
TSharedPtr<FJsonObject> HandleCreateStringTable(const TSharedPtr<FJsonObject>& Params)
{
    FString Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    FString PackagePath;
    if (!Params->TryGetStringField(TEXT("package_path"), PackagePath))
    {
        PackagePath = TEXT("/Game/StringTables");
    }

    // Optional explicit namespace; default to the asset name for a stable, sensible value.
    FString Namespace;
    const bool bHasNamespace = Params->TryGetStringField(TEXT("namespace"), Namespace);
    if (!bHasNamespace)
    {
        Namespace = Name;
    }

    const FString FullPath = FString::Printf(TEXT("%s/%s"), *PackagePath, *Name);
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(FullPath)) { return Invalid; }

    // Idempotent: if it already exists, load it and return a STABLE payload.
    if (UEditorAssetLibrary::DoesAssetExist(FullPath))
    {
        UStringTable* Existing = LoadStringTableAt(FullPath);
        if (Existing)
        {
            TSharedPtr<FJsonObject> ExistObj = StringTable_CreatedReply(Name, PackagePath, Existing);
            DataAuthoringUtils::AddAlreadyExistsFlags(ExistObj);
            return mcp_common::MakeOkReply(ExistObj);
        }
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Asset already exists at %s but could not be loaded as UStringTable"), *FullPath));
    }

    FString CreateError;
    UStringTable* NewTable = mcp_common::NewAssetInPackage<UStringTable>(FullPath, Name, TEXT("Failed to create UStringTable"), CreateError);
    if (!NewTable)
    {
        return mcp_common::MakeErrorReply(CreateError);
    }

    // Set the namespace shared by all entries in this table.
    NewTable->GetMutableStringTable()->SetNamespace(Namespace);

    FAssetRegistryModule::AssetCreated(NewTable);

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(NewTable->GetOutermost(), NewTable, SaveError);

    TSharedPtr<FJsonObject> ResultObj = StringTable_CreatedReply(Name, PackagePath, NewTable);
    ResultObj->SetBoolField(TEXT("created"), true);
    ResultObj->SetBoolField(TEXT("already_exists"), false);
    ResultObj->SetStringField(TEXT("message"), bSaved
        ? TEXT("StringTable created and saved to disk")
        : FString::Printf(TEXT("StringTable created but save failed: %s"), *SaveError));

    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("create_string_table"), SaveError);
}

// --------------------------------------------------------------------------- //
// set_string_table_entry  (upsert)
// --------------------------------------------------------------------------- //
// MCP:set_string_table_entry - Set a Key to SourceString entry in a UStringTable (upsert)
//   @param string_table_path - Full asset path to the UStringTable
//   @param key - Entry key to upsert
//   @param source_string - Source string for the key
//   @returns { string_table_path: string, key: string, source_string: string, entry_set: bool, verified: bool, created: bool, updated: bool, namespace: string, entry_count: int, saved: bool }
TSharedPtr<FJsonObject> HandleSetStringTableEntry(const TSharedPtr<FJsonObject>& Params)
{
    return UpsertStringTableEntry(Params, TEXT("Set String Table Entry"));
}

// --------------------------------------------------------------------------- //
// remove_string_table_entry
// --------------------------------------------------------------------------- //
// MCP:remove_string_table_entry - Remove the entry key, with all its metadata, from a UStringTable asset inside an undo transaction, then mark the asset dirty and save it. Idempotent: a key that is not in the table is already the state asked for, so the call succeeds with removed false and already_absent true and changes and saves nothing. A missing table is an error, and so is a failed save, which keeps the removal in memory. Use remove_string_table_metadata to drop only metadata, set_string_table_entry to add or change an entry, get_string_table_entries to list them.
//   @param string_table_path - Full asset path to the UStringTable
//   @param key - Entry key to remove
//   @returns { string_table_path: string, key: string, removed: bool, existed: bool, already_absent: bool, verified: bool, namespace: string, entry_count: int, saved: bool }
TSharedPtr<FJsonObject> HandleRemoveStringTableEntry(const TSharedPtr<FJsonObject>& Params)
{
    FString TablePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("string_table_path"), TablePath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    FString Key;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("key"), Key, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UStringTable* Table = LoadStringTableAt(TablePath);
    if (!Table)
    {
        return StringTable_NotFound(TablePath);
    }

    FString PreviousValue;
    const bool bExistedBefore = Table->GetStringTable()->GetSourceString(Key, PreviousValue);
    if (!bExistedBefore)
    {
        // The key is already absent, the state the call asks for: nothing changes, so the table
        // is neither dirtied nor saved and no undo entry is recorded.
        TSharedPtr<FJsonObject> ResultObj = StringTable_EntryReply(TablePath, Key);
        ResultObj->SetBoolField(TEXT("removed"), false);
        ResultObj->SetBoolField(TEXT("existed"), false);
        ResultObj->SetBoolField(TEXT("already_absent"), true);
        ResultObj->SetBoolField(TEXT("verified"), true);
        ResultObj->SetStringField(TEXT("namespace"), Table->GetStringTable()->GetNamespace());
        ResultObj->SetNumberField(TEXT("entry_count"), CountSourceStrings(Table));
        ResultObj->SetBoolField(TEXT("saved"), false);
        return mcp_common::MakeOkReply(ResultObj);
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "RemoveStringTableEntry", "Remove String Table Entry"));
    Table->Modify();

    Table->GetMutableStringTable()->RemoveSourceString(Key);

    Table->MarkPackageDirty();

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Table->GetOutermost(), Table, SaveError);

    // Verify by read-back: the key must be gone.
    FString ReadValue;
    const bool bStillPresent = Table->GetStringTable()->GetSourceString(Key, ReadValue);
    const bool bRemoved = !bStillPresent;

    TSharedPtr<FJsonObject> ResultObj = StringTable_EntryReply(TablePath, Key);
    ResultObj->SetBoolField(TEXT("removed"), bRemoved);
    ResultObj->SetBoolField(TEXT("existed"), true);
    ResultObj->SetBoolField(TEXT("already_absent"), false);
    ResultObj->SetBoolField(TEXT("verified"), bRemoved);
    ResultObj->SetStringField(TEXT("namespace"), Table->GetStringTable()->GetNamespace());
    ResultObj->SetNumberField(TEXT("entry_count"), CountSourceStrings(Table));
    StringTable_AddSaveFailure(ResultObj, bSaved, TEXT("Entry removed"), SaveError);

    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("remove_string_table_entry"), SaveError);
}

// --------------------------------------------------------------------------- //
// get_string_table_entries
// --------------------------------------------------------------------------- //
// MCP:get_string_table_entries - Lists every entry of a UStringTable asset without changing it. Returns string_table_name, string_table_id, namespace, count and entries, each with key, source (the source string) and its metadata as an id to value object with metadata_count. Edit entries with set_string_table_entry.
//   @param string_table_path - Full asset path to the UStringTable
//   @returns { string_table_path: string, string_table_name: string, string_table_id: string, namespace: string, entries: [object], count: int }
TSharedPtr<FJsonObject> HandleGetStringTableEntries(const TSharedPtr<FJsonObject>& Params)
{
    FString TablePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("string_table_path"), TablePath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UStringTable* Table = LoadStringTableAt(TablePath);
    if (!Table)
    {
        return StringTable_NotFound(TablePath);
    }

    TArray<TSharedPtr<FJsonValue>> EntriesArr;
    Table->GetStringTable()->EnumerateSourceStrings(
        [&EntriesArr, Table](const FString& Key, const FString& Source) -> bool
        {
            TSharedPtr<FJsonObject> EntryObj = MakeShared<FJsonObject>();
            EntryObj->SetStringField(TEXT("key"), Key);
            EntryObj->SetStringField(TEXT("source"), Source);
            // Per-entry metadata.
            int32 MetaCount = 0;
            EntryObj->SetObjectField(TEXT("metadata"), EnumerateEntryMetadata(Table, Key, MetaCount));
            EntryObj->SetNumberField(TEXT("metadata_count"), MetaCount);
            EntriesArr.Add(MakeShared<FJsonValueObject>(EntryObj));
            return true; // continue enumeration
        });

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("string_table_path"), TablePath);
    ResultObj->SetStringField(TEXT("string_table_name"), Table->GetName());
    ResultObj->SetStringField(TEXT("string_table_id"), Table->GetStringTableId().ToString());
    ResultObj->SetStringField(TEXT("namespace"), Table->GetStringTable()->GetNamespace());
    ResultObj->SetArrayField(TEXT("entries"), EntriesArr);
    ResultObj->SetNumberField(TEXT("count"), EntriesArr.Num());

    return mcp_common::MakeOkReply(ResultObj);
}

// --------------------------------------------------------------------------- //
// set_string_table_metadata
// --------------------------------------------------------------------------- //
// MCP:set_string_table_metadata - Set a metadata id->value pair on an existing UStringTable entry (FStringTable::SetMetaData).  Metadata is per-entry localization data (e.g. Comment/Speaker/Context) that the upsert/reader did not expose.  Idempotent (set-to-fixed-value); the entry (key) must already exist.
//   @param string_table_path - Full asset path to the UStringTable.
//   @param key - Entry key the metadata is attached to (must already exist).
//   @param metadata_id - Metadata id (an FName, e.g. 'Comment').
//   @param metadata_value - Metadata value string (may be empty).
//   @returns { string_table_path: string, key: string, metadata_id: string, metadata_value: string, verified: bool, entry_exists: bool, metadata_count: int, namespace: string, saved: bool }
TSharedPtr<FJsonObject> HandleSetStringTableMetadata(const TSharedPtr<FJsonObject>& Params)
{
    FString TablePath, Key, MetaId, MetaValue;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("string_table_path"), TablePath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("key"), Key, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("metadata_id"), MetaId)) { return Missing; }
    if (!Params->TryGetStringField(TEXT("metadata_value"), MetaValue))
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: metadata_value (may be an empty string)"));
    }

    UStringTable* Table = LoadStringTableAt(TablePath);
    if (!Table)
    {
        return StringTable_NotFound(TablePath);
    }

    // Metadata is attached to an entry; require the entry to exist (honest, avoids orphan metadata).
    FString SourceValue;
    if (!Table->GetStringTable()->GetSourceString(Key, SourceValue))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Entry key '%s' not found; add it with set_string_table_entry first"), *Key));
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetStringTableMetadata", "Set String Table Metadata"));
    Table->Modify();

    Table->GetMutableStringTable()->SetMetaData(Key, FName(*MetaId), MetaValue);

    Table->MarkPackageDirty();

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Table->GetOutermost(), Table, SaveError);

    // Verify by read-back from the live table.
    const FString ReadValue = Table->GetStringTable()->GetMetaData(Key, FName(*MetaId));
    const bool bVerified = (ReadValue == MetaValue);
    int32 MetaCount = 0;
    EnumerateEntryMetadata(Table, Key, MetaCount);

    TSharedPtr<FJsonObject> ResultObj = StringTable_EntryReply(TablePath, Key);
    ResultObj->SetStringField(TEXT("metadata_id"), MetaId);
    ResultObj->SetStringField(TEXT("metadata_value"), ReadValue);
    ResultObj->SetBoolField(TEXT("verified"), bVerified);
    ResultObj->SetBoolField(TEXT("entry_exists"), true);
    ResultObj->SetNumberField(TEXT("metadata_count"), MetaCount);
    ResultObj->SetStringField(TEXT("namespace"), Table->GetStringTable()->GetNamespace());
    StringTable_AddSaveFailure(ResultObj, bSaved, TEXT("Metadata set"), SaveError);
    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("set_string_table_metadata"), SaveError);
}

// --------------------------------------------------------------------------- //
// get_string_table_metadata
// --------------------------------------------------------------------------- //
// MCP:get_string_table_metadata - Read all metadata (id->value) attached to a UStringTable entry (FStringTable::EnumerateMetaData).  The reader that pairs with set_string_table_metadata.
//   @param string_table_path - Full asset path to the UStringTable.
//   @param key - Entry key to read metadata from.
//   @returns { string_table_path: string, key: string, entry_exists: bool, metadata: object, metadata_count: int, namespace: string }
TSharedPtr<FJsonObject> HandleGetStringTableMetadata(const TSharedPtr<FJsonObject>& Params)
{
    FString TablePath, Key;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("string_table_path"), TablePath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("key"), Key, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UStringTable* Table = LoadStringTableAt(TablePath);
    if (!Table)
    {
        return StringTable_NotFound(TablePath);
    }

    FString SourceValue;
    const bool bEntryExists = Table->GetStringTable()->GetSourceString(Key, SourceValue);

    int32 MetaCount = 0;
    TSharedPtr<FJsonObject> Meta = EnumerateEntryMetadata(Table, Key, MetaCount);

    TSharedPtr<FJsonObject> ResultObj = StringTable_EntryReply(TablePath, Key);
    ResultObj->SetBoolField(TEXT("entry_exists"), bEntryExists);
    ResultObj->SetObjectField(TEXT("metadata"), Meta);
    ResultObj->SetNumberField(TEXT("metadata_count"), MetaCount);
    ResultObj->SetStringField(TEXT("namespace"), Table->GetStringTable()->GetNamespace());
    return mcp_common::MakeOkReply(ResultObj);
}

// --------------------------------------------------------------------------- //
// remove_string_table_metadata
// --------------------------------------------------------------------------- //
// MCP:remove_string_table_metadata - Remove a metadata id from a UStringTable entry (FStringTable::RemoveMetaData). Idempotent (no-op if the id is absent). Pairs with set/get_string_table_metadata.
//   @param string_table_path - Full asset path to the UStringTable.
//   @param key - Entry key the metadata is attached to.
//   @param metadata_id - Metadata id (an FName, e.g. 'Comment') to remove.
//   @returns { string_table_path: string, key: string, metadata_id: string, existed: bool, removed: bool, still_exists: bool, entry_exists: bool, metadata: object, metadata_count: int, namespace: string, saved: bool }
TSharedPtr<FJsonObject> HandleRemoveStringTableMetadata(const TSharedPtr<FJsonObject>& Params)
{
    FString TablePath, Key, MetaId;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("string_table_path"), TablePath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("key"), Key, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("metadata_id"), MetaId)) { return Missing; }

    UStringTable* Table = LoadStringTableAt(TablePath);
    if (!Table)
    {
        return StringTable_NotFound(TablePath);
    }

    // Metadata is attached to an entry; require the entry to exist (honest, avoids orphan metadata).
    FString SourceValue;
    if (!Table->GetStringTable()->GetSourceString(Key, SourceValue))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Entry key '%s' not found"), *Key));
    }

    const bool bExisted = !Table->GetStringTable()->GetMetaData(FTextKey(Key), FName(*MetaId)).IsEmpty();

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "RemoveStringTableMetadata", "Remove String Table Metadata"));
    Table->Modify();

    Table->GetMutableStringTable()->RemoveMetaData(FTextKey(Key), FName(*MetaId));

    Table->MarkPackageDirty();

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Table->GetOutermost(), Table, SaveError);

    // Verify by read-back from the live table.
    const bool bStillExists = !Table->GetStringTable()->GetMetaData(FTextKey(Key), FName(*MetaId)).IsEmpty();
    const bool bRemoved = bExisted && !bStillExists;

    int32 MetaCount = 0;
    TSharedPtr<FJsonObject> Meta = EnumerateEntryMetadata(Table, Key, MetaCount);

    TSharedPtr<FJsonObject> ResultObj = StringTable_EntryReply(TablePath, Key);
    ResultObj->SetStringField(TEXT("metadata_id"), MetaId);
    ResultObj->SetBoolField(TEXT("existed"), bExisted);
    ResultObj->SetBoolField(TEXT("removed"), bRemoved);
    ResultObj->SetBoolField(TEXT("still_exists"), bStillExists);
    ResultObj->SetBoolField(TEXT("entry_exists"), true);
    ResultObj->SetObjectField(TEXT("metadata"), Meta);
    ResultObj->SetNumberField(TEXT("metadata_count"), MetaCount);
    ResultObj->SetStringField(TEXT("namespace"), Table->GetStringTable()->GetNamespace());
    StringTable_AddSaveFailure(ResultObj, bSaved, TEXT("Metadata removed"), SaveError);
    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("remove_string_table_metadata"), SaveError);
}

// ---------------------------------------------------------------------------
// get_active_culture
// ---------------------------------------------------------------------------
// Culture read/write — FInternationalization, Core module.

// MCP:get_active_culture - Read the editor's current active culture/locale (FInternationalization::GetCurrentCulture).
//   @returns { culture_name: string, display_name: string, native_name: string, iso_two_letter_name: string }
TSharedPtr<FJsonObject> HandleGetActiveCulture(const TSharedPtr<FJsonObject>& Params)
{
    const FCultureRef Culture = FInternationalization::Get().GetCurrentCulture();

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("culture_name"), Culture->GetName());
    ResultObj->SetStringField(TEXT("display_name"), Culture->GetDisplayName());
    ResultObj->SetStringField(TEXT("native_name"), Culture->GetNativeName());
    ResultObj->SetStringField(TEXT("iso_two_letter_name"), Culture->GetTwoLetterISOLanguageName());
    return mcp_common::MakeOkReply(ResultObj);
}

// ---------------------------------------------------------------------------
// set_active_culture
// ---------------------------------------------------------------------------
// MCP:set_active_culture - Set the editor's active culture/locale (FInternationalization::SetCurrentCulture). Honest error if the culture name isn't recognized.
//   @param culture_name - Required culture identifier, e.g. "en", "es-ES", "ja".
//   @returns { culture_name: string, success: bool }
TSharedPtr<FJsonObject> HandleSetActiveCulture(const TSharedPtr<FJsonObject>& Params)
{
    FString CultureName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("culture_name"), CultureName)) { return Missing; }

    const bool bSuccess = FInternationalization::Get().SetCurrentCulture(CultureName);
    if (!bSuccess)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Unknown or unsupported culture: %s"), *CultureName));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("culture_name"), CultureName);
    ResultObj->SetBoolField(TEXT("success"), bSuccess);
    // The verdict measured above is the verdict EMITTED. A hardcoded success:true
    // from MakeOkReply would hide it, and callers only check that flag.
    return mcp_common::MakeOutcomeReply(ResultObj, bSuccess,
        bSuccess ? FString() : mcp_common::MeasuredOutcomeError(TEXT("set_active_culture"), TEXT("bSuccess")));
}

// ---------------------------------------------------------------------------
// get_culture_info
// ---------------------------------------------------------------------------
// MCP:get_culture_info - Look up a culture by name without switching the active one (FInternationalization::GetCulture).
//   @param culture_name - Required culture identifier, e.g. "en", "es-ES", "ja".
//   @returns { culture_name: string, found: bool, display_name: string, native_name: string, iso_two_letter_name: string }
TSharedPtr<FJsonObject> HandleGetCultureInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString CultureName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("culture_name"), CultureName)) { return Missing; }

    const FCulturePtr Culture = FInternationalization::Get().GetCulture(CultureName);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("culture_name"), CultureName);
    ResultObj->SetBoolField(TEXT("found"), Culture.IsValid());
    if (Culture.IsValid())
    {
        ResultObj->SetStringField(TEXT("display_name"), Culture->GetDisplayName());
        ResultObj->SetStringField(TEXT("native_name"), Culture->GetNativeName());
        ResultObj->SetStringField(TEXT("iso_two_letter_name"), Culture->GetTwoLetterISOLanguageName());
    }
    return mcp_common::MakeOkReply(ResultObj);
}

// ---------------------------------------------------------------------------
// list_localization_targets
// ---------------------------------------------------------------------------
// Localization target pipeline (gather/compile) -- Localization module.
// Drives the same ULocalizationTarget config generation + UGatherTextCommandlet used by the Localization
// Dashboard, run in-process (Execute() is the documented embedded-caller entry point) rather than spawning
// an external UAT/editor process.

// MCP:list_localization_targets - List localization targets configured in the Localization Dashboard (ULocalizationSettings::GetEngineTargetSet/GetGameTargetSet), with their native/supported cultures.
//   @param scope - Optional: "engine" | "game" | "all". Defaults to "all".
//   @returns { scope: string, targets: [{ name: string, scope: string, native_culture: string, supported_cultures: string[] }], targets_count: int }
TSharedPtr<FJsonObject> HandleListLocalizationTargets(const TSharedPtr<FJsonObject>& Params)
{
    FString Scope;
    if (!Params->TryGetStringField(TEXT("scope"), Scope) || Scope.IsEmpty())
    {
        Scope = TEXT("all");
    }
    if (Scope != TEXT("engine") && Scope != TEXT("game") && Scope != TEXT("all"))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Invalid scope '%s'; expected 'engine', 'game', or 'all'"), *Scope));
    }

    TArray<TSharedPtr<FJsonValue>> TargetsArr;

    auto AppendTargetSet = [&TargetsArr](ULocalizationTargetSet* TargetSet, const TCHAR* ScopeName)
    {
        if (!TargetSet)
        {
            return;
        }
        for (const TObjectPtr<ULocalizationTarget>& Target : TargetSet->TargetObjects)
        {
            if (!Target)
            {
                continue;
            }
            const FLocalizationTargetSettings& Settings = Target->Settings;

            TArray<TSharedPtr<FJsonValue>> CulturesArr;
            for (const FCultureStatistics& Stat : Settings.SupportedCulturesStatistics)
            {
                CulturesArr.Add(MakeShared<FJsonValueString>(Stat.CultureName));
            }

            FString NativeCulture;
            if (Settings.SupportedCulturesStatistics.IsValidIndex(Settings.NativeCultureIndex))
            {
                NativeCulture = Settings.SupportedCulturesStatistics[Settings.NativeCultureIndex].CultureName;
            }

            TSharedPtr<FJsonObject> TargetObj = MakeShared<FJsonObject>();
            TargetObj->SetStringField(TEXT("name"), Settings.Name);
            TargetObj->SetStringField(TEXT("scope"), ScopeName);
            TargetObj->SetStringField(TEXT("native_culture"), NativeCulture);
            TargetObj->SetArrayField(TEXT("supported_cultures"), CulturesArr);
            TargetsArr.Add(MakeShared<FJsonValueObject>(TargetObj));
        }
    };

    if (Scope == TEXT("engine") || Scope == TEXT("all"))
    {
        AppendTargetSet(ULocalizationSettings::GetEngineTargetSet(), TEXT("engine"));
    }
    if (Scope == TEXT("game") || Scope == TEXT("all"))
    {
        AppendTargetSet(ULocalizationSettings::GetGameTargetSet(), TEXT("game"));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("scope"), Scope);
    ResultObj->SetArrayField(TEXT("targets"), TargetsArr);
    ResultObj->SetNumberField(TEXT("targets_count"), TargetsArr.Num());
    return mcp_common::MakeOkReply(ResultObj);
}

// ---------------------------------------------------------------------------
// gather_text_for_target
// ---------------------------------------------------------------------------
// MCP:gather_text_for_target - Gather text into a localization target's manifest/archive files, by generating its GatherText commandlet config (LocalizationConfigurationScript::GenerateGatherTextConfigFile) and running it in-process (UGatherTextCommandlet::Execute) -- the same pipeline the Localization Dashboard's "Gather Text" button drives.
//   @param target_name - Required. Target name, as listed by list_localization_targets.
//   @param scope - Optional: "engine" | "game". Disambiguates a same-named target in both sets; defaults to searching both.
//   @returns { target_name: string, scope: string, config_path: string, config_written: bool, exit_code: int, success: bool, message: string }
TSharedPtr<FJsonObject> HandleGatherTextForTarget(const TSharedPtr<FJsonObject>& Params)
{
    FString TargetName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("target_name"), TargetName)) { return Missing; }
    FString Scope;
    Params->TryGetStringField(TEXT("scope"), Scope);

    FString ResolvedScope;
    ULocalizationTarget* Target = FindLocalizationTargetByName(TargetName, Scope, ResolvedScope);
    if (!Target)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Localization target not found: %s"), *TargetName));
    }

    FLocalizationConfigurationScript Script = LocalizationConfigurationScript::GenerateGatherTextConfigFile(Target);
    const FString ConfigPath = LocalizationConfigurationScript::GetGatherTextConfigPath(Target);
    const bool bWritten = Script.WriteWithSCC(ConfigPath);
    if (!bWritten)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Failed to write gather-text config to %s"), *ConfigPath));
    }

    const FString CommandletParams = FString::Printf(TEXT("-config=\"%s\" -Unattended"), *ConfigPath);
    UGatherTextCommandlet* Commandlet = NewObject<UGatherTextCommandlet>(GetTransientPackage());
    const int32 ExitCode = Commandlet->Execute(CommandletParams, nullptr);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("target_name"), TargetName);
    ResultObj->SetStringField(TEXT("scope"), ResolvedScope);
    ResultObj->SetStringField(TEXT("config_path"), ConfigPath);
    ResultObj->SetBoolField(TEXT("config_written"), bWritten);
    ResultObj->SetNumberField(TEXT("exit_code"), ExitCode);
    ResultObj->SetBoolField(TEXT("success"), ExitCode == 0);
    ResultObj->SetStringField(TEXT("message"), ExitCode == 0
        ? TEXT("Text gathered into the target's manifest/archive files")
        : FString::Printf(TEXT("GatherText commandlet exited with code %d (see Output Log for details)"), ExitCode));

    // The verdict measured above is the verdict EMITTED. A hardcoded success:true
    // from MakeOkReply would hide it, and callers only check that flag.
    return mcp_common::MakeOutcomeReply(ResultObj, ExitCode == 0,
        ExitCode == 0 ? FString() : mcp_common::MeasuredOutcomeError(TEXT("gather_text_for_target"), TEXT("ExitCode == 0")));
}

// ---------------------------------------------------------------------------
// compile_text_for_target
// ---------------------------------------------------------------------------
// MCP:compile_text_for_target - Compile a localization target's archives into runtime LocRes/LocMeta binaries, by generating its CompileText commandlet config (LocalizationConfigurationScript::GenerateCompileTextConfigFile) and running it in-process (UGatherTextCommandlet::Execute) -- the same pipeline the Localization Dashboard's "Compile Text" button drives. Run gather_text_for_target first so the archives are up to date.
//   @param target_name - Required. Target name, as listed by list_localization_targets.
//   @param scope - Optional: "engine" | "game". Disambiguates a same-named target in both sets; defaults to searching both.
//   @param culture_name - Optional. Restrict compilation to a single supported culture; omit to compile every supported culture.
//   @returns { target_name: string, scope: string, culture_name: string, config_path: string, config_written: bool, exit_code: int, success: bool, message: string }
TSharedPtr<FJsonObject> HandleCompileTextForTarget(const TSharedPtr<FJsonObject>& Params)
{
    FString TargetName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("target_name"), TargetName)) { return Missing; }
    FString Scope;
    Params->TryGetStringField(TEXT("scope"), Scope);
    FString CultureName;
    const bool bHasCulture = Params->TryGetStringField(TEXT("culture_name"), CultureName) && !CultureName.IsEmpty();

    FString ResolvedScope;
    ULocalizationTarget* Target = FindLocalizationTargetByName(TargetName, Scope, ResolvedScope);
    if (!Target)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Localization target not found: %s"), *TargetName));
    }

    const TOptional<FString> CultureOpt = bHasCulture ? TOptional<FString>(CultureName) : TOptional<FString>();

    FLocalizationConfigurationScript Script = LocalizationConfigurationScript::GenerateCompileTextConfigFile(Target, CultureOpt);
    const FString ConfigPath = LocalizationConfigurationScript::GetCompileTextConfigPath(Target, CultureOpt);
    const bool bWritten = Script.WriteWithSCC(ConfigPath);
    if (!bWritten)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Failed to write compile-text config to %s"), *ConfigPath));
    }

    const FString CommandletParams = FString::Printf(TEXT("-config=\"%s\" -Unattended"), *ConfigPath);
    UGatherTextCommandlet* Commandlet = NewObject<UGatherTextCommandlet>(GetTransientPackage());
    const int32 ExitCode = Commandlet->Execute(CommandletParams, nullptr);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("target_name"), TargetName);
    ResultObj->SetStringField(TEXT("scope"), ResolvedScope);
    ResultObj->SetStringField(TEXT("culture_name"), bHasCulture ? CultureName : TEXT(""));
    ResultObj->SetStringField(TEXT("config_path"), ConfigPath);
    ResultObj->SetBoolField(TEXT("config_written"), bWritten);
    ResultObj->SetNumberField(TEXT("exit_code"), ExitCode);
    ResultObj->SetBoolField(TEXT("success"), ExitCode == 0);
    ResultObj->SetStringField(TEXT("message"), ExitCode == 0
        ? TEXT("Text compiled to LocRes/LocMeta binaries")
        : FString::Printf(TEXT("GatherText commandlet (compile step) exited with code %d (see Output Log for details)"), ExitCode));

    // The verdict measured above is the verdict EMITTED. A hardcoded success:true
    // from MakeOkReply would hide it, and callers only check that flag.
    return mcp_common::MakeOutcomeReply(ResultObj, ExitCode == 0,
        ExitCode == 0 ? FString() : mcp_common::MeasuredOutcomeError(TEXT("compile_text_for_target"), TEXT("ExitCode == 0")));
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("compile_text_for_target"), &HandleCompileTextForTarget },
    { TEXT("create_string_table"), &HandleCreateStringTable },
    { TEXT("gather_text_for_target"), &HandleGatherTextForTarget },
    { TEXT("get_active_culture"), &HandleGetActiveCulture },
    { TEXT("get_culture_info"), &HandleGetCultureInfo },
    { TEXT("get_string_table_entries"), &HandleGetStringTableEntries },
    { TEXT("get_string_table_metadata"), &HandleGetStringTableMetadata },
    { TEXT("list_localization_targets"), &HandleListLocalizationTargets },
    { TEXT("remove_string_table_entry"), &HandleRemoveStringTableEntry },
    { TEXT("remove_string_table_metadata"), &HandleRemoveStringTableMetadata },
    { TEXT("set_active_culture"), &HandleSetActiveCulture },
    { TEXT("set_string_table_entry"), &HandleSetStringTableEntry },
    { TEXT("set_string_table_metadata"), &HandleSetStringTableMetadata },
};
} // namespace UEAgentStringTableCommands
