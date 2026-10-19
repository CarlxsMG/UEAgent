#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "GameplayTagContainer.h"
#include "GameplayTagsManager.h"
#include "GameplayTagAssetInterface.h"
#include "GameplayTagsSettings.h"      // UGameplayTagsSettings (where the redirect array lives)
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Misc/PackageName.h"
#include "Engine/DataTable.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"

#include "ScopedTransaction.h"
#include "GameplayTagsEditorModule.h"

#define LOCTEXT_NAMESPACE "UEAgentGameplayTags"

/**
 * MCP commands for gameplay tags.
 *
 * They cover the live tag registry (UGameplayTagsManager) and its ini / DataTable sources, the tags owned by
 * an individual object through IGameplayTagAssetInterface, and the project's tag redirects.
 */
namespace UEAgentGameplayTagsCommands
{

// MCP-DOMAIN: GameplayTags

namespace
{
    /**
     * Finds a writable FGameplayTagContainer UPROPERTY on the given object (the storage
     * that IGameplayTagAssetInterface implementations typically expose via
     * GetOwnedGameplayTags). Returns a pointer into the live object instance, or nullptr
     * if the object has no such reflected property.
     */
    FGameplayTagContainer* FindWritableTagContainer(UObject* Object)
    {
        if (!Object)
        {
            return nullptr;
        }

        UScriptStruct* TagContainerStruct = FGameplayTagContainer::StaticStruct();
        for (TFieldIterator<FStructProperty> PropIt(Object->GetClass()); PropIt; ++PropIt)
        {
            FStructProperty* StructProp = *PropIt;
            if (StructProp && StructProp->Struct == TagContainerStruct)
            {
                return StructProp->ContainerPtrToValuePtr<FGameplayTagContainer>(Object);
            }
        }
        return nullptr;
    }

}

// Stateless: every command reads the live tag registry or the objects it names.

// ---------------------------------------------------------------------------
// Shared pieces of the tag commands: resolving the object a request points at, looking
// tags up in the live registry, the stock error replies, the undoable edit of an
// object's tag container, loading a gameplay-tag DataTable, and the reads of the
// params that name those.
// ---------------------------------------------------------------------------
namespace UEAgentGameplayTagsImpl
{

    TSharedPtr<FJsonObject> ObjectNotFound(const FString& ObjectPath)
    {
        return mcp_common::MakeErrorReply(TEXT("Object not found: ") + ObjectPath);
    }

    TSharedPtr<FJsonObject> LacksTagInterface(const FString& ObjectPath)
    {
        return mcp_common::MakeErrorReply(TEXT("Object does not implement IGameplayTagAssetInterface: ") + ObjectPath);
    }

    TSharedPtr<FJsonObject> TagNotFound(const FString& TagName)
    {
        return mcp_common::MakeErrorReply(TEXT("Tag not found: ") + TagName);
    }

    // object_path is first tried as an object path (asset or already loaded object). If that
    // resolves nothing it is taken as the object name of an actor in the editor world
    // (FString equality, so case is ignored; the first match wins).
    UObject* ResolveTagHolder(const FString& ObjectPath)
    {
        if (UObject* ByPath = mcp_common::ResolveObjectByPath(ObjectPath))
        {
            return ByPath;
        }
        return mcp_common::FindActorByObjectName(mcp_common::GetEditorWorld(), ObjectPath);
    }

    // The registered tag spelled TagName, or an invalid tag. Unknown names are not an error
    // here (ErrorIfNotFound = false); callers decide what an invalid tag means.
    FGameplayTag FindRegisteredTag(const FString& TagName)
    {
        return UGameplayTagsManager::Get().RequestGameplayTag(FName(*TagName), /*ErrorIfNotFound*/ false);
    }

    // Snapshot of the whole live registry (every source, not only dictionary tags).
    TArray<FGameplayTag> AllRegisteredTags()
    {
        FGameplayTagContainer Registry;
        UGameplayTagsManager::Get().RequestAllGameplayTags(Registry, /*OnlyIncludeDictionaryTags*/ false);

        TArray<FGameplayTag> Flat;
        Registry.GetGameplayTagArray(Flat);
        return Flat;
    }

    // The valid tags an object reports through IGameplayTagAssetInterface, as JSON strings.
    TArray<TSharedPtr<FJsonValue>> OwnedTagStrings(const IGameplayTagAssetInterface& Holder)
    {
        FGameplayTagContainer Owned;
        Holder.GetOwnedGameplayTags(Owned);

        TArray<TSharedPtr<FJsonValue>> Strings;
        for (const FGameplayTag& OwnedTag : Owned)
        {
            if (OwnedTag.IsValid())
            {
                Strings.Add(MakeShared<FJsonValueString>(OwnedTag.ToString()));
            }
        }
        return Strings;
    }

    // Runs Edit on the object's tag storage as one undoable step (editor builds) and flags
    // the package dirty. Only called when the container is really about to change.
    void EditOwnedTags(UObject* Holder, const FText& UndoLabel, TFunctionRef<void()> Edit)
    {
        const FScopedTransaction Transaction(UndoLabel);
        Holder->Modify();
        Edit();
        Holder->MarkPackageDirty();
    }

    // Loads a DataTable that holds gameplay-tag rows (FGameplayTagTableRow or a subclass).
    // On failure returns null and leaves the reply to send back in OutError.
    UDataTable* LoadTagTable(const FString& TablePath, TSharedPtr<FJsonObject>& OutError)
    {
        UDataTable* Table = LoadObject<UDataTable>(nullptr, *TablePath);
        if (Table == nullptr)
        {
            OutError = mcp_common::MakeErrorReply(TEXT("DataTable not found: ") + TablePath);
            return nullptr;
        }

        const UScriptStruct* RowType = Table->RowStruct;
        if (RowType == nullptr || !RowType->IsChildOf(FGameplayTagTableRow::StaticStruct()))
        {
            const FString RowTypeName = (RowType != nullptr) ? RowType->GetName() : FString(TEXT("None"));
            OutError = mcp_common::MakeErrorReply(TEXT("DataTable row struct is not FGameplayTagTableRow: ") + RowTypeName);
            return nullptr;
        }
        return Table;
    }

    // Reads the required tag param, trimmed; the error reply when it is missing or blank.
    TSharedPtr<FJsonObject> ReadTagParam(const TSharedPtr<FJsonObject>& Params, FString& OutTag)
    {
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("tag"), OutTag, mcp_common::EAllowEmpty::Yes))
        {
            return Missing;
        }
        OutTag.TrimStartAndEndInline();
        if (OutTag.IsEmpty())
        {
            return mcp_common::MakeErrorReply(TEXT("tag must be a non-empty string"));
        }
        return nullptr;
    }

    // Reads the required table_path and tag (ReadTagParam) of a tag-table row command and loads that table
    // (LoadTagTable). On failure returns null and leaves the reply to send back in OutError.
    UDataTable* ReadTagTableRow(const TSharedPtr<FJsonObject>& Params, FString& OutTag, TSharedPtr<FJsonObject>& OutError)
    {
        FString TablePath;
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("table_path"), TablePath, mcp_common::EAllowEmpty::Yes))
        {
            OutError = Missing;
            return nullptr;
        }
        OutError = ReadTagParam(Params, OutTag);
        if (OutError.IsValid())
        {
            return nullptr;
        }
        return LoadTagTable(TablePath, OutError);
    }

    // The object and tag an add/remove_tag_from_object command names, with the tag container the edit writes.
    struct FTagEdit
    {
        FString ObjectPath;
        FString TagName;
        UObject* Holder = nullptr;
        FGameplayTag Tag;
        const IGameplayTagAssetInterface* TagSource = nullptr;
        FGameplayTagContainer* Storage = nullptr;
    };

    // Reads object_path and tag_name and checks, in this order, the object, the tag, its tag interface and the writable
    // container behind it (the interface only reads tags). The error reply when one fails; Verb names the edit.
    TSharedPtr<FJsonObject> ReadTagEdit(const TSharedPtr<FJsonObject>& Params, const TCHAR* Verb, FTagEdit& Out)
    {
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("object_path"), Out.ObjectPath, mcp_common::EAllowEmpty::Yes))
        {
            return Missing;
        }
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("tag_name"), Out.TagName, mcp_common::EAllowEmpty::Yes))
        {
            return Missing;
        }
        Out.Holder = ResolveTagHolder(Out.ObjectPath);
        if (Out.Holder == nullptr)
        {
            return ObjectNotFound(Out.ObjectPath);
        }
        Out.Tag = FindRegisteredTag(Out.TagName);
        if (!Out.Tag.IsValid())
        {
            return TagNotFound(Out.TagName);
        }
        Out.TagSource = Cast<IGameplayTagAssetInterface>(Out.Holder);
        if (Out.TagSource == nullptr)
        {
            return LacksTagInterface(Out.ObjectPath);
        }
        Out.Storage = FindWritableTagContainer(Out.Holder);
        if (Out.Storage == nullptr)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Object implements IGameplayTagAssetInterface but exposes no writable FGameplayTagContainer property; cannot %s the tag generically."), Verb));
        }
        return nullptr;
    }
}

// --- Single tags: create in the ini, inspect, search -------------------------------

// MCP:create_gameplay_tag - Add a new gameplay tag to the project's DefaultGameplayTags.ini AND register it LIVE in this editor session -- that live half is why you must call this before writing the tag into a DataTable row or a property, since an unregistered tag string is silently reset to None by the importers. But it REWRITES THE WHOLE INI: the list comes back alphabetised, every comment block is deleted, and other tags' DevComments are re-serialised and normalised, so adding one tag can produce a diff full of unrelated insertions and deletions. Copy the ini first; the oracle is `git diff --stat` showing your N insertions and ZERO deletions, and if not, git checkout the ini and re-insert only your line (the tag stays registered live either way -- confirm with get_gameplay_tag_info).
//   @param tag_name - Name of the gameplay tag to create or check
//   @param dev_comment - Optional developer comment for the tag
//   @param source_name - Optional explicit source INI name
//   @returns { tag_name: string, success: bool, tag_string: string, message: string }
TSharedPtr<FJsonObject> HandleCreateGameplayTag(const TSharedPtr<FJsonObject>& Params)
{
    FString TagName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("tag_name"), TagName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    TagName.TrimStartAndEndInline();
    if (TagName.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("tag_name must be a non-empty string"));
    }
    // Both optional: a developer comment, and the .ini the tag goes to.
    FString DevComment;
    Params->TryGetStringField(TEXT("dev_comment"), DevComment);
    FString SourceName;
    Params->TryGetStringField(TEXT("source_name"), SourceName);

    UGameplayTagsManager& Tags = UGameplayTagsManager::Get();
    // Both successful outcomes leave the tag registered; tag_string is its registered spelling.
    const auto Registered = [&TagName](const FString& TagString, const TCHAR* What)
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("message"), What);
        Data->SetStringField(TEXT("tag_string"), TagString);
        Data->SetBoolField(TEXT("success"), true);
        Data->SetStringField(TEXT("tag_name"), TagName);
        return mcp_common::MakeOkReply(Data);
    };

    const FGameplayTag Existing = Tags.RequestGameplayTag(*TagName, /*ErrorIfNotFound=*/false);
    if (Existing.IsValid())
    {
        return Registered(Existing.ToString(), TEXT("Tag already exists"));
    }

    // An invalid string is refused here, with the reason and the engine's suggested fix, where the editor module
    // would only show a notification and fail.
    FText Invalid;
    FString Suggested;
    if (!Tags.IsValidGameplayTagString(TagName, &Invalid, &Suggested))
    {
        FString Message = FString::Printf(TEXT("Invalid gameplay tag string: %s"), *Invalid.ToString());
        if (!Suggested.IsEmpty())
        {
            Message += FString::Printf(TEXT(" (suggested: %s)"), *Suggested);
        }
        return mcp_common::MakeErrorReply(Message);
    }

    // The path of the editor's "Add New Gameplay Tag" widget: the tag joins the project's list in the .ini and the
    // tag tree is refreshed, so query_gameplay_tags lists it.
    const FName SourceFName = SourceName.IsEmpty() ? NAME_None : FName(*SourceName);
    if (!IGameplayTagsEditorModule::Get().AddNewGameplayTagToINI(TagName, DevComment, SourceFName))
    {
        return mcp_common::MakeErrorReply(
            TEXT("Failed to register gameplay tag. The project may not import tags from INI, or the name conflicts with an existing tag/redirector."));
    }
    const FGameplayTag Created = Tags.RequestGameplayTag(*TagName, /*ErrorIfNotFound=*/false);
    return Registered(Created.IsValid() ? Created.ToString() : TagName, TEXT("Gameplay tag created and registered in INI"));
}

// MCP:get_gameplay_tag_info - Does this exact tag string exist and resolve right now, and if so its detail: its PARENT tag and where it is defined. found=false is a normal answer, not an error -- the precondition to check BEFORE using a tag anywhere: an unregistered tag is not an error in UE, it silently becomes an empty/None tag, and a query made with an empty tag fails exactly like a valid tag that does not match. The parent is how you walk the hierarchy (Material.Wood.Oak answers to Material.Wood); hierarchy matching is ASYMMETRIC, HasAny(A, B) only succeeds when a tag of A is B or a DESCENDANT of B, so putting a family where a leaf is expected quietly fails. The source says where the tag comes from -- native C++, DefaultGameplayTags.ini, a tag DataTable, or transient (registered live this session and NOT on disk) -- with its comment and native / restricted / explicit flags: whether a tag survives an editor restart, which file to edit to change it, and whether it is YOURS or a vendor's or the engine's before you rename or remove it. Exact lookup: use query_gameplay_tags for a branch.
//   @param tag_name - Name of the gameplay tag to look up
//   @returns { tag_name: string, found: bool, tag_string?: string, tag_name_string?: string, parent_tag?: string, source?: string, comment?: string, is_native?: bool, is_restricted?: bool, is_explicit?: bool, allow_non_restricted_children?: bool }
TSharedPtr<FJsonObject> HandleGetGameplayTagInfo(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentGameplayTagsImpl;

    FString TagName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("tag_name"), TagName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    // A miss is a normal answer here (found=false), not an error.
    const FGameplayTag Resolved = Impl::FindRegisteredTag(TagName);
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("tag_name"), TagName);
    Data->SetBoolField(TEXT("found"), Resolved.IsValid());
    if (!Resolved.IsValid())
    {
        return mcp_common::MakeOkReply(Data);
    }

    Data->SetStringField(TEXT("tag_string"), Resolved.ToString());
    Data->SetStringField(TEXT("tag_name_string"), Resolved.GetTagName().ToString());

    // One level up the hierarchy (A.B.C -> A.B); root tags have no parent field.
    const FGameplayTag Parent = Resolved.RequestDirectParent();
    if (Parent.IsValid())
    {
        Data->SetStringField(TEXT("parent_tag"), Parent.ToString());
    }

    // Where the tag is defined, from the manager's editor data.
    FString Comment;
    FName Source;
    bool bIsExplicit = false;
    bool bIsRestricted = false;
    bool bAllowNonRestrictedChildren = false;
    UGameplayTagsManager::Get().GetTagEditorData(Resolved.GetTagName(), Comment, Source, bIsExplicit, bIsRestricted, bAllowNonRestrictedChildren);

    Data->SetStringField(TEXT("comment"), Comment);
    Data->SetStringField(TEXT("source"), Source.ToString());
    // "Native" tag detection -- engine convention: source = "Native" or starts with "NativeGameplayTags".
    const FString SrcStr = Source.ToString();
    Data->SetBoolField(TEXT("is_native"), SrcStr.Equals(TEXT("Native"), ESearchCase::IgnoreCase)
                                       || SrcStr.StartsWith(TEXT("NativeGameplayTags")));
    Data->SetBoolField(TEXT("is_restricted"), bIsRestricted);
    Data->SetBoolField(TEXT("is_explicit"), bIsExplicit);
    Data->SetBoolField(TEXT("allow_non_restricted_children"), bAllowNonRestrictedChildren);
    return mcp_common::MakeOkReply(Data);
}

// MCP:query_gameplay_tags - Search the registered gameplay tags for everything under a tag -- the discovery call for "what tags exist under Material.Wood" or "is there already a tag for this" -- or, with no query_tag, list EVERY registered tag (the full vocabulary, plugins and vendor content included; projects run to thousands, so prefer a branch). count_only returns just the count: the cheap health probe that the registry loaded, or that create_gameplay_tag registered (compare before and after). Sees the LIVE registry, so a tag added this session by create_gameplay_tag shows up even if the ini was restored, and one that only exists as text in a file does not. A query_tag that is not registered is an error; to test one exact string use get_gameplay_tag_info.
//   @param query_tag - Optional tag whose branch to list (the tag itself and all of its descendants); omitted or empty lists every registered tag
//   @param count_only - Optional bool, default false: return count (and manager_valid) without the matching_tags list
//   @returns { query_tag: string, count: int, manager_valid: bool, matching_tags?: [{ tag_name: string, tag_string: string }] }
TSharedPtr<FJsonObject> HandleQueryGameplayTags(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentGameplayTagsImpl;

    FString QueryTagName;
    Params->TryGetStringField(TEXT("query_tag"), QueryTagName);
    QueryTagName.TrimStartAndEndInline();
    bool bCountOnly = false;
    Params->TryGetBoolField(TEXT("count_only"), bCountOnly);

    // No query tag: every registered tag matches.
    FGameplayTag Query;
    if (!QueryTagName.IsEmpty())
    {
        Query = Impl::FindRegisteredTag(QueryTagName);
        if (!Query.IsValid())
        {
            return mcp_common::MakeErrorReply(TEXT("Query tag not found: ") + QueryTagName);
        }
    }

    // MatchesTag is hierarchical: the query tag itself and all of its descendants match.
    // tag_name is the tag's string form, tag_string the underlying FName as text.
    TArray<TSharedPtr<FJsonValue>> Matches;
    int32 MatchCount = 0;
    for (const FGameplayTag& Candidate : Impl::AllRegisteredTags())
    {
        if (!Candidate.IsValid() || (Query.IsValid() && !Candidate.MatchesTag(Query)))
        {
            continue;
        }
        ++MatchCount;
        if (!bCountOnly)
        {
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("tag_name"), Candidate.ToString());
            Entry->SetStringField(TEXT("tag_string"), Candidate.GetTagName().ToString());
            Matches.Add(MakeShared<FJsonValueObject>(Entry));
        }
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("query_tag"), QueryTagName);
    if (!bCountOnly)
    {
        Data->SetArrayField(TEXT("matching_tags"), Matches);
    }
    Data->SetNumberField(TEXT("count"), MatchCount);
    Data->SetBoolField(TEXT("manager_valid"), IsValid(&UGameplayTagsManager::Get()));
    return mcp_common::MakeOkReply(Data);
}

// --- Tags owned by one object -------------------------------------------------------

// MCP:get_object_tags - Read the gameplay tags an object currently owns -- what the game believes this actor or asset IS (states, factions, damage types, item categories). Requires the object to implement IGameplayTagAssetInterface; anything else returns nothing, which is a fact about the interface, not about the object's data. Reads the LIVE container, so during PIE tags added at runtime appear here. has_gameplay_tags answers a specific question instead of listing everything.
//   @param object_path - Path or name of the object to get tags from
//   @returns { object_path: string, tags: [string], tag_count: int }
TSharedPtr<FJsonObject> HandleGetObjectTags(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentGameplayTagsImpl;

    FString ObjectPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("object_path"), ObjectPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UObject* Holder = Impl::ResolveTagHolder(ObjectPath);
    if (Holder == nullptr)
    {
        return Impl::ObjectNotFound(ObjectPath);
    }
    const IGameplayTagAssetInterface* TagSource = Cast<IGameplayTagAssetInterface>(Holder);
    if (TagSource == nullptr)
    {
        return Impl::LacksTagInterface(ObjectPath);
    }

    const TArray<TSharedPtr<FJsonValue>> Owned = Impl::OwnedTagStrings(*TagSource);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("object_path"), ObjectPath);
    Data->SetArrayField(TEXT("tags"), Owned);
    Data->SetNumberField(TEXT("tag_count"), Owned.Num());
    return mcp_common::MakeOkReply(Data);
}

// MCP:add_tag_to_object - Grant a gameplay tag to an object that implements IGameplayTagAssetInterface -- the way to put an actor into a state (stunned, wet, on fire, alerted) so that tag-driven systems react without any of them knowing about each other. Reports already_present and echoes the resulting tag list, so the reply is its own oracle. Register the tag first (get_gameplay_tag_info / create_gameplay_tag): an unregistered string resolves to an empty tag and is added as nothing.
//   @param object_path - Path or name of the object to add the tag to
//   @param tag_name - Name of the tag to add
//   @returns { object_path: string, tag_name: string, success: bool, already_present: bool, message: string, tags: [string] }
TSharedPtr<FJsonObject> HandleAddTagToObject(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentGameplayTagsImpl;

    Impl::FTagEdit Target;
    if (TSharedPtr<FJsonObject> Error = Impl::ReadTagEdit(Params, TEXT("add"), Target))
    {
        return Error;
    }

    const bool bWasPresent = Target.Storage->HasTagExact(Target.Tag);
    if (!bWasPresent)
    {
        Impl::EditOwnedTags(Target.Holder, LOCTEXT("MCP_AddGameplayTag", "MCP: Add Gameplay Tag"),
            [&Target]() { Target.Storage->AddTag(Target.Tag); });
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("object_path"), Target.ObjectPath);
    Data->SetStringField(TEXT("tag_name"), Target.TagName);
    Data->SetBoolField(TEXT("success"), true);
    Data->SetBoolField(TEXT("already_present"), bWasPresent);
    Data->SetStringField(TEXT("message"),
        bWasPresent ? TEXT("Tag was already present on the object") : TEXT("Tag added to object"));
    // What the object reports after the edit, so the reply can be checked on its own.
    Data->SetArrayField(TEXT("tags"), Impl::OwnedTagStrings(*Target.TagSource));
    return mcp_common::MakeOkReply(Data);
}

// MCP:remove_tag_from_object - Take a gameplay tag off an object implementing IGameplayTagAssetInterface -- clearing a state (no longer stunned, no longer burning) so the systems keyed on it stop reacting. The was_present flag distinguishes "removed it" from "it was never there", and the reply echoes the remaining tags. Exact match only: removing a parent tag does not remove its children.
//   @param object_path - Path or name of the object to remove the tag from
//   @param tag_name - Name of the tag to remove
//   @returns { object_path: string, tag_name: string, success: bool, was_present: bool, message: string, tags: [string] }
TSharedPtr<FJsonObject> HandleRemoveTagFromObject(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentGameplayTagsImpl;

    Impl::FTagEdit Target;
    if (TSharedPtr<FJsonObject> Error = Impl::ReadTagEdit(Params, TEXT("remove"), Target))
    {
        return Error;
    }

    // Exact match only: removing a parent tag leaves its children alone.
    const bool bWasPresent = Target.Storage->HasTagExact(Target.Tag);
    if (bWasPresent)
    {
        Impl::EditOwnedTags(Target.Holder, LOCTEXT("MCP_RemoveGameplayTag", "MCP: Remove Gameplay Tag"),
            [&Target]() { Target.Storage->RemoveTag(Target.Tag); });
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("object_path"), Target.ObjectPath);
    Data->SetStringField(TEXT("tag_name"), Target.TagName);
    Data->SetBoolField(TEXT("success"), true);
    Data->SetBoolField(TEXT("was_present"), bWasPresent);
    Data->SetStringField(TEXT("message"),
        bWasPresent ? TEXT("Tag removed from object") : TEXT("Tag was not present on the object"));
    Data->SetArrayField(TEXT("tags"), Impl::OwnedTagStrings(*Target.TagSource));
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// GameplayTag table/source authoring
// ---------------------------------------------------------------------------

// MCP:create_gameplay_tag_table - Create a DataTable of gameplay tags (row struct FGameplayTagTableRow) -- the alternative to declaring tags in DefaultGameplayTags.ini, and the better choice for a large or team-owned tag set: rows merge and diff like data instead of fighting over one ini that the create_gameplay_tag path rewrites end to end. The table must be registered in the project's gameplay tag settings to be loaded. Fill it with add_gameplay_tag_to_table and check it with get_gameplay_tag_table_rows (valid, missing_tags).
//   @param name - Asset name for the new data table, e.g. GT_MyTags
//   @param package_path - Target content path, defaults to /Game/GameplayTags
//   @returns { name: string, package_path: string, table_path: string, row_struct: string, success: bool, already_exists: bool, saved: bool, message: string }
TSharedPtr<FJsonObject> HandleCreateGameplayTagTable(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentGameplayTagsImpl;

    FString AssetName, PackagePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), AssetName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    AssetName.TrimStartAndEndInline();
    if (AssetName.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("name must be a non-empty string"));
    }

    // Folder for the new table: /Game/GameplayTags unless one is given, never with a
    // trailing slash (the asset path below adds its own separator).
    if (!Params->TryGetStringField(TEXT("package_path"), PackagePath))
    {
        PackagePath = TEXT("/Game/GameplayTags");
    }
    PackagePath.RemoveFromEnd(TEXT("/"));

    const FString FullPath = FString::Printf(TEXT("%s/%s"), *PackagePath, *AssetName);
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(FullPath)) { return Invalid; }

    // RowStruct is the engine-provided FGameplayTagTableRow (GameplayTags module).
    UScriptStruct* RowStruct = FGameplayTagTableRow::StaticStruct();
    if (!RowStruct)
    {
        return mcp_common::MakeErrorReply(TEXT("FGameplayTagTableRow::StaticStruct() returned null"));
    }

    // Idempotent: if a table already exists at the path, load and return it.
    if (UEditorAssetLibrary::DoesAssetExist(FullPath))
    {
        UDataTable* Existing = Cast<UDataTable>(StaticLoadObject(UDataTable::StaticClass(), nullptr, *FullPath));
        TSharedPtr<FJsonObject> ExObj = MakeShared<FJsonObject>();
        ExObj->SetStringField(TEXT("name"), AssetName);
        ExObj->SetStringField(TEXT("package_path"), PackagePath);
        ExObj->SetStringField(TEXT("table_path"), Existing ? Existing->GetPathName() : FullPath);
        ExObj->SetStringField(TEXT("row_struct"), RowStruct->GetName());
        ExObj->SetBoolField(TEXT("success"), true);
        ExObj->SetBoolField(TEXT("already_exists"), true);
        ExObj->SetStringField(TEXT("message"), TEXT("GameplayTag table already exists"));
        return mcp_common::MakeOkReply(ExObj);
    }

    FString CreateError;
    UDataTable* NewTable = mcp_common::NewAssetInPackage<UDataTable>(FullPath, AssetName, TEXT("Failed to create UDataTable"), CreateError);
    if (!NewTable)
    {
        return mcp_common::MakeErrorReply(CreateError);
    }

    NewTable->RowStruct = RowStruct;

    FAssetRegistryModule::AssetCreated(NewTable);

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(NewTable->GetOutermost(), NewTable, SaveError);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("name"), AssetName);
    ResultObj->SetStringField(TEXT("package_path"), PackagePath);
    ResultObj->SetStringField(TEXT("table_path"), NewTable->GetPathName());
    ResultObj->SetStringField(TEXT("row_struct"), RowStruct->GetName());
    ResultObj->SetBoolField(TEXT("success"), bSaved);
    ResultObj->SetBoolField(TEXT("saved"), bSaved);
    ResultObj->SetStringField(TEXT("message"), bSaved
        ? TEXT("GameplayTag table (UDataTable / FGameplayTagTableRow) created and saved to disk")
        : FString::Printf(TEXT("GameplayTag table created but save failed: %s"), *SaveError));

    // The verdict measured above is the verdict EMITTED.
    return mcp_common::MakeOutcomeReply(ResultObj, bSaved,
        bSaved ? FString() : mcp_common::MeasuredOutcomeError(TEXT("create_gameplay_tag_table"), TEXT("bSaved")));
}

// MCP:add_gameplay_tag_to_table - Add one tag (with an optional developer comment) as a row in a gameplay tag DataTable, verifying the write by read-back. The team-friendly way to declare tags: it touches only this asset instead of rewriting the shared DefaultGameplayTags.ini the way create_gameplay_tag does. The trade-off is that a tag added here is not registered LIVE, so it may not resolve until the table is reloaded -- if you must use it in the same session, check with get_gameplay_tag_info first.
//   @param table_path - Full asset path of the gameplay tag data table
//   @param tag - The gameplay tag string used as both row Tag and row name
//   @param dev_comment - Optional developer comment for the row
//   @returns { table_path: string, tag: string, row_name: string, dev_comment: string, verified: bool, saved: bool, row_count: int, success: bool, message: string }
TSharedPtr<FJsonObject> HandleAddGameplayTagToTable(const TSharedPtr<FJsonObject>& Params)
{
    FString TagString;
    TSharedPtr<FJsonObject> LoadError;
    UDataTable* DataTable = UEAgentGameplayTagsImpl::ReadTagTableRow(Params, TagString, LoadError);
    if (DataTable == nullptr)
    {
        return LoadError;
    }

    FString DevComment;
    Params->TryGetStringField(TEXT("dev_comment"), DevComment);

    // Use the tag string as the row name (DataTable row names are FName keys).
    const FName RowName(*TagString);

    // Build the row value and copy it in via the typed AddRow overload.
    FGameplayTagTableRow Row;
    Row.Tag = FName(*TagString);
    Row.DevComment = DevComment;

    const FScopedTransaction Transaction(LOCTEXT("MCP_AddGameplayTagRow", "MCP: Add Gameplay Tag Row"));
    DataTable->Modify();
    DataTable->AddRow(RowName, Row);
    DataTable->MarkPackageDirty();

    // Persist so the change survives editor restarts / reloads.
    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(DataTable->GetOutermost(), DataTable, SaveError);

    // Read back to verify the row landed with the expected values.
    bool bVerified = false;
    FString ReadTag;
    FString ReadComment;
    if (const FGameplayTagTableRow* Found = DataTable->FindRow<FGameplayTagTableRow>(RowName, TEXT("MCP_AddGameplayTagToTable")))
    {
        ReadTag = Found->Tag.ToString();
        ReadComment = Found->DevComment;
        bVerified = (Found->Tag == Row.Tag);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("table_path"), DataTable->GetPathName());
    ResultObj->SetStringField(TEXT("tag"), TagString);
    ResultObj->SetStringField(TEXT("row_name"), RowName.ToString());
    ResultObj->SetStringField(TEXT("dev_comment"), ReadComment);
    ResultObj->SetBoolField(TEXT("verified"), bVerified);
    ResultObj->SetBoolField(TEXT("saved"), bSaved);
    ResultObj->SetNumberField(TEXT("row_count"), DataTable->GetRowMap().Num());
    ResultObj->SetBoolField(TEXT("success"), bVerified);
    ResultObj->SetStringField(TEXT("message"), bVerified
        ? TEXT("Gameplay tag row added and verified by read-back")
        : TEXT("Gameplay tag row add could not be verified by read-back"));

    // The verdict measured above is the verdict EMITTED. The verdict is BOTH things: the operation
    // succeeded AND it persisted. If the save fails (e.g. a read-only file) the change stays only
    // in memory, and that is not a success.
    return mcp_common::MakeOutcomeReply(ResultObj, (bVerified) && bSaved,
        ((bVerified) && bSaved) ? FString() : mcp_common::SavedOutcomeError(TEXT("add_gameplay_tag_to_table")));
}

// MCP:remove_gameplay_tag_from_table - Remove a gameplay tag row (by tag, the row name) from a FGameplayTagTableRow data table.  Idempotent: a missing row reports already_absent without error.  The remove counterpart of add_gameplay_tag_to_table.
//   @param table_path - Full asset path of the gameplay tag data table.
//   @param tag - The gameplay tag string identifying the row to remove (used as the row name).
//   @returns { table_path: string, tag: string, row_name: string, removed: bool, already_absent: bool, row_count: int, saved: bool }
TSharedPtr<FJsonObject> HandleRemoveGameplayTagFromTable(const TSharedPtr<FJsonObject>& Params)
{
    FString TagString;
    TSharedPtr<FJsonObject> LoadError;
    UDataTable* DataTable = UEAgentGameplayTagsImpl::ReadTagTableRow(Params, TagString, LoadError);
    if (DataTable == nullptr)
    {
        return LoadError;
    }

    const FName RowName(*TagString);
    const bool bExisted = (DataTable->FindRow<FGameplayTagTableRow>(RowName, TEXT("MCP_RemoveGameplayTagFromTable")) != nullptr);

    bool bRemoved = false;
    bool bSaved = false;
    if (bExisted)
    {
        const FScopedTransaction Transaction(LOCTEXT("MCP_RemoveGameplayTagRow", "MCP: Remove Gameplay Tag Row"));
        DataTable->Modify();
        DataTable->RemoveRow(RowName);
        DataTable->MarkPackageDirty();

        FString SaveError;
        bSaved = mcp_common::SaveAssetPackage(DataTable->GetOutermost(), DataTable, SaveError);

        // Verify by read-back: the row must be gone.
        bRemoved = (DataTable->FindRow<FGameplayTagTableRow>(RowName, TEXT("MCP_RemoveGameplayTagFromTable_Verify")) == nullptr);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("table_path"), DataTable->GetPathName());
    ResultObj->SetStringField(TEXT("tag"), TagString);
    ResultObj->SetStringField(TEXT("row_name"), RowName.ToString());
    ResultObj->SetBoolField(TEXT("removed"), bRemoved);
    ResultObj->SetBoolField(TEXT("already_absent"), !bExisted);
    ResultObj->SetNumberField(TEXT("row_count"), DataTable->GetRowMap().Num());
    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("remove_gameplay_tag_from_table"));
}

// MCP:get_gameplay_tag_table_rows - List every tag declared in a gameplay tag DataTable (FGameplayTagTableRow), with its developer comment and whether the live tag manager has it registered -- what this table contributes to the project vocabulary, as opposed to query_gameplay_tags which returns the merged live registry from all sources. valid is false when any row's tag is not registered, and missing_tags names each such tag with its row key, so the table can be fixed or the tags registered. Read-only.
//   @param table_path - Full asset path of the gameplay tag data table
//   @returns { table_path: string, row_struct: string, rows: [{ row_name: string, tag: string, dev_comment: string, registered: bool }], row_count: int, valid: bool, missing_count: int, missing_tags: [{ tag_name: string, row_name: string }] }
TSharedPtr<FJsonObject> HandleGetGameplayTagTableRows(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentGameplayTagsImpl;

    FString TablePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("table_path"), TablePath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    TSharedPtr<FJsonObject> LoadError;
    UDataTable* Table = Impl::LoadTagTable(TablePath, LoadError);
    if (Table == nullptr)
    {
        return LoadError;
    }

    // Row memory is laid out as FGameplayTagTableRow (LoadTagTable checked the row struct).
    TArray<TSharedPtr<FJsonValue>> Rows;
    TArray<TSharedPtr<FJsonValue>> MissingTags;
    for (const TPair<FName, uint8*>& Entry : Table->GetRowMap())
    {
        if (Entry.Value == nullptr)
        {
            continue;
        }
        const FGameplayTagTableRow& TagRow = *reinterpret_cast<const FGameplayTagTableRow*>(Entry.Value);

        TSharedPtr<FJsonObject> RowJson = MakeShared<FJsonObject>();
        RowJson->SetStringField(TEXT("row_name"), Entry.Key.ToString());
        RowJson->SetStringField(TEXT("tag"), TagRow.Tag.ToString());
        RowJson->SetStringField(TEXT("dev_comment"), TagRow.DevComment);
        // Registered in the live manager? A row whose tag is not is reported in missing_tags under its row key.
        const FString TagName = TagRow.Tag.ToString();
        const bool bRegistered = !TagName.IsEmpty() && Impl::FindRegisteredTag(TagName).IsValid();
        RowJson->SetBoolField(TEXT("registered"), bRegistered);
        if (!TagName.IsEmpty() && !bRegistered)
        {
            TSharedPtr<FJsonObject> MissingEntry = MakeShared<FJsonObject>();
            MissingEntry->SetStringField(TEXT("tag_name"), TagName);
            MissingEntry->SetStringField(TEXT("row_name"), Entry.Key.ToString());
            MissingTags.Add(MakeShared<FJsonValueObject>(MissingEntry));
        }
        Rows.Add(MakeShared<FJsonValueObject>(RowJson));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("table_path"), Table->GetPathName());
    Data->SetStringField(TEXT("row_struct"), Table->RowStruct->GetName());
    Data->SetArrayField(TEXT("rows"), Rows);
    Data->SetNumberField(TEXT("row_count"), Rows.Num());
    Data->SetBoolField(TEXT("valid"), MissingTags.Num() == 0);
    Data->SetNumberField(TEXT("missing_count"), MissingTags.Num());
    Data->SetArrayField(TEXT("missing_tags"), MissingTags);
    return mcp_common::MakeOkReply(Data);
}

// MCP:remove_gameplay_tag - Delete a gameplay tag from its ini source list. Two cautions. Nothing tells you who was using it -- the engine exposes no tag reference count, so audit with find_blueprint_node_references and a binary grep of Content/ first, and add_gameplay_tag_redirect if anything still points at it. And like create_gameplay_tag this rewrites DefaultGameplayTags.ini wholesale, reordering it and dropping comments, so the oracle is `git diff` on the ini, not the success flag.
//   @param tag - Gameplay tag string to remove, e.g. MyGame.Temp
//   @returns { tag: string, success: bool, was_present: bool, delete_returned: bool, removed: bool, message: string }
TSharedPtr<FJsonObject> HandleRemoveGameplayTag(const TSharedPtr<FJsonObject>& Params)
{
    FString TagString;
    if (TSharedPtr<FJsonObject> Error = UEAgentGameplayTagsImpl::ReadTagParam(Params, TagString))
    {
        return Error;
    }

    UGameplayTagsManager& TagsManager = UGameplayTagsManager::Get();

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("tag"), TagString);

    // Was the tag registered before we attempt removal?
    const FGameplayTag ExistingTag = TagsManager.RequestGameplayTag(*TagString, false);
    if (!ExistingTag.IsValid())
    {
        return mcp_common::MakeErrorReply(TEXT("Tag is not registered; nothing to remove"));
    }

    // DeleteTagFromINI takes the tag's node from the manager. It removes the tag
    // from its INI source list, deletes redirectors, and refreshes the tag tree.
    // It returns false (and logs a notification) when the tag is implicit, has no
    // source, or is still referenced by content.
    TSharedPtr<FGameplayTagNode> TagNode = TagsManager.FindTagNode(ExistingTag);
    if (!TagNode.IsValid())
    {
        return mcp_common::MakeErrorReply(TEXT("Could not resolve a tag node for the tag; cannot remove from INI"));
    }

    const bool bDeleted = IGameplayTagsEditorModule::Get().DeleteTagFromINI(TagNode);

    // Verify by read-back: the tag should no longer be registered.
    const FGameplayTag AfterTag = TagsManager.RequestGameplayTag(*TagString, false);
    const bool bGone = !AfterTag.IsValid();

    ResultObj->SetBoolField(TEXT("was_present"), true);
    ResultObj->SetBoolField(TEXT("delete_returned"), bDeleted);
    ResultObj->SetBoolField(TEXT("removed"), bGone);
    ResultObj->SetBoolField(TEXT("success"), bGone);
    ResultObj->SetStringField(TEXT("message"), bGone
        ? TEXT("Gameplay tag removed from INI source (verified: tag no longer registered)")
        : TEXT("Failed to remove gameplay tag. It may be implicit, sourced from a non-INI list (native/DataTable), or still referenced by content."));

    // The verdict measured above is the verdict EMITTED.
    return mcp_common::MakeOutcomeReply(ResultObj, bGone,
        bGone ? FString() : mcp_common::MeasuredOutcomeError(TEXT("remove_gameplay_tag"), TEXT("bGone")));
}

// ===========================================================================
// Advanced container queries + redirects + validation
// ===========================================================================

namespace
{
    /**
     * Resolve a list of tag name strings from the JSON array field "tags" (or
     * "tag_names") into an FGameplayTagContainer. Returns false on the first
     * unknown tag, with OutError populated.
     */
    bool ParseTagNamesIntoContainer(
        const TSharedPtr<FJsonObject>& Params,
        const TCHAR* ArrayFieldName,
        FGameplayTagContainer& OutContainer,
        FString& OutError)
    {
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (!Params->TryGetArrayField(ArrayFieldName, Arr) || !Arr)
        {
            OutError = FString::Printf(TEXT("Missing required array field '%s'"), ArrayFieldName);
            return false;
        }
        for (const TSharedPtr<FJsonValue>& V : *Arr)
        {
            FString Name;
            if (!V.IsValid() || !V->TryGetString(Name) || Name.IsEmpty())
            {
                OutError = TEXT("Empty or non-string entry in tags array");
                return false;
            }
            const FGameplayTag T = UEAgentGameplayTagsImpl::FindRegisteredTag(Name);
            if (!T.IsValid())
            {
                OutError = FString::Printf(TEXT("Unknown tag: %s"), *Name);
                return false;
            }
            OutContainer.AddTag(T);
        }
        return true;
    }

    /** Helper: look up an FGameplayTagContainer on an object (UPROPERTY scan). */
    bool FindOwnedContainerProperty(UObject* Obj, FProperty*& OutProp, void*& OutValuePtr)
    {
        if (!Obj) { return false; }
        for (TFieldIterator<FProperty> It(Obj->GetClass()); It; ++It)
        {
            FProperty* P = *It;
            if (FStructProperty* SP = CastField<FStructProperty>(P))
            {
                if (SP->Struct == FGameplayTagContainer::StaticStruct())
                {
                    OutProp = P;
                    OutValuePtr = P->ContainerPtrToValuePtr<void>(Obj);
                    return true;
                }
            }
        }
        return false;
    }

    // The object a has_*_gameplay_tags command names, the tags it asks about and the tags the object owns.
    struct FGameplayTagsHasRequest
    {
        FString ObjectPath;
        FGameplayTagContainer Requested;
        FGameplayTagContainer Owned;
    };

    // Reads object_path and the non-empty tags array, then the tags the object owns: through IGameplayTagAssetInterface,
    // else from its first FGameplayTagContainer property. The error reply when a param is missing or does not resolve.
    TSharedPtr<FJsonObject> GameplayTags_ReadHasRequest(const TSharedPtr<FJsonObject>& Params, FGameplayTagsHasRequest& Out)
    {
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("object_path"), Out.ObjectPath))
        {
            return Missing;
        }
        UObject* Obj = UEAgentGameplayTagsImpl::ResolveTagHolder(Out.ObjectPath);
        if (!Obj)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Cannot load object: %s"), *Out.ObjectPath));
        }

        FString Err;
        if (!ParseTagNamesIntoContainer(Params, TEXT("tags"), Out.Requested, Err))
        {
            return mcp_common::MakeErrorReply(Err);
        }
        if (Out.Requested.IsEmpty())
        {
            return mcp_common::MakeErrorReply(TEXT("'tags' array is empty"));
        }

        if (IGameplayTagAssetInterface* TagIface = Cast<IGameplayTagAssetInterface>(Obj))
        {
            TagIface->GetOwnedGameplayTags(Out.Owned);
        }
        else
        {
            FProperty* P = nullptr;
            void* Ptr = nullptr;
            if (FindOwnedContainerProperty(Obj, P, Ptr))
            {
                Out.Owned = *static_cast<FGameplayTagContainer*>(Ptr);
            }
        }
        return nullptr;
    }

    // Writes the redirect list to DefaultGameplayTags.ini, its source of truth. FGameplayTagRedirectors::RefreshTagRedirects
    // is not exported, so the tag manager picks the change up on its next init or settings reimport, or an editor restart.
    void GameplayTags_SaveRedirects(UGameplayTagsSettings* Settings)
    {
        Settings->TryUpdateDefaultConfigFile();
    }
}

// ---------------------------------------------------------------------------
// has_gameplay_tags (match any / all / none)
// ---------------------------------------------------------------------------
// MCP:has_gameplay_tags - Test the tags an object owns (through IGameplayTagAssetInterface, else its first FGameplayTagContainer property) against a list: match "any" (default, OR) -- the "is it any kind of X" gate: any Damage.*, any Status.*; "all" (AND) -- the multi-condition gate: alive AND armed AND not restrained; "none" -- the blocker/immunity check: not stunned, not already burning (computed as !HasAny, so a true means "clear to proceed", not "has them"). A single tag is a one-element list. Hierarchy matching is ASYMMETRIC and this is where it bites: a requested FAMILY accepts its descendants, a requested LEAF does not accept the family, so swapping the two silently turns an always-true gate into an always-false one. When in doubt request leaf tags, which match either way. owned_tags comes back every time, so a surprising answer is diagnosable in the same call. An unregistered tag name is an error (check it with get_gameplay_tag_info).
//   @param object_path - Path of the UObject to inspect
//   @param tags [array!] - Non-empty array of tag-name strings (e.g. ["Combat.Hit","Character.State.Alive"])
//   @param match - Optional "any" (default) | "all" | "none": OR, AND, or the negation of OR
//   @returns { object_path: string, match: string, result: bool, has_any?: bool, has_all?: bool, has_none?: bool, requested: [string], owned_tags: [string] }
TSharedPtr<FJsonObject> HandleHasAnyGameplayTags(const TSharedPtr<FJsonObject>& Params)
{
    // The predicate is checked before the object is resolved: a bad match changes nothing and needs no object.
    FString Match = TEXT("any");
    Params->TryGetStringField(TEXT("match"), Match);
    Match.TrimStartAndEndInline();
    Match.ToLowerInline();
    if (Match.IsEmpty())
    {
        Match = TEXT("any");
    }
    if (Match != TEXT("any") && Match != TEXT("all") && Match != TEXT("none"))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("match must be \"any\", \"all\" or \"none\", got \"%s\""), *Match));
    }

    FGameplayTagsHasRequest Request;
    if (TSharedPtr<FJsonObject> Error = GameplayTags_ReadHasRequest(Params, Request))
    {
        return Error;
    }

    // FGameplayTagContainer has no HasNone: owning none of the tags is !HasAny.
    bool bResult = false;
    const TCHAR* ResultField = TEXT("has_any");
    if (Match == TEXT("all"))
    {
        bResult = Request.Owned.HasAll(Request.Requested);
        ResultField = TEXT("has_all");
    }
    else if (Match == TEXT("none"))
    {
        bResult = !Request.Owned.HasAny(Request.Requested);
        ResultField = TEXT("has_none");
    }
    else
    {
        bResult = Request.Owned.HasAny(Request.Requested);
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("object_path"), Request.ObjectPath);
    Data->SetStringField(TEXT("match"), Match);
    Data->SetBoolField(TEXT("result"), bResult);
    Data->SetBoolField(ResultField, bResult);
    Data->SetArrayField(TEXT("requested"), mcp_common::GameplayTagsToJson(Request.Requested));
    Data->SetArrayField(TEXT("owned_tags"), mcp_common::GameplayTagsToJson(Request.Owned));
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// add_gameplay_tag_redirect  / remove_gameplay_tag_redirect
// ---------------------------------------------------------------------------
// MCP:add_gameplay_tag_redirect - Add an entry to UGameplayTagsSettings::GameplayTagRedirects (the persistent redirect config), then call FGameplayTagRedirectors::RefreshTagRedirects() so the runtime map picks it up. Idempotent (same Old->New pair returns already_present=true). Requires a ConfigRestartRequired=true flag — UGameplayTagsSettings flags it as such; callers should note that the running game still resolves Old to itself until next process restart in Shipping, but in the editor the redirect is live immediately.
//   @param old_tag_name - Source tag name (FName).
//   @param new_tag_name - Destination tag name (FName).
//   @returns { old_tag_name, new_tag_name, added: bool, already_present: bool, total_redirects: int, needs_restart: bool }
TSharedPtr<FJsonObject> HandleAddGameplayTagRedirect(const TSharedPtr<FJsonObject>& Params)
{
    FString OldName, NewName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("old_tag_name"), OldName)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("new_tag_name"), NewName)) { return Missing; }

    UGameplayTagsSettings* Settings = GetMutableDefault<UGameplayTagsSettings>();
    if (!Settings)
    {
        return mcp_common::MakeErrorReply(TEXT("UGameplayTagsSettings CDO unavailable"));
    }

    // Idempotent: skip if the exact pair already exists.
    bool bAlready = false;
    for (const FGameplayTagRedirect& R : Settings->GameplayTagRedirects)
    {
        if (R.OldTagName == FName(*OldName) && R.NewTagName == FName(*NewName))
        {
            bAlready = true;
            break;
        }
    }
    if (!bAlready)
    {
        FGameplayTagRedirect NewEntry;
        NewEntry.OldTagName = FName(*OldName);
        NewEntry.NewTagName = FName(*NewName);
        Settings->GameplayTagRedirects.Add(NewEntry);
        GameplayTags_SaveRedirects(Settings);
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("old_tag_name"), OldName);
    Data->SetStringField(TEXT("new_tag_name"), NewName);
    Data->SetBoolField(TEXT("added"), !bAlready);
    Data->SetBoolField(TEXT("already_present"), bAlready);
    Data->SetNumberField(TEXT("total_redirects"), Settings->GameplayTagRedirects.Num());
    // The redirect list is a ConfigRestartRequired setting: outside the editor it needs a restart.
    Data->SetBoolField(TEXT("needs_restart"), !GIsEditor);
    return mcp_common::MakeOkReply(Data);
}

// MCP:remove_gameplay_tag_redirect - Delete the gameplay tag redirect(s) that rewrite a given old tag name -- cleanup once every asset has been resaved and nothing references the old name any more. Removing it too early silently breaks whatever still points at the old tag, since an unresolved tag becomes empty rather than an error. If the same old name was rewritten more than once over time there will be several entries: remove_all=true clears them all, and removed_count tells you how many went.
//   @param old_tag_name - Source tag name to unmap.
//   @param remove_all - If true (default false), remove every redirect with that OldTagName.
//   @returns { old_tag_name, removed_count: int, already_absent: bool, total_redirects: int }
TSharedPtr<FJsonObject> HandleRemoveGameplayTagRedirect(const TSharedPtr<FJsonObject>& Params)
{
    FString OldName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("old_tag_name"), OldName)) { return Missing; }
    bool bRemoveAll = false;
    Params->TryGetBoolField(TEXT("remove_all"), bRemoveAll);

    UGameplayTagsSettings* Settings = GetMutableDefault<UGameplayTagsSettings>();
    if (!Settings)
    {
        return mcp_common::MakeErrorReply(TEXT("UGameplayTagsSettings CDO unavailable"));
    }

    const int32 Before = Settings->GameplayTagRedirects.Num();
    if (bRemoveAll)
    {
        Settings->GameplayTagRedirects.RemoveAll([&](const FGameplayTagRedirect& R) { return R.OldTagName == FName(*OldName); });
    }
    else
    {
        // Remove first match only.
        for (int32 i = 0; i < Settings->GameplayTagRedirects.Num(); ++i)
        {
            if (Settings->GameplayTagRedirects[i].OldTagName == FName(*OldName))
            {
                Settings->GameplayTagRedirects.RemoveAt(i);
                break;
            }
        }
    }
    const int32 Removed = Before - Settings->GameplayTagRedirects.Num();

    if (Removed > 0)
    {
        GameplayTags_SaveRedirects(Settings);
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("old_tag_name"), OldName);
    Data->SetNumberField(TEXT("removed_count"), Removed);
    Data->SetBoolField(TEXT("already_absent"), Removed == 0);
    Data->SetNumberField(TEXT("total_redirects"), Settings->GameplayTagRedirects.Num());
    return mcp_common::MakeOkReply(Data);
}

// MCP:get_gameplay_tag_redirects - List every entry configured in UGameplayTagsSettings::GameplayTagRedirects (the persistent Old->New redirect config). Read-only reader counterpart of add_gameplay_tag_redirect / remove_gameplay_tag_redirect; does NOT mutate or save.
//   @returns { redirects: [{ old_tag_name, new_tag_name }], count: int }
TSharedPtr<FJsonObject> HandleGetGameplayTagRedirects(const TSharedPtr<FJsonObject>& Params)
{
    UGameplayTagsSettings* Settings = GetMutableDefault<UGameplayTagsSettings>();
    if (!Settings)
    {
        return mcp_common::MakeErrorReply(TEXT("UGameplayTagsSettings CDO unavailable"));
    }

    TArray<TSharedPtr<FJsonValue>> RedirectsJson;
    for (const FGameplayTagRedirect& R : Settings->GameplayTagRedirects)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("old_tag_name"), R.OldTagName.ToString());
        Entry->SetStringField(TEXT("new_tag_name"), R.NewTagName.ToString());
        RedirectsJson.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("redirects"), RedirectsJson);
    Data->SetNumberField(TEXT("count"), RedirectsJson.Num());
    return mcp_common::MakeOkReply(Data);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_gameplay_tag_redirect"), &HandleAddGameplayTagRedirect },
    { TEXT("add_gameplay_tag_to_table"), &HandleAddGameplayTagToTable },
    { TEXT("add_tag_to_object"), &HandleAddTagToObject },
    { TEXT("create_gameplay_tag"), &HandleCreateGameplayTag },
    { TEXT("create_gameplay_tag_table"), &HandleCreateGameplayTagTable },
    { TEXT("get_gameplay_tag_info"), &HandleGetGameplayTagInfo },
    { TEXT("get_gameplay_tag_redirects"), &HandleGetGameplayTagRedirects },
    { TEXT("get_gameplay_tag_table_rows"), &HandleGetGameplayTagTableRows },
    { TEXT("get_object_tags"), &HandleGetObjectTags },
    { TEXT("has_gameplay_tags"), &HandleHasAnyGameplayTags },
    { TEXT("query_gameplay_tags"), &HandleQueryGameplayTags },
    { TEXT("remove_gameplay_tag"), &HandleRemoveGameplayTag },
    { TEXT("remove_gameplay_tag_from_table"), &HandleRemoveGameplayTagFromTable },
    { TEXT("remove_gameplay_tag_redirect"), &HandleRemoveGameplayTagRedirect },
    { TEXT("remove_tag_from_object"), &HandleRemoveTagFromObject },
};
} // namespace UEAgentGameplayTagsCommands

#undef LOCTEXT_NAMESPACE
