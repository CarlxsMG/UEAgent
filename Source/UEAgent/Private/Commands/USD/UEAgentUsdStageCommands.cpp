// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

// UnrealUSDWrapper headless USD API (UE::FUsdStage / FUsdPrim / FSdfLayer / FSdfPath)
// + UnrealUSDWrapper static factory functions. UNREALUSDWRAPPER_API.
#include "UnrealUSDWrapper.h"
#include "Commands/USD/UEAgentUsdRegistry.h"  // stage registry + helpers shared by the USD handlers
#include "UsdWrappers/UsdStage.h"
#include "UsdWrappers/UsdPrim.h"
#include "UsdWrappers/SdfLayer.h"
#include "UsdWrappers/SdfPath.h"
#include "UsdWrappers/ForwardDeclarations.h"

/**
 * USD stage I/O commands.
 *
 * Reads, opens, creates, lists, mutates, saves and flattens USD stages via the
 * UnrealUSDWrapper headless API (no UObject, no World, no PIE).
 *
 * Engine APIs (module UnrealUSDWrapper):
 *   - UnrealUSDWrapper::OpenStage / NewStage / GetAllStagesFromCache /
 *     EraseStageFromCache / GetNativeFileFormats / GetAllSupportedFileFormats /
 *     AddUsdImportFileFormatDescriptions (UnrealUSDWrapper.h)
 *   - UE::FUsdStage::GetPseudoRoot / GetRootLayer / GetLayerStack /
 *     SetStartTimeCode / SetEndTimeCode / SetFramesPerSecond /
 *     GetStartTimeCode / GetEndTimeCode / GetFramesPerSecond / Export
 *     (UsdWrappers/UsdStage.h)
 *   - UE::FSdfLayer::Save(bool bForce=false) / IsDirty / GetIdentifier /
 *     GetRealPath (UsdWrappers/SdfLayer.h)
 *   - UE::FUsdPrim::GetChildren (UsdWrappers/UsdPrim.h)
 *
 * USD errors are captured through UEAgent::UsdCommon::CaptureUsdErrors (UEAgentUsdRegistry.h), which
 * wraps the deprecated UsdUtils::StartMonitoringErrors / GetErrorsAndStopMonitoring (USDUtilities): the
 * non-deprecated replacement (FScopedUsdMessageLog + FUsdLogManager::HasAccumulatedErrors) exposes only
 * the presence of errors, not their text, which the LLM-facing error payload needs.
 *
 * Modules: UnrealUSDWrapper, USDUtilities (Runtime).
 * Plugin: USDCore (EnabledByDefault:false; UEAgent.uplugin enables it).
 */

// LLM-facing stage index, keyed by an arbitrary stage_name. The
// UnrealUSDWrapper has its OWN stage cache keyed by file path; this is a
// separate index so the LLM can refer to stages by a stable name across
// open/create calls. Memory stages (no file_path) live here ONLY — they
// cannot be retrieved by any other path.
//
// STORAGE: this lives in `UEAgent::UsdRegistry::GStageNameCache`
// (UEAgentUsdRegistry.h, C++17 inline variable). Both UsdStageCommands and
// UsdPrimCommands consult the SAME storage (one cache, not one per family).
namespace UEAgentUsdStageCommands
{

// MCP-DOMAIN: USD

namespace
{
    // ---- Enums / params helpers ---------------------------------------------

    // Map a string ("LoadAll" / "LoadNone") to EUsdInitialLoadSet. Unknown values
    // fall back to LoadAll (the OpenStage default — the safe choice for an
    // inspection workflow).
    EUsdInitialLoadSet ParseInitialLoadSet(const FString& InValue)
    {
        const FString V = InValue.ToLower();
        if (V == TEXT("loadnone")) { return EUsdInitialLoadSet::LoadNone; }
        return EUsdInitialLoadSet::LoadAll;
    }

    // ---- prim recursion ------------------------------------------------------

    // Walk the prim tree breadth-first and count all prims (including the
    // pseudo-root). Stage.GetPseudoRoot() is always valid, so no null-check
    // needed at the entry point. No recursion limit — USD stages are flat
    // enough that this is safe for any realistic fixture.
    int32 CountPrimsRecursive(const UE::FUsdPrim& Prim)
    {
        if (!Prim) { return 0; }
        int32 Count = 1;  // self
        for (const UE::FUsdPrim& Child : Prim.GetChildren())
        {
            Count += CountPrimsRecursive(Child);
        }
        return Count;
    }

    // ---- stage resolution ---------------------------------------------------

    // Resolve a stage by stage_name. Two paths:
    //   1) stage_name present in our LLM-facing UEAgent::UsdRegistry::GStageNameCache -> use it.
    //   2) stage_name NOT in cache -> treat as a file path and try OpenStage
    //      (LoadAll, use cache). If OpenStage succeeds, auto-insert it into
    //      the cache under stage_name so subsequent calls are O(1).
    //
    // Returns an invalid (operator bool()==false) stage on failure. Always
    // populates OutResolution with one of "cache_hit" / "auto_opened" /
    // "missing" so callers can decide whether to error or auto-create.
    UE::FUsdStage ResolveStageByName(
        const FString& StageName,
        TMap<FString, UE::FUsdStage>& Cache,
        FString& OutResolution)
    {
        if (UE::FUsdStage* Found = Cache.Find(StageName))
        {
            if (*Found)
            {
                OutResolution = TEXT("cache_hit");
                return *Found;
            }
            // stale entry — fall through and try to re-open
        }

        // Treat StageName as a file path and try to open it.
        UE::FUsdStage Opened = UnrealUSDWrapper::OpenStage(
            *StageName,
            EUsdInitialLoadSet::LoadAll,
            /*bUseStageCache*/ true,
            /*bForceReload*/ false);

        if (Opened)
        {
            Cache.Add(StageName, Opened);
            OutResolution = TEXT("auto_opened");
            return Opened;
        }

        OutResolution = TEXT("missing");
        return UE::FUsdStage();
    }

    // ResolveStageByName over the registry, run under the USD error monitor so its errors land in Result.
    UE::FUsdStage UsdStage_Resolve(const FString& StageName, const TSharedPtr<FJsonObject>& Result, FString& OutResolution)
    {
        UE::FUsdStage Stage;
        UEAgent::UsdCommon::CaptureUsdErrors(
            [&Stage, &StageName, &OutResolution]()
            {
                Stage = ResolveStageByName(StageName, UEAgent::UsdRegistry::GStageNameCache, OutResolution);
            },
            Result);
        return Stage;
    }
} // namespace

// ---------------------------------------------------------------------------
// list_usd_file_formats
// ---------------------------------------------------------------------------
// MCP:list_usd_file_formats - List the USD file format extensions the engine supports, with optional human-readable descriptions -- also the probe of whether the USDCore plugin loaded: native_present / supported_present false (a zero count) means it did not.  Always returns the native set ("usd"/"usda"/"usdc"/"usdz", UnrealUSDWrapper::GetNativeFileFormats) and the full supported set (native + third-party USD plugin extensions like "abc" from usdAbc, GetAllSupportedFileFormats). When include_descriptions is true, also calls AddUsdImportFileFormatDescriptions to populate the human-readable per-extension description strings (e.g. "usdc; Universal Scene Description binary files").
//   @param include_descriptions [bool] - Also return the per-extension import description strings from UnrealUSDWrapper::AddUsdImportFileFormatDescriptions (optional, default false).
//   @returns { native_formats: [string], supported_formats: [string], native_count: int, supported_count: int, native_present: bool, supported_present: bool, descriptions: [{format: string, description: string}]? }
//   @hints read-only

TSharedPtr<FJsonObject> HandleListUsdFileFormats(const TSharedPtr<FJsonObject>& Params)
{
    bool bIncludeDescriptions = false;
    Params->TryGetBoolField(TEXT("include_descriptions"), bIncludeDescriptions);

    const TArray<FString> Native = UnrealUSDWrapper::GetNativeFileFormats();
    const TArray<FString> All    = UnrealUSDWrapper::GetAllSupportedFileFormats();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

    Data->SetArrayField(TEXT("native_formats"), mcp_common::StringsToJson(Native));
    Data->SetArrayField(TEXT("supported_formats"), mcp_common::StringsToJson(All));

    Data->SetNumberField(TEXT("native_count"),    Native.Num());
    Data->SetNumberField(TEXT("supported_count"), All.Num());

    // A zero count on either means USDCore did not load.
    Data->SetBoolField(TEXT("native_present"),    Native.Num() > 0);
    Data->SetBoolField(TEXT("supported_present"), All.Num() > 0);

    if (bIncludeDescriptions)
    {
        // AddUsdImportFileFormatDescriptions emits strings like
        // "usda; Universal Scene Description text files" — split on the first
        // ';' to pair (format, description) for the LLM.
        TArray<FString> RawDescriptions;
        UnrealUSDWrapper::AddUsdImportFileFormatDescriptions(RawDescriptions);

        TArray<TSharedPtr<FJsonValue>> DescArr;
        for (const FString& Raw : RawDescriptions)
        {
            FString Format, Description;
            if (Raw.Split(TEXT(";"), &Format, &Description))
            {
                Format.TrimStartAndEndInline();
                Description.TrimStartAndEndInline();
            }
            else
            {
                Format = Raw;
            }
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("format"), Format);
            Entry->SetStringField(TEXT("description"), Description);
            DescArr.Add(MakeShared<FJsonValueObject>(Entry));
        }
        Data->SetArrayField(TEXT("descriptions"), DescArr);
    }

    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// open_usd_stage
// ---------------------------------------------------------------------------
// MCP:open_usd_stage - Open an existing USD stage from a file path.  Wraps UnrealUSDWrapper::OpenStage with caching and force-reload controls. When use_stage_cache is true (default) and the file is already open in the UnrealUSDWrapper stage cache, the existing stage is returned. When force_reload is true, all local layers are forced-reloaded from disk (useful when the file was modified by another tool since the last open). The opened stage is added to the LLM-facing UEAgent::UsdRegistry::GStageNameCache under either the explicit stage_name or a derived one (basename without extension). The stage_id returned is the root layer's USD identifier — a stable string handle useful as a unique reference across follow-up commands.
//   @param file_path [string!] - Absolute path to the .usd/.usda/.usdc/.usdz file (or a USD layer identifier).
//   @param stage_name [string] - LLM-facing name to register the opened stage under. Defaults to the file basename without extension (optional).
//   @param initial_load_set [string] - "LoadAll" (default; loads payloads) or "LoadNone" (faster; inspect without loading payloads).
//   @param use_stage_cache [bool] - Use UnrealUSDWrapper's stage cache (default true). When false the stage is re-read and not added to the cache.
//   @param force_reload [bool] - Force-reload all local layers from disk, bypassing the layer registry (default false).
//   @returns { stage_name: string, stage_id: string, identifier: string, real_path: string, from_cache: bool, file_path: string, errors: [string]? }

TSharedPtr<FJsonObject> HandleOpenUsdStage(const TSharedPtr<FJsonObject>& Params)
{
    FString FilePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("file_path"), FilePath)) { return Missing; }

    FString StageName;
    if (!Params->TryGetStringField(TEXT("stage_name"), StageName) || StageName.IsEmpty())
    {
        // Default: derive from the file basename without extension.
        StageName = FPaths::GetBaseFilename(FilePath);
        if (StageName.IsEmpty())
        {
            StageName = FilePath;
        }
    }

    FString InitialLoadSetStr = TEXT("LoadAll");
    Params->TryGetStringField(TEXT("initial_load_set"), InitialLoadSetStr);
    const EUsdInitialLoadSet LoadSet = ParseInitialLoadSet(InitialLoadSetStr);

    bool bUseStageCache = true;
    bool bForceReload   = false;
    Params->TryGetBoolField(TEXT("use_stage_cache"), bUseStageCache);
    Params->TryGetBoolField(TEXT("force_reload"),    bForceReload);

    // Was this path already open in the UnrealUSDWrapper stage cache before our
    // call? We can only know by comparing the post-OpenStage identity against
    // a pre-fetched list (cheap — UnrealUSDWrapper does NOT mutate this).
    UE::FUsdStage PreExisting;
    for (const UE::FUsdStage& S : UnrealUSDWrapper::GetAllStagesFromCache())
    {
        if (S && S.GetRootLayer().GetRealPath() == FilePath)
        {
            PreExisting = S;
            break;
        }
    }
    const bool bWasAlreadyCached = static_cast<bool>(PreExisting);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("file_path"), FilePath);
    Result->SetStringField(TEXT("stage_name"), StageName);

    UE::FUsdStage Stage;
    UEAgent::UsdCommon::CaptureUsdErrors(
        [&Stage, &FilePath, LoadSet, bUseStageCache, bForceReload]()
        {
            Stage = UnrealUSDWrapper::OpenStage(
                *FilePath, LoadSet, bUseStageCache, bForceReload);
        },
        Result);

    if (!Stage)
    {
        // Failing on Result itself keeps the captured USD errors in the reply;
        // the 1-arg MakeErrorReply would drop them.
        return UEAgent::UsdCommon::FailReply(Result, FString::Printf(TEXT("OpenStage failed for '%s'"), *FilePath));
    }

    // Register under the LLM-facing name. Overwrites any prior entry with the
    // same name (intentional — the new open wins).
    UEAgent::UsdRegistry::GStageNameCache.Add(StageName, Stage);

    Result->SetBoolField(TEXT("from_cache"), bWasAlreadyCached);

    const UE::FSdfLayer RootLayer = Stage.GetRootLayer();
    Result->SetStringField(TEXT("identifier"), RootLayer.GetIdentifier());
    Result->SetStringField(TEXT("real_path"),  RootLayer.GetRealPath());

    // stage_id: string handle derived from the root layer's identifier (stable
    // for on-disk stages, anonymous for memory stages). The UnrealUSDWrapper
    // does NOT expose GetTypeHash(FUsdStage) in the public header, so we use
    // the identifier as the stable cross-call handle — already a FString, and
    // more useful to the LLM than a pointer hash would be.
    Result->SetStringField(TEXT("stage_id"), RootLayer.GetIdentifier());

    return mcp_common::MakeOkReply(Result);
}

// ---------------------------------------------------------------------------
// create_usd_stage
// ---------------------------------------------------------------------------
// MCP:create_usd_stage - Create a new USD stage (either on-disk or in-memory).  When file_path is provided, UnrealUSDWrapper::NewStage(FilePath) creates a root layer file at that path and opens it as a stage. When file_path is omitted or empty, NewStage() (no args) creates a memory-only stage — this is the only way to author a USD stage that is NOT persisted to disk; the only way to clean it up is close_usd_stage. The new stage is always added to the LLM-facing UEAgent::UsdRegistry::GStageNameCache under stage_name (default "Untitled"), so subsequent calls can refer to it by name. The returned identifier is the root layer's USD identifier (real path for on-disk; an anonymous "anon:0x..." handle for memory stages).
//   @param file_path [string] - Absolute path for the new USD root layer file (.usd/.usda/.usdc/.usdz). Omit or empty for a memory-only stage (optional).
//   @param stage_name [string] - LLM-facing name to register the new stage under (optional, default "Untitled").
//   @returns { stage_name: string, file_path: string, memory: bool, identifier: string, real_path: string, dirty: bool, is_valid: bool, errors: [string]? }

TSharedPtr<FJsonObject> HandleCreateUsdStage(const TSharedPtr<FJsonObject>& Params)
{
    FString FilePath;
    Params->TryGetStringField(TEXT("file_path"), FilePath);

    FString StageName = TEXT("Untitled");
    Params->TryGetStringField(TEXT("stage_name"), StageName);
    if (StageName.IsEmpty())
    {
        StageName = TEXT("Untitled");
    }

    const bool bMemory = FilePath.IsEmpty();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("stage_name"), StageName);
    Result->SetBoolField(TEXT("memory"), bMemory);

    UE::FUsdStage Stage;
    if (bMemory)
    {
        UEAgent::UsdCommon::CaptureUsdErrors(
            [&Stage]() { Stage = UnrealUSDWrapper::NewStage(); },
            Result);
    }
    else
    {
        Result->SetStringField(TEXT("file_path"), FilePath);
        UEAgent::UsdCommon::CaptureUsdErrors(
            [&Stage, &FilePath]() { Stage = UnrealUSDWrapper::NewStage(*FilePath); },
            Result);
    }

    if (!Stage)
    {
        return UEAgent::UsdCommon::FailReply(Result,
            bMemory
                ? FString(TEXT("NewStage() (memory) failed"))
                : FString::Printf(TEXT("NewStage('%s') failed"), *FilePath));
    }

    UEAgent::UsdRegistry::GStageNameCache.Add(StageName, Stage);

    const UE::FSdfLayer RootLayer = Stage.GetRootLayer();
    Result->SetStringField(TEXT("identifier"), RootLayer.GetIdentifier());
    Result->SetStringField(TEXT("real_path"),  RootLayer.GetRealPath());
    Result->SetBoolField(TEXT("dirty"),       RootLayer.IsDirty());
    Result->SetBoolField(TEXT("is_valid"),    static_cast<bool>(Stage));

    return mcp_common::MakeOkReply(Result);
}

// ---------------------------------------------------------------------------
// close_usd_stage
// ---------------------------------------------------------------------------
// MCP:close_usd_stage - Close a USD stage by its LLM-facing stage_name.  Removes the stage from the LLM-facing UEAgent::UsdRegistry::GStageNameCache AND calls UnrealUSDWrapper::EraseStageFromCache on it — this is the ONLY way to release a memory stage (memory stages are never closed automatically, even on editor shutdown). If stage_name is not in the cache the call returns success with a note rather than an error (idempotent — closing an already-closed stage is a no-op). The 'removed_from_cache' field reports whether the stage was actually present in the UnrealUSDWrapper stage cache at close time.
//   @param stage_name [string!] - Name the stage was registered under via open_usd_stage or create_usd_stage.
//   @returns { stage_name: string, closed: bool, was_in_cache: bool, removed_from_cache: bool, note: string? }

TSharedPtr<FJsonObject> HandleCloseUsdStage(const TSharedPtr<FJsonObject>& Params)
{
    FString StageName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("stage_name"), StageName)) { return Missing; }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("stage_name"), StageName);

    UE::FUsdStage* Found = UEAgent::UsdRegistry::GStageNameCache.Find(StageName);
    if (!Found || !(*Found))
    {
        // Idempotent: not in cache = already closed. Return success with a note.
        Data->SetBoolField(TEXT("closed"), true);
        Data->SetBoolField(TEXT("was_in_cache"), false);
        Data->SetBoolField(TEXT("removed_from_cache"), false);
        Data->SetStringField(TEXT("note"), TEXT("stage_name not in cache (already closed or never opened)"));
        return mcp_common::MakeOkReply(Data);
    }

    const UE::FUsdStage Stage = *Found;

    // Was it in the UnrealUSDWrapper stage cache? Snapshot before erase.
    bool bWasInUsdCache = false;
    for (const UE::FUsdStage& S : UnrealUSDWrapper::GetAllStagesFromCache())
    {
        if (S && S == Stage)  // FUsdStage operator== compares underlying pxr::UsdStageRefPtr
        {
            bWasInUsdCache = true;
            break;
        }
    }

    UnrealUSDWrapper::EraseStageFromCache(Stage);
    UEAgent::UsdRegistry::GStageNameCache.Remove(StageName);

    Data->SetBoolField(TEXT("closed"), true);
    Data->SetBoolField(TEXT("was_in_cache"), bWasInUsdCache);
    Data->SetBoolField(TEXT("removed_from_cache"), bWasInUsdCache);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// list_usd_stages
// ---------------------------------------------------------------------------
// MCP:list_usd_stages - Enumerate every stage currently in the UnrealUSDWrapper stage cache (shared across C++ + Python).  Returns one entry per cached stage with the root layer identifier, real path and dirty flag. It reads the engine's stage cache directly, so it also lists the stages opened outside these tools (Python USD scripts, the editor's USD importer). Each entry's 'stage_name' is the name the stage was opened under through these tools; it is empty for a stage they did not open.
//   @returns { stages: [{stage_name: string, identifier: string, real_path: string, dirty: bool}], count: int }

TSharedPtr<FJsonObject> HandleListUsdStages(const TSharedPtr<FJsonObject>& Params)
{
    const TArray<UE::FUsdStage> Stages = UnrealUSDWrapper::GetAllStagesFromCache();

    // Build a reverse map from FUsdStage identity -> LLM-facing stage_name by
    // scanning the UEAgent::UsdRegistry::GStageNameCache once. Operator== on FUsdStage compares the
    // underlying pxr::UsdStageRefPtr, which is the same identity check the
    // UnrealUSDWrapper stage cache uses, so a match is reliable.
    auto FindStageName = [](const UE::FUsdStage& S) -> FString
    {
        for (const TPair<FString, UE::FUsdStage>& Pair : UEAgent::UsdRegistry::GStageNameCache)
        {
            if (Pair.Value && Pair.Value == S) { return Pair.Key; }
        }
        return FString();
    };

    TArray<TSharedPtr<FJsonValue>> Arr;
    for (const UE::FUsdStage& S : Stages)
    {
        if (!S) { continue; }

        const UE::FSdfLayer Root = S.GetRootLayer();
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("stage_name"), FindStageName(S));
        Entry->SetStringField(TEXT("identifier"), Root.GetIdentifier());
        Entry->SetStringField(TEXT("real_path"),  Root.GetRealPath());
        Entry->SetBoolField(TEXT("dirty"),       Root.IsDirty());
        Arr.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("stages"), Arr);
    Data->SetNumberField(TEXT("count"), Arr.Num());
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_usd_stage_info
// ---------------------------------------------------------------------------
// MCP:get_usd_stage_info - Read metadata about a USD stage: root layer identifier, real path, dirty flag, timecode range (start/end), frames-per-second, total prim count (recursive) and layer-stack depth (includes session layers when include_session_layers is true).  Resolves stage_name via the LLM-facing UEAgent::UsdRegistry::GStageNameCache first, then falls back to treating stage_name as a file path and auto-opening it (LoadAll, use cache). Returns 'resolution' as one of "cache_hit" / "auto_opened" so the caller knows which path served the stage.
//   @param stage_name [string!] - Name the stage was registered under, OR an absolute file path to auto-open.
//   @param include_session_layers [bool] - Include session layers when computing the layer-stack depth (default true).
//   @returns { stage_name: string, resolution: string, identifier: string, real_path: string, dirty: bool, start_time: number, end_time: number, frames_per_second: number, prim_count: int, layer_stack_count: int, has_start_time: bool, has_end_time: bool, has_fps: bool, errors: [string]? }

TSharedPtr<FJsonObject> HandleGetUsdStageInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString StageName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("stage_name"), StageName)) { return Missing; }

    bool bIncludeSession = true;
    Params->TryGetBoolField(TEXT("include_session_layers"), bIncludeSession);

    FString Resolution;
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("stage_name"), StageName);

    UE::FUsdStage Stage = UsdStage_Resolve(StageName, Result, Resolution);
    if (!Stage)
    {
        return UEAgent::UsdCommon::FailReply(Result,
            FString::Printf(TEXT("Stage '%s' could not be resolved (cache miss and file open failed)"), *StageName));
    }

    Result->SetStringField(TEXT("resolution"), Resolution);

    const UE::FSdfLayer RootLayer = Stage.GetRootLayer();
    Result->SetStringField(TEXT("identifier"), RootLayer.GetIdentifier());
    Result->SetStringField(TEXT("real_path"),  RootLayer.GetRealPath());
    Result->SetBoolField(TEXT("dirty"),       RootLayer.IsDirty());

    Result->SetNumberField(TEXT("start_time"),        Stage.GetStartTimeCode());
    Result->SetNumberField(TEXT("end_time"),          Stage.GetEndTimeCode());
    Result->SetNumberField(TEXT("frames_per_second"), Stage.GetFramesPerSecond());

    // prim count (recursive from the pseudo-root)
    Result->SetNumberField(TEXT("prim_count"), CountPrimsRecursive(Stage.GetPseudoRoot()));

    // layer stack depth
    Result->SetNumberField(TEXT("layer_stack_count"), Stage.GetLayerStack(bIncludeSession).Num());

    return mcp_common::MakeOkReply(Result);
}

// ---------------------------------------------------------------------------
// set_usd_stage_time_range
// ---------------------------------------------------------------------------
// MCP:set_usd_stage_time_range - Set a USD stage's start/end time codes and optionally its frames-per-second, then save the root layer (idempotent — no-op when !dirty).  Both start_time and end_time are required and are written via UE::FUsdStage::SetStartTimeCode / SetEndTimeCode (which write to the stage's root layer metadata). When frames_per_second > 0, also calls SetFramesPerSecond. The Save() at the end is force=false so a layer that wasn't actually mutated by the Set* calls will not produce an unnecessary write.
//   @param stage_name [string!] - Name the stage was registered under, OR an absolute file path to auto-open.
//   @param start_time [number!] - New start time code (e.g. 0.0).
//   @param end_time [number!] - New end time code (e.g. 24.0).
//   @param frames_per_second [number] - Optional frames-per-second override; values <= 0 are ignored (use get_usd_stage_info / set_usd_stage_time_range to leave fps untouched).
//   @returns { stage_name: string, resolution: string, start_time: number, end_time: number, frames_per_second: number, applied: bool, saved: bool, dirty: bool, errors: [string]? }

TSharedPtr<FJsonObject> HandleSetUsdStageTimeRange(const TSharedPtr<FJsonObject>& Params)
{
    FString StageName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("stage_name"), StageName)) { return Missing; }

    double StartTime = 0.0;
    double EndTime   = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("start_time"), StartTime)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("end_time"), EndTime)) { return Missing; }

    double Fps = -1.0;
    Params->TryGetNumberField(TEXT("frames_per_second"), Fps);

    FString Resolution;
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("stage_name"), StageName);

    UE::FUsdStage Stage = UsdStage_Resolve(StageName, Result, Resolution);
    if (!Stage)
    {
        return UEAgent::UsdCommon::FailReply(Result, FString::Printf(TEXT("Stage '%s' could not be resolved"), *StageName));
    }

    Result->SetStringField(TEXT("resolution"), Resolution);

    bool bApplied = false;
    UEAgent::UsdCommon::CaptureUsdErrors(
        [&Stage, StartTime, EndTime, Fps, &bApplied]()
        {
            Stage.SetStartTimeCode(StartTime);
            Stage.SetEndTimeCode(EndTime);
            if (Fps > 0.0)
            {
                Stage.SetFramesPerSecond(Fps);
            }
            bApplied = true;
        },
        Result);

    // Save the root layer. force=false is idempotent — a layer that was not
    // actually mutated by the Set* calls will report saved=false but NOT an
    // error (pxr::'s Save() returns false on no-op).
    UE::FSdfLayer RootLayer = Stage.GetRootLayer();
    bool bSaved = false;
    UEAgent::UsdCommon::CaptureUsdErrors(
        [&RootLayer, &bSaved]() { bSaved = RootLayer.Save(/*bForce*/ false); },
        Result);

    Result->SetBoolField(TEXT("applied"), bApplied);
    Result->SetBoolField(TEXT("saved"),   bSaved);
    Result->SetBoolField(TEXT("dirty"),   RootLayer.IsDirty());
    Result->SetNumberField(TEXT("start_time"),        Stage.GetStartTimeCode());
    Result->SetNumberField(TEXT("end_time"),          Stage.GetEndTimeCode());
    Result->SetNumberField(TEXT("frames_per_second"), Stage.GetFramesPerSecond());
    return mcp_common::MakeSavedReply(Result, bSaved, TEXT("set_usd_stage_time_range"));
}

// ---------------------------------------------------------------------------
// save_usd_stage
// ---------------------------------------------------------------------------
// MCP:save_usd_stage - Save a USD stage's root layer to disk.  Wraps UE::FSdfLayer::Save(force) on the resolved stage's root layer. When force is false (default) and the root layer is clean, Save is a no-op and returns false — this is the USD-spec behavior and is reported honestly via 'saved:false' rather than as an error. Use force=true to re-write a clean layer (rare — typically only when a downstream tool requires a specific mtime).
//   @param stage_name [string!] - Name the stage was registered under, OR an absolute file path to auto-open.
//   @param force [bool] - Force-re-save even when the layer is clean (default false).
//   @returns { stage_name: string, resolution: string, saved: bool, dirty: bool, force: bool, real_path: string, errors: [string]? }

TSharedPtr<FJsonObject> HandleSaveUsdStage(const TSharedPtr<FJsonObject>& Params)
{
    FString StageName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("stage_name"), StageName)) { return Missing; }

    bool bForce = false;
    Params->TryGetBoolField(TEXT("force"), bForce);

    FString Resolution;
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("stage_name"), StageName);

    UE::FUsdStage Stage = UsdStage_Resolve(StageName, Result, Resolution);
    if (!Stage)
    {
        return UEAgent::UsdCommon::FailReply(Result, FString::Printf(TEXT("Stage '%s' could not be resolved"), *StageName));
    }

    Result->SetStringField(TEXT("resolution"), Resolution);
    Result->SetBoolField(TEXT("force"), bForce);

    UE::FSdfLayer RootLayer = Stage.GetRootLayer();
    Result->SetStringField(TEXT("real_path"), RootLayer.GetRealPath());

    bool bSaved = false;
    UEAgent::UsdCommon::CaptureUsdErrors(
        [&RootLayer, &bSaved, bForce]() { bSaved = RootLayer.Save(bForce); },
        Result);

    Result->SetBoolField(TEXT("saved"), bSaved);
    Result->SetBoolField(TEXT("dirty"), RootLayer.IsDirty());
    return mcp_common::MakeOutcomeReply(Result, bSaved,
        bSaved ? FString() : FString::Printf(TEXT("RootLayer.Save(force=%s) returned false for stage '%s'"), bForce ? TEXT("true") : TEXT("false"), *StageName));
}

// ---------------------------------------------------------------------------
// flatten_usd_stage
// ---------------------------------------------------------------------------
// MCP:flatten_usd_stage - Export a flattened copy of the stage to a .usd/.usda/.usdc/.usdz file (the file writer is chosen by the extension).  Wraps UE::FUsdStage::Export(OutputPath, bAddSourceFileComment, FileFormatArguments) — does NOT alter the in-memory stage. When add_source_file_comment is true (default) the flattened file includes a comment header noting the source file (useful for provenance). The returned 'exported' field reports pxr::'s bool return — false means the file writer could not write the requested path (e.g. invalid extension, permission denied). Realistic gotcha: stage.Export() requires the stage NOT be in a restricted-edit state — under headless cov this is not an issue, but in-editor it can be during PIE.
//   @param stage_name [string!] - Name the stage was registered under, OR an absolute file path to auto-open.
//   @param output_path [string!] - Absolute path of the flattened file to write. The file extension picks the writer (.usda=text, .usdc=binary, .usdz=archive).
//   @param add_source_file_comment [bool] - Include the source-file comment header in the flattened file (default true).
//   @returns { stage_name: string, resolution: string, output_path: string, exported: bool, add_source_file_comment: bool, errors: [string]? }

TSharedPtr<FJsonObject> HandleFlattenUsdStage(const TSharedPtr<FJsonObject>& Params)
{
    FString StageName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("stage_name"), StageName)) { return Missing; }

    FString OutputPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("output_path"), OutputPath)) { return Missing; }

    bool bAddSourceFileComment = true;
    Params->TryGetBoolField(TEXT("add_source_file_comment"), bAddSourceFileComment);

    FString Resolution;
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("stage_name"), StageName);
    Result->SetStringField(TEXT("output_path"), OutputPath);

    UE::FUsdStage Stage = UsdStage_Resolve(StageName, Result, Resolution);
    if (!Stage)
    {
        return UEAgent::UsdCommon::FailReply(Result, FString::Printf(TEXT("Stage '%s' could not be resolved"), *StageName));
    }

    Result->SetStringField(TEXT("resolution"), Resolution);
    Result->SetBoolField(TEXT("add_source_file_comment"), bAddSourceFileComment);

    bool bExported = false;
    UEAgent::UsdCommon::CaptureUsdErrors(
        [&Stage, &OutputPath, &bExported, bAddSourceFileComment]()
        {
            bExported = Stage.Export(*OutputPath, bAddSourceFileComment);
        },
        Result);

    Result->SetBoolField(TEXT("exported"), bExported);
    return mcp_common::MakeOkReply(Result);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("close_usd_stage"), &HandleCloseUsdStage },
    { TEXT("create_usd_stage"), &HandleCreateUsdStage },
    { TEXT("flatten_usd_stage"), &HandleFlattenUsdStage },
    { TEXT("get_usd_stage_info"), &HandleGetUsdStageInfo },
    { TEXT("list_usd_file_formats"), &HandleListUsdFileFormats },
    { TEXT("list_usd_stages"), &HandleListUsdStages },
    { TEXT("open_usd_stage"), &HandleOpenUsdStage },
    { TEXT("save_usd_stage"), &HandleSaveUsdStage },
    { TEXT("set_usd_stage_time_range"), &HandleSetUsdStageTimeRange },
};
} // namespace UEAgentUsdStageCommands
