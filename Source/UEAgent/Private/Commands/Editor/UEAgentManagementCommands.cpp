// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Asset/AssetCommandHelpers.h"
#include "Commands/Common/UEAgentGraphUtils.h"  // FUEAgentGraphUtils::ResolveClass (expected_class of delete_asset / duplicate_asset)
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "GameFramework/Actor.h"
#include "EngineUtils.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Engine.h"
#include "Misc/PackageName.h"
#include "UObject/UnrealType.h"
#include "UObject/PropertyPortFlags.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Blueprint/BlueprintSupport.h"
#include "Engine/Selection.h"
#include "HAL/PlatformFilemanager.h"
#include "HAL/FileManager.h"             // IFileManager::FileExists (delete_asset: is the file still on disk)
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "EditorAssetLibrary.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Misc/ConfigCacheIni.h"
#include "CoreGlobals.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/Timespan.h"
#include "Runtime/Launch/Resources/Version.h"
#include "Interfaces/IPluginManager.h"
#include "PluginDescriptor.h"
#include "ModuleDescriptor.h"
#include "ObjectTools.h"                 // ObjectTools::ForceDeleteObjects (registry-independent map/asset delete)
#include "Editor.h"                       // GEditor (PIE check for duplicate_asset's post-duplicate save)
// Not used directly here (PIE detection lives in mcp_common::IsPieActive).
// The include stays: this module builds with bUseUnity=true, and removing it could
// break ANOTHER file in the same unity blob that relies on it being pulled in here.
#include "Editor/LevelEditor/Public/LevelEditorSubsystem.h"
#include "UObject/UObjectGlobals.h"      // StaticLoadObject / StaticFindObject
#include "UObject/UObjectIterator.h"     // TObjectIterator<UClass> (CDO modal pre-check for rename/move)
#include "UObject/ObjectRedirector.h"    // UObjectRedirector (redirector_left probe after a rename/move)
#include "Serialization/ArchiveReplaceObjectRef.h" // inter-asset ref remap for duplicate_directory
#include "Materials/MaterialInterface.h"           // UMaterialInterface (MaterialInstance parent repoint)
#include "Materials/MaterialInstanceConstant.h"    // UMaterialInstanceConstant::SetParentEditorOnly
#include "Serialization/JsonSerializer.h"          // FJsonSerializer::Serialize (run_external_project_script cloned .uproject)
#include "Serialization/JsonWriter.h"              // TJsonWriterFactory (run_external_project_script cloned .uproject)
#include "HAL/PlatformProcess.h"                    // FPlatformProcess (run_external_project_script headless commandlet)
#include "HAL/PlatformTime.h"                       // FPlatformTime::Seconds (timeout / duration)
#include "Misc/Guid.h"                              // FGuid (unique temp .uproject name)
#include "AssetRegistry/IAssetRegistry.h"           // IAssetRegistry::GetDependencies (audit_fork_refs)
#include "Misc/AssetRegistryInterface.h"            // UE::AssetRegistry::EDependencyCategory
#include "UEAgentProjectSettings.h"               // content roots and the fork-origin tag

namespace UEAgentManagementCommands
{

// MCP-DOMAIN: Management

namespace
{
    // Second-pass reference remap shared by duplicate_asset (remap_internal_refs) and
    // duplicate_directory. For every freshly duplicated object, rewrite any reference that
    // still points at an original in ReplacementMap so it points at the corresponding copy
    // instead. References to objects OUTSIDE the set are left untouched -> the copies stay
    // shared, read-only, with the originals ("copy what you modify, share the rest").
    // When bRepointMaterialInstanceParents is set, MaterialInstanceConstant parents that map
    // to a copy are additionally re-applied via SetParentEditorOnly so the material chain's
    // recache/PostEditChange runs (belt-and-suspenders on top of the archive pass, which also
    // catches the Parent pointer). Returns the number of objects whose graph was rewritten.
    int32 RemapReferencesAmongCopies(
        const TMap<UObject*, UObject*>& ReplacementMap,
        const TArray<UObject*>& NewObjects,
        bool bRepointMaterialInstanceParents)
    {
        int32 NumRemapped = 0;
        for (UObject* Dup : NewObjects)
        {
            if (!Dup) { continue; }

            // Capture a MaterialInstance parent that needs re-applying BEFORE the archive pass
            // rewrites the pointer (afterwards MIC->Parent already equals the copy and would no
            // longer be a key in ReplacementMap).
            UMaterialInterface* RepointParentTo = nullptr;
            if (bRepointMaterialInstanceParents)
            {
                if (UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(Dup))
                {
                    if (UMaterialInterface* CurParent = MIC->Parent)
                    {
                        if (UObject* const* Mapped = ReplacementMap.Find(CurParent))
                        {
                            RepointParentTo = Cast<UMaterialInterface>(*Mapped);
                        }
                    }
                }
            }

            FArchiveReplaceObjectRef<UObject> ReplaceAr(Dup, ReplacementMap,
                EArchiveReplaceObjectFlags::IgnoreOuterRef | EArchiveReplaceObjectFlags::IgnoreArchetypeRef);
            Dup->MarkPackageDirty();
            ++NumRemapped;

            if (RepointParentTo)
            {
                if (UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(Dup))
                {
                    MIC->SetParentEditorOnly(RepointParentTo, /*RecacheShader=*/false);
                    MIC->PostEditChange();
                    MIC->MarkPackageDirty();
                }
            }
        }
        return NumRemapped;
    }

    // --- rename/move guards (shared by rename_asset and move_asset) ----------------------------
    // BOTH handlers end up inside FAssetRenameManager (rename_asset calls
    // IAssetTools::RenameAssets directly; move_asset calls UEditorAssetLibrary::RenameAsset, which
    // routes to the same place), so they share the same two hazards, visible in the engine
    // source (Developer/AssetTools/Private/AssetRenameManager.cpp):
    //
    //   1. MODAL. FixReferencesAndRename opens an FMessageDialog (OkCancel) when the asset
    //      being renamed is referenced by a class default object. `bWithDialog=false` (what
    //      RenameAssets passes) does NOT gate that call -- it only gates the slow-task widget
    //      and the failure report. In an interactive editor that modal BLOCKS the game thread,
    //      which kills the MCP session that asked for the rename. The other modal ("save
    //      referencing packages?") is gated by bAutoCheckout, which RenameAssets sets to true,
    //      so it cannot fire from here.
    //   2. SILENT HALF-FAILURE. PerformAssetRename collects FailedPackages from
    //      FEditorFileUtils::PromptForCheckoutAndSave and only ever uses them to revert files WHEN
    //      SOURCE CONTROL IS ENABLED (`ISourceControlModule::Get().IsEnabled()`). Without a UE
    //      source-control provider (e.g. plain git) a package that failed to save is
    //      dropped on the floor and RenameAssets still returns true. Hence the post-rename
    //      verification below reads the RESULT (new object resolves + new file on disk + whether a
    //      redirector was left behind) instead of trusting that bool.

    // True when a NATIVE class' CDO holds a direct hard reference to Asset, which is the condition
    // FAssetRenameManager::FindCDOReferences tests before opening the modal -- replicated
    // here (same iteration, same filters, same pointer comparison) so we can REFUSE instead of
    // letting the engine open it. Only the HARD half is reproducible: the soft-reference half
    // needs FSoftObjectPathRenameSerializer, which is private to the AssetTools module,
    // so a false NEGATIVE is possible and the caller is told so in the response.
    bool AnyNativeCdoHardReferences(const UObject* Asset, FString& OutFirstReferencingClass)
    {
        if (!Asset) { return false; }
        for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
        {
            UClass* Cls = *ClassIt;
            // Native classes only: Blueprint (and SKEL_) classes are skipped.
            UObject* CDO = AssetCommandHelpers::NativeClassDefault(Cls);
            if (!CDO)
            {
                continue;
            }

            for (TFieldIterator<FObjectProperty> PropIt(Cls); PropIt; ++PropIt)
            {
                const UObject* Value = PropIt->GetPropertyValue(PropIt->ContainerPtrToValuePtr<UObject>(CDO));
                if (Value == Asset)
                {
                    OutFirstReferencingClass = Cls->GetPathName();
                    return true;
                }
            }
        }
        return false;
    }

    // Post-rename per-item verification: never report the rename by the bool RenameAssets returned.
    // Adds { new_object_resolves, new_file_on_disk, new_file_path, redirector_left } to Result and
    // one piece of verification evidence. Verified only when the new object resolves AND its
    // package is on disk; the "resolves but no file" case is exactly the FailedPackages drop
    // described above and is reported as a FAILED verification, not as a success. Returns whether
    // the readback verified, so the caller can also amend its human-readable message field (a
    // caller that only looks at `*_success` must not be left believing a half-failure worked).
    bool VerifyRenameOutcome(
        const TSharedPtr<FJsonObject>& Result,
        const FString& OldObjectPath,
        const FString& NewObjectPath,
        const FString& NewPackageName)
    {
        if (!Result.IsValid()) { return false; }

        const bool bNewResolves = StaticFindObject(UObject::StaticClass(), nullptr, *NewObjectPath) != nullptr;

        FString NewFilePath;
        bool bNewFileOnDisk = false;
        if (FPackageName::DoesPackageExist(NewPackageName, &NewFilePath))
        {
            bNewFileOnDisk = FPlatformFileManager::Get().GetPlatformFile().FileExists(*NewFilePath);
        }

        // A redirector object at the pre-rename path is what keeps existing referencers working;
        // its presence/absence is a fact the caller needs (fixup_redirectors input) and is checked
        // in memory, where the rename just created it -- no registry-timing dependency.
        const bool bRedirectorLeft = StaticFindObject(UObjectRedirector::StaticClass(), nullptr, *OldObjectPath) != nullptr;

        Result->SetBoolField(TEXT("new_object_resolves"), bNewResolves);
        Result->SetBoolField(TEXT("new_file_on_disk"), bNewFileOnDisk);
        Result->SetStringField(TEXT("new_file_path"), NewFilePath);
        Result->SetBoolField(TEXT("redirector_left"), bRedirectorLeft);

        mcp_common::EMcpVerification Status = mcp_common::EMcpVerification::Verified;
        FString Detail;
        if (!bNewResolves)
        {
            Status = mcp_common::EMcpVerification::Failed;
            Detail = FString::Printf(TEXT("no object resolves at '%s' after the rename"), *NewObjectPath);
        }
        else if (!bNewFileOnDisk)
        {
            Status = mcp_common::EMcpVerification::Failed;
            Detail = FString::Printf(
                TEXT("'%s' exists in memory but its package was NOT written to disk. FAssetRenameManager drops ")
                TEXT("PromptForCheckoutAndSave's FailedPackages unless UE source control is connected, so this ")
                TEXT("half-failure is invisible in its return value. Re-save with save_asset."),
                *NewObjectPath);
        }
        else
        {
            Detail = FString::Printf(TEXT("'%s' resolves and '%s' exists on disk; redirector at the old path: %s"),
                *NewObjectPath, *NewFilePath, bRedirectorLeft ? TEXT("yes") : TEXT("no"));
        }
        mcp_common::AddVerificationEvidence(Result, Status,
            TEXT("post-rename readback (StaticFindObject + DoesPackageExist/FileExists + redirector probe)"), Detail);
        return Status == mcp_common::EMcpVerification::Verified;
    }

    // Amend the handler's human-readable message when the engine claimed success but the readback
    // did not confirm it. Keeps `*_success` exactly as the engine reported it (callers parse it)
    // while making sure the message next to it cannot be read as "all good".
    void AppendUnverifiedWarning(const TSharedPtr<FJsonObject>& Result, const FString& MessageField)
    {
        if (!Result.IsValid()) { return; }
        FString Message;
        Result->TryGetStringField(MessageField, Message);
        Result->SetStringField(MessageField, Message +
            TEXT(" -- WARNING: the engine reported success but the post-operation readback did NOT confirm it; ")
            TEXT("see verification_checks for what failed (new_object_resolves / new_file_on_disk)."));
    }

    // --- audit_class_purity helpers (fork-inheritance lineage) ---------------------------------
    // Content package that owns a UClass: a BPGC's asset package (/Game/MyGame/.../BP_X) or the
    // native module (/Script/Engine). Empty if the class or its package is null.
    FString ClassPackageName(const UClass* C)
    {
        const UPackage* Pkg = C ? C->GetOutermost() : nullptr;
        return Pkg ? Pkg->GetName() : FString();
    }

    // True when a package path lies at or under a content root (exact match or Root + "/").
    bool PackageUnderRoot(const FString& Pkg, const FString& Root)
    {
        return !Pkg.IsEmpty() && (Pkg == Root || Pkg.StartsWith(Root + TEXT("/")));
    }

    // First base-library root the package falls under, or empty when it matches none.
    FString MatchBaseLibrary(const FString& Pkg, const TArray<FString>& BaseLibs)
    {
        for (const FString& Lib : BaseLibs)
        {
            if (PackageUnderRoot(Pkg, Lib)) { return Lib; }
        }
        return FString();
    }

    // Normalize a fork-origin tag value down to its PACKAGE name, so it can be compared
    // against a real ancestor's package regardless of which spelling the tag carries.
    // The tag usually holds a bare asset path ("/Game/VendorPack/.../BP_Base"), but tolerate the
    // object path ("....BP_Base") and the generated-class path ("....BP_Base_C") too --
    // everything from the first '.' on is the object/class part of the same package.
    FString ForkOriginToPackageName(const FString& RawOrigin)
    {
        FString S = RawOrigin;
        S.TrimStartAndEndInline();
        int32 DotIdx = INDEX_NONE;
        if (S.FindChar(TEXT('.'), DotIdx)) { S = S.Left(DotIdx); }
        S.RemoveFromEnd(TEXT("/"));
        return S;
    }

    // Ascend Cur's FULL parent chain (exclusive of Cur, up to UObject) looking for the class the
    // fork-origin tag names -- i.e. ask whether the tag TELLS THE TRUTH.
    //
    // The whole chain matters, not just the immediate parent: a legitimate fork may declare an
    // ancestor further up (a fork declaring BP_VendorBase while parented to BP_VendorChild,
    // which is BP_VendorBase's child -- that is a true statement).
    // Conversely a COPY-sibling (BP_MyCharacter declaring BP_VendorCharacter while parented to
    // BP_VendorCharacterBase, their shared parent) never appears in the chain -- that is the lie this catches.
    //
    // OutChain collects the walked ancestors (for the violation report) and is capped so a
    // pathological hierarchy can't balloon the response.
    bool ForkOriginIsAncestorOf(
        const UClass* Cur,
        const FString& OriginPkg,
        const FString& RawOrigin,
        FString& OutMatchedAncestor,
        TArray<FString>& OutChain)
    {
        constexpr int32 MaxChainWalk = 256; // UClass chains can't cycle; this only bounds the report.
        bool bFound = false;
        int32 Steps = 0;
        for (const UClass* A = Cur ? Cur->GetSuperClass() : nullptr; A && Steps < MaxChainWalk; A = A->GetSuperClass(), ++Steps)
        {
            const FString APath = A->GetPathName();
            OutChain.Add(APath);
            if (!bFound
                && (ClassPackageName(A).Equals(OriginPkg, ESearchCase::IgnoreCase)
                    || APath.Equals(RawOrigin, ESearchCase::IgnoreCase)))
            {
                bFound = true;
                OutMatchedAncestor = APath;
            }
        }
        return bFound;
    }
}

// Helpers for the asset and plugin handlers below.
namespace UEAgentManagementCommandsImpl
{

    TSharedPtr<FJsonObject> AssetLoadFailedReply(const FString& Path)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Failed to load asset: %s"), *Path));
    }

    // "/Game/Foo/DT_X" becomes "/Game/Foo/DT_X.DT_X"; a path that already names the object
    // (its last segment has a '.') is returned unchanged. The registry lookup by object path
    // needs the ".<name>" form, and a bare package path cannot load an asset that is not
    // loaded yet.
    FString ToObjectPath(const FString& AssetPath)
    {
        int32 LastSlash = INDEX_NONE;
        const FString Leaf = AssetPath.FindLastChar(TEXT('/'), LastSlash)
            ? AssetPath.RightChop(LastSlash + 1)
            : AssetPath;
        if (Leaf.IsEmpty() || Leaf.Contains(TEXT(".")))
        {
            return AssetPath;
        }
        return AssetPath + TEXT(".") + Leaf;
    }

    IAssetTools& AssetTools()
    {
        return FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
    }

    // Outcome block shared by duplicate_asset, rename_asset and move_asset:
    // "<Prefix>_success", "<Prefix>_message", and what the registry knows about the asset at
    // its new object path (class, package, the file size when the file is there). A failed
    // operation reports "None". Returns that registry entry -- an empty one after a failure --
    // so the caller can publish whether the new asset really resolves.
    FAssetData AddOperationOutcome(const TSharedPtr<FJsonObject>& Result, const FString& Prefix, bool bDone,
                                   const FString& NewObjectPath, const TCHAR* DoneMessage, const TCHAR* FailMessage)
    {
        Result->SetBoolField(Prefix + TEXT("_success"), bDone);
        Result->SetStringField(Prefix + TEXT("_message"), bDone ? DoneMessage : FailMessage);
        if (!bDone)
        {
            Result->SetStringField(TEXT("new_asset_class"), TEXT("None"));
            Result->SetStringField(TEXT("new_package_name"), TEXT("None"));
            return FAssetData();
        }

        const FAssetData Landed = mcp_common::FindAssetDataByPath(NewObjectPath);
        const FString LandedPackage = Landed.PackageName.ToString();
        Result->SetStringField(TEXT("new_asset_class"), Landed.AssetClassPath.ToString());
        Result->SetStringField(TEXT("new_package_name"), LandedPackage);

        FString LandedFile;
        if (mcp_common::FindPackageFileOnDisk(LandedPackage, LandedFile))
        {
            const int64 Bytes = FMath::Max<int64>(0, FPlatformFileManager::Get().GetPlatformFile().FileSize(*LandedFile));
            Result->SetNumberField(TEXT("new_file_size_bytes"), Bytes);
            Result->SetNumberField(TEXT("new_file_size_mb"), Bytes / (1024.0 * 1024.0));
        }
        return Landed;
    }

    // After a rename or move: is the ORIGINAL package file still on disk (a redirector, or a file
    // the operation left behind)?
    void AddOriginalFileExists(const TSharedPtr<FJsonObject>& Result, const FString& OriginalPackage)
    {
        if (FPackageName::IsValidLongPackageName(OriginalPackage))
        {
            FString OriginalFile;
            Result->SetBoolField(TEXT("original_file_exists"), mcp_common::FindPackageFileOnDisk(OriginalPackage, OriginalFile));
        }
    }

    // What the registry knew about the source asset before the operation.
    void AddOriginalAssetInfo(const TSharedPtr<FJsonObject>& Result, const FAssetData& Source)
    {
        Result->SetStringField(TEXT("original_asset_class"), Source.AssetClassPath.ToString());
        Result->SetStringField(TEXT("original_package_name"), Source.PackageName.ToString());
    }

    // The part of a plugin's identity get_plugin_status reports. FPluginDescriptor has no
    // bEnabled and IPlugin no IsLoaded(), so "loaded" and "enabled" both report IsEnabled().
    void AddPluginIdentity(const TSharedPtr<FJsonObject>& Result, const IPlugin& Plugin)
    {
        const FPluginDescriptor& Descriptor = Plugin.GetDescriptor();
        const bool bEnabled = Plugin.IsEnabled();
        Result->SetStringField(TEXT("plugin_status"), bEnabled ? TEXT("Enabled") : TEXT("Disabled"));
        Result->SetStringField(TEXT("plugin_version"), Descriptor.VersionName);
        Result->SetStringField(TEXT("plugin_description"), Descriptor.Description);
        Result->SetStringField(TEXT("plugin_author"), Descriptor.CreatedBy);
        Result->SetStringField(TEXT("plugin_category"), Descriptor.Category);
        Result->SetBoolField(TEXT("plugin_loaded"), bEnabled);
        Result->SetBoolField(TEXT("plugin_enabled"), bEnabled);
        Result->SetStringField(TEXT("plugin_path"), Plugin.GetBaseDir());
        Result->SetStringField(TEXT("plugin_file"), Plugin.GetDescriptorFileName());
    }

    // Plugins this one references. The reference descriptor carries no version, so every
    // entry reports "Unknown".
    TArray<TSharedPtr<FJsonValue>> DescribePluginReferences(const FPluginDescriptor& Descriptor)
    {
        TArray<TSharedPtr<FJsonValue>> Rows;
        for (const FPluginReferenceDescriptor& Reference : Descriptor.Plugins)
        {
            TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("name"), Reference.Name);
            Row->SetBoolField(TEXT("enabled"), Reference.bEnabled);
            Row->SetBoolField(TEXT("optional"), Reference.bOptional);
            Row->SetStringField(TEXT("version"), TEXT("Unknown"));
            Rows.Add(MakeShared<FJsonValueObject>(Row));
        }
        return Rows;
    }

    // Host types other than Runtime and Editor are all reported as "Developer".
    const TCHAR* HostTypeLabel(EHostType::Type HostType)
    {
        if (HostType == EHostType::Runtime)
        {
            return TEXT("Runtime");
        }
        if (HostType == EHostType::Editor)
        {
            return TEXT("Editor");
        }
        return TEXT("Developer");
    }

    // The plugin's modules; loading_phase is true only for the Default phase. (Platform lists
    // are not reported: FModuleDescriptor has no Platforms field.)
    TArray<TSharedPtr<FJsonValue>> DescribePluginModules(const FPluginDescriptor& Descriptor)
    {
        TArray<TSharedPtr<FJsonValue>> Rows;
        for (const FModuleDescriptor& Module : Descriptor.Modules)
        {
            TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("name"), Module.Name.ToString());
            Row->SetStringField(TEXT("type"), HostTypeLabel(Module.Type));
            Row->SetBoolField(TEXT("loading_phase"), Module.LoadingPhase == ELoadingPhase::Default);
            Rows.Add(MakeShared<FJsonValueObject>(Row));
        }
        return Rows;
    }

    // Why duplicate_asset may not write a new asset at PackageName, or empty when it may: a package under an
    // enforced read-only root, a name under no mounted content root (it has no file), or a package whose file is
    // outside the project directory (engine or engine-plugin content, which a copy is meant to leave untouched).
    FString PackageWriteBlocker(const FString& PackageName)
    {
        if (mcp_common::IsUnderReadOnlyRoot(PackageName) && mcp_common::IsReadOnlyRootSaveEnforcementEnabled())
        {
            return FString::Printf(TEXT("%s is under a read-only root"), *PackageName);
        }
        FString FileName;
        if (!FPackageName::TryConvertLongPackageNameToFilename(PackageName, FileName, FPackageName::GetAssetPackageExtension()))
        {
            return FString::Printf(TEXT("%s is not under a mounted content root"), *PackageName);
        }
        if (!FPaths::IsUnderDirectory(FPaths::ConvertRelativePathToFull(FileName), FPaths::ConvertRelativePathToFull(FPaths::ProjectDir())))
        {
            return FString::Printf(TEXT("%s is engine content outside the project"), *PackageName);
        }
        return FString();
    }

    // The type guard of delete_asset and duplicate_asset: the classes named by expected_class (Field), one class name
    // or a list of them. Field absent or null leaves OutClasses empty (no guard). Anything else that is not a non-empty
    // name or a non-empty list of non-empty names, or a name that resolves to no class, is the error reply of Command
    // returned (nothing is done).
    TSharedPtr<FJsonObject> ResolveExpectedClasses(const TCHAR* Command, const TSharedPtr<FJsonValue>& Field,
        TArray<UClass*>& OutClasses)
    {
        if (!Field.IsValid() || Field->IsNull())
        {
            return nullptr;
        }
        TArray<FString> Names;
        if (Field->Type == EJson::String)
        {
            Names.Add(Field->AsString());
        }
        else if (Field->Type == EJson::Array)
        {
            for (const TSharedPtr<FJsonValue>& Entry : Field->AsArray())
            {
                if (!Entry.IsValid() || Entry->Type != EJson::String)
                {
                    return mcp_common::MakeErrorReply(FString::Printf(
                        TEXT("%s: every entry of 'expected_class' must be a class name string; nothing was done"), Command));
                }
                Names.Add(Entry->AsString());
            }
            if (Names.Num() == 0)
            {
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("%s: 'expected_class' is an empty list; nothing was done"), Command));
            }
        }
        else
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("%s: 'expected_class' must be a class name or a list of class names; nothing was done"), Command));
        }
        for (const FString& Raw : Names)
        {
            const FString Name = Raw.TrimStartAndEnd();
            if (Name.IsEmpty())
            {
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("%s: 'expected_class' has an empty class name; nothing was done"), Command));
            }
            UClass* Class = FUEAgentGraphUtils::ResolveClass(Name);
            if (!Class)
            {
                return mcp_common::MakeErrorReply(FString::Printf(TEXT("%s: expected_class names no class, nothing was done. %s"),
                    Command, *FUEAgentGraphUtils::DescribeClassNotFound(Name)));
            }
            OutClasses.AddUnique(Class);
        }
        return nullptr;
    }

    // Whether Asset passes the expected_class guard: it IsA one of Classes, or it is a Blueprint whose generated
    // class IsChildOf one of them (the Blueprint asset is a UBlueprint; its instances have the generated class).
    bool MatchesExpectedClass(const UObject& Asset, const TArray<UClass*>& Classes)
    {
        const UBlueprint* Blueprint = Cast<UBlueprint>(&Asset);
        const UClass* Generated = Blueprint ? Blueprint->GeneratedClass.Get() : nullptr;
        for (const UClass* Class : Classes)
        {
            if (Asset.IsA(Class) || (Generated && Generated->IsChildOf(Class)))
            {
                return true;
            }
        }
        return false;
    }

    // "asset is not a A or B (got C)" for Asset failing the expected_class guard; a Blueprint names its generated class.
    FString WrongClassNote(const UObject& Asset, const TArray<UClass*>& Classes)
    {
        TArray<FString> Wanted;
        for (const UClass* Class : Classes)
        {
            Wanted.Add(Class->GetName());
        }
        const UBlueprint* Blueprint = Cast<UBlueprint>(&Asset);
        const FString Got = (Blueprint && Blueprint->GeneratedClass)
            ? FString::Printf(TEXT("%s generating %s"), *Asset.GetClass()->GetName(), *Blueprint->GeneratedClass->GetName())
            : Asset.GetClass()->GetName();
        return FString::Printf(TEXT("asset is not a %s (got %s) - wrong type"), *FString::Join(Wanted, TEXT(" or ")), *Got);
    }
}

// Asset file operations

// MCP:duplicate_asset - Duplicates an asset to a new name, in the same directory or in destination_path, then PERSISTS the copy to disk immediately (unless Play-In-Editor is active -- see the "saved"/"save_error" note on the save step below; a duplicate is very often used AS a safety net before a risky edit, so leaving it memory-only defeats the point). A target name already taken, in memory or on disk, is refused with nothing touched, unless if_exists='return' hands back an existing asset of the source's class with created=false; a destination outside the project's writable content (engine content, a read-only root, no mounted root) is refused, and so is a source that is not of expected_class when one is given. By default (remap_internal_refs absent/false) this is otherwise a plain copy: like EditorAssetLibrary.duplicate_asset, any internal references the copy holds still point at the ORIGINAL (not remapped). Set remap_internal_refs=true to run the SAME 2nd-pass reference remap that duplicate_directory uses (FArchiveReplaceObjectRef mapping original->copy) so the copy's own self-references point at the copy, before that save. repoint_parents_within additionally re-applies a MaterialInstance parent via SetParentEditorOnly when the parent maps into the copied set (for a single asset this only fires if the copy's parent is the original itself).
//   @pack assets
//   @param asset_path - Full asset path to duplicate (e.g. /Game/Assets/MyAsset); engine content such as /Engine/BasicShapes/Cube can be copied into the project with destination_path
//   @param new_asset_name - Name for the duplicated asset
//   @param destination_path - Optional folder for the copy, a long package path (/Game/MyMeshes, a project plugin mount) or a folder relative to /Game; default the source asset's own folder
//   @param if_exists - Optional "error" (default: a taken target name is refused) or "return" (an asset of the source's class already at the target is returned untouched with created=false; one of another class is still refused)
//   @param expected_class - Optional class the SOURCE asset must be, or a list of classes it may be any one of, short names ("SkeletalMesh") or /Script/ paths; a Blueprint also passes when its generated class derives from one. Any other source is refused and nothing is duplicated; a name that resolves to no class is an error.
//   @param remap_internal_refs - Optional bool (default false). When true, remap the copy's internal refs (original->copy) before the save; adds { new_path, refs_remapped } to the result.
//   @param repoint_parents_within - Optional bool (default false). When true (and remap_internal_refs true), also re-apply MaterialInstanceConstant parents that map into the copied set via SetParentEditorOnly.
//   @returns { original_asset_path: string, new_asset_name: string, new_asset_path: string, destination_path: string, created: bool, duplication_success: bool, duplication_message: string, new_asset_class: string, new_package_name: string, asset_verified: bool, new_file_size_bytes: number?, new_file_size_mb: number?, original_asset_class: string, original_package_name: string, new_path: string?, refs_remapped: int?, saved: bool?, save_error: string? }

TSharedPtr<FJsonObject> HandleDuplicateAsset(const TSharedPtr<FJsonObject>& Params)
{
    FString SourcePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_path"), SourcePath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString CopyName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("new_asset_name"), CopyName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString DestinationPath;
    Params->TryGetStringField(TEXT("destination_path"), DestinationPath);
    FString IfExists = TEXT("error");
    Params->TryGetStringField(TEXT("if_exists"), IfExists);
    const bool bReturnExisting = IfExists.Equals(TEXT("return"), ESearchCase::IgnoreCase);
    if (!bReturnExisting && !IfExists.Equals(TEXT("error"), ESearchCase::IgnoreCase))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("duplicate_asset: if_exists must be \"error\" or \"return\", got \"%s\"; nothing was duplicated"), *IfExists));
    }
    TArray<UClass*> ExpectedClasses;
    if (TSharedPtr<FJsonObject> Invalid = UEAgentManagementCommandsImpl::ResolveExpectedClasses(
            TEXT("duplicate_asset"), Params->TryGetField(TEXT("expected_class")), ExpectedClasses))
    {
        return Invalid;
    }

    // A bare package path ("/Game/Foo/DT_X") and a full object path ("/Game/Foo/DT_X.DT_X")
    // are both accepted.
    SourcePath = UEAgentManagementCommandsImpl::ToObjectPath(SourcePath);

    const FAssetData Source = mcp_common::FindAssetDataByPath(SourcePath);
    if (!Source.IsValid())
    {
        return AssetCommandHelpers::AssetNotFoundError(SourcePath);
    }
    UObject* Original = Source.GetAsset();
    if (Original == nullptr)
    {
        return UEAgentManagementCommandsImpl::AssetLoadFailedReply(SourcePath);
    }
    if (ExpectedClasses.Num() > 0 && !UEAgentManagementCommandsImpl::MatchesExpectedClass(*Original, ExpectedClasses))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("duplicate_asset: the source %s; nothing was duplicated"),
            *UEAgentManagementCommandsImpl::WrongClassNote(*Original, ExpectedClasses)));
    }

    // The copy is created in destination_path, or next to the original, under the new name. A destination is a long
    // package path used as given, or a folder taken under /Game.
    while (DestinationPath.EndsWith(TEXT("/")))
    {
        DestinationPath.LeftChopInline(1);
    }
    if (!DestinationPath.IsEmpty() && !DestinationPath.StartsWith(TEXT("/")))
    {
        DestinationPath = TEXT("/Game/") + DestinationPath;
    }
    const FString Folder = DestinationPath.IsEmpty() ? FPaths::GetPath(Source.PackageName.ToString()) : DestinationPath;
    const FString CopyPackageName = Folder / CopyName;
    const FString CopyObjectPath = CopyPackageName + TEXT(".") + CopyName;
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(CopyPackageName)) { return Invalid; }
    const FString DestinationBlocker = UEAgentManagementCommandsImpl::PackageWriteBlocker(CopyPackageName);
    if (!DestinationBlocker.IsEmpty())
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("duplicate_asset: cannot write the copy, %s; nothing was ")
            TEXT("duplicated. Pick a destination_path in the project's content, e.g. /Game/MyGame."), *DestinationBlocker));
    }

    // A target name already taken, in memory or on disk, whatever its class: duplicating over it would replace a live
    // object in place, under the editors and references that still hold it. if_exists='return' hands back an asset of
    // the source's class instead.
    UObject* Existing = FindObject<UObject>(nullptr, *CopyObjectPath);
    const bool bTaken = Existing != nullptr
        || IAssetRegistry::GetChecked().GetAssetByObjectPath(FSoftObjectPath(CopyObjectPath), /*bIncludeOnlyOnDiskAssets*/ true).IsValid();
    if (bTaken)
    {
        if (bReturnExisting && !Existing)
        {
            Existing = StaticLoadObject(UObject::StaticClass(), nullptr, *CopyObjectPath);
        }
        if (!bReturnExisting || !Existing || !Existing->IsA(Original->GetClass()))
        {
            const FString How = !bReturnExisting
                ? FString(TEXT(" (if_exists='return' returns an asset of the source's class untouched)"))
                : FString::Printf(TEXT(" as a %s, not a %s"),
                    Existing ? *Existing->GetClass()->GetName() : TEXT("asset that does not load"), *Original->GetClass()->GetName());
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("duplicate_asset: '%s' already exists%s; nothing was changed. ")
                TEXT("Choose another new_asset_name or destination_path, or delete or rename the existing asset first."),
                *CopyObjectPath, *How));
        }
        TSharedPtr<FJsonObject> Kept = MakeShared<FJsonObject>();
        Kept->SetStringField(TEXT("original_asset_path"), SourcePath);
        Kept->SetStringField(TEXT("new_asset_name"), CopyName);
        Kept->SetStringField(TEXT("new_asset_path"), CopyObjectPath);
        Kept->SetStringField(TEXT("destination_path"), Folder);
        Kept->SetBoolField(TEXT("created"), false);
        Kept->SetBoolField(TEXT("duplication_success"), false);
        Kept->SetStringField(TEXT("duplication_message"),
            TEXT("An asset of the source's class already exists at the target; returned untouched (if_exists='return')"));
        Kept->SetStringField(TEXT("new_asset_class"), Existing->GetClass()->GetPathName());
        Kept->SetStringField(TEXT("new_package_name"), Existing->GetPackage()->GetName());
        Kept->SetBoolField(TEXT("asset_verified"), true);
        UEAgentManagementCommandsImpl::AddOriginalAssetInfo(Kept, Source);
        return Kept;
    }

    // The copy's package joins the folder to new_asset_name, and DuplicateAsset aborts the editor on one past the FName
    // limit, or on the path of an object it copies under the copy's object path: the original's deepest object is
    // measured (a map's actors are packaged after its package name too, which the floor covers).
    if (const FString TooLong = AssetCommandHelpers::CopiedObjectPathsTooLong(Original, CopyObjectPath); !TooLong.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TooLong);
    }
    UObject* Copy = UEAgentManagementCommandsImpl::AssetTools().DuplicateAsset(CopyName, Folder, Original);

    TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
    Report->SetStringField(TEXT("original_asset_path"), SourcePath);
    Report->SetStringField(TEXT("new_asset_name"), CopyName);
    Report->SetStringField(TEXT("new_asset_path"), CopyObjectPath);
    Report->SetStringField(TEXT("destination_path"), Folder);
    Report->SetBoolField(TEXT("created"), Copy != nullptr);
    const FAssetData Landed = UEAgentManagementCommandsImpl::AddOperationOutcome(Report, TEXT("duplication"),
        Copy != nullptr, CopyObjectPath, TEXT("Asset duplication completed successfully"), TEXT("Failed to duplicate asset"));
    // Whether the registry resolves the copy at its new path (always false after a failure).
    Report->SetBoolField(TEXT("asset_verified"), Landed.IsValid());

    if (Copy != nullptr)
    {
        // Optional second pass that rewrites the copy's own references original->copy (the
        // same pass duplicate_directory runs), BEFORE the save below so the persisted copy is
        // the remapped one. new_path / refs_remapped only appear on this path, so callers that
        // do not pass the flag keep the plain-copy response shape.
        bool bRemapInternalRefs = false;
        Params->TryGetBoolField(TEXT("remap_internal_refs"), bRemapInternalRefs);
        if (bRemapInternalRefs)
        {
            bool bRepointParentsWithin = false;
            Params->TryGetBoolField(TEXT("repoint_parents_within"), bRepointParentsWithin);

            TMap<UObject*, UObject*> ReplacementMap;
            ReplacementMap.Add(Original, Copy);
            TArray<UObject*> NewObjects;
            NewObjects.Add(Copy);
            const int32 RefsRemapped = RemapReferencesAmongCopies(ReplacementMap, NewObjects, bRepointParentsWithin);

            Report->SetNumberField(TEXT("refs_remapped"), RefsRemapped);
            Report->SetStringField(TEXT("new_path"), Copy->GetPathName());
        }

        // IAssetTools::DuplicateAsset only creates the copy IN MEMORY, and a duplicate is
        // very often made AS a safety net before a risky edit -- so it is persisted right away,
        // unconditionally, UNLESS PIE is active (the same policy as create_blueprint).
        //
        // With PIE active the save is skipped ENTIRELY, never attempted and never silently:
        // saved:false plus save_error tell the caller to run save_asset once PIE has stopped.
        // SaveAssetPackage would refuse during PIE as well; this branch stays because its
        // message names the asset left in memory, which is what has to be saved later. Note
        // that duplicate_asset is NOT in McpPieSafety.gen.h (it neither compiles nor saves by
        // itself), so the dispatcher does not stop it: this is the only guard it sees.
        if (mcp_common::IsPieActive())
        {
            mcp_common::AddSaveOutcome(Report, /*bSaved=*/false, FString::Printf(
                TEXT("Play-In-Editor is active; duplicate_asset left '%s' in memory only (unsaved) rather than ")
                TEXT("stopping a live PIE session to save it. Call save_asset on it explicitly once PIE has stopped."),
                *CopyObjectPath));
        }
        else
        {
            FString SaveError;
            const bool bSaved = mcp_common::SaveAssetPackage(Copy->GetPackage(), Copy, SaveError);
            mcp_common::AddSaveOutcome(Report, bSaved, SaveError);
        }
    }

    UEAgentManagementCommandsImpl::AddOriginalAssetInfo(Report, Source);
    return Report;
}

// MCP:duplicate_directory - Recursively copy a content folder to a new path, REMAPPING references among the copied assets so the new subtree points at its own copies (true graph isolation) while references to assets OUTSIDE the set stay shared with the originals (the "copy what you modify, share the rest" policy). Unlike EditorAssetLibrary.DuplicateDirectory / duplicate_asset (which copy without remapping, so copies keep pointing at the originals), this duplicates every asset then runs a second reference-replacement pass with the full old->new map, and saves the copies. Intended for material/instance/texture/data-asset libraries; Blueprints may need a recompile and maps (UWorld) are not handled specially.
//   @param source_path - Source content folder, e.g. /Game/VendorPack/Environment/AutoLandscape
//   @param dest_path - Destination content folder, e.g. /Game/MyGame/Environment/AutoLandscape
//   @returns { source_path: string, dest_path: string, source_asset_count: int, duplicated_count: int, remapped: bool, new_asset_paths: [string] }
TSharedPtr<FJsonObject> HandleDuplicateDirectory(const TSharedPtr<FJsonObject>& Params)
{
    FString SourcePath;
    FString DestPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("source_path"), SourcePath)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("dest_path"), DestPath)) { return Missing; }
    // Normalize: strip a single trailing slash so the prefix math below is exact.
    SourcePath.RemoveFromEnd(TEXT("/"));
    DestPath.RemoveFromEnd(TEXT("/"));
    if (!SourcePath.StartsWith(TEXT("/")) || !DestPath.StartsWith(TEXT("/")))
    {
        return mcp_common::MakeErrorReply(TEXT("source_path and dest_path must be content paths (e.g. /Game/...)"));
    }
    if (SourcePath.Equals(DestPath, ESearchCase::IgnoreCase))
    {
        return mcp_common::MakeErrorReply(TEXT("dest_path must differ from source_path"));
    }
    if (DestPath.StartsWith(SourcePath + TEXT("/")))
    {
        return mcp_common::MakeErrorReply(TEXT("dest_path must not be nested inside source_path"));
    }

    // Enumerate every asset under the source folder (recursive).
    TArray<FAssetData> SourceAssets;
    mcp_common::AssetRegistry().GetAssetsByPath(FName(*SourcePath), SourceAssets, /*bRecursive=*/true, /*bIncludeOnlyOnDiskAssets=*/false);
    if (SourceAssets.Num() == 0)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("No assets found under source_path: %s"), *SourcePath));
    }

    // Each copy's package joins dest_path to the asset's path under source_path, and DuplicateAsset aborts the editor on
    // one past the FName limit, or on the path of an object it copies under the copy's object path: the whole set is
    // checked before anything is copied, each asset by its measured deepest object (pass 1 loads them all anyway).
    for (const FAssetData& AD : SourceAssets)
    {
        const FString SrcPkg = AD.PackageName.ToString();
        if (!SrcPkg.StartsWith(SourcePath)) { continue; }
        const FString DestPkg = DestPath + SrcPkg.Mid(SourcePath.Len());
        if (const FString TooLong = AssetCommandHelpers::CopiedObjectPathsTooLong(AD.GetAsset(),
                DestPkg + TEXT(".") + AD.AssetName.ToString()); !TooLong.IsEmpty())
        {
            return mcp_common::MakeErrorReply(TooLong);
        }
    }

    FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
    IAssetTools& AssetTools = AssetToolsModule.Get();

    // Pass 1: duplicate each asset into the mirrored dest folder; record old -> new.
    TMap<UObject*, UObject*> ReplacementMap;
    TArray<UObject*> NewObjects;
    for (const FAssetData& AD : SourceAssets)
    {
        UObject* SrcObj = AD.GetAsset();
        if (!SrcObj) { continue; }
        const FString SrcPkg = AD.PackageName.ToString();       // /Game/VendorPack/Foo/Bar/A
        if (!SrcPkg.StartsWith(SourcePath)) { continue; }
        const FString RelPkg = SrcPkg.Mid(SourcePath.Len());    // /Bar/A
        const FString DestPkg = DestPath + RelPkg;              // /Game/MyGame/Foo/Bar/A
        const FString DestFolder = FPaths::GetPath(DestPkg);    // /Game/MyGame/Foo/Bar
        const FString DestName = AD.AssetName.ToString();       // A
        UObject* Dup = AssetTools.DuplicateAsset(DestName, DestFolder, SrcObj);
        if (Dup)
        {
            ReplacementMap.Add(SrcObj, Dup);
            NewObjects.Add(Dup);
        }
    }

    // Pass 2: remap references AMONG the copied set so copies point at copies. Refs to
    // assets OUTSIDE the set are left untouched -> shared read-only with the originals.
    // (Shared with duplicate_asset via RemapReferencesAmongCopies; the archive pass already
    // catches MaterialInstance Parent pointers, so no explicit parent repoint is needed here.)
    RemapReferencesAmongCopies(ReplacementMap, NewObjects, /*bRepointMaterialInstanceParents=*/false);

    // Persist the copies (DuplicateAsset leaves them in-memory unless source control forces a save).
    UEditorAssetLibrary::SaveLoadedAssets(NewObjects, /*bOnlyIfIsDirty=*/false);

    TArray<TSharedPtr<FJsonValue>> NewPaths;
    for (UObject* Dup : NewObjects)
    {
        NewPaths.Add(MakeShared<FJsonValueString>(Dup->GetPathName()));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("source_path"), SourcePath);
    ResultObj->SetStringField(TEXT("dest_path"), DestPath);
    ResultObj->SetNumberField(TEXT("source_asset_count"), SourceAssets.Num());
    ResultObj->SetNumberField(TEXT("duplicated_count"), NewObjects.Num());
    ResultObj->SetBoolField(TEXT("remapped"), true);
    ResultObj->SetArrayField(TEXT("new_asset_paths"), NewPaths);
    return ResultObj;
}

// MCP:rename_asset - Renames an asset to a new name in its current directory. GUARDED (both guards are shared with move_asset, and both come from the AssetRenameManager source -- see the comment block on AnyNativeCdoHardReferences): (1) refuses to touch anything under a read-only root (readonly_roots, default: the project's Read Only Content Roots -- the upstream tree is never mutated in place); (2) refuses BEFORE calling the engine when a native class' CDO hard-references the asset, because FAssetRenameManager would open an FMessageDialog there and block the game thread (which kills the MCP session). And it does not trust IAssetTools::RenameAssets' return value: the result is READ BACK (new object resolves + new package on disk + redirector left at the original path), because the engine silently drops packages that failed to save unless UE source control is connected (with plain git it never is).
//   @param asset_path - Full asset path to rename
//   @param new_asset_name - New name for the asset
//   @param readonly_roots [array] - Optional array of content roots that must never be written. Absent => the project's Read Only Content Roots. An EXPLICIT [] disables the guard. (Read through mcp_common::ParseReadonlyRoots, so the type hint is what puts it in the catalog.)
//   @param check_cdo_references - Optional bool (default true). Scan native CDOs for a hard reference to the asset and refuse instead of letting the engine open a modal. Set false to skip the scan (faster, but a modal can then hang the editor).
//   @returns { original_asset_path: string, new_asset_name: string, new_asset_path: string, rename_success: bool, rename_message: string, new_asset_class: string, new_package_name: string, asset_verified: bool, new_file_size_bytes: number?, new_file_size_mb: number?, original_file_exists: bool?, original_asset_class: string, original_package_name: string, new_object_resolves: bool?, new_file_on_disk: bool?, new_file_path: string?, redirector_left: bool?, verification: object?, verification_checks: [object]?, would_block_on_modal: bool?, cdo_referencing_class: string?, cdo_soft_reference_check: string? }

TSharedPtr<FJsonObject> HandleRenameAsset(const TSharedPtr<FJsonObject>& Params)
{
    FString SourcePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_path"), SourcePath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString NewName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("new_asset_name"), NewName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    // A bare package path and a full object path are both accepted.
    SourcePath = UEAgentManagementCommandsImpl::ToObjectPath(SourcePath);

    const FAssetData Source = mcp_common::FindAssetDataByPath(SourcePath);
    if (!Source.IsValid())
    {
        return AssetCommandHelpers::AssetNotFoundError(SourcePath);
    }

    // read-only root guard, BEFORE loading anything (a refusal must not even page the
    // upstream asset in). Same parse/semantics as fixup_redirectors via the shared helper:
    // absent field => the project's Read Only Content Roots; an explicit [] disables the guard.
    const TArray<FString> ReadOnlyRoots = mcp_common::ParseReadonlyRoots(Params);
    const FString SourcePackageName = Source.PackageName.ToString();
    if (mcp_common::IsUnderReadOnlyRoot(SourcePackageName, ReadOnlyRoots))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("rename_asset refused: '%s' is under a read-only root (%s). Renaming an upstream asset ")
            TEXT("mutates the vendored tree in place. Fork it to your own tree instead, or pass readonly_roots:[] to override."),
            *SourcePackageName, *FString::Join(ReadOnlyRoots, TEXT(", "))));
    }

    UObject* Original = Source.GetAsset();
    if (Original == nullptr)
    {
        return UEAgentManagementCommandsImpl::AssetLoadFailedReply(SourcePath);
    }

    // Modal pre-check (see AnyNativeCdoHardReferences). Refuse rather than let
    // FAssetRenameManager open the "referenced by a CDO" dialog on the game thread.
    bool bCheckCdoReferences = true;
    Params->TryGetBoolField(TEXT("check_cdo_references"), bCheckCdoReferences);
    if (bCheckCdoReferences)
    {
        FString ReferencingClass;
        if (AnyNativeCdoHardReferences(Original, ReferencingClass))
        {
            TSharedPtr<FJsonObject> Refused = MakeShared<FJsonObject>();
            Refused->SetStringField(TEXT("original_asset_path"), SourcePath);
            Refused->SetStringField(TEXT("new_asset_name"), NewName);
            Refused->SetBoolField(TEXT("rename_success"), false);
            Refused->SetBoolField(TEXT("would_block_on_modal"), true);
            Refused->SetStringField(TEXT("cdo_referencing_class"), ReferencingClass);
            Refused->SetStringField(TEXT("rename_message"), FString::Printf(
                TEXT("Refused without renaming: the CDO of '%s' hard-references this asset, so ")
                TEXT("FAssetRenameManager would open a modal dialog and block the editor's game thread. ")
                TEXT("Clear that reference first, or pass check_cdo_references:false to accept the risk."),
                *ReferencingClass));
            mcp_common::AddVerificationEvidence(Refused, mcp_common::EMcpVerification::Verified,
                TEXT("native CDO hard-reference scan (pre-flight, nothing was renamed)"),
                FString::Printf(TEXT("first referencing class: %s"), *ReferencingClass));
            return Refused;
        }
    }

    // Same folder, new name. FAssetRenameData takes that folder (no package name in it) and
    // the new asset name; IAssetTools::RenameAssets does the work.
    const FString OriginalPackage = Source.PackageName.ToString();
    const FString Folder = FPaths::GetPath(OriginalPackage);
    const FString NewPackage = Folder / NewName;
    const FString NewObjectPath = NewPackage + TEXT(".") + NewName;
    // The new package joins the folder to new_asset_name, and the rename aborts the editor on one past the FName limit,
    // or on the new path of an object it carries under the new object path: the asset's deepest object is measured (a
    // map's actors are repackaged after its package name too, which the floor covers).
    if (const FString TooLong = AssetCommandHelpers::CopiedObjectPathsTooLong(Original, NewObjectPath); !TooLong.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TooLong);
    }

    TArray<FAssetRenameData> Renames;
    Renames.Emplace(MakeWeakObjectPtr(Original), Folder, NewName);
    const bool bRenamed = UEAgentManagementCommandsImpl::AssetTools().RenameAssets(Renames);

    TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
    Report->SetStringField(TEXT("original_asset_path"), SourcePath);
    Report->SetStringField(TEXT("new_asset_name"), NewName);
    Report->SetStringField(TEXT("new_asset_path"), NewObjectPath);
    const FAssetData Landed = UEAgentManagementCommandsImpl::AddOperationOutcome(Report, TEXT("rename"),
        bRenamed, NewObjectPath, TEXT("Asset rename completed successfully"), TEXT("Failed to rename asset"));
    Report->SetBoolField(TEXT("asset_verified"), Landed.IsValid());
    if (bRenamed)
    {
        UEAgentManagementCommandsImpl::AddOriginalFileExists(Report, OriginalPackage);
    }

    // per-item readback, run on BOTH paths on purpose. RenameAssets' bool is not the
    // outcome (see VerifyRenameOutcome): a `true` can still leave nothing on disk, and after a
    // `false` the caller still needs to know whether the asset moved, stayed, or half-moved.
    const bool bVerified = VerifyRenameOutcome(Report, SourcePath, NewObjectPath, NewPackage);
    if (bRenamed && !bVerified)
    {
        AppendUnverifiedWarning(Report, TEXT("rename_message"));
    }
    if (bCheckCdoReferences)
    {
        // Say which half of the engine's modal condition was NOT checked, instead of implying
        // the pre-check was exhaustive.
        Report->SetStringField(TEXT("cdo_soft_reference_check"),
            TEXT("not_performed: the soft-reference half of the engine's modal condition needs ")
            TEXT("FSoftObjectPathRenameSerializer, which is private to the AssetTools module"));
    }

    UEAgentManagementCommandsImpl::AddOriginalAssetInfo(Report, Source);
    return Report;
}

// MCP:move_asset - Moves an asset to a new folder while keeping its name. GUARDED exactly like rename_asset (it reaches the same FAssetRenameManager through UEditorAssetLibrary::RenameAsset): (1) read-only roots are refused -- and here BOTH ends are checked, source and destination, since a move can also drop an asset INTO the upstream tree; (2) a native CDO hard-reference makes it refuse before the engine can open its blocking modal; (3) the outcome is read back from memory+disk instead of trusting the returned bool, which stays true even when the new package failed to save (the engine only reacts to that when UE source control is connected).
//   @param asset_path - Full asset path to move
//   @param new_folder_path - Destination folder path (e.g. /Game/NewFolder)
//   @param readonly_roots [array] - Optional array of content roots that must never be written (checked on source AND destination). Absent => the project's Read Only Content Roots. An EXPLICIT [] disables the guard. (Read through mcp_common::ParseReadonlyRoots, so the type hint is what puts it in the catalog.)
//   @param check_cdo_references - Optional bool (default true). Scan native CDOs for a hard reference to the asset and refuse instead of letting the engine open a modal.
//   @returns { original_asset_path: string, new_folder_path: string, new_asset_path: string, move_success: bool, move_message: string, new_asset_class: string, new_package_name: string, asset_verified: bool, new_file_size_bytes: number?, new_file_size_mb: number?, original_file_exists: bool?, original_asset_class: string, original_package_name: string, new_object_resolves: bool?, new_file_on_disk: bool?, new_file_path: string?, redirector_left: bool?, verification: object?, verification_checks: [object]?, would_block_on_modal: bool?, cdo_referencing_class: string?, cdo_soft_reference_check: string? }

TSharedPtr<FJsonObject> HandleMoveAsset(const TSharedPtr<FJsonObject>& Params)
{
    FString SourcePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_path"), SourcePath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString TargetFolder;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("new_folder_path"), TargetFolder, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    // A bare package path and a full object path are both accepted.
    SourcePath = UEAgentManagementCommandsImpl::ToObjectPath(SourcePath);

    const FAssetData Source = mcp_common::FindAssetDataByPath(SourcePath);
    if (!Source.IsValid())
    {
        return AssetCommandHelpers::AssetNotFoundError(SourcePath);
    }

    // read-only root guard on BOTH ends, before loading anything. A move differs from a
    // rename in that it can also push an asset INTO the protected tree, so the destination is
    // checked too. Same shared parse as fixup_redirectors; an explicit [] disables the guard.
    const TArray<FString> ReadOnlyRoots = mcp_common::ParseReadonlyRoots(Params);
    const FString SourcePackageName = Source.PackageName.ToString();
    const FString DestPackageName = TargetFolder / FPaths::GetBaseFilename(SourcePackageName);
    // The new package joins new_folder_path to the asset's name, and the readback of the move aborts the editor on one
    // past the FName limit; the move itself aborts on the new path of an object it moves (named after the new object
    // path; a map's actors are repackaged after its package name too, which that limit covers). The asset's measured
    // depth is checked once it is loaded, below.
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(DestPackageName)) { return Invalid; }
    const bool bSourceProtected = mcp_common::IsUnderReadOnlyRoot(SourcePackageName, ReadOnlyRoots);
    const bool bDestProtected = mcp_common::IsUnderReadOnlyRoot(DestPackageName, ReadOnlyRoots);
    if (bSourceProtected || bDestProtected)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("move_asset refused: %s is under a read-only root (%s). The upstream tree is neither ")
            TEXT("moved out of nor written into. Pass readonly_roots:[] to override."),
            bSourceProtected
                ? (bDestProtected ? TEXT("both the source and the destination") : TEXT("the source"))
                : TEXT("the destination"),
            *FString::Join(ReadOnlyRoots, TEXT(", "))));
    }

    UObject* Original = Source.GetAsset();
    if (Original == nullptr)
    {
        return UEAgentManagementCommandsImpl::AssetLoadFailedReply(SourcePath);
    }
    // The move carries the asset's objects under the new object path: its deepest object, measured now that it is
    // loaded, must stay under the FName limit there.
    if (const FString TooLong = AssetCommandHelpers::CopiedObjectPathsTooLong(Original,
            DestPackageName + TEXT(".") + Source.AssetName.ToString()); !TooLong.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TooLong);
    }

    // Modal pre-check -- identical to rename_asset's (same engine code path underneath).
    bool bCheckCdoReferences = true;
    Params->TryGetBoolField(TEXT("check_cdo_references"), bCheckCdoReferences);
    if (bCheckCdoReferences)
    {
        FString ReferencingClass;
        if (AnyNativeCdoHardReferences(Original, ReferencingClass))
        {
            TSharedPtr<FJsonObject> Refused = MakeShared<FJsonObject>();
            Refused->SetStringField(TEXT("original_asset_path"), SourcePath);
            Refused->SetStringField(TEXT("new_folder_path"), TargetFolder);
            Refused->SetBoolField(TEXT("move_success"), false);
            Refused->SetBoolField(TEXT("would_block_on_modal"), true);
            Refused->SetStringField(TEXT("cdo_referencing_class"), ReferencingClass);
            Refused->SetStringField(TEXT("move_message"), FString::Printf(
                TEXT("Refused without moving: the CDO of '%s' hard-references this asset, so ")
                TEXT("FAssetRenameManager would open a modal dialog and block the editor's game thread. ")
                TEXT("Clear that reference first, or pass check_cdo_references:false to accept the risk."),
                *ReferencingClass));
            mcp_common::AddVerificationEvidence(Refused, mcp_common::EMcpVerification::Verified,
                TEXT("native CDO hard-reference scan (pre-flight, nothing was moved)"),
                FString::Printf(TEXT("first referencing class: %s"), *ReferencingClass));
            return Refused;
        }
    }

    // The asset keeps its name and changes folder.
    const FString OriginalPackage = Source.PackageName.ToString();
    const FString AssetName = FPaths::GetBaseFilename(OriginalPackage);
    const FString NewPackage = TargetFolder / AssetName;
    const FString NewObjectPath = NewPackage + TEXT(".") + AssetName;

    // There is no IAssetTools::MoveAssets: UEditorAssetLibrary::RenameAsset performs
    // the move, and ITS return value is the outcome (a hardcoded true would report failed
    // moves as move_success:true).
    const FString FromPath = Source.GetObjectPathString();
    const FString ToPath = TargetFolder + TEXT("/") + FPaths::GetCleanFilename(FromPath);
    const bool bMoved = UEditorAssetLibrary::RenameAsset(FromPath, ToPath);

    TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
    Report->SetStringField(TEXT("original_asset_path"), SourcePath);
    Report->SetStringField(TEXT("new_folder_path"), TargetFolder);
    Report->SetStringField(TEXT("new_asset_path"), NewObjectPath);
    const FAssetData Landed = UEAgentManagementCommandsImpl::AddOperationOutcome(Report, TEXT("move"),
        bMoved, NewObjectPath, TEXT("Asset move completed successfully"), TEXT("Failed to move asset"));
    Report->SetBoolField(TEXT("asset_verified"), Landed.IsValid());
    if (bMoved)
    {
        UEAgentManagementCommandsImpl::AddOriginalFileExists(Report, OriginalPackage);
    }

    // per-item readback on BOTH paths -- same reasoning as rename_asset.
    const bool bVerified = VerifyRenameOutcome(Report, SourcePath, NewObjectPath, NewPackage);
    if (bMoved && !bVerified)
    {
        AppendUnverifiedWarning(Report, TEXT("move_message"));
    }
    if (bCheckCdoReferences)
    {
        Report->SetStringField(TEXT("cdo_soft_reference_check"),
            TEXT("not_performed: the soft-reference half of the engine's modal condition needs ")
            TEXT("FSoftObjectPathRenameSerializer, which is private to the AssetTools module"));
    }

    UEAgentManagementCommandsImpl::AddOriginalAssetInfo(Report, Source);
    return Report;
}

// MCP:delete_asset - Force-delete one asset with no confirmation or undo: references to it are nulled, the object is purged and its .uasset/.umap file removed from disk. Check get_asset_referencers first. Succeeds only when the file is gone from disk and the object from memory: a file left behind (read-only, or held open by another process) fails the call with file_still_exists true, and the asset comes back when the editor restarts. With expected_class it is a typed delete: an asset of any other class is refused and nothing is deleted, and a path with no asset is a no-op success.
//   @pack assets
//   @param asset_path - Asset to delete, as an object path ("/Game/Props/SM_Box.SM_Box") or a bare package path ("/Game/Props/SM_Box")
//   @param expected_class - Optional class the asset must be, or a list of classes it may be any one of, as short names ("AnimSequence") or /Script/ paths ("/Script/Engine.StaticMesh"). A Blueprint asset also passes when its generated class derives from one ("Subsystem" accepts a subsystem Blueprint). Another class is refused with success false, deleted false, expected_class_matched false, its asset_class (and generated_class_name for a Blueprint) and a note, and nothing is deleted. With it, a path that names no asset is a no-op: success, deleted false, already_existed false. A name that resolves to no class is an error.
//   @returns { asset_path: string, object_path: string, asset_class: string (class path), asset_class_name: string (short class name of the asset), generated_class_name?: string, expected_class_matched?: bool, package_name: string, original_file_size_bytes: number, original_file_size_mb: number, delete_success: bool, delete_message: string, asset_verified_deleted: bool, file_still_exists: bool, deleted: bool, already_existed: bool, note?: string, deletion_timestamp: string? }

TSharedPtr<FJsonObject> HandleDeleteAsset(const TSharedPtr<FJsonObject>& Params)
{
    // The asset to delete: a full object path or a bare package path, non-empty.
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_path"), AssetPath)) { return Missing; }

    // The optional type guard, resolved before anything is looked up: a bad value refuses the call untouched.
    TArray<UClass*> ExpectedClasses;
    if (TSharedPtr<FJsonObject> Invalid = UEAgentManagementCommandsImpl::ResolveExpectedClasses(
            TEXT("delete_asset"), Params->TryGetField(TEXT("expected_class")), ExpectedClasses))
    {
        return Invalid;
    }
    const bool bTyped = ExpectedClasses.Num() > 0;

    // Resolve the asset by DIRECT LOAD, not via the AssetRegistry. Two reasons:
    //   1. GetAssetByObjectPath(FSoftObjectPath("/Game/X/Foo")) with a BARE
    //      package path yields an empty asset name and never matches, so maps
    //      returned by create_world_partition_map (which echoes a bare package
    //      path) would be "Asset not found" and never deleted.
    //   2. The registry is unreliable in headless for assets created/saved earlier
    //      this session.
    // StaticLoadObject handles the bare path natively (the engine appends
    // ".<ShortName>") and finds in-session in-memory objects (e.g. the just-created
    // UWorld) without touching the registry at all.
    //
    // Probe side-effect-free BEFORE ever calling StaticLoadObject on a
    // path that might not exist. StaticLoadObject's internal ResolveName2 runs with
    // Create=true, which unconditionally CreatePackage()s (and leaves the resulting
    // empty UPackage resident) when LoadPackage fails to find the path on disk --
    // verified in UObjectGlobals.cpp. That phantom Package then poisons every later
    // create_X at the same path (their idempotent "does this already exist" check
    // finds the bare Package via StaticFindObject instead of nothing, then fails to
    // Cast it to the real type). StaticFindObject (Create=false, never creates
    // anything) plus FPackageName::DoesPackageExist (pure filesystem/registry check,
    // no side effects) together resolve the same two cases the comment above cares
    // about -- an unsaved in-memory object, or one that exists on disk but isn't
    // loaded yet -- without ever exercising StaticLoadObject's failure path.
    UObject* Asset = StaticFindObject(UObject::StaticClass(), nullptr, *AssetPath);
    if (!Asset)
    {
        const FString ProbePackageName = FPackageName::ObjectPathToPackageName(AssetPath);
        if (!FPackageName::DoesPackageExist(ProbePackageName))
        {
            if (bTyped)
            {
                // A typed delete is idempotent: nothing there to delete is a no-op, not an error.
                TSharedPtr<FJsonObject> Nothing = MakeShared<FJsonObject>();
                Nothing->SetStringField(TEXT("asset_path"), AssetPath);
                Nothing->SetStringField(TEXT("asset_class"), TEXT(""));
                Nothing->SetStringField(TEXT("asset_class_name"), TEXT(""));
                Nothing->SetBoolField(TEXT("deleted"), false);
                Nothing->SetBoolField(TEXT("already_existed"), false);
                Nothing->SetStringField(TEXT("note"), TEXT("no asset at asset_path; nothing to delete"));
                return mcp_common::MakeOkReply(Nothing);
            }
            return AssetCommandHelpers::AssetNotFoundError(AssetPath);
        }
        // On disk but not yet loaded -- safe to load now (the file exists, so this
        // will not hit the failure path that creates a phantom package).
        Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
        if (!Asset)
        {
            if (bTyped)
            {
                return mcp_common::MakeErrorReply(FString::Printf(TEXT("delete_asset: the package of '%s' is on disk but ")
                    TEXT("does not load, so its class cannot be checked against expected_class; nothing deleted"), *AssetPath));
            }
            return AssetCommandHelpers::AssetNotFoundError(AssetPath);
        }
    }

    // What the type guard judges: a bare package path of a loaded package finds the UPackage itself, so the
    // asset inside it is the subject. A Blueprint also reports the class its instances have.
    UObject* TypeSubject = Asset;
    if (const UPackage* AsPackage = Cast<UPackage>(Asset))
    {
        if (UObject* Inner = AsPackage->FindAssetInPackage())
        {
            TypeSubject = Inner;
        }
    }
    const UBlueprint* AsBlueprint = Cast<UBlueprint>(TypeSubject);
    const FString GeneratedClassName = (AsBlueprint && AsBlueprint->GeneratedClass) ? AsBlueprint->GeneratedClass->GetName() : FString();
    // Read before the delete, which can collect garbage and free the asset.
    const FString AssetClassName = TypeSubject->GetClass()->GetName();
    if (bTyped && !UEAgentManagementCommandsImpl::MatchesExpectedClass(*TypeSubject, ExpectedClasses))
    {
        const FString Note = UEAgentManagementCommandsImpl::WrongClassNote(*TypeSubject, ExpectedClasses) + TEXT(", refusing to delete");
        TSharedPtr<FJsonObject> Refused = MakeShared<FJsonObject>();
        Refused->SetStringField(TEXT("asset_path"), AssetPath);
        Refused->SetStringField(TEXT("object_path"), TypeSubject->GetPathName());
        Refused->SetStringField(TEXT("asset_class"), TypeSubject->GetClass()->GetPathName());
        Refused->SetStringField(TEXT("asset_class_name"), AssetClassName);
        if (!GeneratedClassName.IsEmpty())
        {
            Refused->SetStringField(TEXT("generated_class_name"), GeneratedClassName);
        }
        Refused->SetBoolField(TEXT("expected_class_matched"), false);
        Refused->SetBoolField(TEXT("deleted"), false);
        Refused->SetBoolField(TEXT("already_existed"), true);
        Refused->SetStringField(TEXT("note"), Note);
        return mcp_common::MakeOutcomeReply(Refused, false, FString::Printf(TEXT("delete_asset: %s at %s"), *Note, *AssetPath));
    }

    // Metadata straight from the loaded object (registry-independent).
    const FString ResolvedObjectPath = Asset->GetPathName();
    const FString PackageName = Asset->GetPackage()->GetName();
    const FString AssetClass = Asset->GetClass()->GetPathName();

    // Size on disk before the delete. DoesPackageExist yields the real filename WITH the
    // right extension (.umap for a map, .uasset otherwise).
    int64 BytesBefore = 0;
    FString OnDiskFilePath;
    if (FPackageName::DoesPackageExist(PackageName, &OnDiskFilePath))
    {
        IPlatformFile& Disk = FPlatformFileManager::Get().GetPlatformFile();
        if (Disk.FileExists(*OnDiskFilePath))
        {
            BytesBefore = Disk.FileSize(*OnDiskFilePath);
        }
    }

    // Force-delete the loaded object: nulls references, GCs it, unloads the
    // package, unregisters from the registry, and deletes the .uasset/.umap from
    // disk (ObjectTools::CleanupAfterSuccessfulDelete). This is registry-independent
    // so it works for maps the registry never surfaced. ShowConfirmation=false is
    // mandatory headless (true would open a modal dialog). The created UWorld is not
    // the editor world (create_world_partition_map never swaps it), so the delete is
    // safe (no editor-world reload path is triggered).
    const TArray<UObject*> Doomed = { Asset };
    const bool bDeleteSuccess = ObjectTools::ForceDeleteObjects(Doomed, /*bShowConfirmation=*/false) > 0;

    TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
    Report->SetStringField(TEXT("asset_path"), AssetPath);
    Report->SetStringField(TEXT("object_path"), ResolvedObjectPath);
    Report->SetStringField(TEXT("asset_class"), AssetClass);
    Report->SetStringField(TEXT("asset_class_name"), AssetClassName);
    if (!GeneratedClassName.IsEmpty())
    {
        Report->SetStringField(TEXT("generated_class_name"), GeneratedClassName);
    }
    if (bTyped)
    {
        Report->SetBoolField(TEXT("expected_class_matched"), true);
    }
    Report->SetStringField(TEXT("package_name"), PackageName);
    Report->SetNumberField(TEXT("original_file_size_bytes"), BytesBefore);
    Report->SetNumberField(TEXT("original_file_size_mb"), BytesBefore / (1024.0 * 1024.0));
    Report->SetBoolField(TEXT("delete_success"), bDeleteSuccess);
    Report->SetBoolField(TEXT("already_existed"), true);

    // Whether the file is still on disk, asked of the file system: FPackageName::DoesPackageExist
    // answers from the asset registry first, and right after a delete the registry can still list a
    // package whose file is already gone (measured: a false "still there" with no delete error logged).
    auto FileStillOnDisk = [&OnDiskFilePath]() { return !OnDiskFilePath.IsEmpty() && IFileManager::Get().FileExists(*OnDiskFilePath); };

    if (!bDeleteSuccess)
    {
        Report->SetStringField(TEXT("delete_message"), TEXT("Failed to delete asset (ForceDeleteObjects returned 0)"));
        Report->SetBoolField(TEXT("asset_verified_deleted"), false);
        Report->SetBoolField(TEXT("file_still_exists"), FileStillOnDisk());
        Report->SetBoolField(TEXT("deleted"), false);
        return mcp_common::MakeOutcomeReply(Report, false, FString::Printf(
            TEXT("delete_asset: the engine deleted nothing (ForceDeleteObjects returned 0), so '%s' is still there."), *AssetPath));
    }

    // Verified without the registry: the package file must be gone, and the object must no
    // longer be findable in memory. ForceDeleteObjects counts the purged objects whether or not
    // the file could be removed, so either one left behind is a failed delete: the file brings
    // the asset back when the editor restarts, and a loaded object can be saved back to disk.
    const bool bFileStillExists = FileStillOnDisk();
    const bool bObjectStillLoaded = StaticFindObject(UObject::StaticClass(), nullptr, *ResolvedObjectPath) != nullptr;
    FString Failure;
    if (bFileStillExists)
    {
        Failure = FString::Printf(TEXT("delete_asset: the object was purged but its file is still on disk (read-only, or held open by another ")
            TEXT("process), so '%s' comes back when the editor restarts. Free the file and delete it again."), *AssetPath);
    }
    else if (bObjectStillLoaded)
    {
        Failure = FString::Printf(TEXT("delete_asset: the file is gone but the object is still loaded (something keeps it alive, such as an ")
            TEXT("open asset editor), so a later save can write '%s' back."), *AssetPath);
    }
    Report->SetStringField(TEXT("delete_message"), Failure.IsEmpty()
        ? TEXT("Asset deleted: its file is gone and the object is no longer loaded")
        : TEXT("Asset only partly deleted"));
    Report->SetBoolField(TEXT("asset_verified_deleted"), !bObjectStillLoaded);
    Report->SetBoolField(TEXT("file_still_exists"), bFileStillExists);
    Report->SetBoolField(TEXT("deleted"), Failure.IsEmpty());
    Report->SetStringField(TEXT("deletion_timestamp"), FDateTime::Now().ToString());
    return mcp_common::MakeOutcomeReply(Report, Failure.IsEmpty(), Failure);
}

// Plugin status

// MCP:get_plugin_status - Read one plugin by name, read-only: Enabled or Disabled, version, author, category, base dir, .uplugin file, dependencies and modules. An unknown name returns plugin_status "Not Found" with "N/A" fields, not an error. plugin_loaded only mirrors plugin_enabled, and plugin_created_date holds the CreatedByURL.
//   @param plugin_name - Name of the plugin to query, as its .uplugin file is named (e.g. "UEAgent")
//   @returns { plugin_name: string, plugin_status: string, plugin_version: string, plugin_description: string, plugin_author: string, plugin_created_date: string, plugin_category: string, plugin_loaded: bool, plugin_enabled: bool, plugin_path: string, plugin_file: string, plugin_dependencies: [{ name: string, enabled: bool, optional: bool, version: string }], plugin_modules: [{ name: string, type: string, loading_phase: bool }], plugin_message: string, plugin_manager_version: string, plugin_search_paths: string, plugin_status_timestamp: string }

TSharedPtr<FJsonObject> HandleGetPluginStatus(const TSharedPtr<FJsonObject>& Params)
{
    FString Wanted;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("plugin_name"), Wanted, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    TSharedPtr<FJsonObject> Status = MakeShared<FJsonObject>();
    Status->SetStringField(TEXT("plugin_name"), Wanted);

    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(Wanted);
    if (Plugin.IsValid())
    {
        const FPluginDescriptor& Descriptor = Plugin->GetDescriptor();
        UEAgentManagementCommandsImpl::AddPluginIdentity(Status, *Plugin);
        // Kept under this key for compatibility: the descriptor's CreatedByURL.
        Status->SetStringField(TEXT("plugin_created_date"), Descriptor.CreatedByURL);
        Status->SetArrayField(TEXT("plugin_dependencies"), UEAgentManagementCommandsImpl::DescribePluginReferences(Descriptor));
        Status->SetArrayField(TEXT("plugin_modules"), UEAgentManagementCommandsImpl::DescribePluginModules(Descriptor));
        Status->SetStringField(TEXT("plugin_message"), TEXT("Plugin status retrieved successfully"));
    }
    else
    {
        // Unknown plugin: the same keys, with placeholders, and no error.
        Status->SetStringField(TEXT("plugin_status"), TEXT("Not Found"));
        static const TCHAR* const UnknownTextFields[] = {
            TEXT("plugin_version"), TEXT("plugin_description"), TEXT("plugin_author"),
            TEXT("plugin_created_date"), TEXT("plugin_category"), TEXT("plugin_path"), TEXT("plugin_file"),
        };
        for (const TCHAR* Field : UnknownTextFields)
        {
            Status->SetStringField(Field, TEXT("N/A"));
        }
        Status->SetBoolField(TEXT("plugin_loaded"), false);
        Status->SetBoolField(TEXT("plugin_enabled"), false);
        Status->SetStringField(TEXT("plugin_message"), TEXT("Plugin not found"));
    }

    Status->SetStringField(TEXT("plugin_manager_version"), TEXT("1.0.0"));
    Status->SetStringField(TEXT("plugin_search_paths"), FPaths::ProjectPluginsDir());
    Status->SetStringField(TEXT("plugin_status_timestamp"), FDateTime::Now().ToString());
    return Status;
}

// ==========================================================================
// Cross-project / fork-lineage tools.
// ==========================================================================

// MCP:run_external_project_script - Spawn a SEPARATE headless UnrealEditor-Cmd.exe to run a python script against an EXTERNAL project and capture its stdout. Needed because another project's /Game packages collide with the live project's (they cannot be loaded side by side), so reading data from content outside this project needs an isolated editor. Give it an existing .uproject OR a module_dir (a minimal engine-only .uproject is cloned in place, with any 'disable_plugins' turned off, then deleted); it launches `<uproject> -run=pythonscript -script=<script> -unattended -nullrhi -NoLiveCoding -stdout`, blocks until exit or timeout, and returns stdout. HEAVY: spawns a full editor process (tens of seconds) and BLOCKS the editor for the duration -- run with confirmation. The engine binary is resolved from THIS editor's install (FPaths::EngineDir); no path is assumed.
//   @param script - Absolute path to the .py file to run (the -script argument).
//   @param uproject - Path to an existing external .uproject to run against (optional; provide this OR module_dir).
//   @param module_dir - Directory of a content folder or project whose Content to load; a temporary engine-only .uproject is cloned here and cleaned up (optional; provide this OR uproject).
//   @param disable_plugins - Optional list of plugin names to disable in the CLONED .uproject (e.g. custom plugins that hang headless startup). Only applies when module_dir is used.
//   @param extra_args - Optional list of extra command-line tokens appended to the launch.
//   @param timeout_seconds - Max seconds to wait before terminating the child (optional, default 300; clamped 30..3600).
//   @returns { exe: string, uproject_used: string, cloned: bool, script: string, params: string, launched: bool, exit_code: int, success: bool, timed_out: bool, duration_seconds: number, stdout: string, stdout_length: int, stdout_file: string, cleaned_up: bool, message: string }
TSharedPtr<FJsonObject> HandleRunExternalProjectScript(const TSharedPtr<FJsonObject>& Params)
{
    FString ScriptPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("script"), ScriptPath)) { return Missing; }
    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    if (!PlatformFile.FileExists(*ScriptPath))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("script file not found: %s"), *ScriptPath));
    }

    FString UProjectParam, ModuleDir;
    Params->TryGetStringField(TEXT("uproject"), UProjectParam);
    Params->TryGetStringField(TEXT("module_dir"), ModuleDir);
    if (UProjectParam.IsEmpty() && ModuleDir.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Provide either 'uproject' (existing .uproject) or 'module_dir' (to clone one)"));
    }

    int32 TimeoutSeconds = 300;
    double TimeoutD = 0.0;
    if (Params->TryGetNumberField(TEXT("timeout_seconds"), TimeoutD)) { TimeoutSeconds = FMath::Clamp(static_cast<int32>(TimeoutD), 30, 3600); }

    TArray<FString> DisablePlugins;
    const TArray<TSharedPtr<FJsonValue>>* DisArr = nullptr;
    if (Params->TryGetArrayField(TEXT("disable_plugins"), DisArr) && DisArr)
    {
        for (const TSharedPtr<FJsonValue>& V : *DisArr) { FString S; if (V.IsValid() && V->TryGetString(S) && !S.IsEmpty()) { DisablePlugins.Add(S); } }
    }
    TArray<FString> ExtraArgs;
    const TArray<TSharedPtr<FJsonValue>>* ExArr = nullptr;
    if (Params->TryGetArrayField(TEXT("extra_args"), ExArr) && ExArr)
    {
        for (const TSharedPtr<FJsonValue>& V : *ExArr) { FString S; if (V.IsValid() && V->TryGetString(S) && !S.IsEmpty()) { ExtraArgs.Add(S); } }
    }

    // Resolve THIS editor's UnrealEditor-Cmd.exe (never a hardcoded engine path).
    const FString Exe = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::EngineDir(), TEXT("Binaries"), TEXT("Win64"), TEXT("UnrealEditor-Cmd.exe")));
    if (!PlatformFile.FileExists(*Exe))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("UnrealEditor-Cmd.exe not found at %s"), *Exe));
    }

    // Determine the target .uproject (existing, or cloned engine-only over module_dir).
    FString UProjectPath;
    bool bCloned = false;
    FString CloneSavedDir;      // Saved dir to remove afterwards (only if we created it)
    bool bSavedExistedBefore = false;
    if (!UProjectParam.IsEmpty())
    {
        UProjectPath = FPaths::ConvertRelativePathToFull(UProjectParam);
        if (!PlatformFile.FileExists(*UProjectPath))
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("uproject not found: %s"), *UProjectPath));
        }
    }
    else
    {
        FString ModuleFull = FPaths::ConvertRelativePathToFull(ModuleDir);
        ModuleFull.ReplaceInline(TEXT("\\"), TEXT("/"));
        ModuleFull.RemoveFromEnd(TEXT("/"));
        if (!PlatformFile.DirectoryExists(*ModuleFull))
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("module_dir not found: %s"), *ModuleFull));
        }

        // Build a minimal engine-only .uproject in the module dir (its Content is the sibling).
        TSharedPtr<FJsonObject> UProj = MakeShared<FJsonObject>();
        UProj->SetNumberField(TEXT("FileVersion"), 3);
        UProj->SetStringField(TEXT("EngineAssociation"), TEXT(""));
        UProj->SetStringField(TEXT("Category"), TEXT(""));
        UProj->SetStringField(TEXT("Description"), TEXT("Temporary engine-only project created by UEAgent run_external_project_script"));
        UProj->SetArrayField(TEXT("Modules"), TArray<TSharedPtr<FJsonValue>>());
        TArray<TSharedPtr<FJsonValue>> Plugins;
        for (const FString& P : DisablePlugins)
        {
            TSharedPtr<FJsonObject> PObj = MakeShared<FJsonObject>();
            PObj->SetStringField(TEXT("Name"), P);
            PObj->SetBoolField(TEXT("Enabled"), false);
            Plugins.Add(MakeShared<FJsonValueObject>(PObj));
        }
        UProj->SetArrayField(TEXT("Plugins"), Plugins);

        FString UProjJson;
        TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&UProjJson);
        FJsonSerializer::Serialize(UProj.ToSharedRef(), Writer);

        UProjectPath = FPaths::Combine(ModuleFull, FString::Printf(TEXT("__mcp_ext_%s.uproject"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
        if (!FFileHelper::SaveStringToFile(UProjJson, *UProjectPath))
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Failed to write cloned .uproject: %s"), *UProjectPath));
        }
        bCloned = true;
        CloneSavedDir = FPaths::Combine(ModuleFull, TEXT("Saved"));
        bSavedExistedBefore = PlatformFile.DirectoryExists(*CloneSavedDir);
    }

    // Compose the command line.
    FString CmdParams = FString::Printf(
        TEXT("\"%s\" -run=pythonscript -script=\"%s\" -unattended -nullrhi -NoLiveCoding -stdout -nopause -nosplash -nosound"),
        *UProjectPath, *ScriptPath);
    for (const FString& A : ExtraArgs) { CmdParams += TEXT(" ") + A; }

    // Launch with a captured stdout pipe. Poll so the child never blocks on a full pipe.
    void* ReadPipe = nullptr;
    void* WritePipe = nullptr;
    FPlatformProcess::CreatePipe(ReadPipe, WritePipe);

    uint32 ProcID = 0;
    const double Start = FPlatformTime::Seconds();
    FProcHandle Proc = FPlatformProcess::CreateProc(
        *Exe, *CmdParams,
        /*bLaunchDetached=*/false, /*bLaunchHidden=*/true, /*bLaunchReallyHidden=*/true,
        &ProcID, /*PriorityModifier=*/0, /*OptionalWorkingDirectory=*/nullptr,
        /*PipeWriteChild=*/WritePipe, /*PipeReadChild=*/nullptr);

    if (!Proc.IsValid())
    {
        FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
        if (bCloned) { PlatformFile.DeleteFile(*UProjectPath); }
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Failed to launch: %s %s"), *Exe, *CmdParams));
    }

    FString Output;
    bool bTimedOut = false;
    while (FPlatformProcess::IsProcRunning(Proc))
    {
        Output += FPlatformProcess::ReadPipe(ReadPipe);
        if (FPlatformTime::Seconds() - Start > static_cast<double>(TimeoutSeconds))
        {
            bTimedOut = true;
            FPlatformProcess::TerminateProc(Proc, /*KillTree=*/true);
            break;
        }
        FPlatformProcess::Sleep(0.05f);
    }
    // Drain anything left in the pipe.
    Output += FPlatformProcess::ReadPipe(ReadPipe);

    int32 ReturnCode = -1;
    FPlatformProcess::GetProcReturnCode(Proc, &ReturnCode);
    const double Duration = FPlatformTime::Seconds() - Start;

    FPlatformProcess::CloseProc(Proc);
    FPlatformProcess::ClosePipe(ReadPipe, WritePipe);

    // Persist full stdout; return a capped tail inline (errors surface at the end).
    const FString LogDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEAgent"), TEXT("ext_runs"));
    PlatformFile.CreateDirectoryTree(*LogDir);
    const FString LogFile = FPaths::Combine(LogDir, FString::Printf(TEXT("ext_run_%s.log"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"))));
    FFileHelper::SaveStringToFile(Output, *LogFile);

    // Cleanup the clone (and its freshly-created Saved) best-effort.
    bool bCleanedUp = false;
    if (bCloned)
    {
        PlatformFile.DeleteFile(*UProjectPath);
        if (!bSavedExistedBefore && !CloneSavedDir.IsEmpty() && PlatformFile.DirectoryExists(*CloneSavedDir))
        {
            PlatformFile.DeleteDirectoryRecursively(*CloneSavedDir);
        }
        bCleanedUp = true;
    }

    const int32 MaxInline = 16000;
    FString InlineOut = Output;
    if (InlineOut.Len() > MaxInline) { InlineOut = InlineOut.Right(MaxInline); }

    const bool bSuccess = !bTimedOut && ReturnCode == 0;
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("exe"), Exe);
    Result->SetStringField(TEXT("uproject_used"), UProjectPath);
    Result->SetBoolField(TEXT("cloned"), bCloned);
    Result->SetStringField(TEXT("script"), ScriptPath);
    Result->SetStringField(TEXT("params"), CmdParams);
    Result->SetBoolField(TEXT("launched"), true);
    Result->SetNumberField(TEXT("exit_code"), ReturnCode);
    Result->SetBoolField(TEXT("success"), bSuccess);
    Result->SetBoolField(TEXT("timed_out"), bTimedOut);
    Result->SetNumberField(TEXT("duration_seconds"), Duration);
    Result->SetStringField(TEXT("stdout"), InlineOut);
    Result->SetNumberField(TEXT("stdout_length"), Output.Len());
    Result->SetStringField(TEXT("stdout_file"), LogFile);
    Result->SetBoolField(TEXT("cleaned_up"), bCleanedUp);
    Result->SetStringField(TEXT("message"), bTimedOut
        ? FString::Printf(TEXT("Terminated after %d s timeout."), TimeoutSeconds)
        : FString::Printf(TEXT("Exited with code %d after %.1f s."), ReturnCode, Duration));
    return mcp_common::MakeOkReply(Result);
}

// MCP:audit_fork_refs - Audit a fork subtree for references that still point at the SOURCE tree it was copied from (duplicate_asset / duplicate_directory by script do NOT remap internal refs, so a copy keeps pointing at the original -- e.g. a MaterialInstance Parent still under /Game/VendorPack). Reports, per fork asset, the source dependencies (authoritative, from the AssetRegistry) and, best-effort, the exact PROPERTY holding each source object ref. With fix=true it FArchiveReplaceObjectRefs each source object to a same-named counterpart found inside the fork and saves the asset.
//   @param fork_root - Fork content path to audit, e.g. '/Game/MyGame/Environment/AutoLandscape'.
//   @param source_root - Origin tree the refs should not point at (optional; defaults to the project's Read Only Content Root when exactly one is configured).
//   @param include_properties - Optional bool (default true). Load each offending asset and report which property holds each source ref (best-effort; the dependency list is authoritative).
//   @param fix - Optional bool (default false). Remap source object refs to a same-named fork counterpart via FArchiveReplaceObjectRef and save the asset.
//   @param max_assets - Max fork assets to scan (optional, default 2000; clamped 1..20000).
//   @param dependency_type - Optional string, one of "hard"|"soft"|"all" (case-insensitive, default "all"). Scopes the per-asset dependency scan: "hard" surfaces only load-bearing refs into source_root (e.g. a MaterialInstance Parent, a hard object property) -- the correctness-critical case this audit exists for; "soft" surfaces only soft/lazy refs (TSoftObjectPtr, FSoftObjectPath); "all" (default) leaves the scan unscoped.
//   @returns { fork_root: string, source_root: string, fix: bool, dependency_type: string, assets_scanned: int, offending_count: int, total_source_dependencies: int, offenders: [{asset, package, source_dependencies:[string], properties:[{property, ref}], remapped:int}], fixed_assets: int, refs_remapped: int, unresolved_counterparts: [string], message: string }
TSharedPtr<FJsonObject> HandleAuditForkRefs(const TSharedPtr<FJsonObject>& Params)
{
    FString ForkRoot;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("fork_root"), ForkRoot)) { return Missing; }
    ForkRoot.RemoveFromEnd(TEXT("/"));
    // Optional: read it first and default afterwards (an early-return on a failed TryGet is what
    // the catalog codegen reads as "required", and the server would then refuse the call).
    FString SourceRoot;
    Params->TryGetStringField(TEXT("source_root"), SourceRoot);
    if (SourceRoot.IsEmpty())
    {
        // Default: the project's vendored-library root, but only when exactly one is configured --
        // with several there is no way to know which one the fork was copied from.
        const TArray<FString> LibraryRoots = mcp_common::GetDefaultReadonlyRoots();
        if (LibraryRoots.Num() != 1)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Missing parameter: source_root (the tree the fork was copied from). It defaults to the project's ")
                TEXT("read-only content root only when exactly one is configured, and %d are ")
                TEXT("(Project Settings > Plugins > UEAgent)."), LibraryRoots.Num()));
        }
        SourceRoot = LibraryRoots[0];
    }
    SourceRoot.RemoveFromEnd(TEXT("/"));

    bool bIncludeProperties = true;
    Params->TryGetBoolField(TEXT("include_properties"), bIncludeProperties);
    bool bFix = false;
    Params->TryGetBoolField(TEXT("fix"), bFix);
    int32 MaxAssets = 2000;
    double MaxAssetsD = 0.0;
    if (Params->TryGetNumberField(TEXT("max_assets"), MaxAssetsD)) { MaxAssets = FMath::Clamp(static_cast<int32>(MaxAssetsD), 1, 20000); }

    // Optional hard/soft scope on the per-asset dependency scan (see get_asset_referencers'
    // HandleGetAssetReferencers for the same pattern). Default "all": a default-constructed
    // FDependencyQuery == NoRequirements == both hard and soft.
    FString DependencyType = TEXT("all");
    Params->TryGetStringField(TEXT("dependency_type"), DependencyType);
    UE::AssetRegistry::FDependencyQuery DepQuery; // default = "all" (NoRequirements)
    if (DependencyType.Equals(TEXT("hard"), ESearchCase::IgnoreCase))
    {
        DependencyType = TEXT("hard");
        DepQuery = UE::AssetRegistry::FDependencyQuery(UE::AssetRegistry::EDependencyQuery::Hard);
    }
    else if (DependencyType.Equals(TEXT("soft"), ESearchCase::IgnoreCase))
    {
        DependencyType = TEXT("soft");
        DepQuery = UE::AssetRegistry::FDependencyQuery(UE::AssetRegistry::EDependencyQuery::Soft);
    }
    else
    {
        DependencyType = TEXT("all"); // any unrecognized value normalizes to "all" too
    }

    const FString SourcePrefix = SourceRoot + TEXT("/");

    IAssetRegistry& AssetRegistry = mcp_common::AssetRegistry();
    const UE::AssetRegistry::EDependencyCategory Cat = UE::AssetRegistry::EDependencyCategory::Package;

    TArray<FAssetData> ForkAssets;
    AssetRegistry.GetAssetsByPath(FName(*ForkRoot), ForkAssets, /*bRecursive=*/true, /*bIncludeOnlyOnDiskAssets=*/false);
    if (ForkAssets.Num() == 0)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("No assets found under fork_root: %s"), *ForkRoot));
    }

    // Build a fork counterpart index (short AssetName -> unique fork object path) for bFix.
    TMap<FString, FString> ForkByName;      // name -> object path (empty string flags "ambiguous")
    if (bFix)
    {
        for (const FAssetData& AD : ForkAssets)
        {
            const FString Name = AD.AssetName.ToString();
            const FString ObjPath = AD.GetObjectPathString();
            if (FString* Existing = ForkByName.Find(Name)) { *Existing = FString(); } // ambiguous
            else { ForkByName.Add(Name, ObjPath); }
        }
    }

    TArray<TSharedPtr<FJsonValue>> Offenders;
    TSet<FString> UnresolvedCounterparts;
    int32 Scanned = 0;
    int32 OffendingCount = 0;
    int32 TotalSourceDeps = 0;
    int32 FixedAssets = 0;
    int32 RefsRemapped = 0;

    for (const FAssetData& AD : ForkAssets)
    {
        if (Scanned >= MaxAssets) { break; }
        ++Scanned;

        const FName PkgName = AD.PackageName;
        TArray<FName> Deps;
        AssetRegistry.GetDependencies(PkgName, Deps, Cat, DepQuery);

        TArray<FString> SourceDeps;
        for (const FName& Dep : Deps)
        {
            const FString DepStr = Dep.ToString();
            if (DepStr == SourceRoot || DepStr.StartsWith(SourcePrefix)) { SourceDeps.Add(DepStr); }
        }
        if (SourceDeps.Num() == 0) { continue; }

        ++OffendingCount;
        TotalSourceDeps += SourceDeps.Num();

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("asset"), AD.GetObjectPathString());
        Entry->SetStringField(TEXT("package"), PkgName.ToString());
        Entry->SetArrayField(TEXT("source_dependencies"), mcp_common::StringsToJson(SourceDeps));

        TArray<TSharedPtr<FJsonValue>> PropsJson;
        int32 RemappedThis = 0;
        if (bIncludeProperties || bFix)
        {
            UObject* ForkObj = AD.GetAsset();
            if (ForkObj)
            {
                TMap<UObject*, UObject*> ReplacementMap;   // source obj -> fork counterpart (for bFix)
                for (TPropertyValueIterator<FObjectPropertyBase> It(ForkObj->GetClass(), ForkObj); It; ++It)
                {
                    const FObjectPropertyBase* ObjProp = It.Key();
                    const void* ValuePtr = It->Value;
                    if (!ObjProp || !ValuePtr) { continue; }
                    UObject* Referenced = ObjProp->GetObjectPropertyValue(ValuePtr);
                    if (!Referenced) { continue; }
                    const UPackage* RefPkg = Referenced->GetPackage();
                    if (!RefPkg) { continue; }
                    const FString RefPkgName = RefPkg->GetName();
                    if (RefPkgName != SourceRoot && !RefPkgName.StartsWith(SourcePrefix)) { continue; }

                    // Build a dotted property-chain path for readability.
                    TArray<const FProperty*> PropChain;
                    It.GetPropertyChain(PropChain);
                    FString PropPath;
                    for (int32 i = PropChain.Num() - 1; i >= 0; --i)
                    {
                        if (!PropPath.IsEmpty()) { PropPath += TEXT("."); }
                        PropPath += PropChain[i]->GetName();
                    }
                    if (PropPath.IsEmpty()) { PropPath = ObjProp->GetName(); }

                    if (PropsJson.Num() < 200)
                    {
                        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
                        P->SetStringField(TEXT("property"), PropPath);
                        P->SetStringField(TEXT("ref"), Referenced->GetPathName());
                        PropsJson.Add(MakeShared<FJsonValueObject>(P));
                    }

                    if (bFix)
                    {
                        const FString CounterName = Referenced->GetName();
                        const FString* ForkPath = ForkByName.Find(CounterName);
                        if (ForkPath && !ForkPath->IsEmpty())
                        {
                            UObject* Counterpart = StaticLoadObject(UObject::StaticClass(), nullptr, **ForkPath);
                            if (Counterpart && Counterpart != Referenced)
                            {
                                ReplacementMap.Add(Referenced, Counterpart);
                            }
                            else
                            {
                                UnresolvedCounterparts.Add(Referenced->GetPathName());
                            }
                        }
                        else
                        {
                            UnresolvedCounterparts.Add(Referenced->GetPathName());
                        }
                    }
                }

                if (bFix && ReplacementMap.Num() > 0)
                {
                    FArchiveReplaceObjectRef<UObject> ReplaceAr(ForkObj, ReplacementMap,
                        EArchiveReplaceObjectFlags::IgnoreOuterRef | EArchiveReplaceObjectFlags::IgnoreArchetypeRef);
                    ForkObj->MarkPackageDirty();
                    UEditorAssetLibrary::SaveLoadedAsset(ForkObj, /*bOnlyIfIsDirty=*/false);
                    RemappedThis = ReplacementMap.Num();
                    RefsRemapped += RemappedThis;
                    ++FixedAssets;
                }
            }
        }
        Entry->SetArrayField(TEXT("properties"), PropsJson);
        if (bFix) { Entry->SetNumberField(TEXT("remapped"), RemappedThis); }
        Offenders.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TArray<TSharedPtr<FJsonValue>> UnresolvedJson;
    for (const FString& S : UnresolvedCounterparts) { UnresolvedJson.Add(MakeShared<FJsonValueString>(S)); }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("fork_root"), ForkRoot);
    Result->SetStringField(TEXT("source_root"), SourceRoot);
    Result->SetBoolField(TEXT("fix"), bFix);
    Result->SetStringField(TEXT("dependency_type"), DependencyType);
    Result->SetNumberField(TEXT("assets_scanned"), Scanned);
    Result->SetNumberField(TEXT("offending_count"), OffendingCount);
    Result->SetNumberField(TEXT("total_source_dependencies"), TotalSourceDeps);
    Result->SetArrayField(TEXT("offenders"), Offenders);
    Result->SetNumberField(TEXT("fixed_assets"), FixedAssets);
    Result->SetNumberField(TEXT("refs_remapped"), RefsRemapped);
    Result->SetArrayField(TEXT("unresolved_counterparts"), UnresolvedJson);
    Result->SetStringField(TEXT("message"), FString::Printf(
        TEXT("%d/%d fork asset(s) still reference %s (%d source deps). %s"),
        OffendingCount, Scanned, *SourceRoot, TotalSourceDeps,
        bFix ? *FString::Printf(TEXT("Remapped %d ref(s) across %d asset(s)."), RefsRemapped, FixedAssets) : TEXT("Audit only (fix=false).")));
    return mcp_common::MakeOkReply(Result);
}

// MCP:audit_class_purity - Project-wide "fork purity" audit of INHERITANCE lineage (complements audit_fork_refs, which audits a fork's INTERNAL refs). Walks every Blueprint UClass under class_root, ascends each parent chain to the point where it leaves class_root, and flags any project class whose chain crosses DIRECTLY into an upstream base library (base_libraries, e.g. /Game/VendorPack) WITHOUT going through a declared thin fork -- a class carrying the fork-origin metadata tag (project setting Fork Origin Tag) that fork_class_chain writes. Catches a new asset that inherits straight from a vendored library instead of via a project fork. Read-only: classes are loaded to resolve the chain (like audit_replication) but never saved. Returns a summary plus the full per-class classification and the actionable violation list inline. NOTE: matches exact UBlueprint assets (as audit_replication/audit_fork_refs do); Widget/Anim blueprints are out of scope for v1. A legit fork created without the fork-origin tag reports as a violation -- back-fill it with set_asset_metadata_tag. The tag is also CHECKED FOR TRUTH, not just for presence: the class it names must really be in the fork point's ancestor chain (the full chain, so declaring a grandparent is fine). A class that declares a fork origin it does not actually descend from -- a copy-sibling rather than a child, or a stale tag left behind by a re-parent -- reports verdict fork_origin_mismatch, whatever its boundary looks like; that also catches copies parented straight to a native class, which the boundary check alone would wave through as native_rooted.
//   @param class_root - Own content tree to audit. Defaults to the project setting Project Content Root ("/Game" unless configured).
//   @param base_libraries - Array of upstream roots a project class must not inherit from directly. Defaults to the project's Read Only Content Roots.
//   @param recursive - Optional bool (default true). Recurse into subfolders.
//   @param max_results - Cap on classes processed (clamped 1..100000). Defaults to 5000.
//   @param max_depth - Max parent-chain steps to ascend within class_root (clamped 1..256). Defaults to 32.
//   @returns { class_root: string, base_libraries: [string], summary: { class_count: int, pure_fork_count: int, native_rooted_count: int, other_parent_count: int, violation_count: int (missing_fork_tag + fork_origin_mismatch; == violations.length), missing_fork_tag_count: int, fork_origin_mismatch_count: int, distinct_impure_fork_points: int }, classes: [{ class, package, immediate_parent, fork_point, boundary_parent, boundary_kind, boundary_library, is_declared_fork, fork_origin, fork_origin_in_chain: bool, fork_origin_matched_ancestor: string, fork_origin_exists: bool, verdict: "pure_fork"|"native_rooted"|"other_parent"|"violation"|"fork_origin_mismatch" }], violations: [{ class, kind: "missing_fork_tag"|"fork_origin_mismatch", fork_point, boundary_parent, boundary_library, declared_fork_origin?, actual_immediate_parent?, fork_origin_exists?, actual_ancestor_chain?: [string], detail? }], message: string }
TSharedPtr<FJsonObject> HandleAuditClassPurity(const TSharedPtr<FJsonObject>& Params)
{
    const UUEAgentProjectSettings* ProjectSettings = GetDefault<UUEAgentProjectSettings>();
    FString ClassRoot = ProjectSettings->GetProjectContentRoot();
    Params->TryGetStringField(TEXT("class_root"), ClassRoot);
    ClassRoot.RemoveFromEnd(TEXT("/"));
    if (ClassRoot.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("class_root must be a non-empty content path (e.g. /Game)"));
    }

    // Library roots an own class must not inherit from directly (default: the project's
    // read-only content roots).
    TArray<FString> BaseLibs;
    const TArray<TSharedPtr<FJsonValue>>* BaseLibsJson = nullptr;
    if (Params->TryGetArrayField(TEXT("base_libraries"), BaseLibsJson) && BaseLibsJson)
    {
        for (const TSharedPtr<FJsonValue>& V : *BaseLibsJson)
        {
            FString S = V.IsValid() ? V->AsString() : FString();
            S.RemoveFromEnd(TEXT("/"));
            if (!S.IsEmpty()) { BaseLibs.Add(S); }
        }
    }
    if (BaseLibs.Num() == 0) { BaseLibs = mcp_common::GetDefaultReadonlyRoots(); }

    bool bRecursive = true;
    Params->TryGetBoolField(TEXT("recursive"), bRecursive);
    int32 MaxResults = 5000;
    double MaxResultsD = 0.0;
    if (Params->TryGetNumberField(TEXT("max_results"), MaxResultsD)) { MaxResults = FMath::Clamp(static_cast<int32>(MaxResultsD), 1, 100000); }
    int32 MaxDepth = 32;
    double MaxDepthD = 0.0;
    if (Params->TryGetNumberField(TEXT("max_depth"), MaxDepthD)) { MaxDepth = FMath::Clamp(static_cast<int32>(MaxDepthD), 1, 256); }
    const FName ForkOriginTag = ProjectSettings->GetForkOriginTag();
    const FString ForkOriginTagName = ForkOriginTag.ToString();

    IAssetRegistry& AssetRegistry = mcp_common::AssetRegistry();

    TArray<FAssetData> AllAssets;
    AssetRegistry.GetAssetsByPath(FName(*ClassRoot), AllAssets, bRecursive, /*bIncludeOnlyOnDiskAssets=*/false);
    const FTopLevelAssetPath BlueprintClassPath = UBlueprint::StaticClass()->GetClassPathName();

    TArray<TSharedPtr<FJsonValue>> ClassEntries;
    TArray<TSharedPtr<FJsonValue>> ViolationEntries;
    TSet<FString> ImpureForkPoints;
    int32 Processed = 0, PureForkCount = 0, NativeRootedCount = 0, OtherParentCount = 0;
    // violation_count is the TOTAL actionable set (== violations.Num()); these are its two kinds.
    int32 MissingForkTagCount = 0, ForkOriginMismatchCount = 0;

    // An own class lives under ClassRoot and NOT under a library root: with the default
    // ClassRoot (/Game) the libraries sit inside it, and their classes are not the project's.
    auto IsOwnPackage = [&ClassRoot, &BaseLibs](const FString& PackageName)
    {
        return PackageUnderRoot(PackageName, ClassRoot) && MatchBaseLibrary(PackageName, BaseLibs).IsEmpty();
    };

    for (const FAssetData& AD : AllAssets)
    {
        if (AD.AssetClassPath != BlueprintClassPath) { continue; } // exact UBlueprint (parity w/ audit_replication/audit_fork_refs)
        if (!IsOwnPackage(AD.PackageName.ToString())) { continue; } // a library class, not audited
        if (Processed >= MaxResults) { break; }
        UBlueprint* BP = Cast<UBlueprint>(AD.GetAsset());
        if (!BP || !BP->GeneratedClass) { continue; }
        ++Processed;

        UClass* GenClass = BP->GeneratedClass;

        // Ascend while the super is still an own class -> Cur = topmost own class (fork point).
        UClass* Cur = GenClass;
        int32 Depth = 0;
        while (Cur->GetSuperClass()
            && IsOwnPackage(ClassPackageName(Cur->GetSuperClass()))
            && Depth < MaxDepth)
        {
            Cur = Cur->GetSuperClass();
            ++Depth;
        }
        UClass* Boundary = Cur->GetSuperClass();          // first non-own ancestor
        const FString BoundaryPkg = ClassPackageName(Boundary);
        const FString BoundaryLib = MatchBaseLibrary(BoundaryPkg, BaseLibs);

        FString BoundaryKind;
        if (!Boundary)                                     { BoundaryKind = TEXT("native"); } // UObject root (own can't be this in practice)
        else if (!BoundaryLib.IsEmpty())                   { BoundaryKind = TEXT("base_library"); }
        else if (BoundaryPkg.StartsWith(TEXT("/Script/"))) { BoundaryKind = TEXT("native"); }
        else                                               { BoundaryKind = TEXT("other"); }

        // Is the fork point a DECLARED thin fork? (the fork-origin tag on its Blueprint's primary asset.)
        FString ForkOrigin;
        if (UBlueprint* CurBP = UBlueprint::GetBlueprintFromClass(Cur))
        {
            ForkOrigin = UEditorAssetLibrary::GetMetadataTag(CurBP, ForkOriginTag);
        }
        const bool bDeclaredFork = !ForkOrigin.IsEmpty();

        // Does the tag tell the TRUTH? `is_declared_fork` above is presence-only; on its own it
        // rubber-stamps any class carrying the tag, however unrelated its real parent is. Verify
        // the declared origin is actually an ancestor of the fork point.
        FString ForkOriginPkg, MatchedAncestor;
        TArray<FString> AncestorChain;
        bool bOriginInChain = false;
        bool bOriginAssetExists = false;
        if (bDeclaredFork)
        {
            ForkOriginPkg = ForkOriginToPackageName(ForkOrigin);
            bOriginInChain = ForkOriginIsAncestorOf(Cur, ForkOriginPkg, ForkOrigin, MatchedAncestor, AncestorChain);
            // Distinguish "tag points at a wrong-but-real class" from "tag is stale/dangling"
            // (asset renamed or deleted). Registry-only lookup: no loading.
            if (!ForkOriginPkg.IsEmpty())
            {
                TArray<FAssetData> OriginAssets;
                AssetRegistry.GetAssetsByPackageName(FName(*ForkOriginPkg), OriginAssets, /*bIncludeOnlyOnDiskAssets=*/false);
                bOriginAssetExists = OriginAssets.Num() > 0;
            }
        }

        FString Verdict;
        if (BoundaryKind == TEXT("base_library"))
        {
            Verdict = bDeclaredFork ? TEXT("pure_fork") : TEXT("violation");
        }
        else if (BoundaryKind == TEXT("other")) { Verdict = TEXT("other_parent"); }
        else                                    { Verdict = TEXT("native_rooted"); }

        // A declared fork whose origin is NOT in the real chain outranks the boundary verdict.
        // It has to: a copy-sibling of an upstream class (BP_MyPlayerController declaring BP_VendorPlayerController while
        // parented straight to /Script/Engine.PlayerController) never crosses a base library at
        // all, so the boundary check alone classifies it as a clean native_rooted and never
        // even lists it.
        if (bDeclaredFork && !bOriginInChain)
        {
            Verdict = TEXT("fork_origin_mismatch");
        }

        if      (Verdict == TEXT("pure_fork"))     { ++PureForkCount; }
        else if (Verdict == TEXT("native_rooted")) { ++NativeRootedCount; }
        else if (Verdict == TEXT("other_parent"))  { ++OtherParentCount; }
        else if (Verdict == TEXT("fork_origin_mismatch"))
        {
            ++ForkOriginMismatchCount;
            ImpureForkPoints.Add(Cur->GetPathName());
        }
        else /* violation */
        {
            ++MissingForkTagCount;
            ImpureForkPoints.Add(Cur->GetPathName());
        }

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("class"), GenClass->GetPathName());
        Entry->SetStringField(TEXT("package"), AD.PackageName.ToString());
        Entry->SetStringField(TEXT("immediate_parent"),
            GenClass->GetSuperClass() ? GenClass->GetSuperClass()->GetPathName() : FString());
        Entry->SetStringField(TEXT("fork_point"), Cur->GetPathName());
        Entry->SetStringField(TEXT("boundary_parent"), Boundary ? Boundary->GetPathName() : FString());
        Entry->SetStringField(TEXT("boundary_kind"), BoundaryKind);
        Entry->SetStringField(TEXT("boundary_library"), BoundaryLib);
        Entry->SetBoolField(TEXT("is_declared_fork"), bDeclaredFork);
        Entry->SetStringField(TEXT("fork_origin"), ForkOrigin);
        // Truth of the declaration, not just its presence (meaningless when there's no tag).
        Entry->SetBoolField(TEXT("fork_origin_in_chain"), bDeclaredFork ? bOriginInChain : false);
        Entry->SetStringField(TEXT("fork_origin_matched_ancestor"), MatchedAncestor);
        Entry->SetBoolField(TEXT("fork_origin_exists"), bDeclaredFork ? bOriginAssetExists : false);
        Entry->SetStringField(TEXT("verdict"), Verdict);
        ClassEntries.Add(MakeShared<FJsonValueObject>(Entry));

        if (Verdict == TEXT("violation") || Verdict == TEXT("fork_origin_mismatch"))
        {
            TSharedPtr<FJsonObject> Vn = MakeShared<FJsonObject>();
            Vn->SetStringField(TEXT("class"), GenClass->GetPathName());
            Vn->SetStringField(TEXT("kind"),
                Verdict == TEXT("fork_origin_mismatch") ? TEXT("fork_origin_mismatch") : TEXT("missing_fork_tag"));
            Vn->SetStringField(TEXT("fork_point"), Cur->GetPathName());
            Vn->SetStringField(TEXT("boundary_parent"), Boundary ? Boundary->GetPathName() : FString());
            Vn->SetStringField(TEXT("boundary_library"), BoundaryLib);
            if (Verdict == TEXT("fork_origin_mismatch"))
            {
                // Everything needed to act without a second round-trip: what was claimed, what is
                // real, whether the claim is merely wrong or outright dangling.
                Vn->SetStringField(TEXT("declared_fork_origin"), ForkOrigin);
                Vn->SetStringField(TEXT("actual_immediate_parent"),
                    Cur->GetSuperClass() ? Cur->GetSuperClass()->GetPathName() : FString());
                Vn->SetBoolField(TEXT("fork_origin_exists"), bOriginAssetExists);
                Vn->SetArrayField(TEXT("actual_ancestor_chain"), mcp_common::StringsToJson(AncestorChain));
                Vn->SetStringField(TEXT("detail"), FString::Printf(
                    TEXT("%s claims '%s'%s, but that class is not in this class's ancestor chain (real parent: %s). ")
                    TEXT("Either the fork is a copy-sibling rather than a child (re-parent it), or the tag is wrong (fix the tag)."),
                    *ForkOriginTagName,
                    *ForkOrigin,
                    bOriginAssetExists ? TEXT("") : TEXT(" (which no longer exists on disk)"),
                    Cur->GetSuperClass() ? *Cur->GetSuperClass()->GetPathName() : TEXT("<none>")));
            }
            ViolationEntries.Add(MakeShared<FJsonValueObject>(Vn));
        }
    }

    const int32 ViolationCount = MissingForkTagCount + ForkOriginMismatchCount;

    TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
    Summary->SetNumberField(TEXT("class_count"), Processed);
    Summary->SetNumberField(TEXT("pure_fork_count"), PureForkCount);
    Summary->SetNumberField(TEXT("native_rooted_count"), NativeRootedCount);
    Summary->SetNumberField(TEXT("other_parent_count"), OtherParentCount);
    Summary->SetNumberField(TEXT("violation_count"), ViolationCount);
    Summary->SetNumberField(TEXT("missing_fork_tag_count"), MissingForkTagCount);
    Summary->SetNumberField(TEXT("fork_origin_mismatch_count"), ForkOriginMismatchCount);
    Summary->SetNumberField(TEXT("distinct_impure_fork_points"), ImpureForkPoints.Num());

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("class_root"), ClassRoot);
    Result->SetArrayField(TEXT("base_libraries"), mcp_common::StringsToJson(BaseLibs));
    Result->SetObjectField(TEXT("summary"), Summary);
    Result->SetArrayField(TEXT("classes"), ClassEntries);
    Result->SetArrayField(TEXT("violations"), ViolationEntries);
    Result->SetStringField(TEXT("message"), FString::Printf(
        TEXT("%d class(es) under %s: %d pure_fork, %d native_rooted, %d other_parent, %d violation(s) ")
        TEXT("(%d missing %s, %d fork_origin_mismatch) across %d distinct impure fork point(s)."),
        Processed, *ClassRoot, PureForkCount, NativeRootedCount, OtherParentCount, ViolationCount,
        MissingForkTagCount, *ForkOriginTagName, ForkOriginMismatchCount, ImpureForkPoints.Num()));
    return mcp_common::MakeOkReply(Result);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("audit_class_purity"), &HandleAuditClassPurity },
    { TEXT("audit_fork_refs"), &HandleAuditForkRefs },
    { TEXT("delete_asset"), &HandleDeleteAsset },
    { TEXT("duplicate_asset"), &HandleDuplicateAsset },
    { TEXT("duplicate_directory"), &HandleDuplicateDirectory },
    { TEXT("get_plugin_status"), &HandleGetPluginStatus },
    { TEXT("move_asset"), &HandleMoveAsset },
    { TEXT("rename_asset"), &HandleRenameAsset },
    { TEXT("run_external_project_script"), &HandleRunExternalProjectScript },
};
} // namespace UEAgentManagementCommands
