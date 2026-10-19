#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Common/McpEnumLabels.h"
#include "Commands/Asset/AssetCommandHelpers.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "GameFramework/Actor.h"
#include "EngineUtils.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/NetDriver.h"
#include "Net/UnrealNetwork.h"
#include "RenderingThread.h"
#include "Stats/Stats.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformMisc.h"
#include "Misc/App.h"
#include "Engine/GameEngine.h"
#include "DynamicRHI.h"               // GDynamicRHI, RHIGetTextureMemoryStats (get_memory_usage)
#include "RHIStats.h"                // FTextureMemoryStats
#include "RenderCore.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Rendering/Texture2DResource.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UObjectIterator.h"
#include "UnrealClient.h"
#include "Engine/Canvas.h"
#include "Stats/StatsData.h"
#include "HAL/PlatformFilemanager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/FileManager.h"
#include "ProfilingDebugging/TraceAuxiliary.h"
#include "ProfilingDebugging/MiscTrace.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Logging/LogMacros.h"
#include "Windows/WindowsPlatformMisc.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include "Windows/MinWindows.h"
#include "Windows/HideWindowsPlatformTypes.h"
// RVT (Runtime Virtual Texture)
#include "VT/RuntimeVirtualTexture.h"
#include "VT/RuntimeVirtualTextureVolume.h"
#include "Components/RuntimeVirtualTextureComponent.h"
#include "RuntimeVirtualTextureSetBounds.h"  // RuntimeVirtualTexture::SetBounds (VirtualTexturingEditor)
#include "Editor.h"                            // GEditor->FindActorFactoryForActorClass
#include "ActorFactories/ActorFactory.h"      // UActorFactory (Place Actors spawn path)
#include "ScopedTransaction.h"                 // FScopedTransaction
#include "Landscape.h"
// assign_rvt_to_landscape has to reach the STREAMING PROXIES, not just the
// ALandscape -- in World Partition the proxies are what render (see the handler's
// comment block). ULandscapeInfo::ForEachLandscapeProxy + ALandscapeStreamingProxy
// are the enumeration route; both are LANDSCAPE_API / StaticClass()-reachable.
#include "LandscapeInfo.h"
#include "LandscapeProxy.h"
#include "LandscapeStreamingProxy.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/SavePackage.h"
#include "HAL/IConsoleManager.h"

namespace UEAgentPerformanceCommands
{

// MCP-DOMAIN: Performance

// Trace and region tracking, across calls: when the trace started (for the duration reported on stop) and
// the regions still open.
static double TraceStartTime = 0.0;
static TMap<FString, double> ActiveRegions;

// ---------------------------------------------------------------------------
// get_fps_info / get_memory_usage building blocks.
// ---------------------------------------------------------------------------
namespace UEAgentPerformanceImpl
{
    /** The world the level editor is showing (not a PIE world). May be null. */
    UWorld* CurrentEditorWorld()
    {
        return GEditor->GetEditorWorldContext().World();
    }

    /** Rate implied by the world's last tick delta. Zero while paused or before the first tick. */
    float FrameRateFromWorld(UWorld* World)
    {
        if (World == nullptr || !World->IsValidLowLevel())
        {
            return 0.0f;
        }
        const float TickDelta = World->GetDeltaSeconds();
        return TickDelta > 0.0f ? 1.0f / TickDelta : 0.0f;
    }

    /** The engine frame cap when one is configured; 60 otherwise. */
    float FrameRateGoal()
    {
        const float EngineCap = GEngine ? GEngine->GetMaxFPS() : 0.0f;
        return EngineCap > 0.0f ? EngineCap : 60.0f;
    }

    /**
     * Frame pacing as the engine has it: vsync from r.VSync; max_fps from the engine cap (0 = none);
     * min_fps and smooth_frame_rate from frame-rate smoothing, whose range's lower bound is the
     * only minimum the engine knows (0 when smoothing is off or the range is unbounded below).
     */
    TSharedPtr<FJsonObject> FramePacingReport()
    {
        TSharedPtr<FJsonObject> Pacing = MakeShared<FJsonObject>();
        const IConsoleVariable* VSync = IConsoleManager::Get().FindConsoleVariable(TEXT("r.VSync"));
        Pacing->SetBoolField(TEXT("vsync_enabled"), VSync != nullptr && VSync->GetInt() != 0);
        const bool bSmoothing = GEngine != nullptr && GEngine->bSmoothFrameRate;
        Pacing->SetBoolField(TEXT("smooth_frame_rate"), bSmoothing);
        Pacing->SetNumberField(TEXT("max_fps"), GEngine != nullptr ? GEngine->GetMaxFPS() : 0.0f);
        const bool bHasMinimum = bSmoothing && GEngine->SmoothedFrameRateRange.HasLowerBound();
        Pacing->SetNumberField(TEXT("min_fps"), bHasMinimum ? GEngine->SmoothedFrameRateRange.GetLowerBoundValue() : 0.0f);
        return Pacing;
    }

    /** Bytes expressed in whole megabytes (integer division, then widened). */
    double WholeMegabytes(const uint64 Bytes)
    {
        return static_cast<double>(Bytes / (1024 * 1024));
    }

    TSharedPtr<FJsonObject> ProcessMemoryReport()
    {
        const FPlatformMemoryStats Snapshot = FPlatformMemory::GetStats();

        TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
        Report->SetNumberField(TEXT("used_physical_memory_mb"), WholeMegabytes(Snapshot.UsedPhysical));
        Report->SetNumberField(TEXT("peak_used_physical_memory_mb"), WholeMegabytes(Snapshot.PeakUsedPhysical));
        Report->SetNumberField(TEXT("available_physical_memory_mb"), WholeMegabytes(Snapshot.AvailablePhysical));
        Report->SetNumberField(TEXT("total_physical_memory_mb"), WholeMegabytes(Snapshot.TotalPhysical));
        Report->SetNumberField(TEXT("used_virtual_memory_mb"), WholeMegabytes(Snapshot.UsedVirtual));
        Report->SetNumberField(TEXT("peak_used_virtual_memory_mb"), WholeMegabytes(Snapshot.PeakUsedVirtual));
        return Report;
    }

    /** Object counts for the scene_memory block. */
    struct FSceneTally
    {
        int32 Actors = 0;
        int32 Components = 0;
        int32 MeshesAssigned = 0;
        int32 MaterialSlotsFilled = 0;
        int32 UniqueMaterials = 0;
        int32 SceneTextures = 0;     // distinct textures the scene's materials use
        int32 ResidentTextures = 0;  // every UTexture2D in memory, process-wide
    };

    /**
     * Walks every actor of World. Meshes and materials are only counted on AStaticMeshActor
     * instances; textures are every live UTexture2D object in memory, not only the ones the
     * world uses.
     */
    FSceneTally TallyScene(UWorld* World)
    {
        FSceneTally Tally;
        TSet<UMaterialInterface*> Materials;
        for (TActorIterator<AActor> ActorCursor(World); ActorCursor; ++ActorCursor)
        {
            AActor* Actor = *ActorCursor;
            Tally.Actors += 1;

            TArray<UActorComponent*> OwnedComponents;
            Actor->GetComponents(OwnedComponents);
            Tally.Components += OwnedComponents.Num();

            AStaticMeshActor* MeshActor = Cast<AStaticMeshActor>(Actor);
            UStaticMeshComponent* MeshComponent = MeshActor ? MeshActor->GetStaticMeshComponent() : nullptr;
            if (MeshComponent == nullptr)
            {
                continue;
            }

            if (MeshComponent->GetStaticMesh() != nullptr)
            {
                Tally.MeshesAssigned += 1;
            }
            for (int32 SlotIndex = 0; SlotIndex < MeshComponent->GetNumMaterials(); ++SlotIndex)
            {
                if (UMaterialInterface* Material = MeshComponent->GetMaterial(SlotIndex))
                {
                    Tally.MaterialSlotsFilled += 1;
                    Materials.Add(Material);
                }
            }
        }

        // The scene's textures are the ones its materials use (every quality level and platform).
        TSet<UTexture*> Textures;
        for (const UMaterialInterface* Material : Materials)
        {
            TArray<UTexture*> Used;
            Material->GetUsedTextures(Used);
            Textures.Append(Used);
        }
        Textures.Remove(nullptr);
        Tally.UniqueMaterials = Materials.Num();
        Tally.SceneTextures = Textures.Num();

        for (TObjectIterator<UTexture2D> TextureCursor; TextureCursor; ++TextureCursor)
        {
            if (TextureCursor->IsValidLowLevel())
            {
                Tally.ResidentTextures += 1;
            }
        }
        return Tally;
    }

    TSharedPtr<FJsonObject> SceneMemoryReport(const FSceneTally& Tally)
    {
        TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
        Report->SetNumberField(TEXT("actor_count"), Tally.Actors);
        Report->SetNumberField(TEXT("component_count"), Tally.Components);
        Report->SetNumberField(TEXT("static_mesh_count"), Tally.MeshesAssigned);
        Report->SetNumberField(TEXT("material_count"), Tally.UniqueMaterials);
        Report->SetNumberField(TEXT("material_slot_count"), Tally.MaterialSlotsFilled);
        Report->SetNumberField(TEXT("texture_count"), Tally.SceneTextures);
        Report->SetNumberField(TEXT("resident_texture_count"), Tally.ResidentTextures);
        return Report;
    }

    // Read from the RHI. A null RHI (-NullRHI) or a platform that does not expose the adapter's
    // memory reports no hardware numbers: gpu_memory_available is then false and they are left out.
    TSharedPtr<FJsonObject> GpuMemoryReport()
    {
        TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
        FTextureMemoryStats Stats;
        if (GDynamicRHI != nullptr)
        {
            RHIGetTextureMemoryStats(Stats);
        }
        const bool bKnown = GDynamicRHI != nullptr && Stats.AreHardwareStatsValid();
        Report->SetBoolField(TEXT("gpu_memory_available"), bKnown);
        Report->SetStringField(TEXT("gpu_memory_status"), bKnown
            ? TEXT("Read from the RHI")
            : TEXT("The RHI reports no hardware memory numbers (null RHI, or not exposed on this platform)"));
        Report->SetStringField(TEXT("rhi_name"), GDynamicRHI != nullptr ? GDynamicRHI->GetName() : TEXT("None"));
        if (bKnown)
        {
            Report->SetNumberField(TEXT("dedicated_video_memory_mb"), WholeMegabytes(static_cast<uint64>(Stats.DedicatedVideoMemory)));
            Report->SetNumberField(TEXT("shared_system_memory_mb"), WholeMegabytes(static_cast<uint64>(FMath::Max<int64>(0, Stats.SharedSystemMemory))));
            Report->SetNumberField(TEXT("total_graphics_memory_mb"), WholeMegabytes(static_cast<uint64>(FMath::Max<int64>(0, Stats.TotalGraphicsMemory))));
        }
        Report->SetNumberField(TEXT("streaming_texture_memory_mb"), WholeMegabytes(Stats.StreamingMemorySize));
        Report->SetNumberField(TEXT("non_streaming_texture_memory_mb"), WholeMegabytes(Stats.NonStreamingMemorySize));
        return Report;
    }

    TSharedPtr<FJsonObject> HostSystemReport()
    {
        TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
        Report->SetStringField(TEXT("platform"), FPlatformProperties::PlatformName());
        Report->SetStringField(TEXT("cpu_brand"), FPlatformMisc::GetCPUBrand());
        Report->SetNumberField(TEXT("cpu_cores"), FPlatformMisc::NumberOfCores());
        Report->SetNumberField(TEXT("cpu_threads"), FPlatformMisc::NumberOfWorkerThreadsToSpawn());
        return Report;
    }
}

// MCP:get_fps_info - Retrieves current and target FPS along with performance settings and world network mode.
//   @returns { current_fps: float, target_fps: float, fps_variance: float, engine_version: string, is_editor: bool, is_dedicated_server: bool, performance_settings: {vsync_enabled (r.VSync), smooth_frame_rate, max_fps (0 = uncapped), min_fps (lower bound of frame-rate smoothing; 0 without it)}, world_name: string, net_mode: string }

TSharedPtr<FJsonObject> HandleGetFPSInfo(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentPerformanceImpl;

    UWorld* EditorWorld = Impl::CurrentEditorWorld();
    if (EditorWorld == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("No world found"));
    }

    // Both rates stay single precision so the reported difference is the float one.
    const float MeasuredRate = Impl::FrameRateFromWorld(EditorWorld);
    const float GoalRate = Impl::FrameRateGoal();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("current_fps"), MeasuredRate);
    Payload->SetNumberField(TEXT("target_fps"), GoalRate);
    Payload->SetNumberField(TEXT("fps_variance"), FMath::Abs(MeasuredRate - GoalRate));

    Payload->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
    Payload->SetBoolField(TEXT("is_editor"), GIsEditor);
    Payload->SetBoolField(TEXT("is_dedicated_server"),
        IsRunningDedicatedServer() || EditorWorld->GetNetMode() == NM_DedicatedServer);
    Payload->SetObjectField(TEXT("performance_settings"), Impl::FramePacingReport());

    Payload->SetStringField(TEXT("world_name"), EditorWorld->GetName());
    Payload->SetStringField(TEXT("net_mode"), mcp_common::NetModeLabel(EditorWorld->GetNetMode()));
    return mcp_common::MakeOkReply(Payload);
}

// MCP:get_memory_usage - Retrieves detailed memory usage including process, scene, GPU, and system information.
//   @returns { process_memory: {used_physical_memory_mb, peak_used_physical_memory_mb, available_physical_memory_mb, total_physical_memory_mb, used_virtual_memory_mb, peak_used_virtual_memory_mb}, scene_memory: {actor_count, component_count, static_mesh_count, material_count (distinct), material_slot_count, texture_count (distinct textures the scene's materials use), resident_texture_count (every UTexture2D in memory)}, gpu_memory: {gpu_memory_available, gpu_memory_status, rhi_name, dedicated_video_memory_mb?, shared_system_memory_mb?, total_graphics_memory_mb?, streaming_texture_memory_mb, non_streaming_texture_memory_mb}, system_info: {platform, cpu_brand, cpu_cores, cpu_threads} }

TSharedPtr<FJsonObject> HandleGetMemoryUsage(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentPerformanceImpl;

    UWorld* EditorWorld = Impl::CurrentEditorWorld();
    if (EditorWorld == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("No world found"));
    }

    // Sections are sampled in this order: process stats, scene walk, GPU stub, host info.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetObjectField(TEXT("process_memory"), Impl::ProcessMemoryReport());
    Payload->SetObjectField(TEXT("scene_memory"), Impl::SceneMemoryReport(Impl::TallyScene(EditorWorld)));
    Payload->SetObjectField(TEXT("gpu_memory"), Impl::GpuMemoryReport());
    Payload->SetObjectField(TEXT("system_info"), Impl::HostSystemReport());
    return mcp_common::MakeOkReply(Payload);
}

// =========================================================================
// Performance & Profiling (frame timing + Unreal Insights tracing)
//
// From the engine source:
//   - GGameThreadTime / GRenderThreadTime / GRHIThreadTime / G*WaitTime / GSwapBufferTime
//     live in Runtime/RenderCore/Public/RenderTimer.h (RENDERCORE_API, uint32 cycles).
//   - RHIGetGPUFrameCycles(uint32 GPUIndex=0) lives in RHI/Public/DynamicRHI.h
//     (RHI_API, uint32 cycles). GRHIGPUFrameTime / GGPUFrameTime are NOT public globals
//     — declaring extern your own would alias a private uint32 touched via
//     FPlatformAtomics::InterlockedExchange on volatile int32*, undefined behaviour.
//   - FTraceAuxiliary lives in Core/Public/ProfilingDebugging/TraceAuxiliary.h (CORE_API).
//     Start signature is (EConnectionType, const TCHAR* Target, const TCHAR* Channels,
//     FOptions*, LogCategory) — NOT (FStringView, FStringView). Idempotency note:
//     Start returns false if already connected (TraceAuxiliary.cpp).
//   - TRACE_BOOKMARK(Format, ...) macro at Core/Public/ProfilingDebugging/MiscTrace.h
//     requires Format to be a literal TEXT(...) array — static_assert enforces it. We
//     use TEXT("%s") as Format and pass *FString as the vararg so the literal constraint
//     is satisfied while the actual text is dynamic.
//   - Trace bookmarks only land in the .utrace if the 'bookmark' channel is enabled in
//     the Channels string passed to Start (we default to "cpu,frame,bookmark").
//
// Headless behaviour: with -nullrhi or in a commandlet, all G*ThreadTime globals and
// RHIGetGPUFrameCycles() return 0. We surface that via the `null_rhi` boolean in the
// payload so callers can distinguish "frame is 0 ms" from "no thread timing data".
// =========================================================================

// MCP:frame_timing - Read the current frame's Game/Render/GPU/RHI thread times plus a CPU-vs-GPU bottleneck verdict with an actionable hint.
//   @returns { frame_ms: float, game_thread_ms: float, render_thread_ms: float, rhi_thread_ms: float, game_thread_wait_ms: float, render_thread_wait_ms: float, gpu_ms: float, cpu_busy_ms: float, verdict: "GPU-bound"|"CPU-bound (game thread)"|"CPU-bound (render thread)"|"VSync/present bound"|"balanced"|"no-data", hint: string, null_rhi: bool, note: string } -- cpu_busy_ms is game + render + rhi; null_rhi is true with -nullrhi or in a commandlet, where the timings read 0 (verdict "no-data" when they all do) and note warns of it; otherwise note is empty.
//   @hints read-only
//   NOTE (impl): frame_ms is FApp::GetDeltaTime() * 1000, and gpu_ms is RHIGetGPUFrameCycles converted to ms.
TSharedPtr<FJsonObject> HandleFrameTiming(const TSharedPtr<FJsonObject>& Params)
{
    const bool bNullRHI = (GUsingNullRHI || IsRunningCommandlet());

    // Cycles -> ms via FPlatformTime::ToMilliseconds(uint32) — returns float.
    const float GameMs       = FPlatformTime::ToMilliseconds(GGameThreadTime);
    const float RenderMs     = FPlatformTime::ToMilliseconds(GRenderThreadTime);
    const float RHIMs        = FPlatformTime::ToMilliseconds(GRHIThreadTime);
    const float GameWaitMs   = FPlatformTime::ToMilliseconds(GGameThreadWaitTime);
    const float RenderWaitMs = FPlatformTime::ToMilliseconds(GRenderThreadWaitTime);
    const float GPUMs        = FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles());

    const float CpuBusyMs = GameMs + RenderMs + RHIMs;
    const float FrameMs   = static_cast<float>(FApp::GetDeltaTime() * 1000.0);

    // Bottleneck classification (honest headless verdict).
    constexpr float Epsilon = 0.5f;
    FString Verdict;
    FString Hint;
    if (bNullRHI && GPUMs < Epsilon && GameMs < Epsilon && RenderMs < Epsilon)
    {
        Verdict = TEXT("no-data");
        Hint    = TEXT("Editor is running with null RHI / commandlet -- thread timing globals are 0. "
                       "Run with a real RHI (or PIE / standalone) to read timing data.");
    }
    else if (GPUMs > CpuBusyMs + Epsilon)
    {
        Verdict = TEXT("GPU-bound");
        Hint    = TEXT("GPU is the bottleneck. Reduce draw calls, polygon count, shadow quality, "
                       "post-process passes, material complexity, or render scale.");
    }
    else if (GameMs > GPUMs + Epsilon && GameMs >= RenderMs)
    {
        Verdict = TEXT("CPU-bound (game thread)");
        Hint    = TEXT("Game thread is the bottleneck. Profile tick functions, blueprint logic, "
                       "AI, physics step, and BeginPlay/construction work. Use Insights 'cpu' channel.");
    }
    else if (RenderMs > GPUMs + Epsilon)
    {
        Verdict = TEXT("CPU-bound (render thread)");
        Hint    = TEXT("Render thread is the bottleneck. Too many draw calls, expensive materials, "
                       "or heavy scene updates on the render thread. Use Insights 'cpu' channel.");
    }
    else if (FrameMs > FMath::Max(CpuBusyMs, GPUMs) + Epsilon * 2.0f)
    {
        Verdict = TEXT("VSync/present bound");
        Hint    = TEXT("Frame time exceeds max(CPU, GPU) by a wide margin -- the present thread is "
                       "idle (VSync limit) or the target FPS cap is too low. Check vsync and max FPS.");
    }
    else
    {
        Verdict = TEXT("balanced");
        Hint    = TEXT("CPU and GPU loads are roughly balanced. Look for hitches via bookmark() "
                       "around suspect sections and re-inspect with the .utrace.");
    }

    TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetNumberField(TEXT("frame_ms"),               FrameMs);
    Response->SetNumberField(TEXT("game_thread_ms"),         GameMs);
    Response->SetNumberField(TEXT("render_thread_ms"),       RenderMs);
    Response->SetNumberField(TEXT("rhi_thread_ms"),          RHIMs);
    Response->SetNumberField(TEXT("game_thread_wait_ms"),    GameWaitMs);
    Response->SetNumberField(TEXT("render_thread_wait_ms"),  RenderWaitMs);
    Response->SetNumberField(TEXT("gpu_ms"),                 GPUMs);
    Response->SetNumberField(TEXT("cpu_busy_ms"),            CpuBusyMs);
    Response->SetStringField(TEXT("verdict"),                Verdict);
    Response->SetStringField(TEXT("hint"),                   Hint);
    Response->SetBoolField  (TEXT("null_rhi"),               bNullRHI);
    Response->SetStringField(TEXT("note"),                   bNullRHI
        ? TEXT("Running with null RHI / commandlet -- thread timing values will be 0.")
        : TEXT(""));

    return mcp_common::MakeOkReply(Response);
}

// MCP:start_trace - Start an Unreal Insights trace recording to a .utrace file. Idempotent — if a trace is already active, returns already_tracing:true with the current destination instead of failing.
//   @param channels - Comma-separated channel list (default "cpu,frame,bookmark"; also: gpu, region, memtag, counters, log, screenshot)
//   @param output_path - Absolute .utrace path. If empty, defaults to <ProjectDir>/Saved/Profiling/MCP_<timestamp>.utrace
//   @returns { tracing: bool, already_tracing: bool, destination: string, channels: string, note: string }
TSharedPtr<FJsonObject> HandleStartTrace(const TSharedPtr<FJsonObject>& Params)
{
    // Idempotency: if a trace is already running, surface that and don't try
    // to call Start again — FTraceAuxiliary::Start returns false silently if connected
    // (TraceAuxiliary.cpp), which would look like a real failure to the caller.
    if (FTraceAuxiliary::IsConnected())
    {
        const FTraceAuxiliary::ETraceSystemStatus Status = FTraceAuxiliary::GetTraceSystemStatus();
        const FString StatusStr = (Status == FTraceAuxiliary::ETraceSystemStatus::TracingToFile) ? TEXT("file")
                                    : (Status == FTraceAuxiliary::ETraceSystemStatus::TracingToServer) ? TEXT("server")
                                    : (Status == FTraceAuxiliary::ETraceSystemStatus::TracingToSecureNetwork) ? TEXT("secure_network")
                                    : (Status == FTraceAuxiliary::ETraceSystemStatus::TracingToCustomRelay) ? TEXT("custom_relay")
                                    : TEXT("other");

        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetBoolField  (TEXT("tracing"),         true);
        Resp->SetBoolField  (TEXT("already_tracing"), true);
        Resp->SetStringField(TEXT("destination"),     FTraceAuxiliary::GetTraceDestinationString());
        Resp->SetStringField(TEXT("connection_type"), StatusStr);
        Resp->SetNumberField(TEXT("elapsed_s"),       TraceStartTime > 0.0 ? (FPlatformTime::Seconds() - TraceStartTime) : 0.0);
        Resp->SetStringField(TEXT("note"),            TEXT("A trace is already active. Call stop_trace first to start a new one."));
        return mcp_common::MakeOkReply(Resp);
    }

    FString Channels = TEXT("cpu,frame,bookmark");
    Params->TryGetStringField(TEXT("channels"), Channels);

    FString OutputPath;
    if (!Params->TryGetStringField(TEXT("output_path"), OutputPath) || OutputPath.IsEmpty())
    {
        const FString Stamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
        OutputPath = FPaths::Combine(FPaths::ProfilingDir(), FString::Printf(TEXT("MCP_%s.utrace"), *Stamp));
    }

    // Pre-create the parent directory if it does not exist — FTraceAuxiliary::Start
    // doesn't mkdir for us and silently fails to a missing destination.
    IFileManager& FM = IFileManager::Get();
    const FString OutDir = FPaths::GetPath(OutputPath);
    if (!OutDir.IsEmpty() && !FM.DirectoryExists(*OutDir))
    {
        FM.MakeDirectory(*OutDir, /*Tree=*/true);
    }

    const bool bOk = FTraceAuxiliary::Start(
        FTraceAuxiliary::EConnectionType::File,
        *OutputPath,
        *Channels,
        /*Options=*/nullptr,
        /*LogCategory=*/LogCore
    );

    if (!bOk)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("FTraceAuxiliary::Start returned false (path='%s', channels='%s'). "
                                 "Check that UE_TRACE_SERVER_CONTROLS_ENABLED is compiled in (it is in editor "
                                 "by default, but disabled in Shipping builds)."),
                            *OutputPath, *Channels));
    }

    TraceStartTime = FPlatformTime::Seconds();

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField  (TEXT("tracing"),         true);
    Resp->SetBoolField  (TEXT("already_tracing"), false);
    Resp->SetStringField(TEXT("destination"),     OutputPath);
    Resp->SetStringField(TEXT("channels"),        Channels);
    Resp->SetNumberField(TEXT("started_at_s"),    TraceStartTime);
    Resp->SetStringField(TEXT("note"),            TEXT("Trace active. Use bookmark() and region_start/end() to annotate, then stop_trace to flush."));
    return mcp_common::MakeOkReply(Resp);
}

// MCP:stop_trace - Stop the active Unreal Insights trace and return its destination, duration and file size. Idempotent — calling stop_trace when no trace is active returns was_tracing:false with a note instead of failing.
//   @returns { stopped: bool, was_tracing: bool, destination?: string, duration_s?: float, size_bytes?: int, note: string } -- with no trace running only stopped:false, was_tracing:false and note; when the engine's Stop() refuses because the trace is already stopping, stopped is false and size_bytes is absent; after a stop, note says to open the .utrace in Unreal Insights. duration_s is 0 for a trace start_trace did not start.
TSharedPtr<FJsonObject> HandleStopTrace(const TSharedPtr<FJsonObject>& Params)
{
    if (!FTraceAuxiliary::IsConnected())
    {
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetBoolField  (TEXT("stopped"),     false);
        Resp->SetBoolField  (TEXT("was_tracing"), false);
        Resp->SetStringField(TEXT("note"),        TEXT("No active trace to stop."));
        return mcp_common::MakeOkReply(Resp);
    }

    const FString  Dest     = FTraceAuxiliary::GetTraceDestinationString();
    const double   ElapsedS = TraceStartTime > 0.0 ? (FPlatformTime::Seconds() - TraceStartTime) : 0.0;
    const bool     bOk      = FTraceAuxiliary::Stop();
    TraceStartTime = 0.0;

    if (!bOk)
    {
        // Idempotent in the wider sense — between Stop() and IsConnected() flipping
        // to false there's a brief window where a second Stop call returns false but
        // the trace IS being torn down. Don't fail the caller; report partial state
        // so the agent doesn't chase a phantom error. (Honest but not
        // gratuitously noisy.)
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetBoolField  (TEXT("stopped"),      false);
        Resp->SetBoolField  (TEXT("was_tracing"),  true);
        Resp->SetStringField(TEXT("destination"),  Dest);
        Resp->SetNumberField(TEXT("duration_s"),   ElapsedS);
        Resp->SetStringField(TEXT("note"),         TEXT("Stop returned false (trace was already stopping or torn down). "
                                                        "The .utrace file at the destination above is the final recording."));
        return mcp_common::MakeOkReply(Resp);
    }

    // File-based traces produce a .utrace file — read its size for the response.
    IFileManager& FM = IFileManager::Get();
    const int64    SizeBytes = Dest.IsEmpty() ? 0 : FM.FileSize(*Dest);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField  (TEXT("stopped"),     true);
    Resp->SetBoolField  (TEXT("was_tracing"), true);
    Resp->SetStringField(TEXT("destination"), Dest);
    Resp->SetNumberField(TEXT("duration_s"),  ElapsedS);
    Resp->SetNumberField(TEXT("size_bytes"),  SizeBytes);
    Resp->SetStringField(TEXT("note"),        TEXT("Open this .utrace in UnrealInsights for analysis. "
                                                   "Double-click the file or use Trace.Start <path> in Insights."));
    return mcp_common::MakeOkReply(Resp);
}

// MCP:get_trace_status - Report whether Unreal Insights tracing is compiled in, currently connected, and to what destination.
//   @returns { system_status: "not_available"|"available"|"tracing_to_file"|"tracing_to_server"|"tracing_to_secure_network"|"tracing_to_custom_relay"|"unknown", compile_time_enabled: bool, connected: bool, destination: string, elapsed_s: float } -- compile_time_enabled is false only when system_status is not_available (tracing compiled out, as in Shipping); destination is empty and elapsed_s 0 when not connected, and elapsed_s is also 0 for a trace start_trace did not start.
//   NOTE (impl): connected is FTraceAuxiliary::IsConnected().
TSharedPtr<FJsonObject> HandleGetTraceStatus(const TSharedPtr<FJsonObject>& Params)
{
    const FTraceAuxiliary::ETraceSystemStatus Status    = FTraceAuxiliary::GetTraceSystemStatus();
    const bool                               bConnected = FTraceAuxiliary::IsConnected();

    FString StatusStr;
    switch (Status)
    {
        case FTraceAuxiliary::ETraceSystemStatus::NotAvailable:           StatusStr = TEXT("not_available");           break;
        case FTraceAuxiliary::ETraceSystemStatus::Available:              StatusStr = TEXT("available");              break;
        case FTraceAuxiliary::ETraceSystemStatus::TracingToFile:          StatusStr = TEXT("tracing_to_file");        break;
        case FTraceAuxiliary::ETraceSystemStatus::TracingToServer:        StatusStr = TEXT("tracing_to_server");      break;
        case FTraceAuxiliary::ETraceSystemStatus::TracingToSecureNetwork: StatusStr = TEXT("tracing_to_secure_network"); break;
        case FTraceAuxiliary::ETraceSystemStatus::TracingToCustomRelay:   StatusStr = TEXT("tracing_to_custom_relay");   break;
        default:                                                          StatusStr = TEXT("unknown");                break;
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("system_status"),         StatusStr);
    Resp->SetBoolField  (TEXT("compile_time_enabled"),  Status != FTraceAuxiliary::ETraceSystemStatus::NotAvailable);
    Resp->SetBoolField  (TEXT("connected"),             bConnected);

    if (bConnected)
    {
        Resp->SetStringField(TEXT("destination"), FTraceAuxiliary::GetTraceDestinationString());
        Resp->SetNumberField(TEXT("elapsed_s"),   TraceStartTime > 0.0 ? (FPlatformTime::Seconds() - TraceStartTime) : 0.0);
    }
    else
    {
        Resp->SetStringField(TEXT("destination"), FString());
        Resp->SetNumberField(TEXT("elapsed_s"),   0.0);
    }

    return mcp_common::MakeOkReply(Resp);
}

// MCP:bookmark - Emit a TRACE_BOOKMARK into the active .utrace at the current frame. The bookmark channel must be enabled in the trace (default start_trace channels include it). The text is dynamic via printf-style formatting, but the Format token itself is a TEXT() literal as required by the macro's static_assert.
//   @param text - The bookmark text. Becomes the bookmark label in UnrealInsights.
//   @returns { bookmarked: bool, text: string, frame: int, note: string }
TSharedPtr<FJsonObject> HandleBookmark(const TSharedPtr<FJsonObject>& Params)
{
    FString Text;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("text"), Text)) { return Missing; }

    if (!FTraceAuxiliary::IsConnected())
    {
        // Honest error — a bookmark without a trace is a no-op, so reject
        // instead of silently swallowing the call. Caller can fall back to region_start/end
        // for in-process timing.
        return mcp_common::MakeErrorReply(
            TEXT("No active trace -- bookmarks require an active Insights recording. "
                 "Call start_trace first (or use region_start/end for in-process timing)."));
    }

    // TRACE_BOOKMARK enforces via static_assert that the Format parameter is a literal
    // const TCHAR[] array (MiscTrace.h). We use TEXT("%s") as the literal and
    // pass *Text as the vararg so the dynamic content is honoured without breaking
    // the assertion.
    TRACE_BOOKMARK(TEXT("%s"), *Text);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField  (TEXT("bookmarked"), true);
    Resp->SetStringField(TEXT("text"),       Text);
    Resp->SetNumberField(TEXT("frame"),      static_cast<double>(GFrameCounter));
    Resp->SetStringField(TEXT("note"),       TEXT("Bookmark landed in the active .utrace. Open in UnrealInsights to view."));
    return mcp_common::MakeOkReply(Resp);
}

// MCP:region_start - Begin tracking an in-process named region. Region start time is stored in this bridge instance and matched by region_end for duration calculation. Idempotent — calling region_start twice with the same name returns already_started:true.
//   @param name - Region identifier. Required.
//   @returns { started: bool, already_started: bool, name: string, started_at_s: float }
TSharedPtr<FJsonObject> HandleRegionStart(const TSharedPtr<FJsonObject>& Params)
{
    FString Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name)) { return Missing; }

    // Idempotency — if the region already exists, return its current start
    // time instead of overwriting. region_end will compute the duration against the
    // original start.
    if (const double* Existing = ActiveRegions.Find(Name))
    {
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetBoolField  (TEXT("started"),        false);
        Resp->SetBoolField  (TEXT("already_started"), true);
        Resp->SetStringField(TEXT("name"),            Name);
        Resp->SetNumberField(TEXT("started_at_s"),    *Existing);
        Resp->SetStringField(TEXT("note"),            TEXT("Region was already started; original start time preserved. Call region_end to close."));
        return mcp_common::MakeOkReply(Resp);
    }

    const double Now = FPlatformTime::Seconds();
    ActiveRegions.Add(Name, Now);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField  (TEXT("started"),         true);
    Resp->SetBoolField  (TEXT("already_started"), false);
    Resp->SetStringField(TEXT("name"),            Name);
    Resp->SetNumberField(TEXT("started_at_s"),    Now);
    return mcp_common::MakeOkReply(Resp);
}

// MCP:region_end - Close an open named region and return its duration in ms. Removes the region from the active set; subsequent region_end with the same name returns an error.
//   @param name - Region identifier. Must match an open region.
//   @returns { ended: bool, name: string, duration_ms: float, duration_s: float }
TSharedPtr<FJsonObject> HandleRegionEnd(const TSharedPtr<FJsonObject>& Params)
{
    FString Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name)) { return Missing; }

    const double* StartTime = ActiveRegions.Find(Name);
    if (!StartTime)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Region '%s' was not started. Call region_start first."), *Name));
    }

    const double DurationS = FPlatformTime::Seconds() - *StartTime;
    const float  DurationMs = static_cast<float>(DurationS * 1000.0);
    ActiveRegions.Remove(Name);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField  (TEXT("ended"),       true);
    Resp->SetStringField(TEXT("name"),        Name);
    Resp->SetNumberField(TEXT("duration_ms"), DurationMs);
    Resp->SetNumberField(TEXT("duration_s"),  DurationS);
    return mcp_common::MakeOkReply(Resp);
}

// MCP:list_traces - Enumerate .utrace files in <ProjectDir>/Saved/Profiling/, returning their path, size and modification time.
//   @returns { directory: string, count: int, traces: [{ path, name, size_bytes, modified_iso }] } -- directory is the resolved profiling folder; only its own files are listed, not subfolders.
TSharedPtr<FJsonObject> HandleListTraces(const TSharedPtr<FJsonObject>& Params)
{
    const FString    ProfilingDir = FPaths::ProfilingDir();
    IFileManager&    FM           = IFileManager::Get();
    TArray<FString>  Files;
    FM.FindFiles(Files, *ProfilingDir, TEXT("utrace"));

    TArray<TSharedPtr<FJsonValue>> TracesJson;
    TracesJson.Reserve(Files.Num());
    for (const FString& FileName : Files)
    {
        const FString  FullPath = FPaths::Combine(ProfilingDir, FileName);
        const int64    SizeBytes = FM.FileSize(*FullPath);
        const FDateTime ModTime   = FM.GetTimeStamp(*FullPath);

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("path"),         FullPath);
        Entry->SetStringField(TEXT("name"),         FileName);
        Entry->SetNumberField(TEXT("size_bytes"),   static_cast<double>(SizeBytes));
        Entry->SetStringField(TEXT("modified_iso"), ModTime.ToIso8601());
        TracesJson.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("directory"), ProfilingDir);
    Resp->SetNumberField(TEXT("count"),     Files.Num());
    Resp->SetArrayField (TEXT("traces"),    TracesJson);
    return mcp_common::MakeOkReply(Resp);
}

// ===========================================================================
// Runtime Virtual Texture
// All cmds operate on URuntimeVirtualTexture assets + ARuntimeVirtualTextureVolume.
// Headless-friendly (no Slate / no render).
// ===========================================================================

// MCP:create_rvt_asset - Create a URuntimeVirtualTexture asset at directory_path/asset_name with the given material type, tile count and tile size (both exponents, clamped silently), and save it; the save result is not checked. Returns asset_path, tile_count, tile_size and size in texels. Then use create_rvt_volume and assign_rvt_to_landscape.
//   @param asset_name - Name of the new RVT asset (e.g. "RVT_Landscape"), without a folder.
//   @param directory_path - Content folder as a long package path (e.g. "/Game/VirtualTextures"), not a relative one (default /Game).
//   @param material_type - One of "BaseColor_Normal_Specular", "BaseColor_Normal_Roughness" or "WorldHeight", case-insensitive (default "BaseColor_Normal_Specular"); any other value, BaseColor_YCoCg included, is refused.
//   @param tile_count - Exponent of the size in tiles: 2^n tiles, clamped to 0..12 (default 8 = 256 tiles); a texel value such as 256 is clamped to 12.
//   @param tile_size - Exponent of the tile size: 2^(n+6) texels, clamped to 0..4 (default 2 = 256 texels).
//   @param continuous_update - When true, already mapped pages are re-rendered round-robin (bContinuousUpdate); default false.
//   @returns { asset_path, material_type, tile_count, tile_size, size }
TSharedPtr<FJsonObject> HandleCreateRVTAsset(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetName, DirPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_name"), AssetName)) { return Missing; }
    Params->TryGetStringField(TEXT("directory_path"), DirPath);
    if (DirPath.IsEmpty()) { DirPath = TEXT("/Game"); }

    FString MatTypeStr = TEXT("BaseColor_Normal_Specular");
    Params->TryGetStringField(TEXT("material_type"), MatTypeStr);
    ERuntimeVirtualTextureMaterialType MatType = ERuntimeVirtualTextureMaterialType::BaseColor_Normal_Specular;
    if (MatTypeStr.Equals(TEXT("BaseColor_Normal_Roughness"), ESearchCase::IgnoreCase)) { MatType = ERuntimeVirtualTextureMaterialType::BaseColor_Normal_Roughness; }
    else if (MatTypeStr.Equals(TEXT("WorldHeight"), ESearchCase::IgnoreCase))            { MatType = ERuntimeVirtualTextureMaterialType::WorldHeight; }
    else if (!MatTypeStr.Equals(TEXT("BaseColor_Normal_Specular"), ESearchCase::IgnoreCase))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Unknown material_type '%s'"), *MatTypeStr));
    }

    int32 TileCountLog2 = 8;
    int32 TileSizeLog2 = 2;
    bool bContinuous = false;
    Params->TryGetNumberField(TEXT("tile_count"), TileCountLog2);
    Params->TryGetNumberField(TEXT("tile_size"), TileSizeLog2);
    Params->TryGetBoolField(TEXT("continuous_update"), bContinuous);

    FString CreateError;
    URuntimeVirtualTexture* RVT = mcp_common::NewAssetInPackage<URuntimeVirtualTexture>(DirPath / AssetName, AssetName,
        TEXT("NewObject<URuntimeVirtualTexture> returned null"), CreateError);
    if (!RVT)
    {
        return mcp_common::MakeErrorReply(CreateError);
    }
    UPackage* Package = RVT->GetOutermost();
    // TileCount/TileSize/MaterialType/bContinuousUpdate are PROTECTED. Use reflection to set them.
    if (FIntProperty* TileCountProp = FindFProperty<FIntProperty>(RVT->GetClass(), TEXT("TileCount")))
    {
        const int32 TileCountClamped = FMath::Clamp(TileCountLog2, 0, 12);
        TileCountProp->SetPropertyValue_InContainer(RVT, TileCountClamped);
    }
    if (FIntProperty* TileSizeProp = FindFProperty<FIntProperty>(RVT->GetClass(), TEXT("TileSize")))
    {
        const int32 TileSizeClamped = FMath::Clamp(TileSizeLog2, 0, 4);
        TileSizeProp->SetPropertyValue_InContainer(RVT, TileSizeClamped);
    }
    if (FByteProperty* MatTypeProp = FindFProperty<FByteProperty>(RVT->GetClass(), TEXT("MaterialType")))
    {
        const uint8 MatTypeByte = (uint8)MatType;
        MatTypeProp->SetPropertyValue_InContainer(RVT, MatTypeByte);
    }
    if (FBoolProperty* ContinuousProp = FindFProperty<FBoolProperty>(RVT->GetClass(), TEXT("bContinuousUpdate")))
    {
        ContinuousProp->SetPropertyValue_InContainer(RVT, (bool)bContinuous);
    }

    FAssetRegistryModule::AssetCreated(RVT);
    Package->MarkPackageDirty();
    const FString PackageFileName = FPackageName::LongPackageNameToFilename(
        Package->GetName(), FPackageName::GetAssetPackageExtension());
    FSavePackageArgs SaveArgs;
    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
    SaveArgs.SaveFlags = SAVE_NoError;
    SaveArgs.Error = GError;
    UPackage::SavePackage(Package, RVT, *PackageFileName, SaveArgs);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), RVT->GetPathName());
    Data->SetStringField(TEXT("material_type"), MatTypeStr);
    Data->SetNumberField(TEXT("tile_count"), RVT->GetTileCount());
    Data->SetNumberField(TEXT("tile_size"), RVT->GetTileSize());
    Data->SetNumberField(TEXT("size"), RVT->GetSize());
    return mcp_common::MakeOkReply(Data);
}

// MCP:get_rvt_info - Read a URuntimeVirtualTexture asset without changing it: material_type (full enum name, e.g. "ERuntimeVirtualTextureMaterialType::WorldHeight"), tile_count in tiles, tile_size and size in texels (not the exponents create_rvt_asset takes), and continuous_update.
//   @param asset_path - Object or package path of the RVT asset (e.g. "/Game/VirtualTextures/RVT_Landscape").
//   @returns { asset_path, material_type, tile_count, tile_size, size, continuous_update }
TSharedPtr<FJsonObject> HandleGetRVTInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_path"), AssetPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    URuntimeVirtualTexture* RVT = Cast<URuntimeVirtualTexture>(StaticLoadObject(URuntimeVirtualTexture::StaticClass(), nullptr, *AssetPath));
    if (!RVT)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("RVT asset not found: %s"), *AssetPath));
    }
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), AssetPath);
    Data->SetStringField(TEXT("material_type"), UEnum::GetValueAsString(RVT->GetMaterialType()));
    Data->SetNumberField(TEXT("tile_count"), RVT->GetTileCount());
    Data->SetNumberField(TEXT("tile_size"), RVT->GetTileSize());
    Data->SetNumberField(TEXT("size"), RVT->GetSize());
    Data->SetBoolField(TEXT("continuous_update"), RVT->GetContinuousUpdate());
    return mcp_common::MakeOkReply(Data);
}

// create_rvt_volume — the headless crash to avoid when spawning an
// ARuntimeVirtualTextureVolume in the non-PIE editor is the VT producer init
// dereferencing a null AllocatedVirtualTexture inside ENQUEUE_RENDER_COMMAND.
// It is a matter of order, not a fundamental block:
//   - The producer scene proxy is built ONLY when VirtualTexture != null at
//     registration: URuntimeVirtualTextureComponent::CreateRenderState_Concurrent
//     gates AddRuntimeVirtualTexture on `if (VirtualTexture != nullptr)`
//     (RuntimeVirtualTextureComponent.cpp). A spawn with a NULL VT never
//     creates the proxy -> the crash line is unreachable.
//   - The null-deref (FRuntimeVirtualTextureSceneProxy ctor,
//     RuntimeVirtualTextureSceneProxy.cpp) only fires if GetAllocatedVirtualTexture()
//     is null, which for a VALID RVT (Resource != null, layer count >= 1) never
//     happens: FVirtualTextureSystem::AllocateVirtualTexture always returns non-null
//     for a real producer (VirtualTextureSystem.cpp). It is NOT an async race
//     -- the alloc command is enqueued before the readback command (FIFO).
// So the headless-safe order is: spawn with VT null (via the Place-Actors factory,
// which also sets a non-degenerate default scale) -> SetVirtualTexture on a valid
// RVT -> the producer allocates cleanly. create_rvt_volume below follows that order, and
// set_rvt_volume_bounds refuses a volume whose RVT is not bound yet.
//
// Why the volume matters: assign_rvt_to_landscape only wires the landscape as a
// WRITER into its RuntimeVirtualTextures array. Without an ARuntimeVirtualTextureVolume
// (the URuntimeVirtualTextureComponent that registers the producer + defines the
// world-space region) nothing PRODUCES the RVT -- the chain is incomplete.

// MCP:create_rvt_volume - Spawn an ARuntimeVirtualTextureVolume (the RVT producer that registers the runtime virtual texture into the scene and defines its world-space region) bound to an RVT asset. Without a volume, assign_rvt_to_landscape only wires the landscape as a writer -- nothing produces the RVT. Optionally fits the volume bounds to an actor (usually the landscape) in the same call. Headless-safe: spawns with a null VirtualTexture (no render-state) then binds a valid RVT. Idempotent: returns the existing volume if one already targets this RVT.
//   @param rvt_asset_path - Required. /Game path to the URuntimeVirtualTexture asset (create_rvt_asset).
//   @param bounds_align_actor - Optional. Name/label of an actor (usually the ALandscape) to fit the volume bounds to; runs the engine "Set Bounds" after binding.
//   @param volume_name - Optional. Actor label for the spawned volume.
//   @param location - Optional [x,y,z] spawn location (default [0,0,0]; overridden by the bounds-align when bounds_align_actor is given).
//   @returns { volume_name, rvt_asset_path, bounds_aligned, bounds_align_actor?, already_existed }
TSharedPtr<FJsonObject> HandleCreateRVTVolume(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World)) { return NoWorld; }

    FString RVTPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("rvt_asset_path"), RVTPath)) { return Missing; }
    URuntimeVirtualTexture* RVT = Cast<URuntimeVirtualTexture>(
        mcp_common::ResolveObjectByPath(RVTPath, URuntimeVirtualTexture::StaticClass()));
    if (!RVT)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("RVT asset not found: %s"), *RVTPath));
    }

    // Idempotent: reuse an existing volume already bound to this RVT.
    for (TActorIterator<ARuntimeVirtualTextureVolume> It(World); It; ++It)
    {
        URuntimeVirtualTextureComponent* ExComp =
            *It ? Cast<URuntimeVirtualTextureComponent>(It->GetRootComponent()) : nullptr;
        if (ExComp && ExComp->GetVirtualTexture() == RVT)
        {
            TSharedPtr<FJsonObject> D = MakeShared<FJsonObject>();
            D->SetStringField(TEXT("volume_name"), It->GetActorLabel());
            D->SetStringField(TEXT("rvt_asset_path"), RVTPath);
            D->SetBoolField(TEXT("bounds_aligned"), false);
            D->SetBoolField(TEXT("already_existed"), true);
            return mcp_common::MakeOkReply(D);
        }
    }

    // Optional bounds-align actor (resolve before spawn so the payload is honest).
    FString AlignName;
    AActor* AlignActor = nullptr;
    if (Params->TryGetStringField(TEXT("bounds_align_actor"), AlignName) && !AlignName.IsEmpty())
    {
        AlignActor = mcp_common::FindActorByName(World, AlignName);
        if (!AlignActor)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("bounds_align_actor not found: %s"), *AlignName));
        }
    }

    // Spawn through the engine's own actor factory (the Place-Actors path) so the
    // volume gets the factory's non-degenerate default scale (100,100,1). VirtualTexture
    // stays NULL at spawn -> the producer render-state is NOT created (the crash path
    // is unreachable). We arm the producer with SetVirtualTexture only after the actor
    // exists, with a valid RVT.
    FActorSpawnParameters SpawnParams;
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    const FVector SpawnLoc = mcp_common::ReadJsonVector(Params, TEXT("location"));
    const FTransform SpawnTransform(FRotator::ZeroRotator, SpawnLoc);
    ARuntimeVirtualTextureVolume* Volume = nullptr;
    if (UActorFactory* Factory = GEditor->FindActorFactoryForActorClass(ARuntimeVirtualTextureVolume::StaticClass()))
    {
        Volume = Cast<ARuntimeVirtualTextureVolume>(Factory->CreateActor(
            ARuntimeVirtualTextureVolume::StaticClass(), World->GetCurrentLevel(), SpawnTransform, SpawnParams));
    }
    if (!Volume)
    {
        // Defensive fallback: raw spawn + the factory's non-degenerate default scale.
        Volume = World->SpawnActor<ARuntimeVirtualTextureVolume>(
            ARuntimeVirtualTextureVolume::StaticClass(), SpawnLoc, FRotator::ZeroRotator, SpawnParams);
        if (Volume)
        {
            Volume->SetActorScale3D(FVector(100.f, 100.f, 1.f));
        }
    }
    if (!Volume)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to spawn ARuntimeVirtualTextureVolume"));
    }
    URuntimeVirtualTextureComponent* Comp = Cast<URuntimeVirtualTextureComponent>(Volume->GetRootComponent());
    if (!Comp)
    {
        return mcp_common::MakeErrorReply(TEXT("Spawned volume has no URuntimeVirtualTextureComponent root"));
    }

    FString VolumeLabel;
    if (Params->TryGetStringField(TEXT("volume_name"), VolumeLabel) && !VolumeLabel.IsEmpty())
    {
        Volume->SetActorLabel(VolumeLabel);
    }

    // Order matters: BoundsAlignActor before SetBounds; VirtualTexture before SetBounds
    // (RuntimeVirtualTexture::SetBounds has check(VirtualTexture != nullptr)).
    bool bAligned = false;
    if (AlignActor)
    {
        Comp->SetBoundsAlignActor(AlignActor);
    }
    Comp->SetVirtualTexture(RVT);   // arms the producer (MarkRenderStateDirty inside)
    if (AlignActor)
    {
        RuntimeVirtualTexture::SetBounds(Comp);  // the "Set Bounds" op: Modify + SetActorTransform + PostEditMove
        bAligned = true;
    }
    Volume->MarkPackageDirty();

    TSharedPtr<FJsonObject> D = MakeShared<FJsonObject>();
    D->SetStringField(TEXT("volume_name"), Volume->GetActorLabel());
    D->SetStringField(TEXT("rvt_asset_path"), RVTPath);
    D->SetBoolField(TEXT("bounds_aligned"), bAligned);
    if (AlignActor)
    {
        D->SetStringField(TEXT("bounds_align_actor"), AlignName);
    }
    D->SetBoolField(TEXT("already_existed"), false);
    return mcp_common::MakeOkReply(D);
}

// MCP:set_rvt_volume_bounds - Fit an existing ARuntimeVirtualTextureVolume's bounds to an actor -- the engine "Set Bounds" button (RuntimeVirtualTexture::SetBounds). Sets the volume's BoundsAlignActor and recomputes its transform to cover that actor (a landscape uses its complete bounds) plus any primitives writing to the volume's RVT. The volume must already have its RVT bound (create_rvt_volume). Optional landscape texel-snap + bounds expansion.
//   @param volume_name - Required. Actor name/label of the ARuntimeVirtualTextureVolume.
//   @param bounds_align_actor - Required. Name/label of the actor (usually the ALandscape) to fit the bounds to.
//   @param snap_to_landscape - Optional bool. Snap virtual-texture texels to landscape vertices (only meaningful when the align actor is a landscape).
//   @param expand_bounds - Optional float >= 0. Amount to expand the computed bounds.
//   @returns { volume_name, bounds_align_actor, snap_to_landscape, expand_bounds, applied }
TSharedPtr<FJsonObject> HandleSetRVTVolumeBounds(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World)) { return NoWorld; }

    FString VolumeName, AlignName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("volume_name"), VolumeName)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("bounds_align_actor"), AlignName)) { return Missing; }

    ARuntimeVirtualTextureVolume* Volume = nullptr;
    for (TActorIterator<ARuntimeVirtualTextureVolume> It(World); It; ++It)
    {
        if (*It && (It->GetActorLabel() == VolumeName || It->GetName() == VolumeName))
        {
            Volume = *It; break;
        }
    }
    if (!Volume)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("RVT volume not found: %s"), *VolumeName));
    }
    URuntimeVirtualTextureComponent* Comp = Cast<URuntimeVirtualTextureComponent>(Volume->GetRootComponent());
    if (!Comp)
    {
        return mcp_common::MakeErrorReply(TEXT("RVT volume has no URuntimeVirtualTextureComponent root"));
    }
    if (!Comp->GetVirtualTexture())
    {
        return mcp_common::MakeErrorReply(
            TEXT("RVT volume has no VirtualTexture bound; call create_rvt_volume first (RuntimeVirtualTexture::SetBounds requires it)"));
    }

    AActor* AlignActor = mcp_common::FindActorByName(World, AlignName);
    if (!AlignActor)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("bounds_align_actor not found: %s"), *AlignName));
    }

    // snap/expand are protected UPROPERTYs with no public setter -> reflection.
    bool bSnap = false;
    if (Params->TryGetBoolField(TEXT("snap_to_landscape"), bSnap))
    {
        FString Err;
        mcp_common::SetObjectProperty(Comp, TEXT("bSnapBoundsToLandscape"), MakeShared<FJsonValueBoolean>(bSnap), Err);
    }
    double ExpandD = 0.0;
    if (Params->TryGetNumberField(TEXT("expand_bounds"), ExpandD))
    {
        FString Err;
        mcp_common::SetObjectProperty(Comp, TEXT("ExpandBounds"), MakeShared<FJsonValueNumber>(ExpandD), Err);
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetRVTVolumeBounds", "Set RVT Volume Bounds (MCP)"));
    Comp->SetBoundsAlignActor(AlignActor);
    RuntimeVirtualTexture::SetBounds(Comp);  // Modify + SetActorTransform + PostEditMove on the volume
    Volume->MarkPackageDirty();

    TSharedPtr<FJsonObject> D = MakeShared<FJsonObject>();
    D->SetStringField(TEXT("volume_name"), Volume->GetActorLabel());
    D->SetStringField(TEXT("bounds_align_actor"), AlignName);
    D->SetBoolField(TEXT("snap_to_landscape"), bSnap);
    D->SetNumberField(TEXT("expand_bounds"), ExpandD);
    D->SetBoolField(TEXT("applied"), true);
    return mcp_common::MakeOkReply(D);
}

namespace UEAgentPerformanceImpl
{
    /**
     * RVT into slot SlotIdx of Proxy's RuntimeVirtualTextures, the array grown to reach it, after Modify(). The
     * components' render state is recreated so the new RVT renders WITHOUT a save+reload -- exactly what the engine's
     * ALandscapeProxy::PostEditChangeProperty does for this property (LandscapeEdit.cpp) -- and MarkPackageDirty then
     * persists the change.
     */
    void WriteRVTSlot(ALandscapeProxy& Proxy, int32 SlotIdx, URuntimeVirtualTexture* RVT)
    {
        Proxy.Modify();
        if (Proxy.RuntimeVirtualTextures.Num() <= SlotIdx)
        {
            Proxy.RuntimeVirtualTextures.SetNum(SlotIdx + 1);
        }
        Proxy.RuntimeVirtualTextures[SlotIdx] = RVT;
        Proxy.MarkComponentsRenderStateDirty();
        Proxy.MarkPackageDirty();
    }

    bool HasRVTInSlot(const ALandscapeProxy& Proxy, int32 SlotIdx, URuntimeVirtualTexture* RVT)
    {
        return Proxy.RuntimeVirtualTextures.IsValidIndex(SlotIdx) && Proxy.RuntimeVirtualTextures[SlotIdx] == RVT;
    }
}

// ===========================================================================
// assign_rvt_to_landscape -- THE PROXIES ARE WHAT RENDER
//
// Writing ONLY the ALandscape actor puts the RVT in `Landscape.RuntimeVirtualTextures`
// and writes NOTHING to the RVT cache: on a World Partition landscape every
// ALandscapeStreamingProxy keeps an EMPTY RuntimeVirtualTextures array, and in World
// Partition the proxies are the actors that hold the ULandscapeComponents that
// actually render -- with no error, no warning, no log line. (set_landscape_material
// does reach all of them, because it routes through ALandscapeProxy::PostEditChangeProperty,
// whose material branch propagates; this property has no such path.)
//
// RuntimeVirtualTextures is declared `meta = (LandscapeOverridable)` on
// ALandscapeProxy (LandscapeProxy.h), i.e. it IS a shared property, but
// nothing in this path runs the synchronization. So every proxy is written explicitly
// (Modify() first, which dirties its own OFPA package -- the same proxies + 1 packages
// set_landscape_material dirties) and then every one of them is RE-READ. `applied` and the envelope's `success` are
// derived from that re-read, never asserted: all-but-one is reported as a FAILURE,
// because in practice it is one.
// ===========================================================================

// MCP:assign_rvt_to_landscape - Add a URuntimeVirtualTexture to the landscape's RuntimeVirtualTextures array (slot-index based) AND propagate it to every ALandscapeStreamingProxy of that landscape, because in World Partition the proxies are the actors that render. Every proxy is re-read after the write; `applied` and the envelope `success` are DERIVED from that re-read (proxies_with_rvt == proxies_total), never asserted.
//   @param landscape_name_or_label - Actor label/name of the target ALandscape
//   @param rvt_asset_path - /Game path to the RVT asset
//   @param slot_index - 0-based index into LandscapeProxy::RuntimeVirtualTextures (default 0)
//   @param propagate_to_proxies - Optional bool, default TRUE. Opt-out that writes the landscape actor alone. NOTE: with it false the proxies stay empty and nothing writes to the RVT cache, so `applied` will still report false (plus a `warning`) unless the proxies already carried the RVT -- the opt-out exists to reproduce that write, not to bless its result.
//   @returns { applied, slot_index, rvt_asset_path, landscape, landscape_has_rvt, propagate_to_proxies, proxies_total, proxies_updated, proxies_with_rvt, proxies_missing: [string], proxies_missing_truncated: bool, proxy_enumeration, warning?, requested: {...} }
TSharedPtr<FJsonObject> HandleAssignRVTToLandscape(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World)) { return NoWorld; }
    FString LandscapeId, RVTPath;
    int32 SlotIdx = 0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("landscape_name_or_label"), LandscapeId)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("rvt_asset_path"), RVTPath)) { return Missing; }
    Params->TryGetNumberField(TEXT("slot_index"), SlotIdx);
    if (SlotIdx < 0)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("slot_index must be >= 0 (got %d)"), SlotIdx));
    }

    // Default TRUE: the proxies are the ones that render. The opt-out writes the landscape
    // actor alone, e.g. to compare both.
    bool bPropagate = true;
    Params->TryGetBoolField(TEXT("propagate_to_proxies"), bPropagate);

    URuntimeVirtualTexture* RVT = Cast<URuntimeVirtualTexture>(StaticLoadObject(URuntimeVirtualTexture::StaticClass(), nullptr, *RVTPath));
    if (!RVT)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("RVT asset not found: %s"), *RVTPath));
    }

    ALandscape* Landscape = nullptr;
    for (TActorIterator<ALandscape> It(World); It; ++It)
    {
        if (It->GetActorLabel() == LandscapeId || It->GetName() == LandscapeId)
        {
            Landscape = *It; break;
        }
    }
    if (!Landscape)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Landscape not found: %s"), *LandscapeId));
    }

    // -- Enumerate the streaming proxies BEFORE writing, so proxies_total is a
    // property of the landscape and not of how far the write got.
    // Primary route: ULandscapeInfo::ForEachLandscapeProxy (LANDSCAPE_API) walks
    // the root actor + every registered proxy. Fallback: a GUID-filtered actor
    // iterator, for the case where the landscape has no ULandscapeInfo yet.
    TArray<ALandscapeStreamingProxy*> Proxies;
    FString ProxyEnumeration;
    if (ULandscapeInfo* Info = Landscape->GetLandscapeInfo())
    {
        Info->ForEachLandscapeProxy([&Proxies](ALandscapeProxy* Proxy) -> bool
        {
            if (ALandscapeStreamingProxy* StreamingProxy = Cast<ALandscapeStreamingProxy>(Proxy))
            {
                Proxies.AddUnique(StreamingProxy);
            }
            return true;   // keep walking
        });
        ProxyEnumeration = TEXT("ULandscapeInfo::ForEachLandscapeProxy");
    }
    if (Proxies.Num() == 0)
    {
        const FGuid LandscapeGuid = Landscape->GetLandscapeGuid();
        for (TActorIterator<ALandscapeStreamingProxy> It(World); It; ++It)
        {
            ALandscapeStreamingProxy* StreamingProxy = *It;
            if (StreamingProxy && (!LandscapeGuid.IsValid() || StreamingProxy->GetLandscapeGuid() == LandscapeGuid))
            {
                Proxies.AddUnique(StreamingProxy);
            }
        }
        ProxyEnumeration = TEXT("TActorIterator<ALandscapeStreamingProxy> (LandscapeGuid-filtered)");
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "AssignRVTToLandscape", "Assign RVT To Landscape (MCP)"));
    UEAgentPerformanceImpl::WriteRVTSlot(*Landscape, SlotIdx, RVT);

    // -- The write that actually reaches the renderer. Modify() per proxy is what
    // dirties each proxy's own OFPA package (same thing set_landscape_material's
    // PostEditChange path ends up doing for one package per proxy).
    int32 ProxiesUpdated = 0;
    if (bPropagate)
    {
        for (ALandscapeStreamingProxy* Proxy : Proxies)
        {
            if (!Proxy)
            {
                continue;
            }
            UEAgentPerformanceImpl::WriteRVTSlot(*Proxy, SlotIdx, RVT);
            ++ProxiesUpdated;
        }
    }

    // -- RE-READ. This is the only thing `applied` is allowed to be built from.
    const bool bLandscapeHasRVT = UEAgentPerformanceImpl::HasRVTInSlot(*Landscape, SlotIdx, RVT);

    // Bounded: a large World Partition landscape has hundreds of proxies, and a full miss
    // would otherwise emit every one of their names.
    constexpr int32 MaxMissingReported = 32;
    int32 ProxiesWithRVT = 0;
    TArray<TSharedPtr<FJsonValue>> MissingJson;
    int32 MissingCount = 0;
    for (ALandscapeStreamingProxy* Proxy : Proxies)
    {
        if (!Proxy)
        {
            continue;
        }
        if (UEAgentPerformanceImpl::HasRVTInSlot(*Proxy, SlotIdx, RVT))
        {
            ++ProxiesWithRVT;
        }
        else
        {
            ++MissingCount;
            if (MissingJson.Num() < MaxMissingReported)
            {
                MissingJson.Add(MakeShared<FJsonValueString>(Proxy->GetActorLabel()));
            }
        }
    }

    const bool bApplied = bLandscapeHasRVT && (ProxiesWithRVT == Proxies.Num());

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("applied"), bApplied);
    Data->SetNumberField(TEXT("slot_index"), SlotIdx);
    Data->SetStringField(TEXT("rvt_asset_path"), RVTPath);
    Data->SetStringField(TEXT("landscape"), LandscapeId);
    Data->SetBoolField(TEXT("landscape_has_rvt"), bLandscapeHasRVT);
    Data->SetBoolField(TEXT("propagate_to_proxies"), bPropagate);
    Data->SetNumberField(TEXT("proxies_total"), Proxies.Num());
    Data->SetNumberField(TEXT("proxies_updated"), ProxiesUpdated);
    Data->SetNumberField(TEXT("proxies_with_rvt"), ProxiesWithRVT);
    Data->SetArrayField(TEXT("proxies_missing"), MissingJson);
    Data->SetBoolField(TEXT("proxies_missing_truncated"), MissingCount > MissingJson.Num());
    Data->SetStringField(TEXT("proxy_enumeration"), ProxyEnumeration);

    FString Error;
    if (!bApplied)
    {
        const FString LandscapeState = bLandscapeHasRVT ? TEXT("ok") : TEXT("NOT set");
        Error = FString::Printf(
            TEXT("RVT reached %d/%d landscape streaming proxies (landscape actor: %s). In World Partition the PROXIES are what render, so a partial assignment writes nothing to the RVT cache."),
            ProxiesWithRVT, Proxies.Num(), *LandscapeState);
        FString Warning = Error;
        if (!bPropagate)
        {
            Warning = FString::Printf(TEXT("propagate_to_proxies=false: %s"), *Error);
        }
        Data->SetStringField(TEXT("warning"), Warning);
    }

    mcp_common::AddRequestedEcho(Data, Params,
        { TEXT("landscape_name_or_label"), TEXT("rvt_asset_path"), TEXT("slot_index"), TEXT("propagate_to_proxies") });

    return mcp_common::MakeOutcomeReply(Data, bApplied, Error);
}

// ---------------------------------------------------------------------------
// Typed delete RVT + unassign + region introspection.
// ---------------------------------------------------------------------------

// MCP:remove_rvt_asset - Destroy a URuntimeVirtualTexture asset (mirror of create_rvt_asset). Typed-delete: it loads the asset and REFUSES to delete it if it is not a URuntimeVirtualTexture, so it cannot wipe the wrong asset the way the generic delete_asset can; that refusal, and a delete the engine does not carry out, make success false with deleted false. Idempotent on missing assets. It deletes the ASSET, not any binding: unassign_rvt_from_landscape is what detaches it from a landscape first.
//   @param asset_path - Full /Game/... asset path of the RVT.
//   @returns { asset_path, deleted, already_existed, asset_class?, note? }
//   NOTE (impl): delegates the actual delete to UEditorAssetLibrary::DeleteAsset once the type check passes.

TSharedPtr<FJsonObject> HandleRemoveRVTAsset(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_path"), AssetPath)) { return Missing; }
    if (!UEditorAssetLibrary::DoesAssetExist(AssetPath))
    {
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetStringField(TEXT("asset_path"), AssetPath);
        Resp->SetBoolField(TEXT("deleted"), false);
        Resp->SetBoolField(TEXT("already_existed"), false);
        Resp->SetStringField(TEXT("note"), TEXT("asset_path does not exist; nothing to remove"));
        return mcp_common::MakeOkReply(Resp);
    }

    UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
    if (!Asset)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("remove_rvt_asset: could not load asset at %s"), *AssetPath));
    }

    // URuntimeVirtualTexture is the typed-delete target (the same UClass that
    // create_rvt_asset returns). Anything else (e.g. a UTexture2D accidentally
    // passed) is refused: success false, nothing deleted.
    if (!Asset->IsA<URuntimeVirtualTexture>())
    {
        return AssetCommandHelpers::WrongTypeRefusal(TEXT("remove_rvt_asset"), AssetPath, *Asset, TEXT("URuntimeVirtualTexture"));
    }

    const FString AssetClassName = Asset->GetClass()->GetName();
    const bool bDeleted = UEditorAssetLibrary::DeleteAsset(AssetPath);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("asset_path"), AssetPath);
    Resp->SetStringField(TEXT("asset_class"), AssetClassName);
    Resp->SetBoolField(TEXT("deleted"), bDeleted);
    Resp->SetBoolField(TEXT("already_existed"), true);
    return AssetCommandHelpers::DeleteOutcomeReply(TEXT("remove_rvt_asset"), Resp, bDeleted, AssetPath);
}

// MCP:unassign_rvt_from_landscape - Inverse of assign_rvt_to_landscape: clears ONE slot of a landscape's RuntimeVirtualTextures array, breaking the binding without leaving a dangling entry behind. It unbinds only -- the RVT asset itself survives (remove_rvt_asset destroys that). Reports the asset path that was in the slot, so the operation is reversible.
//   @param landscape_name - Actor name (or label) of the ALandscape actor (omit for first landscape in world)
//   @param slot_index - 0-based index to clear (default 0)
//   @returns { cleared, slot_index, previous_rvt_asset_path?, landscape }

TSharedPtr<FJsonObject> HandleUnassignRVTFromLandscape(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World)) { return NoWorld; }

    ALandscape* Landscape = nullptr;
    FString LandscapeName;
    if (Params->TryGetStringField(TEXT("landscape_name"), LandscapeName) && !LandscapeName.IsEmpty())
    {
        for (TActorIterator<ALandscape> It(World); It; ++It)
        {
            if (It->GetName() == LandscapeName || It->GetActorLabel() == LandscapeName)
            {
                Landscape = *It;
                break;
            }
        }
        if (!Landscape)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Landscape '%s' not found"), *LandscapeName));
        }
    }
    else
    {
        for (TActorIterator<ALandscape> It(World); It; ++It)
        {
            Landscape = *It;
            break;
        }
        if (!Landscape)
        {
            return mcp_common::MakeErrorReply(TEXT("No landscape in editor world"));
        }
    }

    int32 SlotIdx = 0;
    Params->TryGetNumberField(TEXT("slot_index"), SlotIdx);
    if (SlotIdx < 0 || SlotIdx >= Landscape->RuntimeVirtualTextures.Num())
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("slot_index %d out of range [0, %d)"), SlotIdx, Landscape->RuntimeVirtualTextures.Num()));
    }

    FString PreviousPath;
    if (Landscape->RuntimeVirtualTextures[SlotIdx])
    {
        PreviousPath = Landscape->RuntimeVirtualTextures[SlotIdx]->GetPathName();
    }
    Landscape->Modify();
    Landscape->RuntimeVirtualTextures[SlotIdx] = nullptr;
    Landscape->MarkPackageDirty();

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("cleared"), true);
    Resp->SetNumberField(TEXT("slot_index"), SlotIdx);
    if (!PreviousPath.IsEmpty())
    {
        Resp->SetStringField(TEXT("previous_rvt_asset_path"), PreviousPath);
    }
    Resp->SetStringField(TEXT("landscape"), Landscape->GetName());
    return mcp_common::MakeOkReply(Resp);
}

// MCP:list_active_regions - Read-side of region_start: the named timing regions still OPEN in this bridge instance, each with how long it has been running. Use it to spot a region you forgot to close, or to read an elapsed time without ending the region. Entries disappear when region_end closes them, one name at a time; the state is per bridge instance and does not survive a restart.
//   @returns { total, regions:[{ name, started_at_s, elapsed_s }] }

TSharedPtr<FJsonObject> HandleListActiveRegions(const TSharedPtr<FJsonObject>& Params)
{
    const double Now = FPlatformTime::Seconds();
    TArray<TSharedPtr<FJsonValue>> Entries;
    for (const TPair<FString, double>& Pair : ActiveRegions)
    {
        TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
        E->SetStringField(TEXT("name"), Pair.Key);
        E->SetNumberField(TEXT("started_at_s"), Pair.Value);
        E->SetNumberField(TEXT("elapsed_s"), Now - Pair.Value);
        Entries.Add(MakeShared<FJsonValueObject>(E));
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetNumberField(TEXT("total"), Entries.Num());
    Resp->SetArrayField(TEXT("regions"), Entries);
    return mcp_common::MakeOkReply(Resp);
}

// MCP:get_frame_count - Read the engine frame counters without side effects: frame_count (GFrameCounter, game thread) and frame_count_render_thread (GFrameCounterRenderThread). Read it before and after another call to measure how many frames ticked; for frame times use frame_timing or get_fps_info.
//   @returns { frame_count: uint64, frame_count_render_thread: uint64 }
//   Read-only, headless-safe. Single-int sync anchor for dead-reckoning / asset-load timing.
TSharedPtr<FJsonObject> HandleGetFrameCount(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetNumberField(TEXT("frame_count"), static_cast<double>(GFrameCounter));
    Resp->SetNumberField(TEXT("frame_count_render_thread"), static_cast<double>(GFrameCounterRenderThread));
    return mcp_common::MakeOkReply(Resp);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("assign_rvt_to_landscape"), &HandleAssignRVTToLandscape },
    { TEXT("bookmark"), &HandleBookmark },
    { TEXT("create_rvt_asset"), &HandleCreateRVTAsset },
    { TEXT("create_rvt_volume"), &HandleCreateRVTVolume },
    { TEXT("frame_timing"), &HandleFrameTiming },
    { TEXT("get_fps_info"), &HandleGetFPSInfo },
    { TEXT("get_frame_count"), &HandleGetFrameCount },
    { TEXT("get_memory_usage"), &HandleGetMemoryUsage },
    { TEXT("get_rvt_info"), &HandleGetRVTInfo },
    { TEXT("get_trace_status"), &HandleGetTraceStatus },
    { TEXT("list_active_regions"), &HandleListActiveRegions },
    { TEXT("list_traces"), &HandleListTraces },
    { TEXT("region_end"), &HandleRegionEnd },
    { TEXT("region_start"), &HandleRegionStart },
    { TEXT("remove_rvt_asset"), &HandleRemoveRVTAsset },
    { TEXT("set_rvt_volume_bounds"), &HandleSetRVTVolumeBounds },
    { TEXT("start_trace"), &HandleStartTrace },
    { TEXT("stop_trace"), &HandleStopTrace },
    { TEXT("unassign_rvt_from_landscape"), &HandleUnassignRVTFromLandscape },
};
} // namespace UEAgentPerformanceCommands
