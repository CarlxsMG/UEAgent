// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

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
        if (const FHandler* Handler = Registry().Find(Command))
        {
            return (*Handler)(Params);
        }
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Unknown command: %s"), *Command));
    }
}
