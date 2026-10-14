#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

// get_engine_stats
#include "Engine/Engine.h"
#include "HAL/PlatformMemory.h"
#include "UObject/UObjectArray.h"
#include "Misc/EngineVersion.h"

// flush_async_loading
#include "UObject/UObjectGlobals.h"

// get_source_control_status
#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"

// set_packaging_setting
#include "Misc/ConfigCacheIni.h"
#include "CoreGlobals.h"

// GAverageFPS / GAverageMS are exported ENGINE_API globals defined in
// Engine/Private/UnrealEngine.cpp but only declared via extern in private
// code. Re-declare the same exported symbols here so we link against them.
extern ENGINE_API float GAverageFPS;
extern ENGINE_API float GAverageMS;

/**
 * Pipeline / profiling / source-control commands.
 *
 * All commands are headless-safe and perform real reads/effects:
 *   - get_engine_stats         : average FPS/MS (GAverageFPS/GAverageMS), process
 *                                memory (FPlatformMemory::GetStats), live UObject
 *                                count (GUObjectArray) and the engine version.
 *   - flush_async_loading      : ::FlushAsyncLoading() + async-package counts.
 *   - get_source_control_status: ISourceControlModule provider/enabled/available.
 *   - set_packaging_setting    : write a /Script/UnrealEd.ProjectPackagingSettings
 *                                key to DefaultGame.ini via GConfig + read-back.
 */
namespace UEAgentPipelineCommands
{

// MCP-DOMAIN: Export

// ---------------------------------------------------------------------------
// get_engine_stats
// ---------------------------------------------------------------------------
// MCP:get_engine_stats - Report live engine profiling stats: average FPS/MS, process memory, the live UObject count and the engine version.
//   @returns { average_fps, average_ms, max_fps, used_physical_bytes, peak_used_physical_bytes, available_physical_bytes, total_physical_bytes, used_physical_mb, peak_used_physical_mb, available_physical_mb, object_count, engine_version, is_editor }
TSharedPtr<FJsonObject> HandleGetEngineStats(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

    // --- Frame timing (engine-wide rolling averages) ---
    const float AvgFPS = GAverageFPS;
    const float AvgMS = GAverageMS;
    Data->SetNumberField(TEXT("average_fps"), AvgFPS);
    Data->SetNumberField(TEXT("average_ms"), AvgMS);
    if (GEngine)
    {
        Data->SetNumberField(TEXT("max_fps"), GEngine->GetMaxFPS());
    }

    // --- Process memory (real platform stats) ---
    const FPlatformMemoryStats MemStats = FPlatformMemory::GetStats();
    TSharedPtr<FJsonObject> Memory = MakeShared<FJsonObject>();
    const double BytesPerMB = 1024.0 * 1024.0;
    Memory->SetNumberField(TEXT("used_physical_bytes"), (double)MemStats.UsedPhysical);
    Memory->SetNumberField(TEXT("peak_used_physical_bytes"), (double)MemStats.PeakUsedPhysical);
    Memory->SetNumberField(TEXT("available_physical_bytes"), (double)MemStats.AvailablePhysical);
    Memory->SetNumberField(TEXT("total_physical_bytes"), (double)MemStats.TotalPhysical);
    Memory->SetNumberField(TEXT("used_physical_mb"), (double)MemStats.UsedPhysical / BytesPerMB);
    Memory->SetNumberField(TEXT("peak_used_physical_mb"), (double)MemStats.PeakUsedPhysical / BytesPerMB);
    Memory->SetNumberField(TEXT("available_physical_mb"), (double)MemStats.AvailablePhysical / BytesPerMB);
    Data->SetObjectField(TEXT("memory"), Memory);

    // --- Live UObject count ---
    const int32 ObjectCount = GUObjectArray.GetObjectArrayNumMinusAvailable();
    Data->SetNumberField(TEXT("object_count"), ObjectCount);

    // --- Engine version ---
    Data->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
    Data->SetBoolField(TEXT("is_editor"), GIsEditor);

    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// flush_async_loading
// ---------------------------------------------------------------------------
// MCP:flush_async_loading - Block until all in-flight async package loads finish (::FlushAsyncLoading).
//   @returns { flushed, async_packages_before, async_packages_after, packages_flushed }
TSharedPtr<FJsonObject> HandleFlushAsyncLoading(const TSharedPtr<FJsonObject>& Params)
{
    const int32 PackagesBefore = GetNumAsyncPackages();

    // Block until all in-flight async package loads complete (real engine call).
    ::FlushAsyncLoading();

    const int32 PackagesAfter = GetNumAsyncPackages();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("flushed"), true);
    Data->SetNumberField(TEXT("async_packages_before"), PackagesBefore);
    Data->SetNumberField(TEXT("async_packages_after"), PackagesAfter);
    Data->SetNumberField(TEXT("packages_flushed"), FMath::Max(0, PackagesBefore - PackagesAfter));

    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_source_control_status
// ---------------------------------------------------------------------------
// MCP:get_source_control_status - Report the editor's revision-control state via ISourceControlModule.
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

namespace
{
    // Writes Value under Setting, flushes, and reads it back into Data: value_type, applied, read_back, verified.
    void Pipeline_WriteIniString(const TCHAR* Section, const FString& Setting, const FString& Value, const FString& Ini,
        const TCHAR* ValueType, const TSharedPtr<FJsonObject>& Data)
    {
        GConfig->SetString(Section, *Setting, *Value, Ini);
        GConfig->Flush(false, Ini);

        FString ReadBack;
        const bool bFound = GConfig->GetString(Section, *Setting, ReadBack, Ini);

        Data->SetStringField(TEXT("value_type"), ValueType);
        Data->SetStringField(TEXT("applied"), Value);
        Data->SetStringField(TEXT("read_back"), ReadBack);
        Data->SetBoolField(TEXT("verified"), bFound && (ReadBack == Value));
    }
}

// ---------------------------------------------------------------------------
// set_packaging_setting
// ---------------------------------------------------------------------------
// MCP:set_packaging_setting - Write a /Script/UnrealEd.ProjectPackagingSettings key into DefaultGame.ini (via GConfig) and read it back to verify
//   @param setting - the ini key, e.g. "bUsePakFile" (bool) or "BuildConfiguration"
//   @param value - a bool, string or number to persist.
//   @returns { setting, section, ini_file, value_type, applied, read_back, verified }
TSharedPtr<FJsonObject> HandleSetPackagingSetting(const TSharedPtr<FJsonObject>& Params)
{
    FString Setting;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("setting"), Setting)) { return Missing; }

    if (!GConfig)
    {
        return mcp_common::MakeErrorReply(TEXT("GConfig is not available"));
    }

    const TCHAR* Section = TEXT("/Script/UnrealEd.ProjectPackagingSettings");
    const FString& Ini = GGameIni; // DefaultGame.ini

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("setting"), Setting);
    Data->SetStringField(TEXT("section"), Section);
    Data->SetStringField(TEXT("ini_file"), Ini);

    // Boolean settings (e.g. bUsePakFile, bGenerateChunks, bCompressed, bShareMaterialShaderCode).
    bool BoolValue = false;
    FString StringValue;
    if (Params->TryGetBoolField(TEXT("value"), BoolValue))
    {
        GConfig->SetBool(Section, *Setting, BoolValue, Ini);
        GConfig->Flush(false, Ini);

        // Read it back from the cache to verify the write landed.
        bool ReadBack = !BoolValue;
        const bool bFound = GConfig->GetBool(Section, *Setting, ReadBack, Ini);

        Data->SetStringField(TEXT("value_type"), TEXT("bool"));
        Data->SetBoolField(TEXT("applied"), true);
        Data->SetBoolField(TEXT("read_back"), ReadBack);
        Data->SetBoolField(TEXT("verified"), bFound && (ReadBack == BoolValue));
        return mcp_common::MakeOkReply(Data);
    }
    else if (Params->TryGetStringField(TEXT("value"), StringValue))
    {
        Pipeline_WriteIniString(Section, Setting, StringValue, Ini, TEXT("string"), Data);
        return mcp_common::MakeOkReply(Data);
    }

    // Numeric value -> persist as its string form (ini keys are textual).
    double NumberValue = 0.0;
    if (Params->TryGetNumberField(TEXT("value"), NumberValue))
    {
        Pipeline_WriteIniString(Section, Setting, FString::SanitizeFloat(NumberValue), Ini, TEXT("number"), Data);
        return mcp_common::MakeOkReply(Data);
    }

    return mcp_common::MakeErrorReply(TEXT("Missing required parameter: value (bool, string, or number)"));
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("flush_async_loading"), &HandleFlushAsyncLoading },
    { TEXT("get_engine_stats"), &HandleGetEngineStats },
    { TEXT("get_source_control_status"), &HandleGetSourceControlStatus },
    { TEXT("set_packaging_setting"), &HandleSetPackagingSetting },
};
} // namespace UEAgentPipelineCommands
