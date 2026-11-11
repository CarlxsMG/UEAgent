// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

// flush_async_loading
#include "UObject/UObjectGlobals.h"

/**
 * Pipeline commands.
 *
 * All commands are headless-safe and perform real reads/effects:
 *   - flush_async_loading      : ::FlushAsyncLoading() + async-package counts.
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

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("flush_async_loading"), &HandleFlushAsyncLoading },
};
} // namespace UEAgentPipelineCommands
