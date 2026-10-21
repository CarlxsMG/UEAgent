// Copyright (c) 2026 CarlxsMG.

#include "UEAgentModule.h"
#include "UEAgentBridge.h"
#include "Commands/Editor/UEAgentAutomationCommands.h"
#include "Commands/Gameplay/UEAgentGameplayDebuggerCommands.h"
#include "Mcp/McpEditorUI.h"
#include "Mcp/McpUpdate.h"
#include "Modules/ModuleManager.h"
#include "EditorSubsystem.h"
#include "Editor.h"

#define LOCTEXT_NAMESPACE "FUEAgentModule"

void FUEAgentModule::StartupModule()
{
	// The server is started by the subsystem (UUEAgentBridge); this sets up its editor UI and the log
	// capture that the commands read.
	McpUpdate::Startup();
	McpEditorUI::Startup();
	UEAgentAutomationCommands::StartLogCapture();
	UE_LOG(LogTemp, Display, TEXT("UEAgent Module has started"));
}

void FUEAgentModule::ShutdownModule()
{
	McpEditorUI::Shutdown();
	McpUpdate::Shutdown();
	UEAgentAutomationCommands::StopLogCapture();
	// Drop any GameplayDebugger categories registered on demand via register_gameplay_debugger_category.
	UEAgentGameplayDebuggerCommands::UnregisterAllCategories();
	UE_LOG(LogTemp, Display, TEXT("UEAgent Module has shut down"));
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FUEAgentModule, UEAgent) 