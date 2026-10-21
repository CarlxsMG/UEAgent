// Copyright (c) 2026 CarlxsMG.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

/**
 * The plugin's editor module. It only sets up and tears down the editor UI (the status-bar
 * indicator and its menu): the MCP server lives in UUEAgentBridge, an editor subsystem the
 * engine starts and stops on its own.
 */
class FUEAgentModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
