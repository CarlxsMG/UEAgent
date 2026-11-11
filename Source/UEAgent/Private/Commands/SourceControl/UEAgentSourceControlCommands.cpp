// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"
#include "ISourceControlRevision.h"
#include "ISourceControlState.h"
#include "SourceControlHelpers.h"
#include "SourceControlOperations.h"

#include "Editor.h"
#include "Engine/World.h"
#include "Misc/CoreDelegates.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PackageTools.h"
#include "UObject/Linker.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

/**
 * Revision-control commands, through the editor's own provider (ISourceControlModule), so they work with whichever
 * provider the project uses: Perforce, Git, Subversion, Plastic/Unity Version Control.
 *
 * A file is named by its content path (`asset_paths`) or by its disk path (`files`, for .ini, .uproject and source
 * files). Status, checkout, add, revert and history run synchronously on the game thread; a submit can take minutes,
 * so it is start + poll with one job at a time, as start_package_build is. Every write re-reads the files' state after
 * the operation and builds its reply from that re-read, and none of them is an undo transaction.
 */
namespace UEAgentSourceControlCommands
{

// MCP-DOMAIN: SourceControl

namespace
{
    /** One file a call names: its full disk path and, when it is a package, its long package name. */
    struct FScTarget
    {
        FString File;
        FString AssetPath;
    };

    FString JoinNames(const TArray<FString>& Names)
    {
        return FString::Join(Names, TEXT(", "));
    }

    /** The provider's messages of a finished operation, errors first, as one line. */
    FString OperationMessages(const FSourceControlOperationRef& Operation)
    {
        TArray<FString> Lines;
        const FSourceControlResultInfo& Info = Operation->GetResultInfo();
        for (const FText& Error : Info.ErrorMessages)
        {
            Lines.Add(Error.ToString());
        }
        for (const FText& Message : Info.InfoMessages)
        {
            Lines.Add(Message.ToString());
        }
        return FString::Join(Lines, TEXT(" | "));
    }

    /**
     * The files a call names, from `asset_paths` (package or object paths) and `files` (disk paths, relative ones taken
     * from the project folder). Null when they are valid; otherwise the error reply.
     */
    TSharedPtr<FJsonObject> ReadTargets(const TSharedPtr<FJsonObject>& Params, TArray<FScTarget>& Out)
    {
        const TArray<TSharedPtr<FJsonValue>>* AssetPaths = nullptr;
        const TArray<TSharedPtr<FJsonValue>>* Files = nullptr;
        Params->TryGetArrayField(TEXT("asset_paths"), AssetPaths);
        Params->TryGetArrayField(TEXT("files"), Files);
        if ((!AssetPaths || AssetPaths->Num() == 0) && (!Files || Files->Num() == 0))
        {
            return mcp_common::MakeErrorReply(
                TEXT("Name at least one file: asset_paths (content paths such as /Game/Maps/Main) or files (disk paths)."));
        }

        TSet<FString> Seen;
        if (AssetPaths)
        {
            for (const TSharedPtr<FJsonValue>& Value : *AssetPaths)
            {
                FString PackageName;
                if (!Value.IsValid() || !Value->TryGetString(PackageName) || PackageName.IsEmpty())
                {
                    return mcp_common::MakeErrorReply(TEXT("asset_paths must be an array of non-empty strings."));
                }
                int32 Dot = INDEX_NONE;
                if (PackageName.FindChar(TEXT('.'), Dot))
                {
                    PackageName.LeftInline(Dot);
                }
                FText Reason;
                if (!FPackageName::IsValidLongPackageName(PackageName, false, &Reason))
                {
                    return mcp_common::MakeErrorReply(FString::Printf(TEXT("'%s' is not a content path: %s"), *PackageName, *Reason.ToString()));
                }
                const FString File = FPaths::ConvertRelativePathToFull(USourceControlHelpers::PackageFilename(PackageName));
                if (!Seen.Contains(File))
                {
                    Seen.Add(File);
                    Out.Add({ File, PackageName });
                }
            }
        }
        if (Files)
        {
            for (const TSharedPtr<FJsonValue>& Value : *Files)
            {
                FString File;
                if (!Value.IsValid() || !Value->TryGetString(File) || File.IsEmpty())
                {
                    return mcp_common::MakeErrorReply(TEXT("files must be an array of non-empty strings."));
                }
                File = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), File);
                FPaths::NormalizeFilename(File);
                FString PackageName;
                const FString Extension = FPaths::GetExtension(File, true);
                if (Extension == FPackageName::GetAssetPackageExtension() || Extension == FPackageName::GetMapPackageExtension())
                {
                    FPackageName::TryConvertFilenameToLongPackageName(File, PackageName);
                }
                if (!Seen.Contains(File))
                {
                    Seen.Add(File);
                    Out.Add({ File, PackageName });
                }
            }
        }
        return nullptr;
    }

    TArray<FString> FilesOf(const TArray<FScTarget>& Targets)
    {
        TArray<FString> Files;
        for (const FScTarget& Target : Targets)
        {
            Files.Add(Target.File);
        }
        return Files;
    }

    /** Null when revision control can run Tool; otherwise the error that names the cause and how to enable it. */
    TSharedPtr<FJsonObject> RequireProvider(const TCHAR* Tool)
    {
        ISourceControlModule& Module = ISourceControlModule::Get();
        if (!Module.IsEnabled())
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("%s: revision control is disabled in this editor, so nothing was changed. Enable it from the Revision ")
                TEXT("Control menu of the editor's status bar (Connect to Revision Control...) and pick the project's provider; ")
                TEXT("get_source_control_status reports the connection."), Tool));
        }
        ISourceControlProvider& Provider = Module.GetProvider();
        if (!Provider.IsAvailable())
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("%s: the revision control provider '%s' is enabled but not available (%s), so nothing was changed. ")
                TEXT("Check its connection settings in the Revision Control menu of the editor's status bar."),
                Tool, *Provider.GetName().ToString(), *Provider.GetStatusText().ToString()));
        }
        return nullptr;
    }

    /** Null when no target is under a Read Only Content Root; otherwise the error that names them. */
    TSharedPtr<FJsonObject> RefuseReadOnlyRoots(const TCHAR* Tool, const TArray<FScTarget>& Targets)
    {
        TArray<FString> Refused;
        for (const FScTarget& Target : Targets)
        {
            if (!Target.AssetPath.IsEmpty() && mcp_common::IsUnderReadOnlyRoot(Target.AssetPath))
            {
                Refused.Add(Target.AssetPath);
            }
        }
        if (Refused.Num() == 0)
        {
            return nullptr;
        }
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("%s: refused, nothing was changed. These paths are under a Read Only Content Root (Project Settings > ")
            TEXT("Plugins > UEAgent): %s"), Tool, *JoinNames(Refused)));
    }

    /** The provider's state of every target, re-read from the provider when bForceUpdate; false when the query failed. */
    bool ReadStates(const TArray<FScTarget>& Targets, bool bForceUpdate, TArray<FSourceControlStateRef>& OutStates)
    {
        ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();
        OutStates.Reset();
        const ECommandResult::Type Result = Provider.GetState(FilesOf(Targets), OutStates,
            bForceUpdate ? EStateCacheUsage::ForceUpdate : EStateCacheUsage::Use);
        return Result == ECommandResult::Succeeded && OutStates.Num() == Targets.Num();
    }

    TSharedPtr<FJsonObject> StateRow(const FSourceControlStateRef& State, const FString& AssetPath)
    {
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("file"), State->GetFilename());
        if (!AssetPath.IsEmpty())
        {
            Row->SetStringField(TEXT("asset_path"), AssetPath);
        }
        FString OtherUser;
        const bool bCheckedOutOther = State->IsCheckedOutOther(&OtherUser);
        Row->SetStringField(TEXT("status"), State->GetDisplayName().ToString());
        Row->SetBoolField(TEXT("source_controlled"), State->IsSourceControlled());
        // Git reports every tracked file as checked out; only a provider that uses checkout has one to report.
        Row->SetBoolField(TEXT("checked_out"), ISourceControlModule::Get().GetProvider().UsesCheckout() && State->IsCheckedOut());
        Row->SetBoolField(TEXT("checked_out_by_other"), bCheckedOutOther);
        Row->SetStringField(TEXT("other_user"), bCheckedOutOther ? OtherUser : FString());
        Row->SetBoolField(TEXT("added"), State->IsAdded());
        Row->SetBoolField(TEXT("deleted"), State->IsDeleted());
        Row->SetBoolField(TEXT("modified"), State->IsModified());
        Row->SetBoolField(TEXT("current"), State->IsCurrent());
        Row->SetBoolField(TEXT("conflicted"), State->IsConflicted());
        Row->SetBoolField(TEXT("ignored"), State->IsIgnored());
        Row->SetBoolField(TEXT("can_checkout"), State->CanCheckout());
        Row->SetBoolField(TEXT("can_checkin"), State->CanCheckIn());
        Row->SetBoolField(TEXT("can_add"), State->CanAdd());
        return Row;
    }

    /**
     * Whether State holds a checkout to release: only a provider that uses checkout has one. Git reports every tracked
     * file as checked out, which says nothing about local changes.
     */
    bool HoldsCheckout(const FSourceControlStateRef& State)
    {
        return ISourceControlModule::Get().GetProvider().UsesCheckout() && State->IsCheckedOut();
    }

    /** The long package name of a disk file, or empty when it is not a package. */
    FString PackageNameOfFile(const FString& File)
    {
        FString PackageName;
        const FString Extension = FPaths::GetExtension(File, true);
        if (Extension == FPackageName::GetAssetPackageExtension() || Extension == FPackageName::GetMapPackageExtension())
        {
            FPackageName::TryConvertFilenameToLongPackageName(File, PackageName);
        }
        return PackageName;
    }

    TSharedPtr<FJsonObject> FailedRow(const FString& File, const FString& Error)
    {
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("file"), File);
        Row->SetStringField(TEXT("error"), Error);
        return Row;
    }

    void SetStrings(const TSharedPtr<FJsonObject>& Data, const TCHAR* Field, const TArray<FString>& Values)
    {
        TArray<TSharedPtr<FJsonValue>> Array;
        for (const FString& Value : Values)
        {
            Array.Add(MakeShared<FJsonValueString>(Value));
        }
        Data->SetArrayField(Field, Array);
    }

    /** The reply of a reader while revision control cannot answer: not an error, enabled says why. */
    TSharedPtr<FJsonObject> UnavailableReaderData()
    {
        ISourceControlModule& Module = ISourceControlModule::Get();
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("provider"), Module.GetProvider().GetName().ToString());
        Data->SetBoolField(TEXT("enabled"), Module.IsEnabled());
        Data->SetBoolField(TEXT("available"), Module.IsEnabled() && Module.GetProvider().IsAvailable());
        Data->SetStringField(TEXT("note"), Module.IsEnabled()
            ? TEXT("The revision control provider is not available, so no file state was read; get_source_control_status reports the connection.")
            : TEXT("Revision control is disabled in this editor, so no file state was read. Enable it from the Revision Control menu of the editor's status bar."));
        return Data;
    }

    bool CanRead()
    {
        ISourceControlModule& Module = ISourceControlModule::Get();
        return Module.IsEnabled() && Module.GetProvider().IsAvailable();
    }
}

// ---------------------------------------------------------------------------
// get_source_control_status
// ---------------------------------------------------------------------------
// MCP:get_source_control_status - Report the editor revision-control connection, read-only: provider name, whether revision control is enabled and the provider available, and its status_text. Says nothing about individual files or their checkout state, and never connects or changes settings.
//   @returns { provider, enabled, available, status_text }
TSharedPtr<FJsonObject> HandleGetSourceControlStatus(const TSharedPtr<FJsonObject>& Params)
{
    ISourceControlModule& SCModule = ISourceControlModule::Get();

    const bool bEnabled = SCModule.IsEnabled();

    ISourceControlProvider& Provider = SCModule.GetProvider();
    const FString ProviderName = Provider.GetName().ToString();
    const bool bAvailable = Provider.IsAvailable();
    const FString StatusText = Provider.GetStatusText().ToString();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("provider"), ProviderName);
    Data->SetBoolField(TEXT("enabled"), bEnabled);
    Data->SetBoolField(TEXT("available"), bAvailable);
    Data->SetStringField(TEXT("status_text"), StatusText);

    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_source_control_file_status
// ---------------------------------------------------------------------------
// MCP:get_source_control_file_status - Read the revision-control state of files through the editor's provider (Perforce, Git...): whether each is under revision control, checked out (by you or by someone else, with their name), added, deleted, modified, at the latest revision, conflicted or ignored, and what can be done with it next. Name assets by content path or other files (.ini, .uproject, source) by disk path. With revision control disabled it answers enabled false and no rows, without an error.
//   @param asset_paths - Content paths of the assets (/Game/Maps/Main or /Game/Maps/Main.Main). This or files is required.
//   @param files - Disk paths of other files, absolute or relative to the project folder. This or asset_paths is required.
//   @param force_update - Optional. Ask the provider again instead of reading its cache. Default true.
//   @returns { provider: string, enabled: bool, available: bool, files: [{file, asset_path?, status, source_controlled, checked_out, checked_out_by_other, other_user, added, deleted, modified, current, conflicted, ignored, can_checkout, can_checkin, can_add}], note?: string }
TSharedPtr<FJsonObject> HandleGetSourceControlFileStatus(const TSharedPtr<FJsonObject>& Params)
{
    TArray<FScTarget> Targets;
    if (TSharedPtr<FJsonObject> Error = ReadTargets(Params, Targets))
    {
        return Error;
    }
    bool bForceUpdate = true;
    Params->TryGetBoolField(TEXT("force_update"), bForceUpdate);

    if (!CanRead())
    {
        TSharedPtr<FJsonObject> Data = UnavailableReaderData();
        Data->SetArrayField(TEXT("files"), {});
        return mcp_common::MakeOkReply(Data);
    }

    TArray<FSourceControlStateRef> States;
    if (!ReadStates(Targets, bForceUpdate, States))
    {
        return mcp_common::MakeErrorReply(TEXT("get_source_control_file_status: the revision control provider did not answer for these files."));
    }
    TArray<TSharedPtr<FJsonValue>> Rows;
    for (int32 Index = 0; Index < States.Num(); ++Index)
    {
        Rows.Add(MakeShared<FJsonValueObject>(StateRow(States[Index], Targets[Index].AssetPath)));
    }
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("provider"), ISourceControlModule::Get().GetProvider().GetName().ToString());
    Data->SetBoolField(TEXT("enabled"), true);
    Data->SetBoolField(TEXT("available"), true);
    Data->SetArrayField(TEXT("files"), Rows);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// list_source_control_changes
// ---------------------------------------------------------------------------
// MCP:list_source_control_changes - List the files under a content folder that have local changes in revision control: checked out, added, deleted, modified, conflicted, or new and not yet added. The provider is asked for the folder's state first. With revision control disabled it answers enabled false and no rows, without an error.
//   @param path - Optional content folder. Default /Game.
//   @param max_results - Optional cap on the rows returned. Default 500.
//   @returns { provider: string, enabled: bool, available: bool, path: string, files: [{file, asset_path?, status, source_controlled, checked_out, checked_out_by_other, other_user, added, deleted, modified, current, conflicted, ignored, can_checkout, can_checkin, can_add}], count: int, truncated: bool, note?: string }
TSharedPtr<FJsonObject> HandleListSourceControlChanges(const TSharedPtr<FJsonObject>& Params)
{
    FString Path = TEXT("/Game");
    Params->TryGetStringField(TEXT("path"), Path);
    while (Path.Len() > 1 && Path.EndsWith(TEXT("/")))
    {
        Path.LeftChopInline(1);
    }
    int32 MaxResults = 500;
    if (Params->TryGetNumberField(TEXT("max_results"), MaxResults))
    {
        MaxResults = FMath::Clamp(MaxResults, 1, 100000);
    }

    FString Directory;
    if (!FPackageName::TryConvertLongPackageNameToFilename(Path / TEXT(""), Directory))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("'%s' is not a content folder (/Game, /Game/Maps, /PluginName/...)."), *Path));
    }
    Directory = FPaths::ConvertRelativePathToFull(Directory);
    if (!Directory.EndsWith(TEXT("/")))
    {
        Directory += TEXT("/");
    }

    if (!CanRead())
    {
        TSharedPtr<FJsonObject> Data = UnavailableReaderData();
        Data->SetStringField(TEXT("path"), Path);
        Data->SetArrayField(TEXT("files"), {});
        Data->SetNumberField(TEXT("count"), 0);
        Data->SetBoolField(TEXT("truncated"), false);
        return mcp_common::MakeOkReply(Data);
    }

    ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();
    TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> Update = ISourceControlOperation::Create<FUpdateStatus>();
    Update->SetUpdateModifiedState(true);
    Update->SetQuiet(true);
    if (Provider.Execute(Update, TArray<FString>{ Directory }, EConcurrency::Synchronous) != ECommandResult::Succeeded)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("list_source_control_changes: the provider could not read the state of %s: %s"),
            *Path, *OperationMessages(Update)));
    }

    TArray<FSourceControlStateRef> States = Provider.GetCachedStateByPredicate([&Directory](const FSourceControlStateRef& State)
    {
        if (!State->GetFilename().StartsWith(Directory) || State->IsIgnored())
        {
            return false;
        }
        return HoldsCheckout(State) || State->IsAdded() || State->IsDeleted() || State->IsModified() || State->IsConflicted()
            || (!State->IsSourceControlled() && State->CanAdd());
    });
    States.Sort([](const FSourceControlStateRef& A, const FSourceControlStateRef& B) { return A->GetFilename() < B->GetFilename(); });

    TArray<TSharedPtr<FJsonValue>> Rows;
    for (const FSourceControlStateRef& State : States)
    {
        if (Rows.Num() >= MaxResults)
        {
            break;
        }
        Rows.Add(MakeShared<FJsonValueObject>(StateRow(State, PackageNameOfFile(State->GetFilename()))));
    }
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("provider"), Provider.GetName().ToString());
    Data->SetBoolField(TEXT("enabled"), true);
    Data->SetBoolField(TEXT("available"), true);
    Data->SetStringField(TEXT("path"), Path);
    Data->SetArrayField(TEXT("files"), Rows);
    Data->SetNumberField(TEXT("count"), States.Num());
    Data->SetBoolField(TEXT("truncated"), States.Num() > Rows.Num());
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// checkout_files
// ---------------------------------------------------------------------------
// MCP:checkout_files - Check files out in revision control so they can be edited and saved, through the editor's provider. A file already checked out or added by you counts as checked out. A file checked out by someone else on an exclusive-checkout provider, one not at the latest revision, or one not under revision control comes back in failed with the reason (the other user's name included). With a provider that does not use checkout (Git), it changes nothing and answers provider_uses_checkout false. Idempotent. Refuses while revision control is disabled, and refuses paths under a Read Only Content Root. Success means every file reads checked out afterwards.
//   @param asset_paths - Content paths of the assets (/Game/Maps/Main or /Game/Maps/Main.Main). This or files is required.
//   @param files - Disk paths of other files, absolute or relative to the project folder. This or asset_paths is required.
//   @returns { checked_out: [string], failed: [{file, error}], provider_uses_checkout: bool, note?: string }
//   @hints idempotent
TSharedPtr<FJsonObject> HandleCheckoutFiles(const TSharedPtr<FJsonObject>& Params)
{
    TArray<FScTarget> Targets;
    if (TSharedPtr<FJsonObject> Error = ReadTargets(Params, Targets))
    {
        return Error;
    }
    if (TSharedPtr<FJsonObject> Error = RequireProvider(TEXT("checkout_files")))
    {
        return Error;
    }
    if (TSharedPtr<FJsonObject> Error = RefuseReadOnlyRoots(TEXT("checkout_files"), Targets))
    {
        return Error;
    }

    ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("provider_uses_checkout"), Provider.UsesCheckout());
    if (!Provider.UsesCheckout())
    {
        Data->SetArrayField(TEXT("checked_out"), {});
        Data->SetArrayField(TEXT("failed"), {});
        Data->SetStringField(TEXT("note"), FString::Printf(
            TEXT("The %s provider does not use checkout: its files are writable as they are, so nothing was changed."),
            *Provider.GetName().ToString()));
        return mcp_common::MakeOkReply(Data);
    }

    TArray<FSourceControlStateRef> Before;
    if (!ReadStates(Targets, true, Before))
    {
        return mcp_common::MakeErrorReply(TEXT("checkout_files: the revision control provider did not answer for these files; nothing was changed."));
    }
    TArray<FString> ToCheckOut;
    TMap<FString, FString> Refusals;
    for (int32 Index = 0; Index < Before.Num(); ++Index)
    {
        const FSourceControlStateRef& State = Before[Index];
        const FString& File = Targets[Index].File;
        FString OtherUser;
        if (State->IsCheckedOut() || State->IsAdded())
        {
            continue;
        }
        if (State->IsCheckedOutOther(&OtherUser))
        {
            Refusals.Add(File, FString::Printf(TEXT("checked out by %s"), *OtherUser));
        }
        else if (!State->IsSourceControlled())
        {
            Refusals.Add(File, TEXT("not under revision control: add it with add_files_to_source_control"));
        }
        else if (!State->IsCurrent())
        {
            Refusals.Add(File, TEXT("not at the latest revision: get the latest revision first"));
        }
        else if (!State->CanCheckout())
        {
            Refusals.Add(File, FString::Printf(TEXT("the provider cannot check it out (%s)"), *State->GetDisplayName().ToString()));
        }
        else
        {
            ToCheckOut.Add(File);
        }
    }

    FString OperationError;
    if (ToCheckOut.Num() > 0)
    {
        TSharedRef<FCheckOut, ESPMode::ThreadSafe> CheckOut = ISourceControlOperation::Create<FCheckOut>();
        if (Provider.Execute(CheckOut, ToCheckOut, EConcurrency::Synchronous) != ECommandResult::Succeeded)
        {
            OperationError = OperationMessages(CheckOut);
        }
    }

    TArray<FSourceControlStateRef> After;
    const bool bReread = ReadStates(Targets, true, After);
    TArray<FString> CheckedOut;
    TArray<TSharedPtr<FJsonValue>> Failed;
    for (int32 Index = 0; Index < Targets.Num(); ++Index)
    {
        const FString& File = Targets[Index].File;
        if (bReread && (After[Index]->IsCheckedOut() || After[Index]->IsAdded()))
        {
            CheckedOut.Add(File);
            continue;
        }
        FString Reason;
        if (const FString* Refusal = Refusals.Find(File))
        {
            Reason = *Refusal;
        }
        else if (!bReread)
        {
            Reason = TEXT("its state could not be read back after the checkout");
        }
        else if (After[Index]->IsCheckedOutOther(&Reason))
        {
            Reason = FString::Printf(TEXT("checked out by %s"), *Reason);
        }
        else
        {
            Reason = OperationError.IsEmpty() ? FString::Printf(TEXT("still %s after the checkout"), *After[Index]->GetDisplayName().ToString()) : OperationError;
        }
        Failed.Add(MakeShared<FJsonValueObject>(FailedRow(File, Reason)));
    }
    SetStrings(Data, TEXT("checked_out"), CheckedOut);
    Data->SetArrayField(TEXT("failed"), Failed);
    return mcp_common::MakeOutcomeReply(Data, Failed.Num() == 0,
        Failed.Num() == 0 ? FString() : FString::Printf(TEXT("checkout_files: %d of %d file(s) are not checked out; see failed."), Failed.Num(), Targets.Num()));
}

// ---------------------------------------------------------------------------
// add_files_to_source_control
// ---------------------------------------------------------------------------
// MCP:add_files_to_source_control - Mark new files for add in revision control, so the next submit includes them. The files must exist on disk: save a new asset first. A file already under revision control or already added counts as added. A file the provider ignores (.gitignore, .p4ignore) comes back in failed. Idempotent. Refuses while revision control is disabled, and refuses paths under a Read Only Content Root. Success means every file reads added or under revision control afterwards.
//   @param asset_paths - Content paths of the assets (/Game/Maps/Main or /Game/Maps/Main.Main). This or files is required.
//   @param files - Disk paths of other files, absolute or relative to the project folder. This or asset_paths is required.
//   @returns { added: [string], failed: [{file, error}] }
//   @hints additive idempotent
TSharedPtr<FJsonObject> HandleAddFilesToSourceControl(const TSharedPtr<FJsonObject>& Params)
{
    TArray<FScTarget> Targets;
    if (TSharedPtr<FJsonObject> Error = ReadTargets(Params, Targets))
    {
        return Error;
    }
    if (TSharedPtr<FJsonObject> Error = RequireProvider(TEXT("add_files_to_source_control")))
    {
        return Error;
    }
    if (TSharedPtr<FJsonObject> Error = RefuseReadOnlyRoots(TEXT("add_files_to_source_control"), Targets))
    {
        return Error;
    }

    ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();
    TArray<FSourceControlStateRef> Before;
    if (!ReadStates(Targets, true, Before))
    {
        return mcp_common::MakeErrorReply(TEXT("add_files_to_source_control: the revision control provider did not answer for these files; nothing was changed."));
    }
    TArray<FString> ToAdd;
    TMap<FString, FString> Refusals;
    for (int32 Index = 0; Index < Before.Num(); ++Index)
    {
        const FSourceControlStateRef& State = Before[Index];
        const FString& File = Targets[Index].File;
        if (State->IsAdded() || State->IsSourceControlled())
        {
            continue;
        }
        if (!FPaths::FileExists(File))
        {
            Refusals.Add(File, TEXT("no file on disk: save the asset first"));
        }
        else if (State->IsIgnored())
        {
            Refusals.Add(File, TEXT("the provider ignores this file (.gitignore, .p4ignore)"));
        }
        else if (!State->CanAdd())
        {
            Refusals.Add(File, FString::Printf(TEXT("the provider cannot add it (%s)"), *State->GetDisplayName().ToString()));
        }
        else
        {
            ToAdd.Add(File);
        }
    }

    FString OperationError;
    if (ToAdd.Num() > 0)
    {
        TSharedRef<FMarkForAdd, ESPMode::ThreadSafe> MarkForAdd = ISourceControlOperation::Create<FMarkForAdd>();
        if (Provider.Execute(MarkForAdd, ToAdd, EConcurrency::Synchronous) != ECommandResult::Succeeded)
        {
            OperationError = OperationMessages(MarkForAdd);
        }
    }

    TArray<FSourceControlStateRef> After;
    const bool bReread = ReadStates(Targets, true, After);
    TArray<FString> Added;
    TArray<TSharedPtr<FJsonValue>> Failed;
    for (int32 Index = 0; Index < Targets.Num(); ++Index)
    {
        const FString& File = Targets[Index].File;
        if (bReread && (After[Index]->IsAdded() || After[Index]->IsSourceControlled()))
        {
            Added.Add(File);
            continue;
        }
        FString Reason;
        if (const FString* Refusal = Refusals.Find(File))
        {
            Reason = *Refusal;
        }
        else if (!bReread)
        {
            Reason = TEXT("its state could not be read back after the add");
        }
        else
        {
            Reason = OperationError.IsEmpty() ? FString::Printf(TEXT("still %s after the add"), *After[Index]->GetDisplayName().ToString()) : OperationError;
        }
        Failed.Add(MakeShared<FJsonValueObject>(FailedRow(File, Reason)));
    }
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    SetStrings(Data, TEXT("added"), Added);
    Data->SetArrayField(TEXT("failed"), Failed);
    return mcp_common::MakeOutcomeReply(Data, Failed.Num() == 0,
        Failed.Num() == 0 ? FString() : FString::Printf(TEXT("add_files_to_source_control: %d of %d file(s) are not added; see failed."), Failed.Num(), Targets.Num()));
}

// ---------------------------------------------------------------------------
// revert_files
// ---------------------------------------------------------------------------
// MCP:revert_files - Revert files in revision control: local changes are discarded and the file goes back to the revision you have; a checkout is released and an add is undone (the file stays on disk, no longer added). The loaded packages among them are reloaded from disk, discarding their unsaved edits too, and their undo history no longer applies. A level loaded in the editor is refused: open another level first. Refuses while revision control is disabled, and refuses paths under a Read Only Content Root. Success means every file reads unchanged afterwards.
//   @param asset_paths - Content paths of the assets (/Game/Maps/Main or /Game/Maps/Main.Main). This or files is required.
//   @param files - Disk paths of other files, absolute or relative to the project folder. This or asset_paths is required.
//   @param reload - Optional. Reload the loaded packages from disk after the revert. Default true; with false they keep what is in memory, which no longer matches the file.
//   @returns { reverted: [string], reloaded: [string], failed: [{file, error}], not_reloaded?: [string], note?: string }
TSharedPtr<FJsonObject> HandleRevertFiles(const TSharedPtr<FJsonObject>& Params)
{
    TArray<FScTarget> Targets;
    if (TSharedPtr<FJsonObject> Error = ReadTargets(Params, Targets))
    {
        return Error;
    }
    bool bReload = true;
    Params->TryGetBoolField(TEXT("reload"), bReload);
    if (TSharedPtr<FJsonObject> Error = RequireProvider(TEXT("revert_files")))
    {
        return Error;
    }
    if (TSharedPtr<FJsonObject> Error = RefuseReadOnlyRoots(TEXT("revert_files"), Targets))
    {
        return Error;
    }

    TArray<UPackage*> LoadedPackages;
    TArray<FString> LoadedLevels;
    for (const FScTarget& Target : Targets)
    {
        if (Target.AssetPath.IsEmpty())
        {
            continue;
        }
        if (UPackage* Package = FindPackage(nullptr, *Target.AssetPath))
        {
            if (UWorld::FindWorldInPackage(Package))
            {
                LoadedLevels.Add(Target.AssetPath);
            }
            LoadedPackages.Add(Package);
        }
    }
    if (LoadedLevels.Num() > 0)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("revert_files: refused, nothing was changed. These levels are loaded in the editor, and reverting one under ")
            TEXT("it would leave the open world out of step with its file: %s. Open another level first (load_level)."),
            *JoinNames(LoadedLevels)));
    }

    // A loaded package keeps its file open through its linker; release it so the provider can rewrite the file.
    if (LoadedPackages.Num() > 0)
    {
        TArray<UObject*> Outers;
        for (UPackage* Package : LoadedPackages)
        {
            Outers.Add(Package);
        }
        FlushAsyncLoading();
        ResetLoaders(Outers);
    }

    ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();
    TSharedRef<FRevert, ESPMode::ThreadSafe> Revert = ISourceControlOperation::Create<FRevert>();
    const bool bRevertOk = Provider.Execute(Revert, FilesOf(Targets), EConcurrency::Synchronous) == ECommandResult::Succeeded;
    const FString OperationError = bRevertOk ? FString() : OperationMessages(Revert);

    TArray<FString> Reloaded;
    TArray<FString> NotReloaded;
    FString ReloadError;
    if (LoadedPackages.Num() > 0)
    {
        if (bReload)
        {
            TArray<UPackage*> ToReload;
            for (UPackage* Package : LoadedPackages)
            {
                if (FPaths::FileExists(USourceControlHelpers::PackageFilename(Package)))
                {
                    ToReload.Add(Package);
                }
                else
                {
                    NotReloaded.Add(Package->GetName());
                }
            }
            TArray<FString> Names;
            for (UPackage* Package : ToReload)
            {
                Names.Add(Package->GetName());
            }
            FText ReloadErrorText;
            if (ToReload.Num() > 0)
            {
                UPackageTools::ReloadPackages(ToReload, ReloadErrorText, EReloadPackagesInteractionMode::AssumePositive);
            }
            ReloadError = ReloadErrorText.ToString();
            for (const FString& Name : Names)
            {
                UPackage* Package = FindPackage(nullptr, *Name);
                if (Package && !Package->IsDirty())
                {
                    Reloaded.Add(Name);
                }
                else
                {
                    NotReloaded.Add(Name);
                }
            }
        }
        else
        {
            for (UPackage* Package : LoadedPackages)
            {
                NotReloaded.Add(Package->GetName());
            }
        }
    }

    TArray<FSourceControlStateRef> After;
    const bool bReread = ReadStates(Targets, true, After);
    TArray<FString> Reverted;
    TArray<TSharedPtr<FJsonValue>> Failed;
    for (int32 Index = 0; Index < Targets.Num(); ++Index)
    {
        const FString& File = Targets[Index].File;
        if (bReread && !HoldsCheckout(After[Index]) && !After[Index]->IsAdded() && !After[Index]->IsModified() && !After[Index]->IsDeleted())
        {
            Reverted.Add(File);
            continue;
        }
        const FString Reason = !bReread ? FString(TEXT("its state could not be read back after the revert"))
            : !OperationError.IsEmpty() ? OperationError
            : FString::Printf(TEXT("still %s after the revert"), *After[Index]->GetDisplayName().ToString());
        Failed.Add(MakeShared<FJsonValueObject>(FailedRow(File, Reason)));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    SetStrings(Data, TEXT("reverted"), Reverted);
    SetStrings(Data, TEXT("reloaded"), Reloaded);
    Data->SetArrayField(TEXT("failed"), Failed);
    if (NotReloaded.Num() > 0)
    {
        SetStrings(Data, TEXT("not_reloaded"), NotReloaded);
    }
    TArray<FString> Notes;
    if (Reloaded.Num() > 0)
    {
        Notes.Add(TEXT("The reloaded packages lost their unsaved edits, and the undo history recorded for them no longer applies."));
    }
    if (NotReloaded.Num() > 0)
    {
        Notes.Add(bReload
            ? FString::Printf(TEXT("Not reloaded (no file on disk after the revert, or the reload failed%s): what is in memory no longer matches the file."),
                ReloadError.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(": %s"), *ReloadError))
            : FString(TEXT("reload was false: these loaded packages keep what is in memory, which no longer matches the file.")));
    }
    if (Notes.Num() > 0)
    {
        Data->SetStringField(TEXT("note"), FString::Join(Notes, TEXT(" ")));
    }
    return mcp_common::MakeOutcomeReply(Data, Failed.Num() == 0,
        Failed.Num() == 0 ? FString() : FString::Printf(TEXT("revert_files: %d of %d file(s) still have changes; see failed."), Failed.Num(), Targets.Num()));
}

// ---------------------------------------------------------------------------
// Submit: one asynchronous FCheckIn at a time.
// ---------------------------------------------------------------------------
namespace
{
    // The provider runs the check-in on its own thread and calls back on the game thread from its Tick, so every field
    // is read and written on the game thread only.
    struct FSubmitJob
    {
        TSharedPtr<FCheckIn, ESPMode::ThreadSafe> Operation;
        TArray<FScTarget> Targets;
        bool bKeepCheckedOut = false;
        FDateTime StartTime;
        FDateTime EndTime;
        bool bFinished = false;
        bool bCommandSucceeded = false;
        bool bCancelRequested = false;
        FString Message;
        TArray<FString> Submitted;
        TArray<TSharedPtr<FJsonValue>> NotSubmitted;
    };
    TSharedPtr<FSubmitJob> CurrentSubmit;
    const FDelegateHandle ReleaseSubmitAtExit = FCoreDelegates::OnPreExit.AddLambda([] { CurrentSubmit.Reset(); });

    /** Records the outcome of the finished check-in from a re-read of every file's state. */
    void FinishSubmit(FSubmitJob& Job, ECommandResult::Type Result)
    {
        Job.bFinished = true;
        Job.EndTime = FDateTime::UtcNow();
        Job.bCommandSucceeded = Result == ECommandResult::Succeeded;
        const FString Success = Job.Operation->GetSuccessMessage().ToString();
        const FString Messages = OperationMessages(Job.Operation.ToSharedRef());
        Job.Message = Job.bCommandSucceeded && !Success.IsEmpty() ? Success
            : Result == ECommandResult::Cancelled ? FString::Printf(TEXT("Canceled. %s"), *Messages)
            : Messages;

        TArray<FSourceControlStateRef> States;
        const bool bReread = ReadStates(Job.Targets, false, States);
        for (int32 Index = 0; Index < Job.Targets.Num(); ++Index)
        {
            const FString& File = Job.Targets[Index].File;
            if (!bReread)
            {
                Job.NotSubmitted.Add(MakeShared<FJsonValueObject>(FailedRow(File, TEXT("its state could not be read back after the submit"))));
                continue;
            }
            const FSourceControlStateRef& State = States[Index];
            // A submitted deletion leaves no file and no revision-control entry behind.
            const bool bSubmittedDelete = !State->IsSourceControlled() && !FPaths::FileExists(File);
            const bool bSubmitted = bSubmittedDelete || (State->IsSourceControlled() && !State->IsAdded() && !State->IsDeleted()
                && !State->IsModified() && (Job.bKeepCheckedOut || !HoldsCheckout(State)));
            if (Job.bCommandSucceeded && bSubmitted)
            {
                Job.Submitted.Add(File);
            }
            else
            {
                Job.NotSubmitted.Add(MakeShared<FJsonValueObject>(FailedRow(File,
                    FString::Printf(TEXT("%s after the submit"), *State->GetDisplayName().ToString()))));
            }
        }
    }
}

// ---------------------------------------------------------------------------
// start_source_control_submit
// ---------------------------------------------------------------------------
// MCP:start_source_control_submit - Start submitting files to revision control with a description (a Perforce submit, a Git commit), asynchronously: the call returns once the provider has the job; poll get_source_control_submit_status for the outcome. Files must be saved: it refuses when any of them has unsaved changes in the editor, naming them, and never saves for you. New files must be added first with add_files_to_source_control. One submit at a time: while one runs, a second answers already_running true and starts nothing. Refuses while revision control is disabled, and refuses paths under a Read Only Content Root.
//   @param asset_paths - Content paths of the assets (/Game/Maps/Main or /Game/Maps/Main.Main). This or files is required.
//   @param files - Disk paths of other files, absolute or relative to the project folder. This or asset_paths is required.
//   @param description - The changelist description or commit message. Required.
//   @param keep_checked_out - Optional. Keep the files checked out after the submit, on providers that check out. Default false.
//   @returns { submit_started: bool, already_running: bool, file_count: int, note: string }
//   @hints destructive open-world
TSharedPtr<FJsonObject> HandleStartSourceControlSubmit(const TSharedPtr<FJsonObject>& Params)
{
    if (CurrentSubmit.IsValid() && !CurrentSubmit->bFinished)
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetBoolField(TEXT("submit_started"), false);
        Data->SetBoolField(TEXT("already_running"), true);
        Data->SetNumberField(TEXT("file_count"), CurrentSubmit->Targets.Num());
        Data->SetStringField(TEXT("note"), TEXT("A submit is already running and nothing was started; poll get_source_control_submit_status, or cancel_source_control_submit it first."));
        return mcp_common::MakeOkReply(Data);
    }

    TArray<FScTarget> Targets;
    if (TSharedPtr<FJsonObject> Error = ReadTargets(Params, Targets))
    {
        return Error;
    }
    FString Description;
    if (TSharedPtr<FJsonObject> Error = mcp_common::ReadRequiredString(Params, TEXT("description"), Description))
    {
        return Error;
    }
    bool bKeepCheckedOut = false;
    Params->TryGetBoolField(TEXT("keep_checked_out"), bKeepCheckedOut);
    if (TSharedPtr<FJsonObject> Error = RequireProvider(TEXT("start_source_control_submit")))
    {
        return Error;
    }
    if (TSharedPtr<FJsonObject> Error = RefuseReadOnlyRoots(TEXT("start_source_control_submit"), Targets))
    {
        return Error;
    }

    TArray<FString> Unsaved;
    for (const FScTarget& Target : Targets)
    {
        if (Target.AssetPath.IsEmpty())
        {
            continue;
        }
        const UPackage* Package = FindPackage(nullptr, *Target.AssetPath);
        if (Package && Package->IsDirty())
        {
            Unsaved.Add(Target.AssetPath);
        }
    }
    if (Unsaved.Num() > 0)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("start_source_control_submit: refused, nothing was submitted. These packages have unsaved changes in the ")
            TEXT("editor: %s. Save them (save_asset) or discard the changes first."), *JoinNames(Unsaved)));
    }

    TSharedPtr<FSubmitJob> Job = MakeShared<FSubmitJob>();
    Job->Targets = Targets;
    Job->bKeepCheckedOut = bKeepCheckedOut;
    Job->StartTime = FDateTime::UtcNow();
    Job->Operation = ISourceControlOperation::Create<FCheckIn>();
    Job->Operation->SetDescription(FText::FromString(Description));
    Job->Operation->SetKeepCheckedOut(bKeepCheckedOut);
    CurrentSubmit = Job;

    TWeakPtr<FSubmitJob> WeakJob = Job;
    ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();
    const ECommandResult::Type Issued = Provider.Execute(Job->Operation.ToSharedRef(), FilesOf(Targets), EConcurrency::Asynchronous,
        FSourceControlOperationComplete::CreateLambda([WeakJob](const FSourceControlOperationRef&, ECommandResult::Type Result)
        {
            if (TSharedPtr<FSubmitJob> Pinned = WeakJob.Pin())
            {
                if (!Pinned->bFinished)
                {
                    FinishSubmit(*Pinned, Result);
                }
            }
        }));
    if (Issued == ECommandResult::Failed && !Job->bFinished)
    {
        FinishSubmit(*Job, ECommandResult::Failed);
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    const bool bStarted = Issued != ECommandResult::Failed;
    Data->SetBoolField(TEXT("submit_started"), bStarted);
    Data->SetBoolField(TEXT("already_running"), false);
    Data->SetNumberField(TEXT("file_count"), Targets.Num());
    Data->SetStringField(TEXT("note"), bStarted
        ? FString::Printf(TEXT("The %s provider is submitting %d file(s); poll get_source_control_submit_status for the outcome."), *Provider.GetName().ToString(), Targets.Num())
        : FString::Printf(TEXT("The provider refused to start the submit: %s"), *Job->Message));
    return mcp_common::MakeOutcomeReply(Data, bStarted, bStarted ? FString() : TEXT("start_source_control_submit: the provider refused to start the submit; see note."));
}

// ---------------------------------------------------------------------------
// get_source_control_submit_status
// ---------------------------------------------------------------------------
// MCP:get_source_control_submit_status - Poll the submit that start_source_control_submit started in this editor session: running or finished, whether it succeeded, the provider's message, the files submitted (read back from the provider) and those that were not. has_submit false when none was started this session.
//   @returns { has_submit: bool, running?: bool, finished?: bool, success?: bool, message?: string, submitted?: [string], not_submitted?: [{file, error}], file_count?: int, elapsed_seconds?: number }
TSharedPtr<FJsonObject> HandleGetSourceControlSubmitStatus(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    if (!CurrentSubmit.IsValid())
    {
        Data->SetBoolField(TEXT("has_submit"), false);
        return mcp_common::MakeOkReply(Data);
    }
    const FSubmitJob& Job = *CurrentSubmit;
    const FDateTime EndPoint = Job.bFinished ? Job.EndTime : FDateTime::UtcNow();
    Data->SetBoolField(TEXT("has_submit"), true);
    Data->SetBoolField(TEXT("running"), !Job.bFinished);
    Data->SetBoolField(TEXT("finished"), Job.bFinished);
    Data->SetNumberField(TEXT("file_count"), Job.Targets.Num());
    Data->SetNumberField(TEXT("elapsed_seconds"), (EndPoint - Job.StartTime).GetTotalSeconds());
    if (Job.bFinished)
    {
        Data->SetBoolField(TEXT("success"), Job.bCommandSucceeded && Job.NotSubmitted.Num() == 0);
        Data->SetStringField(TEXT("message"), Job.Message);
        SetStrings(Data, TEXT("submitted"), Job.Submitted);
        Data->SetArrayField(TEXT("not_submitted"), Job.NotSubmitted);
    }
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// cancel_source_control_submit
// ---------------------------------------------------------------------------
// MCP:cancel_source_control_submit - Ask the provider to cancel the submit that start_source_control_submit started, if it is still running and the provider can cancel it (Git cannot; a submit that already reached the server may complete anyway). Idempotent: with no submit running nothing happens. canceled true means the cancel was requested; confirm with get_source_control_submit_status.
//   @returns { canceled: bool, was_running: bool, note?: string }
//   @hints idempotent
TSharedPtr<FJsonObject> HandleCancelSourceControlSubmit(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    const bool bWasRunning = CurrentSubmit.IsValid() && !CurrentSubmit->bFinished;
    bool bCanceled = false;
    if (bWasRunning)
    {
        ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();
        const FSourceControlOperationRef Operation = CurrentSubmit->Operation.ToSharedRef();
        if (Provider.CanCancelOperation(Operation))
        {
            Provider.CancelOperation(Operation);
            CurrentSubmit->bCancelRequested = true;
            bCanceled = true;
        }
        else
        {
            Data->SetStringField(TEXT("note"), FString::Printf(
                TEXT("The %s provider cannot cancel a submit in progress; it keeps running. Poll get_source_control_submit_status."),
                *Provider.GetName().ToString()));
        }
    }
    Data->SetBoolField(TEXT("canceled"), bCanceled);
    Data->SetBoolField(TEXT("was_running"), bWasRunning);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_source_control_history
// ---------------------------------------------------------------------------
// MCP:get_source_control_history - Read the revision history of one file from revision control, newest first: revision, user, date, description and changelist of each. With revision control disabled it answers enabled false and no revisions, without an error.
//   @param asset_path - Content path of the asset. This or file is required.
//   @param file - Disk path of another file, absolute or relative to the project folder. This or asset_path is required.
//   @param max_results - Optional cap on the revisions returned. Default 20.
//   @returns { provider: string, enabled: bool, available: bool, file?: string, revisions: [{revision, user, date, description, changelist}], count: int, truncated?: bool, note?: string }
TSharedPtr<FJsonObject> HandleGetSourceControlHistory(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    FString FileParam;
    Params->TryGetStringField(TEXT("asset_path"), AssetPath);
    Params->TryGetStringField(TEXT("file"), FileParam);
    if (AssetPath.IsEmpty() == FileParam.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Name one file: asset_path (a content path) or file (a disk path)."));
    }
    int32 MaxResults = 20;
    if (Params->TryGetNumberField(TEXT("max_results"), MaxResults))
    {
        MaxResults = FMath::Clamp(MaxResults, 1, 1000);
    }

    TSharedPtr<FJsonObject> TargetParams = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> OnePath;
    OnePath.Add(MakeShared<FJsonValueString>(AssetPath.IsEmpty() ? FileParam : AssetPath));
    TargetParams->SetArrayField(AssetPath.IsEmpty() ? TEXT("files") : TEXT("asset_paths"), OnePath);
    TArray<FScTarget> Targets;
    if (TSharedPtr<FJsonObject> Error = ReadTargets(TargetParams, Targets))
    {
        return Error;
    }
    const FString File = Targets[0].File;

    if (!CanRead())
    {
        TSharedPtr<FJsonObject> Data = UnavailableReaderData();
        Data->SetArrayField(TEXT("revisions"), {});
        Data->SetNumberField(TEXT("count"), 0);
        return mcp_common::MakeOkReply(Data);
    }

    ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();
    TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> Update = ISourceControlOperation::Create<FUpdateStatus>();
    Update->SetUpdateHistory(true);
    if (Provider.Execute(Update, TArray<FString>{ File }, EConcurrency::Synchronous) != ECommandResult::Succeeded)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("get_source_control_history: the provider could not read the history of %s: %s"),
            *File, *OperationMessages(Update)));
    }
    const FSourceControlStatePtr State = Provider.GetState(File, EStateCacheUsage::Use);
    TArray<TSharedPtr<FJsonValue>> Revisions;
    const int32 HistorySize = State.IsValid() ? State->GetHistorySize() : 0;
    for (int32 Index = 0; Index < HistorySize && Revisions.Num() < MaxResults; ++Index)
    {
        const TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> Revision = State->GetHistoryItem(Index);
        if (!Revision.IsValid())
        {
            continue;
        }
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("revision"), Revision->GetRevision());
        Row->SetStringField(TEXT("user"), Revision->GetUserName());
        Row->SetStringField(TEXT("date"), Revision->GetDate().ToIso8601());
        Row->SetStringField(TEXT("description"), Revision->GetDescription());
        Row->SetNumberField(TEXT("changelist"), Revision->GetCheckInIdentifier());
        Revisions.Add(MakeShared<FJsonValueObject>(Row));
    }
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("provider"), Provider.GetName().ToString());
    Data->SetBoolField(TEXT("enabled"), true);
    Data->SetBoolField(TEXT("available"), true);
    Data->SetStringField(TEXT("file"), File);
    Data->SetArrayField(TEXT("revisions"), Revisions);
    Data->SetNumberField(TEXT("count"), Revisions.Num());
    Data->SetBoolField(TEXT("truncated"), HistorySize > Revisions.Num());
    return mcp_common::MakeOkReply(Data);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_files_to_source_control"), &HandleAddFilesToSourceControl },
    { TEXT("cancel_source_control_submit"), &HandleCancelSourceControlSubmit },
    { TEXT("checkout_files"), &HandleCheckoutFiles },
    { TEXT("get_source_control_file_status"), &HandleGetSourceControlFileStatus },
    { TEXT("get_source_control_history"), &HandleGetSourceControlHistory },
    { TEXT("get_source_control_status"), &HandleGetSourceControlStatus },
    { TEXT("get_source_control_submit_status"), &HandleGetSourceControlSubmitStatus },
    { TEXT("list_source_control_changes"), &HandleListSourceControlChanges },
    { TEXT("revert_files"), &HandleRevertFiles },
    { TEXT("start_source_control_submit"), &HandleStartSourceControlSubmit },
};
} // namespace UEAgentSourceControlCommands
