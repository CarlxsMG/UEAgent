// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/GameFeatures/UEAgentGameFeaturesBackend.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Interfaces/IPluginManager.h"
#include "Modules/ModuleManager.h"

namespace UEAgentGameFeaturesCommandsImpl
{
    IUEAgentGameFeaturesBackend* GBackend = nullptr;

    TSharedPtr<FJsonObject> GameFeaturesOff()
    {
        return mcp_common::MakeErrorReply(TEXT("Game Features is not enabled in this project: enable the Game Features plugin "
            "(Edit > Plugins), restart the editor, and call again."));
    }
}

IUEAgentGameFeaturesBackend* IUEAgentGameFeaturesBackend::Get()
{
    using namespace UEAgentGameFeaturesCommandsImpl;
    if (!GBackend && IPluginManager::Get().FindEnabledPlugin(TEXT("GameFeatures")).IsValid())
    {
        // Its StartupModule registers the backend through Set().
        FModuleManager::Get().LoadModule(TEXT("UEAgentGameFeatures"));
    }
    return GBackend;
}

void IUEAgentGameFeaturesBackend::Set(IUEAgentGameFeaturesBackend* Backend)
{
    UEAgentGameFeaturesCommandsImpl::GBackend = Backend;
}

/**
 * Game Features / Modular Gameplay commands: activate, deactivate, unload, query state, and list Game
 * Feature Plugins at runtime.
 *
 * Why dedicated commands: UGameFeaturesSubsystem's whole lifecycle surface is plain C++ (GAMEFEATURES_API),
 * with ZERO UFUNCTION -- so call_function cannot reach any of it even though get_engine_subsystem resolves
 * the instance. The mutators are also async-by-completion-delegate (unreachable via generic reflection),
 * and EGameFeaturePluginState is a non-UENUM private enum.
 *
 * The engine calls live in the optional UEAgentGameFeatures module (IUEAgentGameFeaturesBackend), loaded
 * only when the project enables the Game Features plugin.
 */
namespace UEAgentGameFeaturesCommands
{

// MCP-DOMAIN: GameFeatures

// MCP:activate_game_feature_plugin - Load and activate a Game Feature Plugin by name or URL. ASYNC + honest-headless: the state transition runs on the subsystem's own ticker, so this fires the activation (logging the eventual result) and returns immediately with the current state -- poll get_game_feature_plugin_state until state == "Active". Idempotent: if already active, no-ops with already_active=true.
//   @param plugin_name - Name of the Game Feature Plugin (resolved to a URL via the subsystem registry). Provide this OR plugin_url.
//   @param plugin_url - The plugin URL directly (e.g. a file: protocol URL). Takes precedence over plugin_name.
//   @returns { url: string, already_active: bool, state: string, is_active: bool, is_loaded: bool, is_registered: bool, note: string }
TSharedPtr<FJsonObject> HandleActivateGameFeaturePlugin(const TSharedPtr<FJsonObject>& Params)
{
    IUEAgentGameFeaturesBackend* Backend = IUEAgentGameFeaturesBackend::Get();
    return Backend ? Backend->Activate(Params) : UEAgentGameFeaturesCommandsImpl::GameFeaturesOff();
}

// MCP:deactivate_game_feature_plugin - Deactivate an active Game Feature Plugin by name or URL. ASYNC + honest-headless (same fire-and-return pattern as activate): returns immediately with the current state; poll get_game_feature_plugin_state. Idempotent: if not active, no-ops with already_inactive=true.
//   @param plugin_name - Name of the Game Feature Plugin (resolved to a URL). Provide this OR plugin_url.
//   @param plugin_url - The plugin URL directly. Takes precedence over plugin_name.
//   @returns { url: string, already_inactive: bool, state: string, is_active: bool, is_loaded: bool, is_registered: bool, note: string }
TSharedPtr<FJsonObject> HandleDeactivateGameFeaturePlugin(const TSharedPtr<FJsonObject>& Params)
{
    IUEAgentGameFeaturesBackend* Backend = IUEAgentGameFeaturesBackend::Get();
    return Backend ? Backend->Deactivate(Params) : UEAgentGameFeaturesCommandsImpl::GameFeaturesOff();
}

// MCP:unload_game_feature_plugin - Unload a Game Feature Plugin by name or URL (complement of activate/deactivate). ASYNC + honest-headless (same fire-and-return pattern): returns immediately with the current state; poll get_game_feature_plugin_state. Idempotent: if not loaded, no-ops with already_unloaded=true. Note: an active plugin is still loaded, so this unloads it regardless of active state.
//   @param plugin_name - Name of the Game Feature Plugin (resolved to a URL). Provide this OR plugin_url.
//   @param plugin_url - The plugin URL directly. Takes precedence over plugin_name.
//   @param keep_registered [bool] - If true, the plugin stays registered after unloading (unwinds to the Registered state instead of fully unregistering). Optional, default false.
//   @returns { url: string, already_unloaded: bool, keep_registered: bool, state: string, is_active: bool, is_loaded: bool, is_registered: bool, note: string }
TSharedPtr<FJsonObject> HandleUnloadGameFeaturePlugin(const TSharedPtr<FJsonObject>& Params)
{
    IUEAgentGameFeaturesBackend* Backend = IUEAgentGameFeaturesBackend::Get();
    return Backend ? Backend->Unload(Params) : UEAgentGameFeaturesCommandsImpl::GameFeaturesOff();
}

// MCP:get_game_feature_plugin_state - Read the current lifecycle state of a Game Feature Plugin by name or URL (synchronous). The engine's EGameFeaturePluginState enum is non-reflective, so this is the canonical way to observe/poll a plugin's state after an async activate/deactivate.
//   @param plugin_name - Name of the Game Feature Plugin (resolved to a URL). Provide this OR plugin_url.
//   @param plugin_url - The plugin URL directly. Takes precedence over plugin_name. An unknown URL reports state "UnknownStatus".
//   @returns { url: string, state: string, is_active: bool, is_loaded: bool, is_registered: bool, is_installed: bool, is_in_error_state: bool }
TSharedPtr<FJsonObject> HandleGetGameFeaturePluginState(const TSharedPtr<FJsonObject>& Params)
{
    IUEAgentGameFeaturesBackend* Backend = IUEAgentGameFeaturesBackend::Get();
    return Backend ? Backend->GetState(Params) : UEAgentGameFeaturesCommandsImpl::GameFeaturesOff();
}

// MCP:list_game_feature_plugins - Enumerate every Game Feature Plugin the subsystem knows about (built-in + registered), with each one's current state. Read-only, synchronous.
//   @returns { plugins: [{ name: string, url: string, loaded_as_built_in: bool, state: string }], count: int }
TSharedPtr<FJsonObject> HandleListGameFeaturePlugins(const TSharedPtr<FJsonObject>& Params)
{
    IUEAgentGameFeaturesBackend* Backend = IUEAgentGameFeaturesBackend::Get();
    return Backend ? Backend->List(Params) : UEAgentGameFeaturesCommandsImpl::GameFeaturesOff();
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("activate_game_feature_plugin"), &HandleActivateGameFeaturePlugin },
    { TEXT("deactivate_game_feature_plugin"), &HandleDeactivateGameFeaturePlugin },
    { TEXT("get_game_feature_plugin_state"), &HandleGetGameFeaturePluginState },
    { TEXT("list_game_feature_plugins"), &HandleListGameFeaturePlugins },
    { TEXT("unload_game_feature_plugin"), &HandleUnloadGameFeaturePlugin },
};
} // namespace UEAgentGameFeaturesCommands
