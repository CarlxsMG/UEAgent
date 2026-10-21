// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Media/LevelSequenceUtils.h"

#include "Editor.h"
#include "Engine/World.h"           // render_sequence_to_movie: the editor world as the default job map
#include "Misc/PackageName.h"       // FPackageName::IsValidLongPackageName -- the map check the PIE executor makes
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

    // Whether the PIE render executor accepts Map as a job's map: before rendering it checks every job of the queue
    // with UMoviePipelineEditorBlueprintLibrary::IsMapValidForRemoteRender -- a valid long package name under a
    // writable root, so neither an empty map nor an unsaved level (under the read-only /Temp root) -- and stops the
    // whole render at once when one fails.
    bool IsRenderableMap(const FSoftObjectPath& Map)
    {
        return FPackageName::IsValidLongPackageName(Map.GetLongPackageName());
    }

    // The map a new job renders in: MapPath when given, else the level open in the editor (what the Movie Render Queue
    // window gives a new job). Null with OutMap set when the PIE executor accepts it, else the error reply of Tool,
    // so no job is added that would stop every render of the queue.
    TSharedPtr<FJsonObject> ResolveJobMap(const FString& MapPath, const TCHAR* Tool, FSoftObjectPath& OutMap)
    {
        OutMap.Reset();
        if (!MapPath.IsEmpty())
        {
            OutMap = FSoftObjectPath(MapPath);
        }
        else if (UWorld* EditorWorld = mcp_common::GetEditorWorld())
        {
            OutMap = FSoftObjectPath(EditorWorld);
        }
        if (IsRenderableMap(OutMap))
        {
            return nullptr;
        }
        const FString MapLabel = MapPath.IsEmpty()
            ? FString::Printf(TEXT("of the level open in the editor ('%s')"), *OutMap.ToString())
            : FString::Printf(TEXT("'%s'"), *MapPath);
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("%s: the map %s is not the package path of a saved map (such as /Game/Maps/MyMap.MyMap; an unsaved level lives ")
            TEXT("under /Temp), so the renderer would stop at once; nothing was added. Pass map_path, or save the level first."),
            Tool, *MapLabel));
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
// MCP:create_render_job - Adds a new job for a Level Sequence to this editor session's Movie Render Queue, without rendering: the job points at the sequence, renders in map_path or, without it, in the level open in the editor (as the Movie Render Queue window does), and has its default primary configuration. A map that is not the package path of a saved map (an unsaved level included) is refused before the job is added, since the renderer would stop the whole queue at once. One undo step; the queue lives in memory, so nothing is saved to disk and the job is gone when the editor closes. Works headless. Each call adds another job, even with a job_name the queue already has (tools that take job_name then act on the first match). Next, set resolution, folder and image format with set_render_job_output and render with render_sequence_to_movie, which renders every job in the queue; get_render_queue lists the jobs and delete_render_job takes one out. For a quick render with default PNG output, render_sequence_to_movie alone is enough.
//   @param sequence_name - Asset name of the Level Sequence, not a path (e.g. "Intro"): the first Level Sequence under /Game with that name, case-insensitive; get_level_sequences lists them.
//   @param job_name - Optional job name (default "RenderJob_<sequence_name>"), the name set_render_job_output and delete_render_job take; not checked for uniqueness.
//   @param map_path - Optional object path of the map the job renders in (e.g. "/Game/Maps/MyMap.MyMap"); without it the job takes the level open in the editor. A map that is not the package path of a saved map (a long package name under a writable root; an unsaved level is under /Temp) is an error and no job is added.
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

    // The map is resolved, and one the renderer would reject refused, before the job is added: a job with such a map
    // would make every later render of the queue stop at once.
    FString MapPath;
    Params->TryGetStringField(TEXT("map_path"), MapPath);
    FSoftObjectPath JobMap;
    if (TSharedPtr<FJsonObject> BadMap = UEAgentMovieRenderImpl::ResolveJobMap(MapPath, TEXT("create_render_job"), JobMap))
    {
        return BadMap;
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

    // Map: map_path, else the level open in the editor (resolved and checked above).
    Job->Map = JobMap;

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
// MCP:set_render_job_output - Set where and how a Movie Render Queue job writes its frames: resolution, output folder and image format, on the job's configuration. Use it after create_render_job, which makes the job; the queue is rendered by render_sequence_to_movie, which adds a job of its own for a sequence and then renders every job in the queue, this one included; read the queue back with get_render_queue. Each format adds its image-sequence output to the job and none is removed, so calling it with png and then jpg writes both (delete_render_job and create_render_job start the job over). Settings that are not given keep their value; an unknown format, or a resolution with only one side or a side of 0 or less, is refused and nothing changes. exr falls back to png when the EXR output is not available, and format says which was used. The queue lives in this editor session: nothing is saved to disk, and the job is lost when the editor closes.
//   @renamed-from configure_render_output
//   @param job_name - Name of a job in the editor's render queue, as create_render_job or get_render_queue returns it.
//   @param resolution_x - Output width in pixels, e.g. 1920; give it together with resolution_y, or neither to keep the current resolution.
//   @param resolution_y - Output height in pixels, e.g. 1080; give it together with resolution_x.
//   @param output_dir - Folder the frames are written to, absolute ("D:/Renders/Shot01") or with Movie Render Queue tokens ("{project_dir}/Saved/MovieRenders/"); omit it to keep the current folder.
//   @param format - Image format of the frames: "png" (default), "jpg" (or "jpeg"), "exr" or "bmp"; any other value is refused.
//   @returns { job_name: string, format: string, configured: bool, resolution_x: number?, resolution_y: number?, output_dir: string?, settings: [string], image_outputs: [string], setting_count: number }

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

    // Validate everything before the first change, so a refusal leaves the job as it was.
    int32 ResX = 0;
    int32 ResY = 0;
    const bool bHasResX = Params->TryGetNumberField(TEXT("resolution_x"), ResX);
    const bool bHasResY = Params->TryGetNumberField(TEXT("resolution_y"), ResY);
    if (bHasResX != bHasResY || (bHasResX && (ResX <= 0 || ResY <= 0)))
    {
        return mcp_common::MakeErrorReply(TEXT("set_render_job_output: give resolution_x and resolution_y together, both greater than 0 (or neither, to keep the current resolution); nothing was changed"));
    }

    FString Format = TEXT("png");
    Params->TryGetStringField(TEXT("format"), Format);
    Format = Format.ToLower();
    if (Format == TEXT("jpeg"))
    {
        Format = TEXT("jpg");
    }
    if (Format != TEXT("png") && Format != TEXT("jpg") && Format != TEXT("exr") && Format != TEXT("bmp"))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("set_render_job_output: unknown format '%s'; use png, jpg, exr or bmp. Nothing was changed"), *Format));
    }

    TArray<TSharedPtr<FJsonValue>> SettingsArray;

    // --- Output setting (resolution + directory + filename) ---
    UMoviePipelineOutputSetting* OutputSetting =
        Cast<UMoviePipelineOutputSetting>(Config->FindOrAddSettingByClass(UMoviePipelineOutputSetting::StaticClass()));
    if (OutputSetting)
    {
        if (bHasResX)
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

    // --- Image output container by format (validated above) ---
    UClass* OutputContainerClass = nullptr;
    if (Format == TEXT("jpg"))
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

    // Every image-sequence output the job now has: a format set by an earlier call stays, and renders too.
    // The base class is found by reflection, like the EXR output, so no further module is linked.
    TArray<TSharedPtr<FJsonValue>> ImageOutputs;
    if (const UClass* ImageOutputBase = UClass::TryFindTypeSlow<UClass>(TEXT("MoviePipelineImageSequenceOutputBase")))
    {
        for (const UMoviePipelineSetting* Setting : Config->GetUserSettings())
        {
            if (Setting && Setting->GetClass()->IsChildOf(ImageOutputBase))
            {
                ImageOutputs.Add(MakeShared<FJsonValueString>(Setting->GetClass()->GetName()));
            }
        }
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
    Data->SetArrayField(TEXT("image_outputs"), ImageOutputs);
    Data->SetNumberField(TEXT("setting_count"), SettingCount);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_render_queue
// ---------------------------------------------------------------------------
// MCP:get_render_queue - List the jobs in the editor's Movie Render Queue right now (the queue of this editor session, held by the Movie Pipeline Queue subsystem; nothing is read from disk): each job's job_name, sequence and map paths, enabled flag and setting_count (settings on its configuration), plus queue_job_count. Use it to find job names before set_render_job_output, delete_render_job or render_sequence_to_movie, and to confirm what create_render_job added; cancel_render_job stops a render in progress. Read-only and idempotent. An empty queue gives queue_job_count 0, not an error; an editor without the subsystem (Movie Render Queue plugin off) is an error.
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
// MCP:render_sequence_to_movie - Renders a Level Sequence to PNG frames with Movie Render Queue: adds a job named OneShot_<sequence_name> with the engine's default output (1920x1080 into {project_dir}/Saved/MovieRenders/) and starts rendering the whole editor queue, jobs from create_render_job included, in a Play-In-Editor session. Asynchronous: the editor starts that session on its next frame, so the call returns before any frame is written, and render_state is the renderer read right after the start: waiting_for_pie or rendering (render_started true); already_rendering when a render was running, which starts nothing new and the job joins its queue; ended_without_rendering when the renderer stopped at once, which makes success false (the editor log, category LogMovieRenderPipeline, says why; the job stays queued). The job renders in map_path, or without it in the level open in the editor; a map that is not the package path of a saved map (an unsaved level included) is refused before the job is added, and so is a start while the queue holds another job with such a map, since the renderer would abort the whole queue (delete_render_job removes it). The frames need an interactive editor, since they are drawn in that PIE session. Frames overwrite earlier files of the same name in the output folder, and each call adds another job. For another resolution or format use create_render_job and set_render_job_output first; follow progress with get_render_queue and stop with cancel_render_job.
//   @param sequence_name - Asset name of the Level Sequence, not a path (e.g. "Intro"): the first Level Sequence under /Game with that name, case-insensitive; get_level_sequences lists them.
//   @param map_path - Optional object path of the map to render in (e.g. "/Game/Maps/MyMap.MyMap"); without it the job takes the level open in the editor, as the Movie Render Queue window does. A map that is not the package path of a saved map (a long package name under a writable root; an unsaved level is under /Temp) is refused before anything changes.
//   @returns { sequence_name: string, job_name: string, job_created: bool, map_path: string, queue_job_count: number, render_started: bool, render_state: "waiting_for_pie"|"rendering"|"already_rendering"|"ended_without_rendering", render_async: bool, note: string }

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

    // The map the job renders in: map_path, else the level open in the editor, which is what the engine's own
    // Movie Render Queue window gives a new job. The PIE executor stops the whole queue at once when any job's
    // map is not a long package name, so such a map is refused here, before the job is added.
    FString MapPath;
    Params->TryGetStringField(TEXT("map_path"), MapPath);
    FSoftObjectPath JobMap;
    if (TSharedPtr<FJsonObject> BadMap = UEAgentMovieRenderImpl::ResolveJobMap(MapPath, TEXT("render_sequence_to_movie"), JobMap))
    {
        return BadMap;
    }
    const bool bAlreadyRendering = QueueSubsystem->IsRendering();
    if (!bAlreadyRendering)
    {
        TArray<FString> UnrenderableJobs;
        for (const UMoviePipelineExecutorJob* Existing : Queue->GetJobs())
        {
            if (Existing && !UEAgentMovieRenderImpl::IsRenderableMap(Existing->Map))
            {
                UnrenderableJobs.Add(FString::Printf(TEXT("'%s' (map '%s')"), *Existing->JobName, *Existing->Map.ToString()));
            }
        }
        if (UnrenderableJobs.Num() > 0)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("render_sequence_to_movie: the queue holds job(s) whose map is not the package path of a saved map: %s. The renderer would stop ")
                TEXT("at once for the whole queue, so nothing was added or started; remove them with delete_render_job and re-add them with create_render_job and a map_path."),
                *FString::Join(UnrenderableJobs, TEXT(", "))));
        }
    }

    // Author a real one-off job for this sequence with a default PNG output container.
    UMoviePipelineExecutorJob* Job = Queue->AllocateNewJob(UMoviePipelineExecutorJob::StaticClass());
    if (!Job)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to allocate a render job"));
    }
    Job->JobName = FString::Printf(TEXT("OneShot_%s"), *SequenceName);
    Job->SetSequence(FSoftObjectPath(Sequence->GetPathName()));
    Job->Map = JobMap;

    if (UMoviePipelinePrimaryConfig* Config = Job->GetConfiguration())
    {
        Config->FindOrAddSettingByClass(UMoviePipelineOutputSetting::StaticClass());
        Config->FindOrAddSettingByClass(UMoviePipelineImageSequenceOutput_PNG::StaticClass());
    }

    const int32 JobCount = Queue->GetJobs().Num();

    // The render is asynchronous: the PIE executor requests a Play-In-Editor session, which the editor starts on its
    // next frame, and renders there. What is measured is the executor right after the start: the subsystem drops an
    // executor as soon as it finishes -- RenderQueueWithExecutor then returns null, since it returns the active
    // executor after starting it -- so a null or no-longer-active executor ended without rendering, and one still
    // active is waiting for its PIE session or already rendering.
    FString RenderState;
    FString Note;
    if (bAlreadyRendering)
    {
        RenderState = TEXT("already_rendering");
        Note = TEXT("A movie render was already in progress, so nothing new was started; the job joins the running queue. Completion is asynchronous (PIE).");
    }
    else
    {
        UMoviePipelineExecutorBase* Executor =
            QueueSubsystem->RenderQueueWithExecutor(UMoviePipelinePIEExecutor::StaticClass());
        if (!Executor || QueueSubsystem->GetActiveExecutor() != Executor)
        {
            RenderState = TEXT("ended_without_rendering");
            Note = TEXT("The render executor finished at once without rendering a frame; the editor log (LogMovieRenderPipeline) says why.");
        }
        else if (Executor->IsRendering())
        {
            RenderState = TEXT("rendering");
            Note = TEXT("Rendering in the PIE session. Completion is asynchronous: frames land in the output folder as they are drawn.");
        }
        else
        {
            RenderState = TEXT("waiting_for_pie");
            Note = TEXT("The render executor is waiting for its PIE session, which the editor starts on its next frame; frames land in the output folder as they are drawn.");
        }
    }
    const bool bRenderStarted = RenderState == TEXT("rendering") || RenderState == TEXT("waiting_for_pie");
    const bool bRenderGoing = bRenderStarted || bAlreadyRendering;

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("sequence_name"), SequenceName);
    Data->SetStringField(TEXT("job_name"), Job->JobName);
    Data->SetBoolField(TEXT("job_created"), true);
    Data->SetStringField(TEXT("map_path"), Job->Map.ToString());
    Data->SetNumberField(TEXT("queue_job_count"), JobCount);
    Data->SetBoolField(TEXT("render_started"), bRenderStarted);
    Data->SetStringField(TEXT("render_state"), RenderState);
    Data->SetBoolField(TEXT("render_async"), true);
    Data->SetStringField(TEXT("note"), Note);
    return mcp_common::MakeOutcomeReply(Data, bRenderGoing, bRenderGoing ? FString() : FString::Printf(
        TEXT("render_sequence_to_movie: the job %s was added but no render is running (render_state %s): %s ")
        TEXT("The job stays in the queue; delete_render_job removes it."), *Job->JobName, *RenderState, *Note));
}

// ---------------------------------------------------------------------------
// delete_render_job
// ---------------------------------------------------------------------------
// MCP:delete_render_job - Deletes one job, by name, from this editor session's Movie Render Queue, or every job with delete_all true. Only the queued job goes: its sequence, its map and any frames already rendered stay. One undo step; nothing is saved to disk, since the queue lives in memory. Works headless. Idempotent: a job_name the queue does not have succeeds with already_absent true and nothing changes; when several jobs share the name, only the first is deleted. A call with neither job_name nor delete_all is an error. get_render_queue lists the job names, create_render_job and render_sequence_to_movie add jobs, and cancel_render_job stops a render in progress, which this tool does not do.
//   @param job_name - Name of the job to delete, as get_render_queue lists it (e.g. "RenderJob_Intro"), matched case-insensitively; required unless delete_all is true.
//   @param delete_all - Optional bool, default false; true deletes every job in the queue and ignores job_name.
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
