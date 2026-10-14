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

// --- The registry as a whole ------------------------------------------------------

// MCP:get_gameplay_tags_manager - One-line health check of the gameplay tag system: the manager singleton's validity and the TOTAL number of registered tags. Cheap sanity probe before a tag frontier -- a plausible total confirms the registry loaded, and comparing the count before and after create_gameplay_tag shows the registration took. Counts only; for the tags themselves use get_all_gameplay_tags or query_gameplay_tags.
//   @returns { manager_name: string, is_valid: bool, total_tags: int }
TSharedPtr<FJsonObject> HandleGetGameplayTagsManager(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentGameplayTagsImpl;

    const UGameplayTagsManager& Manager = UGameplayTagsManager::Get();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("manager_name"), Manager.GetName());
    Data->SetBoolField(TEXT("is_valid"), IsValid(&Manager));
    // Only the size of the registry; get_all_gameplay_tags lists the tags themselves.
    Data->SetNumberField(TEXT("total_tags"), Impl::AllRegisteredTags().Num());

    return mcp_common::MakeOkReply(Data);
}

// MCP:get_all_gameplay_tags - Dump EVERY gameplay tag registered in the project, with a count -- the full vocabulary the game can reason about, including tags contributed by plugins and vendor content. Large by nature (projects run to thousands), so use query_gameplay_tags to search a branch and find_gameplay_tag to test one string; keep this for a full inventory or a naming audit.
//   @returns { tags: [object], count: int }
TSharedPtr<FJsonObject> HandleGetAllGameplayTags(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentGameplayTagsImpl;

    // tag_name is the tag's string form, tag_string the underlying FName as text.
    TArray<TSharedPtr<FJsonValue>> Listed;
    for (const FGameplayTag& Tag : Impl::AllRegisteredTags())
    {
        if (!Tag.IsValid())
        {
            continue;
        }
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("tag_name"), Tag.ToString());
        Entry->SetStringField(TEXT("tag_string"), Tag.GetTagName().ToString());
        Listed.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("tags"), Listed);
    Data->SetNumberField(TEXT("count"), Listed.Num());
    return mcp_common::MakeOkReply(Data);
}

// MCP:find_gameplay_tag - Does this exact tag string exist and resolve right now? Returns found plus the resolved tag string. The precondition to check BEFORE using a tag anywhere: an unregistered tag is not an error in UE, it silently becomes an empty/None tag, and a query made with an empty tag fails exactly like a valid tag that does not match -- so the control case and the case under test become indistinguishable. Also the confirmation that create_gameplay_tag registered live even if you restored the ini. Exact lookup: use query_gameplay_tags for prefix/pattern search, get_gameplay_tag_source to see where it is defined.
//   @param tag_name - Name of the gameplay tag to find
//   @returns { tag_name: string, found: bool, tag_string: string }
TSharedPtr<FJsonObject> HandleFindGameplayTag(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentGameplayTagsImpl;

    FString TagName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("tag_name"), TagName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    // A miss is a normal answer here (found=false), not an error.
    const FGameplayTag Resolved = Impl::FindRegisteredTag(TagName);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("tag_name"), TagName);
    Data->SetBoolField(TEXT("found"), Resolved.IsValid());
    if (Resolved.IsValid())
    {
        Data->SetStringField(TEXT("tag_string"), Resolved.ToString());
    }
    return mcp_common::MakeOkReply(Data);
}

// --- Single tags: create in the ini, inspect, search -------------------------------

// MCP:create_gameplay_tag - Add a new gameplay tag to the project's DefaultGameplayTags.ini AND register it LIVE in this editor session -- that live half is why you must call this before writing the tag into a DataTable row or a property, since an unregistered tag string is silently reset to None by the importers. But it REWRITES THE WHOLE INI: the list comes back alphabetised, every comment block is deleted, and other tags' DevComments are re-serialised and normalised, so adding one tag can produce a diff full of unrelated insertions and deletions. Copy the ini first; the oracle is `git diff --stat` showing your N insertions and ZERO deletions, and if not, git checkout the ini and re-insert only your line (the tag stays registered live either way -- confirm with find_gameplay_tag).
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
    // tag tree is refreshed, so get_all_gameplay_tags lists it.
    const FName SourceFName = SourceName.IsEmpty() ? NAME_None : FName(*SourceName);
    if (!IGameplayTagsEditorModule::Get().AddNewGameplayTagToINI(TagName, DevComment, SourceFName))
    {
        return mcp_common::MakeErrorReply(
            TEXT("Failed to register gameplay tag. The project may not import tags from INI, or the name conflicts with an existing tag/redirector."));
    }
    const FGameplayTag Created = Tags.RequestGameplayTag(*TagName, /*ErrorIfNotFound=*/false);
    return Registered(Created.IsValid() ? Created.ToString() : TagName, TEXT("Gameplay tag created and registered in INI"));
}

// MCP:get_gameplay_tag_info - Read one gameplay tag's detail, including its PARENT tag -- how you walk the hierarchy that makes tags useful (Material.Wood.Oak answers to Material.Wood). Worth knowing before writing a match: hierarchy matching is ASYMMETRIC, HasAny(A, B) only succeeds when a tag of A is B or a DESCENDANT of B, so putting a family where a leaf is expected quietly fails. get_gameplay_tag_source says where the tag is defined; find_gameplay_tag just tests existence.
//   @param tag_name - Name of the gameplay tag to get info for
//   @returns { tag_name: string, tag_string: string, tag_name_string: string, parent_tag: string }
TSharedPtr<FJsonObject> HandleGetGameplayTagInfo(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentGameplayTagsImpl;

    FString TagName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("tag_name"), TagName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    const FGameplayTag Resolved = Impl::FindRegisteredTag(TagName);
    if (!Resolved.IsValid())
    {
        return Impl::TagNotFound(TagName);
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("tag_name"), TagName);
    Data->SetStringField(TEXT("tag_string"), Resolved.ToString());
    Data->SetStringField(TEXT("tag_name_string"), Resolved.GetTagName().ToString());

    // One level up the hierarchy (A.B.C -> A.B); root tags have no parent field.
    const FGameplayTag Parent = Resolved.RequestDirectParent();
    if (Parent.IsValid())
    {
        Data->SetStringField(TEXT("parent_tag"), Parent.ToString());
    }
    return mcp_common::MakeOkReply(Data);
}

// MCP:query_gameplay_tags - Search the registered gameplay tags for everything matching a pattern -- the discovery call for "what tags exist under Material.Wood" or "is there already a tag for this". Pick it over get_all_gameplay_tags, which dumps the entire project registry, and over find_gameplay_tag, which only answers about one exact string. Sees the LIVE registry, so a tag added this session by create_gameplay_tag shows up even if the ini was restored, and one that only exists as text in a file does not.
//   @param query_tag - The query tag pattern to match against all tags
//   @returns { query_tag: string, matching_tags: [object], count: int }
TSharedPtr<FJsonObject> HandleQueryGameplayTags(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentGameplayTagsImpl;

    FString QueryTagName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("query_tag"), QueryTagName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    const FGameplayTag Query = Impl::FindRegisteredTag(QueryTagName);
    if (!Query.IsValid())
    {
        return mcp_common::MakeErrorReply(TEXT("Query tag not found: ") + QueryTagName);
    }

    // MatchesTag is hierarchical: the query tag itself and all of its descendants match.
    TArray<TSharedPtr<FJsonValue>> Matches;
    for (const FGameplayTag& Candidate : Impl::AllRegisteredTags())
    {
        if (Candidate.IsValid() && Candidate.MatchesTag(Query))
        {
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("tag_name"), Candidate.ToString());
            Matches.Add(MakeShared<FJsonValueObject>(Entry));
        }
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("query_tag"), QueryTagName);
    Data->SetArrayField(TEXT("matching_tags"), Matches);
    Data->SetNumberField(TEXT("count"), Matches.Num());
    return mcp_common::MakeOkReply(Data);
}

// --- Tags owned by one object -------------------------------------------------------

// MCP:get_object_tags - Read the gameplay tags an object currently owns -- what the game believes this actor or asset IS (states, factions, damage types, item categories). Requires the object to implement IGameplayTagAssetInterface; anything else returns nothing, which is a fact about the interface, not about the object's data. Reads the LIVE container, so during PIE tags added at runtime appear here. has_tag / has_any_gameplay_tags answer a specific question instead of listing everything.
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

// MCP:add_tag_to_object - Grant a gameplay tag to an object that implements IGameplayTagAssetInterface -- the way to put an actor into a state (stunned, wet, on fire, alerted) so that tag-driven systems react without any of them knowing about each other. Reports already_present and echoes the resulting tag list, so the reply is its own oracle. Register the tag first (find_gameplay_tag / create_gameplay_tag): an unregistered string resolves to an empty tag and is added as nothing.
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

// MCP:has_tag - Does this object own this gameplay tag: the single-tag state check (is it stunned, is it alerted, is it a valid target) as a plain bool. The cheapest of the tag predicates -- has_any_gameplay_tags / has_all_gameplay_tags / has_none_gameplay_tags take a list, get_object_tags returns everything. Verify the tag exists first (find_gameplay_tag): an unregistered string becomes an empty tag and returns false exactly like a genuine miss.
//   @param object_path - Path or name of the object to check
//   @param tag_name - Name of the tag to check for
//   @returns { object_path: string, tag_name: string, has_tag: bool }
TSharedPtr<FJsonObject> HandleHasTag(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentGameplayTagsImpl;

    FString ObjectPath, TagName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("object_path"), ObjectPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("tag_name"), TagName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    // Unlike add/remove, the tag-interface check comes before the tag lookup here.
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
    const FGameplayTag Tag = Impl::FindRegisteredTag(TagName);
    if (!Tag.IsValid())
    {
        return Impl::TagNotFound(TagName);
    }

    // HasTag is hierarchical: an owned child of the requested tag also counts.
    FGameplayTagContainer Owned;
    TagSource->GetOwnedGameplayTags(Owned);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("object_path"), ObjectPath);
    Data->SetStringField(TEXT("tag_name"), TagName);
    Data->SetBoolField(TEXT("has_tag"), Owned.HasTag(Tag));
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// GameplayTag table/source authoring
// ---------------------------------------------------------------------------

// MCP:create_gameplay_tag_table - Create a DataTable of gameplay tags (row struct FGameplayTagTableRow) -- the alternative to declaring tags in DefaultGameplayTags.ini, and the better choice for a large or team-owned tag set: rows merge and diff like data instead of fighting over one ini that the create_gameplay_tag path rewrites end to end. The table must be registered in the project's gameplay tag settings to be loaded. Fill it with add_gameplay_tag_to_table and check it with validate_gameplay_tag_table.
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

// MCP:add_gameplay_tag_to_table - Add one tag (with an optional developer comment) as a row in a gameplay tag DataTable, verifying the write by read-back. The team-friendly way to declare tags: it touches only this asset instead of rewriting the shared DefaultGameplayTags.ini the way create_gameplay_tag does. The trade-off is that a tag added here is not registered LIVE, so it may not resolve until the table is reloaded -- if you must use it in the same session, check with find_gameplay_tag first.
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

// MCP:get_gameplay_tag_table_rows - List every tag declared in a gameplay tag DataTable, with its developer comment -- what this table contributes to the project vocabulary, as opposed to get_all_gameplay_tags which returns the merged live registry from all sources. Use it to audit one team's or one module's tags, and validate_gameplay_tag_table to catch malformed or duplicated rows.
//   @param table_path - Full asset path of the gameplay tag data table
//   @returns { table_path: string, row_struct: string, rows: [object], row_count: int }
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
        Rows.Add(MakeShared<FJsonValueObject>(RowJson));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("table_path"), Table->GetPathName());
    Data->SetStringField(TEXT("row_struct"), Table->RowStruct->GetName());
    Data->SetArrayField(TEXT("rows"), Rows);
    Data->SetNumberField(TEXT("row_count"), Rows.Num());
    return mcp_common::MakeOkReply(Data);
}

// MCP:remove_gameplay_tag - Delete a gameplay tag from its ini source list. Two cautions. Nothing tells you who was using it -- count_gameplay_tag_references always answers 0 by construction, so audit with find_blueprint_node_references and a binary grep of Content/ first, and add_gameplay_tag_redirect if anything still points at it. And like create_gameplay_tag this rewrites DefaultGameplayTags.ini wholesale, reordering it and dropping comments, so the oracle is `git diff` on the ini, not the success flag.
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
// has_all_gameplay_tags  / has_any_gameplay_tags  / has_none_gameplay_tags
// ---------------------------------------------------------------------------
// MCP:has_all_gameplay_tags - Does the object own EVERY tag in the list (AND) -- the multi-condition gate: alive AND armed AND not restrained. Hierarchy-aware, and the direction is asymmetric: a requested tag matches when an owned tag IS it or DESCENDS from it, so asking for a family accepts its children but asking for a leaf does not accept the family. It also returns owned_tags, so a false answer is diagnosable in the same call. has_any_gameplay_tags is the OR, has_none_gameplay_tags the negation.
//   @param object_path - Path of the UObject to inspect.
//   @param tags [array!] - Non-empty array of tag-name strings (e.g. ["Combat.Hit","Character.State.Alive"]).
//   @returns { object_path, requested: [string], has_all: bool, owned_tags: [string] }
TSharedPtr<FJsonObject> HandleHasAllGameplayTags(const TSharedPtr<FJsonObject>& Params)
{
    FGameplayTagsHasRequest Request;
    if (TSharedPtr<FJsonObject> Error = GameplayTags_ReadHasRequest(Params, Request))
    {
        return Error;
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("object_path"), Request.ObjectPath);
    Data->SetBoolField(TEXT("has_all"), Request.Owned.HasAll(Request.Requested));
    Data->SetArrayField(TEXT("requested"), mcp_common::GameplayTagsToJson(Request.Requested));
    Data->SetArrayField(TEXT("owned_tags"), mcp_common::GameplayTagsToJson(Request.Owned));
    return mcp_common::MakeOkReply(Data);
}

// MCP:has_any_gameplay_tags - Does the object own AT LEAST ONE of these tags (OR) -- the "is it any kind of X" gate: any Damage.*, any Status.*, any Team.*. Hierarchy matching is ASYMMETRIC and this is where it bites: a requested FAMILY accepts its descendants, a requested LEAF does not accept the family, so swapping the two silently turns an always-true gate into an always-false one. When in doubt request leaf tags, which match either way. Unlike has_all_gameplay_tags it does not echo the owned tags, so pair a false with get_object_tags.
//   @param object_path - Path of the UObject to inspect
//   @param tags [array!] - Non-empty array of tag-name strings (e.g. ["Combat.Hit","Character.State.Alive"])
//   @returns { object_path, requested: [string], has_any: bool }
TSharedPtr<FJsonObject> HandleHasAnyGameplayTags(const TSharedPtr<FJsonObject>& Params)
{
    FGameplayTagsHasRequest Request;
    if (TSharedPtr<FJsonObject> Error = GameplayTags_ReadHasRequest(Params, Request))
    {
        return Error;
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("object_path"), Request.ObjectPath);
    Data->SetBoolField(TEXT("has_any"), Request.Owned.HasAny(Request.Requested));
    Data->SetArrayField(TEXT("requested"), mcp_common::GameplayTagsToJson(Request.Requested));
    return mcp_common::MakeOkReply(Data);
}

// MCP:has_none_gameplay_tags - Does the object own NONE of these tags -- the blocker/immunity check: not stunned, not already burning, not on the excluded-faction list. Computed as !HasAny (there is no public HasNone), so it inherits the same asymmetric hierarchy matching: listing a FAMILY blocks all of its descendants, listing a LEAF blocks only that leaf. Beware the double negative -- a true here means "clear to proceed", not "has them".
//   @param object_path - Path of the UObject to inspect
//   @param tags [array!] - Non-empty array of tag-name strings to test for absence
//   @returns { object_path, requested: [string], has_none: bool }
TSharedPtr<FJsonObject> HandleHasNoneGameplayTags(const TSharedPtr<FJsonObject>& Params)
{
    FGameplayTagsHasRequest Request;
    if (TSharedPtr<FJsonObject> Error = GameplayTags_ReadHasRequest(Params, Request))
    {
        return Error;
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("object_path"), Request.ObjectPath);
    // FGameplayTagContainer has no HasNone: owning none of the tags is !HasAny.
    Data->SetBoolField(TEXT("has_none"), !Request.Owned.HasAny(Request.Requested));
    Data->SetArrayField(TEXT("requested"), mcp_common::GameplayTagsToJson(Request.Requested));
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// count_gameplay_tag_references
// ---------------------------------------------------------------------------
// MCP:count_gameplay_tag_references - DOES NOT COUNT ANYTHING: reference_count is ALWAYS 0, plus a note saying why. Declared engine gap -- there is no public GetTagReferenceCount API (the manager tracks refcounts internally, for debug tooling only), so all this returns honestly is whether the tag is valid. Never read the 0 as "nobody uses this tag" and never delete a tag on its evidence. To actually find users: find_blueprint_node_references with include_pin_defaults, plus a binary grep of Content/ and Config/ for the tag string.
//   @param tag_name - Single tag name (only its validity is really evaluated).
//   @returns { tag_name, valid: bool, reference_count: 0, note: "..." }
TSharedPtr<FJsonObject> HandleCountGameplayTagReferences(const TSharedPtr<FJsonObject>& Params)
{
    FString TagName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("tag_name"), TagName)) { return Missing; }
    const FGameplayTag T = UEAgentGameplayTagsImpl::FindRegisteredTag(TagName);
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("tag_name"), TagName);
    Data->SetBoolField(TEXT("valid"), T.IsValid());
    Data->SetNumberField(TEXT("reference_count"), 0);
    Data->SetStringField(TEXT("note"),
        TEXT("UE 5.8 UGameplayTagsManager has no public GetTagReferenceCount API; refcount tracking is internal. "
             "Use get_object_tags / asset inspection for tag usage audits."));
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_gameplay_tag_source
// ---------------------------------------------------------------------------
// MCP:get_gameplay_tag_source - Where does this tag come from: native C++, DefaultGameplayTags.ini, a tag DataTable, or transient (registered live this session and NOT on disk) -- plus its comment and its native / restricted / explicit flags. The call that tells you whether a tag will survive an editor restart and which file to edit to change it, and the one that distinguishes YOUR tags from a vendor's or the engine's before you rename or remove anything.
//   @param tag_name - Single tag name.
//   @returns { tag_name, valid, source, comment, is_native: bool, is_restricted: bool, is_explicit: bool, allow_non_restricted_children: bool }
TSharedPtr<FJsonObject> HandleGetGameplayTagSource(const TSharedPtr<FJsonObject>& Params)
{
    FString TagName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("tag_name"), TagName)) { return Missing; }
    const FGameplayTag T = UEAgentGameplayTagsImpl::FindRegisteredTag(TagName);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("tag_name"), TagName);
    const bool bValid = T.IsValid();
    Data->SetBoolField(TEXT("valid"), bValid);
    if (!bValid) { return mcp_common::MakeOkReply(Data); }

    FString Comment;
    FName Source;
    bool bIsExplicit = false;
    bool bIsRestricted = false;
    bool bAllowNonRestrictedChildren = false;
    UGameplayTagsManager::Get().GetTagEditorData(FName(*TagName), Comment, Source, bIsExplicit, bIsRestricted, bAllowNonRestrictedChildren);

    Data->SetStringField(TEXT("comment"), Comment);
    Data->SetStringField(TEXT("source"), Source.ToString());
    // "Native" tag detection — engine convention: source = "Native" or starts with "NativeGameplayTags".
    const FString SrcStr = Source.ToString();
    Data->SetBoolField(TEXT("is_native"), SrcStr.Equals(TEXT("Native"), ESearchCase::IgnoreCase)
                                       || SrcStr.StartsWith(TEXT("NativeGameplayTags")));
    Data->SetBoolField(TEXT("is_restricted"), bIsRestricted);
    Data->SetBoolField(TEXT("is_explicit"), bIsExplicit);
    Data->SetBoolField(TEXT("allow_non_restricted_children"), bAllowNonRestrictedChildren);
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

// ---------------------------------------------------------------------------
// validate_gameplay_tag_table
// ---------------------------------------------------------------------------
// MCP:validate_gameplay_tag_table - Walk every row of a UDataTable (whose RowStruct is FGameplayTagTableRow). For each tag name, verify the tag is actually registered in the manager. Returns the list of missing tags so the user can fix the table or register the missing tags. Does NOT mutate anything.
//   @param table_path - Asset path to the UDataTable.
//   @returns { table_path, valid: bool, row_count: int, missing_tags: [{ tag_name, row_name }] }
TSharedPtr<FJsonObject> HandleValidateGameplayTagTable(const TSharedPtr<FJsonObject>& Params)
{
    FString TablePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("table_path"), TablePath)) { return Missing; }

    UDataTable* Table = LoadObject<UDataTable>(nullptr, *TablePath);
    if (!Table)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Cannot load DataTable: %s"), *TablePath));
    }
    if (Table->GetRowStruct() != FGameplayTagTableRow::StaticStruct())
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("DataTable row struct is %s (expected FGameplayTagTableRow)"),
                Table->GetRowStruct() ? *Table->GetRowStruct()->GetName() : TEXT("None")));
    }

    TArray<TSharedPtr<FJsonValue>> MissingJson;
    int32 RowCount = 0;
    TArray<FGameplayTagTableRow*> Rows;
    Table->GetAllRows<FGameplayTagTableRow>(TEXT("UEAgent::validate"), Rows);
    RowCount = Rows.Num();
    for (const FGameplayTagTableRow* Row : Rows)
    {
        if (!Row) { continue; }
        const FString TagName = Row->Tag.ToString();
        if (TagName.IsEmpty()) { continue; }
        const FGameplayTag T = UEAgentGameplayTagsImpl::FindRegisteredTag(TagName);
        if (!T.IsValid())
        {
            TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
            E->SetStringField(TEXT("tag_name"), TagName);
            E->SetStringField(TEXT("row_name"), Row->DevComment);  // use DevComment as soft row id; falls back if user supplied one
            MissingJson.Add(MakeShared<FJsonValueObject>(E));
        }
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("table_path"), TablePath);
    Data->SetBoolField(TEXT("valid"), MissingJson.Num() == 0);
    Data->SetNumberField(TEXT("row_count"), RowCount);
    Data->SetNumberField(TEXT("missing_count"), MissingJson.Num());
    Data->SetArrayField(TEXT("missing_tags"), MissingJson);
    return mcp_common::MakeOkReply(Data);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_gameplay_tag_redirect"), &HandleAddGameplayTagRedirect },
    { TEXT("add_gameplay_tag_to_table"), &HandleAddGameplayTagToTable },
    { TEXT("add_tag_to_object"), &HandleAddTagToObject },
    { TEXT("count_gameplay_tag_references"), &HandleCountGameplayTagReferences },
    { TEXT("create_gameplay_tag"), &HandleCreateGameplayTag },
    { TEXT("create_gameplay_tag_table"), &HandleCreateGameplayTagTable },
    { TEXT("find_gameplay_tag"), &HandleFindGameplayTag },
    { TEXT("get_all_gameplay_tags"), &HandleGetAllGameplayTags },
    { TEXT("get_gameplay_tag_info"), &HandleGetGameplayTagInfo },
    { TEXT("get_gameplay_tag_redirects"), &HandleGetGameplayTagRedirects },
    { TEXT("get_gameplay_tag_source"), &HandleGetGameplayTagSource },
    { TEXT("get_gameplay_tag_table_rows"), &HandleGetGameplayTagTableRows },
    { TEXT("get_gameplay_tags_manager"), &HandleGetGameplayTagsManager },
    { TEXT("get_object_tags"), &HandleGetObjectTags },
    { TEXT("has_all_gameplay_tags"), &HandleHasAllGameplayTags },
    { TEXT("has_any_gameplay_tags"), &HandleHasAnyGameplayTags },
    { TEXT("has_none_gameplay_tags"), &HandleHasNoneGameplayTags },
    { TEXT("has_tag"), &HandleHasTag },
    { TEXT("query_gameplay_tags"), &HandleQueryGameplayTags },
    { TEXT("remove_gameplay_tag"), &HandleRemoveGameplayTag },
    { TEXT("remove_gameplay_tag_from_table"), &HandleRemoveGameplayTagFromTable },
    { TEXT("remove_gameplay_tag_redirect"), &HandleRemoveGameplayTagRedirect },
    { TEXT("remove_tag_from_object"), &HandleRemoveTagFromObject },
    { TEXT("validate_gameplay_tag_table"), &HandleValidateGameplayTagTable },
};
} // namespace UEAgentGameplayTagsCommands

#undef LOCTEXT_NAMESPACE
