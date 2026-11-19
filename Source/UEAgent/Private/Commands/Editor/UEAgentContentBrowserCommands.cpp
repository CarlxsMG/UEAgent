// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

#include "Editor.h"                                  // GEditor
#include "Subsystems/AssetEditorSubsystem.h"         // UAssetEditorSubsystem, IAssetEditorInstance
#include "ContentBrowserModule.h"                    // FContentBrowserModule
#include "IContentBrowserSingleton.h"                // IContentBrowserSingleton (selection, current path, sync)
#include "ContentBrowserItemPath.h"                  // FContentBrowserItemPath (ContentBrowserData)
#include "AssetRegistry/AssetData.h"
#include "Engine/World.h"                            // UWorld: opening one loads the level, refused here
#include "Modules/ModuleManager.h"
#include "Misc/App.h"                                // FApp::CanEverRender (headless gate)
#include "UObject/Package.h"

/**
 * Content Browser and asset editor commands: what the user has selected or is looking at in the
 * Content Browser, pointing the browser at assets, and opening, closing and listing asset editors.
 *
 *   - get_selected_assets            IContentBrowserSingleton::GetSelectedAssets / GetSelectedFolders
 *   - get_content_browser_path       IContentBrowserSingleton::GetCurrentPath
 *   - sync_content_browser_to_assets IContentBrowserSingleton::SyncBrowserToAssets
 *   - open_asset_editor              UAssetEditorSubsystem::OpenEditorForAsset, read back with FindEditorForAsset
 *   - close_asset_editor             UAssetEditorSubsystem::CloseAllEditorsForAsset / CloseAllAssetEditors
 *   - list_open_asset_editors        UAssetEditorSubsystem::GetAllOpenEditors / GetAllEditedAssets
 */
namespace UEAgentContentBrowserCommands
{

// MCP-DOMAIN: Editor

namespace
{
    // The Content Browser singleton, loading its module when it is not loaded yet; null when the
    // module cannot be loaded.
    IContentBrowserSingleton* ContentBrowserSingleton()
    {
        FContentBrowserModule* Module = FModuleManager::LoadModulePtr<FContentBrowserModule>(TEXT("ContentBrowser"));
        return Module != nullptr ? &Module->Get() : nullptr;
    }

    TSharedPtr<FJsonObject> NoContentBrowserModuleReply()
    {
        return mcp_common::MakeErrorReply(TEXT("The ContentBrowser module is not available in this editor"));
    }

    UAssetEditorSubsystem* AssetEditorSubsystem()
    {
        return GEditor != nullptr ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
    }

    TSharedPtr<FJsonObject> NoAssetEditorSubsystemReply()
    {
        return mcp_common::MakeErrorReply(TEXT("No editor: the asset editor subsystem is not available"));
    }

    TSharedPtr<FJsonObject> AssetDataToJson(const FAssetData& Data)
    {
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("asset_path"), Data.GetObjectPathString());
        Row->SetStringField(TEXT("package_name"), Data.PackageName.ToString());
        Row->SetStringField(TEXT("asset_name"), Data.AssetName.ToString());
        Row->SetStringField(TEXT("asset_class"), Data.AssetClassPath.ToString());
        return Row;
    }

    TSharedPtr<FJsonObject> LoadedAssetToJson(const UObject* Asset)
    {
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("asset_path"), Asset->GetPathName());
        Row->SetStringField(TEXT("asset_class"), Asset->GetClass()->GetPathName());
        const UPackage* Package = Asset->GetPackage();
        Row->SetBoolField(TEXT("dirty"), Package != nullptr && Package->IsDirty());
        return Row;
    }

    // The registry entry an asset_path param names: an object path, a package path, or a bare
    // asset name that matches exactly one asset. Invalid with OutError set otherwise.
    FAssetData ResolveAssetParam(const FString& Path, FString& OutError)
    {
        const FAssetData Direct = mcp_common::FindAssetDataByPath(Path);
        if (Direct.IsValid())
        {
            return Direct;
        }
        if (!Path.Contains(TEXT("/")))
        {
            TArray<FAssetData> Matches;
            FString FlexibleError;
            if (mcp_common::ResolveAssetFlexible(Path, nullptr, FString(), Matches, FlexibleError))
            {
                if (Matches.Num() == 1)
                {
                    return Matches[0];
                }
                TArray<FString> Candidates;
                for (const FAssetData& Match : Matches)
                {
                    Candidates.Add(Match.GetObjectPathString());
                }
                OutError = FString::Printf(TEXT("'%s' names %d assets; pass one of these paths: %s"),
                    *Path, Matches.Num(), *FString::Join(Candidates, TEXT(", ")));
                return FAssetData();
            }
        }
        OutError = FString::Printf(TEXT("No asset found at '%s'"), *Path);
        return FAssetData();
    }

    // True when an asset editor can actually be shown: a rendering editor, not a -nullrhi one.
    bool CanShowEditorUI()
    {
        return FApp::CanEverRender() && !IsRunningCommandlet();
    }
}

// MCP:get_selected_assets - Read what is selected in the primary Content Browser right now: each asset's path, package, name and class, plus the selected folders. Use it to act on "the assets the user just clicked" without asking for paths, and as the read-back for sync_content_browser_to_assets. Nothing is loaded. Without a Content Browser (headless editor, or every browser tab closed) the lists are empty and has_content_browser is false.
//   @param include_folders - Optional bool (default true): also report the selected folders of the asset view and of the path tree.
//   @returns { assets: [{ asset_path: string, package_name: string, asset_name: string, asset_class: string }], count: int, folders: [string], path_view_folders: [string], has_content_browser: bool }

TSharedPtr<FJsonObject> HandleGetSelectedAssets(const TSharedPtr<FJsonObject>& Params)
{
    bool bIncludeFolders = true;
    Params->TryGetBoolField(TEXT("include_folders"), bIncludeFolders);

    IContentBrowserSingleton* Browser = ContentBrowserSingleton();
    if (Browser == nullptr)
    {
        return NoContentBrowserModuleReply();
    }

    const bool bHasBrowser = Browser->HasPrimaryContentBrowser();
    TArray<FAssetData> Selected;
    TArray<FString> Folders;
    TArray<FString> PathViewFolders;
    if (bHasBrowser)
    {
        Browser->GetSelectedAssets(Selected);
        if (bIncludeFolders)
        {
            Browser->GetSelectedFolders(Folders);
            Browser->GetSelectedPathViewFolders(PathViewFolders);
        }
    }

    TArray<TSharedPtr<FJsonValue>> Rows;
    for (const FAssetData& Data : Selected)
    {
        Rows.Add(MakeShared<FJsonValueObject>(AssetDataToJson(Data)));
    }

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetArrayField(TEXT("assets"), Rows);
    Reply->SetNumberField(TEXT("count"), Rows.Num());
    Reply->SetArrayField(TEXT("folders"), mcp_common::StringsToJson(Folders));
    Reply->SetArrayField(TEXT("path_view_folders"), mcp_common::StringsToJson(PathViewFolders));
    Reply->SetBoolField(TEXT("has_content_browser"), bHasBrowser);
    return mcp_common::MakeOkReply(Reply);
}

// MCP:get_content_browser_path - Read the folder the primary Content Browser is showing: its content path ('/Game/Maps', what the asset tools take) and its virtual path as the browser displays it ('/All/Game/Maps'). Use it to create or import assets "where the user is looking". path is empty when the browser shows a virtual folder with no content path behind it, or when there is no Content Browser (has_content_browser false).
//   @returns { path: string, virtual_path: string, has_internal_path: bool, has_content_browser: bool }

TSharedPtr<FJsonObject> HandleGetContentBrowserPath(const TSharedPtr<FJsonObject>& Params)
{
    IContentBrowserSingleton* Browser = ContentBrowserSingleton();
    if (Browser == nullptr)
    {
        return NoContentBrowserModuleReply();
    }

    const bool bHasBrowser = Browser->HasPrimaryContentBrowser();
    FString InternalPath;
    FString VirtualPath;
    bool bHasInternalPath = false;
    if (bHasBrowser)
    {
        const FContentBrowserItemPath Current = Browser->GetCurrentPath();
        bHasInternalPath = Current.HasInternalPath();
        if (bHasInternalPath)
        {
            InternalPath = Current.GetInternalPathString();
        }
        VirtualPath = Current.GetVirtualPathString();
    }

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("path"), InternalPath);
    Reply->SetStringField(TEXT("virtual_path"), VirtualPath);
    Reply->SetBoolField(TEXT("has_internal_path"), bHasInternalPath);
    Reply->SetBoolField(TEXT("has_content_browser"), bHasBrowser);
    return mcp_common::MakeOkReply(Reply);
}

// MCP:sync_content_browser_to_assets - Point the Content Browser at assets: it navigates to their folder and selects them, the way "Browse to Asset" does, so the user sees what you mean. Nothing is loaded or changed on disk. Paths that name no asset are reported in not_found and the rest are still synced; a call where none resolves fails. The browser applies the selection on its next ticks, so the reply cannot read it back: call get_selected_assets afterwards to confirm. Refused in a headless editor with no Content Browser.
//   @param asset_paths - Required array of asset paths: object paths ('/Game/A/B.B'), package paths ('/Game/A/B') or unambiguous bare asset names.
//   @param focus - Optional bool (default true): bring the Content Browser tab to the front.
//   @param allow_locked_browsers - Optional bool (default false): also sync a Content Browser the user has locked.
//   @returns { synced: [{ asset_path: string, package_name: string, asset_name: string, asset_class: string }], synced_count: int, not_found: [{ path: string, error: string }], has_content_browser: bool, focus: bool }
//   @hints additive idempotent

TSharedPtr<FJsonObject> HandleSyncContentBrowserToAssets(const TSharedPtr<FJsonObject>& Params)
{
    const TArray<TSharedPtr<FJsonValue>>* PathValues = nullptr;
    if (!Params->TryGetArrayField(TEXT("asset_paths"), PathValues) || PathValues == nullptr || PathValues->Num() == 0)
    {
        return mcp_common::MissingParameterReply(TEXT("asset_paths"));
    }
    bool bFocus = true;
    Params->TryGetBoolField(TEXT("focus"), bFocus);
    bool bAllowLocked = false;
    Params->TryGetBoolField(TEXT("allow_locked_browsers"), bAllowLocked);

    IContentBrowserSingleton* Browser = ContentBrowserSingleton();
    if (Browser == nullptr)
    {
        return NoContentBrowserModuleReply();
    }
    // With no browser open, a sync spawns one; that needs an editor that can show a window.
    if (!Browser->HasPrimaryContentBrowser() && !CanShowEditorUI())
    {
        return mcp_common::MakeErrorReply(TEXT("No Content Browser to sync: this editor runs headless (-nullrhi or a commandlet)"));
    }

    TArray<FAssetData> ToSync;
    TArray<TSharedPtr<FJsonValue>> SyncedRows;
    TArray<TSharedPtr<FJsonValue>> NotFoundRows;
    for (const TSharedPtr<FJsonValue>& Value : *PathValues)
    {
        FString Path;
        if (!Value.IsValid() || !Value->TryGetString(Path) || Path.IsEmpty())
        {
            TSharedPtr<FJsonObject> Miss = MakeShared<FJsonObject>();
            Miss->SetStringField(TEXT("path"), FString());
            Miss->SetStringField(TEXT("error"), TEXT("not a non-empty string"));
            NotFoundRows.Add(MakeShared<FJsonValueObject>(Miss));
            continue;
        }
        FString Error;
        const FAssetData Data = ResolveAssetParam(Path, Error);
        if (!Data.IsValid())
        {
            TSharedPtr<FJsonObject> Miss = MakeShared<FJsonObject>();
            Miss->SetStringField(TEXT("path"), Path);
            Miss->SetStringField(TEXT("error"), Error);
            NotFoundRows.Add(MakeShared<FJsonValueObject>(Miss));
            continue;
        }
        ToSync.Add(Data);
        SyncedRows.Add(MakeShared<FJsonValueObject>(AssetDataToJson(Data)));
    }

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetArrayField(TEXT("not_found"), NotFoundRows);
    Reply->SetBoolField(TEXT("focus"), bFocus);
    if (ToSync.Num() == 0)
    {
        Reply->SetArrayField(TEXT("synced"), TArray<TSharedPtr<FJsonValue>>());
        Reply->SetNumberField(TEXT("synced_count"), 0);
        Reply->SetBoolField(TEXT("has_content_browser"), Browser->HasPrimaryContentBrowser());
        return mcp_common::MakeOutcomeReply(Reply, false, TEXT("None of asset_paths names an asset; nothing was synced"));
    }

    Browser->SyncBrowserToAssets(ToSync, bAllowLocked, bFocus);

    // The sync is measured by a browser existing to receive it; the selection itself lands on a
    // later tick (SAssetView defers it), so it is reported as not verifiable here.
    const bool bHasBrowser = Browser->HasPrimaryContentBrowser();
    Reply->SetArrayField(TEXT("synced"), SyncedRows);
    Reply->SetNumberField(TEXT("synced_count"), SyncedRows.Num());
    Reply->SetBoolField(TEXT("has_content_browser"), bHasBrowser);
    return mcp_common::MakeVerifiedReply(Reply, bHasBrowser, mcp_common::EMcpVerification::NotVerifiable,
        TEXT("Content Browser selection"),
        TEXT("the browser applies the selection on its next ticks; read it with get_selected_assets"),
        bHasBrowser ? FString() : FString(TEXT("No Content Browser received the sync")));
}

// MCP:open_asset_editor - Open the editor for an asset (Blueprint, material, data table...) the way a double-click in the Content Browser does, loading the asset if needed, or bring its editor to the front when it is already open. The outcome is read back: success only when an editor for the asset is open after the call. Maps are refused (opening one loads it as the level: use load_level), and so is a headless editor, which cannot show an editor window.
//   @param asset_path - Required: object path ('/Game/A/B.B'), package path ('/Game/A/B') or an unambiguous bare asset name.
//   @param focus - Optional bool (default true): bring the editor to the front when it is already open.
//   @returns { asset_path: string, asset_class: string, opened: bool, already_open: bool, editor_name: string, is_primary_editor: bool }
//   @hints additive idempotent

TSharedPtr<FJsonObject> HandleOpenAssetEditor(const TSharedPtr<FJsonObject>& Params)
{
    FString Path;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_path"), Path)) { return Missing; }
    bool bFocus = true;
    Params->TryGetBoolField(TEXT("focus"), bFocus);

    UAssetEditorSubsystem* Editors = AssetEditorSubsystem();
    if (Editors == nullptr)
    {
        return NoAssetEditorSubsystemReply();
    }
    if (!CanShowEditorUI())
    {
        return mcp_common::MakeErrorReply(TEXT("Cannot open an asset editor: this editor runs headless (-nullrhi or a commandlet)"));
    }

    FString Error;
    const FAssetData Data = ResolveAssetParam(Path, Error);
    if (!Data.IsValid())
    {
        return mcp_common::MakeErrorReply(Error);
    }
    if (Data.AssetClassPath == UWorld::StaticClass()->GetClassPathName())
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("%s is a map: opening it loads it as the editor level; use load_level instead"), *Data.GetObjectPathString()));
    }
    UObject* Asset = Data.GetAsset();
    if (Asset == nullptr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Failed to load %s"), *Data.GetObjectPathString()));
    }

    const bool bAlreadyOpen = Editors->FindEditorForAsset(Asset, bFocus) != nullptr;
    if (!bAlreadyOpen)
    {
        FText CannotOpen;
        if (!Editors->CanOpenEditorForAsset(Asset, EAssetTypeActivationOpenedMethod::Edit, &CannotOpen))
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("%s cannot be opened for editing: %s"),
                *Asset->GetPathName(), *CannotOpen.ToString()));
        }
        Editors->OpenEditorForAsset(Asset);
    }

    IAssetEditorInstance* Editor = Editors->FindEditorForAsset(Asset, /*bFocusIfOpen=*/false);
    const bool bOpen = Editor != nullptr;

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("asset_path"), Asset->GetPathName());
    Reply->SetStringField(TEXT("asset_class"), Asset->GetClass()->GetPathName());
    Reply->SetBoolField(TEXT("opened"), bOpen);
    Reply->SetBoolField(TEXT("already_open"), bAlreadyOpen);
    Reply->SetStringField(TEXT("editor_name"), bOpen ? Editor->GetEditorName().ToString() : FString());
    Reply->SetBoolField(TEXT("is_primary_editor"), bOpen && Editor->IsPrimaryEditor());
    return mcp_common::MakeOutcomeReply(Reply, bOpen,
        bOpen ? FString() : FString::Printf(TEXT("No editor is open for %s after the request"), *Asset->GetPathName()));
}

// MCP:close_asset_editor - Close every editor open on one asset (asset_path), or every asset editor (close_all=true). Unsaved changes are not lost and not saved: the asset stays modified in memory (the dirty flag in the reply), so save_asset it if you mean to keep them. An asset that is not loaded, or has no editor open, is a no-op success with closed_count 0. The outcome is read back: success only when no editor is left open on what was asked.
//   @param asset_path - Object path, package path or unambiguous bare asset name of the asset whose editors to close. Required unless close_all is true.
//   @param close_all - Optional bool (default false): close every open asset editor; asset_path is then ignored.
//   @returns { closed_count: int, still_open_count: int, closed_assets: [{ asset_path: string, asset_class: string, dirty: bool }], close_all: bool }
//   @hints additive idempotent

TSharedPtr<FJsonObject> HandleCloseAssetEditor(const TSharedPtr<FJsonObject>& Params)
{
    bool bCloseAll = false;
    Params->TryGetBoolField(TEXT("close_all"), bCloseAll);
    // Either/or, checked here rather than by a guarded read, so asset_path is not published as
    // required (the server would otherwise refuse a call that sends only close_all).
    FString Path;
    Params->TryGetStringField(TEXT("asset_path"), Path);
    if (!bCloseAll && Path.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: asset_path (string) or close_all (true)"));
    }

    UAssetEditorSubsystem* Editors = AssetEditorSubsystem();
    if (Editors == nullptr)
    {
        return NoAssetEditorSubsystemReply();
    }

    TArray<UObject*> Targets;
    if (bCloseAll)
    {
        Targets = Editors->GetAllEditedAssets();
    }
    else
    {
        FString Error;
        const FAssetData Data = ResolveAssetParam(Path, Error);
        if (!Data.IsValid())
        {
            return mcp_common::MakeErrorReply(Error);
        }
        // An asset that is not in memory has no editor open: never load it just to close nothing.
        if (UObject* Loaded = Data.FastGetAsset(/*bLoad=*/false))
        {
            Targets.Add(Loaded);
        }
    }

    // Editors are counted per asset for one asset, and as the subsystem's open editors for close_all
    // (one editor may edit several assets).
    TArray<UObject*> WithEditors;
    for (UObject* Target : Targets)
    {
        if (Target != nullptr && Editors->FindEditorsForAsset(Target).Num() > 0)
        {
            WithEditors.Add(Target);
        }
    }
    int32 OpenBefore = 0;
    if (bCloseAll)
    {
        OpenBefore = Editors->GetAllOpenEditors().Num();
        Editors->CloseAllAssetEditors();
    }
    else
    {
        for (UObject* Target : WithEditors)
        {
            OpenBefore += Editors->FindEditorsForAsset(Target).Num();
            Editors->CloseAllEditorsForAsset(Target);
        }
    }

    int32 StillOpen = 0;
    TArray<TSharedPtr<FJsonValue>> ClosedRows;
    for (UObject* Target : WithEditors)
    {
        const int32 Remaining = Editors->FindEditorsForAsset(Target).Num();
        if (!bCloseAll)
        {
            StillOpen += Remaining;
        }
        if (Remaining == 0)
        {
            ClosedRows.Add(MakeShared<FJsonValueObject>(LoadedAssetToJson(Target)));
        }
    }
    if (bCloseAll)
    {
        StillOpen = Editors->GetAllOpenEditors().Num();
    }

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetNumberField(TEXT("closed_count"), FMath::Max(0, OpenBefore - StillOpen));
    Reply->SetNumberField(TEXT("still_open_count"), StillOpen);
    Reply->SetArrayField(TEXT("closed_assets"), ClosedRows);
    Reply->SetBoolField(TEXT("close_all"), bCloseAll);
    return mcp_common::MakeOutcomeReply(Reply, StillOpen == 0,
        StillOpen == 0 ? FString() : FString::Printf(TEXT("%d asset editor(s) are still open after the close request"), StillOpen));
}

// MCP:list_open_asset_editors - List the asset editors open right now and the assets each one edits, with whether each asset has unsaved changes. Use it to see what the user is working on, or to find editors to close with close_asset_editor. Read-only; nothing is loaded.
//   @returns { editors: [{ editor_name: string, is_primary_editor: bool, assets: [string] }], editor_count: int, assets: [{ asset_path: string, asset_class: string, dirty: bool, editor_names: [string] }], asset_count: int }

TSharedPtr<FJsonObject> HandleListOpenAssetEditors(const TSharedPtr<FJsonObject>& Params)
{
    UAssetEditorSubsystem* Editors = AssetEditorSubsystem();
    if (Editors == nullptr)
    {
        return NoAssetEditorSubsystemReply();
    }

    TArray<IAssetEditorInstance*> OpenEditors = Editors->GetAllOpenEditors();
    TMap<IAssetEditorInstance*, TArray<FString>> AssetsByEditor;
    TArray<TSharedPtr<FJsonValue>> AssetRows;
    for (UObject* Asset : Editors->GetAllEditedAssets())
    {
        if (Asset == nullptr)
        {
            continue;
        }
        TArray<FString> EditorNames;
        for (IAssetEditorInstance* Editor : Editors->FindEditorsForAsset(Asset))
        {
            if (Editor == nullptr)
            {
                continue;
            }
            EditorNames.Add(Editor->GetEditorName().ToString());
            AssetsByEditor.FindOrAdd(Editor).Add(Asset->GetPathName());
            OpenEditors.AddUnique(Editor);
        }
        TSharedPtr<FJsonObject> Row = LoadedAssetToJson(Asset);
        Row->SetArrayField(TEXT("editor_names"), mcp_common::StringsToJson(EditorNames));
        AssetRows.Add(MakeShared<FJsonValueObject>(Row));
    }

    TArray<TSharedPtr<FJsonValue>> EditorRows;
    for (IAssetEditorInstance* Editor : OpenEditors)
    {
        if (Editor == nullptr)
        {
            continue;
        }
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("editor_name"), Editor->GetEditorName().ToString());
        Row->SetBoolField(TEXT("is_primary_editor"), Editor->IsPrimaryEditor());
        const TArray<FString>* Edited = AssetsByEditor.Find(Editor);
        Row->SetArrayField(TEXT("assets"), mcp_common::StringsToJson(Edited != nullptr ? *Edited : TArray<FString>()));
        EditorRows.Add(MakeShared<FJsonValueObject>(Row));
    }

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetArrayField(TEXT("editors"), EditorRows);
    Reply->SetNumberField(TEXT("editor_count"), EditorRows.Num());
    Reply->SetArrayField(TEXT("assets"), AssetRows);
    Reply->SetNumberField(TEXT("asset_count"), AssetRows.Num());
    return mcp_common::MakeOkReply(Reply);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("close_asset_editor"), &HandleCloseAssetEditor },
    { TEXT("get_content_browser_path"), &HandleGetContentBrowserPath },
    { TEXT("get_selected_assets"), &HandleGetSelectedAssets },
    { TEXT("list_open_asset_editors"), &HandleListOpenAssetEditors },
    { TEXT("open_asset_editor"), &HandleOpenAssetEditor },
    { TEXT("sync_content_browser_to_assets"), &HandleSyncContentBrowserToAssets },
};
} // namespace UEAgentContentBrowserCommands
