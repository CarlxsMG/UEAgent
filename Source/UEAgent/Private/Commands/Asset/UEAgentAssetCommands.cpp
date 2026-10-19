#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Common/UEAgentGraphUtils.h" // ResolveClass: get_assets_by_type accepts any class, not five hardwired ones
#include "Commands/Asset/AssetCommandHelpers.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/Texture2D.h"
#include "Engine/Texture2DArray.h"             // UTexture2DArray (cast verify for remove_texture_2d_array)
#include "Engine/TextureRenderTarget2D.h"      // UTextureRenderTarget2D (typed remove)
#include "Engine/TextureRenderTarget2DArray.h" // UTextureRenderTarget2DArray (typed remove)
#include "Engine/TextureRenderTargetCube.h"    // UTextureRenderTargetCube (typed remove)
#include "Engine/World.h"
#include "Engine/Level.h"
#include "Sound/SoundWave.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"                        // FPaths::RemoveDuplicateSlashes (get_asset_metadata)
#include "HAL/PlatformFilemanager.h"
#include "ThumbnailRendering/ThumbnailManager.h"
#include "ThumbnailRendering/ThumbnailRenderer.h"
#include "Engine/Engine.h"
#include "Misc/PackageName.h"
#include "AssetThumbnail.h"
#include "Engine/Texture.h"
#include "Engine/SkeletalMesh.h"

/**
 * The Asset Registry readers: look assets up by exact name, class, folder, name pattern or registry tag,
 * report one asset's tags, thumbnail capability, on-disk size and date, and remove typed texture assets.
 */
namespace UEAgentAssetCommands
{

// MCP-DOMAIN: Asset

// ===========================================================================
// Shared plumbing of the asset readers below: parameter errors, the exact-name
// lookup with its ambiguity report, the row shape of every listing, the
// get_assets_by_type shortcuts and the on-disk .uasset resolution.
// ===========================================================================
namespace UEAgentAssetCommandsImpl
{
    using AssetCommandHelpers::AppendAmbiguity;
    using AssetCommandHelpers::AssetNotFoundError;
    using AssetCommandHelpers::EmptyParamError;
    using AssetCommandHelpers::FNamedAsset;
    using AssetCommandHelpers::RangeError;

    /**
     * asset_name of the single-asset readers, looked up like get_asset_by_name (any class; a project can hold many
     * assets sharing one name, so Out lists every candidate when the pick was ambiguous). Returns the error reply to
     * send back, or nullptr once Out holds the asset.
     */
    TSharedPtr<FJsonObject> ReadNamedAsset(const TSharedPtr<FJsonObject>& Params, FString& OutRequested, FNamedAsset& Out)
    {
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_name"), OutRequested, mcp_common::EAllowEmpty::Yes)) { return Missing; }
        if (OutRequested.IsEmpty())
        {
            return EmptyParamError(TEXT("asset_name"));
        }
        if (!AssetCommandHelpers::FindAssetByExactName(OutRequested, nullptr, Out))
        {
            return AssetNotFoundError(OutRequested);
        }
        return nullptr;
    }

    /** { asset_name, asset_path, asset_class }: the identity the single-asset readers open their reply with. */
    TSharedPtr<FJsonObject> StartNamedAssetReply(const FString& Requested, const FAssetData& Asset)
    {
        TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
        Reply->SetStringField(TEXT("asset_name"), Requested);
        Reply->SetStringField(TEXT("asset_path"), Asset.GetObjectPathString());
        Reply->SetStringField(TEXT("asset_class"), Asset.AssetClassPath.ToString());
        return Reply;
    }

    /** name / path / class / package, plus is_loaded when asked -- the identity every listing reports. */
    TSharedPtr<FJsonObject> DescribeAsset(const FAssetData& Asset, bool bWithLoadedFlag)
    {
        TSharedPtr<FJsonObject> Fields = MakeShared<FJsonObject>();
        Fields->SetStringField(TEXT("name"), Asset.AssetName.ToString());
        Fields->SetStringField(TEXT("path"), Asset.GetObjectPathString());
        Fields->SetStringField(TEXT("class"), Asset.AssetClassPath.ToString());
        Fields->SetStringField(TEXT("package"), Asset.PackageName.ToString());
        if (bWithLoadedFlag)
        {
            Fields->SetBoolField(TEXT("is_loaded"), Asset.IsAssetLoaded());
        }
        return Fields;
    }

    /**
     * The listing part of a reply: the first Cap assets of Found as rows, in registry order, then { assets, count,
     * total_found, max_results, results_truncated }.
     */
    void AddListing(const TSharedPtr<FJsonObject>& Reply, const TArray<FAssetData>& Found, int32 Cap, bool bWithLoadedFlag)
    {
        TArray<TSharedPtr<FJsonValue>> Rows;
        for (const FAssetData& Asset : Found)
        {
            if (Rows.Num() >= Cap)
            {
                break;
            }
            Rows.Add(MakeShared<FJsonValueObject>(UEAgentAssetCommandsImpl::DescribeAsset(Asset, bWithLoadedFlag)));
        }
        Reply->SetArrayField(TEXT("assets"), Rows);
        Reply->SetNumberField(TEXT("count"), Rows.Num());
        Reply->SetNumberField(TEXT("total_found"), Found.Num());
        Reply->SetNumberField(TEXT("max_results"), Cap);
        Reply->SetBoolField(TEXT("results_truncated"), Found.Num() > Rows.Num());
    }

    /**
     * load_full_data extras of get_asset_by_name. Only three asset kinds have any: a Blueprint (parent class,
     * compile state, generated class), a StaticMesh (sections, vertices and LODs of LOD 0), and a Material or
     * MaterialInstanceConstant (whether it is an instance). Each of them loads the asset.
     */
    void AddTypeDetails(const FAssetData& Asset, const TSharedPtr<FJsonObject>& Reply)
    {
        const FTopLevelAssetPath& Kind = Asset.AssetClassPath;
        if (Kind == UBlueprint::StaticClass()->GetClassPathName())
        {
            UBlueprint* Blueprint = Cast<UBlueprint>(Asset.GetAsset());
            if (Blueprint == nullptr)
            {
                return;
            }
            UClass* const Parent = Blueprint->ParentClass;
            Reply->SetStringField(TEXT("parent_class"), Parent ? Parent->GetName() : FString(TEXT("None")));
            Reply->SetBoolField(TEXT("is_compiled"), Blueprint->Status == BS_UpToDate);
            Reply->SetBoolField(TEXT("is_valid"), IsValid(Blueprint));
            if (UClass* const Generated = Blueprint->GeneratedClass)
            {
                Reply->SetStringField(TEXT("generated_class"), Generated->GetName());
            }
            return;
        }
        if (Kind == UStaticMesh::StaticClass()->GetClassPathName())
        {
            if (UStaticMesh* Mesh = Cast<UStaticMesh>(Asset.GetAsset()))
            {
                Reply->SetNumberField(TEXT("num_sections"), Mesh->GetNumSections(0));
                Reply->SetNumberField(TEXT("num_vertices"), Mesh->GetNumVertices(0));
                Reply->SetNumberField(TEXT("num_lods"), Mesh->GetNumLODs());
                Reply->SetBoolField(TEXT("is_valid"), IsValid(Mesh));
            }
            return;
        }
        const bool bInstance = (Kind == UMaterialInstanceConstant::StaticClass()->GetClassPathName());
        if (bInstance || Kind == UMaterial::StaticClass()->GetClassPathName())
        {
            Reply->SetBoolField(TEXT("is_material_instance"), bInstance);
            if (UMaterialInterface* Material = Cast<UMaterialInterface>(Asset.GetAsset()))
            {
                Reply->SetBoolField(TEXT("is_valid"), IsValid(Material));
            }
        }
    }

    /**
     * The five shortcuts of get_assets_by_type (case-insensitive). Each one matches its classes
     * EXACTLY, without subclasses, and Material is the one that groups three classes. Returns false when
     * TypeName is not a shortcut, leaving Filter untouched.
     */
    bool ApplyTypeShortcut(const FString& TypeName, FARFilter& Filter)
    {
        struct FShortcut
        {
            const TCHAR* Name;
            UClass* Classes[3];
        };
        const FShortcut Shortcuts[] = {
            { TEXT("Blueprint"),  { UBlueprint::StaticClass(), nullptr, nullptr } },
            { TEXT("StaticMesh"), { UStaticMesh::StaticClass(), nullptr, nullptr } },
            { TEXT("Material"),   { UMaterial::StaticClass(), UMaterialInstanceConstant::StaticClass(), UMaterialInstanceDynamic::StaticClass() } },
            { TEXT("Texture"),    { UTexture2D::StaticClass(), nullptr, nullptr } },
            { TEXT("Sound"),      { USoundWave::StaticClass(), nullptr, nullptr } },
        };
        for (const FShortcut& Shortcut : Shortcuts)
        {
            if (!TypeName.Equals(Shortcut.Name, ESearchCase::IgnoreCase))
            {
                continue;
            }
            for (UClass* Class : Shortcut.Classes)
            {
                if (Class != nullptr)
                {
                    Filter.ClassPaths.Add(Class->GetClassPathName());
                }
            }
            return true;
        }
        return false;
    }

    /**
     * The .uasset behind an asset. Fills OutFile and returns nullptr when the file exists; otherwise returns
     * the error reply of the file readers (the package does not map to a file name, or the file is absent).
     */
    TSharedPtr<FJsonObject> LocateUassetFile(const FAssetData& Asset, FString& OutFile)
    {
        const FString LongName = Asset.PackageName.ToString();
        if (!FPackageName::TryConvertLongPackageNameToFilename(LongName, OutFile, TEXT(".uasset")))
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Could not convert package name to filename: %s"), *LongName));
        }
        if (!FPlatformFileManager::Get().GetPlatformFile().FileExists(*OutFile))
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("File not found: %s"), *OutFile));
        }
        return nullptr;
    }

    /** The row of a batch reader for an entry that is not a non-empty string: { <KeyField>: "", found: false, error }. */
    TSharedPtr<FJsonValue> InvalidBatchRow(const TCHAR* KeyField, const TCHAR* Error)
    {
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(KeyField, TEXT(""));
        Row->SetBoolField(TEXT("found"), false);
        Row->SetStringField(TEXT("error"), Error);
        return MakeShared<FJsonValueObject>(Row);
    }

    /**
     * The row of a batch reader for Key, from the reply of the single-asset handler it delegated to:
     * { <KeyField>: Key, found }, then every field of a successful reply, or the error of a failed one.
     */
    TSharedPtr<FJsonValue> BatchRow(const TCHAR* KeyField, const FString& Key, const TSharedPtr<FJsonObject>& SingleResult)
    {
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(KeyField, Key);

        bool bFound = true;
        if (SingleResult.IsValid() && SingleResult->HasField(TEXT("success")))
        {
            bool bSuccess = false;
            SingleResult->TryGetBoolField(TEXT("success"), bSuccess);
            bFound = bSuccess;
        }
        Row->SetBoolField(TEXT("found"), bFound);

        if (SingleResult.IsValid())
        {
            if (bFound)
            {
                for (const auto& FieldPair : SingleResult->Values)
                {
                    Row->SetField(FieldPair.Key, FieldPair.Value);
                }
            }
            else if (SingleResult->HasField(TEXT("error")))
            {
                Row->SetStringField(TEXT("error"), SingleResult->GetStringField(TEXT("error")));
            }
        }
        return MakeShared<FJsonValueObject>(Row);
    }
}

// Lookups and listings, plus the per-asset readers.

// MCP:get_asset_by_name - Retrieve ONE asset by exact name and return its identity (name, class, object path, package, is_loaded, file size) plus, with load_full_data, type-specific detail: a Blueprint's parent class / compile status / generated class, a StaticMesh's section-vertex-LOD counts, whether a material is an instance. WARNING: When the name is ambiguous it still resolves to the FIRST match and merely lists the rest in `ambiguous_matches` -- check that field, or use resolve_asset_by_name, which refuses to pick for you. The fallback name scan only covers /Game, so an /Engine or plugin-mount asset must be addressed by its full package name. Use get_assets_by_names for a batch, get_asset_metadata for registry tags, and search_assets_by_pattern when you only know part of the name.
//   @param asset_name - EXACT asset name, or a full package name (which is tried first and is the only form that reaches outside /Game)
//   @param load_full_data - When true (DEFAULT), LOADS the asset to add type-specific details (Blueprint parent class, mesh sections, ...). Pass false to stay metadata-only and avoid a synchronous load.
//   @returns { name: string, class: string, path: string, package: string, is_loaded: bool, file_size_bytes?: int, file_size_kb?: number, parent_class?: string, is_compiled?: bool, is_valid?: bool, generated_class?: string, num_sections?: int, num_vertices?: int, num_lods?: int, is_material_instance?: bool, ambiguous_matches?: [string] }

TSharedPtr<FJsonObject> HandleGetAssetByName(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentAssetCommandsImpl;

    FString Requested;
    Impl::FNamedAsset Found;
    if (TSharedPtr<FJsonObject> Problem = Impl::ReadNamedAsset(Params, Requested, Found))
    {
        return Problem;
    }
    // Loading the asset for type-specific detail is the default; false keeps the call metadata-only.
    bool bLoadFullData = true;
    Params->TryGetBoolField(TEXT("load_full_data"), bLoadFullData);
    const FAssetData& Asset = Found.Asset;

    TSharedPtr<FJsonObject> Reply = Impl::DescribeAsset(Asset, /*bWithLoadedFlag*/ true);
    if (bLoadFullData)
    {
        Impl::AddTypeDetails(Asset, Reply);
    }

    // Size on disk, only when the package maps to a .uasset that exists (silently left out otherwise).
    FString OnDisk;
    if (FPackageName::TryConvertLongPackageNameToFilename(Asset.PackageName.ToString(), OnDisk, TEXT(".uasset")))
    {
        IPlatformFile& Files = FPlatformFileManager::Get().GetPlatformFile();
        if (Files.FileExists(*OnDisk))
        {
            const int64 Bytes = Files.FileSize(*OnDisk);
            Reply->SetNumberField(TEXT("file_size_bytes"), Bytes);
            Reply->SetNumberField(TEXT("file_size_kb"), Bytes / 1024.0);
        }
    }

    Impl::AppendAmbiguity(Reply, Found.AmbiguousMatches);
    return Reply;
}

// MCP:get_assets_by_type - List every asset of a CLASS under a content path, with name, object path, class, package and is_loaded, plus total_found and results_truncated. Takes any resolvable class -- "DataTable", "NiagaraSystem", "LandscapeGrassType", a "/Script/Module.Class" path -- matched WITH its subclasses, plus five shortcuts kept for convenience: "Blueprint", "StaticMesh", "Texture" (Texture2D exactly), "Sound" (SoundWave exactly) and "Material", which is the one that earns its keep by grouping Material + MaterialInstanceConstant + MaterialInstanceDynamic. Searches /Game by default and any mount via package_path (/Engine, /Water, plugin content). Sister tools: get_assets_in_folder to enumerate a directory whatever the class, search_assets_by_pattern to search by NAME, get_assets_by_tag to search by an Asset Registry tag.
//   @param asset_type - A class name ("DataTable"), a "/Script/Module.Class" path, or one of the shortcuts Blueprint | StaticMesh | Material | Texture | Sound. A name that resolves to no class is an error, not an empty result. WARNING: The shortcuts match those classes EXACTLY (Texture finds Texture2D but not TextureCube); any other name matches the class AND its subclasses.
//   @param package_path - Content root to search, recursively. Defaults to "/Game"; pass "/Engine", "/Water", a plugin mount or a subfolder to look elsewhere.
//   @param max_results - Cap on number of returned assets (1..50000); defaults to 1000. `total_found` is the uncapped count.
//   @returns { assets: [{ name, path, class, package, is_loaded }], count: int, total_found: int, asset_type: string, package_path: string, max_results: int, results_truncated: bool }

TSharedPtr<FJsonObject> HandleGetAssetsByType(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentAssetCommandsImpl;

    FString TypeName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_type"), TypeName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    int32 Cap = 1000;
    Params->TryGetNumberField(TEXT("max_results"), Cap);

    // The search root reaches /Engine and every plugin mount, like get_assets_in_folder and
    // search_assets_by_pattern: trimmed, defaulted, and stripped of one trailing slash.
    FString Root = TEXT("/Game");
    Params->TryGetStringField(TEXT("package_path"), Root);
    Root.TrimStartAndEndInline();
    if (Root.IsEmpty())
    {
        Root = TEXT("/Game");
    }
    if (Root.Len() > 1 && Root.EndsWith(TEXT("/")))
    {
        Root.LeftChopInline(1);
    }

    if (TypeName.IsEmpty())
    {
        return Impl::EmptyParamError(TEXT("asset_type"));
    }
    if (TSharedPtr<FJsonObject> OutOfRange = Impl::RangeError(TEXT("max_results"), Cap, 1, 50000))
    {
        return OutOfRange;
    }

    IAssetRegistry& Registry = mcp_common::AssetRegistry();
    FARFilter Filter;
    Filter.PackagePaths.Add(FName(*Root));
    Filter.bRecursivePaths = true;

    // Five names are shortcuts; every other name resolves as a class (short name, "/Script/Module.Class"
    // or a Blueprint asset path).
    TypeName.TrimStartAndEndInline();
    if (!Impl::ApplyTypeShortcut(TypeName, Filter))
    {
        UClass* Resolved = FUEAgentGraphUtils::ResolveClass(TypeName);
        if (Resolved == nullptr)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Unsupported asset type: '%s'. Use one of the shortcuts (Blueprint, StaticMesh, "
                     "Material, Texture, Sound) or any resolvable class name / \"/Script/Module.Class\" path."),
                *TypeName));
        }
        Filter.ClassPaths.Add(Resolved->GetClassPathName());
        // Subclasses count: asking for "Texture" and not seeing a UTexture2D would be an honest-looking zero.
        Filter.bRecursiveClasses = true;
    }

    TArray<FAssetData> Matches;
    Registry.GetAssets(Filter, Matches);

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("asset_type"), TypeName);
    // Where it looked: a zero under /Game and a zero under /Water do not mean the same thing.
    Reply->SetStringField(TEXT("package_path"), Root);
    Impl::AddListing(Reply, Matches, Cap, /*bWithLoadedFlag*/ true);
    return Reply;
}

// MCP:get_assets_in_folder - Enumerate every asset under a content FOLDER (recursively by default), each with name, object path, class, package and is_loaded, plus total_found so you can tell a truncated listing from a complete one. The "what is actually in this directory?" inventory -- metadata only, it never loads an asset. Reaches any mounted content root, not just /Game. Pick this over search_assets_by_pattern when you know WHERE to look rather than what the name contains, over get_assets_by_type when you want everything regardless of class, and over resolve_asset_by_name when you do not have an exact name at all.
//   @param folder_path - Folder to list. A bare/relative name is prefixed with /Game/; a path already starting with "/" (e.g. /Water/Materials, /Engine/..., /<PluginName>/...) is used verbatim, so every mounted content root is reachable.
//   @param max_results - Cap on number of returned assets (1..50000); defaults to 1000. Compare `count` against `total_found` (which is NOT capped) to see how much was left out.
//   @param recursive - When true (DEFAULT), include subfolders; false lists only the assets sitting directly in this folder
//   @returns { assets: [{ name, path, class, package, is_loaded }], count: int, total_found: int, folder_path: string, recursive: bool, max_results: int, results_truncated: bool }

TSharedPtr<FJsonObject> HandleGetAssetsInFolder(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentAssetCommandsImpl;

    FString Folder;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("folder_path"), Folder, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    int32 Cap = 1000;
    Params->TryGetNumberField(TEXT("max_results"), Cap);
    bool bRecursive = true;
    Params->TryGetBoolField(TEXT("recursive"), bRecursive);

    if (Folder.IsEmpty())
    {
        return Impl::EmptyParamError(TEXT("folder_path"));
    }
    if (TSharedPtr<FJsonObject> OutOfRange = Impl::RangeError(TEXT("max_results"), Cap, 1, 50000))
    {
        return OutOfRange;
    }

    // Only a relative name is rooted under /Game/. A path that already starts with "/" (/Water/Materials,
    // /Engine/..., /<Plugin>/...) is kept verbatim: prefixing it produced /Game//Water/... and zero results,
    // which made every non-/Game mount unreachable from here.
    if (!Folder.StartsWith(TEXT("/")))
    {
        Folder.InsertAt(0, TEXT("/Game/"));
    }
    if (!Folder.EndsWith(TEXT("/")))
    {
        Folder.AppendChar(TEXT('/'));
    }

    TArray<FAssetData> InFolder;
    mcp_common::AssetRegistry().GetAssetsByPath(FName(*Folder), InFolder, bRecursive);

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("folder_path"), Folder);
    Reply->SetBoolField(TEXT("recursive"), bRecursive);
    Impl::AddListing(Reply, InFolder, Cap, /*bWithLoadedFlag*/ true);
    return Reply;
}

// MCP:search_assets_by_pattern - Find assets by NAME, scanning every mounted content root (/Game, /Engine, /Water, plugin mounts, ...) and returning name, object path, class and package per hit. The broad "does anything called <token> exist, and where?" search. It has TWO match modes and picks one automatically, reporting which in `match_mode`: a pattern WITHOUT `*`/`?` is a case-insensitive SUBSTRING test (FString::Contains) -- pass the bare token, this is the common case; a pattern WITH `*` or `?` is a real glob (FString::MatchesWildcard). WARNING: The glob is ANCHORED AT BOTH ENDS -- it matches the WHOLE name, not a substring -- so `Grass*` means "starts with Grass" and `*Grass*` is what reproduces substring behaviour. It only ever looks at the asset NAME, so it cannot find an asset by its folder or by a class it belongs to: use resolve_asset_by_name to check ONE exact name and get every homonym, get_assets_in_folder to enumerate a directory, get_assets_by_type for the 5 built-in type filters, and get_assets_by_tag for an Asset Registry tag.
//   @param search_pattern - Matched against the asset NAME. Contains no `*`/`?` -> case-insensitive substring (FString::Contains). Contains `*` or `?` -> glob (FString::MatchesWildcard), ANCHORED at both ends: `*` matches any run including empty, `?` matches 0 or 1 character (UE semantics, not exactly 1). Not a regex.
//   @param max_results - Cap on number of matched assets (1..50000); defaults to 1000. WARNING: Hitting the cap ABORTS the enumeration, so `total_scanned` then counts only the assets visited before the early-out, not the whole registry.
//   @param case_sensitive - When true, match is case-sensitive; defaults to false
//   @param wait_for_scan - When true (default), force-completes the AssetRegistry scan before enumerating so a still-loading registry cannot report total_scanned:0. Set false to skip the (possibly blocking) wait.
//   @returns { assets: [{ name, path, class, package, is_loaded }], count: int, total_scanned: int, scan_was_pending: bool, search_pattern: string, match_mode: "substring"|"wildcard", case_sensitive: bool, max_results: int, results_truncated: bool }

TSharedPtr<FJsonObject> HandleSearchAssetsByPattern(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentAssetCommandsImpl;

    FString Pattern;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("search_pattern"), Pattern, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    int32 Cap = 1000;
    Params->TryGetNumberField(TEXT("max_results"), Cap);
    bool bCaseSensitive = false;
    Params->TryGetBoolField(TEXT("case_sensitive"), bCaseSensitive);
    // Default is the safe behavior: finish the registry scan before enumerating.
    bool bWaitForScan = true;
    Params->TryGetBoolField(TEXT("wait_for_scan"), bWaitForScan);

    if (Pattern.IsEmpty())
    {
        return Impl::EmptyParamError(TEXT("search_pattern"));
    }
    if (TSharedPtr<FJsonObject> OutOfRange = Impl::RangeError(TEXT("max_results"), Cap, 1, 50000))
    {
        return OutOfRange;
    }

    // A pattern with `*` or `?` is a glob (FString::MatchesWildcard); without one it is a substring test
    // (FString::Contains) -- compared literally, a `*` would never match an asset name and every such
    // pattern would answer an honest-looking count:0. The two modes are NOT interchangeable and the
    // reply says which ran: MatchesWildcard is anchored at both ends
    // (Core/Tests/String/MatchesWildcardTest.cpp), so `BP_` as a substring finds `X_BP_Y` while the glob
    // `BP_*` does not -- `*BP_*` is the glob equivalent of the substring test.
    const bool bWildcard = Pattern.Contains(TEXT("*")) || Pattern.Contains(TEXT("?"));

    IAssetRegistry& Registry = mcp_common::AssetRegistry();

    // In a running editor the registry's initial gather can
    // still be in flight, and enumerating then visits an empty or partial set. SearchAllAssets(true) runs
    // the on-disk gather synchronously (starting it if it never started this session) and
    // WaitForCompletion() drains the tick-side processing. Whether a scan was pending is reported either
    // way, so a stray zero stays diagnosable.
    const bool bScanWasPending = Registry.IsLoadingAssets() || !Registry.IsSearchAllAssets();
    if (bWaitForScan && bScanWasPending)
    {
        Registry.SearchAllAssets(/*bSynchronousSearch=*/true);
        Registry.WaitForCompletion();
    }

    // Every mounted root (/Game, /Engine, /Water, plugin mounts) through EnumerateAllAssets, which is
    // what the engine documents for "all assets". An FARFilter that only sets bRecursivePaths compiles to
    // an EMPTY filter (no seed path to expand) and GetAssets returns nothing for it -- this tool's old
    // "returns 0 for everything". The match is applied here, and the enumeration stops at the cap.
    const ESearchCase::Type Casing = bCaseSensitive ? ESearchCase::CaseSensitive : ESearchCase::IgnoreCase;
    TArray<TSharedPtr<FJsonValue>> Rows;
    int32 Visited = 0;
    bool bTruncated = false;
    Registry.EnumerateAllAssets([&](const FAssetData& Candidate) -> bool
    {
        ++Visited;
        const FString CandidateName = Candidate.AssetName.ToString();
        const bool bHit = bWildcard
            ? CandidateName.MatchesWildcard(Pattern, Casing)
            : CandidateName.Contains(Pattern, Casing);
        if (!bHit)
        {
            return true;
        }
        if (Rows.Num() >= Cap)
        {
            bTruncated = true;   // a match past the cap: the list really is cut short
            return false;
        }
        Rows.Add(MakeShared<FJsonValueObject>(Impl::DescribeAsset(Candidate, /*bWithLoadedFlag*/ true)));
        return true;
    });

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("search_pattern"), Pattern);
    // Which matcher ran: a zero means something different in each mode.
    Reply->SetStringField(TEXT("match_mode"), bWildcard ? TEXT("wildcard") : TEXT("substring"));
    Reply->SetBoolField(TEXT("case_sensitive"), bCaseSensitive);
    Reply->SetBoolField(TEXT("scan_was_pending"), bScanWasPending);
    Reply->SetArrayField(TEXT("assets"), Rows);
    Reply->SetNumberField(TEXT("count"), Rows.Num());
    Reply->SetNumberField(TEXT("total_scanned"), Visited);
    Reply->SetNumberField(TEXT("max_results"), Cap);
    Reply->SetBoolField(TEXT("results_truncated"), bTruncated);
    return Reply;
}

// MCP:get_asset_metadata - Read an asset's Asset Registry TAGS (the indexed key/value pairs an asset publishes -- parent class, mesh stats, fork-lineage tags, ...) together with its name, class, object path and package. WARNING: its `asset_name` is NOT a name: the tool literally prefixes "/Game/" to whatever you pass, so it must be a path RELATIVE TO /Game ("MyGame/Systems/Foo/BP_Foo.BP_Foo"), never a bare asset name and never an absolute "/Game/..." path -- and it LOADS the asset to resolve it. Use get_assets_metadata_batch for many paths at once, get_asset_by_name for identity from a bare name, and get_assets_by_tag to search BY a tag value instead of reading the tags of one asset.
//   @param asset_name - Path RELATIVE to /Game, with no leading slash (e.g. "MyGame/Systems/Foo/BP_Foo.BP_Foo"); the tool prepends "/Game/" itself, so a bare name or an absolute path both fail to resolve
//   @returns { name: string, class: string, path: string, package: string, asset_class_path?: string, package_path?: string, tags?: [{ name, value }] }

TSharedPtr<FJsonObject> HandleGetAssetMetadata(const TSharedPtr<FJsonObject>& Params)
{
    FString RelativePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_name"), RelativePath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    // Despite its name, asset_name is a path RELATIVE to /Game: the root is prepended here, and the asset
    // is loaded to resolve it. LoadAsset collapses doubled slashes, so the path is checked as it resolves: with the
    // root, a name under the limit can join to a package name past it, which aborts the editor.
    FString FullPath = TEXT("/Game/") + RelativePath;
    FPaths::RemoveDuplicateSlashes(FullPath);
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(FullPath)) { return Invalid; }
    UObject* Loaded = UEditorAssetLibrary::LoadAsset(FullPath);
    if (Loaded == nullptr)
    {
        return UEAgentAssetCommandsImpl::AssetNotFoundError(RelativePath);
    }

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("name"), Loaded->GetName());
    Reply->SetStringField(TEXT("class"), Loaded->GetClass()->GetName());
    Reply->SetStringField(TEXT("path"), Loaded->GetPathName());
    Reply->SetStringField(TEXT("package"), Loaded->GetOuter()->GetName());

    // The registry entry carries the indexed tags; without one only the identity above is reported.
    FAssetData Registered = mcp_common::FindAssetDataByPath(FullPath);
    if (Registered.IsValid())
    {
        Reply->SetStringField(TEXT("asset_class_path"), Registered.AssetClassPath.ToString());
        Reply->SetStringField(TEXT("package_path"), Registered.PackageName.ToString());

        TArray<TSharedPtr<FJsonValue>> Tags;
        for (const auto& Tag : Registered.TagsAndValues)
        {
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("name"), Tag.Key.ToString());
            Entry->SetStringField(TEXT("value"), Tag.Value.AsString());
            Tags.Add(MakeShared<FJsonValueObject>(Entry));
        }
        Reply->SetArrayField(TEXT("tags"), Tags);
    }
    return Reply;
}

// MCP:get_asset_thumbnail - Report whether an asset is of a type that CAN carry a thumbnail, alongside its resolved object path, class and loaded state. WARNING: `has_thumbnail` is a CLASS test, not a lookup of stored thumbnail bytes: it is true for any StaticMesh, Material, Texture, Blueprint, SkeletalMesh or Level and false for everything else, whether or not a thumbnail was ever rendered -- and it returns no image data at all. To actually see something rendered, use generate_asset_thumbnail or capture_viewport. Resolution matches get_asset_by_name (full package name first, then a /Game-only exact-name scan, first hit wins with the rest in ambiguous_matches).
//   @param asset_name - EXACT asset name, or a full package name (which is tried first and is the only form that reaches outside /Game)
//   @returns { asset_name: string, asset_path: string, has_thumbnail: bool, asset_class: string, asset_loaded: bool, asset_class_name?: string, ambiguous_matches?: [string] }

TSharedPtr<FJsonObject> HandleGetAssetThumbnail(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentAssetCommandsImpl;

    FString Requested;
    Impl::FNamedAsset Found;
    if (TSharedPtr<FJsonObject> Problem = Impl::ReadNamedAsset(Params, Requested, Found))
    {
        return Problem;
    }
    const FAssetData& Asset = Found.Asset;

    // One pool for the whole editor session, created on first use; the thumbnail handle registers with it
    // for the duration of this call.
    static TSharedPtr<FAssetThumbnailPool> SessionPool;
    if (!SessionPool.IsValid())
    {
        SessionPool = MakeShared<FAssetThumbnailPool>(256, 0.1, 1);
    }
    FAssetThumbnail Thumbnail(Asset, 256, 256, SessionPool);

    // has_thumbnail is a CLASS test, not a lookup of stored thumbnail bytes: the asset kinds that ship a
    // thumbnail renderer. The thumbnail manager singleton is still brought up (created on first access).
    UObject* Loaded = Asset.GetAsset();
    bool bThumbnailCapable = false;
    if (Loaded != nullptr)
    {
        (void)UThumbnailManager::Get();
        const UClass* LoadedClass = Loaded->GetClass();
        bThumbnailCapable = LoadedClass != nullptr
            && (LoadedClass->IsChildOf<UStaticMesh>()
                || LoadedClass->IsChildOf<UMaterial>()
                || LoadedClass->IsChildOf<UTexture>()
                || LoadedClass->IsChildOf<UBlueprint>()
                || LoadedClass->IsChildOf<USkeletalMesh>()
                || LoadedClass->IsChildOf<ULevel>());
    }

    TSharedPtr<FJsonObject> Reply = Impl::StartNamedAssetReply(Requested, Asset);
    Reply->SetBoolField(TEXT("has_thumbnail"), bThumbnailCapable);
    Reply->SetBoolField(TEXT("asset_loaded"), Loaded != nullptr);
    if (Loaded != nullptr)
    {
        Reply->SetStringField(TEXT("asset_class_name"), Loaded->GetClass()->GetName());
    }
    Impl::AppendAmbiguity(Reply, Found.AmbiguousMatches);
    return Reply;
}

// MCP:get_asset_file_size - Read the ON-DISK size of an asset's .uasset in bytes, KB and MB, plus the absolute file path it resolved to -- the measurement for a disk-budget question ("what is costing the 491 MB in this import?"). WARNING: Size is NOT a proxy for content or integrity: a DataTable that GREW by 5 KB had lost 46 of its 48 references. To measure content use get_data_table_info, read_data_table_row_complex or get_asset_dependencies; for in-memory cost use get_asset_memory_usage. Resolution matches get_asset_by_name (full package name first, then a /Game-only exact-name scan, first hit wins with the rest in ambiguous_matches).
//   @param asset_name - EXACT asset name, or a full package name (which is tried first and is the only form that reaches outside /Game)
//   @returns { asset_name: string, asset_path: string, file_path: string, file_size_bytes: int, file_size_kb: number, file_size_mb: number, file_exists: bool, asset_class: string, ambiguous_matches?: [string] }

TSharedPtr<FJsonObject> HandleGetAssetFileSize(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentAssetCommandsImpl;

    FString Requested;
    Impl::FNamedAsset Found;
    if (TSharedPtr<FJsonObject> Problem = Impl::ReadNamedAsset(Params, Requested, Found))
    {
        return Problem;
    }

    FString OnDisk;
    if (TSharedPtr<FJsonObject> FileProblem = Impl::LocateUassetFile(Found.Asset, OnDisk))
    {
        return FileProblem;
    }
    const int64 Bytes = FPlatformFileManager::Get().GetPlatformFile().FileSize(*OnDisk);

    TSharedPtr<FJsonObject> Reply = Impl::StartNamedAssetReply(Requested, Found.Asset);
    Reply->SetStringField(TEXT("file_path"), OnDisk);
    Reply->SetBoolField(TEXT("file_exists"), true);
    Reply->SetNumberField(TEXT("file_size_bytes"), Bytes);
    Reply->SetNumberField(TEXT("file_size_kb"), Bytes / 1024.0);
    Reply->SetNumberField(TEXT("file_size_mb"), Bytes / (1024.0 * 1024.0));
    Impl::AppendAmbiguity(Reply, Found.AmbiguousMatches);
    return Reply;
}

// MCP:get_asset_last_modified - Read the .uasset file's last-modified timestamp (formatted string + Unix epoch) and how old it is in days and hours -- "was this asset actually written to disk when I saved it?", the disk-side oracle for a save whose return value claimed success. WARNING: It is a FILE timestamp: an asset edited in memory but not saved reports its old date, and Unreal does not distinguish creation from modification (the tool reads the same stamp for both). Use list_assets_modified_since to sweep a whole folder by date, and list_dirty_packages for unsaved-in-memory changes. Resolution matches get_asset_by_name (full package name first, then a /Game-only exact-name scan, first hit wins with the rest in ambiguous_matches).
//   @param asset_name - EXACT asset name, or a full package name (which is tried first and is the only form that reaches outside /Game)
//   @returns { asset_name: string, asset_path: string, file_path: string, last_modified: string, last_modified_timestamp: int, file_exists: bool, asset_class: string, age_days: number, age_hours: number, ambiguous_matches?: [string] }

TSharedPtr<FJsonObject> HandleGetAssetLastModified(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentAssetCommandsImpl;

    FString Requested;
    Impl::FNamedAsset Found;
    if (TSharedPtr<FJsonObject> Problem = Impl::ReadNamedAsset(Params, Requested, Found))
    {
        return Problem;
    }

    FString OnDisk;
    if (TSharedPtr<FJsonObject> FileProblem = Impl::LocateUassetFile(Found.Asset, OnDisk))
    {
        return FileProblem;
    }
    // The file's single timestamp: the platform layer does not tell creation and modification apart.
    const FDateTime Stamp = FPlatformFileManager::Get().GetPlatformFile().GetTimeStamp(*OnDisk);
    const FTimespan Age = FDateTime::UtcNow() - Stamp;

    TSharedPtr<FJsonObject> Reply = Impl::StartNamedAssetReply(Requested, Found.Asset);
    Reply->SetStringField(TEXT("file_path"), OnDisk);
    Reply->SetBoolField(TEXT("file_exists"), true);
    Reply->SetStringField(TEXT("last_modified"), Stamp.ToString());
    Reply->SetNumberField(TEXT("last_modified_timestamp"), Stamp.ToUnixTimestamp());
    Reply->SetNumberField(TEXT("age_days"), Age.GetTotalDays());
    Reply->SetNumberField(TEXT("age_hours"), Age.GetTotalHours());
    Impl::AppendAmbiguity(Reply, Found.AmbiguousMatches);
    return Reply;
}

// ===========================================================================
// get_assets_by_tag / resolve_asset_by_name / get_assets_by_names /
// get_assets_metadata_batch. class_filter (below) resolves a class name the way the rest
// of the plugin does (e.g. UEAgentSequencerCommands' class resolver): a full
// object/script path via LoadObject first, then the 5.7-safe UClass::TryFindTypeSlow,
// then FindFirstObject as a last resort.
// ===========================================================================

static UClass* ResolveClassFilterToUClass(const FString& ClassFilter)
{
    if (ClassFilter.IsEmpty())
    {
        return nullptr;
    }

    if (ClassFilter.Contains(TEXT(".")) || ClassFilter.StartsWith(TEXT("/Script/")))
    {
        if (UClass* Loaded = LoadObject<UClass>(nullptr, *ClassFilter))
        {
            return Loaded;
        }
    }

    if (UClass* Found = UClass::TryFindTypeSlow<UClass>(ClassFilter))
    {
        return Found;
    }

    return FindFirstObject<UClass>(*ClassFilter, EFindFirstObjectOptions::None);
}

// MCP:get_assets_by_tag - Search assets BY an Asset Registry tag (FARFilter::TagsAndValues), server-side and metadata-only -- it never force-loads anything. This is the only tool here that queries by tag rather than by name, path or type: "every asset carrying the fork-origin tag (mcp.forkOrigin by default)", "every Blueprint whose ParentClass tag is X". It is also the general-class search that get_assets_by_type cannot do, since class_filter resolves ANY class (with subclasses) instead of five hardcoded ones. Search by class alone by pairing class_filter with a tag every asset of that class publishes; use get_asset_metadata to read the tags OF one asset rather than search by them.
//   @param tag_name - Asset Registry tag name to filter on, e.g. "ParentClass" or "mcp.forkOrigin" (required). Read the available tag names off a sample asset with get_asset_metadata.
//   @param tag_value - When given, only assets whose tag equals this value EXACTLY match; when omitted, matches any asset that HAS the tag with any value
//   @param folder_path - Root package path to search under; defaults to "/Game", so /Engine, /Water and plugin mounts need their root passed explicitly
//   @param recursive - When true (DEFAULT), recurse into subfolders
//   @param class_filter - Optional class short name or /Script path (e.g. "Blueprint", "/Script/Engine.DataTable") to additionally constrain the match; subclasses match too (bRecursiveClasses)
//   @param max_results - Cap on number of returned assets (1..50000); defaults to 1000. `total_found` is the uncapped count.
//   @returns { tag_name: string, tag_value: string, folder_path: string, recursive: bool, class_filter: string, assets: [{ name, path, class, package }], count: int, total_found: int, max_results: int, results_truncated: bool }

TSharedPtr<FJsonObject> HandleGetAssetsByTag(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentAssetCommandsImpl;

    FString TagName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("tag_name"), TagName)) { return Missing; }
    // An empty tag_value means "carries the tag, with any value".
    FString TagValue;
    Params->TryGetStringField(TEXT("tag_value"), TagValue);

    FString Folder = TEXT("/Game");
    Params->TryGetStringField(TEXT("folder_path"), Folder);
    if (Folder.IsEmpty())
    {
        Folder = TEXT("/Game");
    }
    if (Folder.Len() > 1 && Folder.EndsWith(TEXT("/")))
    {
        Folder.LeftChopInline(1);
    }

    bool bRecursive = true;
    Params->TryGetBoolField(TEXT("recursive"), bRecursive);
    FString ClassFilter;
    Params->TryGetStringField(TEXT("class_filter"), ClassFilter);

    // Unlike the other listings, an out-of-range max_results is clamped here rather than refused.
    int32 Cap = 1000;
    Params->TryGetNumberField(TEXT("max_results"), Cap);
    Cap = FMath::Clamp(Cap, 1, 50000);

    FARFilter Query;
    Query.PackagePaths.Add(FName(*Folder));
    Query.bRecursivePaths = bRecursive;
    Query.TagsAndValues.Add(FName(*TagName), TagValue.IsEmpty() ? TOptional<FString>() : TOptional<FString>(TagValue));
    if (!ClassFilter.IsEmpty())
    {
        UClass* FilterClass = ResolveClassFilterToUClass(ClassFilter);
        if (FilterClass == nullptr)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("class_filter '%s' did not resolve to a known UClass"), *ClassFilter));
        }
        Query.ClassPaths.Add(FilterClass->GetClassPathName());
        Query.bRecursiveClasses = true;
    }

    // Metadata only: GetAssets never force-loads anything.
    TArray<FAssetData> Matches;
    mcp_common::AssetRegistry().GetAssets(Query, Matches);

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("tag_name"), TagName);
    Reply->SetStringField(TEXT("tag_value"), TagValue);
    Reply->SetStringField(TEXT("class_filter"), ClassFilter);
    Reply->SetStringField(TEXT("folder_path"), Folder);
    Reply->SetBoolField(TEXT("recursive"), bRecursive);
    Impl::AddListing(Reply, Matches, Cap, /*bWithLoadedFlag*/ false);
    return Reply;
}

// MCP:resolve_asset_by_name - Resolve an exact asset name (or path) to EVERY matching asset and report `match_count` + `unambiguous`, instead of silently picking the first one the way get_asset_by_name, get_blueprint_parent, get_graph_nodes and the rest of the by-name tools do. Run this FIRST whenever you are about to address an asset by bare name: a project can have hundreds of duplicate names (the same name in a vendored pack and in the game's own folders, plus dated backup copies), and `unambiguous:false` is the only warning you will get before another tool hands you a stale backup's contents as if they were the real asset. From there, pass the full path everywhere. match_count:0 with success:true is a valid, informative answer -- it means "no asset with this exact name", which is a real measurement of non-existence (unlike search_assets_by_pattern's count:0, which can just mean the pattern had a wildcard in it).
//   @param asset_name - EXACT bare asset name or full package/asset path. This is an equality test, not a substring one -- use search_assets_by_pattern when you only know part of the name.
//   @param class_filter - Optional class short name or /Script path (e.g. "Blueprint", "DataTable") to constrain the match; matches subclasses too
//   @param search_root - Root package path for the fallback scan used when asset_name is not an exact package-name hit; defaults to "/Game", so an asset under /Engine or a plugin mount needs its root passed here explicitly
//   @returns { asset_name: string, search_root: string, class_filter: string, match_count: int, unambiguous: bool, matches: [{ path, package, class }] }

TSharedPtr<FJsonObject> HandleResolveAssetByName(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_name"), AssetName)) { return Missing; }

    FString ClassFilter;
    Params->TryGetStringField(TEXT("class_filter"), ClassFilter);

    FString SearchRoot = TEXT("/Game");
    Params->TryGetStringField(TEXT("search_root"), SearchRoot);
    if (SearchRoot.IsEmpty())
    {
        SearchRoot = TEXT("/Game");
    }

    UClass* ResolvedFilterClass = nullptr;
    if (!ClassFilter.IsEmpty())
    {
        ResolvedFilterClass = ResolveClassFilterToUClass(ClassFilter);
        if (!ResolvedFilterClass)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("class_filter '%s' did not resolve to a known UClass"), *ClassFilter));
        }
    }

    // Robust search (mirrors get_asset_by_name's own fallback-scan pattern, but
    //    collects EVERY match instead of picking Assets[0])
    IAssetRegistry& AssetRegistry = mcp_common::AssetRegistry();

    TArray<FAssetData> Matches;
    AssetRegistry.GetAssetsByPackageName(FName(*AssetName), Matches);

    if (ResolvedFilterClass && Matches.Num() > 0)
    {
        Matches.RemoveAll([ResolvedFilterClass](const FAssetData& Candidate)
        {
            return !Candidate.IsInstanceOf(ResolvedFilterClass);
        });
    }

    if (Matches.Num() == 0)
    {
        FARFilter Filter;
        Filter.PackagePaths.Add(FName(*SearchRoot));
        Filter.bRecursivePaths = true;
        if (ResolvedFilterClass)
        {
            Filter.ClassPaths.Add(ResolvedFilterClass->GetClassPathName());
            Filter.bRecursiveClasses = true;
        }

        TArray<FAssetData> Candidates;
        AssetRegistry.GetAssets(Filter, Candidates);
        const FName WantedName(*AssetName);
        Matches.Append(Candidates.FilterByPredicate([&WantedName](const FAssetData& Candidate)
        {
            return Candidate.AssetName == WantedName;
        }));
    }

    // Build detailed response -- success with match_count:0 is a valid answer
    TArray<TSharedPtr<FJsonValue>> MatchArray;
    for (const FAssetData& AssetData : Matches)
    {
        TSharedPtr<FJsonObject> MatchObj = MakeShared<FJsonObject>();
        MatchObj->SetStringField(TEXT("path"), AssetData.GetObjectPathString());
        MatchObj->SetStringField(TEXT("package"), AssetData.PackageName.ToString());
        MatchObj->SetStringField(TEXT("class"), AssetData.AssetClassPath.ToString());
        MatchArray.Add(MakeShared<FJsonValueObject>(MatchObj));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("asset_name"), AssetName);
    ResultObj->SetStringField(TEXT("search_root"), SearchRoot);
    ResultObj->SetStringField(TEXT("class_filter"), ClassFilter);
    ResultObj->SetNumberField(TEXT("match_count"), Matches.Num());
    ResultObj->SetBoolField(TEXT("unambiguous"), Matches.Num() == 1);
    ResultObj->SetArrayField(TEXT("matches"), MatchArray);

    return ResultObj;
}

// MCP:get_assets_by_names - Batch variant of get_asset_by_name: resolve N asset names in ONE round-trip instead of N calls, returning per entry a found flag plus that asset's identity (class, object path, package, is_loaded) or the error that stopped it. Delegates to get_asset_by_name literally, so it inherits both its /Game-only fallback scan and its ambiguous_matches transparency -- an ambiguous name still resolves to the first hit, so check that field per entry. Use get_assets_metadata_batch when you want registry TAGS rather than identity.
//   @param asset_names - Array of exact asset names (or full package names) to resolve (required)
//   @param load_full_data - When true, adds the type-specific details of get_asset_by_name per asset. Defaults to FALSE here -- the opposite of the single-asset tool -- so a batch call cannot accidentally force N synchronous loads.
//   @returns { requested_count: int, resolved: [{ asset_name, found: bool, name?, class?, path?, package?, is_loaded?, ambiguous_matches?, ... (same fields as get_asset_by_name when found) }] }

TSharedPtr<FJsonObject> HandleGetAssetsByNames(const TSharedPtr<FJsonObject>& Params)
{
    const TArray<TSharedPtr<FJsonValue>>* NamesArray = nullptr;
    if (!Params->TryGetArrayField(TEXT("asset_names"), NamesArray) || !NamesArray)
    {
        return mcp_common::MissingParameterReply(TEXT("asset_names"));
    }

    bool bLoadFullData = false; // batch default = cheap path, unlike get_asset_by_name's default-true
    Params->TryGetBoolField(TEXT("load_full_data"), bLoadFullData);

    // Resolve each name by delegating to get_asset_by_name
    TArray<TSharedPtr<FJsonValue>> ResolvedArray;
    for (const TSharedPtr<FJsonValue>& NameValue : *NamesArray)
    {
        FString AssetName;
        if (!NameValue.IsValid() || !NameValue->TryGetString(AssetName) || AssetName.IsEmpty())
        {
            ResolvedArray.Add(UEAgentAssetCommandsImpl::InvalidBatchRow(TEXT("asset_name"), TEXT("Invalid or empty entry in asset_names")));
            continue;
        }

        TSharedPtr<FJsonObject> SubParams = MakeShared<FJsonObject>();
        SubParams->SetStringField(TEXT("asset_name"), AssetName);
        SubParams->SetBoolField(TEXT("load_full_data"), bLoadFullData);

        TSharedPtr<FJsonObject> SingleResult = HandleGetAssetByName(SubParams);
        ResolvedArray.Add(UEAgentAssetCommandsImpl::BatchRow(TEXT("asset_name"), AssetName, SingleResult));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetNumberField(TEXT("requested_count"), NamesArray->Num());
    ResultObj->SetArrayField(TEXT("resolved"), ResolvedArray);

    return ResultObj;
}

// MCP:get_assets_metadata_batch - Batch variant of get_asset_metadata: read the Asset Registry tags of N assets in ONE round-trip instead of N calls, returning per entry a found flag plus the same fields get_asset_metadata emits (or an error string). It runs get_asset_metadata for each entry, so it inherits its path semantics exactly -- including the trap that each entry is prefixed with "/Game/", so entries must be paths RELATIVE to /Game, never bare names and never absolute "/Game/..." paths. Use get_assets_by_names for identity/type info from bare NAMES instead of tags.
//   @param asset_paths - Array of paths RELATIVE to /Game with no leading slash (e.g. "MyGame/Systems/Foo/BP_Foo.BP_Foo"); each is passed to get_asset_metadata, which prepends "/Game/" itself (required)
//   @param max_results - Safety cap on how many array entries get processed (1..5000); defaults to 500. Entries past the cap are dropped and results_truncated goes true.
//   @returns { requested_count: int, processed_count: int, results_truncated: bool, results: [{ asset_path, found: bool, name?, class?, path?, package?, asset_class_path?, package_path?, tags?, error? }] }

TSharedPtr<FJsonObject> HandleGetAssetsMetadataBatch(const TSharedPtr<FJsonObject>& Params)
{
    const TArray<TSharedPtr<FJsonValue>>* PathsArray = nullptr;
    if (!Params->TryGetArrayField(TEXT("asset_paths"), PathsArray) || !PathsArray)
    {
        return mcp_common::MissingParameterReply(TEXT("asset_paths"));
    }

    int32 MaxResults = 500;
    Params->TryGetNumberField(TEXT("max_results"), MaxResults);
    if (MaxResults < 1)
    {
        MaxResults = 1;
    }
    else if (MaxResults > 5000)
    {
        MaxResults = 5000;
    }

    // Resolve each path by delegating to get_asset_metadata
    TArray<TSharedPtr<FJsonValue>> ResultsArray;
    int32 ProcessedCount = 0;
    for (const TSharedPtr<FJsonValue>& PathValue : *PathsArray)
    {
        if (ProcessedCount >= MaxResults)
        {
            break;
        }

        FString AssetPath;
        if (!PathValue.IsValid() || !PathValue->TryGetString(AssetPath) || AssetPath.IsEmpty())
        {
            ResultsArray.Add(UEAgentAssetCommandsImpl::InvalidBatchRow(TEXT("asset_path"), TEXT("Invalid or empty entry in asset_paths")));
            ProcessedCount++;
            continue;
        }

        TSharedPtr<FJsonObject> SubParams = MakeShared<FJsonObject>();
        SubParams->SetStringField(TEXT("asset_name"), AssetPath); // get_asset_metadata's own param name/semantics

        TSharedPtr<FJsonObject> SingleResult = HandleGetAssetMetadata(SubParams);
        ResultsArray.Add(UEAgentAssetCommandsImpl::BatchRow(TEXT("asset_path"), AssetPath, SingleResult));
        ProcessedCount++;
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetNumberField(TEXT("requested_count"), PathsArray->Num());
    ResultObj->SetNumberField(TEXT("processed_count"), ProcessedCount);
    ResultObj->SetBoolField(TEXT("results_truncated"), PathsArray->Num() > ProcessedCount);
    ResultObj->SetArrayField(TEXT("results"), ResultsArray);

    return ResultObj;
}

// ===========================================================================
// Typed remove commands: each deletes an asset only after verifying it is of the
// expected class, which is what sets them apart from the generic delete_asset. A
// path that does not load answers deleted:false with a `note` and success, so they
// are idempotent; an asset of another class is refused, and a delete the engine
// does not carry out fails the call (AssetCommandHelpers::DeleteTypedAsset). The
// class is a parameter rather than a template argument so the catalog generator
// follows each command into this body.
// ===========================================================================

static TSharedPtr<FJsonObject> HandleRemoveTypedAssetCmd(const TSharedPtr<FJsonObject>& Params, const TCHAR* CmdName,
    const UClass* ExpectedClass)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("[%s] Missing 'asset_path' parameter"), CmdName));
    }

    return AssetCommandHelpers::DeleteTypedAsset(CmdName, AssetPath, ExpectedClass);
}

// MCP:remove_render_target_2d - Delete a render target asset, but only after loading it and verifying it really is a UTextureRenderTarget2D -- the typed counterpart of create_render_target_2d, and what makes it safer than the generic delete_asset, which deletes whatever sits at that path. A missing path returns deleted:false with a `note` and success, so it is idempotent and safe to re-run; an asset of another type is refused with success false and deleted:false, and a delete the engine refuses (Play-in-Editor running, a read-only file) is success false too. Sisters by type: remove_render_target_2d_array, remove_render_target_cube, remove_texture_2d_array, remove_static_texture_from_render_target.
//   @param asset_path - Full object/package path of the render target (e.g. /Game/MyGame/RT/RT_Foo.RT_Foo); a bare name is not resolved here
//   @returns { asset_path, asset_class, package_name, deleted, already_existed, note? }
TSharedPtr<FJsonObject> HandleRemoveRenderTarget2D(const TSharedPtr<FJsonObject>& Params)
{
    return HandleRemoveTypedAssetCmd(Params, TEXT("remove_render_target_2d"), UTextureRenderTarget2D::StaticClass());
}

// MCP:remove_render_target_2d_array - Delete a 2D-ARRAY render target, but only after loading it and verifying it really is a UTextureRenderTarget2DArray -- the typed counterpart of create_render_target_2d_array, and safer than the generic delete_asset, which deletes whatever sits at that path. A missing path returns deleted:false with a `note` and success, so it is idempotent; a wrong type, or a delete the engine refuses, is success false with deleted:false. Use remove_render_target_2d for a plain 2D render target and remove_texture_2d_array for a static UTexture2DArray.
//   @param asset_path - Full object/package path of the 2D-array render target; a bare name is not resolved here
//   @returns { asset_path, asset_class, package_name, deleted, already_existed, note? }
TSharedPtr<FJsonObject> HandleRemoveRenderTarget2DArray(const TSharedPtr<FJsonObject>& Params)
{
    return HandleRemoveTypedAssetCmd(Params, TEXT("remove_render_target_2d_array"), UTextureRenderTarget2DArray::StaticClass());
}

// MCP:remove_render_target_cube - Delete a CUBE render target, but only after loading it and verifying it really is a UTextureRenderTargetCube -- the typed counterpart of create_render_target_cube, and safer than the generic delete_asset, which deletes whatever sits at that path. A missing path returns deleted:false with a `note` and success, so it is idempotent; a wrong type, or a delete the engine refuses, is success false with deleted:false. Use remove_render_target_2d / remove_render_target_2d_array for the flat variants.
//   @param asset_path - Full object/package path of the cube render target; a bare name is not resolved here
//   @returns { asset_path, asset_class, package_name, deleted, already_existed, note? }
TSharedPtr<FJsonObject> HandleRemoveRenderTargetCube(const TSharedPtr<FJsonObject>& Params)
{
    return HandleRemoveTypedAssetCmd(Params, TEXT("remove_render_target_cube"), UTextureRenderTargetCube::StaticClass());
}

// MCP:remove_texture_2d_array - Delete a static TEXTURE array, but only after loading it and verifying it really is a UTexture2DArray -- the typed counterpart of create_texture_2d_array, and safer than the generic delete_asset, which deletes whatever sits at that path. A missing path returns deleted:false with a `note` and success, so it is idempotent; a wrong type, or a delete the engine refuses, is success false with deleted:false. Do not confuse it with remove_render_target_2d_array, which deletes the RENDER TARGET array, not this static texture.
//   @param asset_path - Full object/package path of the 2D-array texture; a bare name is not resolved here
//   @returns { asset_path, asset_class, package_name, deleted, already_existed, note? }
TSharedPtr<FJsonObject> HandleRemoveTexture2DArray(const TSharedPtr<FJsonObject>& Params)
{
    return HandleRemoveTypedAssetCmd(Params, TEXT("remove_texture_2d_array"), UTexture2DArray::StaticClass());
}

// MCP:remove_static_texture_from_render_target - Delete the static UTexture2D that create_static_texture_from_render_target baked out of a render target (it lives beside the render target as a sibling asset), verifying the target really is a UTexture2D first -- so it refuses to delete the RENDER TARGET itself, which is remove_render_target_2d's job. Safer than the generic delete_asset, which deletes whatever sits at that path; a missing path returns deleted:false with a `note` and success, so it is idempotent, while a wrong type, or a delete the engine refuses, is success false with deleted:false.
//   @param asset_path - Full object/package path of the baked static texture; a bare name is not resolved here
//   @returns { asset_path, asset_class, package_name, deleted, already_existed, note? }
TSharedPtr<FJsonObject> HandleRemoveStaticTextureFromRenderTarget(const TSharedPtr<FJsonObject>& Params)
{
    return HandleRemoveTypedAssetCmd(Params, TEXT("remove_static_texture_from_render_target"), UTexture2D::StaticClass());
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("get_asset_by_name"), &HandleGetAssetByName },
    { TEXT("get_asset_file_size"), &HandleGetAssetFileSize },
    { TEXT("get_asset_last_modified"), &HandleGetAssetLastModified },
    { TEXT("get_asset_metadata"), &HandleGetAssetMetadata },
    { TEXT("get_asset_thumbnail"), &HandleGetAssetThumbnail },
    { TEXT("get_assets_by_names"), &HandleGetAssetsByNames },
    { TEXT("get_assets_by_tag"), &HandleGetAssetsByTag },
    { TEXT("get_assets_by_type"), &HandleGetAssetsByType },
    { TEXT("get_assets_in_folder"), &HandleGetAssetsInFolder },
    { TEXT("get_assets_metadata_batch"), &HandleGetAssetsMetadataBatch },
    { TEXT("remove_render_target_2d"), &HandleRemoveRenderTarget2D },
    { TEXT("remove_render_target_2d_array"), &HandleRemoveRenderTarget2DArray },
    { TEXT("remove_render_target_cube"), &HandleRemoveRenderTargetCube },
    { TEXT("remove_static_texture_from_render_target"), &HandleRemoveStaticTextureFromRenderTarget },
    { TEXT("remove_texture_2d_array"), &HandleRemoveTexture2DArray },
    { TEXT("resolve_asset_by_name"), &HandleResolveAssetByName },
    { TEXT("search_assets_by_pattern"), &HandleSearchAssetsByPattern },
};
} // namespace UEAgentAssetCommands
