#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

#include "Misc/MonitoredProcess.h"   // FSerializedUATProcess / FMonitoredProcess (Core)
#include "Misc/CoreDelegates.h"
#include "Misc/Paths.h"

/**
 * Project packaging commands (BuildCookRun via UAT), driven from inside a running editor.
 *
 * Why FSerializedUATProcess (Core, MonitoredProcess.h) directly, not IUATHelperModule:
 * IUATHelperModule::CreateUatTask is the Editor menu's own wrapper -- it drives Slate
 * notifications, EditorAnalytics, and the "PackagingResults" message log, none of which
 * an MCP client can observe. FSerializedUATProcess is what CreateUatTask itself launches
 * underneath; using it directly (with our own OnOutput/OnCompleted/OnCanceled/OnLaunchFailed
 * bindings) gives an honest, pollable job state without any UI dependency.
 *
 * Honest-headless: UAT is a real external process (RunUAT.bat/.sh) that can take
 * minutes (it invokes UBT to -build the target, then cooks). start_package_build never
 * blocks on completion -- it launches and returns immediately with build_started/build_async;
 * the client polls get_package_build_status for progress/outcome. Only one build runs at a
 * time (FSerializedUATProcess itself serializes concurrent UAT invocations process-wide).
 *
 * Game-thread rule: the process's OnOutput/OnCompleted/OnCanceled/OnLaunchFailed delegates
 * fire from FMonitoredProcess's own worker thread (see MonitoredProcess.cpp TickInternal),
 * NOT the game thread. All they do here is write into FCriticalSection-guarded state; no
 * UObject/engine API is touched off the game thread.
 */
namespace UEAgentPackagingCommands
{

// MCP-DOMAIN: Packaging

// The single in-flight (or most recently finished) UAT job. Only one build can run at a time; starting a
// new one while a prior one is still running is refused honestly rather than silently queued
// (FSerializedUATProcess would serialize them at the process level, but that would make status-polling
// ambiguous about which job is being reported). ~FMonitoredProcess() cancels and waits for a process still
// running, so the job is released before the engine exits.
struct FPackageBuildJob
{
    TSharedPtr<FSerializedUATProcess> Process;
    FString Platform;
    FString Configuration;
    FString CommandLine;
    FDateTime StartTime;

    FCriticalSection StateLock;
    FString OutputTail;          // rolling buffer, capped
    bool bFinished = false;
    bool bCanceled = false;
    bool bLaunchFailed = false;
    int32 ExitCode = 0;
    FDateTime EndTime;
};
static TSharedPtr<FPackageBuildJob> CurrentJob;
static const FDelegateHandle ReleaseJobAtExit = FCoreDelegates::OnPreExit.AddLambda([] { CurrentJob.Reset(); });

namespace
{
    // Caps the rolling output buffer kept per job so a long UAT run doesn't grow unbounded.
    constexpr int32 MaxOutputTailChars = 16384;

    void AppendOutputTail(FString& Tail, const FString& Line)
    {
        Tail += Line;
        Tail += TEXT("\n");
        if (Tail.Len() > MaxOutputTailChars)
        {
            Tail = Tail.Right(MaxOutputTailChars);
        }
    }
}

// ---------------------------------------------------------------------------
// start_package_build
// ---------------------------------------------------------------------------
// MCP:start_package_build - Starts an asynchronous project package build (BuildCookRun) via UAT (FSerializedUATProcess), for the current .uproject. Runs RunUAT BuildCookRun -project=<this project> -noP4 -utf8output -platform=<platform> -clientconfig=<configuration> -cook -allmaps -build -stage -pak -archive -archivedirectory=<dir>. This is a real external process that can take minutes (it builds the target, then cooks); the call returns immediately once launched -- poll get_package_build_status for progress/outcome. Refuses to start a second build while one is already in flight (honest, not silently queued).
//   @param platform - Optional target platform (UAT platform name, e.g. "Win64"). Defaults to "Win64".
//   @param configuration - Optional build configuration ("Debug"|"DebugGame"|"Development"|"Test"|"Shipping"). Defaults to "Development".
//   @param archive_directory - Optional output directory for the archived build. Defaults to <ProjectSavedDir>/Packages/<platform>.
//   @param extra_args - Optional raw extra UAT arguments appended verbatim (e.g. "-distribution -iostore").
//   @returns { build_started: bool, already_running: bool, platform: string, configuration: string, archive_directory: string, command_line: string, note: string }
TSharedPtr<FJsonObject> HandleStartPackageBuild(const TSharedPtr<FJsonObject>& Params)
{
    if (CurrentJob.IsValid())
    {
        FScopeLock Lock(&CurrentJob->StateLock);
        if (!CurrentJob->bFinished)
        {
            TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
            Data->SetBoolField(TEXT("build_started"), false);
            Data->SetBoolField(TEXT("already_running"), true);
            Data->SetStringField(TEXT("platform"), CurrentJob->Platform);
            Data->SetStringField(TEXT("configuration"), CurrentJob->Configuration);
            Data->SetStringField(TEXT("note"),
                TEXT("A package build is already in progress; poll get_package_build_status or cancel_package_build it first."));
            return mcp_common::MakeOkReply(Data);
        }
    }

    FString Platform;
    if (!Params->TryGetStringField(TEXT("platform"), Platform) || Platform.IsEmpty())
    {
        Platform = TEXT("Win64");
    }
    FString Configuration;
    if (!Params->TryGetStringField(TEXT("configuration"), Configuration) || Configuration.IsEmpty())
    {
        Configuration = TEXT("Development");
    }
    FString ArchiveDirectory;
    if (!Params->TryGetStringField(TEXT("archive_directory"), ArchiveDirectory) || ArchiveDirectory.IsEmpty())
    {
        ArchiveDirectory = FPaths::ProjectSavedDir() / TEXT("Packages") / Platform;
    }
    FString ExtraArgs;
    Params->TryGetStringField(TEXT("extra_args"), ExtraArgs);

    const FString UatPath = FSerializedUATProcess::GetUATPath();
    if (!FPaths::FileExists(UatPath))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Unreal Automation Tool (UAT) not found at: %s"), *UatPath));
    }

    const FString ProjectPath = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());

    FString CommandLine = FString::Printf(
        TEXT("BuildCookRun -project=\"%s\" -noP4 -utf8output -platform=%s -clientconfig=%s ")
        TEXT("-cook -allmaps -build -stage -pak -archive -archivedirectory=\"%s\""),
        *ProjectPath, *Platform, *Configuration, *ArchiveDirectory);
    if (!ExtraArgs.IsEmpty())
    {
        CommandLine += TEXT(" ");
        CommandLine += ExtraArgs;
    }

    TSharedPtr<FPackageBuildJob> Job = MakeShared<FPackageBuildJob>();
    Job->Platform = Platform;
    Job->Configuration = Configuration;
    Job->CommandLine = CommandLine;
    Job->StartTime = FDateTime::UtcNow();
    Job->Process = MakeShared<FSerializedUATProcess>(CommandLine);

    TWeakPtr<FPackageBuildJob> WeakJob = Job;

    Job->Process->OnOutput().BindLambda([WeakJob](FString Output)
    {
        if (TSharedPtr<FPackageBuildJob> PinnedJob = WeakJob.Pin())
        {
            FScopeLock Lock(&PinnedJob->StateLock);
            AppendOutputTail(PinnedJob->OutputTail, Output);
        }
    });
    Job->Process->OnCompleted().BindLambda([WeakJob](int32 ReturnCode)
    {
        if (TSharedPtr<FPackageBuildJob> PinnedJob = WeakJob.Pin())
        {
            FScopeLock Lock(&PinnedJob->StateLock);
            PinnedJob->bFinished = true;
            PinnedJob->ExitCode = ReturnCode;
            PinnedJob->EndTime = FDateTime::UtcNow();
        }
    });
    Job->Process->OnCanceled().BindLambda([WeakJob]()
    {
        if (TSharedPtr<FPackageBuildJob> PinnedJob = WeakJob.Pin())
        {
            FScopeLock Lock(&PinnedJob->StateLock);
            PinnedJob->bFinished = true;
            PinnedJob->bCanceled = true;
            PinnedJob->EndTime = FDateTime::UtcNow();
        }
    });
    Job->Process->OnLaunchFailed().BindLambda([WeakJob]()
    {
        if (TSharedPtr<FPackageBuildJob> PinnedJob = WeakJob.Pin())
        {
            FScopeLock Lock(&PinnedJob->StateLock);
            PinnedJob->bFinished = true;
            PinnedJob->bLaunchFailed = true;
            PinnedJob->EndTime = FDateTime::UtcNow();
        }
    });

    CurrentJob = Job;

    // FSerializedUATProcess::Launch() always returns true immediately; failures surface
    // later via OnLaunchFailed (it serializes with any other in-flight UAT process engine-wide).
    Job->Process->Launch();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("build_started"), true);
    Data->SetBoolField(TEXT("already_running"), false);
    Data->SetStringField(TEXT("platform"), Platform);
    Data->SetStringField(TEXT("configuration"), Configuration);
    Data->SetStringField(TEXT("archive_directory"), ArchiveDirectory);
    Data->SetStringField(TEXT("command_line"), CommandLine);
    Data->SetStringField(TEXT("note"),
        TEXT("Build started asynchronously via UAT BuildCookRun. Poll get_package_build_status for progress/outcome."));
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_package_build_status
// ---------------------------------------------------------------------------
// MCP:get_package_build_status - Polls the state of the most recent start_package_build job: running / finished / canceled / launch-failed, with elapsed time and a tail of captured UAT output. Honest has_build:false if no build was ever started this editor session.
//   @returns { has_build: bool, running?: bool, finished?: bool, canceled?: bool, launch_failed?: bool, exit_code?: int, success?: bool, platform?: string, configuration?: string, elapsed_seconds?: number, output_tail?: string }
TSharedPtr<FJsonObject> HandleGetPackageBuildStatus(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    if (!CurrentJob.IsValid())
    {
        Data->SetBoolField(TEXT("has_build"), false);
        return mcp_common::MakeOkReply(Data);
    }

    FScopeLock Lock(&CurrentJob->StateLock);
    const FDateTime Now = FDateTime::UtcNow();
    const FDateTime EndPoint = CurrentJob->bFinished ? CurrentJob->EndTime : Now;
    const double ElapsedSeconds = (EndPoint - CurrentJob->StartTime).GetTotalSeconds();

    Data->SetBoolField(TEXT("has_build"), true);
    Data->SetBoolField(TEXT("running"), !CurrentJob->bFinished);
    Data->SetBoolField(TEXT("finished"), CurrentJob->bFinished);
    Data->SetBoolField(TEXT("canceled"), CurrentJob->bCanceled);
    Data->SetBoolField(TEXT("launch_failed"), CurrentJob->bLaunchFailed);
    Data->SetStringField(TEXT("platform"), CurrentJob->Platform);
    Data->SetStringField(TEXT("configuration"), CurrentJob->Configuration);
    Data->SetNumberField(TEXT("elapsed_seconds"), ElapsedSeconds);
    Data->SetStringField(TEXT("output_tail"), CurrentJob->OutputTail);
    if (CurrentJob->bFinished)
    {
        Data->SetNumberField(TEXT("exit_code"), CurrentJob->ExitCode);
        Data->SetBoolField(TEXT("success"),
            !CurrentJob->bCanceled && !CurrentJob->bLaunchFailed && CurrentJob->ExitCode == 0);
    }
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// cancel_package_build
// ---------------------------------------------------------------------------
// MCP:cancel_package_build - Cancels the in-progress package build, if one is running (honest no-op otherwise). FMonitoredProcess::Cancel(true) kills the whole process tree, since UAT spawns child processes (UBT, the cooker) that would otherwise keep running.
//   @returns { canceled: bool, was_running: bool }
TSharedPtr<FJsonObject> HandleCancelPackageBuild(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    if (!CurrentJob.IsValid())
    {
        Data->SetBoolField(TEXT("canceled"), false);
        Data->SetBoolField(TEXT("was_running"), false);
        return mcp_common::MakeOkReply(Data);
    }

    bool bWasRunning = false;
    {
        FScopeLock Lock(&CurrentJob->StateLock);
        bWasRunning = !CurrentJob->bFinished;
    }

    if (bWasRunning)
    {
        CurrentJob->Process->Cancel(true);
    }

    Data->SetBoolField(TEXT("canceled"), bWasRunning);
    Data->SetBoolField(TEXT("was_running"), bWasRunning);
    return mcp_common::MakeOkReply(Data);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("cancel_package_build"), &HandleCancelPackageBuild },
    { TEXT("get_package_build_status"), &HandleGetPackageBuildStatus },
    { TEXT("start_package_build"), &HandleStartPackageBuild },
};
} // namespace UEAgentPackagingCommands
