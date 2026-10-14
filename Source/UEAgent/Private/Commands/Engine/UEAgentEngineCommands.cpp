#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "GameFramework/PlayerController.h"
#include "UObject/UObjectGlobals.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Scalability.h"
#include "HAL/IConsoleManager.h"
#include "CoreGlobals.h"

/**
 * Engine-level commands: the live UEngine and the world contexts it owns, the GameInstance of a running
 * game session, the frame-rate cap and screen percentage, and the cheap editor liveness probe. All commands
 * are read-only.
 */
namespace UEAgentEngineCommands
{

// MCP-DOMAIN: Engine

namespace UEAgentEngineImpl
{
    // Label published as "world_type". Types without a dedicated label (None, GameRPC, ...)
    // are reported as "Unknown".
    const TCHAR* WorldTypeLabel(const EWorldType::Type Type)
    {
        switch (Type)
        {
            case EWorldType::Game:          return TEXT("Game");
            case EWorldType::Editor:        return TEXT("Editor");
            case EWorldType::PIE:           return TEXT("PIE");
            case EWorldType::EditorPreview: return TEXT("EditorPreview");
            case EWorldType::GamePreview:   return TEXT("GamePreview");
            case EWorldType::Inactive:      return TEXT("Inactive");
            default:                        break;
        }
        return TEXT("Unknown");
    }

    // Publishes "world_context_count" (every context, with or without a world) and "worlds"
    // (one entry per context that currently holds a world). The engine report also flags
    // which of those worlds has a GameInstance attached.
    void WriteWorldContexts(FJsonObject& Out, const UEngine& Source, const bool bFlagGameInstance)
    {
        const TIndirectArray<FWorldContext>& AllContexts = Source.GetWorldContexts();

        TArray<TSharedPtr<FJsonValue>> Entries;
        for (const FWorldContext& Slot : AllContexts)
        {
            const UWorld* SlotWorld = Slot.World();
            if (SlotWorld == nullptr)
            {
                continue;
            }

            TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("world_name"), SlotWorld->GetName());
            Entry->SetStringField(TEXT("world_type"), WorldTypeLabel(SlotWorld->WorldType));
            if (bFlagGameInstance)
            {
                Entry->SetBoolField(TEXT("has_game_instance"), SlotWorld->GetGameInstance() != nullptr);
            }
            Entries.Emplace(MakeShared<FJsonValueObject>(Entry));
        }

        Out.SetNumberField(TEXT("world_context_count"), AllContexts.Num());
        Out.SetArrayField(TEXT("worlds"), MoveTemp(Entries));
    }
}

UGameInstance* GetGameInstance()
{
    // In the editor a UGameInstance only exists while a PIE (Play In Editor)
    // session is running. At rest (just the editor world open) there is no
    // GameInstance, and that is a perfectly normal state -- not an error.
    if (GEditor)
    {
        if (FWorldContext* PIEContext = GEditor->GetPIEWorldContext())
        {
            if (PIEContext->OwningGameInstance)
            {
                return PIEContext->OwningGameInstance;
            }
            if (PIEContext->World())
            {
                return PIEContext->World()->GetGameInstance();
            }
        }
    }

    // Fallback: scan all world contexts for any that own a GameInstance
    // (covers -game / standalone style world contexts if present).
    if (GEngine)
    {
        for (const FWorldContext& Candidate : GEngine->GetWorldContexts())
        {
            if (Candidate.OwningGameInstance)
            {
                return Candidate.OwningGameInstance;
            }
            if (Candidate.World() && Candidate.World()->GetGameInstance())
            {
                return Candidate.World()->GetGameInstance();
            }
        }
    }

    return nullptr;
}

UEngine* GetEngine()
{
    // The engine singleton; null only very early or very late in the process lifetime.
    UEngine* const Singleton = GEngine;
    return Singleton;
}

// MCP:get_game_instance_info - Read the live UGameInstance (name, class, object path) plus every engine world context (world_name + world_type: Editor/PIE/Game/EditorPreview/Inactive). A UGameInstance only exists while PIE or a game world is running, so at rest exists=false with empty strings is the NORMAL answer, not an error -- use it to answer "is a game session up, and which GameInstance class did it pick?". Pick this over get_engine_info when you care about the GameInstance itself; get_engine_info answers the same world-context question without it.
//   @returns { exists: bool, game_instance_name: string, game_instance_class: string, game_instance_path: string, world_context_count: int, worlds: [object] }
TSharedPtr<FJsonObject> HandleGetGameInstanceInfo(const TSharedPtr<FJsonObject>& Params)
{
    // Without a running game there is no GameInstance. That is reported with exists=false and
    // empty strings, not as an error: the world-context half of the answer still holds.
    const UGameInstance* Session = GetGameInstance();

    FString SessionName;
    FString SessionClass;
    FString SessionPath;
    if (Session != nullptr)
    {
        SessionName = Session->GetName();
        SessionClass = Session->GetClass()->GetName();
        SessionPath = Session->GetPathName();
    }

    TSharedPtr<FJsonObject> Answer = MakeShared<FJsonObject>();
    Answer->SetBoolField(TEXT("exists"), Session != nullptr);
    Answer->SetStringField(TEXT("game_instance_name"), SessionName);
    Answer->SetStringField(TEXT("game_instance_class"), SessionClass);
    Answer->SetStringField(TEXT("game_instance_path"), SessionPath);

    // Without an engine the world fields are simply left out.
    if (GEngine != nullptr)
    {
        UEAgentEngineImpl::WriteWorldContexts(*Answer, *GEngine, /*bFlagGameInstance=*/false);
    }

    return mcp_common::MakeOkReply(Answer);
}

// MCP:get_engine_info - Read the GEngine object (engine_name, engine_class) and enumerate every world context with its world_name, world_type (Editor/PIE/Game/...) and has_game_instance flag. Use it to see how many worlds exist at once -- in co-op PIE the host and each client are SEPARATE world contexts, so this is how you confirm two PIE worlds are up before reading state that must be checked on both sides. Sibling: get_game_instance_info adds the GameInstance class; editor_status is the cheaper "is the editor alive" probe.
//   @returns { engine_name: string, engine_class: string, world_context_count: int, worlds: [object] }
TSharedPtr<FJsonObject> HandleGetEngineInfo(const TSharedPtr<FJsonObject>& Params)
{
    const UEngine* const Live = GetEngine();
    if (Live == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("Engine not available"));
    }

    TSharedPtr<FJsonObject> Answer = MakeShared<FJsonObject>();
    Answer->SetStringField(TEXT("engine_name"), Live->GetName());
    Answer->SetStringField(TEXT("engine_class"), Live->GetClass()->GetName());
    // Here each listed world also says whether a GameInstance owns it.
    UEAgentEngineImpl::WriteWorldContexts(*Answer, *Live, /*bFlagGameInstance=*/true);

    return mcp_common::MakeOkReply(Answer);
}

// MCP:editor_status - Prove the Unreal editor is alive and TICKING: a no-argument game-thread liveness/health probe, and the first call to make when the MCP bridge goes quiet. It runs on the GAME THREAD via the bridge's command drain, so a reply proves the game thread is pumping -- unlike a transport-only ping, which a wedged editor still answers. After a "Connection reset" use it to tell an editor CRASH (this errors, the reconnect refuses) from a mere tool TIMEOUT (this returns alive=true); call it twice and diff frame_counter to rule out a deadlock, since a frozen game thread replies once but never advances the frame. Also reports whether PIE is running (pie_active / is_simulating) and the open editor world, without starting or stopping anything. Read-only and cheap; safe to call any time.
//   @returns { alive: bool, gis_running: bool, exit_requested: bool, is_editor: bool, frame_counter: number, pie_active: bool, is_simulating: bool, editor_world: string, world_context_count: int }
TSharedPtr<FJsonObject> HandleEditorStatus(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Status = MakeShared<FJsonObject>();

    // Reaching this code AT ALL means the game-thread drain executed us: the editor is
    // alive and pumping. The globals below add finer-grained state.
    Status->SetBoolField(TEXT("alive"), true);
    Status->SetBoolField(TEXT("gis_running"), static_cast<bool>(GIsRunning));
    Status->SetBoolField(TEXT("exit_requested"), IsEngineExitRequested());
    Status->SetBoolField(TEXT("is_editor"), GIsEditor);

    // Monotonic engine frame counter -- call editor_status twice and diff this to prove the
    // game thread is actively TICKING (not merely that one drain fired). uint64 -> double is
    // lossless for any realistic session length.
    Status->SetNumberField(TEXT("frame_counter"), static_cast<double>(GFrameCounter));

    // PIE / Simulate state. PlayWorld is the canonical "PIE running" pointer -- mirrors the
    // bridge's own PIE-ownership guard (UEAgentBridge.cpp).
    const bool bPIEActive = (GEditor != nullptr) && (GEditor->PlayWorld != nullptr);
    Status->SetBoolField(TEXT("pie_active"), bPIEActive);
    Status->SetBoolField(TEXT("is_simulating"),
        (GEditor != nullptr) && GEditor->bIsSimulatingInEditor);

    // Editor world name (empty if none, e.g. headless/no GEditor).
    FString EditorWorldName;
    if (UWorld* EditorWorld = mcp_common::GetEditorWorld())
    {
        EditorWorldName = EditorWorld->GetName();
    }
    Status->SetStringField(TEXT("editor_world"), EditorWorldName);

    Status->SetNumberField(TEXT("world_context_count"),
        GEngine ? GEngine->GetWorldContexts().Num() : 0);

    return mcp_common::MakeOkReply(Status);
}

// MCP:get_engine_settings - Read exactly two live engine knobs: max_fps (UEngine::GetMaxFPS) and screen_percentage (Scalability resolution scale). NOT a general settings dump -- for project/plugin settings use get_project_settings_value or get_engine_ini_value, for scalability buckets get_scalability_levels, and for frame timing get_fps_info / frame_timing.
//   @returns { max_fps: number, screen_percentage: number }
TSharedPtr<FJsonObject> HandleGetEngineSettings(const TSharedPtr<FJsonObject>& Params)
{
    const UEngine* const Live = GetEngine();
    if (Live == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("Engine not available"));
    }

    // Only two knobs are published: the frame-rate cap and the scalability resolution scale.
    const float FrameRateCap = Live->GetMaxFPS();
    // r.ScreenPercentage within the scalability range, as Scalability::GetResolutionScreenPercentage reads it.
    static IConsoleVariable* ScreenPercentage = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ScreenPercentage"));
    const float ResolutionScale = FMath::Clamp(ScreenPercentage->GetFloat(), Scalability::MinResolutionScale, Scalability::MaxResolutionScale);

    TSharedPtr<FJsonObject> Knobs = MakeShared<FJsonObject>();
    Knobs->SetNumberField(TEXT("max_fps"), FrameRateCap);
    Knobs->SetNumberField(TEXT("screen_percentage"), ResolutionScale);
    return mcp_common::MakeOkReply(Knobs);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("editor_status"), &HandleEditorStatus },
    { TEXT("get_engine_info"), &HandleGetEngineInfo },
    { TEXT("get_engine_settings"), &HandleGetEngineSettings },
    { TEXT("get_game_instance_info"), &HandleGetGameInstanceInfo },
};
} // namespace UEAgentEngineCommands
