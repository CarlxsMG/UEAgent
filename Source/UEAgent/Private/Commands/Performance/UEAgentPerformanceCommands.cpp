// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Misc/EngineVersionComparison.h"
#include "Commands/Common/McpEnumLabels.h"
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
// assign_runtime_virtual_texture_to_landscape has to reach the STREAMING PROXIES, not just the
// ALandscape -- in World Partition the proxies are what render (see the handler's
// comment block). ULandscapeInfo::ForEachLandscapeProxy + ALandscapeStreamingProxy
// are the enumeration route; both are LANDSCAPE_API / StaticClass()-reachable.
#include "LandscapeInfo.h"
#include "LandscapeProxy.h"
#include "LandscapeStreamingProxy.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/SavePackage.h"
#include "HAL/IConsoleManager.h"
#include "UObject/UObjectArray.h"   // GUObjectArray (get_fps_info object_count)
#include "Misc/EngineVersion.h"
#include "Scalability.h"            // Scalability::Min/MaxResolutionScale (get_fps_info screen_percentage)

// GAverageFPS / GAverageMS (get_fps_info) are exported ENGINE_API globals defined in Engine/Private/UnrealEngine.cpp
// and declared only in engine-private code, so the exported symbols are declared here to link against them.
extern ENGINE_API float GAverageFPS;
extern ENGINE_API float GAverageMS;

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

// MCP:get_fps_info - Read a one-call frame-rate snapshot of the editor without changing anything: current_fps from the editor world's last tick (0 while paused or before the first tick, and the editor world even while PIE runs), target_fps (the engine max-FPS cap, or 60 when uncapped) and their difference, the engine-wide rolling averages GAverageFPS and GAverageMS, frame pacing (r.VSync, smoothing, max and min FPS), the r.ScreenPercentage in use, the live UObject count, process physical memory in bytes and MB, and the world's name and net mode. Use it for a quick is-it-fast-enough check; for per-thread times and a CPU/GPU bottleneck verdict use get_frame_timing, for a memory breakdown (scene, GPU, system) get_memory_usage, for quality presets get_scalability_levels, and for a profile over time start_trace. The values are instantaneous: an unfocused editor is throttled, so call it twice and compare. Errors only when there is no editor world.
//   @returns { current_fps: float, target_fps: float, fps_variance: float, average_fps: float (GAverageFPS), average_ms: float (GAverageMS), engine_version: string, is_editor: bool, is_dedicated_server: bool, performance_settings: {vsync_enabled (r.VSync), smooth_frame_rate, max_fps (0 = uncapped), min_fps (lower bound of frame-rate smoothing; 0 without it)}, screen_percentage: float (r.ScreenPercentage within the scalability range), object_count: int, memory: { used_physical_bytes, peak_used_physical_bytes, available_physical_bytes, total_physical_bytes, used_physical_mb, peak_used_physical_mb, available_physical_mb }, world_name: string, net_mode: string }

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
    // The engine-wide rolling averages (smoothed over frames), next to the last tick's instant rate above.
    Payload->SetNumberField(TEXT("average_fps"), GAverageFPS);
    Payload->SetNumberField(TEXT("average_ms"), GAverageMS);

    Payload->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
    Payload->SetBoolField(TEXT("is_editor"), GIsEditor);
    Payload->SetBoolField(TEXT("is_dedicated_server"),
        IsRunningDedicatedServer() || EditorWorld->GetNetMode() == NM_DedicatedServer);
    Payload->SetObjectField(TEXT("performance_settings"), Impl::FramePacingReport());
    // r.ScreenPercentage within the scalability range, as Scalability::GetResolutionScreenPercentage reads it.
    const IConsoleVariable* ScreenPercentage = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ScreenPercentage"));
    Payload->SetNumberField(TEXT("screen_percentage"), ScreenPercentage
        ? FMath::Clamp(ScreenPercentage->GetFloat(), Scalability::MinResolutionScale, Scalability::MaxResolutionScale) : 0.0f);
    Payload->SetNumberField(TEXT("object_count"), GUObjectArray.GetObjectArrayNumMinusAvailable());

    // Process memory to the byte (get_memory_usage rounds to whole megabytes).
    const FPlatformMemoryStats MemStats = FPlatformMemory::GetStats();
    const double BytesPerMB = 1024.0 * 1024.0;
    TSharedPtr<FJsonObject> Memory = MakeShared<FJsonObject>();
    Memory->SetNumberField(TEXT("used_physical_bytes"), static_cast<double>(MemStats.UsedPhysical));
    Memory->SetNumberField(TEXT("peak_used_physical_bytes"), static_cast<double>(MemStats.PeakUsedPhysical));
    Memory->SetNumberField(TEXT("available_physical_bytes"), static_cast<double>(MemStats.AvailablePhysical));
    Memory->SetNumberField(TEXT("total_physical_bytes"), static_cast<double>(MemStats.TotalPhysical));
    Memory->SetNumberField(TEXT("used_physical_mb"), MemStats.UsedPhysical / BytesPerMB);
    Memory->SetNumberField(TEXT("peak_used_physical_mb"), MemStats.PeakUsedPhysical / BytesPerMB);
    Memory->SetNumberField(TEXT("available_physical_mb"), MemStats.AvailablePhysical / BytesPerMB);
    Payload->SetObjectField(TEXT("memory"), Memory);

    Payload->SetStringField(TEXT("world_name"), EditorWorld->GetName());
    Payload->SetStringField(TEXT("net_mode"), mcp_common::NetModeLabel(EditorWorld->GetNetMode()));
    return mcp_common::MakeOkReply(Payload);
}

// MCP:get_memory_usage - Read a snapshot of memory use for the whole editor process without changing anything, in four blocks: process_memory (used, peak, available and total physical, used and peak virtual, in whole MB), scene_memory (a walk of every actor of the editor world, not a PIE world: actors, components, and on StaticMeshActors only the meshes, distinct materials and the distinct textures those materials use, plus every UTexture2D resident in memory process-wide), gpu_memory (dedicated, shared and total video memory, present only when the RHI reports them, and streaming and non-streaming texture memory) and system_info (platform, CPU brand, cores, threads). Use it to see where memory goes overall; for one asset's own size use get_asset_memory_usage, for byte-precise process memory next to the frame rate get_fps_info. The scene walk visits every actor, so it costs more on big levels. Errors only when there is no editor world.
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

// MCP:get_frame_timing - Read the current frame's Game/Render/GPU/RHI thread times plus a CPU-vs-GPU bottleneck verdict with an actionable hint.
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
        Hint    = TEXT("CPU and GPU loads are roughly balanced. Look for hitches via add_trace_bookmark "
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

// MCP:start_trace - Starts recording an Unreal Insights trace of this editor process to a new .utrace file, for profiling CPU, GPU and frame timing with the chosen channels. Idempotent: while any trace is active, including one started with -trace on the command line, it starts nothing and returns already_tracing true with the current destination; call stop_trace first for a new recording. The engine will not write over an existing file: an output_path that already exists makes the call fail with nothing started. Only the trace file is written; assets and levels are untouched. Flow: start_trace, reproduce the problem, mark moments with add_trace_bookmark, stop_trace to flush, list_traces to find the file; get_trace_status tells whether a trace is running. For a quick reading without a file use get_frame_timing or get_fps_info.
//   @hints additive
//   @param channels - Comma-separated Insights channels to enable (default "cpu,frame,bookmark"; others: gpu, region, memtag, counters, log, screenshot), e.g. "cpu,gpu,frame,bookmark".
//   @param output_path - Path of the .utrace file to create, absolute (e.g. "D:/Traces/run1.utrace") or relative to <ProjectDir>/Saved/Profiling; another extension is changed to .utrace and the folder is created. Default <ProjectDir>/Saved/Profiling/MCP_<YYYYMMDD_HHMMSS>.utrace.
//   @returns { tracing: bool, already_tracing: bool, destination: string, channels?: string, started_at_s?: number, connection_type?: string, elapsed_s?: number, note: string } -- channels and started_at_s for a new trace; connection_type (file, server, custom_relay...) and elapsed_s when a trace was already active
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
#if !UE_VERSION_OLDER_THAN(5, 8, 0) // TracingToSecureNetwork: secure-network tracing only exists from UE 5.8
                                    : (Status == FTraceAuxiliary::ETraceSystemStatus::TracingToSecureNetwork) ? TEXT("secure_network")
#endif
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
    Resp->SetStringField(TEXT("note"),            TEXT("Trace active. Use add_trace_bookmark and start_timing_region/end_timing_region to annotate, then stop_trace to flush."));
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

// MCP:get_trace_status - Reports whether Unreal Insights tracing is available in this build, whether a trace is recording right now, and to which file or server. Read-only and instant: it starts and stops nothing. Call it before start_trace to see whether a trace is already running (for example one started with -trace on the command line, which start_trace would not replace), or after stop_trace to confirm the recording ended. list_traces lists the recorded .utrace files on disk, and list_timing_regions the open timers of start_timing_region, which are not traces.
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
#if !UE_VERSION_OLDER_THAN(5, 8, 0) // TracingToSecureNetwork: secure-network tracing only exists from UE 5.8
        case FTraceAuxiliary::ETraceSystemStatus::TracingToSecureNetwork: StatusStr = TEXT("tracing_to_secure_network"); break;
#endif
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

// MCP:add_trace_bookmark - Emit a TRACE_BOOKMARK into the active .utrace at the current frame. The bookmark channel must be enabled in the trace (default start_trace channels include it). The text is dynamic via printf-style formatting, but the Format token itself is a TEXT() literal as required by the macro's static_assert.
//   @param text - The bookmark text. Becomes the bookmark label in UnrealInsights.
//   @returns { bookmarked: bool, text: string, frame: int, note: string }
TSharedPtr<FJsonObject> HandleBookmark(const TSharedPtr<FJsonObject>& Params)
{
    FString Text;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("text"), Text)) { return Missing; }

    if (!FTraceAuxiliary::IsConnected())
    {
        // Honest error — a bookmark without a trace is a no-op, so reject
        // instead of silently swallowing the call. Caller can fall back to start_timing_region/end
        // for in-process timing.
        return mcp_common::MakeErrorReply(
            TEXT("No active trace -- bookmarks require an active Insights recording. "
                 "Call start_trace first (or use start_timing_region/end for in-process timing)."));
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

// MCP:start_timing_region - Starts a named wall-clock timer in this editor session, to measure how long a stretch of work (several tool calls, a PIE run) takes; end_timing_region with the same name stops it and returns the elapsed time in ms. It needs no Insights trace and writes nothing to a trace or to disk: the start time is kept in memory only and is lost when the editor restarts. Idempotent: a name that is already open keeps its original start time and returns already_started true with started false. list_timing_regions shows the open timers; for engine-level profiling use start_trace with add_trace_bookmark instead.
//   @hints additive
//   @param name - Required timer name, any non-empty string (e.g. "import_pass"); pass the same name to end_timing_region (names match case-insensitively).
//   @returns { started: bool, already_started: bool, name: string, started_at_s: float, note?: string } -- started_at_s is the editor's platform clock in seconds; note only when the region was already open
TSharedPtr<FJsonObject> HandleRegionStart(const TSharedPtr<FJsonObject>& Params)
{
    FString Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name)) { return Missing; }

    // Idempotency — if the region already exists, return its current start
    // time instead of overwriting. end_timing_region will compute the duration against the
    // original start.
    if (const double* Existing = ActiveRegions.Find(Name))
    {
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetBoolField  (TEXT("started"),        false);
        Resp->SetBoolField  (TEXT("already_started"), true);
        Resp->SetStringField(TEXT("name"),            Name);
        Resp->SetNumberField(TEXT("started_at_s"),    *Existing);
        Resp->SetStringField(TEXT("note"),            TEXT("Region was already started; original start time preserved. Call end_timing_region to close."));
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

// MCP:end_timing_region - Close an open named region and return its duration in ms. Removes the region from the active set; subsequent end_timing_region with the same name returns an error.
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
            FString::Printf(TEXT("Region '%s' was not started. Call start_timing_region first."), *Name));
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

// MCP:create_runtime_virtual_texture - Create a URuntimeVirtualTexture asset at directory_path/asset_name with the given material type, tile count and tile size (both exponents, clamped silently), and save it; the save result is not checked. Returns asset_path, tile_count, tile_size and size in texels. Then use create_runtime_virtual_texture_volume and assign_runtime_virtual_texture_to_landscape.
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

// MCP:get_runtime_virtual_texture_info - Read a URuntimeVirtualTexture asset without changing it: material_type (full enum name, e.g. "ERuntimeVirtualTextureMaterialType::WorldHeight"), tile_count in tiles, tile_size and size in texels (not the exponents create_runtime_virtual_texture takes), and continuous_update.
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

// create_runtime_virtual_texture_volume — the headless crash to avoid when spawning an
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
// RVT -> the producer allocates cleanly. create_runtime_virtual_texture_volume below follows that order, and
// set_runtime_virtual_texture_volume_bounds refuses a volume whose RVT is not bound yet.
//
// Why the volume matters: assign_runtime_virtual_texture_to_landscape only wires the landscape as a
// WRITER into its RuntimeVirtualTextures array. Without an ARuntimeVirtualTextureVolume
// (the URuntimeVirtualTextureComponent that registers the producer + defines the
// world-space region) nothing PRODUCES the RVT -- the chain is incomplete.

// MCP:create_runtime_virtual_texture_volume - Spawn an ARuntimeVirtualTextureVolume (the RVT producer that registers the runtime virtual texture into the scene and defines its world-space region) bound to an RVT asset. Without a volume, assign_runtime_virtual_texture_to_landscape only wires the landscape as a writer -- nothing produces the RVT. Optionally fits the volume bounds to an actor (usually the landscape) in the same call. Headless-safe: spawns with a null VirtualTexture (no render-state) then binds a valid RVT. Idempotent: returns the existing volume if one already targets this RVT.
//   @param rvt_asset_path - Required. /Game path to the URuntimeVirtualTexture asset (create_runtime_virtual_texture).
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

// MCP:set_runtime_virtual_texture_volume_bounds - Fit an existing ARuntimeVirtualTextureVolume's bounds to an actor -- the engine "Set Bounds" button (RuntimeVirtualTexture::SetBounds). Sets the volume's BoundsAlignActor and recomputes its transform to cover that actor (a landscape uses its complete bounds) plus any primitives writing to the volume's RVT. The volume must already have its RVT bound (create_runtime_virtual_texture_volume). Optional landscape texel-snap + bounds expansion.
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
            TEXT("RVT volume has no VirtualTexture bound; call create_runtime_virtual_texture_volume first (RuntimeVirtualTexture::SetBounds requires it)"));
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
// assign_runtime_virtual_texture_to_landscape -- THE PROXIES ARE WHAT RENDER
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

// MCP:assign_runtime_virtual_texture_to_landscape - Add a URuntimeVirtualTexture to the landscape's RuntimeVirtualTextures array (slot-index based) AND propagate it to every ALandscapeStreamingProxy of that landscape, because in World Partition the proxies are the actors that render. Every proxy is re-read after the write; `applied` and the envelope `success` are DERIVED from that re-read (proxies_with_rvt == proxies_total), never asserted.
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
// Unassign RVT + region introspection.
// ---------------------------------------------------------------------------

// MCP:unassign_runtime_virtual_texture_from_landscape - Inverse of assign_runtime_virtual_texture_to_landscape: clears ONE slot of a landscape's RuntimeVirtualTextures array, breaking the binding without leaving a dangling entry behind. It unbinds only -- the RVT asset itself survives (delete_asset with expected_class "RuntimeVirtualTexture" destroys that). Reports the asset path that was in the slot, so the operation is reversible.
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

// MCP:list_timing_regions - Read-side of start_timing_region: the named timing regions still OPEN in this bridge instance, each with how long it has been running. Use it to spot a region you forgot to close, or to read an elapsed time without ending the region. Entries disappear when end_timing_region closes them, one name at a time; the state is per bridge instance and does not survive a restart.
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

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("assign_runtime_virtual_texture_to_landscape"), &HandleAssignRVTToLandscape },
    { TEXT("add_trace_bookmark"), &HandleBookmark },
    { TEXT("create_runtime_virtual_texture"), &HandleCreateRVTAsset },
    { TEXT("create_runtime_virtual_texture_volume"), &HandleCreateRVTVolume },
    { TEXT("get_frame_timing"), &HandleFrameTiming },
    { TEXT("get_fps_info"), &HandleGetFPSInfo },
    { TEXT("get_memory_usage"), &HandleGetMemoryUsage },
    { TEXT("get_runtime_virtual_texture_info"), &HandleGetRVTInfo },
    { TEXT("get_trace_status"), &HandleGetTraceStatus },
    { TEXT("list_timing_regions"), &HandleListActiveRegions },
    { TEXT("list_traces"), &HandleListTraces },
    { TEXT("end_timing_region"), &HandleRegionEnd },
    { TEXT("start_timing_region"), &HandleRegionStart },
    { TEXT("set_runtime_virtual_texture_volume_bounds"), &HandleSetRVTVolumeBounds },
    { TEXT("start_trace"), &HandleStartTrace },
    { TEXT("stop_trace"), &HandleStopTrace },
    { TEXT("unassign_runtime_virtual_texture_from_landscape"), &HandleUnassignRVTFromLandscape },
};
} // namespace UEAgentPerformanceCommands
