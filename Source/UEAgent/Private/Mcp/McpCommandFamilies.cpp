// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Mcp/McpToolCatalog.h"

namespace McpCommandFamilies
{
    namespace
    {
        // Filled while the module's static objects are constructed, so it must exist before the first
        // registration whatever the order of the .cpp files: a function-local static does.
        TMap<FString, FHandler>& Registry()
        {
            static TMap<FString, FHandler> Routes;
            return Routes;
        }
    }

    FRegistrar::FRegistrar(std::initializer_list<TPairInitializer<const FString&, const FHandler&>> Routes)
    {
        for (const TPairInitializer<const FString&, const FHandler&>& Route : Routes)
        {
            Registry().Add(Route.Key, Route.Value);
        }
    }

    TSharedPtr<FJsonObject> Run(const FString& Command, const TSharedPtr<FJsonObject>& Params)
    {
        // A renamed tool's old name runs the new one (the bridge has usually resolved it already).
        if (const FHandler* Handler = Registry().Find(FMcpToolCatalog::ResolveCallName(Command)))
        {
            return (*Handler)(Params);
        }
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Unknown command: %s"), *Command));
    }
}
