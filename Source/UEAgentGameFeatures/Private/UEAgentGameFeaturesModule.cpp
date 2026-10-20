#include "Commands/GameFeatures/UEAgentGameFeaturesBackend.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Misc/EngineVersionComparison.h" // UE_VERSION_OLDER_THAN: the UE 5.7 paths
#include "GameFeaturesSubsystem.h"
#include "GameFeatureTypesFwd.h"
#if UE_VERSION_OLDER_THAN(5, 8, 0)
#include "GameFeatureTypes.h" // the EGameFeaturePluginState enumerators, for IsErrorPluginState
#endif
#include "GameFeaturePluginOperationResult.h"
#include "Engine/Engine.h"
#include "Modules/ModuleManager.h"

namespace
{
    // Defensive resolver for the engine subsystem. UGameFeaturesSubsystem::Get() blindly
    // dereferences GEngine->GetEngineSubsystem<>(); guard GEngine (rare, but honest) so a
    // very-early/headless call returns a clean error instead of crashing.
    UGameFeaturesSubsystem* GetGFS()
    {
        if (!GEngine)
        {
            return nullptr;
        }
        return GEngine->GetEngineSubsystem<UGameFeaturesSubsystem>();
    }

    TSharedPtr<FJsonObject> SubsystemUnavailable()
    {
        return mcp_common::MakeErrorReply(TEXT("GameFeaturesSubsystem unavailable (GEngine not ready)"));
    }

    // Resolves the { plugin_name | plugin_url } identifier to a plugin URL. plugin_url wins
    // if both are given. A bare plugin_name is resolved via the subsystem's own registry
    // (GetPluginURLByName) -- only known Game Feature Plugins resolve; anything else is an
    // honest "not found by name". Returns false + OutError on missing/unresolvable input.
    bool ResolvePluginURL(UGameFeaturesSubsystem& GFS, const TSharedPtr<FJsonObject>& Params, FString& OutURL, FString& OutError)
    {
        FString PluginURL;
        if (Params->TryGetStringField(TEXT("plugin_url"), PluginURL) && !PluginURL.IsEmpty())
        {
            OutURL = PluginURL;
            return true;
        }

        FString PluginName;
        if (Params->TryGetStringField(TEXT("plugin_name"), PluginName) && !PluginName.IsEmpty())
        {
            FString ResolvedURL;
            if (GFS.GetPluginURLByName(PluginName, ResolvedURL))
            {
                OutURL = ResolvedURL;
                return true;
            }
            OutError = FString::Printf(TEXT("Game feature plugin not found by name: %s"), *PluginName);
            return false;
        }

        OutError = TEXT("Missing required parameter: provide plugin_name or plugin_url");
        return false;
    }

    // Appends the canonical synchronous state fields for a plugin URL. The enum
    // EGameFeaturePluginState is a non-UENUM private enum, so it is serialized via the
    // engine's exported UE::GameFeatures::ToString -- never emitted raw.
    void AddStateFields(UGameFeaturesSubsystem& GFS, const FString& URL, const TSharedPtr<FJsonObject>& Obj)
    {
        Obj->SetStringField(TEXT("state"), UE::GameFeatures::ToString(GFS.GetPluginState(URL)));
        Obj->SetBoolField(TEXT("is_active"), GFS.IsGameFeaturePluginActive(URL));
        Obj->SetBoolField(TEXT("is_loaded"), GFS.IsGameFeaturePluginLoaded(URL));
        Obj->SetBoolField(TEXT("is_registered"), GFS.IsGameFeaturePluginRegistered(URL));
    }

#if UE_VERSION_OLDER_THAN(5, 8, 0)
    // UE 5.7 has no UGameFeaturesSubsystem::IsGameFeaturePluginInErrorState (added in 5.8), which answers whether
    // the plugin's state machine sits in a state of type Error. Those are exactly the eleven Error* states of
    // GAME_FEATURE_PLUGIN_STATE_LIST (each one an FErrorGameFeaturePluginState); a plugin without a state machine
    // reads UnknownStatus, not an error, as 5.8 answers false for it.
    bool IsErrorPluginState(EGameFeaturePluginState State)
    {
        switch (State)
        {
        case EGameFeaturePluginState::ErrorUninstalling:
        case EGameFeaturePluginState::ErrorCheckingStatus:
        case EGameFeaturePluginState::ErrorUnavailable:
        case EGameFeaturePluginState::ErrorManagingData:
        case EGameFeaturePluginState::ErrorMounting:
        case EGameFeaturePluginState::ErrorWaitingForDependencies:
        case EGameFeaturePluginState::ErrorRegistering:
        case EGameFeaturePluginState::ErrorAssetDependencyStreaming:
        case EGameFeaturePluginState::ErrorLoading:
        case EGameFeaturePluginState::ErrorActivatingDependencies:
        case EGameFeaturePluginState::ErrorDeactivatingDependencies:
            return true;
        default:
            return false;
        }
    }
#endif

    // Logs the eventual result of an async transition; the command has already replied.
    void LogResult(const TCHAR* Command, const FString& URL, const UE::GameFeatures::FResult& Result)
    {
        if (Result.HasError())
        {
            UE_LOG(LogTemp, Warning, TEXT("[MCP] %s('%s') failed: %s"), Command, *URL, *UE::GameFeatures::ToString(Result));
        }
        else
        {
            UE_LOG(LogTemp, Log, TEXT("[MCP] %s('%s') completed"), Command, *URL);
        }
    }

    class FGameFeaturesBackend final : public IUEAgentGameFeaturesBackend
    {
    public:
        // State transitions run on the subsystem's OWN FTSTicker -- the same tick loop the bridge drains
        // from -- so the mutators never block on their completion delegate (that would starve the ticker
        // and deadlock): they fire the async op, log its eventual result, and reply with the current
        // (transitioning) state for the client to poll.
        virtual TSharedPtr<FJsonObject> Activate(const TSharedPtr<FJsonObject>& Params) override
        {
            UGameFeaturesSubsystem* GFS = GetGFS();
            if (!GFS)
            {
                return SubsystemUnavailable();
            }
            FString URL, Error;
            if (!ResolvePluginURL(*GFS, Params, URL, Error))
            {
                return mcp_common::MakeErrorReply(Error);
            }

            TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
            ResultObj->SetStringField(TEXT("url"), URL);

            // Idempotency: engine-native (ChangeGameFeatureTargetState no-ops when already at target),
            // but report it explicitly and skip re-firing the state machine.
            if (GFS->IsGameFeaturePluginActive(URL))
            {
                ResultObj->SetBoolField(TEXT("already_active"), true);
                AddStateFields(*GFS, URL, ResultObj);
                ResultObj->SetStringField(TEXT("note"), TEXT("Plugin already active; no-op."));
                return mcp_common::MakeOkReply(ResultObj);
            }

            GFS->LoadAndActivateGameFeaturePlugin(URL, FGameFeaturePluginLoadComplete::CreateLambda(
                [URL](const UE::GameFeatures::FResult& Result) { LogResult(TEXT("activate_game_feature_plugin"), URL, Result); }));

            ResultObj->SetBoolField(TEXT("already_active"), false);
            AddStateFields(*GFS, URL, ResultObj);
            ResultObj->SetStringField(TEXT("note"), TEXT("Activation started (async, runs on the game-feature ticker). Poll get_game_feature_plugin_state until state == 'Active'."));
            return mcp_common::MakeOkReply(ResultObj);
        }

        virtual TSharedPtr<FJsonObject> Deactivate(const TSharedPtr<FJsonObject>& Params) override
        {
            UGameFeaturesSubsystem* GFS = GetGFS();
            if (!GFS)
            {
                return SubsystemUnavailable();
            }
            FString URL, Error;
            if (!ResolvePluginURL(*GFS, Params, URL, Error))
            {
                return mcp_common::MakeErrorReply(Error);
            }

            TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
            ResultObj->SetStringField(TEXT("url"), URL);

            if (!GFS->IsGameFeaturePluginActive(URL))
            {
                ResultObj->SetBoolField(TEXT("already_inactive"), true);
                AddStateFields(*GFS, URL, ResultObj);
                ResultObj->SetStringField(TEXT("note"), TEXT("Plugin not active; no-op."));
                return mcp_common::MakeOkReply(ResultObj);
            }

            GFS->DeactivateGameFeaturePlugin(URL, FGameFeaturePluginDeactivateComplete::CreateLambda(
                [URL](const UE::GameFeatures::FResult& Result) { LogResult(TEXT("deactivate_game_feature_plugin"), URL, Result); }));

            ResultObj->SetBoolField(TEXT("already_inactive"), false);
            AddStateFields(*GFS, URL, ResultObj);
            ResultObj->SetStringField(TEXT("note"), TEXT("Deactivation started (async). Poll get_game_feature_plugin_state until state is no longer 'Active'."));
            return mcp_common::MakeOkReply(ResultObj);
        }

        virtual TSharedPtr<FJsonObject> Unload(const TSharedPtr<FJsonObject>& Params) override
        {
            UGameFeaturesSubsystem* GFS = GetGFS();
            if (!GFS)
            {
                return SubsystemUnavailable();
            }
            FString URL, Error;
            if (!ResolvePluginURL(*GFS, Params, URL, Error))
            {
                return mcp_common::MakeErrorReply(Error);
            }

            bool bKeepRegistered = false;
            Params->TryGetBoolField(TEXT("keep_registered"), bKeepRegistered);

            TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
            ResultObj->SetStringField(TEXT("url"), URL);
            ResultObj->SetBoolField(TEXT("keep_registered"), bKeepRegistered);

            // Idempotency: gate on IsGameFeaturePluginLoaded (NOT IsActive -- an active plugin is
            // still loaded, so unload is meaningful there; only a not-loaded plugin is a true no-op).
            if (!GFS->IsGameFeaturePluginLoaded(URL))
            {
                ResultObj->SetBoolField(TEXT("already_unloaded"), true);
                AddStateFields(*GFS, URL, ResultObj);
                ResultObj->SetStringField(TEXT("note"), TEXT("Plugin not loaded; no-op."));
                return mcp_common::MakeOkReply(ResultObj);
            }

            GFS->UnloadGameFeaturePlugin(URL, FGameFeaturePluginUnloadComplete::CreateLambda(
                [URL](const UE::GameFeatures::FResult& Result) { LogResult(TEXT("unload_game_feature_plugin"), URL, Result); }),
                bKeepRegistered);

            ResultObj->SetBoolField(TEXT("already_unloaded"), false);
            AddStateFields(*GFS, URL, ResultObj);
            ResultObj->SetStringField(TEXT("note"), TEXT("Unload started (async). Poll get_game_feature_plugin_state until state is no longer 'Loaded'."));
            return mcp_common::MakeOkReply(ResultObj);
        }

        virtual TSharedPtr<FJsonObject> GetState(const TSharedPtr<FJsonObject>& Params) override
        {
            UGameFeaturesSubsystem* GFS = GetGFS();
            if (!GFS)
            {
                return SubsystemUnavailable();
            }
            FString URL, Error;
            if (!ResolvePluginURL(*GFS, Params, URL, Error))
            {
                return mcp_common::MakeErrorReply(Error);
            }

            TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
            ResultObj->SetStringField(TEXT("url"), URL);
            AddStateFields(*GFS, URL, ResultObj);
            ResultObj->SetBoolField(TEXT("is_installed"), GFS->IsGameFeaturePluginInstalled(URL));
#if UE_VERSION_OLDER_THAN(5, 8, 0)
            ResultObj->SetBoolField(TEXT("is_in_error_state"), IsErrorPluginState(GFS->GetPluginState(URL)));
#else
            ResultObj->SetBoolField(TEXT("is_in_error_state"), GFS->IsGameFeaturePluginInErrorState(URL));
#endif
            return mcp_common::MakeOkReply(ResultObj);
        }

        virtual TSharedPtr<FJsonObject> List(const TSharedPtr<FJsonObject>& Params) override
        {
            UGameFeaturesSubsystem* GFS = GetGFS();
            if (!GFS)
            {
                return SubsystemUnavailable();
            }

            TArray<TSharedPtr<FJsonValue>> PluginsArray;
            GFS->ForEachGameFeature([&PluginsArray](FGameFeatureInfo&& Info)
            {
                TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("name"), Info.Name);
                Entry->SetStringField(TEXT("url"), Info.URL);
                Entry->SetBoolField(TEXT("loaded_as_built_in"), Info.bLoadedAsBuiltIn);
                Entry->SetStringField(TEXT("state"), UE::GameFeatures::ToString(Info.CurrentState));
                PluginsArray.Add(MakeShared<FJsonValueObject>(Entry));
            });

            TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
            ResultObj->SetArrayField(TEXT("plugins"), PluginsArray);
            ResultObj->SetNumberField(TEXT("count"), PluginsArray.Num());
            return mcp_common::MakeOkReply(ResultObj);
        }
    };

    class FUEAgentGameFeaturesModule final : public IModuleInterface
    {
    public:
        virtual void StartupModule() override
        {
            IUEAgentGameFeaturesBackend::Set(&Backend);
        }

        virtual void ShutdownModule() override
        {
            IUEAgentGameFeaturesBackend::Set(nullptr);
        }

    private:
        FGameFeaturesBackend Backend;
    };
}

IMPLEMENT_MODULE(FUEAgentGameFeaturesModule, UEAgentGameFeatures)
