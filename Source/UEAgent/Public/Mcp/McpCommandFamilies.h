// Copyright (c) 2026 CarlxsMG.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * The command families. A family is a .cpp whose handlers are functions of its own namespace, and whose route
 * table -- each command string and the function that runs it -- is a FRegistrar constant at namespace scope:
 * the table adds itself to the registry when the module loads, and Run finds each command there. Adding a
 * command is a function and a row of its family's table; adding a family is a new .cpp with its table.
 */
namespace McpCommandFamilies
{
    /** Runs one command: reads its params, does the work and returns the reply. */
    using FHandler = TSharedPtr<FJsonObject> (*)(const TSharedPtr<FJsonObject>& Params);

    /** Adds a family's route table to the registry. */
    struct UEAGENT_API FRegistrar
    {
        FRegistrar(std::initializer_list<TPairInitializer<const FString&, const FHandler&>> Routes);
    };

    /**
     * Runs Command on the spot (no queue, none of the bridge's guards: UUEAgentBridge::DispatchSingleCommand
     * applies those). Commands compare case-insensitively, like FString ==; one that no family registers gets an
     * error reply.
     */
    UEAGENT_API TSharedPtr<FJsonObject> Run(const FString& Command, const TSharedPtr<FJsonObject>& Params);
}
