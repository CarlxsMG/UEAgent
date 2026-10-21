// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Media/LevelSequenceUtils.h"

#include "Editor.h"
#include "UObject/SoftObjectPath.h" // FSoftObjectPath + FDirectoryPath
#include "Math/IntPoint.h"

// Level sequence asset model (resolve the sequence to render)
#include "LevelSequence.h"

// Movie Render Pipeline (Editor subsystem + queue/job/config)
#include "MoviePipelineQueueSubsystem.h"      // MovieRenderPipelineEditor
#include "MoviePipelineQueue.h"               // MovieRenderPipelineCore (Queue + Job)
#include "MoviePipelinePrimaryConfig.h"       // MovieRenderPipelineCore
#include "MoviePipelineConfigBase.h"          // MovieRenderPipelineCore (FindOrAddSettingByClass)
#include "MoviePipelineOutputSetting.h"       // MovieRenderPipelineCore (resolution / dir)
#include "MoviePipelineSetting.h"             // MovieRenderPipelineCore (UMoviePipelineSetting)
#include "MoviePipelineExecutor.h"            // MovieRenderPipelineCore (UMoviePipelineExecutorBase)

// Image output passes (MovieRenderPipelineRenderPasses)
#include "MoviePipelineImageSequenceOutput.h" // _PNG / _JPG / _BMP (EXR resolved by reflection -- its header pulls Imath/OpenEXR)

// PIE executor (MovieRenderPipelineEditor) -- used by render_sequence_to_movie
#include "MoviePipelinePIEExecutor.h"

#define LOCTEXT_NAMESPACE "UEAgentMovieRender"

/**
 * Movie Render Queue commands: author, inspect and run the jobs of the editor Movie Pipeline Queue
 * (UMoviePipelineQueueSubsystem).
 *
 * Jobs, configurations and output settings are built on the asset/config model, which is headless-safe:
 * UMoviePipelineQueue::AllocateNewJob, then FindOrAddSettingByClass on the job's primary config. The render
 * itself is asynchronous -- it spins up a Play-In-Editor session through UMoviePipelinePIEExecutor -- so
 * render_sequence_to_movie reports render_started with a note instead of blocking until it completes.
 */
namespace UEAgentMovieRenderCommands
{

// MCP-DOMAIN: Export

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace UEAgentMovieRenderImpl
{
    // The editor render queue of QueueSubsystem; null with OutError set to the reply when the subsystem (no editor)
    // or its queue is missing.
    UMoviePipelineQueue* QueueOf(UMoviePipelineQueueSubsystem* QueueSubsystem, TSharedPtr<FJsonObject>& OutError)
    {
        if (!QueueSubsystem)
        {
            OutError = mcp_common::MakeErrorReply(
                TEXT("Movie Pipeline Queue subsystem unavailable (no editor)"));
            return nullptr;
        }
        UMoviePipelineQueue* Queue = QueueSubsystem->GetQueue();
        if (!Queue)
        {
            OutError = mcp_common::MakeErrorReply(TEXT("Movie Pipeline queue unavailable"));
        }
        return Queue;
    }
}

UMoviePipelineQueueSubsystem* GetQueueSubsystem()
{
    if (!GEditor)
    {
        return nullptr;
    }
    return GEditor->GetEditorSubsystem<UMoviePipelineQueueSubsystem>();
}

UMoviePipelineExecutorJob* FindJobByName(const FString& JobName)
{
    UMoviePipelineQueueSubsystem* QueueSubsystem = GetQueueSubsystem();
    if (!QueueSubsystem)
    {
        return nullptr;
    }
    UMoviePipelineQueue* Queue = QueueSubsystem->GetQueue();
    if (!Queue)
    {
        return nullptr;
    }
    for (UMoviePipelineExecutorJob* Job : Queue->GetJobs())
    {
        if (Job && Job->JobName == JobName)
        {
            return Job;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// create_render_job
// ---------------------------------------------------------------------------
// MCP:create_render_job - Allocates a real Movie Render Queue job for a Level Sequence (headless authoring).  Adds a new UMoviePipelineExecutorJob to the editor Movie Pipeline queue, points it at the given Level Sequence, optionally sets its map, and ensures it has a primary configuration. The job persists in the editor's transient render queue.          (e.g. "/Game/Maps/MyMap.MyMap").
//   @param sequence_name - Name of the target Level Sequence (found by AssetName under /Game).
//   @param job_name - Optional explicit job name; auto-generated as "RenderJob_<sequence_name>" if omitted
//   @param map_path - Optional map object path for the job (e.g. "/Game/Maps/MyMap.MyMap")
//   @returns { job_name: string, sequence_name: string, sequence_path: string, map: string, has_configuration: bool, job_created: bool, queue_job_count: number }

TSharedPtr<FJsonObject> HandleCreateRenderJob(const TSharedPtr<FJsonObject>& Params)
{
    FString SequenceName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sequence_name"), SequenceName)) { return Missing; }

    ULevelSequence* Sequence = LevelSequenceUtils::FindByName(SequenceName);
    if (!Sequence)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Level Sequence '%s' not found under /Game"), *SequenceName));
    }

    TSharedPtr<FJsonObject> Error;
    UMoviePipelineQueue* Queue = UEAgentMovieRenderImpl::QueueOf(GetQueueSubsystem(), Error);
    if (!Queue)
    {
        return Error;
    }

    // Allocate the real job in the editor queue.
    UMoviePipelineExecutorJob* Job = Queue->AllocateNewJob(UMoviePipelineExecutorJob::StaticClass());
    if (!Job)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to allocate a new render job"));
    }

    // Name the job.
    FString JobName;
    if (!Params->TryGetStringField(TEXT("job_name"), JobName) || JobName.IsEmpty())
    {
        JobName = FString::Printf(TEXT("RenderJob_%s"), *SequenceName);
    }
    Job->JobName = JobName;

    // Point the job at the sequence (SetSequence is the BlueprintSetter).
    const FString SequencePath = Sequence->GetPathName();
    Job->SetSequence(FSoftObjectPath(SequencePath));

    // Map: explicit param, else the sequence's owning world if discoverable. Headless-safe
    // to leave this as whatever was supplied; the actual render later resolves the map.
    FString MapPath;
    if (Params->TryGetStringField(TEXT("map_path"), MapPath) && !MapPath.IsEmpty())
    {
        Job->Map = FSoftObjectPath(MapPath);
    }

    // Ensure the job has a primary configuration (it auto-creates a DefaultConfig in its
    // constructor; read it back to confirm and report).
    UMoviePipelinePrimaryConfig* Config = Job->GetConfiguration();
    const bool bHasConfig = (Config != nullptr);

    const int32 JobCount = Queue->GetJobs().Num();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("job_name"), Job->JobName);
    Data->SetStringField(TEXT("sequence_name"), SequenceName);
    Data->SetStringField(TEXT("sequence_path"), SequencePath);
    Data->SetStringField(TEXT("map"), Job->Map.ToString());
    Data->SetBoolField(TEXT("has_configuration"), bHasConfig);
    Data->SetBoolField(TEXT("job_created"), true);
    Data->SetNumberField(TEXT("queue_job_count"), JobCount);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_render_job_output
// ---------------------------------------------------------------------------
// MCP:set_render_job_output - Configures output resolution / directory / image format on a render job's config.  Finds (or adds) a UMoviePipelineOutputSetting on the job's primary configuration to set resolution + output directory, and adds the matching image-sequence output container for the requested format (png/jpg/exr/bmp).
//   @renamed-from configure_render_output
//   @param job_name - Name of an existing render job in the queue (from create_render_job).
//   @param resolution_x - Output width in pixels (optional, must be paired with resolution_y)
//   @param resolution_y - Output height in pixels (optional, must be paired with resolution_x)
//   @param output_dir - Output directory path (optional)
//   @param format - Image format: "png" | "jpg" | "exr" | "bmp" (optional, default "png"; exr falls back to png if class unavailable)
//   @returns { job_name: string, format: string, configured: bool, resolution_x: number?, resolution_y: number?, output_dir: string?, settings: [string], setting_count: number }

TSharedPtr<FJsonObject> HandleConfigureRenderOutput(const TSharedPtr<FJsonObject>& Params)
{
    FString JobName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("job_name"), JobName)) { return Missing; }

    UMoviePipelineExecutorJob* Job = FindJobByName(JobName);
    if (!Job)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Render job '%s' not found in the queue"), *JobName));
    }

    UMoviePipelinePrimaryConfig* Config = Job->GetConfiguration();
    if (!Config)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Render job '%s' has no configuration"), *JobName));
    }

    TArray<TSharedPtr<FJsonValue>> SettingsArray;

    // --- Output setting (resolution + directory + filename) ---
    UMoviePipelineOutputSetting* OutputSetting =
        Cast<UMoviePipelineOutputSetting>(Config->FindOrAddSettingByClass(UMoviePipelineOutputSetting::StaticClass()));
    if (OutputSetting)
    {
        int32 ResX = 0;
        int32 ResY = 0;
        const bool bHasResX = Params->TryGetNumberField(TEXT("resolution_x"), ResX);
        const bool bHasResY = Params->TryGetNumberField(TEXT("resolution_y"), ResY);
        if (bHasResX && bHasResY && ResX > 0 && ResY > 0)
        {
            OutputSetting->OutputResolution = FIntPoint(ResX, ResY);
        }

        FString OutputDir;
        if (Params->TryGetStringField(TEXT("output_dir"), OutputDir) && !OutputDir.IsEmpty())
        {
            OutputSetting->OutputDirectory.Path = OutputDir;
        }

        SettingsArray.Add(MakeShared<FJsonValueString>(OutputSetting->GetClass()->GetName()));
    }

    // --- Image output container by format ---
    FString Format = TEXT("png");
    Params->TryGetStringField(TEXT("format"), Format);
    Format = Format.ToLower();

    UClass* OutputContainerClass = nullptr;
    if (Format == TEXT("jpg") || Format == TEXT("jpeg"))
    {
        OutputContainerClass = UMoviePipelineImageSequenceOutput_JPG::StaticClass();
    }
    else if (Format == TEXT("exr"))
    {
        // EXR output lives in MoviePipelineEXROutput.h, which transitively includes the
        // Imath/OpenEXR third-party headers (not on this plugin's include path). Resolve
        // the class by reflected path instead of #including the header; fall back to PNG.
        OutputContainerClass = UClass::TryFindTypeSlow<UClass>(TEXT("MoviePipelineImageSequenceOutput_EXR"));
    }
    else if (Format == TEXT("bmp"))
    {
        OutputContainerClass = UMoviePipelineImageSequenceOutput_BMP::StaticClass();
    }
    else
    {
        // Default + explicit "png"
        Format = TEXT("png");
        OutputContainerClass = UMoviePipelineImageSequenceOutput_PNG::StaticClass();
    }

    if (!OutputContainerClass)
    {
        // EXR class unavailable by reflection -> fall back to PNG.
        Format = TEXT("png");
        OutputContainerClass = UMoviePipelineImageSequenceOutput_PNG::StaticClass();
    }
    UMoviePipelineSetting* OutputContainer = Config->FindOrAddSettingByClass(OutputContainerClass);
    if (OutputContainer)
    {
        SettingsArray.Add(MakeShared<FJsonValueString>(OutputContainer->GetClass()->GetName()));
    }

    const int32 SettingCount = Config->GetUserSettings().Num();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("job_name"), JobName);
    Data->SetStringField(TEXT("format"), Format);
    Data->SetBoolField(TEXT("configured"), true);
    if (OutputSetting)
    {
        Data->SetNumberField(TEXT("resolution_x"), OutputSetting->OutputResolution.X);
        Data->SetNumberField(TEXT("resolution_y"), OutputSetting->OutputResolution.Y);
        Data->SetStringField(TEXT("output_dir"), OutputSetting->OutputDirectory.Path);
    }
    Data->SetArrayField(TEXT("settings"), SettingsArray);
    Data->SetNumberField(TEXT("setting_count"), SettingCount);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_render_queue
// ---------------------------------------------------------------------------
// MCP:get_render_queue - Lists the jobs currently in the editor Movie Render Queue.
//   @returns { jobs: [{ job_name: string, sequence: string, map: string, enabled: bool, setting_count: number }], queue_job_count: number }

TSharedPtr<FJsonObject> HandleGetRenderQueue(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UMoviePipelineQueue* Queue = UEAgentMovieRenderImpl::QueueOf(GetQueueSubsystem(), Error);
    if (!Queue)
    {
        return Error;
    }

    TArray<TSharedPtr<FJsonValue>> JobsArray;
    const TArray<UMoviePipelineExecutorJob*> Jobs = Queue->GetJobs();
    for (UMoviePipelineExecutorJob* Job : Jobs)
    {
        if (!Job)
        {
            continue;
        }
        TSharedPtr<FJsonObject> JobObj = MakeShared<FJsonObject>();
        JobObj->SetStringField(TEXT("job_name"), Job->JobName);
        JobObj->SetStringField(TEXT("sequence"), Job->Sequence.ToString());
        JobObj->SetStringField(TEXT("map"), Job->Map.ToString());
        JobObj->SetBoolField(TEXT("enabled"), Job->IsEnabled());

        int32 SettingCount = 0;
        if (UMoviePipelinePrimaryConfig* Config = Job->GetConfiguration())
        {
            SettingCount = Config->GetUserSettings().Num();
        }
        JobObj->SetNumberField(TEXT("setting_count"), SettingCount);
        JobsArray.Add(MakeShared<FJsonValueObject>(JobObj));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("jobs"), JobsArray);
    Data->SetNumberField(TEXT("queue_job_count"), Jobs.Num());
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// render_sequence_to_movie
// ---------------------------------------------------------------------------
// MCP:render_sequence_to_movie - Authors a one-off render job for a sequence and starts the (async) movie render.  Allocates a real job (sequence + default PNG output config) and triggers RenderQueueWithExecutor using the PIE executor. The render runs ASYNCHRONOUSLY (it spins up a Play-In-Editor session), so this returns render_started immediately with an honest async note rather than blocking for completion. In a headless / non-interactive context the render may not start; the job + config are still authored for real.
//   @param sequence_name - Name of the target Level Sequence (found by AssetName under /Game).
//   @param map_path - Optional map object path (e.g. "/Game/Maps/MyMap.MyMap")
//   @returns { sequence_name: string, job_name: string, job_created: bool, queue_job_count: number, render_started: bool, render_async: bool, note: string }

TSharedPtr<FJsonObject> HandleRenderSequenceToMovie(const TSharedPtr<FJsonObject>& Params)
{
    FString SequenceName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sequence_name"), SequenceName)) { return Missing; }

    ULevelSequence* Sequence = LevelSequenceUtils::FindByName(SequenceName);
    if (!Sequence)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Level Sequence '%s' not found under /Game"), *SequenceName));
    }

    UMoviePipelineQueueSubsystem* QueueSubsystem = GetQueueSubsystem();
    TSharedPtr<FJsonObject> Error;
    UMoviePipelineQueue* Queue = UEAgentMovieRenderImpl::QueueOf(QueueSubsystem, Error);
    if (!Queue)
    {
        return Error;
    }

    // Author a real one-off job for this sequence with a default PNG output container.
    UMoviePipelineExecutorJob* Job = Queue->AllocateNewJob(UMoviePipelineExecutorJob::StaticClass());
    if (!Job)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to allocate a render job"));
    }
    Job->JobName = FString::Printf(TEXT("OneShot_%s"), *SequenceName);
    Job->SetSequence(FSoftObjectPath(Sequence->GetPathName()));

    FString MapPath;
    if (Params->TryGetStringField(TEXT("map_path"), MapPath) && !MapPath.IsEmpty())
    {
        Job->Map = FSoftObjectPath(MapPath);
    }

    if (UMoviePipelinePrimaryConfig* Config = Job->GetConfiguration())
    {
        Config->FindOrAddSettingByClass(UMoviePipelineOutputSetting::StaticClass());
        Config->FindOrAddSettingByClass(UMoviePipelineImageSequenceOutput_PNG::StaticClass());
    }

    const int32 JobCount = Queue->GetJobs().Num();

    // The render is asynchronous: RenderQueueWithExecutor spins up a Play-In-Editor session
    // through UMoviePipelinePIEExecutor. We start it but do NOT block on completion -- that
    // happens across PIE ticks and is reported async. We only attempt the kick if the queue
    // is not already rendering, and we report honestly whether it started.
    bool bRenderStarted = false;
    FString Note;
    if (QueueSubsystem->IsRendering())
    {
        Note = TEXT("A movie pipeline render is already in progress; queued job will be picked up. Completion is asynchronous (PIE).");
    }
    else
    {
        UMoviePipelineExecutorBase* Executor =
            QueueSubsystem->RenderQueueWithExecutor(UMoviePipelinePIEExecutor::StaticClass());
        bRenderStarted = (Executor != nullptr);
        Note = bRenderStarted
            ? TEXT("Render started via UMoviePipelinePIEExecutor. Completion is asynchronous (runs in PIE); poll get_render_queue / the editor for progress.")
            : TEXT("Render executor could not be started in this context (requires an interactive editor / PIE). The queue + job + config were authored for real.");
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("sequence_name"), SequenceName);
    Data->SetStringField(TEXT("job_name"), Job->JobName);
    Data->SetBoolField(TEXT("job_created"), true);
    Data->SetNumberField(TEXT("queue_job_count"), JobCount);
    Data->SetBoolField(TEXT("render_started"), bRenderStarted);
    Data->SetBoolField(TEXT("render_async"), true);
    Data->SetStringField(TEXT("note"), Note);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// delete_render_job
// ---------------------------------------------------------------------------
// MCP:delete_render_job - Deletes a job from the editor Movie Render Queue (headless-safe).  Removes a single job by name via UMoviePipelineQueue::DeleteJob, or clears the whole queue via DeleteAllJobs when delete_all is set. Idempotent: deleting a job that is not present reports already_absent:true honestly instead of failing.
//   @param job_name - Name of the render job to delete (required unless delete_all is true).
//   @param delete_all - Optional bool; when true, clears every job from the queue and ignores job_name.
//   @returns { job_name?: string, deleted?: bool, already_absent?: bool, deleted_all?: bool, queue_job_count: number }

TSharedPtr<FJsonObject> HandleDeleteRenderJob(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UMoviePipelineQueue* Queue = UEAgentMovieRenderImpl::QueueOf(GetQueueSubsystem(), Error);
    if (!Queue)
    {
        return Error;
    }

    // delete_all: clear the whole queue.
    bool bDeleteAll = false;
    if (Params->TryGetBoolField(TEXT("delete_all"), bDeleteAll) && bDeleteAll)
    {
        Queue->DeleteAllJobs();

        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetBoolField(TEXT("deleted_all"), true);
        Data->SetNumberField(TEXT("queue_job_count"), Queue->GetJobs().Num());
        return mcp_common::MakeOkReply(Data);
    }

    // Single-job delete by name. Read into a bool, not negated in the if: job_name is needed only
    // here (delete_all returned above), and a negated read would make the catalog require it always.
    FString JobName;
    const bool bHasJobName = Params->TryGetStringField(TEXT("job_name"), JobName);
    if (!bHasJobName || JobName.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("'job_name' is required (unless 'delete_all' is true)"));
    }

    // Idempotent: a job that is already gone is reported as already_absent, not as a failure.
    UMoviePipelineExecutorJob* Job = FindJobByName(JobName);
    const bool bFound = (Job != nullptr);
    if (bFound)
    {
        Queue->DeleteJob(Job);
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("job_name"), JobName);
    Data->SetBoolField(TEXT("deleted"), bFound);
    Data->SetBoolField(TEXT("already_absent"), !bFound);
    Data->SetNumberField(TEXT("queue_job_count"), Queue->GetJobs().Num());
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// cancel_render_job
// ---------------------------------------------------------------------------
// MCP:cancel_render_job - Cancels the in-progress movie render, if one is running (honest headless gate).  Movie renders run inside a Play-In-Editor session (UMoviePipelinePIEExecutor), so in a headless / non-interactive editor there is normally no active executor. When an active render exists it is canceled via UMoviePipelineExecutorBase::CancelAllJobs; otherwise this reports was_rendering:false honestly (an expected headless state, not an error).
//   @param job_name - Optional job name (informational only; cancellation stops the active executor's whole render).
//   @returns { canceled: bool, was_rendering: bool, note: string }

TSharedPtr<FJsonObject> HandleCancelRenderJob(const TSharedPtr<FJsonObject>& Params)
{
    UMoviePipelineQueueSubsystem* QueueSubsystem = GetQueueSubsystem();
    if (!QueueSubsystem)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Movie Pipeline Queue subsystem unavailable (no editor)"));
    }

    UMoviePipelineExecutorBase* Executor = QueueSubsystem->GetActiveExecutor();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    if (Executor && Executor->IsRendering())
    {
        Executor->CancelAllJobs();
        Data->SetBoolField(TEXT("canceled"), true);
        Data->SetBoolField(TEXT("was_rendering"), true);
        Data->SetStringField(TEXT("note"), TEXT("Active render canceled via CancelAllJobs()."));
    }
    else
    {
        // Expected headless state: nothing is rendering. This is NOT an error.
        Data->SetBoolField(TEXT("canceled"), false);
        Data->SetBoolField(TEXT("was_rendering"), false);
        Data->SetStringField(TEXT("note"), TEXT("No active movie render to cancel (rendering runs in PIE and requires an interactive editor). Nothing was executing."));
    }
    return mcp_common::MakeOkReply(Data);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("cancel_render_job"), &HandleCancelRenderJob },
    { TEXT("set_render_job_output"), &HandleConfigureRenderOutput },
    { TEXT("create_render_job"), &HandleCreateRenderJob },
    { TEXT("delete_render_job"), &HandleDeleteRenderJob },
    { TEXT("get_render_queue"), &HandleGetRenderQueue },
    { TEXT("render_sequence_to_movie"), &HandleRenderSequenceToMovie },
};
} // namespace UEAgentMovieRenderCommands

#undef LOCTEXT_NAMESPACE
