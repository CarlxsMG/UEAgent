#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

// IAssetTools / FAssetToolsModule (AssetTools developer module)
#include "IAssetTools.h"
#include "AssetToolsModule.h"
#include "PackageMigrationContext.h"                          // migrate_assets: the packages the migration wrote, and where
#include "HAL/IConsoleManager.h"                              // migrate_assets: AssetTools.UseNewPackageMigration

// UEditorAssetSubsystem (UnrealEd editor subsystem)
#include "Subsystems/EditorAssetSubsystem.h"

// IAssetRegistry / FAssetRegistryModule (AssetRegistry runtime module)
#include "AssetRegistry/AssetRegistryModule.h"

// Editor core
#include "Editor.h"
#include "UObject/ObjectRedirector.h"

// bulk_delete_assets guards / verification.
#include "ObjectTools.h"                                      // ObjectTools::ForceDeleteObjects (same call UEditorAssetSubsystem::DeleteAsset makes internally, EditorAssetSubsystem.cpp)
#include "Editor/LevelEditor/Public/LevelEditorSubsystem.h"   // ULevelEditorSubsystem::IsInPlayInEditor -- same PIE-guard idiom fixup_redirectors uses (SystemCommands.cpp)
#include "Engine/World.h"                                     // UWorld (open-editor-world gate via mcp_common::GetEditorWorld)
#include "UObject/UObjectGlobals.h"                           // StaticFindObject / StaticLoadObject
#include "UObject/Package.h"                                  // UPackage::GetName (open-world gate, lineage ascent)
#include "Misc/PackageName.h"                                 // FPackageName::DoesPackageExist / ObjectPathToPackageName
#include "HAL/FileManager.h"                                  // IFileManager::FindFiles / FileSize

// find_unused_assets keep_reasons.
#include "Engine/Blueprint.h"                                 // UBlueprint (base-library lineage ascent)
#include "Misc/Paths.h"                                       // FPaths::ProjectConfigDir / Combine
#include "Misc/FileHelper.h"                                  // FFileHelper::LoadFileToString (Config/Default*.ini scan)

/**
 * Editor automation / bulk asset operations, performed editor-side via IAssetTools,
 * UEditorAssetSubsystem and IAssetRegistry:
 *   - bulk_delete_assets       -> GUARDED batch delete. dry_run defaults to TRUE; a
 *                                 readonly_roots / protect_paths / open-editor-world /
 *                                 (opt-in) zero-referencer gate REFUSES per item, and the
 *                                 delete itself is ObjectTools::ForceDeleteObjects over the
 *                                 whole eligible batch followed by a per-item re-check
 *                                 (FPackageName::DoesPackageExist + StaticFindObject).
 *   - find_unused_assets       -> UEditorAssetSubsystem::ListAssets + IAssetRegistry
 *                                 ::GetReferencers per asset (dependency_type-aware via
 *                                 FDependencyQuery), filter zero-referencers, then annotate
 *                                 the survivors with keep_reasons (protect_paths /
 *                                 Config/Default*.ini / opt-in base-library lineage).
 *   - export_assets            -> IAssetTools::ExportAssets (string-path overload); what it
 *                                 wrote is measured per asset on disk.
 *   - migrate_assets           -> IAssetTools::MigratePackages with FMigrationOptions; what it
 *                                 wrote is measured from the migration's own package list
 *                                 (GetOnPackageMigration) against the disk.
 *   - consolidate_assets       -> UEditorAssetSubsystem::ConsolidateAssets, partitioned
 *                                 by class compatibility, with an optional dry_run preview.
 *   - find_duplicate_assets    -> IAssetRegistry::GetAssetsByPath, grouped by AssetName;
 *                                 metadata-only (never force-loads UObjects).
 *
 * Modules: AssetTools (IAssetTools, FAssetToolsModule), UnrealEd
 * (UEditorAssetSubsystem), AssetRegistry (IAssetRegistry / FAssetRegistryModule).
 */
namespace UEAgentEditorAutomationCommands
{

// MCP-DOMAIN: Editor

// ---------------------------------------------------------------------------
// Anonymous helpers
// ---------------------------------------------------------------------------
namespace
{
	/**
	 * Resolve the UEditorAssetSubsystem. Returns nullptr when the editor is not
	 * available (should never happen for editor-only commands, but guard anyway).
	 */
	UEditorAssetSubsystem* GetEditorAssetSubsystem()
	{
		if (!GEditor)
		{
			return nullptr;
		}
		return GEditor->GetEditorSubsystem<UEditorAssetSubsystem>();
	}

	/** Reads the required, non-empty "asset_paths" array param. Null when it is there, else the error reply. */
	TSharedPtr<FJsonObject> EditorAuto_ReadAssetPaths(const TSharedPtr<FJsonObject>& Params,
		const TArray<TSharedPtr<FJsonValue>>*& OutPaths)
	{
		OutPaths = nullptr;
		if (!Params->TryGetArrayField(TEXT("asset_paths"), OutPaths) || !OutPaths || OutPaths->Num() == 0)
		{
			return mcp_common::MakeErrorReply(TEXT("asset_paths array is required and must not be empty"));
		}
		return nullptr;
	}

	/**
	 * The referencer query a dependency_type value selects: "hard" or "soft" (case-insensitive), else no requirement
	 * (hard + soft + every category, what the plain GetReferencers overload uses).
	 */
	UE::AssetRegistry::FDependencyQuery EditorAuto_DependencyQuery(const FString& DependencyType)
	{
		if (DependencyType.Equals(TEXT("hard"), ESearchCase::IgnoreCase))
		{
			return UE::AssetRegistry::FDependencyQuery(UE::AssetRegistry::EDependencyQuery::Hard);
		}
		if (DependencyType.Equals(TEXT("soft"), ESearchCase::IgnoreCase))
		{
			return UE::AssetRegistry::FDependencyQuery(UE::AssetRegistry::EDependencyQuery::Soft);
		}
		return UE::AssetRegistry::FDependencyQuery();
	}

	/**
	 * Convert an asset object path (/Game/Folder/AssetName) to its package name
	 * (same string for top-level assets). Used for IAssetRegistry queries.
	 */
	FName AssetPathToPackageName(const FString& AssetPath)
	{
		// The asset registry uses package names which for simple assets equal the
		// long package path. Strip any sub-object suffix.
		FString Pkg = AssetPath;
		int32 DotIdx;
		if (Pkg.FindLastChar(TEXT('.'), DotIdx))
		{
			// If the part after the dot equals the part before (self-referencing
			// package), keep as-is. Otherwise strip sub-object.
			FString Before = Pkg.Left(DotIdx);
			FString After  = Pkg.Mid(DotIdx + 1);
			if (Before.EndsWith(After))
			{
				Pkg = Before;
			}
		}
		return FName(*Pkg);
	}

	/**
	 * Get the on-disk size of an asset package in KB. Returns 0 if the file
	 * doesn't exist or can't be stat'd.
	 */
	int64 GetAssetSizeKB(const FString& AssetPath)
	{
		FString PackageFile;
		if (!mcp_common::FindPackageFileOnDisk(AssetPath, PackageFile))
		{
			return 0;
		}
		const int64 FileSize = IFileManager::Get().FileSize(*PackageFile);
		return FileSize > 0 ? FileSize / 1024 : 0;
	}

	// -----------------------------------------------------------------------
	// Shared helpers for the guarded bulk_delete_assets / find_unused_assets
	// paths.
	// -----------------------------------------------------------------------

	/** Read an optional JSON string array param. Absent/non-array field leaves OutValues empty. */
	void ParseStringArrayParam_EditorAuto(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, TArray<FString>& OutValues)
	{
		OutValues.Reset();
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Params.IsValid() && Params->TryGetArrayField(FieldName, Arr) && Arr)
		{
			for (const TSharedPtr<FJsonValue>& Val : *Arr)
			{
				FString S;
				if (Val.IsValid() && Val->TryGetString(S))
				{
					S.TrimStartAndEndInline();
					if (!S.IsEmpty())
					{
						OutValues.Add(S);
					}
				}
			}
		}
	}

	/**
	 * Canonical object path for a package name -- "/Game/A/B" -> "/Game/A/B.B".
	 * Holds for every top-level asset AND for maps (the UWorld's object name always
	 * equals the package short name), which is what StaticFindObject/StaticLoadObject
	 * need. A path that already carries a '.' is returned untouched.
	 */
	FString MakeObjectPath_EditorAuto(const FString& PackageOrObjectPath)
	{
		if (PackageOrObjectPath.Contains(TEXT(".")))
		{
			return PackageOrObjectPath;
		}
		const FString ShortName = FPackageName::GetShortName(PackageOrObjectPath);
		return FString::Printf(TEXT("%s.%s"), *PackageOrObjectPath, *ShortName);
	}

	/**
	 * True when Candidate matches any of Patterns (FString::MatchesWildcard,
	 * case-insensitive; `*` spans '/' so "/Game/MyGame/*" covers whole subtrees and
	 * "*_OLD*" covers a naming convention). OutMatched receives the first hit.
	 * An empty pattern list never matches -- protect_paths is opt-in.
	 */
	bool MatchesAnyWildcard_EditorAuto(const FString& Candidate, const TArray<FString>& Patterns, FString& OutMatched)
	{
		for (const FString& Pattern : Patterns)
		{
			if (Candidate.MatchesWildcard(Pattern))
			{
				OutMatched = Pattern;
				return true;
			}
		}
		return false;
	}

	// True when a package path lies at, or under, a content root (exact match or Root + "/").
	// Case-insensitive on BOTH arms so it agrees exactly with mcp_common::IsUnderReadOnlyRoot --
	// a mismatch there would leave the reported matched_root empty on a refusal.
	bool PackageUnderRoot_EditorAuto(const FString& Pkg, const FString& Root)
	{
		return !Pkg.IsEmpty() && (Pkg.Equals(Root, ESearchCase::IgnoreCase) || Pkg.StartsWith(Root + TEXT("/"), ESearchCase::IgnoreCase));
	}

	/**
	 * Parse the `base_libraries` param (vendored-library content roots), defaulting
	 * to the project's Read Only Content Roots (mcp_common::GetDefaultReadonlyRoots()).
	 * Trailing slashes trimmed. An EXPLICIT empty array yields the default too -- unlike
	 * readonly_roots, an empty base-library list has no useful "disable" meaning here (it
	 * would just make the lineage reason unreachable, which `include_lineage_reasons=false`
	 * already expresses).
	 */
	TArray<FString> ParseBaseLibraries_EditorAuto(const TSharedPtr<FJsonObject>& Params)
	{
		TArray<FString> BaseLibs;
		ParseStringArrayParam_EditorAuto(Params, TEXT("base_libraries"), BaseLibs);
		for (FString& Lib : BaseLibs)
		{
			while (Lib.Len() > 1 && Lib.EndsWith(TEXT("/"))) { Lib.LeftChopInline(1); }
		}
		BaseLibs.RemoveAll([](const FString& S) { return S.IsEmpty(); });
		if (BaseLibs.Num() == 0)
		{
			BaseLibs = mcp_common::GetDefaultReadonlyRoots();
		}
		return BaseLibs;
	}

	/**
	 * Walk a generated class' FULL super chain looking for an ancestor whose owning
	 * package lives under one of BaseLibs -- the "this is a fork of the vendored library,
	 * not orphan debris" signal. Same ascent audit_class_purity performs
	 * (ManagementCommands.cpp), reduced to the boolean this tool needs: here we
	 * do NOT stop at the first non-own ancestor, because a project asset may sit several own
	 * classes above its library boundary. Returns the matched library root and ancestor path.
	 */
	bool ClassDescendsFromBaseLibrary_EditorAuto(
		const UClass* GeneratedClass,
		const TArray<FString>& BaseLibs,
		FString& OutMatchedLibrary,
		FString& OutMatchedAncestor)
	{
		constexpr int32 MaxChainWalk = 256; // UClass chains cannot cycle; this only bounds the walk.
		int32 Steps = 0;
		for (const UClass* A = GeneratedClass ? GeneratedClass->GetSuperClass() : nullptr; A && Steps < MaxChainWalk; A = A->GetSuperClass(), ++Steps)
		{
			const UPackage* Pkg = A->GetOutermost();
			const FString PkgName = Pkg ? Pkg->GetName() : FString();
			for (const FString& Lib : BaseLibs)
			{
				if (PackageUnderRoot_EditorAuto(PkgName, Lib))
				{
					OutMatchedLibrary = Lib;
					OutMatchedAncestor = A->GetPathName();
					return true;
				}
			}
		}
		return false;
	}

	/**
	 * Concatenated text of every Config/Default*.ini. Deliberately NOT the whole
	 * Saved/Config tree: only Default*.ini is version-controlled project intent.
	 * OutFileNames receives the files actually read, so an empty scan is visible instead
	 * of silently meaning "nothing referenced".
	 */
	FString LoadDefaultConfigBlob_EditorAuto(TArray<FString>& OutFileNames)
	{
		OutFileNames.Reset();
		FString Blob;
		const FString ConfigDir = FPaths::ProjectConfigDir();
		TArray<FString> Found;
		IFileManager::Get().FindFiles(Found, *FPaths::Combine(ConfigDir, TEXT("Default*.ini")), /*Files=*/true, /*Directories=*/false);
		for (const FString& FileName : Found)
		{
			FString Contents;
			if (FFileHelper::LoadFileToString(Contents, *FPaths::Combine(ConfigDir, FileName)))
			{
				OutFileNames.Add(FileName);
				Blob += Contents;
				Blob += TEXT("\n");
			}
		}
		return Blob;
	}
}

// ---------------------------------------------------------------------------
// 2. bulk_delete_assets
// ---------------------------------------------------------------------------
// MCP:bulk_delete_assets - GUARDED batch asset deletion. ***dry_run DEFAULTS TO TRUE*** (deliberate: it is the most destructive command in the plugin, `bulk_delete_assets(["/Game/VendorPack"], recursive=true)` would be a valid unconfirmed call, and agent allowlists may run mutating tools with no prompt). Four independent gates REFUSE per item instead of skipping silently: readonly_roots (default: the project's Read Only Content Roots), protect_paths (wildcards -- the only gate that saves a zero-referencer asset), the currently-open editor world, and the OPT-IN require_zero_referencers gate whose referencer count EXCLUDES referencers that are themselves in this batch (so a self-contained dead subgraph can still be deleted whole). Deletion is a single ObjectTools::ForceDeleteObjects over the whole eligible batch -- resolving each object here via StaticFindObject/StaticLoadObject rather than through UEditorAssetSubsystem::DeleteAsset, whose UE::EditorAssetUtils::LoadAssetFromPath/EnsureAssetsLoaded preamble is a known crash site ("Array index out of bounds", deterministic on corrupted leftovers). deleted_count counts ONLY items that passed BOTH post-delete probes -- the package file is gone (FPackageName::DoesPackageExist) AND the object no longer resolves at its original path (StaticFindObject) -- never the API's own return value. Both are required because for a never-saved, registry-visible asset the disk probe is vacuously true (it was never on disk), so residency is the only real evidence; each failure names which probe still sees the item.
//   @param asset_paths - List of asset paths (or folder paths when recursive is true). Required, non-empty.
//   @param recursive - If true, expand folder paths to all contained assets. Default false.
//   @param dry_run - Default TRUE. Nothing is deleted; would_delete reports what WOULD go, with each item's gate verdict. Pass false to actually delete.
//   @param readonly_roots [array] - Content roots that may never be deleted from. Default: the project's Read Only Content Roots; pass [] to disable the gate entirely. Matches are REFUSED (refused_readonly_root), never silently skipped. (Read via mcp_common::ParseReadonlyRoots, so the type is declared here.)
//   @param protect_paths [array] - Wildcard patterns (FString::MatchesWildcard, case-insensitive, `*` spans '/') matched against both the package name and the object path. Default [] (gate off). Matches are REFUSED (refused_protected).
//   @param require_zero_referencers - Default FALSE. When true, an asset with at least one referencer OUTSIDE this batch is refused. The per-item external_referencer_count is reported either way.
//   @param dependency_type - Referencer filter for the count above: "hard"|"soft"|"all" (case-insensitive), default "all".
//   @returns { requested_count, expanded_count, recursive, dry_run, readonly_roots: [string], protect_paths: [string], require_zero_referencers: bool, dependency_type: string, expanded_folders: [{folder, asset_count}], eligible_count, would_delete: [{path, package_name, external_referencer_count, external_referencers: [string], on_disk}], deleted: [{path, package_name, deleted, still_loaded, error?}], deleted_count, failed_count, force_delete_reported_count, refused_readonly_root: [{path, matched_root}], refused_protected: [{path, matched_pattern}], refused_referenced: [{path, external_referencer_count, external_referencers: [string]}], refused_open_world: [{path, reason}], refused_count, results: [{path, success, error}], note, verification: {...} }
TSharedPtr<FJsonObject> HandleBulkDeleteAssets(
	const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* PathsArray = nullptr;
	if (TSharedPtr<FJsonObject> Missing = EditorAuto_ReadAssetPaths(Params, PathsArray)) { return Missing; }

	UEditorAssetSubsystem* AssetSubsystem = GetEditorAssetSubsystem();
	if (!AssetSubsystem)
	{
		return mcp_common::MakeErrorReply(TEXT("EditorAssetSubsystem is not available"));
	}

	// The registry is REQUIRED, not optional. Without it the referencer count would be a
	// constant 0 -- i.e. the gate would silently report "nothing references this" for every asset.
	// Refusing is the only honest option.
	IAssetRegistry* Registry = &mcp_common::AssetRegistry();
	if (!Registry)
	{
		return mcp_common::MakeErrorReply(TEXT(
			"AssetRegistry is not available: the referencer gate cannot be evaluated, so bulk_delete_assets refuses to delete anything."));
	}

	// PIE guard. UEditorAssetSubsystem::DeleteAsset already fails during PIE
	// (EditorScriptingHelpers::CheckIfInEditorAndPIE, EditorScriptingHelpers.cpp), so
	// this call would quietly return failed_count == N. Refusing up front turns that silent per-item
	// failure into one explicit error -- same idiom fixup_redirectors uses (SystemCommands.cpp).
	if (GEditor)
	{
		ULevelEditorSubsystem* LevelEditorSubsystem = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>();
		if (LevelEditorSubsystem && LevelEditorSubsystem->IsInPlayInEditor())
		{
			return mcp_common::MakeErrorReply(TEXT(
				"bulk_delete_assets refuses to run while Play-In-Editor is active: asset deletion is rejected "
				"by the engine during PIE and would report a batch of silent failures. Stop PIE "
				"(stop_play_in_editor) and call again."));
		}
	}

	bool bRecursive = false;
	Params->TryGetBoolField(TEXT("recursive"), bRecursive);

	// Defaults to TRUE on purpose: see the tool description.
	bool bDryRun = true;
	Params->TryGetBoolField(TEXT("dry_run"), bDryRun);

	bool bRequireZeroReferencers = false;
	Params->TryGetBoolField(TEXT("require_zero_referencers"), bRequireZeroReferencers);

	FString DependencyType = TEXT("all");
	Params->TryGetStringField(TEXT("dependency_type"), DependencyType);
	const UE::AssetRegistry::FDependencyQuery DependencyQuery = EditorAuto_DependencyQuery(DependencyType);

	const TArray<FString> ReadOnlyRoots = mcp_common::ParseReadonlyRoots(Params);

	TArray<FString> ProtectPaths;
	ParseStringArrayParam_EditorAuto(Params, TEXT("protect_paths"), ProtectPaths);

	// Never delete the package backing the world the editor currently has open.
	FString OpenWorldPackage;
	if (UWorld* EditorWorld = mcp_common::GetEditorWorld())
	{
		if (const UPackage* WorldPkg = EditorWorld->GetOutermost())
		{
			OpenWorldPackage = WorldPkg->GetName();
		}
	}

	// ---------------------------------------------------------------------
	// 1. Expand folders (when recursive) and de-duplicate.
	// ---------------------------------------------------------------------
	TArray<FString> AllPaths;
	TSet<FString> SeenPaths;
	TArray<TSharedPtr<FJsonValue>> ExpandedFolders;
	for (const TSharedPtr<FJsonValue>& Val : *PathsArray)
	{
		FString Path = Val.IsValid() ? Val->AsString() : FString();
		Path.TrimStartAndEndInline();
		if (Path.IsEmpty()) { continue; }

		bool bExpanded = false;
		if (bRecursive)
		{
			TArray<FString> SubAssets = AssetSubsystem->ListAssets(Path, true, false);
			if (SubAssets.Num() > 0)
			{
				for (const FString& Sub : SubAssets)
				{
					if (!Sub.IsEmpty() && !SeenPaths.Contains(Sub))
					{
						SeenPaths.Add(Sub);
						AllPaths.Add(Sub);
					}
				}
				TSharedPtr<FJsonObject> FolderObj = MakeShared<FJsonObject>();
				FolderObj->SetStringField(TEXT("folder"), Path);
				FolderObj->SetNumberField(TEXT("asset_count"), SubAssets.Num());
				ExpandedFolders.Add(MakeShared<FJsonValueObject>(FolderObj));
				bExpanded = true;
			}
		}
		if (!bExpanded && !SeenPaths.Contains(Path))
		{
			SeenPaths.Add(Path);
			AllPaths.Add(Path);
		}
	}

	// Package set of the WHOLE batch -- referencers inside it are not "external".
	// Without this exclusion a self-contained dead subgraph (A -> B, both being deleted)
	// refuses itself entirely and the referencer gate is useless for its only real use case.
	// FPackageName::ObjectPathToPackageName (not this file's AssetPathToPackageName) on purpose:
	// it strips everything from the FIRST '.' unconditionally, so a mismatched sub-object suffix
	// can never leak a wrong package name into the read-only gate.
	TSet<FName> BatchPackages;
	TArray<FString> PackageNames;
	PackageNames.Reserve(AllPaths.Num());
	for (const FString& P : AllPaths)
	{
		const FString Pkg = FPackageName::ObjectPathToPackageName(P);
		PackageNames.Add(Pkg);
		BatchPackages.Add(FName(*Pkg));
	}

	// ---------------------------------------------------------------------
	// 2. Gate every candidate. Refusals are reported, never silently skipped.
	// ---------------------------------------------------------------------
	TArray<TSharedPtr<FJsonValue>> RefusedReadOnly;
	TArray<TSharedPtr<FJsonValue>> RefusedProtected;
	TArray<TSharedPtr<FJsonValue>> RefusedReferenced;
	TArray<TSharedPtr<FJsonValue>> RefusedOpenWorld;

	// Per-path result rows kept for compatibility, keyed by index into AllPaths so the
	// `results` array keeps its shape for existing callers.
	TArray<FString> ResultErrors;
	TArray<bool> ResultSuccess;
	ResultErrors.SetNum(AllPaths.Num());
	ResultSuccess.Init(false, AllPaths.Num());

	struct FEligibleItem
	{
		int32 Index = INDEX_NONE;
		FString Path;
		FString PackageName;
		FString ObjectPath;
		int32 ExternalRefCount = 0;
		TArray<FString> ExternalRefs;
		bool bOnDisk = false;
	};
	TArray<FEligibleItem> Eligible;

	for (int32 Idx = 0; Idx < AllPaths.Num(); ++Idx)
	{
		const FString& AssetPath = AllPaths[Idx];
		const FString& PackageName = PackageNames[Idx];

		if (mcp_common::IsUnderReadOnlyRoot(PackageName, ReadOnlyRoots))
		{
			FString MatchedRoot;
			for (const FString& Root : ReadOnlyRoots)
			{
				if (PackageUnderRoot_EditorAuto(PackageName, Root)) { MatchedRoot = Root; break; }
			}
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("path"), AssetPath);
			Obj->SetStringField(TEXT("matched_root"), MatchedRoot);
			RefusedReadOnly.Add(MakeShared<FJsonValueObject>(Obj));
			ResultErrors[Idx] = FString::Printf(
				TEXT("REFUSED: '%s' lies under read-only root '%s' (readonly_roots). Pass readonly_roots=[] to disable the gate."),
				*PackageName, *MatchedRoot);
			continue;
		}

		FString MatchedPattern;
		if (MatchesAnyWildcard_EditorAuto(PackageName, ProtectPaths, MatchedPattern)
			|| MatchesAnyWildcard_EditorAuto(AssetPath, ProtectPaths, MatchedPattern))
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("path"), AssetPath);
			Obj->SetStringField(TEXT("matched_pattern"), MatchedPattern);
			RefusedProtected.Add(MakeShared<FJsonValueObject>(Obj));
			ResultErrors[Idx] = FString::Printf(
				TEXT("REFUSED: '%s' matches protect_paths pattern '%s'."), *AssetPath, *MatchedPattern);
			continue;
		}

		if (!OpenWorldPackage.IsEmpty() && PackageName.Equals(OpenWorldPackage, ESearchCase::IgnoreCase))
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("path"), AssetPath);
			Obj->SetStringField(TEXT("reason"), TEXT("This package is the world currently open in the editor; open a different map first."));
			RefusedOpenWorld.Add(MakeShared<FJsonValueObject>(Obj));
			ResultErrors[Idx] = TEXT("REFUSED: package is the world currently open in the editor.");
			continue;
		}

		// Referencers OUTSIDE the batch.
		TArray<FName> Referencers;
		Registry->GetReferencers(FName(*PackageName), Referencers, UE::AssetRegistry::EDependencyCategory::Package, DependencyQuery);
		TArray<FString> ExternalRefs;
		for (const FName& Ref : Referencers)
		{
			if (Ref == FName(*PackageName)) { continue; }        // self-reference is not a referencer
			if (BatchPackages.Contains(Ref)) { continue; }       // inside the batch -> about to die too
			ExternalRefs.Add(Ref.ToString());
		}

		if (bRequireZeroReferencers && ExternalRefs.Num() > 0)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("path"), AssetPath);
			Obj->SetNumberField(TEXT("external_referencer_count"), ExternalRefs.Num());
			Obj->SetArrayField(TEXT("external_referencers"), mcp_common::StringsToJson(ExternalRefs));
			RefusedReferenced.Add(MakeShared<FJsonValueObject>(Obj));
			ResultErrors[Idx] = FString::Printf(
				TEXT("REFUSED: '%s' still has %d referencer(s) outside this batch (require_zero_referencers=true)."),
				*PackageName, ExternalRefs.Num());
			continue;
		}

		FEligibleItem Item;
		Item.Index = Idx;
		Item.Path = AssetPath;
		Item.PackageName = PackageName;
		Item.ObjectPath = MakeObjectPath_EditorAuto(AssetPath);
		Item.ExternalRefCount = ExternalRefs.Num();
		Item.ExternalRefs = MoveTemp(ExternalRefs);
		Item.bOnDisk = FPackageName::DoesPackageExist(PackageName);
		Eligible.Add(MoveTemp(Item));
	}

	const int32 RefusedCount = RefusedReadOnly.Num() + RefusedProtected.Num() + RefusedReferenced.Num() + RefusedOpenWorld.Num();

	// ---------------------------------------------------------------------
	// 3. dry_run: report, delete nothing.
	// ---------------------------------------------------------------------
	TArray<TSharedPtr<FJsonValue>> WouldDelete;
	TArray<TSharedPtr<FJsonValue>> DeletedEntries;
	int32 DeletedCount = 0;
	int32 FailedCount  = 0;
	// What ObjectTools CLAIMS it deleted. Reported side by side with deleted_count (what the
	// disk CONFIRMS) precisely so a divergence between the two is visible instead of hidden.
	int32 ForceDeleteReportedCount = 0;

	for (const FEligibleItem& Item : Eligible)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("path"), Item.Path);
		Obj->SetStringField(TEXT("package_name"), Item.PackageName);
		Obj->SetNumberField(TEXT("external_referencer_count"), Item.ExternalRefCount);
		Obj->SetArrayField(TEXT("external_referencers"), mcp_common::StringsToJson(Item.ExternalRefs));
		Obj->SetBoolField(TEXT("on_disk"), Item.bOnDisk);
		WouldDelete.Add(MakeShared<FJsonValueObject>(Obj));
	}

	if (bDryRun)
	{
		for (const FEligibleItem& Item : Eligible)
		{
			ResultErrors[Item.Index] = TEXT("dry_run=true (the default): nothing was deleted.");
		}
	}
	else
	{
		// -----------------------------------------------------------------
		// 4. Resolve every eligible object BEFORE deleting -- ObjectTools needs live
		//    UObject*s, and resolving here (instead of inside
		//    UEditorAssetSubsystem::DeleteAsset) sidesteps the LoadAssetFromPath /
		//    EnsureAssetsLoaded preamble, which crashes the editor deterministically on
		//    corrupted leftovers ("Array index out of bounds").
		// -----------------------------------------------------------------
		TArray<UObject*> ObjectsToDelete;
		TArray<int32> ResolvedEligibleIdx;   // indices into Eligible, parallel to ObjectsToDelete
		TArray<int32> UnresolvedEligibleIdx;

		for (int32 EIdx = 0; EIdx < Eligible.Num(); ++EIdx)
		{
			const FEligibleItem& Item = Eligible[EIdx];
			UObject* Obj = StaticFindObject(UObject::StaticClass(), nullptr, *Item.ObjectPath);
			if (!Obj && Item.bOnDisk)
			{
				// On disk but not resident -- safe to load (the file exists, so this cannot
				// create a phantom package). Same guarded order delete_asset uses
				// (ManagementCommands.cpp).
				Obj = StaticLoadObject(UObject::StaticClass(), nullptr, *Item.ObjectPath);
			}
			if (Obj)
			{
				ObjectsToDelete.Add(Obj);
				ResolvedEligibleIdx.Add(EIdx);
			}
			else
			{
				UnresolvedEligibleIdx.Add(EIdx);
			}
		}

		// One batched call: ForceDeleteObjects nulls references BETWEEN the objects it is
		// given, which is exactly what makes deleting a dead subgraph in one shot work.
		// bShowConfirmation=false is mandatory (true opens a modal and kills the MCP session).
		if (ObjectsToDelete.Num() > 0)
		{
			ForceDeleteReportedCount = ObjectTools::ForceDeleteObjects(ObjectsToDelete, /*bShowConfirmation=*/false);
		}
		// NOTE: every UObject* in ObjectsToDelete may be dangling from here on. Only the
		// cached strings are touched below.

		for (const int32 EIdx : ResolvedEligibleIdx)
		{
			const FEligibleItem& Item = Eligible[EIdx];
			const bool bFileStillExists  = FPackageName::DoesPackageExist(Item.PackageName);
			const bool bStillLoaded      = (StaticFindObject(UObject::StaticClass(), nullptr, *Item.ObjectPath) != nullptr);
			// The oracle must rest on evidence that EXISTS for this item. A never-saved,
			// registry-visible asset was never on disk, so DoesPackageExist returns false both
			// before AND after the delete -- "verified gone from disk" is vacuously true and
			// would score a success for an object that is demonstrably still resident. Require
			// BOTH probes: ForceDeleteObjects renames survivors out of their package, so after a
			// real delete StaticFindObject on the ORIGINAL object path must also fail. On the
			// most destructive tool in the plugin, a counter must count what it claims to count.
			const bool bReallyDeleted    = !bFileStillExists && !bStillLoaded;

			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("path"), Item.Path);
			Obj->SetStringField(TEXT("package_name"), Item.PackageName);
			Obj->SetBoolField(TEXT("deleted"), bReallyDeleted);
			Obj->SetBoolField(TEXT("still_loaded"), bStillLoaded);
			if (!bReallyDeleted)
			{
				// Name WHICH probe still sees it -- "still on disk" and "still resident in
				// memory" have different causes (read-only file / in use vs. a live reference
				// holding the object), and collapsing them sends the reader down the wrong path.
				const FString Witness = bFileStillExists
					? (bStillLoaded
						? TEXT("the package file is still on disk AND the object still resolves at its original path")
						: TEXT("the package file is still on disk"))
					: TEXT("the object still resolves at its original path (never-saved asset: residency is the only available evidence)");
				Obj->SetStringField(TEXT("error"), FString::Printf(TEXT("ForceDeleteObjects ran but %s (asset in use / referenced / read-only file)."), *Witness));
				Obj->SetBoolField(TEXT("was_on_disk"), Item.bOnDisk);
				ResultErrors[Item.Index] = FString::Printf(TEXT("Delete did not take: %s after ForceDeleteObjects."), *Witness);
				++FailedCount;
			}
			else
			{
				ResultSuccess[Item.Index] = true;
				++DeletedCount;
			}
			DeletedEntries.Add(MakeShared<FJsonValueObject>(Obj));
		}

		for (const int32 EIdx : UnresolvedEligibleIdx)
		{
			const FEligibleItem& Item = Eligible[EIdx];
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("path"), Item.Path);
			Obj->SetStringField(TEXT("package_name"), Item.PackageName);
			Obj->SetBoolField(TEXT("deleted"), false);
			Obj->SetBoolField(TEXT("still_loaded"), false);
			Obj->SetStringField(TEXT("error"), Item.bOnDisk
				? TEXT("Package exists on disk but the object could not be loaded.")
				: TEXT("No such package on disk and no such object in memory."));
			ResultErrors[Item.Index] = Item.bOnDisk
				? TEXT("Package exists on disk but the object could not be loaded.")
				: TEXT("No such package on disk and no such object in memory.");
			DeletedEntries.Add(MakeShared<FJsonValueObject>(Obj));
			++FailedCount;
		}
	}

	// ---------------------------------------------------------------------
	// 5. Response. `results` / `deleted_count` / `failed_count` / `recursive`
	//    keep the shape callers rely on; the rest are extras.
	// ---------------------------------------------------------------------
	TArray<TSharedPtr<FJsonValue>> ResultEntries;
	for (int32 Idx = 0; Idx < AllPaths.Num(); ++Idx)
	{
		TSharedPtr<FJsonObject> ResultEntry = MakeShared<FJsonObject>();
		ResultEntry->SetStringField(TEXT("path"), AllPaths[Idx]);
		ResultEntry->SetBoolField(TEXT("success"), ResultSuccess[Idx]);
		ResultEntry->SetStringField(TEXT("error"), ResultSuccess[Idx] ? FString() : ResultErrors[Idx]);
		ResultEntries.Add(MakeShared<FJsonValueObject>(ResultEntry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("requested_count"), PathsArray->Num());
	Data->SetNumberField(TEXT("expanded_count"), AllPaths.Num());
	Data->SetBoolField(TEXT("recursive"), bRecursive);
	Data->SetBoolField(TEXT("dry_run"), bDryRun);
	Data->SetArrayField(TEXT("readonly_roots"), mcp_common::StringsToJson(ReadOnlyRoots));
	Data->SetArrayField(TEXT("protect_paths"), mcp_common::StringsToJson(ProtectPaths));
	Data->SetBoolField(TEXT("require_zero_referencers"), bRequireZeroReferencers);
	Data->SetStringField(TEXT("dependency_type"), DependencyType);
	Data->SetArrayField(TEXT("expanded_folders"), ExpandedFolders);
	Data->SetNumberField(TEXT("eligible_count"), Eligible.Num());
	Data->SetArrayField(TEXT("would_delete"), WouldDelete);
	Data->SetArrayField(TEXT("deleted"), DeletedEntries);
	Data->SetNumberField(TEXT("deleted_count"), DeletedCount);
	Data->SetNumberField(TEXT("failed_count"), FailedCount);
	Data->SetNumberField(TEXT("force_delete_reported_count"), ForceDeleteReportedCount);
	Data->SetArrayField(TEXT("refused_readonly_root"), RefusedReadOnly);
	Data->SetArrayField(TEXT("refused_protected"), RefusedProtected);
	Data->SetArrayField(TEXT("refused_referenced"), RefusedReferenced);
	Data->SetArrayField(TEXT("refused_open_world"), RefusedOpenWorld);
	Data->SetNumberField(TEXT("refused_count"), RefusedCount);
	Data->SetArrayField(TEXT("results"), ResultEntries);
	Data->SetStringField(TEXT("note"), bDryRun
		? TEXT("dry_run defaults to TRUE for this tool: nothing was deleted. would_delete lists what WOULD go; re-run with dry_run=false to actually delete.")
		: TEXT("deleted_count counts ONLY items where BOTH post-delete probes agree: the package file is gone (DoesPackageExist) AND the object no longer resolves at its original path (StaticFindObject). Not the API's return value. For a never-saved package the disk probe is vacuous, so residency is the evidence that counts. The AssetRegistry cache can still show stale entries until its next scan."));

	// Honest outcome: refusals are a non-result, so they must not read as success.
	const bool bOk = (RefusedCount == 0) && (FailedCount == 0);
	FString ErrorMessage;
	if (!bOk)
	{
		ErrorMessage = FString::Printf(
			TEXT("%d of %d path(s) refused by a guard (readonly_root=%d, protected=%d, referenced=%d, open_world=%d) and %d failed to delete; see the refused_* arrays."),
			RefusedCount, AllPaths.Num(), RefusedReadOnly.Num(), RefusedProtected.Num(),
			RefusedReferenced.Num(), RefusedOpenWorld.Num(), FailedCount);
	}

	if (bDryRun)
	{
		return mcp_common::MakeVerifiedReply(
			Data, bOk, mcp_common::EMcpVerification::NotAttempted,
			TEXT("dry_run"),
			TEXT("dry_run=true: no deletion was attempted, so there is nothing to verify on disk."),
			ErrorMessage);
	}

	return mcp_common::MakeVerifiedReply(
		Data, bOk,
		(Eligible.Num() > 0 && FailedCount == 0) ? mcp_common::EMcpVerification::Verified
		: (Eligible.Num() == 0 ? mcp_common::EMcpVerification::NotAttempted : mcp_common::EMcpVerification::Failed),
		TEXT("FPackageName::DoesPackageExist + StaticFindObject per item"),
		FString::Printf(TEXT("%d of %d eligible item(s) confirmed gone by BOTH probes (file absent AND object no longer resolving) after ForceDeleteObjects."),
			DeletedCount, Eligible.Num()),
		ErrorMessage);
}

// ---------------------------------------------------------------------------
// 3. find_unused_assets
// ---------------------------------------------------------------------------
// MCP:find_unused_assets - Find assets in a folder with zero referencers, and annotate each survivor with the reasons it should be KEPT anyway. "Zero referencers" is not the same as "safe to delete": there are three common ways an asset stays alive with an empty referencer set -- it is explicitly protected, a Config/Default*.ini names it, or it is a fork descending from a vendored base library. Each becomes a keep_reason string on the entry; deletion_candidates lists only the entries with NO keep_reason. Nothing is ever removed from unused_assets by these flags (hiding a genuinely unreferenced asset would be the same lie in the other direction). NOTE: there is deliberately no `is_world` -- it is already visible in class_name ("World").
//   @param folder_path - Content folder to scan, e.g. /Game/Unused
//   @param recursive - If true, scan subfolders, default true
//   @param exclude_classes - Optional list of class name fragments to skip
//   @param dependency_type - Referencer filter: "hard"|"soft"|"all" (case-insensitive), default "all" (identical behavior to omitting it)
//   @param protect_paths [array] - Wildcard patterns (FString::MatchesWildcard, case-insensitive, `*` spans '/') matched against the package name and the object path. Default [] . A match adds keep_reason "protected_path".
//   @param include_lineage_reasons - Default FALSE because it LOADS every candidate Blueprint. When true, a Blueprint whose super chain reaches a class owned by one of base_libraries gets keep_reason "descends_from_base_library" -- the only signal that saves a fork carrying no fork-origin tag (project setting Fork Origin Tag).
//   @param base_libraries [array] - Vendored-library content roots for the lineage check. Default: the project's Read Only Content Roots (same default as audit_class_purity).
//   @returns { folder_path, folder_exists, recursive, dependency_type, protect_paths: [string], include_lineage_reasons, base_libraries: [string], unused_assets: [{path, class_name, size_kb, keep_reasons: [string], keep_reason_detail: {...}}], unused_count, total_scanned, deletion_candidates: [string], deletion_candidate_count, assets_with_keep_reason, keep_reason_counts: {protected_path, referenced_by_config, descends_from_base_library}, config_scan: {files_scanned: int, files: [string]}, lineage_scan: {attempted, blueprints_loaded, skipped_non_blueprint, load_failures} }
TSharedPtr<FJsonObject> HandleFindUnusedAssets(
	const TSharedPtr<FJsonObject>& Params)
{
	FString FolderPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("folder_path"), FolderPath)) { return Missing; }

	bool bRecursive = true;
	Params->TryGetBoolField(TEXT("recursive"), bRecursive);

	TArray<FString> ProtectPaths;
	ParseStringArrayParam_EditorAuto(Params, TEXT("protect_paths"), ProtectPaths);

	bool bIncludeLineageReasons = false;
	Params->TryGetBoolField(TEXT("include_lineage_reasons"), bIncludeLineageReasons);

	const TArray<FString> BaseLibraries = ParseBaseLibraries_EditorAuto(Params);

	// dependency_type: "hard"|"soft"|"all" (case-insensitive). Default/omitted/"all" is the
	// plain GetReferencers behavior exactly (default-constructed FDependencyQuery =
	// NoRequirements = mixes hard+soft+every category).
	FString DependencyType = TEXT("all");
	Params->TryGetStringField(TEXT("dependency_type"), DependencyType);

	// Optional class exclusion list
	TArray<FString> ExcludeClasses;
	const TArray<TSharedPtr<FJsonValue>>* ExcludeArr = nullptr;
	if (Params->TryGetArrayField(TEXT("exclude_classes"), ExcludeArr) && ExcludeArr)
	{
		for (const TSharedPtr<FJsonValue>& Val : *ExcludeArr)
		{
			FString Cls = Val->AsString();
			if (!Cls.IsEmpty())
			{
				ExcludeClasses.Add(Cls.ToLower());
			}
		}
	}

	UEditorAssetSubsystem* AssetSubsystem = GetEditorAssetSubsystem();
	if (!AssetSubsystem)
	{
		return mcp_common::MakeErrorReply(TEXT("EditorAssetSubsystem is not available"));
	}

	IAssetRegistry* Registry = &mcp_common::AssetRegistry();
	// Without the registry every asset would report RefCount == 0 -- i.e. the tool would
	// answer "all of them are unused" and be believed. Refuse instead of lying.
	if (!Registry)
	{
		return mcp_common::MakeErrorReply(TEXT(
			"AssetRegistry is not available: referencer counts cannot be computed, so every asset would be "
			"mis-reported as unused. Refusing to answer."));
	}

	// GetReferencers takes (FName, TArray<FName>&, EDependencyCategory, FDependencyQuery).
	const UE::AssetRegistry::FDependencyQuery Query = EditorAuto_DependencyQuery(DependencyType);

	// List all assets in the folder
	TArray<FString> AllAssets = AssetSubsystem->ListAssets(FolderPath, bRecursive, false);
	// An empty result is ambiguous (empty folder vs. typo'd path) -- say which. The
	// `AllAssets.Num() > 0` fallback keeps a spelling PathExists rejects but ListAssets
	// resolved (trailing slash, case) from being reported as a nonexistent folder.
	const bool bFolderExists = (AllAssets.Num() > 0) || Registry->PathExists(FolderPath);

	// Config/Default*.ini blob, read once per call. Cheap (a handful of small ini files) and
	// scoped to version-controlled project intent only -- never Saved/Config.
	TArray<FString> ConfigFilesScanned;
	const FString ConfigBlob = LoadDefaultConfigBlob_EditorAuto(ConfigFilesScanned);

	const FTopLevelAssetPath BlueprintClassPath = UBlueprint::StaticClass()->GetClassPathName();

	TArray<TSharedPtr<FJsonValue>> UnusedEntries;
	TArray<TSharedPtr<FJsonValue>> DeletionCandidates;
	int32 ProtectedCount = 0, ConfigCount = 0, LineageCount = 0, AssetsWithKeepReason = 0;
	int32 LineageBlueprintsLoaded = 0, LineageSkippedNonBlueprint = 0, LineageLoadFailures = 0;

	for (const FString& AssetPath : AllAssets)
	{
		// Determine class name from the asset registry
		FName PkgName = AssetPathToPackageName(AssetPath);
		FString ClassName = TEXT("Unknown");

		// IAssetRegistry::GetAssetByObjectPath(const FSoftObjectPath&, ...) returns
		// an FAssetData by value; validate via FAssetData::IsValid().
		FAssetData AssetData = mcp_common::FindAssetDataByPath(AssetPath);
		if (AssetData.IsValid())
		{
			// FTopLevelAssetPath::GetAssetName() returns an FName.
			ClassName = AssetData.AssetClassPath.GetAssetName().ToString();
		}

		// Check exclusion
		bool bExcluded = false;
		for (const FString& ExCl : ExcludeClasses)
		{
			if (ClassName.ToLower().Contains(ExCl))
			{
				bExcluded = true;
				break;
			}
		}
		if (bExcluded) { continue; }

		// Check referencers
		TArray<FName> Referencers;
		Registry->GetReferencers(PkgName, Referencers, UE::AssetRegistry::EDependencyCategory::Package, Query);
		if (Referencers.Num() != 0)
		{
			continue;
		}

		// ------------------------------------------------------------------
		// Zero referencers -- now collect every reason to KEEP it anyway.
		// ------------------------------------------------------------------
		const FString PackageNameStr = PkgName.ToString();
		TArray<TSharedPtr<FJsonValue>> KeepReasons;
		TSharedPtr<FJsonObject> KeepDetail = MakeShared<FJsonObject>();

		FString MatchedPattern;
		if (MatchesAnyWildcard_EditorAuto(PackageNameStr, ProtectPaths, MatchedPattern)
			|| MatchesAnyWildcard_EditorAuto(AssetPath, ProtectPaths, MatchedPattern))
		{
			KeepReasons.Add(MakeShared<FJsonValueString>(TEXT("protected_path")));
			KeepDetail->SetStringField(TEXT("matched_pattern"), MatchedPattern);
			++ProtectedCount;
		}

		if (!ConfigBlob.IsEmpty() && ConfigBlob.Contains(PackageNameStr))
		{
			KeepReasons.Add(MakeShared<FJsonValueString>(TEXT("referenced_by_config")));
			++ConfigCount;
		}

		if (bIncludeLineageReasons)
		{
			if (AssetData.IsValid() && AssetData.AssetClassPath == BlueprintClassPath)
			{
				// GetAsset() LOADS the package -- this is exactly why the whole branch is
				// behind include_lineage_reasons.
				UBlueprint* BP = Cast<UBlueprint>(AssetData.GetAsset());
				if (BP && BP->GeneratedClass)
				{
					++LineageBlueprintsLoaded;
					FString MatchedLibrary, MatchedAncestor;
					if (ClassDescendsFromBaseLibrary_EditorAuto(BP->GeneratedClass, BaseLibraries, MatchedLibrary, MatchedAncestor))
					{
						KeepReasons.Add(MakeShared<FJsonValueString>(TEXT("descends_from_base_library")));
						KeepDetail->SetStringField(TEXT("base_library"), MatchedLibrary);
						KeepDetail->SetStringField(TEXT("base_library_ancestor"), MatchedAncestor);
						++LineageCount;
					}
				}
				else
				{
					++LineageLoadFailures;
				}
			}
			else
			{
				++LineageSkippedNonBlueprint;
			}
		}

		if (KeepReasons.Num() > 0)
		{
			++AssetsWithKeepReason;
		}
		else
		{
			DeletionCandidates.Add(MakeShared<FJsonValueString>(AssetPath));
		}

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("path"), AssetPath);
		Entry->SetStringField(TEXT("class_name"), ClassName);
		// PackageNameStr, not AssetPath. UEditorAssetSubsystem::ListAssets returns full
		// OBJECT paths ("/Game/A/B.B", EditorAssetSubsystem.cpp), and GetAssetSizeKB feeds
		// its argument straight to FPackageName::DoesPackageExist -- which rejects an object
		// path, so with AssetPath size_kb would be 0 for every entry.
		Entry->SetNumberField(TEXT("size_kb"), GetAssetSizeKB(PackageNameStr));
		Entry->SetArrayField(TEXT("keep_reasons"), KeepReasons);
		Entry->SetObjectField(TEXT("keep_reason_detail"), KeepDetail);
		UnusedEntries.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> KeepReasonCounts = MakeShared<FJsonObject>();
	KeepReasonCounts->SetNumberField(TEXT("protected_path"), ProtectedCount);
	KeepReasonCounts->SetNumberField(TEXT("referenced_by_config"), ConfigCount);
	KeepReasonCounts->SetNumberField(TEXT("descends_from_base_library"), LineageCount);

	TSharedPtr<FJsonObject> ConfigScan = MakeShared<FJsonObject>();
	ConfigScan->SetNumberField(TEXT("files_scanned"), ConfigFilesScanned.Num());
	ConfigScan->SetArrayField(TEXT("files"), mcp_common::StringsToJson(ConfigFilesScanned));

	TSharedPtr<FJsonObject> LineageScan = MakeShared<FJsonObject>();
	LineageScan->SetBoolField(TEXT("attempted"), bIncludeLineageReasons);
	LineageScan->SetNumberField(TEXT("blueprints_loaded"), LineageBlueprintsLoaded);
	LineageScan->SetNumberField(TEXT("skipped_non_blueprint"), LineageSkippedNonBlueprint);
	LineageScan->SetNumberField(TEXT("load_failures"), LineageLoadFailures);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("folder_path"), FolderPath);
	Data->SetBoolField(TEXT("folder_exists"), bFolderExists);
	Data->SetBoolField(TEXT("recursive"), bRecursive);
	Data->SetStringField(TEXT("dependency_type"), DependencyType);
	Data->SetArrayField(TEXT("protect_paths"), mcp_common::StringsToJson(ProtectPaths));
	Data->SetBoolField(TEXT("include_lineage_reasons"), bIncludeLineageReasons);
	Data->SetArrayField(TEXT("base_libraries"), mcp_common::StringsToJson(BaseLibraries));
	Data->SetArrayField(TEXT("unused_assets"), UnusedEntries);
	Data->SetNumberField(TEXT("unused_count"), UnusedEntries.Num());
	Data->SetNumberField(TEXT("total_scanned"), AllAssets.Num());
	Data->SetArrayField(TEXT("deletion_candidates"), DeletionCandidates);
	Data->SetNumberField(TEXT("deletion_candidate_count"), DeletionCandidates.Num());
	Data->SetNumberField(TEXT("assets_with_keep_reason"), AssetsWithKeepReason);
	Data->SetObjectField(TEXT("keep_reason_counts"), KeepReasonCounts);
	Data->SetObjectField(TEXT("config_scan"), ConfigScan);
	Data->SetObjectField(TEXT("lineage_scan"), LineageScan);
	if (!bFolderExists)
	{
		Data->SetStringField(TEXT("note"), TEXT(
			"folder_path is not a known content path in the AssetRegistry -- total_scanned=0 here means "
			"'no such folder', NOT 'folder is clean'."));
	}

	// Read-only tool: the honest failure mode is "the folder you named does not exist".
	return mcp_common::MakeOutcomeReply(Data, bFolderExists,
		bFolderExists ? FString() : FString::Printf(
			TEXT("folder_path '%s' does not exist in the AssetRegistry; nothing was scanned."), *FolderPath));
}

// ---------------------------------------------------------------------------
// 5. export_assets
// ---------------------------------------------------------------------------
// MCP:export_assets - Export assets to files via IAssetTools::ExportAssets (preferred format, FBX/PNG/EXR first), each written to export_path/<package path without its leading slash>.<extension>, e.g. D:/Exports/Game/Meshes/SM_Rock.FBX. What was written is measured per asset on disk: exported lists each asset with its file, and not_exported the ones that did not load, have no exporter or failed to write. Succeeds only when every requested asset was written.
//   @param asset_paths - Asset object or package paths to export (e.g. ["/Game/Meshes/SM_Rock"]); empty strings are ignored.
//   @param export_path - Destination directory on disk (e.g. "D:/Exports"), created if missing; each file lands in subfolders named after its package path.
//   @returns { exported_count: int, failed_count: int, requested_count: int, export_path: string, exported: [{ asset_path: string, file: string }], not_exported: [string] }
TSharedPtr<FJsonObject> HandleExportAssets(
	const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* PathsArray = nullptr;
	if (TSharedPtr<FJsonObject> Missing = EditorAuto_ReadAssetPaths(Params, PathsArray)) { return Missing; }

	FString ExportPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("export_path"), ExportPath)) { return Missing; }

	if (!FAssetToolsModule::IsModuleLoaded())
	{
		return mcp_common::MakeErrorReply(TEXT("AssetTools module is not loaded"));
	}

	IAssetTools& AssetTools = FAssetToolsModule::GetModule().Get();

	// Build the string array of asset paths
	TArray<FString> AssetPaths;
	for (const TSharedPtr<FJsonValue>& Val : *PathsArray)
	{
		FString P = Val->AsString();
		if (!P.IsEmpty())
		{
			AssetPaths.Add(P);
		}
	}

	if (AssetPaths.IsEmpty())
	{
		return mcp_common::MakeErrorReply(TEXT("asset_paths holds no non-empty path"));
	}

	// Ensure the export directory exists
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	PlatformFile.CreateDirectoryTree(*ExportPath);

	// ExportAssets returns nothing and skips, with only a log line, an asset that does not load
	// or has no exporter. It writes each asset to <export_path>/<package path without its leading
	// slash>.<extension the exporter picks>, so what it wrote is measured there, per asset: a file
	// named after the package whose timestamp moved during the call (a re-export rewrites it).
	struct FExportTarget
	{
		FString AssetPath;
		FString Folder;
		FString Pattern;
		TMap<FString, FDateTime> Before;
	};
	IFileManager& Files = IFileManager::Get();
	auto Stamps = [&Files](const FExportTarget& Target)
	{
		TArray<FString> Names;
		Files.FindFiles(Names, *(Target.Folder / Target.Pattern), /*Files=*/true, /*Directories=*/false);
		TMap<FString, FDateTime> Found;
		for (const FString& Name : Names)
		{
			Found.Add(Name, Files.GetTimeStamp(*(Target.Folder / Name)));
		}
		return Found;
	};
	TArray<FExportTarget> Targets;
	for (const FString& AssetPath : AssetPaths)
	{
		FString Relative = FPackageName::ObjectPathToPackageName(AssetPath);
		Relative.RemoveFromStart(TEXT("/"));
		FExportTarget& Target = Targets.AddDefaulted_GetRef();
		Target.AssetPath = AssetPath;
		Target.Folder = FPaths::Combine(ExportPath, FPaths::GetPath(Relative));
		Target.Pattern = FPaths::GetCleanFilename(Relative) + TEXT(".*");
		Target.Before = Stamps(Target);
	}

	AssetTools.ExportAssets(AssetPaths, ExportPath);

	TArray<TSharedPtr<FJsonValue>> Exported;
	TArray<TSharedPtr<FJsonValue>> NotExported;
	for (const FExportTarget& Target : Targets)
	{
		FString Written;
		for (const TPair<FString, FDateTime>& File : Stamps(Target))
		{
			const FDateTime* Before = Target.Before.Find(File.Key);
			if (!Before || *Before != File.Value)
			{
				Written = Target.Folder / File.Key;
				break;
			}
		}
		if (Written.IsEmpty())
		{
			NotExported.Add(MakeShared<FJsonValueString>(Target.AssetPath));
			continue;
		}
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("asset_path"), Target.AssetPath);
		Entry->SetStringField(TEXT("file"), Written);
		Exported.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("exported_count"), Exported.Num());
	Data->SetNumberField(TEXT("failed_count"), NotExported.Num());
	Data->SetNumberField(TEXT("requested_count"), AssetPaths.Num());
	Data->SetStringField(TEXT("export_path"), ExportPath);
	Data->SetArrayField(TEXT("exported"), Exported);
	Data->SetArrayField(TEXT("not_exported"), NotExported);
	const bool bAllExported = NotExported.IsEmpty();
	return mcp_common::MakeOutcomeReply(Data, bAllExported, bAllExported ? FString() : FString::Printf(
		TEXT("export_assets wrote %d of %d assets: the ones in not_exported did not load, have no exporter, or failed to write (the editor log says which)."),
		Exported.Num(), AssetPaths.Num()));
}

// ---------------------------------------------------------------------------
// 6. migrate_assets
// ---------------------------------------------------------------------------
// MCP:migrate_assets - Copy assets, and their dependencies unless ignore_dependencies, into another project's Content folder with IAssetTools::MigratePackages, with no prompt and skipping files that already exist there. The engine first saves every dirty package of this editor, and migrates nothing if one of them fails to save. What was copied is measured from the migration's own package list against the disk: migrated lists each package written (dependencies included), skipped_existing the requested ones the destination already had, failed the requested ones neither written nor skipped. Succeeds when nothing requested failed. destination_path must be a Content folder with a .uproject (or one .uplugin) next to it, or the engine migrates nothing and the call fails.
//   @param asset_paths - Asset object or package paths to migrate (e.g. ["/Game/Props/SM_Crate"]); each is reduced to its package name.
//   @param destination_path - Target content folder, e.g. D:/OtherProject/Content
//   @param ignore_dependencies - When true only the listed packages are copied, without their dependencies (default false).
//   @returns { migrated_count: int, failed_count: int, requested_count: int, destination_path: string, ignore_dependencies: bool, migrated: [{ package: string, file: string }], skipped_existing: [string], failed: [string], note: string? }
TSharedPtr<FJsonObject> HandleMigrateAssets(
	const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* PathsArray = nullptr;
	if (TSharedPtr<FJsonObject> Missing = EditorAuto_ReadAssetPaths(Params, PathsArray)) { return Missing; }

	FString DestinationPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("destination_path"), DestinationPath)) { return Missing; }

	if (!FAssetToolsModule::IsModuleLoaded())
	{
		return mcp_common::MakeErrorReply(TEXT("AssetTools module is not loaded"));
	}

	IAssetTools& AssetTools = FAssetToolsModule::GetModule().Get();

	bool bIgnoreDeps = false;
	Params->TryGetBoolField(TEXT("ignore_dependencies"), bIgnoreDeps);

	// Build package name array
	TArray<FName> PackageNames;
	for (const TSharedPtr<FJsonValue>& Val : *PathsArray)
	{
		FString P = Val->AsString();
		if (!P.IsEmpty())
		{
			PackageNames.Add(AssetPathToPackageName(P));
		}
	}

	// Configure migration options
	FMigrationOptions Options;
	Options.bPrompt = false;
	Options.bIgnoreDependencies = bIgnoreDeps;
	Options.AssetConflict = EAssetMigrationConflict::Skip;

	if (PackageNames.IsEmpty())
	{
		return mcp_common::MakeErrorReply(TEXT("asset_paths holds no non-empty path"));
	}

	// MigratePackages returns nothing. The package migration the engine uses by default
	// (AssetTools.UseNewPackageMigration) announces each step on GetOnPackageMigration, and once
	// the packages are saved its context lists every package it migrates -- the requested ones
	// and their dependencies -- with the file it was to write and whether it was to write it at
	// all (false for one the destination already has). That list, read against the disk, is what
	// this call wrote: a file is the call's when its timestamp is newer than the call.
	using UE::AssetTools::FPackageMigrationContext;
	struct FMigrationEntry
	{
		FString PackageName;
		FString Filename;
		bool bToWrite = false;
	};
	TArray<FMigrationEntry> Entries;
	bool bMigrationRan = false;
	const FDelegateHandle Listener = AssetTools.GetOnPackageMigration().AddLambda(
		[&Entries, &bMigrationRan](FPackageMigrationContext& Context)
		{
			bMigrationRan = true;
			if (Context.GetCurrentStep() == FPackageMigrationContext::EPackageMigrationStep::InstancedPackagesSaved)
			{
				for (const FPackageMigrationContext::FMigrationPackageData& Package : Context.GetMigrationPackagesData())
				{
					Entries.Add({ Package.GetOriginalPackageName(), Package.GetDestinationFilename(), Package.bNeedToBeSaveMigrated });
				}
			}
		});
	// The slack covers file systems that keep timestamps to the second or two.
	const FDateTime CallStart = FDateTime::UtcNow() - FTimespan::FromSeconds(2.0);
	AssetTools.MigratePackages(PackageNames, DestinationPath, Options);
	AssetTools.GetOnPackageMigration().Remove(Listener);

	IFileManager& Files = IFileManager::Get();
	TArray<TSharedPtr<FJsonValue>> Migrated;
	TSet<FString> Written;
	TSet<FString> AlreadyThere;
	for (const FMigrationEntry& Entry : Entries)
	{
		if (Entry.bToWrite && Files.GetTimeStamp(*Entry.Filename) >= CallStart)
		{
			Written.Add(Entry.PackageName);
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("package"), Entry.PackageName);
			Row->SetStringField(TEXT("file"), Entry.Filename);
			Migrated.Add(MakeShared<FJsonValueObject>(Row));
		}
		else if (!Entry.bToWrite && Files.FileExists(*Entry.Filename))
		{
			AlreadyThere.Add(Entry.PackageName);
		}
	}
	TArray<TSharedPtr<FJsonValue>> SkippedExisting;
	TArray<TSharedPtr<FJsonValue>> Failed;
	for (const FName& Requested : PackageNames)
	{
		const FString Name = Requested.ToString();
		if (!Written.Contains(Name))
		{
			(AlreadyThere.Contains(Name) ? SkippedExisting : Failed).Add(MakeShared<FJsonValueString>(Name));
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("migrated_count"), Migrated.Num());
	Data->SetNumberField(TEXT("failed_count"), Failed.Num());
	Data->SetNumberField(TEXT("requested_count"), PackageNames.Num());
	Data->SetStringField(TEXT("destination_path"), DestinationPath);
	Data->SetBoolField(TEXT("ignore_dependencies"), bIgnoreDeps);
	Data->SetArrayField(TEXT("migrated"), Migrated);
	Data->SetArrayField(TEXT("skipped_existing"), SkippedExisting);
	Data->SetArrayField(TEXT("failed"), Failed);

	if (!bMigrationRan)
	{
		const IConsoleVariable* NewMigration = IConsoleManager::Get().FindConsoleVariable(TEXT("AssetTools.UseNewPackageMigration"));
		if (NewMigration && !NewMigration->GetBool())
		{
			// The legacy migration announces nothing, so what it copied cannot be measured here.
			Data->SetStringField(TEXT("note"), TEXT("AssetTools.UseNewPackageMigration is 0: the legacy migration ran and reports nothing per package, so what it copied was not measured; check destination_path."));
			return mcp_common::MakeVerifiedReply(Data, true, mcp_common::EMcpVerification::NotVerifiable,
				TEXT("legacy package migration"), TEXT("the legacy migration exposes no per-package result"));
		}
		Data->SetStringField(TEXT("note"), TEXT("The engine did not start the migration (see the editor log)."));
		return mcp_common::MakeOutcomeReply(Data, false, TEXT("migrate_assets: the engine did not start the migration, so nothing was copied. ")
			TEXT("destination_path must be a Content folder with a .uproject, or exactly one .uplugin, next to it, and writable; ")
			TEXT("the engine first saves every dirty package and stops if one fails to save; and it waits, without migrating, ")
			TEXT("while the asset registry is still scanning."));
	}
	const bool bNothingFailed = Failed.IsEmpty();
	return mcp_common::MakeOutcomeReply(Data, bNothingFailed, bNothingFailed ? FString() : FString::Printf(
		TEXT("migrate_assets: %d of %d requested packages were neither written nor already at the destination (see failed): ")
		TEXT("a path that names no package, or a load or save that failed (the editor log says which)."),
		Failed.Num(), PackageNames.Num()));
}

// ---------------------------------------------------------------------------
// 7. consolidate_assets
// ---------------------------------------------------------------------------
// MCP:consolidate_assets - Redirect every reference to the source assets onto the target with UEditorAssetSubsystem::ConsolidateAssets, which DELETES the sources (leaving redirectors) and saves what it changed. Only class-compatible sources are used; others are listed in skipped_incompatible and unloadable ones are dropped silently. dry_run=true only reports.
//   @param target_asset_path - The asset to keep, all references point here
//   @param source_asset_paths - Assets to consolidate into the target
//   @param dry_run - If true, skip ConsolidateAssets entirely and only report what would happen, default false
//   @returns dry_run=true: { target_asset_path, target_class, dry_run: true, would_consolidate: [string], skipped_incompatible: [{path,class}], compatible_count: int, incompatible_count: int }
//   @returns dry_run=false (default): { target_asset_path, target_class, dry_run: false, consolidated_count: int, source_count: int, success: bool, skipped_incompatible: [{path,class}], incompatible_count: int }
TSharedPtr<FJsonObject> HandleConsolidateAssets(
	const TSharedPtr<FJsonObject>& Params)
{
	FString TargetPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("target_asset_path"), TargetPath)) { return Missing; }

	const TArray<TSharedPtr<FJsonValue>>* SourcesArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("source_asset_paths"), SourcesArray) || !SourcesArray || SourcesArray->Num() == 0)
	{
		return mcp_common::MakeErrorReply(TEXT("source_asset_paths array is required and must not be empty"));
	}

	bool bDryRun = false;
	Params->TryGetBoolField(TEXT("dry_run"), bDryRun);

	UEditorAssetSubsystem* AssetSubsystem = GetEditorAssetSubsystem();
	if (!AssetSubsystem)
	{
		return mcp_common::MakeErrorReply(TEXT("EditorAssetSubsystem is not available"));
	}

	// Load the target asset
	UObject* TargetAsset = LoadObject<UObject>(nullptr, *TargetPath);
	if (!TargetAsset)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Cannot load target asset: %s"), *TargetPath));
	}

	// Load all source assets, keeping the original path alongside each loaded object so the
	// class-compatibility partition below can report *which* path got skipped and why.
	TArray<TPair<FString, UObject*>> LoadedSources;
	for (const TSharedPtr<FJsonValue>& Val : *SourcesArray)
	{
		FString SrcPath = Val->AsString();
		if (SrcPath.IsEmpty()) { continue; }

		UObject* SrcObj = LoadObject<UObject>(nullptr, *SrcPath);
		if (SrcObj)
		{
			LoadedSources.Add(TPair<FString, UObject*>(SrcPath, SrcObj));
		}
	}

	if (LoadedSources.Num() == 0)
	{
		return mcp_common::MakeErrorReply(TEXT("None of the source assets could be loaded"));
	}

	// Class-compatibility partition: ConsolidateAssets blindly redirects every reference to the
	// source objects onto the target, regardless of class. Consolidating incompatible classes can
	// corrupt references, so only pass sources that are class-compatible with the target (a common
	// ancestor/descendant relationship either way) into ConsolidateAssets. Everything else is
	// reported as skipped_incompatible instead.
	TArray<UObject*> CompatibleSources;
	TArray<TSharedPtr<FJsonValue>> SkippedIncompatible;
	for (const TPair<FString, UObject*>& Pair : LoadedSources)
	{
		UObject* SrcObj = Pair.Value;
		const bool bCompatible = SrcObj->GetClass()->IsChildOf(TargetAsset->GetClass())
			|| TargetAsset->GetClass()->IsChildOf(SrcObj->GetClass());
		if (bCompatible)
		{
			CompatibleSources.Add(SrcObj);
		}
		else
		{
			TSharedPtr<FJsonObject> SkipEntry = MakeShared<FJsonObject>();
			SkipEntry->SetStringField(TEXT("path"), Pair.Key);
			SkipEntry->SetStringField(TEXT("class"), SrcObj->GetClass()->GetName());
			SkippedIncompatible.Add(MakeShared<FJsonValueObject>(SkipEntry));
		}
	}

	if (bDryRun)
	{
		TArray<TSharedPtr<FJsonValue>> WouldConsolidate;
		for (UObject* Obj : CompatibleSources)
		{
			WouldConsolidate.Add(MakeShared<FJsonValueString>(Obj->GetPathName()));
		}

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("target_asset_path"), TargetPath);
		Data->SetStringField(TEXT("target_class"), TargetAsset->GetClass()->GetName());
		Data->SetBoolField(TEXT("dry_run"), true);
		Data->SetArrayField(TEXT("would_consolidate"), WouldConsolidate);
		Data->SetArrayField(TEXT("skipped_incompatible"), SkippedIncompatible);
		Data->SetNumberField(TEXT("compatible_count"), CompatibleSources.Num());
		Data->SetNumberField(TEXT("incompatible_count"), SkippedIncompatible.Num());
		return mcp_common::MakeOkReply(Data);
	}

	// Perform consolidation with only the compatible sources -- never hand incompatible-class
	// objects to ConsolidateAssets.
	bool bSuccess = false;
	int32 ConsolidatedCount = 0;
	if (CompatibleSources.Num() > 0)
	{
		bSuccess = AssetSubsystem->ConsolidateAssets(TargetAsset, CompatibleSources);
		ConsolidatedCount = bSuccess ? CompatibleSources.Num() : 0;
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("target_asset_path"), TargetPath);
	Data->SetStringField(TEXT("target_class"), TargetAsset->GetClass()->GetName());
	Data->SetBoolField(TEXT("dry_run"), false);
	Data->SetNumberField(TEXT("consolidated_count"), ConsolidatedCount);
	Data->SetNumberField(TEXT("source_count"), LoadedSources.Num());
	Data->SetBoolField(TEXT("success"), bSuccess);
	Data->SetArrayField(TEXT("skipped_incompatible"), SkippedIncompatible);
	Data->SetNumberField(TEXT("incompatible_count"), SkippedIncompatible.Num());
	// The outcome measured above is the outcome EMITTED: the bridge only looks at the
	// envelope's success field.
	return mcp_common::MakeOutcomeReply(Data, bSuccess,
	    bSuccess ? FString() : mcp_common::MeasuredOutcomeError(TEXT("consolidate_assets"), TEXT("bSuccess")));
}

// ---------------------------------------------------------------------------
// 8. find_duplicate_assets
// ---------------------------------------------------------------------------
// MCP:find_duplicate_assets - Find groups of assets sharing the same base name under a folder tree (e.g. same name existing under both a vendored library tree and a first-party fork)
//   @param folder_path - Content folder to scan, default /Game
//   @param recursive - If true, scan subfolders, default true
//   @param min_group_size - Minimum number of same-named assets to report as a duplicate group, default 2
//   @param class_filter - Optional list of class short names (e.g. "StaticMesh","Material"); only assets whose AssetClassPath short name case-insensitively matches one of these are considered
//   @param max_results - Cap on the number of duplicate GROUPS returned (not raw assets), clamped 1..20000, default 2000
//   @returns { folder_path, recursive, class_filter, min_group_size, group_count, total_duplicate_assets, assets_scanned, max_results, results_truncated, groups: [{ name, count, classes: [string], assets: [{path, package, class}] }] } (groups sorted by count descending)
TSharedPtr<FJsonObject> HandleFindDuplicateAssets(
	const TSharedPtr<FJsonObject>& Params)
{
	FString FolderPath = TEXT("/Game");
	FString FolderPathParam;
	if (Params->TryGetStringField(TEXT("folder_path"), FolderPathParam) && !FolderPathParam.IsEmpty())
	{
		FolderPath = FolderPathParam;
	}

	bool bRecursive = true;
	Params->TryGetBoolField(TEXT("recursive"), bRecursive);

	int32 MinGroupSize = 2;
	Params->TryGetNumberField(TEXT("min_group_size"), MinGroupSize);
	if (MinGroupSize < 1)
	{
		MinGroupSize = 2;
	}

	TArray<FString> ClassFilter;
	const TArray<TSharedPtr<FJsonValue>>* ClassFilterArr = nullptr;
	if (Params->TryGetArrayField(TEXT("class_filter"), ClassFilterArr) && ClassFilterArr)
	{
		for (const TSharedPtr<FJsonValue>& Val : *ClassFilterArr)
		{
			FString Cls = Val->AsString();
			if (!Cls.IsEmpty())
			{
				ClassFilter.Add(Cls);
			}
		}
	}

	int32 MaxResults = 2000;
	Params->TryGetNumberField(TEXT("max_results"), MaxResults);
	MaxResults = FMath::Clamp(MaxResults, 1, 20000);

	IAssetRegistry* Registry = &mcp_common::AssetRegistry();
	if (!Registry)
	{
		return mcp_common::MakeErrorReply(TEXT("AssetRegistry is not available"));
	}

	// Metadata-only scan -- GetAssetsByPath never force-loads a UObject, so this stays cheap even
	// over a full /Game tree.
	TArray<FAssetData> AllAssets;
	Registry->GetAssetsByPath(FName(*FolderPath), AllAssets, bRecursive, /*bIncludeOnlyOnDiskAssets=*/false);

	// class_filter is a post-filter over the metadata scan, matched case-insensitively against the
	// short class name (AssetClassPath.GetAssetName().ToString()), e.g. "StaticMesh", "Material".
	TArray<FAssetData> ConsideredAssets;
	if (ClassFilter.Num() > 0)
	{
		for (const FAssetData& AD : AllAssets)
		{
			const FString ClassName = AD.AssetClassPath.GetAssetName().ToString();
			bool bMatches = false;
			for (const FString& Cls : ClassFilter)
			{
				if (ClassName.Equals(Cls, ESearchCase::IgnoreCase))
				{
					bMatches = true;
					break;
				}
			}
			if (bMatches)
			{
				ConsideredAssets.Add(AD);
			}
		}
	}
	else
	{
		ConsideredAssets = AllAssets;
	}

	// Group by AssetName.
	TMap<FName, TArray<FAssetData>> Groups;
	for (const FAssetData& AD : ConsideredAssets)
	{
		Groups.FindOrAdd(AD.AssetName).Add(AD);
	}

	struct FDupGroup
	{
		FName Name;
		TArray<FAssetData> Assets;
	};
	TArray<FDupGroup> QualifyingGroups;
	for (TPair<FName, TArray<FAssetData>>& Pair : Groups)
	{
		if (Pair.Value.Num() >= MinGroupSize)
		{
			FDupGroup Group;
			Group.Name = Pair.Key;
			Group.Assets = MoveTemp(Pair.Value);
			QualifyingGroups.Add(MoveTemp(Group));
		}
	}

	// Most-duplicated first -- the most useful triage order. Ties break on name for determinism
	// (TMap iteration order is hash-based, not insertion order).
	QualifyingGroups.Sort([](const FDupGroup& A, const FDupGroup& B)
	{
		if (A.Assets.Num() != B.Assets.Num())
		{
			return A.Assets.Num() > B.Assets.Num();
		}
		return A.Name.LexicalLess(B.Name);
	});

	const int32 TotalGroups = QualifyingGroups.Num();
	const bool bResultsTruncated = TotalGroups > MaxResults;
	const int32 GroupsToReturn = FMath::Min(TotalGroups, MaxResults);

	TArray<TSharedPtr<FJsonValue>> GroupEntries;
	int32 TotalDuplicateAssets = 0;
	for (int32 GroupIdx = 0; GroupIdx < GroupsToReturn; ++GroupIdx)
	{
		const FDupGroup& Group = QualifyingGroups[GroupIdx];

		TSet<FString> DistinctClasses;
		TArray<TSharedPtr<FJsonValue>> AssetEntries;
		for (const FAssetData& AD : Group.Assets)
		{
			const FString ClassName = AD.AssetClassPath.GetAssetName().ToString();
			DistinctClasses.Add(ClassName);

			TSharedPtr<FJsonObject> AssetObj = MakeShared<FJsonObject>();
			AssetObj->SetStringField(TEXT("path"), AD.GetObjectPathString());
			AssetObj->SetStringField(TEXT("package"), AD.PackageName.ToString());
			AssetObj->SetStringField(TEXT("class"), ClassName);
			AssetEntries.Add(MakeShared<FJsonValueObject>(AssetObj));
		}

		TSharedPtr<FJsonObject> GroupObj = MakeShared<FJsonObject>();
		GroupObj->SetStringField(TEXT("name"), Group.Name.ToString());
		GroupObj->SetNumberField(TEXT("count"), Group.Assets.Num());
		GroupObj->SetArrayField(TEXT("classes"), mcp_common::StringsToJson(DistinctClasses.Array()));
		GroupObj->SetArrayField(TEXT("assets"), AssetEntries);
		GroupEntries.Add(MakeShared<FJsonValueObject>(GroupObj));

		TotalDuplicateAssets += (Group.Assets.Num() - 1);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("folder_path"), FolderPath);
	Data->SetBoolField(TEXT("recursive"), bRecursive);
	Data->SetArrayField(TEXT("class_filter"), mcp_common::StringsToJson(ClassFilter));
	Data->SetNumberField(TEXT("min_group_size"), MinGroupSize);
	Data->SetNumberField(TEXT("group_count"), GroupEntries.Num());
	Data->SetNumberField(TEXT("total_duplicate_assets"), TotalDuplicateAssets);
	Data->SetNumberField(TEXT("assets_scanned"), ConsideredAssets.Num());
	Data->SetNumberField(TEXT("max_results"), MaxResults);
	Data->SetBoolField(TEXT("results_truncated"), bResultsTruncated);
	Data->SetArrayField(TEXT("groups"), GroupEntries);
	return mcp_common::MakeOkReply(Data);
}

const McpCommandFamilies::FRegistrar Registration =
{
	{ TEXT("bulk_delete_assets"), &HandleBulkDeleteAssets },
	{ TEXT("consolidate_assets"), &HandleConsolidateAssets },
	{ TEXT("export_assets"), &HandleExportAssets },
	{ TEXT("find_duplicate_assets"), &HandleFindDuplicateAssets },
	{ TEXT("find_unused_assets"), &HandleFindUnusedAssets },
	{ TEXT("migrate_assets"), &HandleMigrateAssets },
};
} // namespace UEAgentEditorAutomationCommands
