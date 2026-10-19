#include "Mcp/McpCommandFamilies.h"
#include "SaveGameSystem.h"     // ISaveGameSystem::GetSaveGameNames (list_save_games)
#include "PlatformFeatures.h"    // IPlatformFeaturesModule::GetSaveGameSystem
#include "Commands/Common/McpCommonUtils.h"
#include "GameFramework/SaveGame.h"
#include "MCPSaveGame.h"
#include "Kismet/GameplayStatics.h"
#include "HAL/PlatformFilemanager.h"
#include "HAL/FileManager.h"      // IFileManager::FindFiles / FindFilesRecursive (slot folders)
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Base64.h"
#include "Async/Future.h"

/**
 * MCP domain for USaveGame objects and the slot files the engine's save system keeps:
 * create, write, read, delete and inspect slots, plus the in-memory (base64) round trip.
 */
namespace UEAgentSaveGameCommands
{

// MCP-DOMAIN: SaveGame

namespace UEAgentSaveGameImpl
{
    // USaveGame is UCLASS(abstract): a real save needs a concrete subclass. Resolve + validate the
    // requested name to a CONCRETE USaveGame subclass. NewObject on an abstract class yields a
    // degenerate instance the linker nulls out on save (UObjectGlobals abstract-class ensure).
    // Returns nullptr + sets OutError on failure (missing / unresolved / not-a-SaveGame / abstract).
    UClass* ResolveConcreteSaveGameClass(const FString& ClassName, FString& OutError)
    {
        if (ClassName.IsEmpty())
        {
            // No class supplied -> the plugin's concrete default (the USaveGame base is abstract).
            return UMCPSaveGame::StaticClass();
        }
        UClass* Class = FindFirstObject<UClass>(*ClassName);
        if (!Class)
        {
            OutError = FString::Printf(TEXT("SaveGame class not found: %s"), *ClassName);
            return nullptr;
        }
        if (!Class->IsChildOf(USaveGame::StaticClass()))
        {
            OutError = FString::Printf(TEXT("Class is not a SaveGame: %s"), *ClassName);
            return nullptr;
        }
        if (Class->HasAnyClassFlags(CLASS_Abstract))
        {
            OutError = FString::Printf(TEXT("Cannot instantiate abstract SaveGame class '%s'; provide a concrete subclass"), *ClassName);
            return nullptr;
        }
        return Class;
    }

    // A fresh SaveGame of the requested (or default) concrete class, owned by the transient
    // package: null once OutSaveGame holds it, otherwise the error reply.
    TSharedPtr<FJsonObject> NewTransientSaveGame(const FString& RequestedClass, USaveGame*& OutSaveGame)
    {
        FString Problem;
        UClass* const Concrete = ResolveConcreteSaveGameClass(RequestedClass, Problem);
        if (Concrete == nullptr)
        {
            return mcp_common::MakeErrorReply(Problem);
        }

        OutSaveGame = NewObject<USaveGame>(GetTransientPackage(), Concrete);
        if (OutSaveGame == nullptr)
        {
            return mcp_common::MakeErrorReply(TEXT("Failed to create SaveGame instance"));
        }
        return nullptr;
    }

    // The load path does not instantiate anything: the slot file names its own class. A named
    // class that exists and derives from USaveGame is still looked up; anything else falls back
    // to the USaveGame base.
    UClass* LookupLoadClass(const FString& RequestedClass)
    {
        if (!RequestedClass.IsEmpty())
        {
            UClass* const Named = FindFirstObject<UClass>(*RequestedClass);
            if (Named != nullptr && Named->IsChildOf(USaveGame::StaticClass()))
            {
                return Named;
            }
        }
        return USaveGame::StaticClass();
    }

    // A reply that starts with the slot's name and user index.
    TSharedPtr<FJsonObject> StartSlotReply(const FString& Slot, int32 User)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("slot_name"), Slot);
        Payload->SetNumberField(TEXT("user_index"), User);
        return Payload;
    }

    // The generic desktop save system writes <ProjectSaved>/SaveGames/<slot>.sav.
    FString SaveGamesFolder()
    {
        return FPaths::ProjectSavedDir() + TEXT("SaveGames/");
    }

    FString SlotFileOnDisk(const FString& Slot)
    {
        return SaveGamesFolder() + Slot + TEXT(".sav");
    }

    // save_path is always published; file_size (and, on request, the modification time) only
    // when a file is really there.
    void WriteSlotFileFacts(FJsonObject& Payload, const FString& Slot, const bool bWithTimestamp)
    {
        const FString OnDisk = SlotFileOnDisk(Slot);
        Payload.SetStringField(TEXT("save_path"), OnDisk);

        IPlatformFile& Disk = FPlatformFileManager::Get().GetPlatformFile();
        if (!Disk.FileExists(*OnDisk))
        {
            return;
        }
        Payload.SetNumberField(TEXT("file_size"), static_cast<double>(Disk.FileSize(*OnDisk)));

        if (!bWithTimestamp)
        {
            return;
        }
        const FDateTime Modified = Disk.GetTimeStamp(*OnDisk);
        if (Modified != FDateTime::MinValue())
        {
            Payload.SetStringField(TEXT("modified_iso"), Modified.ToIso8601());
            Payload.SetNumberField(TEXT("modified_ticks"), static_cast<double>(Modified.GetTicks()));
        }
    }
}

// Slot lifecycle and inspection.

// MCP:create_save_game - Create a transient, in-memory SaveGame object of save_game_class (default UMCPSaveGame), which checks that the class resolves and can be instantiated; nothing is written to disk and no other tool takes the object as input. save_game_class is matched by name among loaded classes and must be a concrete USaveGame subclass: an unknown, non-SaveGame or abstract class is an error. Every call makes a new object, named by save_game_instance and save_game_path; save_game_class echoes the name given, empty for the default. To write a slot use save_game, which builds its own default-valued object; to serialize to memory, save_game_to_memory; to read a slot back, load_game.
//   @param save_game_class - Optional name of a loaded, concrete USaveGame subclass, e.g. "MySaveGame" (found with FindFirstObject, never loaded from disk); defaults to the plugin's concrete UMCPSaveGame (the abstract USaveGame base is rejected)
//   @returns { save_game_class: string, save_game_instance: string, save_game_path: string, success: bool }
TSharedPtr<FJsonObject> HandleCreateSaveGame(const TSharedPtr<FJsonObject>& Params)
{
    FString RequestedClass;
    Params->TryGetStringField(TEXT("save_game_class"), RequestedClass);

    USaveGame* Created = nullptr;
    if (TSharedPtr<FJsonObject> Problem = UEAgentSaveGameImpl::NewTransientSaveGame(RequestedClass, Created)) { return Problem; }

    // save_game_class echoes the name as it was requested (empty when the default class served).
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetBoolField(TEXT("success"), true);
    Payload->SetStringField(TEXT("save_game_class"), RequestedClass);
    Payload->SetStringField(TEXT("save_game_instance"), Created->GetName());
    Payload->SetStringField(TEXT("save_game_path"), Created->GetPathName());
    return mcp_common::MakeOkReply(Payload);
}

// MCP:save_game - Write a NEW default-valued SaveGame object of save_game_class (default UMCPSaveGame) to a slot with SaveGameToSlot, overwriting what the slot held; it does not save an object made by create_save_game. On desktop the file is Saved/SaveGames/<slot>.sav. Returns success and message.
//   @param slot_name - Save slot name where the game will be saved
//   @param user_index - Platform user index for the save system; default 0 (the generic desktop save system ignores it)
//   @param save_game_class - Name of a loaded, concrete USaveGame subclass, e.g. "MySaveGame" (found with FindFirstObject, never loaded from disk); default UMCPSaveGame
//   @returns { slot_name: string, user_index: int, success: bool, message: string }
TSharedPtr<FJsonObject> HandleSaveGame(const TSharedPtr<FJsonObject>& Params)
{
    FString Slot;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("slot_name"), Slot, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    int32 User = 0;
    Params->TryGetNumberField(TEXT("user_index"), User);

    // What gets written is a fresh object of a concrete class (the USaveGame base is abstract).
    FString RequestedClass;
    Params->TryGetStringField(TEXT("save_game_class"), RequestedClass);

    USaveGame* Snapshot = nullptr;
    if (TSharedPtr<FJsonObject> Problem = UEAgentSaveGameImpl::NewTransientSaveGame(RequestedClass, Snapshot)) { return Problem; }

    const bool bWritten = UGameplayStatics::SaveGameToSlot(Snapshot, Slot, User);

    TSharedPtr<FJsonObject> Payload = UEAgentSaveGameImpl::StartSlotReply(Slot, User);
    Payload->SetBoolField(TEXT("success"), bWritten);
    Payload->SetStringField(TEXT("message"), bWritten ? TEXT("Game saved successfully") : TEXT("Failed to save game"));

    const FString Failure = bWritten
        ? FString()
        : FString::Printf(TEXT("UGameplayStatics::SaveGameToSlot returned false for slot '%s'"), *Slot);
    return mcp_common::MakeOutcomeReply(Payload, bWritten, Failure);
}

// MCP:load_game - Load the SaveGame stored in a slot with LoadGameFromSlot and report the transient object: save_game_instance, save_game_class and save_game_path. success is false when the slot is empty or unreadable, or holds a class other than save_game_class; does_save_game_exist tells missing from corrupt. It only reads: the slot file is not changed and the object is transient. Write a slot with save_game; the same read without the class check is load_game_async.
//   @param slot_name - Save slot name to load from
//   @param user_index - Platform user index for the save system; default 0 (the generic desktop save system ignores it)
//   @param save_game_class - Optional SaveGame subclass the slot is expected to hold. A slot file records its own class and is rebuilt as that; when the loaded object is not this class (or a child of it) the load is reported as failed. An unknown class name is an error.
//   @returns { slot_name: string, user_index: int, success: bool, save_game_instance: string, save_game_class: string, save_game_path: string, expected_class?: string, error: string }
//   @hints read-only
TSharedPtr<FJsonObject> HandleLoadGame(const TSharedPtr<FJsonObject>& Params)
{
    FString Slot;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("slot_name"), Slot, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    int32 User = 0;
    Params->TryGetNumberField(TEXT("user_index"), User);
    FString RequestedClass;
    Params->TryGetStringField(TEXT("save_game_class"), RequestedClass);

    // LoadGameFromSlot rebuilds whatever class the slot file recorded, so the requested class
    // cannot steer the load: it is checked against what came back.
    UClass* const Expected = UEAgentSaveGameImpl::LookupLoadClass(RequestedClass);
    if (!RequestedClass.IsEmpty() && Expected == USaveGame::StaticClass() && !RequestedClass.Equals(TEXT("SaveGame")))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("save_game_class '%s' is not a loaded USaveGame subclass"), *RequestedClass));
    }

    USaveGame* const Loaded = UGameplayStatics::LoadGameFromSlot(Slot, User);
    const bool bClassMatches = Loaded == nullptr || Loaded->IsA(Expected);
    USaveGame* const Restored = bClassMatches ? Loaded : nullptr;
    const bool bRestored = (Restored != nullptr);

    TSharedPtr<FJsonObject> Payload = UEAgentSaveGameImpl::StartSlotReply(Slot, User);
    Payload->SetBoolField(TEXT("success"), bRestored);
    if (!RequestedClass.IsEmpty())
    {
        Payload->SetStringField(TEXT("expected_class"), Expected->GetName());
    }
    if (Loaded != nullptr)
    {
        Payload->SetStringField(TEXT("save_game_instance"), Loaded->GetName());
        Payload->SetStringField(TEXT("save_game_class"), Loaded->GetClass()->GetName());
        Payload->SetStringField(TEXT("save_game_path"), Loaded->GetPathName());
    }

    // The top-level verdict is the measured one, so an empty slot cannot travel as an outer
    // success:true with success:false buried inside data.
    FString Failure;
    if (!bClassMatches)
    {
        Failure = FString::Printf(TEXT("Slot '%s' holds a %s, not a %s (save_game_class)."),
            *Slot, *Loaded->GetClass()->GetName(), *Expected->GetName());
    }
    else if (!bRestored)
    {
        Failure = FString::Printf(
            TEXT("No SaveGame in slot '%s' for user %d: UGameplayStatics::LoadGameFromSlot ")
            TEXT("returned null. Use does_save_game_exist to tell 'no file' from 'file that ")
            TEXT("failed to deserialize'."),
            *Slot, User);
    }
    return mcp_common::MakeOutcomeReply(Payload, bRestored, Failure);
}

// MCP:delete_save_game - Delete the save file of a slot with DeleteGameInSlot; the data is gone for good. success is false, with an error, when the engine refuses, usually because the slot does not exist (check with does_save_game_exist). Returns slot_name, user_index and message.
//   @param slot_name - Save slot name to delete
//   @param user_index - Platform user index for the save system; default 0 (the generic desktop save system ignores it)
//   @returns { slot_name: string, user_index: int, success: bool, message: string }
TSharedPtr<FJsonObject> HandleDeleteSaveGame(const TSharedPtr<FJsonObject>& Params)
{
    FString Slot;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("slot_name"), Slot, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    int32 User = 0;
    Params->TryGetNumberField(TEXT("user_index"), User);

    const bool bErased = UGameplayStatics::DeleteGameInSlot(Slot, User);

    TSharedPtr<FJsonObject> Payload = UEAgentSaveGameImpl::StartSlotReply(Slot, User);
    Payload->SetBoolField(TEXT("success"), bErased);
    Payload->SetStringField(TEXT("message"), bErased ? TEXT("Save game deleted successfully") : TEXT("Failed to delete save game"));

    // As in load_game, the engine's answer decides the top-level verdict.
    FString Failure;
    if (!bErased)
    {
        Failure = FString::Printf(
            TEXT("UGameplayStatics::DeleteGameInSlot returned false for slot '%s' (user %d). ")
            TEXT("The usual cause is that the slot was not there to begin with: check with ")
            TEXT("does_save_game_exist."),
            *Slot, User);
    }
    return mcp_common::MakeOutcomeReply(Payload, bErased, Failure);
}

// MCP:get_save_game_info - Check one save slot with UGameplayStatics::DoesSaveGameExist and, when it exists, report save_path (the generic desktop Saved/SaveGames/<slot>.sav) plus file_size when that file is on disk. Read-only; a missing slot is not an error, just exists false with no path or size. get_save_game_slot_info returns the same plus the modification time, does_save_game_exist only the flag, list_save_games every slot.
//   @param slot_name - Save slot name to query
//   @param user_index - Platform user index for the save system; default 0 (the generic desktop save system ignores it)
//   @returns { slot_name: string, user_index: int, exists: bool, save_path?: string, file_size?: int }
TSharedPtr<FJsonObject> HandleGetSaveGameInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString Slot;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("slot_name"), Slot, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    int32 User = 0;
    Params->TryGetNumberField(TEXT("user_index"), User);

    const bool bPresent = UGameplayStatics::DoesSaveGameExist(Slot, User);

    TSharedPtr<FJsonObject> Payload = UEAgentSaveGameImpl::StartSlotReply(Slot, User);
    Payload->SetBoolField(TEXT("exists"), bPresent);
    if (bPresent)
    {
        UEAgentSaveGameImpl::WriteSlotFileFacts(*Payload, Slot, /*bWithTimestamp=*/false);
    }
    return mcp_common::MakeOkReply(Payload);
}

// MCP:list_save_games - List all save games for a given user index. The list is what the platform's save system calls slots: on desktop, the .sav files at the root of the save folder. A save system can also keep a slot as a FOLDER of .sav files, which is not a slot here: those folders are reported apart, in slot_folders, so an empty list is only proof of no save when slot_folders is empty too.
//   @param user_index - Optional user index, default 0. Passed to the platform's save system; the generic desktop one keeps every user's slots in one folder, so there it changes nothing.
//   @returns { user_index: int, save_directory: string, save_games: [{ slot_name, file_path?, file_size? }], count: int, slot_folders: [{ folder, sav_files }] (subfolders of save_directory holding .sav files, searched recursively) }
TSharedPtr<FJsonObject> HandleListSaveGames(const TSharedPtr<FJsonObject>& Params)
{
    int32 User = 0;
    Params->TryGetNumberField(TEXT("user_index"), User);

    // The platform's save system lists the slots for this user, as LoadGameFromSlot would see them;
    // a slot that also has a file in the generic folder gets its path and size.
    const FString Folder = UEAgentSaveGameImpl::SaveGamesFolder();
    TArray<FString> SlotNames;
    if (ISaveGameSystem* SaveSystem = IPlatformFeaturesModule::Get().GetSaveGameSystem())
    {
        SaveSystem->GetSaveGameNames(SlotNames, User);
    }
    IPlatformFile& Disk = FPlatformFileManager::Get().GetPlatformFile();
    TArray<TSharedPtr<FJsonValue>> Found;
    for (const FString& SlotName : SlotNames)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("slot_name"), SlotName);
        const FString SavFile = Folder / (SlotName + TEXT(".sav"));
        if (Disk.FileExists(*SavFile))
        {
            Entry->SetStringField(TEXT("file_path"), SavFile);
            Entry->SetNumberField(TEXT("file_size"), static_cast<double>(Disk.FileSize(*SavFile)));
        }
        Found.Add(MakeShared<FJsonValueObject>(Entry));
    }

    // A save system can keep one slot as a folder of .sav files, which the flat list above never
    // shows. Every subfolder holding at least one .sav (at any depth) is reported with its count.
    TArray<TSharedPtr<FJsonValue>> SlotFolders;
    TArray<FString> SubFolders;
    IFileManager::Get().FindFiles(SubFolders, *(Folder / TEXT("*")), /*Files=*/false, /*Directories=*/true);
    SubFolders.Sort();
    for (const FString& Sub : SubFolders)
    {
        TArray<FString> SavFiles;
        IFileManager::Get().FindFilesRecursive(SavFiles, *(Folder / Sub), TEXT("*.sav"), /*Files=*/true, /*Directories=*/false);
        if (SavFiles.Num() > 0)
        {
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("folder"), Sub);
            Entry->SetNumberField(TEXT("sav_files"), SavFiles.Num());
            SlotFolders.Add(MakeShared<FJsonValueObject>(Entry));
        }
    }

    TSharedPtr<FJsonObject> Listing = MakeShared<FJsonObject>();
    Listing->SetNumberField(TEXT("user_index"), User);
    Listing->SetStringField(TEXT("save_directory"), Folder);
    Listing->SetNumberField(TEXT("count"), Found.Num());
    Listing->SetArrayField(TEXT("save_games"), Found);
    Listing->SetArrayField(TEXT("slot_folders"), SlotFolders);
    return mcp_common::MakeOkReply(Listing);
}

// ===========================================================================
// SaveGame async + memory + slot info.
//
// Honest note on async: UGameplayStatics::AsyncSaveGameToSlot / AsyncLoadGameFromSlot
// fire-and-forget a delegate on the GAME THREAD. The bridge is itself a
// game-thread consumer (FTSTicker drain from the UEAgentBridge), so
// waiting on a delegate that must be pumped by the same thread would
// deadlock. We therefore resolve the "async" commands by routing through
// the synchronous overloads (SaveGameToSlot / LoadGameFromSlot) and report
// `mode: "sync_fallback"` + an explanatory `note`. The async *API surface*
// is exposed (same command names exist in UE) and the operations are
// observable to the caller; the only difference is that the work is done
// inline rather than queued.
//
// For true non-blocking behaviour the caller would need to drive the editor
// from a non-game-thread side channel; that's outside the bridge's contract.
// ===========================================================================

// MCP:save_game_async - Save a SaveGame instance to slot, sync fallback over AsyncSaveGameToSlot
//   @param slot_name - Save slot name
//   @param user_index - Optional user index, default 0
//   @param save_game_class - Optional SaveGame subclass; defaults to the plugin's concrete UMCPSaveGame (the abstract USaveGame base is rejected)
//   @returns { slot_name: string, user_index: int, success: bool, mode: string, note: string, message: string }
TSharedPtr<FJsonObject> HandleSaveGameAsync(const TSharedPtr<FJsonObject>& Params)
{
    FString Slot;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("slot_name"), Slot)) { return Missing; }
    int32 User = 0;
    Params->TryGetNumberField(TEXT("user_index"), User);
    FString RequestedClass;
    Params->TryGetStringField(TEXT("save_game_class"), RequestedClass);

    USaveGame* Snapshot = nullptr;
    if (TSharedPtr<FJsonObject> Problem = UEAgentSaveGameImpl::NewTransientSaveGame(RequestedClass, Snapshot)) { return Problem; }

    // The bridge itself runs on the game thread, which is also where the async delegate would
    // have to fire: waiting for it would deadlock. The synchronous overload does the work, and
    // the payload says so in "mode" and "note".
    const bool bWritten = UGameplayStatics::SaveGameToSlot(Snapshot, Slot, User);

    TSharedPtr<FJsonObject> Payload = UEAgentSaveGameImpl::StartSlotReply(Slot, User);
    Payload->SetBoolField(TEXT("success"), bWritten);
    Payload->SetStringField(TEXT("mode"), TEXT("sync_fallback"));
    Payload->SetStringField(TEXT("note"),
        TEXT("bridge runs on game thread; awaiting UGameplayStatics::AsyncSaveGameToSlot's delegate would deadlock. Routed through SaveGameToSlot."));
    Payload->SetStringField(TEXT("message"), bWritten ? TEXT("Game saved (sync fallback)") : TEXT("Failed to save game"));

    const FString Failure = bWritten
        ? FString()
        : FString::Printf(TEXT("UGameplayStatics::SaveGameToSlot returned false for slot '%s' (sync fallback path)"), *Slot);
    return mcp_common::MakeOutcomeReply(Payload, bWritten, Failure);
}

// MCP:load_game_async - Load the SaveGame stored in a slot and report the object it rebuilds (save_game_instance, save_game_class, save_game_path). Despite the name it runs synchronously through LoadGameFromSlot (mode=sync_fallback), since waiting on the game thread for the async delegate would deadlock; the outcome matches load_game without its save_game_class check. It only reads: the slot file is not changed and the object is transient. An empty slot or a file that fails to deserialize gives success=false with an error; tell them apart with does_save_game_exist. Write a slot with save_game_async or save_game; file path, size and date: get_save_game_slot_info; every slot: list_save_games.
//   @param slot_name - Save slot name, non-empty; on desktop the file is Saved/SaveGames/<slot>.sav.
//   @param user_index - Platform user index for the save system; default 0 (the generic desktop save system ignores it)
//   @returns { slot_name: string, user_index: int, success: bool, mode: string, note: string, save_game_instance?: string, save_game_class?: string, save_game_path?: string, error?: string }
//   @hints read-only
TSharedPtr<FJsonObject> HandleLoadGameAsync(const TSharedPtr<FJsonObject>& Params)
{
    FString Slot;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("slot_name"), Slot)) { return Missing; }
    int32 User = 0;
    Params->TryGetNumberField(TEXT("user_index"), User);

    // Synchronous for the same reason as save_game_async.
    USaveGame* const Restored = UGameplayStatics::LoadGameFromSlot(Slot, User);
    const bool bRestored = (Restored != nullptr);

    TSharedPtr<FJsonObject> Payload = UEAgentSaveGameImpl::StartSlotReply(Slot, User);
    Payload->SetBoolField(TEXT("success"), bRestored);
    Payload->SetStringField(TEXT("mode"), TEXT("sync_fallback"));
    Payload->SetStringField(TEXT("note"),
        TEXT("bridge runs on game thread; awaiting AsyncLoadGameFromSlot's delegate would deadlock. Routed through LoadGameFromSlot."));
    if (bRestored)
    {
        Payload->SetStringField(TEXT("save_game_instance"), Restored->GetName());
        Payload->SetStringField(TEXT("save_game_class"), Restored->GetClass()->GetName());
        Payload->SetStringField(TEXT("save_game_path"), Restored->GetPathName());
    }

    // The measured result is also the emitted verdict (see load_game).
    FString Failure;
    if (!bRestored)
    {
        Failure = FString::Printf(
            TEXT("No SaveGame in slot '%s' for user %d (sync fallback path). ")
            TEXT("Check does_save_game_exist."),
            *Slot, User);
    }
    return mcp_common::MakeOutcomeReply(Payload, bRestored, Failure);
}

// MCP:does_save_game_exist - Check whether a save slot has data with UGameplayStatics::DoesSaveGameExist; returns exists. Read-only. Use get_save_game_slot_info for the file path, size and modification time, or list_save_games to see every slot.
//   @param slot_name - Save slot name to check
//   @param user_index - Platform user index for the save system; default 0 (the generic desktop save system ignores it)
//   @returns { slot_name: string, user_index: int, exists: bool }
TSharedPtr<FJsonObject> HandleDoesSaveGameExist(const TSharedPtr<FJsonObject>& Params)
{
    FString Slot;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("slot_name"), Slot)) { return Missing; }
    int32 User = 0;
    Params->TryGetNumberField(TEXT("user_index"), User);

    TSharedPtr<FJsonObject> Payload = UEAgentSaveGameImpl::StartSlotReply(Slot, User);
    Payload->SetBoolField(TEXT("exists"), UGameplayStatics::DoesSaveGameExist(Slot, User));
    return mcp_common::MakeOkReply(Payload);
}

// MCP:get_save_game_slot_info - Read the metadata of a save slot: exists and, for an existing slot, save_path (the generic desktop Saved/SaveGames/<slot>.sav) plus file_size, modified_iso and modified_ticks when that file is on disk. A missing slot is exists=false with an error field, not a failure.
//   @param slot_name - Save slot name to query
//   @param user_index - Platform user index for the save system; default 0 (the generic desktop save system ignores it)
//   @returns { slot_name: string, user_index: int, exists: bool, save_path: string, file_size: int, modified_iso: string, modified_ticks: int, error: string }
TSharedPtr<FJsonObject> HandleGetSaveGameSlotInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString Slot;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("slot_name"), Slot)) { return Missing; }
    int32 User = 0;
    Params->TryGetNumberField(TEXT("user_index"), User);

    const bool bPresent = UGameplayStatics::DoesSaveGameExist(Slot, User);

    TSharedPtr<FJsonObject> Payload = UEAgentSaveGameImpl::StartSlotReply(Slot, User);
    Payload->SetBoolField(TEXT("exists"), bPresent);
    if (!bPresent)
    {
        // A missing slot is a valid answer here, reported in data rather than as a failure.
        Payload->SetStringField(TEXT("error"), TEXT("Slot does not exist"));
    }
    else
    {
        UEAgentSaveGameImpl::WriteSlotFileFacts(*Payload, Slot, /*bWithTimestamp=*/true);
    }
    return mcp_common::MakeOkReply(Payload);
}

// MCP:save_game_to_memory - Serialize a NEW default-valued SaveGame object of save_game_class (default UMCPSaveGame) with SaveGameToMemory and return it as memory_base64 with memory_size; nothing is written to disk. Pass the blob to load_game_from_memory to round-trip it.
//   @param save_game_class - Name of a loaded, concrete USaveGame subclass, e.g. "MySaveGame" (found with FindFirstObject, never loaded from disk); default UMCPSaveGame
//   @returns { success: bool, save_game_class: string, memory_size: int, memory_base64: string }
TSharedPtr<FJsonObject> HandleSaveGameToMemory(const TSharedPtr<FJsonObject>& Params)
{
    // Serializing also needs a concrete class: the USaveGame base is abstract.
    FString RequestedClass;
    Params->TryGetStringField(TEXT("save_game_class"), RequestedClass);

    USaveGame* Snapshot = nullptr;
    if (TSharedPtr<FJsonObject> Problem = UEAgentSaveGameImpl::NewTransientSaveGame(RequestedClass, Snapshot)) { return Problem; }

    TArray<uint8> Bytes;
    const bool bSerialized = UGameplayStatics::SaveGameToMemory(Snapshot, Bytes);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetBoolField(TEXT("success"), bSerialized);
    Payload->SetStringField(TEXT("save_game_class"), Snapshot->GetClass()->GetName());
    Payload->SetNumberField(TEXT("memory_size"), static_cast<double>(Bytes.Num()));
    // The blob is only attached when there is something to carry.
    if (bSerialized && Bytes.Num() > 0)
    {
        Payload->SetStringField(TEXT("memory_base64"), FBase64::Encode(Bytes));
    }

    const FString Failure = bSerialized ? FString() : FString(TEXT("UGameplayStatics::SaveGameToMemory returned false"));
    return mcp_common::MakeOutcomeReply(Payload, bSerialized, Failure);
}

// MCP:load_game_from_memory - Deserialize a SaveGame from a base64 blob (the memory_base64 of save_game_to_memory) with LoadGameFromMemory; returns the transient object save_game_instance, save_game_class and save_game_path. Invalid base64 is an error; success is false when the blob is corrupt, truncated or from another SaveGame class. It only reads: nothing is written to disk and the object is transient. Make a blob with save_game_to_memory; to read a slot file use load_game.
//   @param memory_base64 - Base64 text of a serialized SaveGame, as returned in memory_base64 by save_game_to_memory
//   @returns { success: bool, memory_size: int, save_game_instance: string, save_game_class: string, save_game_path: string, error: string }
//   @hints read-only
TSharedPtr<FJsonObject> HandleLoadGameFromMemory(const TSharedPtr<FJsonObject>& Params)
{
    FString Base64Data;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("memory_base64"), Base64Data)) { return Missing; }

    TArray<uint8> Memory;
    if (!FBase64::Decode(Base64Data, Memory))
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to decode memory_base64"));
    }

    USaveGame* Loaded = UGameplayStatics::LoadGameFromMemory(Memory);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("success"), Loaded != nullptr);
    ResultObj->SetNumberField(TEXT("memory_size"), (int64)Memory.Num());
    if (Loaded)
    {
        ResultObj->SetStringField(TEXT("save_game_instance"), Loaded->GetName());
        ResultObj->SetStringField(TEXT("save_game_class"), Loaded->GetClass()->GetName());
        ResultObj->SetStringField(TEXT("save_game_path"), Loaded->GetPathName());
        // (the verdict is emitted below with MakeOutcomeReply -- see load_game)
    }
    else
    {
        ResultObj->SetStringField(TEXT("error"), TEXT("LoadGameFromMemory returned null (corrupt or wrong class)"));
    }
    return mcp_common::MakeOutcomeReply(ResultObj, Loaded != nullptr,
        Loaded != nullptr ? FString() : TEXT(
            "UGameplayStatics::LoadGameFromMemory returned null: the base64 blob is corrupt, "
            "truncated, or was written by a different SaveGame class."));
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("create_save_game"), &HandleCreateSaveGame },
    { TEXT("delete_save_game"), &HandleDeleteSaveGame },
    { TEXT("does_save_game_exist"), &HandleDoesSaveGameExist },
    { TEXT("get_save_game_info"), &HandleGetSaveGameInfo },
    { TEXT("get_save_game_slot_info"), &HandleGetSaveGameSlotInfo },
    { TEXT("list_save_games"), &HandleListSaveGames },
    { TEXT("load_game"), &HandleLoadGame },
    { TEXT("load_game_async"), &HandleLoadGameAsync },
    { TEXT("load_game_from_memory"), &HandleLoadGameFromMemory },
    { TEXT("save_game"), &HandleSaveGame },
    { TEXT("save_game_async"), &HandleSaveGameAsync },
    { TEXT("save_game_to_memory"), &HandleSaveGameToMemory },
};
} // namespace UEAgentSaveGameCommands
