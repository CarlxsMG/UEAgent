// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Common/McpEnumLabels.h"
#include "Misc/MonitoredProcess.h"
#include "Misc/CoreDelegates.h"
#include "HAL/PlatformProcess.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "GameFramework/Actor.h"
#include "EngineUtils.h"
#include "Engine/NetDriver.h"
#include "Net/UnrealNetwork.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"
#include "Engine/Engine.h"
#include "ScopedTransaction.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/DateTime.h"
#include "Misc/Timespan.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UnrealType.h"
#include "UObject/PropertyPortFlags.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Blueprint/BlueprintSupport.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Engine/Selection.h"
#include "HAL/PlatformFilemanager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "EditorAssetLibrary.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "UObject/FieldPath.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "LevelEditorViewport.h"
#include "ImageUtils.h"
#include "HighResScreenshot.h"
#include "Engine/GameViewportClient.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/PointLight.h"
#include "Engine/SpotLight.h"
#include "Camera/CameraActor.h"
#include "Components/StaticMeshComponent.h"
#include "EditorSubsystem.h"
#include "Subsystems/EditorActorSubsystem.h"

namespace UEAgentNetworkingCommands
{

// MCP-DOMAIN: Networking

// The single in-flight (or most recently finished) dedicated-server process. ~FMonitoredProcess() cancels and
// waits for a process still running, so the job is released before the engine exits: a server never outlives
// the editor.
struct FDedicatedServerJob
{
    TSharedPtr<FMonitoredProcess> Process;
    FString CommandLine;
    FDateTime StartTime;

    FCriticalSection StateLock;
    FString OutputTail;
    bool bFinished = false;
    bool bCanceled = false;
    int32 ExitCode = 0;
    FDateTime EndTime;
};
static TSharedPtr<FDedicatedServerJob> CurrentServerJob;
static const FDelegateHandle ReleaseServerJobAtExit = FCoreDelegates::OnPreExit.AddLambda([] { CurrentServerJob.Reset(); });

namespace UEAgentNetworkingImpl
{
    // ENetRole spelled the way the networking reports publish it.
    const TCHAR* NetRoleLabel(const ENetRole Role)
    {
        switch (Role)
        {
            case ROLE_Authority:       return TEXT("Authority");
            case ROLE_SimulatedProxy:  return TEXT("SimulatedProxy");
            case ROLE_AutonomousProxy: return TEXT("AutonomousProxy");
            case ROLE_None:            return TEXT("None");
            default:                   break;
        }
        return TEXT("Unknown");
    }

    // Inverse of NetRoleLabel for the four accepted names (FString equality ignores case).
    // Returns false for anything else.
    bool ParseNetRole(const FString& Text, ENetRole& OutRole)
    {
        const ENetRole Candidates[] = { ROLE_Authority, ROLE_SimulatedProxy, ROLE_AutonomousProxy, ROLE_None };
        for (const ENetRole Candidate : Candidates)
        {
            if (Text == NetRoleLabel(Candidate))
            {
                OutRole = Candidate;
                return true;
            }
        }
        return false;
    }

    // The editor-world actor whose object name or label is Name (first match): null once OutActor holds it,
    // otherwise the error reply.
    TSharedPtr<FJsonObject> FindEditorActorOrError(const FString& Name, AActor*& OutActor)
    {
        UWorld* const Level = mcp_common::GetEditorWorld();
        if (Level == nullptr)
        {
            return mcp_common::MakeErrorReply(TEXT("No world found"));
        }
        OutActor = mcp_common::FindActorByName(Level, Name);
        if (OutActor == nullptr)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor '%s' not found"), *Name));
        }
        return nullptr;
    }

    // One entry per valid PlayerState registered with the GameState.
    TArray<TSharedPtr<FJsonValue>> DescribePlayers(const AGameStateBase& State)
    {
        TArray<TSharedPtr<FJsonValue>> Entries;
        for (APlayerState* const Player : State.PlayerArray)
        {
            if (!IsValid(Player))
            {
                continue;
            }
            TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("player_name"), Player->GetPlayerName());
            Entry->SetNumberField(TEXT("player_id"), Player->GetPlayerId());
            Entry->SetBoolField(TEXT("is_valid"), IsValid(Player));
            Entry->SetStringField(TEXT("player_class"), Player->GetClass()->GetName());
            Entries.Add(MakeShared<FJsonValueObject>(Entry));
        }
        return Entries;
    }

    // Session-related keys of the [OnlineSubsystem] section of the engine ini. The two strings
    // appear only when present; the player cap defaults to 4 and the dedicated flag to false.
    TSharedPtr<FJsonObject> ReadOnlineSubsystemConfig()
    {
        const TCHAR* const Section = TEXT("OnlineSubsystem");
        TSharedPtr<FJsonObject> Config = MakeShared<FJsonObject>();

        FString Text;
        if (GConfig->GetString(Section, TEXT("DefaultPlatformService"), Text, GEngineIni))
        {
            Config->SetStringField(TEXT("default_platform_service"), Text);
        }
        if (GConfig->GetString(Section, TEXT("DefaultIdentityInterface"), Text, GEngineIni))
        {
            Config->SetStringField(TEXT("default_identity_interface"), Text);
        }

        int32 PlayerCap = 4;
        GConfig->GetInt(Section, TEXT("MaxPlayers"), PlayerCap, GEngineIni);
        Config->SetNumberField(TEXT("max_players"), PlayerCap);

        bool bDedicated = false;
        GConfig->GetBool(Section, TEXT("bUseDedicatedServer"), bDedicated, GEngineIni);
        Config->SetBoolField(TEXT("use_dedicated_server"), bDedicated);
        return Config;
    }

    // Every valid actor of World that replicates, with its local and remote net roles and its dormancy; only the
    // actors whose local role is *RoleFilter when RoleFilter is set.
    TArray<TSharedPtr<FJsonValue>> DescribeReplicatedActors(UWorld& World, const ENetRole* RoleFilter)
    {
        TArray<TSharedPtr<FJsonValue>> Entries;
        for (TActorIterator<AActor> Cursor(&World); Cursor; ++Cursor)
        {
            const AActor* const Actor = *Cursor;
            if (!IsValid(Actor) || !Actor->GetIsReplicated())
            {
                continue;
            }
            if (RoleFilter != nullptr && Actor->GetLocalRole() != *RoleFilter)
            {
                continue;
            }
            TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("name"), Actor->GetName());
            Entry->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
            Entry->SetStringField(TEXT("local_role"), NetRoleLabel(Actor->GetLocalRole()));
            Entry->SetStringField(TEXT("remote_role"), NetRoleLabel(Actor->GetRemoteRole()));
            Entry->SetBoolField(TEXT("is_replicated"), Actor->GetIsReplicated());
            Entry->SetStringField(TEXT("net_dormancy"), mcp_common::NetDormancyLabel(Actor->NetDormancy.GetValue()));
            Entries.Add(MakeShared<FJsonValueObject>(Entry));
        }
        return Entries;
    }

    // Caps the rolling output buffer kept for the dedicated-server job, same idiom as
    // Packaging's start_package_build (a long-running process shouldn't grow this unbounded).
    constexpr int32 DedicatedServerMaxOutputTailChars = 16384;

    void AppendServerOutputTail(FString& Tail, const FString& Line)
    {
        Tail += Line;
        Tail += TEXT("\n");
        if (Tail.Len() > DedicatedServerMaxOutputTailChars)
        {
            Tail = Tail.Right(DedicatedServerMaxOutputTailChars);
        }
    }
}

// Replication and net roles

// MCP:set_actor_replication - Configure the replication of an editor-world actor found by object name or label: movement (bReplicateMovement) and lifecycle (bReplicates: the actor is spawned and destroyed on clients). Only the settings sent are applied, inside one undoable transaction, and each is read back: a call that sends neither replicate_movement nor replicate_lifecycle changes nothing and is an error. Physics has no switch of its own in the engine -- a simulating root's state travels with movement replication -- so replicate_physics is reported in not_applied. Only that placed instance changes, not its class defaults; nothing is saved. Read every replication setting with get_actor_replication_info.
//   @retired set_properties - object_path=<actor>, properties={"bReplicates":true,"bReplicateMovement":true}
//   @renamed-from configure_replication
//   @param actor_name - Object name or label of the actor in the editor world (first match)
//   @param replicate_movement - Optional: whether to replicate movement (bReplicateMovement); applied only when sent
//   @param replicate_physics - Optional; NOT applied, see the description; listed in not_applied when sent
//   @param replicate_lifecycle - Optional: whether the actor replicates at all, i.e. exists on clients (bReplicates, set through AActor::SetReplicates); applied only when sent. Movement replication does nothing without it.
//   @returns { actor_name: string, replication_configured: bool, message: string, previous_settings: { replicate_movement: bool, replicate_lifecycle: bool }, new_settings: { replicate_movement?: bool, replicate_lifecycle?: bool }, actual_settings: { replicate_movement: bool, replicate_lifecycle: bool }, not_applied?: [string], actor_valid: bool, actor_class: string }

TSharedPtr<FJsonObject> HandleConfigureReplication(const TSharedPtr<FJsonObject>& Params)
{
    FString Target;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), Target, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    // Each setting is applied only when the call sends it, so a lifecycle-only call never touches movement.
    bool bMovement = false;
    const bool bMovementSent = Params->TryGetBoolField(TEXT("replicate_movement"), bMovement);
    bool bLifecycle = false;
    const bool bLifecycleSent = Params->TryGetBoolField(TEXT("replicate_lifecycle"), bLifecycle);
    bool bPhysics = false;
    const bool bPhysicsSent = Params->TryGetBoolField(TEXT("replicate_physics"), bPhysics);
    if (!bMovementSent && !bLifecycleSent)
    {
        return mcp_common::MakeErrorReply(TEXT("set_actor_replication: nothing to apply -- send replicate_movement and/or replicate_lifecycle (replicate_physics has no engine switch of its own)."));
    }

    AActor* Actor = nullptr;
    if (TSharedPtr<FJsonObject> NotFound = UEAgentNetworkingImpl::FindEditorActorOrError(Target, Actor)) { return NotFound; }

    auto Snapshot = [Actor]()
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetBoolField(TEXT("replicate_movement"), Actor->IsReplicatingMovement());
        Out->SetBoolField(TEXT("replicate_lifecycle"), Actor->GetIsReplicated());
        return Out;
    };
    const TSharedPtr<FJsonObject> Before = Snapshot();
    {
        const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "ConfigureReplication", "MCP: Configure Replication"));
        Actor->Modify();
        if (bLifecycleSent)
        {
            Actor->SetReplicates(bLifecycle);
        }
        if (bMovementSent)
        {
            Actor->SetReplicateMovement(bMovement);
        }
    }
    const TSharedPtr<FJsonObject> After = Snapshot();

    // Configured = every applied setting reads back as requested.
    const bool bConfigured = (!bMovementSent || Actor->IsReplicatingMovement() == bMovement)
        && (!bLifecycleSent || Actor->GetIsReplicated() == bLifecycle);

    TSharedPtr<FJsonObject> Requested = MakeShared<FJsonObject>();
    if (bMovementSent)
    {
        Requested->SetBoolField(TEXT("replicate_movement"), bMovement);
    }
    if (bLifecycleSent)
    {
        Requested->SetBoolField(TEXT("replicate_lifecycle"), bLifecycle);
    }

    TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
    Report->SetStringField(TEXT("actor_name"), Target);
    Report->SetBoolField(TEXT("replication_configured"), bConfigured);
    FString Message = bConfigured ? TEXT("Replication settings applied and read back.")
                                  : TEXT("Replication settings did not read back as requested.");
    if (Actor->IsReplicatingMovement() && !Actor->GetIsReplicated())
    {
        Message += TEXT(" Movement replication has no effect while the actor does not replicate (replicate_lifecycle false).");
    }
    Report->SetStringField(TEXT("message"), Message);
    Report->SetObjectField(TEXT("previous_settings"), Before);
    Report->SetObjectField(TEXT("new_settings"), Requested);
    Report->SetObjectField(TEXT("actual_settings"), After);
    if (bPhysicsSent)
    {
        TArray<TSharedPtr<FJsonValue>> NotApplied;
        NotApplied.Add(MakeShared<FJsonValueString>(TEXT("replicate_physics: no engine switch of its own; physics state replicates with movement")));
        Report->SetArrayField(TEXT("not_applied"), NotApplied);
    }

    Report->SetBoolField(TEXT("actor_valid"), IsValid(Actor));
    Report->SetStringField(TEXT("actor_class"), Actor->GetClass()->GetName());
    return mcp_common::MakeOutcomeReply(Report, bConfigured,
        bConfigured ? FString() : FString(TEXT("set_actor_replication: the actor does not read back the requested replication settings.")));
}

// MCP:set_network_authority - Set the local net role (AActor::SetRole) of an editor-world actor, found by object name rather than label, to Authority, SimulatedProxy, AutonomousProxy or None (case-insensitive; any other value is an error before anything changes). Only that placed instance's in-memory Role changes: RemoteRole and replication settings stay as they are, there is no undo entry and the level is not marked dirty. authority_set compares the role read back with the request. Read roles with get_actor_replication_info (one actor) or get_network_info (every replicated actor, filterable by role); choose what replicates with set_properties.
//   @param actor_name - Object name of the editor-world actor (case-insensitive); labels are not matched
//   @param authority - The network authority role to set; valid values: Authority, SimulatedProxy, AutonomousProxy, None
//   @returns { actor_name: string, authority_set: bool, previous_authority: string, requested_authority: string, actual_authority: string, actor_valid: bool, actor_class: string }

TSharedPtr<FJsonObject> HandleSetNetworkAuthority(const TSharedPtr<FJsonObject>& Params)
{
    FString Target;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), Target, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString RoleName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("authority"), RoleName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    // The role name is validated before anything in the world is touched.
    ENetRole Wanted = ROLE_None;
    if (!UEAgentNetworkingImpl::ParseNetRole(RoleName, Wanted))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Invalid authority value: %s. Valid values: Authority, SimulatedProxy, AutonomousProxy, None"), *RoleName));
    }

    UWorld* const Level = GEditor->GetEditorWorldContext().World();
    if (Level == nullptr)
    {
        return mcp_common::MakeErrorReply("No world found");
    }

    AActor* const Actor = mcp_common::FindActorByObjectName(Level, Target);
    if (Actor == nullptr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor '%s' not found"), *Target));
    }

    // Read, write, read again: authority_set compares the second read with the request.
    const ENetRole RoleBefore = Actor->GetLocalRole();
    Actor->SetRole(Wanted);
    const ENetRole RoleAfter = Actor->GetLocalRole();

    TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
    Report->SetStringField(TEXT("actor_name"), Target);
    Report->SetBoolField(TEXT("authority_set"), RoleAfter == Wanted);
    Report->SetStringField(TEXT("previous_authority"), UEAgentNetworkingImpl::NetRoleLabel(RoleBefore));
    Report->SetStringField(TEXT("requested_authority"), RoleName);
    Report->SetStringField(TEXT("actual_authority"), UEAgentNetworkingImpl::NetRoleLabel(RoleAfter));
    Report->SetBoolField(TEXT("actor_valid"), IsValid(Actor));
    Report->SetStringField(TEXT("actor_class"), Actor->GetClass()->GetName());
    return mcp_common::MakeOkReply(Report);
}

// MCP:get_network_info - Read a network snapshot of the editor world, never a PIE world: its net mode (with its code and the server/client flags); whether it has a net driver, and the driver's class, tick rates and client connection count; every replicating actor with its class, local and remote role and dormancy, optionally only those with one local role; when they exist, the GameState (class, player count, each player) and the first PlayerController; and the session keys of the [OnlineSubsystem] engine config. The editor world normally has no net driver, GameState or PlayerController, so net_driver_info comes back empty and those two blocks are left out. Nothing is written; without an editor world it is an error. One actor in detail: get_actor_replication_info; a whole-project Blueprint sweep: audit_replication.
//   @param role_filter - Optional local role to keep in replicated_actors: Authority, SimulatedProxy, AutonomousProxy or None (case-insensitive); omitted lists every replicating actor. Any other value is an error.
//   @returns { world_name: string, world_valid: bool, net_mode: string, net_mode_code: int, is_server: bool, is_client: bool, has_net_driver: bool, net_driver_info: { class?: string, max_net_tick_rate?: number, net_server_max_tick_rate?: number, connection_count?: number }, replicated_actors: [{ name: string, class: string, local_role: string, remote_role: string, is_replicated: bool, net_dormancy: string }], replicated_actor_count: number, role_filter?: string, has_game_state: bool, game_state_info?: { class: string, player_count: number, is_valid: bool, players: [{ player_name: string, player_id: number, is_valid: bool, player_class: string }] }, player_controller_info?: { class: string, is_valid: bool, is_local_player: bool }, config_settings: { default_platform_service?: string, default_identity_interface?: string, max_players: number, use_dedicated_server: bool } }

TSharedPtr<FJsonObject> HandleGetNetworkInfo(const TSharedPtr<FJsonObject>& Params)
{
    // The role filter is validated before the world is read.
    FString RoleFilter;
    Params->TryGetStringField(TEXT("role_filter"), RoleFilter);
    ENetRole FilterRole = ROLE_None;
    if (!RoleFilter.IsEmpty() && !UEAgentNetworkingImpl::ParseNetRole(RoleFilter, FilterRole))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Invalid role_filter: %s. Valid: Authority, SimulatedProxy, AutonomousProxy, None"), *RoleFilter));
    }

    // Reads the editor world; GEditor is taken as present.
    UWorld* const Level = GEditor->GetEditorWorldContext().World();
    if (Level == nullptr)
    {
        return mcp_common::MakeErrorReply("No world found");
    }

    TSharedPtr<FJsonObject> Info = MakeShared<FJsonObject>();
    Info->SetStringField(TEXT("world_name"), Level->GetName());
    Info->SetBoolField(TEXT("world_valid"), IsValid(Level));
    const ENetMode Mode = Level->GetNetMode();
    Info->SetStringField(TEXT("net_mode"), mcp_common::NetModeLabel(Mode));
    Info->SetNumberField(TEXT("net_mode_code"), static_cast<int32>(Mode));
    Info->SetBoolField(TEXT("is_server"), Mode == NM_DedicatedServer || Mode == NM_ListenServer);
    Info->SetBoolField(TEXT("is_client"), Mode == NM_Client);

    // Without a net driver the block is still there, just empty.
    TSharedPtr<FJsonObject> DriverInfo = MakeShared<FJsonObject>();
    const UNetDriver* const Driver = Level->GetNetDriver();
    Info->SetBoolField(TEXT("has_net_driver"), Driver != nullptr);
    if (Driver != nullptr)
    {
        DriverInfo->SetStringField(TEXT("class"), Driver->GetClass()->GetName());
        DriverInfo->SetNumberField(TEXT("max_net_tick_rate"), Driver->MaxNetTickRate);
        DriverInfo->SetNumberField(TEXT("net_server_max_tick_rate"), Driver->GetNetServerMaxTickRate());
        DriverInfo->SetNumberField(TEXT("connection_count"), Driver->ClientConnections.Num());
    }
    Info->SetObjectField(TEXT("net_driver_info"), DriverInfo);

    const TArray<TSharedPtr<FJsonValue>> Replicated = UEAgentNetworkingImpl::DescribeReplicatedActors(
        *Level, RoleFilter.IsEmpty() ? nullptr : &FilterRole);
    Info->SetNumberField(TEXT("replicated_actor_count"), Replicated.Num());
    Info->SetArrayField(TEXT("replicated_actors"), Replicated);
    if (!RoleFilter.IsEmpty())
    {
        Info->SetStringField(TEXT("role_filter"), UEAgentNetworkingImpl::NetRoleLabel(FilterRole));
    }

    // The GameState and first PlayerController blocks are omitted when those do not exist.
    const AGameStateBase* const State = Level->GetGameState();
    Info->SetBoolField(TEXT("has_game_state"), State != nullptr);
    if (State != nullptr)
    {
        TSharedPtr<FJsonObject> StateInfo = MakeShared<FJsonObject>();
        StateInfo->SetStringField(TEXT("class"), State->GetClass()->GetName());
        StateInfo->SetNumberField(TEXT("player_count"), State->PlayerArray.Num());
        StateInfo->SetBoolField(TEXT("is_valid"), IsValid(State));
        StateInfo->SetArrayField(TEXT("players"), UEAgentNetworkingImpl::DescribePlayers(*State));
        Info->SetObjectField(TEXT("game_state_info"), StateInfo);
    }
    if (const APlayerController* const Controller = Level->GetFirstPlayerController())
    {
        TSharedPtr<FJsonObject> ControllerInfo = MakeShared<FJsonObject>();
        ControllerInfo->SetStringField(TEXT("class"), Controller->GetClass()->GetName());
        ControllerInfo->SetBoolField(TEXT("is_valid"), IsValid(Controller));
        ControllerInfo->SetBoolField(TEXT("is_local_player"), Controller->IsLocalPlayerController());
        Info->SetObjectField(TEXT("player_controller_info"), ControllerInfo);
    }

    Info->SetObjectField(TEXT("config_settings"), UEAgentNetworkingImpl::ReadOnlineSubsystemConfig());
    return mcp_common::MakeOkReply(Info);
}

// Dedicated-server test launch. The async job runs on FMonitoredProcess, like Packaging's
// start/get/cancel_package_build, and not on FSerializedUATProcess: this is not a UAT invocation and must not
// be serialized behind a concurrent package build.

// MCP:launch_dedicated_server - Relaunches the CURRENT editor binary with "-server -log": a real, non-rendering, network-listening game instance (GIsServer=true/GIsClient=false/GIsEditor=false -- confirmed via UE 5.8 source: StaticDedicatedServerCheck/SetIsRunningAsDedicatedServer in LaunchEngineLoop.cpp, FApp::CanEverRender()==false whenever IsRunningDedicatedServer()). This is honestly NOT a Server.Target.cs shipping build (still the full editor binary, WITH_EDITOR=1) -- it closes only the TESTABLE half of the dedicated-server gap; real server packaging is out of scope. Async, like start_package_build: returns immediately once the process is launched, poll get_dedicated_server_status for output/exit; the server never exits on its own, so call stop_dedicated_server to terminate it. Refuses to launch a second server while one is already running (honest, not silently queued/clobbered).
//   @param extra_args - Optional raw extra command-line arguments appended verbatim after "-server -log" (e.g. "-port=8888").
//   @returns { server_started: bool, already_running: bool, command_line: string, note: string }
TSharedPtr<FJsonObject> HandleLaunchDedicatedServer(const TSharedPtr<FJsonObject>& Params)
{
    if (CurrentServerJob.IsValid())
    {
        FScopeLock Lock(&CurrentServerJob->StateLock);
        if (!CurrentServerJob->bFinished)
        {
            TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
            Data->SetBoolField(TEXT("server_started"), false);
            Data->SetBoolField(TEXT("already_running"), true);
            Data->SetStringField(TEXT("command_line"), CurrentServerJob->CommandLine);
            Data->SetStringField(TEXT("note"),
                TEXT("A dedicated server is already running; poll get_dedicated_server_status or stop_dedicated_server it first."));
            return mcp_common::MakeOkReply(Data);
        }
    }

    FString ExtraArgs;
    Params->TryGetStringField(TEXT("extra_args"), ExtraArgs);

    const FString ProjectPath = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
    const FString ExePath = FPlatformProcess::ExecutablePath();
    if (!FPaths::FileExists(ExePath))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Could not resolve the running editor executable path: %s"), *ExePath));
    }

    FString CommandLine = FString::Printf(TEXT("\"%s\" -server -log"), *ProjectPath);
    if (!ExtraArgs.IsEmpty())
    {
        CommandLine += TEXT(" ");
        CommandLine += ExtraArgs;
    }

    TSharedPtr<FDedicatedServerJob> Job = MakeShared<FDedicatedServerJob>();
    Job->CommandLine = CommandLine;
    Job->StartTime = FDateTime::UtcNow();
    Job->Process = MakeShared<FMonitoredProcess>(ExePath, CommandLine, /*InHidden=*/true);

    TWeakPtr<FDedicatedServerJob> WeakJob = Job;

    Job->Process->OnOutput().BindLambda([WeakJob](FString Output)
    {
        if (TSharedPtr<FDedicatedServerJob> PinnedJob = WeakJob.Pin())
        {
            FScopeLock Lock(&PinnedJob->StateLock);
            UEAgentNetworkingImpl::AppendServerOutputTail(PinnedJob->OutputTail, Output);
        }
    });
    Job->Process->OnCompleted().BindLambda([WeakJob](int32 ReturnCode)
    {
        if (TSharedPtr<FDedicatedServerJob> PinnedJob = WeakJob.Pin())
        {
            FScopeLock Lock(&PinnedJob->StateLock);
            PinnedJob->bFinished = true;
            PinnedJob->ExitCode = ReturnCode;
            PinnedJob->EndTime = FDateTime::UtcNow();
        }
    });
    Job->Process->OnCanceled().BindLambda([WeakJob]()
    {
        if (TSharedPtr<FDedicatedServerJob> PinnedJob = WeakJob.Pin())
        {
            FScopeLock Lock(&PinnedJob->StateLock);
            PinnedJob->bFinished = true;
            PinnedJob->bCanceled = true;
            PinnedJob->EndTime = FDateTime::UtcNow();
        }
    });

    // Unlike FSerializedUATProcess::Launch() (always returns true, defers failure to
    // OnLaunchFailed), the base FMonitoredProcess::Launch() returns synchronously: false means
    // FPlatformProcess::CreateProc failed outright (bad path, etc.) -- report that honestly.
    if (!Job->Process->Launch())
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Failed to launch dedicated server process: %s"), *CommandLine));
    }

    CurrentServerJob = Job;

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("server_started"), true);
    Data->SetBoolField(TEXT("already_running"), false);
    Data->SetStringField(TEXT("command_line"), CommandLine);
    Data->SetStringField(TEXT("note"),
        TEXT("Launched the current editor binary in dedicated-server mode (-server): a real, ")
        TEXT("non-rendering, network-listening game instance, NOT a Server.Target.cs shipping ")
        TEXT("build -- closes the testable half of the dedicated-server gap only. Poll ")
        TEXT("get_dedicated_server_status for output/exit; the server never exits on its own, ")
        TEXT("so call stop_dedicated_server to terminate it."));
    return mcp_common::MakeOkReply(Data);
}

// MCP:get_dedicated_server_status - Polls the state of the most recent launch_dedicated_server job: running / finished / canceled, with elapsed time and a tail of captured process output. Honest has_server:false if no server was ever launched this editor session.
//   @returns { has_server: bool, running?: bool, finished?: bool, canceled?: bool, exit_code?: int, command_line?: string, elapsed_seconds?: number, output_tail?: string }
TSharedPtr<FJsonObject> HandleGetDedicatedServerStatus(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    if (!CurrentServerJob.IsValid())
    {
        Data->SetBoolField(TEXT("has_server"), false);
        return mcp_common::MakeOkReply(Data);
    }

    FScopeLock Lock(&CurrentServerJob->StateLock);
    const FDateTime Now = FDateTime::UtcNow();
    const FDateTime EndPoint = CurrentServerJob->bFinished ? CurrentServerJob->EndTime : Now;
    const double ElapsedSeconds = (EndPoint - CurrentServerJob->StartTime).GetTotalSeconds();

    Data->SetBoolField(TEXT("has_server"), true);
    Data->SetBoolField(TEXT("running"), !CurrentServerJob->bFinished);
    Data->SetBoolField(TEXT("finished"), CurrentServerJob->bFinished);
    Data->SetBoolField(TEXT("canceled"), CurrentServerJob->bCanceled);
    Data->SetStringField(TEXT("command_line"), CurrentServerJob->CommandLine);
    Data->SetNumberField(TEXT("elapsed_seconds"), ElapsedSeconds);
    Data->SetStringField(TEXT("output_tail"), CurrentServerJob->OutputTail);
    if (CurrentServerJob->bFinished)
    {
        Data->SetNumberField(TEXT("exit_code"), CurrentServerJob->ExitCode);
    }
    return mcp_common::MakeOkReply(Data);
}

// MCP:stop_dedicated_server - Terminates the in-progress dedicated server, if one is running (honest no-op otherwise). FMonitoredProcess::Cancel(true) kills the whole process tree.
//   @returns { stopped: bool, was_running: bool }
TSharedPtr<FJsonObject> HandleStopDedicatedServer(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    if (!CurrentServerJob.IsValid())
    {
        Data->SetBoolField(TEXT("stopped"), false);
        Data->SetBoolField(TEXT("was_running"), false);
        return mcp_common::MakeOkReply(Data);
    }

    bool bWasRunning = false;
    {
        FScopeLock Lock(&CurrentServerJob->StateLock);
        bWasRunning = !CurrentServerJob->bFinished;
    }

    if (bWasRunning)
    {
        CurrentServerJob->Process->Cancel(true);
    }

    Data->SetBoolField(TEXT("stopped"), bWasRunning);
    Data->SetBoolField(TEXT("was_running"), bWasRunning);
    return mcp_common::MakeOkReply(Data);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("set_actor_replication"), &HandleConfigureReplication },
    { TEXT("get_dedicated_server_status"), &HandleGetDedicatedServerStatus },
    { TEXT("get_network_info"), &HandleGetNetworkInfo },
    { TEXT("launch_dedicated_server"), &HandleLaunchDedicatedServer },
    { TEXT("set_network_authority"), &HandleSetNetworkAuthority },
    { TEXT("stop_dedicated_server"), &HandleStopDedicatedServer },
};
} // namespace UEAgentNetworkingCommands
