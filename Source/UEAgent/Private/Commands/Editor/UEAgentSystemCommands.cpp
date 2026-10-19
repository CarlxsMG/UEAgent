#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
// Font import. The factory produces a UFontFace and, on request, the companion UFont,
// which is what an FSlateFontInfo knows how to use.
#include "Factories/FontFileImportFactory.h"
#include "Engine/FontFace.h"
#include "Engine/Font.h"
#include "UObject/ObjectRedirector.h"
#include "Interfaces/IProjectManager.h"
#include "Interfaces/IPluginManager.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "Modules/ModuleManager.h"
#include "Factories/Factory.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/SavePackage.h"
#include "Engine/World.h"
#include "ObjectTools.h"                      // ObjectTools::ForceDeleteObjects (non-interactive delete)
#include "Editor/LevelEditor/Public/LevelEditorSubsystem.h"  // IsInPlayInEditor (PIE guard for fixup_redirectors / reload_asset_package)
#include "HAL/PlatformFilemanager.h"          // IPlatformFile::FileExists -- disk-verify redirector deletion
#include "FileHelpers.h"                      // FEditorFileUtils::GetDirtyWorldPackages / GetDirtyContentPackages (list_dirty_packages)
#include "PackageTools.h"                     // UPackageTools::ReloadPackages + EReloadPackagesInteractionMode (reload_asset_package)
#include "UObject/UObjectHash.h"              // ForEachObjectWithPackage (resolve the AActor behind an OFPA hash package name)
#include "GameFramework/Actor.h"              // AActor::GetActorLabel (makes __ExternalActors__/<hash> readable)

/** Project/system commands: console, directories, font import, redirectors, plugins, saving assets,
 *  and the in-memory package-state tools (dirty-package audit + non-interactive package reload). */
namespace UEAgentSystemCommands
{

// MCP-DOMAIN: Editor

// MCP:execute_console_command - Run a console command through the EDITOR (GEditor->Exec against the editor world): cvars, `stat` overlays, `r.*` toggles, engine commands. WARNING: it does NOT reach a running PIE session's PlayerController, so player cheats like `BugItGo` come back handled:false -- to move or cheat a PIE pawn use execute_python against the anchored PIE world. `handled` is the engine's own "some Exec claimed this string", not proof of an effect: verify with the state (get_cvar, a property read), never with this bool.
//   @param command - Console command line, e.g. 'stat fps', 'r.ScreenPercentage 50', 'ShowFlag.Grid 0'. Editor-side only.
//   @returns { command: string, handled: bool (an Exec handler claimed the string -- not evidence the effect happened) }
TSharedPtr<FJsonObject> HandleExecuteConsoleCommand(const TSharedPtr<FJsonObject>& Params)
{
    FString Command;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("command"), Command, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (!GEditor)
        return mcp_common::MakeErrorReply(TEXT("GEditor unavailable"));

    UWorld* World = mcp_common::GetEditorWorld();
    const bool bHandled = GEditor->Exec(World, *Command, *GLog);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("command"), Command);
    Result->SetBoolField(TEXT("handled"), bHandled);
    return Result;
}

// MCP:create_directory - Make a content-browser folder at a UE long path (e.g. /Game/MyGame/Systems/Foo). Idempotent: an existing folder is not an error, it comes back already_existed:true. Use it to lay out a system's folders before creating assets there; a path that does not start with '/' is rejected.
//   @param path - UE long path of the folder to create, e.g. /Game/MyGame/Systems/Foo. Must start with '/', not a disk path.
//   @returns { path: string, already_existed: bool (true = nothing was created, and that is a success) }
TSharedPtr<FJsonObject> HandleCreateDirectory(const TSharedPtr<FJsonObject>& Params)
{
    FString Path;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("path"), Path, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (!Path.StartsWith(TEXT("/")))
        return mcp_common::MakeErrorReply(TEXT("path must be a UE long path, e.g. /Game/Foo"));

    const bool bAlready = UEditorAssetLibrary::DoesDirectoryExist(Path);
    if (!bAlready && !UEditorAssetLibrary::MakeDirectory(Path))
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Failed to create directory: %s"), *Path));

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("path"), Path);
    Result->SetBoolField(TEXT("already_existed"), bAlready);
    return Result;
}

// MCP:delete_directory - DESTRUCTIVE: removes a content-browser folder AND every asset inside it, recursively, with no dry-run and no referencer check (unlike delete_assets, which defaults to dry_run and refuses vendored roots). Driven through UEditorAssetSubsystem, which is the pair that works headless. Idempotent: an already-absent folder answers already_absent:true. Verify afterwards with `git status` on the corresponding Content/ path -- never with the `deleted` flag alone.
//   @param path - UE long path of the folder to delete, e.g. /Game/Scratch. Must start with '/'. Everything under it goes.
//   @returns { path: string, already_absent: bool, deleted: bool }
TSharedPtr<FJsonObject> HandleDeleteDirectory(const TSharedPtr<FJsonObject>& Params)
{
    FString Path;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("path"), Path, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (!Path.StartsWith(TEXT("/")))
        return mcp_common::MakeErrorReply(TEXT("path must be a UE long path, e.g. /Game/Foo"));

    // UEditorAssetLibrary's directory helpers are unreliable headless; the
    // UEditorAssetSubsystem drives DoesDirectoryExist / DeleteDirectory reliably
    // in both interactive and commandlet contexts (mirrors the automation handler).
    if (!GEditor)
        return mcp_common::MakeErrorReply(TEXT("GEditor unavailable"));
    UEditorAssetSubsystem* AssetSubsystem = GEditor->GetEditorSubsystem<UEditorAssetSubsystem>();
    if (!AssetSubsystem)
        return mcp_common::MakeErrorReply(TEXT("UEditorAssetSubsystem unavailable"));

    // Idempotent: deleting a directory that is already gone is a success.
    const bool bAlreadyAbsent = !AssetSubsystem->DoesDirectoryExist(Path);
    bool bDeleted = false;
    if (!bAlreadyAbsent && !AssetSubsystem->DeleteDirectory(Path))
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Failed to delete directory: %s"), *Path));
    bDeleted = !bAlreadyAbsent;

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("path"), Path);
    Result->SetBoolField(TEXT("already_absent"), bAlreadyAbsent);
    Result->SetBoolField(TEXT("deleted"), bDeleted);
    return Result;
}

// MCP:fixup_redirectors - Clean up the redirectors a rename/move left behind, under a folder: load each referencing package (which repoints it) and re-save it, optionally force-deleting redirectors whose referencers all saved. Hand-rolled on purpose -- IAssetTools::FixupReferencers HARD-CRASHES on World-Partition/OFPA projects -- and heavily railed: it REFUSES while PIE is active, never writes under readonly_roots (default: the project's Read Only Content Roots), never outside /Game, and never loads or saves map or OFPA packages, handing the resident ones back in skipped_current_world for you to save_level instead. Deletions are DISK-verified (a redirector whose .uasset survives lands in redirectors_delete_failed, not in the count). WARNING: redirectors_found/deleted count OBJECTS: one renamed Blueprint leaves THREE in a single package -- read redirector_packages_found for files.
//   @param path - Long path to scan, defaults to /Game
//   @param delete_redirectors - Optional bool (default false). After a successful repoint, force-delete the redirectors whose every non-redirector referencer was saved. Left in place by default (a redirector is ~2 KB and harmless).
//   @param readonly_roots - Optional array of long-path prefixes that must NEVER be written (default: the project's Read Only Content Roots, i.e. the vendored library roots). Referencers under them are reported in skipped_readonly instead of being saved.
//   @returns { path: string, redirectors_found: int, redirector_packages_found: int, referencers_repointed: int, packages_saved: int, packages_failed: [{ package: string, error: string }], skipped_readonly: [string], skipped_current_world: [string], redirectors_deleted: int, redirector_packages_deleted: int, redirectors_delete_failed: [{ package: string, reason: string }], redirectors_fixed: int, message: string }
//
// redirectors_found/redirectors_fixed count OBJECTS, not packages -- a single
// renamed Blueprint's old package holds THREE UObjectRedirector objects (one for the
// BP, one for its GeneratedClass, one for the SKEL), all inside ONE package.
// redirector_packages_found/redirector_packages_deleted count distinct PACKAGES
// instead, so "how many redirector .uasset files are actually involved" is answerable
// without dividing by an unknown factor.
TSharedPtr<FJsonObject> HandleFixupRedirectors(const TSharedPtr<FJsonObject>& Params)
{
    // -------------------------------------------------------------------------
    // WHY THIS IS HAND-ROLLED
    //
    // IAssetTools::FixupReferencers HARD-CRASHES on World Partition + OFPA projects:
    //   Assertion failed: IsSet() [Misc/Optional.h]
    //   "It is an error to call GetValue() on an unset TOptional"
    // Passing ERedirectFixupMode::LeaveFixedUpRedirectors instead of
    // DeleteFixedUpRedirectors does not dodge it, so the fault is NOT only in
    // the delete branch: it is inside ExecuteFixUp itself
    // (Engine/Source/Developer/AssetTools/Private/AssetFixUpRedirectors.cpp).
    // That routine walks every referencing package, reads
    // FAssetData::GetOptionalOuterPathName() and drives
    // ULevelInstanceSubsystem::ResetLoadersForWorldAsset + the source-control
    // checkout/save flow. On a World Partition + OFPA project (every actor of
    // the map lives in its own __ExternalActors__ package)
    // the referencer set is exactly that world, and the WP/LevelInstance loader
    // reset path is what asserts. It is ENGINE code -- unpatchable from here.
    //
    // So we never call it. Instead we do the only thing FixupReferencers really
    // needs to do for us, deterministically and inline:
    //   1. find redirectors under `path` (AssetRegistry, cheap, no loading),
    //   2. ask the AssetRegistry which PACKAGES reference each redirector,
    //   3. LoadPackage() those referencers -- loading is what repoints a
    //      reference from the redirector to its real destination in memory,
    //   4. MarkPackageDirty + UPackage::SavePackage them, with no dialog and no
    //      interactive source-control step,
    //   5. leave the redirectors alone unless delete_redirectors=true.
    //
    // SAFETY RAILS (this path can crash the editor):
    //   * PIE active            -> refuse outright (saving packages mid-PIE is
    //                              the documented silent-failure/crash zone).
    //   * readonly_roots        -> never saved (default: the project's Read Only
    //                              Content Roots, i.e. vendored upstream).
    //   * anything outside /Game-> never saved (/Engine, /Script, plugin content).
    //   * map packages and OFPA external packages (__ExternalActors__ /
    //     __ExternalObjects__, i.e. FAssetData::GetOptionalOuterPathName() set)
    //     -> NEVER loaded and NEVER saved by us. Two distinct reasons:
    //        (a) LoadPackage()ing an external-actor package whose world is not
    //            the open one is precisely how orphan OFPA actors get
    //            resurrected (a known World Partition gotcha);
    //        (b) for the world that IS open, its packages are already loaded,
    //            so their references are ALREADY repointed in memory -- all
    //            that is missing is a save, and saving a live world belongs to
    //            save_level, not here. We therefore only MarkPackageDirty()
    //            the resident ones and hand them back in
    //            skipped_current_world so the caller runs save_level.
    // -------------------------------------------------------------------------

    FString Path = TEXT("/Game");
    Params->TryGetStringField(TEXT("path"), Path);
    if (!Path.StartsWith(TEXT("/")))
        return mcp_common::MakeErrorReply(TEXT("path must be a UE long path, e.g. /Game/Foo"));
    while (Path.Len() > 1 && Path.EndsWith(TEXT("/")))
    {
        Path.LeftChopInline(1);
    }

    bool bDeleteRedirectors = false;
    Params->TryGetBoolField(TEXT("delete_redirectors"), bDeleteRedirectors);

    TArray<FString> ReadOnlyRoots;
    const TArray<TSharedPtr<FJsonValue>>* ReadOnlyRootsJson = nullptr;
    if (Params->TryGetArrayField(TEXT("readonly_roots"), ReadOnlyRootsJson) && ReadOnlyRootsJson)
    {
        for (const TSharedPtr<FJsonValue>& Value : *ReadOnlyRootsJson)
        {
            FString Root;
            if (Value.IsValid() && Value->TryGetString(Root) && !Root.IsEmpty())
            {
                while (Root.Len() > 1 && Root.EndsWith(TEXT("/"))) { Root.LeftChopInline(1); }
                ReadOnlyRoots.Add(Root);
            }
        }
    }
    else
    {
        ReadOnlyRoots = mcp_common::GetDefaultReadonlyRoots();
    }

    // PIE guard. Refuse rather than stop PIE for the caller: this command can
    // save an unbounded number of packages, and silently killing a live session
    // to do so is a worse surprise than an explicit error.
    if (GEditor)
    {
        ULevelEditorSubsystem* LevelEditorSubsystem = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>();
        if (LevelEditorSubsystem && LevelEditorSubsystem->IsInPlayInEditor())
        {
            return mcp_common::MakeErrorReply(TEXT(
                "fixup_redirectors refuses to run while Play-In-Editor is active: it re-saves referencing "
                "packages, which is unreliable (and has crashed) during PIE. Stop PIE (stop_play_in_editor) "
                "and call again."));
        }
    }

    IAssetRegistry& AssetRegistry = mcp_common::AssetRegistry();
    AssetRegistry.ScanPathsSynchronous({ Path }, /*bForceRescan=*/true);

    // 1. Redirectors under `path` (registry-only: no asset is loaded here).
    FARFilter Filter;
    Filter.PackagePaths.Add(FName(*Path));
    Filter.bRecursivePaths = true;
    Filter.ClassPaths.Add(UObjectRedirector::StaticClass()->GetClassPathName());
    Filter.bRecursiveClasses = true;

    TArray<FAssetData> RedirectorAssets;
    AssetRegistry.GetAssets(Filter, RedirectorAssets);

    const int32 NumRedirectors = RedirectorAssets.Num();

    TSet<FName> RedirectorPackages;
    for (const FAssetData& AssetData : RedirectorAssets)
    {
        RedirectorPackages.Add(AssetData.PackageName);
    }

    // 2. Referencing packages, per redirector (package-level dependencies,
    //    hard + soft -- the default-constructed FDependencyQuery).
    const UE::AssetRegistry::FDependencyQuery DependencyQuery;
    TMap<FName, TArray<FName>> ReferencersByRedirector;
    TSet<FName> AllReferencers;
    for (const FName& RedirectorPackage : RedirectorPackages)
    {
        TArray<FName> Referencers;
        AssetRegistry.GetReferencers(RedirectorPackage, Referencers,
            UE::AssetRegistry::EDependencyCategory::Package, DependencyQuery);
        Referencers.Remove(RedirectorPackage);
        ReferencersByRedirector.Add(RedirectorPackage, Referencers);
        for (const FName& Referencer : Referencers)
        {
            AllReferencers.Add(Referencer);
        }
    }

    // The package name of the world currently open in the editor, used only to
    // label the report (both the open world and any other world-owned package
    // are treated identically: never written from here).
    FString CurrentWorldPackage;
    if (UWorld* EditorWorld = mcp_common::GetEditorWorld())
    {
        CurrentWorldPackage = EditorWorld->GetPackage()->GetName();
    }

    const FTopLevelAssetPath WorldClassPath = UWorld::StaticClass()->GetClassPathName();

    TArray<TSharedPtr<FJsonValue>> SkippedReadOnlyJson;
    TArray<TSharedPtr<FJsonValue>> SkippedWorldJson;
    TArray<TSharedPtr<FJsonValue>> FailedJson;
    auto AddFailure = [&FailedJson](const FString& Package, const FString& Error)
    {
        TSharedPtr<FJsonObject> FailObj = MakeShared<FJsonObject>();
        FailObj->SetStringField(TEXT("package"), Package);
        FailObj->SetStringField(TEXT("error"), Error);
        FailedJson.Add(MakeShared<FJsonValueObject>(FailObj));
    };
    TSet<FName> SavedPackages;
    int32 NumRepointed = 0;

    for (const FName& ReferencerName : AllReferencers)
    {
        if (RedirectorPackages.Contains(ReferencerName))
        {
            // A redirector pointing at another redirector: nothing to re-save.
            continue;
        }

        const FString ReferencerStr = ReferencerName.ToString();

        // -- read-only / non-writable roots -----------------------------------
        if (!ReferencerStr.StartsWith(TEXT("/Game/")))
        {
            SkippedReadOnlyJson.Add(MakeShared<FJsonValueString>(ReferencerStr));
            continue;
        }
        bool bReadOnly = false;
        for (const FString& Root : ReadOnlyRoots)
        {
            if (ReferencerStr == Root || ReferencerStr.StartsWith(Root + TEXT("/")))
            {
                bReadOnly = true;
                break;
            }
        }
        if (bReadOnly)
        {
            SkippedReadOnlyJson.Add(MakeShared<FJsonValueString>(ReferencerStr));
            continue;
        }

        // -- world-owned packages (map or OFPA external actor/object) ---------
        bool bWorldOwned =
            ReferencerStr.Contains(TEXT("/__ExternalActors__/")) ||
            ReferencerStr.Contains(TEXT("/__ExternalObjects__/"));
        if (!bWorldOwned)
        {
            TArray<FAssetData> PackageAssets;
            AssetRegistry.GetAssetsByPackageName(ReferencerName, PackageAssets, /*bIncludeOnlyOnDiskAssets=*/true);
            for (const FAssetData& PackageAsset : PackageAssets)
            {
                // A non-none optional outer == the asset lives in an external
                // package owned by a world; this is the exact test the engine's
                // own ExecuteFixUp uses right before the WP loader-reset that
                // asserts.
                if (!PackageAsset.GetOptionalOuterPathName().IsNone() ||
                    PackageAsset.AssetClassPath == WorldClassPath)
                {
                    bWorldOwned = true;
                    break;
                }
            }
        }
        if (bWorldOwned)
        {
            // Only touch it if it is ALREADY resident: loading it ourselves is
            // the OFPA-orphan-resurrection footgun. Resident packages already
            // hold repointed references (LoadPackage did that when the world was
            // opened); dirtying them is what makes a later save_level persist it.
            if (UPackage* ResidentPackage = FindPackage(nullptr, *ReferencerStr))
            {
                ResidentPackage->MarkPackageDirty();
                ++NumRepointed;
            }
            SkippedWorldJson.Add(MakeShared<FJsonValueString>(ReferencerStr));
            continue;
        }

        // -- ordinary asset package: load (= repoint) then save ---------------
        FString OnDiskFilename;
        if (!FPackageName::DoesPackageExist(ReferencerStr, &OnDiskFilename))
        {
            AddFailure(ReferencerStr, TEXT("Referencing package does not exist on disk"));
            continue;
        }

        UPackage* ReferencerPackage = LoadPackage(nullptr, *ReferencerStr, LOAD_None);
        if (!ReferencerPackage)
        {
            AddFailure(ReferencerStr, TEXT("LoadPackage returned null"));
            continue;
        }
        ReferencerPackage->FullyLoad();
        ++NumRepointed;

        ReferencerPackage->MarkPackageDirty();

        FSavePackageArgs SaveArgs;
        SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
        SaveArgs.SaveFlags = SAVE_NoError;
        SaveArgs.Error = GError;

        UObject* PackageAsset = ReferencerPackage->FindAssetInPackage();
        const bool bSaved = UPackage::SavePackage(ReferencerPackage, PackageAsset, *OnDiskFilename, SaveArgs);
        if (bSaved)
        {
            SavedPackages.Add(ReferencerName);
        }
        else
        {
            AddFailure(ReferencerStr, FString::Printf(TEXT("UPackage::SavePackage returned false for '%s'"), *OnDiskFilename));
        }
    }

    // 3. Optional, opt-in redirector removal, ONE PACKAGE AT A TIME. Only for
    //    packages whose every non-redirector referencer was actually saved
    //    (computed from OUR own bookkeeping rather than a re-query, so it does
    //    not depend on how fast the AssetRegistry catches up with the saves we
    //    just made). Deletion goes through ObjectTools::ForceDeleteObjects with
    //    bShowConfirmation=false -- the same non-interactive path delete_asset
    //    (HandleDeleteAsset, UEAgentManagementCommands.cpp) uses --
    //    NOT through AssetTools' DeleteFixedUpRedirectors flow, which can crash.
    //
    // ObjectTools::ForceDeleteObjects frees the in-memory UObjects and drops the
    // AssetRegistry entry, but that is NOT proof the .uasset file left disk: its
    // return value is an object count, and a redirector file can keep its
    // size/mtime and still load. So, like delete_asset, the result is verified
    // against disk AFTER the call (IPlatformFile::FileExists on the resolved
    // filename), grouped per PACKAGE since redirectors_found counts objects, not
    // packages (see the @returns comment above).
    int32 NumObjectsVerifiedDeleted = 0;   // -> redirectors_deleted (object-granularity, disk-verified)
    int32 NumPackagesDeleted = 0;          // -> redirector_packages_deleted
    TArray<TSharedPtr<FJsonValue>> DeleteFailedJson;
    if (bDeleteRedirectors && NumRedirectors > 0)
    {
        TMap<FName, TArray<FAssetData>> RedirectorsByPackage;
        for (const FAssetData& AssetData : RedirectorAssets)
        {
            RedirectorsByPackage.FindOrAdd(AssetData.PackageName).Add(AssetData);
        }

        for (const TPair<FName, TArray<FAssetData>>& PackagePair : RedirectorsByPackage)
        {
            const FString RedirectorStr = PackagePair.Key.ToString();

            bool bProtected = !RedirectorStr.StartsWith(TEXT("/Game/"));
            for (const FString& Root : ReadOnlyRoots)
            {
                if (RedirectorStr == Root || RedirectorStr.StartsWith(Root + TEXT("/"))) { bProtected = true; break; }
            }
            if (bProtected)
            {
                continue;
            }

            bool bAllReferencersSaved = true;
            if (const TArray<FName>* Referencers = ReferencersByRedirector.Find(PackagePair.Key))
            {
                for (const FName& Referencer : *Referencers)
                {
                    if (RedirectorPackages.Contains(Referencer)) { continue; }
                    if (!SavedPackages.Contains(Referencer)) { bAllReferencersSaved = false; break; }
                }
            }
            if (!bAllReferencersSaved)
            {
                continue;
            }

            TArray<UObject*> ObjectsToDelete;
            for (const FAssetData& AssetData : PackagePair.Value)
            {
                if (UObjectRedirector* Redirector = Cast<UObjectRedirector>(AssetData.GetAsset()))
                {
                    ObjectsToDelete.Add(Redirector);
                }
            }
            if (ObjectsToDelete.Num() == 0)
            {
                continue;
            }

            ObjectTools::ForceDeleteObjects(ObjectsToDelete, /*bShowConfirmation=*/false);

            // Verify against disk -- never trust the object count above as proof
            // the file is gone. DoesPackageExist resolves the correct on-disk
            // filename (.uasset here; redirectors from a renamed map would be
            // .umap, but those packages are already excluded upstream by the
            // world-owned check) and we additionally confirm with IPlatformFile
            // directly, the same primitive save_asset uses.
            FString OnDiskFilename;
            const bool bPackageStillRegistered = FPackageName::DoesPackageExist(RedirectorStr, &OnDiskFilename);
            IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
            const bool bFileStillOnDisk = bPackageStillRegistered && PlatformFile.FileExists(*OnDiskFilename);

            if (!bFileStillOnDisk)
            {
                ++NumPackagesDeleted;
                NumObjectsVerifiedDeleted += ObjectsToDelete.Num();
            }
            else
            {
                TSharedPtr<FJsonObject> FailObj = MakeShared<FJsonObject>();
                FailObj->SetStringField(TEXT("package"), RedirectorStr);
                FailObj->SetStringField(TEXT("reason"), TEXT(
                    "ObjectTools::ForceDeleteObjects removed the in-memory object(s) and the "
                    "AssetRegistry entry, but the .uasset is still on disk (verified via IPlatformFile)."));
                DeleteFailedJson.Add(MakeShared<FJsonValueObject>(FailObj));
            }
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("path"), Path);
    Result->SetNumberField(TEXT("redirectors_found"), NumRedirectors);
    Result->SetNumberField(TEXT("redirector_packages_found"), RedirectorPackages.Num());
    Result->SetNumberField(TEXT("referencers_repointed"), NumRepointed);
    Result->SetNumberField(TEXT("packages_saved"), SavedPackages.Num());
    Result->SetArrayField(TEXT("packages_failed"), FailedJson);
    Result->SetArrayField(TEXT("skipped_readonly"), SkippedReadOnlyJson);
    Result->SetArrayField(TEXT("skipped_current_world"), SkippedWorldJson);
    // Disk-verified (see the comment on the deletion loop above), not
    // ObjectTools::ForceDeleteObjects' return value, which counts in-memory object
    // deletion, not a file actually leaving disk.
    Result->SetNumberField(TEXT("redirectors_deleted"), NumObjectsVerifiedDeleted);
    Result->SetNumberField(TEXT("redirector_packages_deleted"), NumPackagesDeleted);
    Result->SetArrayField(TEXT("redirectors_delete_failed"), DeleteFailedJson);
    // Kept for compatibility.
    Result->SetNumberField(TEXT("redirectors_fixed"), NumRedirectors);

    FString Message = FString::Printf(
        TEXT("Found %d redirector(s) under %s; repointed %d referencing package(s), saved %d."),
        NumRedirectors, *Path, NumRepointed, SavedPackages.Num());
    if (SkippedWorldJson.Num() > 0)
    {
        Message += FString::Printf(
            TEXT(" %d world-owned package(s) (map / OFPA external actors) were marked dirty but NOT saved -- "
                 "use save_level to persist them."),
            SkippedWorldJson.Num());
        if (!CurrentWorldPackage.IsEmpty())
        {
            Message += FString::Printf(TEXT(" Current editor world: %s."), *CurrentWorldPackage);
        }
    }
    if (SkippedReadOnlyJson.Num() > 0)
    {
        Message += FString::Printf(TEXT(" %d referencer(s) under read-only roots were skipped."), SkippedReadOnlyJson.Num());
    }
    if (!bDeleteRedirectors && NumRedirectors > 0)
    {
        Message += TEXT(" Redirectors left in place (pass delete_redirectors=true to remove the fully-repointed ones).");
    }
    if (bDeleteRedirectors)
    {
        Message += FString::Printf(
            TEXT(" Deleted %d redirector package(s) from disk (verified)."), NumPackagesDeleted);
        if (DeleteFailedJson.Num() > 0)
        {
            Message += FString::Printf(
                TEXT(" %d redirector package(s) still have a .uasset on disk after ForceDeleteObjects -- see redirectors_delete_failed."),
                DeleteFailedJson.Num());
        }
    }
    Result->SetStringField(TEXT("message"), Message);
    Result->SetBoolField(TEXT("success"), true);
    return Result;
}

// MCP:set_plugin_enabled - Turn a plugin on (or off, with enabled=false) in the .uproject and save the project file to disk. WARNING: `enabled` echoes the requested state once the .uproject write succeeded, and the plugin's MODULES are not loaded or unloaded by this call -- the editor has to be restarted before an enabled plugin's classes, assets or MCP tools actually exist, and a disabled plugin's classes keep working until then. So do not treat a following "class not found", or a still-working feature, as a failure of this command; check project_saved and then restart. A plugin the .uproject already has in that state is a no-op: project_saved=false and nothing saved; a .uproject that cannot be saved makes success false. A name this editor has not discovered as a plugin is an error, and nothing is written to the .uproject. WARNING: disabling a plugin whose assets are referenced leaves those references broken on the next start; check get_plugin_status and the referencers first. Use get_plugin_status to read the current state.
//   @param plugin_name - Plugin name exactly as in its .uplugin file (e.g. 'PythonScriptPlugin'), not a path.
//   @param enabled - Optional bool (default true): true enables the plugin, false disables it.
//   @returns { plugin_name: string (the plugin's own spelling), enabled: bool (the requested state, set when the .uproject write succeeded), project_saved: bool, note: string about the required restart, or that nothing changed }
TSharedPtr<FJsonObject> HandleSetPluginEnabled(const TSharedPtr<FJsonObject>& Params)
{
    FString PluginName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("plugin_name"), PluginName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    bool bEnable = true;
    Params->TryGetBoolField(TEXT("enabled"), bEnable);

    // SetPluginEnabled writes any name into the .uproject without checking it, and a reference to a plugin that
    // does not exist can stop the project from opening: an unknown name is refused before anything is written.
    // The plugin's own name is used from here on, so the .uproject gets its exact spelling.
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
    if (!Plugin.IsValid())
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("set_plugin_enabled: no plugin named '%s' is known to this editor (IPluginManager::FindPlugin found none); nothing was ")
            TEXT("written to the .uproject. get_plugin_status reads a plugin by its .uplugin name."), *PluginName));
    }
    PluginName = Plugin->GetName();

    FText FailReason;
    if (!IProjectManager::Get().SetPluginEnabled(PluginName, bEnable, FailReason))
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Failed to %s plugin '%s': %s"),
            bEnable ? TEXT("enable") : TEXT("disable"), *PluginName, *FailReason.ToString()));

    // SetPluginEnabled dirties the project only when the .uproject changes: a project that is not dirty
    // already has the plugin in that state, and there is nothing to save.
    const bool bNeedsSave = IProjectManager::Get().IsCurrentProjectDirty();
    bool bSaved = false;
    if (bNeedsSave)
    {
        bSaved = IProjectManager::Get().SaveCurrentProjectToDisk(FailReason);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("plugin_name"), PluginName);
    Result->SetBoolField(TEXT("enabled"), bEnable);
    Result->SetBoolField(TEXT("project_saved"), bSaved);
    Result->SetStringField(TEXT("note"), bNeedsSave
        ? (bEnable ? TEXT("An editor restart is usually required to load a newly-enabled plugin's modules.")
                   : TEXT("An editor restart is usually required to fully unload a newly-disabled plugin's modules."))
        : (bEnable ? TEXT("The .uproject already has the plugin enabled; nothing changed, nothing saved.")
                   : TEXT("The .uproject already has the plugin disabled; nothing changed, nothing saved.")));
    if (bNeedsSave && !bSaved)
    {
        Result->SetBoolField(TEXT("success"), false);
        Result->SetStringField(TEXT("error"), FString::Printf(
            TEXT("set_plugin_enabled: the .uproject was changed in memory but not saved to disk: %s"), *FailReason.ToString()));
    }
    return Result;
}

// MCP:save_asset - Write ONE .uasset to disk by package path (a Blueprint, DataTable, material...) and VERIFY the effect: still_dirty_after_save:false plus file_on_disk:true is the oracle, not `saved`. Use save_level for the .umap and reload_asset_package to undo an in-memory edit instead. PIE: this is REFUSED while Play-in-Editor runs (it never stops or touches your session) -- the engine guard Editor.AllowSavingAssetsDuringPIE=0 rejects the write before attempting anything, so a save during play would report success without writing a byte. Stop PIE and retry, or pass force=true. WARNING: when it answers success:false with PIE down, a common cause is the file being READ-ONLY on disk (e.g. a Git LFS `lockable` file that has not been `git lfs lock`ed). You do not have to check that by hand: `file_read_only` is measured on every call and the error message states whether a read-only file IS or is NOT the explanation. WARNING: it writes exactly one package, so it cannot stop a compile cascade from dirtying a vendored asset that a LATER bulk save then rewrites -- list_dirty_packages before, `git status` after.
//   @param path - Full asset path to save, e.g. /Game/MyGame/Core/BP_MyActor (an .Object suffix is tolerated). Must already exist or the call errors.
//   @param readonly_roots - Optional array of long-path prefixes treated as vendored/upstream (default: the project's Read Only Content Roots). A save under one of them is REFUSED by default; see refuse_readonly. Pass [] to disable the classification entirely.
//   @param refuse_readonly - Optional bool. When true, a save whose package falls under one of readonly_roots is REFUSED (success:false, saved:false, refused_readonly:true) instead of performed. Default = the process-wide switch (mcp_common::IsReadOnlyRootSaveEnforcementEnabled(), true unless `mcp.ReadOnlyRootEnforcement 0`); when false the write happens and is only reported + ledgered.
//   @returns { path: string, package_name: string, saved: bool, success: bool, under_readonly_root: bool, readonly_root_matched: string, refused_readonly: bool, readonly_roots: [string], readonly_roots_defaulted: bool, still_dirty_after_save: bool, file_on_disk: bool, file_read_only: bool (measured on disk -- true means the file is read-only, e.g. an unlocked Git LFS `lockable` file), verification: object, verification_checks: [object], error?: string }
//
// HONESTY NOTE, do not oversell this guard. save_asset writes exactly ONE
// package (UEditorAssetSubsystem::SaveAsset -> EditorAssetSubsystem.cpp),
// so refuse_readonly here does NOT prevent the typical incident: a vendored asset
// that compiling a project Blueprint dirties through the compile cascade, and that a
// LATER bulk save writes -- nobody ever calls save_asset on it. What actually catches
// that class of damage is list_dirty_packages (see below) run BEFORE the save, plus
// `git status` on the vendored folder after. This guard only stops a direct, explicit
// save_asset("/Game/VendorPack/...").
TSharedPtr<FJsonObject> HandleSaveAsset(const TSharedPtr<FJsonObject>& Params)
{
    FString Path;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("path"), Path, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (!UEditorAssetLibrary::DoesAssetExist(Path))
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Asset does not exist: %s"), *Path));

    // This command does NOT go through mcp_common::SaveAssetPackage, so the
    // shared guard living there never sees it -- the classification has to be done
    // here. Parsing is the shared mcp_common::ParseReadonlyRoots (same default
    // roots, same trailing-slash trim, same "[] means disabled" opt-out
    // as fixup_redirectors). The literal TryGetArrayField below only records
    // whether the caller supplied the field, so the response can say whether the
    // default was in play.
    const TArray<TSharedPtr<FJsonValue>>* ReadOnlyRootsJson = nullptr;
    const bool bReadOnlyRootsProvided = Params->TryGetArrayField(TEXT("readonly_roots"), ReadOnlyRootsJson);
    const TArray<FString> ReadOnlyRoots = mcp_common::ParseReadonlyRoots(Params);

    bool bRefuseReadOnly = mcp_common::IsReadOnlyRootSaveEnforcementEnabled();
    Params->TryGetBoolField(TEXT("refuse_readonly"), bRefuseReadOnly);

    // ObjectPathToPackageName is a no-op when `path` has no ".Object" suffix.
    const FString PackageNameStr = FPackageName::ObjectPathToPackageName(Path);
    const bool bUnderReadOnlyRoot = mcp_common::IsUnderReadOnlyRoot(PackageNameStr, ReadOnlyRoots);
    FString MatchedRoot;
    if (bUnderReadOnlyRoot)
    {
        for (const FString& Root : ReadOnlyRoots)
        {
            if (PackageNameStr == Root || PackageNameStr.StartsWith(Root + TEXT("/"))) { MatchedRoot = Root; break; }
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("path"), Path);
    Result->SetStringField(TEXT("package_name"), PackageNameStr);
    Result->SetBoolField(TEXT("under_readonly_root"), bUnderReadOnlyRoot);
    Result->SetStringField(TEXT("readonly_root_matched"), MatchedRoot);
    Result->SetBoolField(TEXT("readonly_roots_defaulted"), !bReadOnlyRootsProvided);
    Result->SetArrayField(TEXT("readonly_roots"), mcp_common::StringsToJson(ReadOnlyRoots));

    if (bUnderReadOnlyRoot && bRefuseReadOnly)
    {
        // Refused: nothing was written, and we say so with the same field names a
        // successful call uses so callers never have to guess.
        Result->SetBoolField(TEXT("saved"), false);
        Result->SetBoolField(TEXT("refused_readonly"), true);
        Result->SetBoolField(TEXT("still_dirty_after_save"), false);
        Result->SetBoolField(TEXT("file_on_disk"), FPackageName::DoesPackageExist(PackageNameStr));
        Result->SetBoolField(TEXT("success"), false);
        Result->SetStringField(TEXT("error"), FString::Printf(
            TEXT("Refused: '%s' lives under the read-only root '%s'. Nothing was written. ")
            TEXT("Pass refuse_readonly=false (or readonly_roots=[]) to override."),
            *PackageNameStr, *MatchedRoot));
        mcp_common::AddVerificationEvidence(Result, mcp_common::EMcpVerification::NotAttempted,
            TEXT("readonly_root_guard"),
            TEXT("Save refused before it was attempted; there is no effect to verify."));
        return Result;
    }
    Result->SetBoolField(TEXT("refused_readonly"), false);

    // PIE: REFUSE. UEditorAssetSubsystem::SaveAsset returns false SILENTLY with PIE
    // running (Editor.AllowSavingAssetsDuringPIE=0), without any error text reaching here.
    // So the call is refused and says so (see mcp_common::IsPieActive); the running PIE
    // session is left alone. The dispatcher normally refuses it before getting here
    // (CheckPIESafetyGuard, with save_asset in McpPieSafety.gen.h); this covers another path
    // in, or force=true.
    if (mcp_common::IsPieActive())
    {
        Result->SetBoolField(TEXT("saved"), false);
        Result->SetBoolField(TEXT("success"), false);
        Result->SetBoolField(TEXT("pie_active"), true);
        Result->SetStringField(TEXT("error"), FString::Printf(
            TEXT("REFUSED: a Play-in-Editor session is running. The engine guard ")
            TEXT("Editor.AllowSavingAssetsDuringPIE=0 refuses the write before attempting anything, ")
            TEXT("so saving '%s' now would report success without writing a byte. ")
            TEXT("Stop PIE (stop_play_in_editor) and save again."),
            *Path));
        mcp_common::AddVerificationEvidence(Result, mcp_common::EMcpVerification::NotAttempted,
            TEXT("pie_guard"),
            TEXT("Save refused before it was attempted; there is no effect to verify."));
        return Result;
    }

    const bool bSaved = UEditorAssetLibrary::SaveAsset(Path, /*bOnlyIfIsDirty=*/false);

    if (bUnderReadOnlyRoot)
    {
        // Report-only mode (refuse_readonly=false): the write happened, and the log says so loudly.
        UE_LOG(LogTemp, Warning, TEXT("[UEAgent] save_asset WROTE '%s', which is under the read-only root '%s' (report-only mode; pass refuse_readonly=true to make this a refusal)."),
            *PackageNameStr, *MatchedRoot);
    }

    // Verify the effect instead of trusting the bool. After a real save the
    // package is clean AND the .uasset resolves on disk. Both are
    // re-read here; a package that is not resident (never loaded) cannot report a
    // dirty flag, which is stated rather than assumed away.
    UPackage* SavedPackage = FindPackage(nullptr, *PackageNameStr);
    const bool bStillDirty = (SavedPackage != nullptr) && SavedPackage->IsDirty();
    FString OnDiskFilename;
    const bool bPackageResolves = FPackageName::DoesPackageExist(PackageNameStr, &OnDiskFilename);
    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    const bool bFileOnDisk = bPackageResolves && PlatformFile.FileExists(*OnDiskFilename);
    // A common cause of SaveAsset returning false -- a read-only file -- is checked here
    // instead of left for the reader to check by hand, and it is reported even when the
    // save DID succeed.
    const bool bFileReadOnly = bFileOnDisk && PlatformFile.IsReadOnly(*OnDiskFilename);

    Result->SetBoolField(TEXT("saved"), bSaved);
    Result->SetBoolField(TEXT("still_dirty_after_save"), bStillDirty);
    Result->SetBoolField(TEXT("file_on_disk"), bFileOnDisk);
    Result->SetBoolField(TEXT("file_read_only"), bFileReadOnly);
    Result->SetBoolField(TEXT("success"), bSaved);

    if (!bSaved)
    {
        // It cannot be PIE here (the guard above returns earlier), so the message points at
        // what IS left, in the order it usually happens -- first a read-only file (Git LFS
        // leaves `lockable` files read-only until they are locked), which bFileReadOnly above
        // confirms or rules out.
        Result->SetStringField(TEXT("error"), FString::Printf(
            TEXT("UEditorAssetLibrary::SaveAsset returned false for '%s'. PIE was NOT active (checked above). %s"),
            *Path,
            bFileReadOnly
                ? TEXT("MEASURED CAUSE: the file is READ-ONLY on disk. In a Git LFS repo that is a `lockable` "
                       "path you have not locked (LFS restores the attribute after every commit): "
                       "git lfs lock <file>. Do not retry the save until the file is writable.")
                : TEXT("The file is NOT read-only (measured), so a source-control lock is NOT the explanation "
                       "here: look at a checkout/permission issue, a path that resolves to a package "
                       "that is not resident, or a disk error.")));
        mcp_common::AddVerificationEvidence(Result, mcp_common::EMcpVerification::Failed,
            TEXT("reread_package_dirty_flag+disk"),
            FString::Printf(TEXT("SaveAsset returned false; file_on_disk=%s, still_dirty=%s."),
                bFileOnDisk ? TEXT("true") : TEXT("false"), bStillDirty ? TEXT("true") : TEXT("false")));
    }
    else if (SavedPackage == nullptr)
    {
        mcp_common::AddVerificationEvidence(Result, mcp_common::EMcpVerification::NotVerifiable,
            TEXT("reread_package_dirty_flag+disk"),
            FString::Printf(TEXT("Package '%s' is not resident in memory after the save, so its dirty flag ")
                            TEXT("cannot be re-read; file_on_disk=%s."),
                *PackageNameStr, bFileOnDisk ? TEXT("true") : TEXT("false")));
    }
    else
    {
        const bool bVerified = bFileOnDisk && !bStillDirty;
        mcp_common::AddVerificationEvidence(Result,
            bVerified ? mcp_common::EMcpVerification::Verified : mcp_common::EMcpVerification::Failed,
            TEXT("reread_package_dirty_flag+disk"),
            FString::Printf(TEXT("file_on_disk=%s, still_dirty_after_save=%s."),
                bFileOnDisk ? TEXT("true") : TEXT("false"), bStillDirty ? TEXT("true") : TEXT("false")));
    }
    return Result;
}

// ---------------------------------------------------------------------------
// Local helpers for list_dirty_packages / reload_asset_package.
// ---------------------------------------------------------------------------
namespace
{
    /** OFPA external packages are recognised by path, exactly as fixup_redirectors does. */
    bool IsExternalPackagePath_System(const FString& PackageName)
    {
        return PackageName.Contains(TEXT("/__ExternalActors__/")) ||
               PackageName.Contains(TEXT("/__ExternalObjects__/"));
    }

    /** "ofpa_actor" | "ofpa_object" | "map" | "asset". */
    FString ClassifyPackageKind_System(const UPackage* Package, const FString& PackageName)
    {
        if (PackageName.Contains(TEXT("/__ExternalActors__/")))  { return TEXT("ofpa_actor"); }
        if (PackageName.Contains(TEXT("/__ExternalObjects__/"))) { return TEXT("ofpa_object"); }
        if (Package && Package->ContainsMap())                   { return TEXT("map"); }
        return TEXT("asset");
    }

    /**
     * Resolve the on-disk filename of a package. Prefers the authoritative
     * DoesPackageExist lookup (it knows the real extension); falls back to a
     * PREDICTED path for packages that have never been written, and says which
     * of the two it returned through OutExistsOnDisk.
     */
    FString ResolvePackageFilename_System(const UPackage* Package, const FString& PackageName, bool& OutExistsOnDisk)
    {
        FString Filename;
        OutExistsOnDisk = FPackageName::DoesPackageExist(PackageName, &Filename);
        if (OutExistsOnDisk)
        {
            return Filename;
        }
        FString Predicted;
        const FString Extension = (Package && Package->ContainsMap())
            ? FPackageName::GetMapPackageExtension()
            : FPackageName::GetAssetPackageExtension();
        if (FPackageName::TryConvertLongPackageNameToFilename(PackageName, Predicted, Extension))
        {
            return Predicted;
        }
        return FString();
    }

    /** First AActor carried by the package, if any -- this is what turns an OFPA hash name into something a human can read. */
    AActor* FindActorInPackage_System(UPackage* Package)
    {
        AActor* Found = nullptr;
        if (!Package)
        {
            return nullptr;
        }
        ForEachObjectWithPackage(Package, [&Found](UObject* Obj)
        {
            if (AActor* Actor = Cast<AActor>(Obj))
            {
                Found = Actor;
                return false; // stop
            }
            return true;
        });
        return Found;
    }

    /** Shared PIE probe (same ULevelEditorSubsystem path fixup_redirectors uses). */
    bool IsPlayInEditorActive_System()
    {
        if (!GEditor)
        {
            return false;
        }
        ULevelEditorSubsystem* LevelEditorSubsystem = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>();
        return LevelEditorSubsystem && LevelEditorSubsystem->IsInPlayInEditor();
    }

    /** Package name of the world currently open in the editor ("" if none). */
    FString GetCurrentEditorWorldPackageName_System()
    {
        if (UWorld* EditorWorld = mcp_common::GetEditorWorld())
        {
            if (UPackage* WorldPackage = EditorWorld->GetPackage())
            {
                return WorldPackage->GetName();
            }
        }
        return FString();
    }
}

// MCP:list_dirty_packages - What is UNSAVED in the editor right now: every dirty package with its disk path, split into map / OFPA-external / asset, and -- the reason to run it -- readonly_dirty_count, the single go/no-go number telling you a vendored root like /Game/VendorPack would be REWRITTEN by the next bulk save. Run it before closing the editor, before compiling, and before any save; it also resolves OFPA hashes to actor labels so /__ExternalActors__/AB/CD/EF12 becomes a name. Strictly read-only: never marks dirty, never saves. WARNING: it reports IN-MEMORY state only -- a package clean here can still differ from HEAD on disk, so `git status` stays the disk authority. And do NOT use its pie_active field as the PIE gate: it can answer false with PIE running; the oracle is is_play_in_editor_active.
//   @param readonly_roots - Optional array of long-path prefixes treated as vendored/upstream (default: the project's Read Only Content Roots). Only used to CLASSIFY (under_readonly_root / readonly_dirty_count); nothing is ever written. Pass [] to switch the classification off.
//   @param path_prefix - Optional long-path prefix filter, e.g. /Game/VendorPack, to ask "is anything of THEIRS dirty". Empty (default) lists everything; what it excluded is counted in filtered_out.
//   @param include_objects - Optional bool (default false). Also list every object carried by each dirty package. One object-hash walk per package -- a dirty map can hold thousands, so leave it off unless you need the contents.
//   @returns { dirty_count: int, readonly_dirty_count: int, map_dirty_count: int, external_dirty_count: int, asset_dirty_count: int, filtered_out: int, pie_active: bool, current_world_package: string, path_prefix: string, readonly_roots: [string], readonly_roots_defaulted: bool, packages: [{ package_name: string, file_path: string, exists_on_disk: bool, kind: string, contains_map: bool, newly_created: bool, actor_label: string, actor_name: string, asset_name: string, asset_class: string, under_readonly_root: bool, objects?: [string] }], message: string, note: string }
//
// WHY THIS EXISTS AND WHAT IT IS NOT.
// Compiling a project Blueprint can dirty -- and a later bulk save then REWRITE --
// assets under a vendored root. Without this tool the only oracle for that is running
// `git status` on the vendored folder by hand after every batch, i.e. AFTER the damage
// is already on disk. This tool answers the same question BEFORE the save, from the
// editor's own in-memory state.
//
// HONESTY -- this is not "a capability that was impossible". Python already
// reaches UEditorLoadingAndSavingUtils.get_dirty_content_packages() through
// execute_python. What this command adds is (a) the readonly_roots
// classification with readonly_dirty_count as a single go/no-go number, and
// (b) actor_label/actor_name resolved for OFPA packages, which turns
// `/Game/.../__ExternalActors__/AB/CD/EF123456` into a name a human recognises.
// actor_label can legitimately come back EMPTY (see the bCreateIfNone note at the
// call site) -- actor_name is the always-present fallback, never a fabrication.
//
// NOT TObjectIterator<UPackage>. A raw iteration MISSES the world's loaded
// external (OFPA) packages, which is exactly the class that matters most here.
// FEditorFileUtils::GetDirtyWorldPackages (FileHelpers.cpp ->
// EnumerateDirtyWorldPackages) walks PersistentLevel->
// GetLoadedExternalObjectPackages() and picks them up.
//
// ONE CAVEAT, stated rather than hidden. This command itself never calls
// MarkPackageDirty and never saves. The ENGINE enumerator it is required to use
// does, in two narrow cases inside EnumerateDirtyWorldPackages: an unnamed
// MapBuildData package of an already-dirty world (FileHelpers.cpp ~5690) and a
// PKG_NewlyCreated world package that has no name yet. Both only apply to a world
// that has never been saved; neither can touch an on-disk vendored asset.
TSharedPtr<FJsonObject> HandleListDirtyPackages(const TSharedPtr<FJsonObject>& Params)
{
    const TArray<TSharedPtr<FJsonValue>>* ReadOnlyRootsJson = nullptr;
    const bool bReadOnlyRootsProvided = Params->TryGetArrayField(TEXT("readonly_roots"), ReadOnlyRootsJson);
    const TArray<FString> ReadOnlyRoots = mcp_common::ParseReadonlyRoots(Params);

    FString PathPrefix;
    Params->TryGetStringField(TEXT("path_prefix"), PathPrefix);
    while (PathPrefix.Len() > 1 && PathPrefix.EndsWith(TEXT("/")))
    {
        PathPrefix.LeftChopInline(1);
    }

    bool bIncludeObjects = false;
    Params->TryGetBoolField(TEXT("include_objects"), bIncludeObjects);

    // Union of world-owned (map + MapBuildData + OFPA externals) and content
    // packages, de-duplicated but order-preserving.
    TArray<UPackage*> DirtyPackages;
    FEditorFileUtils::GetDirtyWorldPackages(DirtyPackages);
    FEditorFileUtils::GetDirtyContentPackages(DirtyPackages);

    TSet<UPackage*> Seen;
    TArray<TSharedPtr<FJsonValue>> PackagesJson;
    int32 NumReadOnlyDirty = 0;
    int32 NumMapDirty = 0;
    int32 NumExternalDirty = 0;
    int32 NumAssetDirty = 0;
    int32 NumFilteredOut = 0;
    int32 NumListed = 0;

    for (UPackage* Package : DirtyPackages)
    {
        if (!Package || Seen.Contains(Package))
        {
            continue;
        }
        Seen.Add(Package);

        const FString PackageName = Package->GetName();
        if (!PathPrefix.IsEmpty() &&
            !(PackageName == PathPrefix || PackageName.StartsWith(PathPrefix + TEXT("/"))))
        {
            ++NumFilteredOut;
            continue;
        }

        const FString Kind = ClassifyPackageKind_System(Package, PackageName);
        const bool bUnderReadOnly = mcp_common::IsUnderReadOnlyRoot(PackageName, ReadOnlyRoots);

        bool bExistsOnDisk = false;
        const FString FilePath = ResolvePackageFilename_System(Package, PackageName, bExistsOnDisk);

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("package_name"), PackageName);
        Entry->SetStringField(TEXT("file_path"), FilePath);
        Entry->SetBoolField(TEXT("exists_on_disk"), bExistsOnDisk);
        Entry->SetStringField(TEXT("kind"), Kind);
        Entry->SetBoolField(TEXT("contains_map"), Package->ContainsMap());
        Entry->SetBoolField(TEXT("newly_created"), Package->HasAnyPackageFlags(PKG_NewlyCreated));
        Entry->SetBoolField(TEXT("under_readonly_root"), bUnderReadOnly);

        // actor_label: only walked for OFPA packages -- that is where the package
        // name is an unreadable hash, and it is the whole point of the field. A
        // dirty map package would cost a walk over every actor in the level for a
        // label the package name already gives us.
        FString ActorLabel;
        FString ActorName;
        if (Kind == TEXT("ofpa_actor") || Kind == TEXT("ofpa_object"))
        {
            if (AActor* Actor = FindActorInPackage_System(Package))
            {
                // bCreateIfNone=FALSE, deliberately. The default (true) is NOT
                // a read: AActor::GetActorLabel (ActorEditor.cpp) does
                // `const_cast<AActor*>(this)->ActorLabel = GetDefaultActorLabel()`
                // and broadcasts OnActorLabelChanged when the label is empty. That
                // writes a serialized editor-only property from a handler whose whole
                // contract is "never mutates" -- and it would do it while auditing
                // dirty packages, of all things. With false we take the label only if
                // one is already cached and fall back to the object name, which costs
                // nothing and mutates nothing.
                ActorLabel = Actor->GetActorLabel(/*bCreateIfNone=*/false);
                ActorName = Actor->GetName();
            }
        }
        Entry->SetStringField(TEXT("actor_label"), ActorLabel);
        Entry->SetStringField(TEXT("actor_name"), ActorName);

        FString AssetName;
        FString AssetClass;
        if (Kind == TEXT("asset"))
        {
            if (UObject* Asset = Package->FindAssetInPackage())
            {
                AssetName = Asset->GetName();
                AssetClass = Asset->GetClass() ? Asset->GetClass()->GetName() : FString();
            }
        }
        Entry->SetStringField(TEXT("asset_name"), AssetName);
        Entry->SetStringField(TEXT("asset_class"), AssetClass);

        if (bIncludeObjects)
        {
            TArray<TSharedPtr<FJsonValue>> ObjectsJson;
            ForEachObjectWithPackage(Package, [&ObjectsJson](UObject* Obj)
            {
                if (Obj)
                {
                    ObjectsJson.Add(MakeShared<FJsonValueString>(Obj->GetPathName()));
                }
                return true;
            });
            Entry->SetArrayField(TEXT("objects"), ObjectsJson);
        }

        PackagesJson.Add(MakeShared<FJsonValueObject>(Entry));
        ++NumListed;
        if (bUnderReadOnly) { ++NumReadOnlyDirty; }
        if (Kind == TEXT("map")) { ++NumMapDirty; }
        else if (Kind == TEXT("ofpa_actor") || Kind == TEXT("ofpa_object")) { ++NumExternalDirty; }
        else { ++NumAssetDirty; }
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetNumberField(TEXT("dirty_count"), NumListed);
    Data->SetNumberField(TEXT("readonly_dirty_count"), NumReadOnlyDirty);
    Data->SetNumberField(TEXT("map_dirty_count"), NumMapDirty);
    Data->SetNumberField(TEXT("external_dirty_count"), NumExternalDirty);
    Data->SetNumberField(TEXT("asset_dirty_count"), NumAssetDirty);
    Data->SetNumberField(TEXT("filtered_out"), NumFilteredOut);
    Data->SetBoolField(TEXT("pie_active"), IsPlayInEditorActive_System());
    Data->SetStringField(TEXT("current_world_package"), GetCurrentEditorWorldPackageName_System());
    Data->SetStringField(TEXT("path_prefix"), PathPrefix);
    Data->SetBoolField(TEXT("readonly_roots_defaulted"), !bReadOnlyRootsProvided);
    Data->SetArrayField(TEXT("readonly_roots"), mcp_common::StringsToJson(ReadOnlyRoots));
    Data->SetArrayField(TEXT("packages"), PackagesJson);

    FString Message = FString::Printf(
        TEXT("%d dirty package(s): %d map, %d external/OFPA, %d asset. %d under a read-only root."),
        NumListed, NumMapDirty, NumExternalDirty, NumAssetDirty, NumReadOnlyDirty);
    if (NumFilteredOut > 0)
    {
        Message += FString::Printf(TEXT(" %d filtered out by path_prefix '%s'."), NumFilteredOut, *PathPrefix);
    }
    if (NumReadOnlyDirty > 0)
    {
        Message += TEXT(" DO NOT run a bulk save now: the read-only ones would be written to disk.");
    }
    Data->SetStringField(TEXT("message"), Message);
    Data->SetStringField(TEXT("note"), TEXT(
        "This is the IN-MEMORY dirty state only. A package that is clean here can still differ "
        "from git HEAD on disk (it was already saved); `git status` remains the "
        "authority on what actually changed on disk. This command never marks dirty and never saves."));

    // The enumeration IS the observation -- there is no separate effect to verify.
    return mcp_common::MakeOutcomeReply(Data, /*bSuccess=*/true);
}

// MCP:reload_asset_package - Re-read already-loaded packages FROM DISK, non-interactively (it never opens the modal that would freeze a headless editor), either preserving or permanently discarding their in-memory edits. This is the repair for the situation list_dirty_packages flags: a vendored package left dirty by a compile cascade is cleaned with on_dirty="discard" (was_dirty:[...] -> still_dirty:[]), so the next bulk save cannot write it. Also the way to throw away a bad in-memory edit without touching disk. It reports before/after evidence per package, and skips what it cannot safely reload (not loaded, OFPA, worlds) instead of pretending. WARNING: on_dirty="discard" is irreversible for unsaved work -- check what is dirty first.
//   @param package_names - Array of long package names (or object paths) to reload, e.g. ["/Game/VendorPack/.../DT_ItemRows"].
//   @param package_name - Convenience single-name alias for package_names.
//   @param on_dirty - Optional "abort" (default) or "discard". "abort" -> EReloadPackagesInteractionMode::AssumeNegative: a package with unsaved in-memory edits is NOT reloaded and its edits are kept. "discard" -> AssumePositive: UPackageTools clears the dirty flag and reloads, PERMANENTLY THROWING AWAY those in-memory edits.
//   @returns { requested: [string], on_dirty: string, interaction_mode: string, attempted: [string], reloaded: [string], was_dirty: [string], still_dirty: [string], failed: [string], skipped_not_loaded: [string], skipped_ofpa: [string], skipped_world: [object], skipped_invalid: [object], api_returned: bool, api_error: string, verify: { before: [object], after: [object] }, message: string }
//
// Contrary to a common belief ("there is no API to clear a package's dirty
// flag; recovery means closing the editor"), UPackage::SetDirtyFlag is public
// COREUOBJECT_API (Package.h), and
// UPackageTools::PromptUserForDirtyPackages (PackageTools.cpp) already
// calls it for us when the interaction mode is AssumePositive -- which is why
// on_dirty is a PARAMETER here and NOT a separate discard_package_changes tool.
//
// NEVER EReloadPackagesInteractionMode::Interactive -- and that is the
// DEFAULT of the engine signature (PackageTools.h), so it has to be passed
// explicitly every time. Interactive reaches FMessageDialog::Open on the game
// thread (PackageTools.cpp) and, for a world package, also
// CreateNewMapForEditing(bPromptForSave=true). A modal there hangs the game
// thread and with it this MCP session.
//
// NO readonly_roots GUARD HERE, on purpose. Reloading READS from disk and
// discards the in-memory copy; it cannot damage a vendored root. Recovering a vendored
// asset after `git checkout -- Content/VendorPack/...` is precisely the intended use.
TSharedPtr<FJsonObject> HandleReloadAssetPackage(const TSharedPtr<FJsonObject>& Params)
{
    // ---- inputs -----------------------------------------------------------
    TArray<FString> RequestedNames;
    const TArray<TSharedPtr<FJsonValue>>* NamesJson = nullptr;
    if (Params->TryGetArrayField(TEXT("package_names"), NamesJson) && NamesJson)
    {
        for (const TSharedPtr<FJsonValue>& Value : *NamesJson)
        {
            FString Name;
            if (Value.IsValid() && Value->TryGetString(Name) && !Name.IsEmpty())
            {
                RequestedNames.AddUnique(FPackageName::ObjectPathToPackageName(Name));
            }
        }
    }
    FString SingleName;
    if (Params->TryGetStringField(TEXT("package_name"), SingleName) && !SingleName.IsEmpty())
    {
        RequestedNames.AddUnique(FPackageName::ObjectPathToPackageName(SingleName));
    }
    if (RequestedNames.Num() == 0)
    {
        return mcp_common::MakeErrorReply(TEXT(
            "Missing required parameter: package_names (array of long package names) or package_name (single)."));
    }

    FString OnDirty = TEXT("abort");
    Params->TryGetStringField(TEXT("on_dirty"), OnDirty);
    OnDirty = OnDirty.ToLower();
    if (OnDirty != TEXT("abort") && OnDirty != TEXT("discard"))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("on_dirty must be \"abort\" or \"discard\" (got '%s'). \"abort\" keeps unsaved in-memory edits "
                 "and skips those packages; \"discard\" throws them away and reloads from disk."), *OnDirty));
    }
    const bool bDiscardDirty = (OnDirty == TEXT("discard"));
    const EReloadPackagesInteractionMode InteractionMode = bDiscardDirty
        ? EReloadPackagesInteractionMode::AssumePositive
        : EReloadPackagesInteractionMode::AssumeNegative;

    // ---- PIE guard (same refusal shape as fixup_redirectors) --------------
    if (IsPlayInEditorActive_System())
    {
        return mcp_common::MakeErrorReply(TEXT(
            "reload_asset_package refuses to run while Play-In-Editor is active: reloading tears down and "
            "rebuilds live UObjects, which a running PIE world still references. Stop PIE "
            "(stop_play_in_editor) and call again."));
    }

    const FString CurrentWorldPackage = GetCurrentEditorWorldPackageName_System();

    // ---- triage -----------------------------------------------------------
    TArray<TSharedPtr<FJsonValue>> SkippedNotLoadedJson;
    TArray<TSharedPtr<FJsonValue>> SkippedOfpaJson;
    TArray<TSharedPtr<FJsonValue>> SkippedWorldJson;
    TArray<TSharedPtr<FJsonValue>> SkippedInvalidJson;
    TArray<TSharedPtr<FJsonValue>> BeforeJson;

    TArray<UPackage*> PackagesToReload;
    TArray<FString>   AttemptedNames;
    TArray<uint64>    AttemptedPointersBefore;   // Stored as integers on purpose -- see the verify step
    TArray<bool>      AttemptedWasDirty;

    auto AddSkipObject = [](TArray<TSharedPtr<FJsonValue>>& Target, const FString& PackageName, const FString& Reason)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("package"), PackageName);
        Obj->SetStringField(TEXT("reason"), Reason);
        Target.Add(MakeShared<FJsonValueObject>(Obj));
    };

    for (const FString& PackageName : RequestedNames)
    {
        if (!PackageName.StartsWith(TEXT("/")))
        {
            AddSkipObject(SkippedInvalidJson, PackageName, TEXT("Not a UE long package name (must start with '/')."));
            continue;
        }
        if (PackageName.StartsWith(TEXT("/Script/")) || PackageName.StartsWith(TEXT("/Temp/")))
        {
            AddSkipObject(SkippedInvalidJson, PackageName, TEXT("Script and /Temp packages have no reloadable on-disk copy."));
            continue;
        }
        if (IsExternalPackagePath_System(PackageName))
        {
            // OFPA: the world owns these. Reloading one out from under a loaded
            // level is the orphan-resurrection / dangling-actor footgun already
            // known from LoadPackage.
            SkippedOfpaJson.Add(MakeShared<FJsonValueString>(PackageName));
            continue;
        }

        // FindPackage, NEVER LoadPackage: if it is not resident there is no stale
        // in-memory copy to refresh, so the honest answer is a no-op, not a load.
        UPackage* Package = FindPackage(nullptr, *PackageName);
        if (!Package)
        {
            SkippedNotLoadedJson.Add(MakeShared<FJsonValueString>(PackageName));
            continue;
        }
        if (Package->HasAnyPackageFlags(PKG_InMemoryOnly))
        {
            AddSkipObject(SkippedInvalidJson, PackageName, TEXT("Package is in-memory only; there is nothing on disk to reload from."));
            continue;
        }
        if (!FPackageName::DoesPackageExist(PackageName))
        {
            AddSkipObject(SkippedInvalidJson, PackageName, TEXT("Package is loaded but has no file on disk (never saved)."));
            continue;
        }
        if (!CurrentWorldPackage.IsEmpty() && PackageName == CurrentWorldPackage)
        {
            AddSkipObject(SkippedWorldJson, PackageName, TEXT(
                "This is the world currently open in the editor. UPackageTools handles that case by tearing "
                "the map down and reopening it (CreateNewMapForEditing + OpenEditorsForAssets), which is a "
                "level-load, not a package refresh -- use load_level explicitly if that is what you want."));
            continue;
        }
        if (Package->ContainsMap())
        {
            AddSkipObject(SkippedWorldJson, PackageName, TEXT(
                "Map package. Reloading a world package unregisters and re-adds its streaming levels; refused "
                "here to keep this tool to plain asset packages."));
            continue;
        }
        if (PackageName.EndsWith(TEXT("_BuiltData")))
        {
            AddSkipObject(SkippedWorldJson, PackageName, TEXT(
                "Map build data of a world; it is reloaded together with its map, not on its own."));
            continue;
        }

        const bool bWasDirty = Package->IsDirty();
        if (bWasDirty && !bDiscardDirty)
        {
            // Report it up-front instead of letting UPackageTools silently drop it:
            // with AssumeNegative it lands in Filtered.DirtyPackages and is never
            // reloaded, and the only trace is prose inside OutErrorMessage.
            AddSkipObject(SkippedInvalidJson, PackageName, TEXT(
                "Package has unsaved in-memory changes and on_dirty=\"abort\". Pass on_dirty=\"discard\" to "
                "throw those changes away and reload from disk."));
            continue;
        }

        PackagesToReload.Add(Package);
        AttemptedNames.Add(PackageName);
        AttemptedPointersBefore.Add(static_cast<uint64>(reinterpret_cast<UPTRINT>(Package)));
        AttemptedWasDirty.Add(bWasDirty);

        TSharedPtr<FJsonObject> BeforeObj = MakeShared<FJsonObject>();
        BeforeObj->SetStringField(TEXT("package"), PackageName);
        BeforeObj->SetBoolField(TEXT("loaded"), true);
        BeforeObj->SetBoolField(TEXT("is_dirty"), bWasDirty);
        BeforeJson.Add(MakeShared<FJsonValueObject>(BeforeObj));
    }

    // ---- reload -----------------------------------------------------------
    bool bApiReturned = false;
    FText ApiError;
    if (PackagesToReload.Num() > 0)
    {
        bApiReturned = UPackageTools::ReloadPackages(PackagesToReload, ApiError, InteractionMode);
    }

    // EVERY UPackage* and UObject* captured above is now DEAD for
    // dereferencing purposes -- ReloadPackage renames the loaded package to
    // "<Name>_DEADPACKAGE" and LoadPackage()s a brand new one under the original
    // name (PackageReload.cpp), and a failed reload leaves only the
    // renamed corpse. That is exactly why the "before" pointers were stored as
    // integers: the comparison below never dereferences them, and it stays valid
    // even if GC already collected the corpse.
    PackagesToReload.Reset();

    TArray<TSharedPtr<FJsonValue>> ReloadedJson;
    TArray<TSharedPtr<FJsonValue>> WasDirtyJson;
    TArray<TSharedPtr<FJsonValue>> StillDirtyJson;
    TArray<TSharedPtr<FJsonValue>> FailedJson;
    TArray<TSharedPtr<FJsonValue>> AfterJson;
    TArray<TSharedPtr<FJsonValue>> AttemptedJson;
    int32 NumReloaded = 0;

    for (int32 Index = 0; Index < AttemptedNames.Num(); ++Index)
    {
        const FString& PackageName = AttemptedNames[Index];
        AttemptedJson.Add(MakeShared<FJsonValueString>(PackageName));
        if (AttemptedWasDirty[Index])
        {
            WasDirtyJson.Add(MakeShared<FJsonValueString>(PackageName));
        }

        UPackage* NewPackage = FindPackage(nullptr, *PackageName);
        const uint64 NewPointer = static_cast<uint64>(reinterpret_cast<UPTRINT>(NewPackage));
        const bool bPackageObjectChanged = (NewPackage != nullptr) && (NewPointer != AttemptedPointersBefore[Index]);
        const bool bIsDirtyNow = (NewPackage != nullptr) && NewPackage->IsDirty();
        const bool bReloaded = bPackageObjectChanged;

        TSharedPtr<FJsonObject> AfterObj = MakeShared<FJsonObject>();
        AfterObj->SetStringField(TEXT("package"), PackageName);
        AfterObj->SetBoolField(TEXT("loaded"), NewPackage != nullptr);
        AfterObj->SetBoolField(TEXT("is_dirty"), bIsDirtyNow);
        AfterObj->SetBoolField(TEXT("package_object_replaced"), bPackageObjectChanged);
        AfterJson.Add(MakeShared<FJsonValueObject>(AfterObj));

        if (bReloaded)
        {
            ReloadedJson.Add(MakeShared<FJsonValueString>(PackageName));
            ++NumReloaded;
        }
        else
        {
            FailedJson.Add(MakeShared<FJsonValueString>(PackageName));
        }
        if (bIsDirtyNow)
        {
            StillDirtyJson.Add(MakeShared<FJsonValueString>(PackageName));
        }
    }

    // ---- response ---------------------------------------------------------
    TSharedPtr<FJsonObject> VerifyObj = MakeShared<FJsonObject>();
    VerifyObj->SetArrayField(TEXT("before"), BeforeJson);
    VerifyObj->SetArrayField(TEXT("after"), AfterJson);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("requested"), mcp_common::StringsToJson(RequestedNames));
    Data->SetStringField(TEXT("on_dirty"), OnDirty);
    Data->SetStringField(TEXT("interaction_mode"), bDiscardDirty ? TEXT("AssumePositive") : TEXT("AssumeNegative"));
    Data->SetArrayField(TEXT("attempted"), AttemptedJson);
    Data->SetArrayField(TEXT("reloaded"), ReloadedJson);
    Data->SetArrayField(TEXT("was_dirty"), WasDirtyJson);
    Data->SetArrayField(TEXT("still_dirty"), StillDirtyJson);
    Data->SetArrayField(TEXT("failed"), FailedJson);
    Data->SetArrayField(TEXT("skipped_not_loaded"), SkippedNotLoadedJson);
    Data->SetArrayField(TEXT("skipped_ofpa"), SkippedOfpaJson);
    Data->SetArrayField(TEXT("skipped_world"), SkippedWorldJson);
    Data->SetArrayField(TEXT("skipped_invalid"), SkippedInvalidJson);
    // Reported, NOT used to decide success. UPackageTools::ReloadPackages
    // returns "the set of loaded packages changed" -- true if ANY package
    // reloaded, even when the one you asked for was silently dropped as dirty.
    Data->SetBoolField(TEXT("api_returned"), bApiReturned);
    Data->SetStringField(TEXT("api_error"), ApiError.ToString());
    Data->SetObjectField(TEXT("verify"), VerifyObj);

    // Success = every REQUESTED package came back as a genuinely new UPackage
    // object under its original name. Anything skipped counts against it.
    const bool bSuccess = (NumReloaded == RequestedNames.Num());

    FString Message = FString::Printf(TEXT("Reloaded %d of %d requested package(s) (on_dirty=%s)."),
        NumReloaded, RequestedNames.Num(), *OnDirty);
    if (SkippedNotLoadedJson.Num() > 0)
    {
        Message += FString::Printf(TEXT(" %d were not resident in memory (nothing to refresh -- no-op, not an error in itself)."),
            SkippedNotLoadedJson.Num());
    }
    if (SkippedOfpaJson.Num() > 0)
    {
        Message += FString::Printf(TEXT(" %d OFPA external package(s) refused."), SkippedOfpaJson.Num());
    }
    if (SkippedWorldJson.Num() > 0)
    {
        Message += FString::Printf(TEXT(" %d world/map package(s) refused."), SkippedWorldJson.Num());
    }
    if (SkippedInvalidJson.Num() > 0)
    {
        Message += FString::Printf(TEXT(" %d refused for other reasons (see skipped_invalid)."), SkippedInvalidJson.Num());
    }
    if (!ApiError.IsEmpty())
    {
        Message += FString::Printf(TEXT(" UPackageTools reported: %s"), *ApiError.ToString());
    }
    Data->SetStringField(TEXT("message"), Message);

    const mcp_common::EMcpVerification Verification = AttemptedNames.Num() == 0
        ? mcp_common::EMcpVerification::NotAttempted
        : (bSuccess ? mcp_common::EMcpVerification::Verified : mcp_common::EMcpVerification::Failed);

    return mcp_common::MakeVerifiedReply(
        Data,
        bSuccess,
        Verification,
        TEXT("refind_package_after_reload"),
        FString::Printf(TEXT("Re-resolved each package with FindPackage after the call: %d of %d came back as a "
                             "NEW UPackage object (the old one is renamed to <Name>_DEADPACKAGE by "
                             "PackageReload.cpp, so an unchanged pointer means no reload happened)."),
            NumReloaded, RequestedNames.Num()),
        bSuccess ? FString() : Message);
}

// ---------------------------------------------------------------------------
// import_font_file
//
// Why it exists: import_texture_file imports images only, and no other tool imports
// fonts. Without it a game's typeface has to be imported outside the agent, by
// someone opening the editor by hand.
//
// THE TRAP TO AVOID: UFontFileImportFactory starts with
// BatchCreateFontAsset = Unknown (EditorFactories.cpp), and with that value its
// ConfigureProperties() opens a MODAL DIALOG asking whether to also create a UFont. In a
// headless call that hangs the editor waiting for a click that never comes. That is why
// it is ALWAYS set explicitly here before importing.
// ---------------------------------------------------------------------------
// MCP:import_font_file - Import a .ttf/.otf/.ttc/.otc font from disk as a UFontFace, and optionally the companion UFont that a FSlateFontInfo needs to actually render text. This is the only way to get a typeface into the project headlessly: import_texture_file imports images only, and nothing else in the toolset touches fonts. Drives UFontFileImportFactory inline (never IAssetTools::ImportAssets, which asserts when re-entered from the engine tick) and ALWAYS sets BatchCreateFontAsset explicitly -- left at its default the factory pops a MODAL DIALOG asking whether to create the UFont, which would hang the editor on a click that is never coming. Once imported, point a widget at it with set_widget_property on the dotted path "Font.FontObject" (a UFont) plus "Font.TypefaceFontName", or "Font.Size" for the size.
//   @param source_file_path [string!] - Absolute path on disk to the .ttf/.otf/.ttc/.otc file. A missing file, or any other extension, is a clean error before anything is touched.
//   @param destination_path [string!] - UE long path of the destination content folder, e.g. /Game/MyGame/UI/Fonts.
//   @param name - Optional asset name; defaults to the source file's base name.
//   @param create_font_asset [bool] - Also create the companion UFont asset (default true). A UFontFace alone is a raw typeface: a FSlateFontInfo's FontObject wants a UFont. Pass false to import only the face, e.g. to add it as another typeface entry of an existing font.
//   @returns { source_file_path, destination_path, imported_object_path, font_face_created, create_font_asset, message }
TSharedPtr<FJsonObject> HandleImportFontFile(const TSharedPtr<FJsonObject>& Params)
{
    FString SourceFilePath, DestinationPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("source_file_path"), SourceFilePath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("destination_path"), DestinationPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    if (!FPaths::FileExists(SourceFilePath))
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Source file does not exist: %s"), *SourceFilePath));
    if (!DestinationPath.StartsWith(TEXT("/")))
        return mcp_common::MakeErrorReply(TEXT("destination_path must be a UE long path, e.g. /Game/MyGame/UI/Fonts"));

    const FString Extension = FPaths::GetExtension(SourceFilePath).ToLower();
    static const TSet<FString> FontExtensions = { TEXT("ttf"), TEXT("ttc"), TEXT("otf"), TEXT("otc") };
    if (!FontExtensions.Contains(Extension))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("import_font_file only accepts .ttf/.ttc/.otf/.otc; got '.%s'. For images use "
                 "import_texture_file."), *Extension));
    }

    FString AssetName;
    Params->TryGetStringField(TEXT("name"), AssetName);

    bool bCreateFontAsset = true;
    Params->TryGetBoolField(TEXT("create_font_asset"), bCreateFontAsset);

    // The factory names the companion UFont's package after the face's, "<folder>/<name>_Font", and makes an FName of
    // it during the import: a face name that fits can still make one past the limit.
    if (bCreateFontAsset)
    {
        const FString FaceName = AssetName.IsEmpty() ? FPaths::GetBaseFilename(SourceFilePath) : AssetName;
        if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(DestinationPath / FaceName + TEXT("_Font")))
        {
            return Invalid;
        }
    }

    UFontFileImportFactory* FontFactory = NewObject<UFontFileImportFactory>();
    FontFactory->bEditorImport = true;
    // WITHOUT this the factory opens a modal and the editor hangs waiting (see the note above).
    FontFactory->BatchCreateFontAsset = bCreateFontAsset
        ? EBatchCreateFontAsset::CreateIfNoFontExists
        : EBatchCreateFontAsset::No;

    FString Error;
    const FString ImportedPath = mcp_common::FactoryCreateBinaryImport(
        SourceFilePath, DestinationPath, AssetName, FontFactory, UFontFace::StaticClass(), Error);

    if (ImportedPath.IsEmpty())
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Font import produced no asset from: %s (%s)"), *SourceFilePath, *Error));
    }

    // WARNING: the factory creates the companion UFont in a DIFFERENT package ("<path>/<Name>_Font",
    // EditorFactories.cpp) and leaves it MarkPackageDirty but NOT saved --
    // FactoryCreateBinaryImport only saves the object it is handed back, which is the UFontFace.
    // Without this block the font "imports fine" and the UFont is lost when the editor closes:
    // the classic success that does not survive the session.
    FString FontAssetPath;
    bool bFontAssetSaved = false;
    FString FontSaveError;
    if (bCreateFontAsset)
    {
        const FString BaseName = AssetName.IsEmpty() ? FPaths::GetBaseFilename(SourceFilePath) : AssetName;
        const FString ExpectedFontPath = FString::Printf(TEXT("%s/%s_Font"),
            *DestinationPath.TrimEnd(), *BaseName);

        if (UObject* FontAsset = UEditorAssetLibrary::LoadAsset(ExpectedFontPath))
        {
            FontAssetPath = FontAsset->GetPathName();
            bFontAssetSaved = mcp_common::SaveAssetPackage(FontAsset->GetOutermost(), FontAsset, FontSaveError);
        }
        else
        {
            FontSaveError = FString::Printf(
                TEXT("no UFont was found at '%s': the factory may have reused an existing one "
                     "(BatchCreateFontAsset=CreateIfNoFontExists) -- check the folder."), *ExpectedFontPath);
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("source_file_path"), SourceFilePath);
    Result->SetStringField(TEXT("destination_path"), DestinationPath);
    Result->SetStringField(TEXT("imported_object_path"), ImportedPath);
    Result->SetBoolField(TEXT("font_face_created"), true);
    Result->SetBoolField(TEXT("create_font_asset"), bCreateFontAsset);
    Result->SetStringField(TEXT("font_asset_path"), FontAssetPath);
    Result->SetBoolField(TEXT("font_asset_saved"), bFontAssetSaved);
    if (!FontSaveError.IsEmpty())
    {
        Result->SetStringField(TEXT("font_asset_note"), FontSaveError);
    }
    Result->SetStringField(TEXT("message"), bCreateFontAsset
        ? TEXT("UFontFace imported, and its companion UFont saved to its own package. Point a widget "
               "at the UFont with set_widget_property on the dotted path \"Font.FontObject\".")
        : TEXT("UFontFace imported WITHOUT a companion UFont: a FSlateFontInfo's FontObject wants a "
               "UFont, so this face is only usable as a typeface entry of an existing font asset."));
    return mcp_common::MakeOkReply(Result);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("create_directory"), &HandleCreateDirectory },
    { TEXT("delete_directory"), &HandleDeleteDirectory },
    { TEXT("set_plugin_enabled"), &HandleSetPluginEnabled },
    { TEXT("execute_console_command"), &HandleExecuteConsoleCommand },
    { TEXT("fixup_redirectors"), &HandleFixupRedirectors },
    { TEXT("import_font_file"), &HandleImportFontFile },
    { TEXT("list_dirty_packages"), &HandleListDirtyPackages },
    { TEXT("reload_asset_package"), &HandleReloadAssetPackage },
    { TEXT("save_asset"), &HandleSaveAsset },
};
} // namespace UEAgentSystemCommands
