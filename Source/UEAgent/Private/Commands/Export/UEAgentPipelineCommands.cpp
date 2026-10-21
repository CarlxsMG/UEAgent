// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

// flush_async_loading
#include "UObject/UObjectGlobals.h"

// get_source_control_status
#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"

/**
 * Pipeline / source-control commands.
 *
 * All commands are headless-safe and perform real reads/effects:
 *   - flush_async_loading      : ::FlushAsyncLoading() + async-package counts.
 *   - get_source_control_status: ISourceControlModule provider/enabled/available.
 */
namespace UEAgentPipelineCommands
{

// MCP-DOMAIN: Export

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

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("flush_async_loading"), &HandleFlushAsyncLoading },
    { TEXT("get_source_control_status"), &HandleGetSourceControlStatus },
};
} // namespace UEAgentPipelineCommands
