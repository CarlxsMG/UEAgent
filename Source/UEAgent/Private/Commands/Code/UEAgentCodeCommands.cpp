// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Code/McpCompilerDiagnostics.h"
#include "Commands/Editor/UEAgentAutomationCommands.h"

#include "GameFramework/Actor.h"
#include "GameProjectUtils.h"
#include "GeneralProjectSettings.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IProjectManager.h"
#include "Misc/App.h"
#include "Misc/CompilationResult.h"
#include "Misc/FileHelper.h"
#include "Misc/HotReloadInterface.h"
#include "Misc/Paths.h"
#include "ModuleDescriptor.h"
#include "Modules/ModuleManager.h"
#include "ProjectDescriptor.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

#if WITH_LIVE_CODING
#include "ILiveCodingModule.h"
#endif

/**
 * The C++ loop of an agent: compile the running editor's modules, read what the compiler said, add a class.
 *
 * Compiling goes through Live Coding (ILiveCodingModule), the editor's own compile button. start_cpp_compile
 * returns once the compile is requested and get_cpp_compile_status polls it; one compile at a time, as in
 * Commands/Packaging. Live Coding has no cancel API, so there is no cancel tool. Hot reload runs only when the
 * caller names it: Live Coding replaced it, and a hot reload can corrupt the defaults of Blueprints built on the
 * reloaded classes.
 *
 * Where the compiler's words are. Live Coding gives the editor a result and nothing else: its console runs
 * UnrealBuildTool as a child process and shows UBT's output in its own window, and neither the editor's log nor
 * the console's log file gets the compiler's lines (measured on 5.7 and 5.8: the editor logs "Live coding failed,
 * please see Live console for more information" under LogLiveCoding; the linker's output does reach LogLiveCoding).
 * UnrealBuildTool writes its whole output, the diagnostics included, to its log file, Log.txt in
 * <user settings>/UnrealBuildTool for an installed engine and Engine/Programs/UnrealBuildTool for a source build;
 * the next UBT run renames it to Log-backup-<date>.txt. So the result comes from LogLiveCoding, and the
 * diagnostics from the UBT log that started after the compile was requested and names this project. Hot reload
 * hands its compiler output to OnModuleCompilerFinished.
 *
 * create_cpp_class writes the header and the source itself instead of calling GameProjectUtils::AddCodeToProject,
 * which regenerates the project files (a blocking UBT run), opens dialogs, and compiles at once through Live
 * Coding or, with Live Coding off, through hot reload. A UCLASS added through Live Coding is not reliable, so the
 * reply says to build and restart.
 */
namespace UEAgentCodeCommands
{

// MCP-DOMAIN: Code

struct FCppCompileJob
{
    FString Mode;
    int64 LogCursorBefore = 0;
    FDateTime StartUtc;
    FDateTime EndUtc;
    /** FPlatformTime::Seconds() when Live Coding stopped compiling without having logged its result yet. */
    double StoppedCompilingAt = 0.0;
    bool bFinished = false;
    FString Result;
    FString Note;
    /** What hot reload's OnModuleCompilerFinished handed over. */
    FString HotReloadOutput;
    /** Filled once, when the job finishes. */
    bool bOutputRead = false;
    FString UbtLogPath;
    FString OutputTail;
    TArray<McpCompilerDiagnostics::FDiagnostic> Errors;
    TArray<McpCompilerDiagnostics::FDiagnostic> Warnings;
};
static TSharedPtr<FCppCompileJob> CurrentCompile;
static FDelegateHandle HotReloadFinishedHandle;

namespace CodeCommandsPrivate
{
    constexpr int32 MaxOutputTailChars = 12000;
    /** How long a Live Coding compile may have stopped without logging its result before it counts as failed. */
    constexpr double ResultGraceSeconds = 30.0;

#if WITH_LIVE_CODING
    ILiveCodingModule* GetLiveCoding()
    {
        return FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);
    }
#endif

    /** Why Live Coding cannot compile now, and what to do; empty when it can. */
    FString LiveCodingBlocker()
    {
#if WITH_LIVE_CODING
        ILiveCodingModule* LiveCoding = GetLiveCoding();
        if (!LiveCoding)
        {
            return TEXT("Live Coding is not available: its module is not loaded in this editor.");
        }
        if (LiveCoding->HasStarted() && LiveCoding->IsEnabledForSession())
        {
            return FString();
        }
        if (!LiveCoding->CanEnableForSession())
        {
            return TEXT("Live Coding cannot be enabled in this session, because modules were hot-reloaded. Close the editor, build the editor target, and reopen it.");
        }
        if (!LiveCoding->GetEnableErrorText().IsEmpty())
        {
            return FString::Printf(TEXT("Live Coding could not start: %s"), *LiveCoding->GetEnableErrorText().ToString());
        }
        if (!LiveCoding->HasStarted())
        {
            return TEXT("Live Coding has not been started in this session (an editor started with -unattended does not start it). Call set_live_coding_enabled with enabled=true.");
        }
        return TEXT("Live Coding is disabled for this session. Call set_live_coding_enabled with enabled=true.");
#else
        return TEXT("Live Coding is not available: this editor was built without it (it exists on Win64 editors).");
#endif
    }

    TSharedPtr<FJsonObject> LiveCodingState()
    {
        TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
        bool bAvailable = false, bEnabled = false, bStarted = false, bCompiling = false;
#if WITH_LIVE_CODING
        if (ILiveCodingModule* LiveCoding = GetLiveCoding())
        {
            bAvailable = true;
            bEnabled = LiveCoding->IsEnabledForSession();
            bStarted = LiveCoding->HasStarted();
            bCompiling = LiveCoding->IsCompiling();
            State->SetBoolField(TEXT("enabled_by_default"), LiveCoding->IsEnabledByDefault());
        }
#endif
        State->SetBoolField(TEXT("available"), bAvailable);
        State->SetBoolField(TEXT("enabled"), bEnabled);
        State->SetBoolField(TEXT("started"), bStarted);
        State->SetBoolField(TEXT("compiling"), bCompiling);
        const FString Blocker = LiveCodingBlocker();
        if (!Blocker.IsEmpty())
        {
            State->SetStringField(TEXT("reason"), Blocker);
        }
        return State;
    }

    /** The result Live Coding logged under LogLiveCoding (LiveCodingModule.cpp), or empty. */
    FString LiveCodingResultOf(const FString& Message)
    {
        if (Message.StartsWith(TEXT("Live coding succeeded, no code changes detected")))
        {
            return TEXT("no_changes");
        }
        if (Message.StartsWith(TEXT("Live coding succeeded")))
        {
            return TEXT("success");
        }
        if (Message.StartsWith(TEXT("Live coding canceled")) || Message.StartsWith(TEXT("Live coding cancelled")))
        {
            return TEXT("cancelled");
        }
        if (Message.StartsWith(TEXT("Live coding failed")))
        {
            return TEXT("failure");
        }
        return FString();
    }

    TArray<UEAgentAutomationCommands::FCapturedLogLine> LiveCodingLines(int64 Since)
    {
        static const FName LogLiveCoding(TEXT("LogLiveCoding"));
        TArray<UEAgentAutomationCommands::FCapturedLogLine> Lines = UEAgentAutomationCommands::PeekLogSince(Since, 4096);
        Lines.RemoveAll([](const UEAgentAutomationCommands::FCapturedLogLine& L) { return L.Category != LogLiveCoding; });
        return Lines;
    }

    FString UbtLogDirectory()
    {
        // Unreal.EngineProgramSavedDirectory in EpicGames.Build: the user settings folder for an installed engine.
        return FApp::IsEngineInstalled()
            ? FPaths::Combine(FPlatformProcess::UserSettingsDir(), TEXT("UnrealBuildTool"))
            : FPaths::Combine(FPaths::EngineDir(), TEXT("Programs"), TEXT("UnrealBuildTool"));
    }

    /** "Log started at <local time> (2026-11-11T12:07:11Z)": the UTC time in the parentheses of the first line. */
    bool ReadUbtLogStart(const FString& Text, FDateTime& OutUtc)
    {
        int32 LineEnd = INDEX_NONE;
        const FString First = Text.FindChar(TEXT('\n'), LineEnd) ? Text.Left(LineEnd) : Text;
        const int32 Open = First.Find(TEXT("("), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
        const int32 Close = First.Find(TEXT(")"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
        return First.StartsWith(TEXT("Log started at")) && Open != INDEX_NONE && Close > Open
            && FDateTime::ParseIso8601(*First.Mid(Open + 1, Close - Open - 1), OutUtc);
    }

    /**
     * The UBT log of the compile requested at StartUtc: the earliest UBT log that started after it and names this
     * project. Empty when there is none (UBT not run, or its log already deleted).
     */
    FString FindUbtLog(const FDateTime& StartUtc, FString& OutText)
    {
        const FString Dir = UbtLogDirectory();
        TArray<FString> Names;
        IFileManager::Get().FindFiles(Names, *FPaths::Combine(Dir, TEXT("Log*.txt")), /*Files*/ true, /*Directories*/ false);

        FString Project = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
        FPaths::NormalizeFilename(Project);
        const FTimespan Slack = FTimespan::FromSeconds(5.0);

        FString Best;
        FDateTime BestStart = FDateTime::MaxValue();
        for (const FString& Name : Names)
        {
            if (Name != TEXT("Log.txt") && !Name.StartsWith(TEXT("Log-backup-")))
            {
                continue;
            }
            const FString Path = FPaths::Combine(Dir, Name);
            if (IFileManager::Get().GetTimeStamp(*Path) < StartUtc - Slack)
            {
                continue;
            }
            FString Text;
            FDateTime LogStart;
            if (!FFileHelper::LoadFileToString(Text, *Path) || !ReadUbtLogStart(Text, LogStart)
                || LogStart < StartUtc - Slack || LogStart >= BestStart)
            {
                continue;
            }
            if (!Text.Replace(TEXT("\\"), TEXT("/")).Contains(Project, ESearchCase::IgnoreCase))
            {
                continue;
            }
            Best = Path;
            BestStart = LogStart;
            OutText = MoveTemp(Text);
        }
        return Best;
    }

    /** The lines of a UBT log worth showing: from its first action on, without .NET stack frames. */
    FString UbtLogTail(const FString& Text)
    {
        TArray<FString> Lines;
        Text.ParseIntoArrayLines(Lines, /*bCullEmpty*/ true);
        int32 First = 0;
        for (int32 i = 0; i < Lines.Num(); ++i)
        {
            if (Lines[i].StartsWith(TEXT("[1/")) || Lines[i].Contains(TEXT("Building ")) || Lines[i].Contains(TEXT("Exited with error code")))
            {
                First = i;
                break;
            }
        }
        FString Out;
        for (int32 i = First; i < Lines.Num(); ++i)
        {
            if (!Lines[i].TrimStart().StartsWith(TEXT("at ")))
            {
                Out += Lines[i];
                Out += TEXT("\n");
            }
        }
        return Out;
    }

    FString CapTail(const FString& Text)
    {
        return Text.Len() > MaxOutputTailChars ? Text.Right(MaxOutputTailChars) : Text;
    }

    /** Reads what the finished job's compile printed, once. */
    void ReadJobOutput(FCppCompileJob& Job)
    {
        if (Job.bOutputRead)
        {
            return;
        }
        Job.bOutputRead = true;

        FString Output;
        for (const UEAgentAutomationCommands::FCapturedLogLine& Line : LiveCodingLines(Job.LogCursorBefore))
        {
            Output += Line.Message;
            Output += TEXT("\n");
        }
        if (Job.Mode == TEXT("hot_reload"))
        {
            Output += Job.HotReloadOutput;
        }
        else
        {
            FString UbtText;
            Job.UbtLogPath = FindUbtLog(Job.StartUtc, UbtText);
            if (!Job.UbtLogPath.IsEmpty())
            {
                Output += FString::Printf(TEXT("--- UnrealBuildTool log %s ---\n"), *Job.UbtLogPath);
                Output += UbtLogTail(UbtText);
            }
        }
        McpCompilerDiagnostics::Parse(Output, Job.Errors, Job.Warnings);
        Job.OutputTail = CapTail(Output);
    }

    void FinishJob(FCppCompileJob& Job, const FString& Result)
    {
        Job.bFinished = true;
        Job.Result = Result;
        Job.EndUtc = FDateTime::UtcNow();
        ReadJobOutput(Job);
    }

    /** Brings the job up to date with what Live Coding has logged. */
    void UpdateJob(FCppCompileJob& Job)
    {
        if (Job.bFinished || Job.Mode != TEXT("live_coding"))
        {
            return;
        }
        for (const UEAgentAutomationCommands::FCapturedLogLine& Line : LiveCodingLines(Job.LogCursorBefore))
        {
            const FString Result = LiveCodingResultOf(Line.Message);
            if (!Result.IsEmpty())
            {
                FinishJob(Job, Result);
                return;
            }
        }
#if WITH_LIVE_CODING
        // The result is logged on the game thread's next Live Coding tick after the compile ends.
        ILiveCodingModule* LiveCoding = GetLiveCoding();
        if (LiveCoding && LiveCoding->IsCompiling())
        {
            Job.StoppedCompilingAt = 0.0;
            return;
        }
#endif
        const double Now = FPlatformTime::Seconds();
        if (Job.StoppedCompilingAt == 0.0)
        {
            Job.StoppedCompilingAt = Now;
        }
        else if (Now - Job.StoppedCompilingAt > ResultGraceSeconds)
        {
            Job.Note = TEXT("Live Coding stopped compiling without logging a result; the log may have been cleared or the console closed.");
            FinishJob(Job, TEXT("failure"));
        }
    }

    TArray<TSharedPtr<FJsonValue>> DiagnosticsJson(const TArray<McpCompilerDiagnostics::FDiagnostic>& Diagnostics, int32 Max)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        for (int32 i = 0; i < Diagnostics.Num() && i < Max; ++i)
        {
            const McpCompilerDiagnostics::FDiagnostic& D = Diagnostics[i];
            TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
            Obj->SetStringField(TEXT("file"), D.File);
            Obj->SetNumberField(TEXT("line"), D.Line);
            Obj->SetNumberField(TEXT("column"), D.Column);
            Obj->SetStringField(TEXT("code"), D.Code);
            Obj->SetStringField(TEXT("message"), D.Message);
            Out.Add(MakeShared<FJsonValueObject>(Obj));
        }
        return Out;
    }

    void OnHotReloadFinished(const FString& Output, ECompilationResult::Type Result, bool /*bShowLog*/)
    {
        if (!CurrentCompile.IsValid() || CurrentCompile->bFinished || CurrentCompile->Mode != TEXT("hot_reload"))
        {
            return;
        }
        CurrentCompile->HotReloadOutput = Output;
        FinishJob(*CurrentCompile,
            Result == ECompilationResult::Succeeded ? TEXT("success")
            : Result == ECompilationResult::UpToDate ? TEXT("no_changes")
            : Result == ECompilationResult::Canceled ? TEXT("cancelled")
            : TEXT("failure"));
    }

    TSharedPtr<FJsonObject> StartedReply(const FCppCompileJob& Job, const FString& Note)
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetBoolField(TEXT("compile_started"), true);
        Data->SetBoolField(TEXT("already_running"), false);
        Data->SetStringField(TEXT("mode"), Job.Mode);
        Data->SetNumberField(TEXT("log_cursor_before"), static_cast<double>(Job.LogCursorBefore));
        Data->SetStringField(TEXT("note"), Note);
        return mcp_common::MakeOkReply(Data);
    }

    TSharedPtr<FJsonObject> AlreadyRunningReply(const FString& Mode, const FString& Note)
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetBoolField(TEXT("compile_started"), false);
        Data->SetBoolField(TEXT("already_running"), true);
        Data->SetStringField(TEXT("mode"), Mode);
        Data->SetStringField(TEXT("note"), Note);
        return mcp_common::MakeOkReply(Data);
    }

    TSharedPtr<FCppCompileJob> NewJob(const TCHAR* Mode)
    {
        TSharedPtr<FCppCompileJob> Job = MakeShared<FCppCompileJob>();
        Job->Mode = Mode;
        Job->LogCursorBefore = UEAgentAutomationCommands::GetLogCursor();
        Job->StartUtc = FDateTime::UtcNow();
        return Job;
    }

    TSharedPtr<FJsonObject> StartLiveCoding(bool bExplicit)
    {
#if WITH_LIVE_CODING
        ILiveCodingModule* LiveCoding = GetLiveCoding();
        if (!LiveCoding)
        {
            return mcp_common::MakeErrorReply(LiveCodingBlocker());
        }
        const FString Blocker = LiveCodingBlocker();
        // An explicit live_coding starts it as the editor's compile button does; auto only uses it when it runs.
        if (!Blocker.IsEmpty() && (!bExplicit || !LiveCoding->CanEnableForSession()))
        {
            return mcp_common::MakeErrorReply(Blocker
                + TEXT(" mode='live_coding' starts Live Coding for this session and compiles. mode='hot_reload' compiles without")
                + TEXT(" Live Coding, but it can corrupt the defaults of Blueprints built on the reloaded classes, so it is never chosen for you."));
        }
        if (LiveCoding->IsCompiling())
        {
            return AlreadyRunningReply(TEXT("live_coding"),
                TEXT("Live Coding is already compiling (started outside this tool, or by an earlier call); nothing was started. Poll get_cpp_compile_status."));
        }

        TSharedPtr<FCppCompileJob> Job = NewJob(TEXT("live_coding"));
        ELiveCodingCompileResult Result = ELiveCodingCompileResult::NotStarted;
        LiveCoding->Compile(ELiveCodingCompileFlags::None, &Result);
        switch (Result)
        {
        case ELiveCodingCompileResult::InProgress:
            CurrentCompile = Job;
            return StartedReply(*Job, TEXT("Live Coding compile requested. Poll get_cpp_compile_status until finished is true; a patch reinstances live objects of the changed classes."));
        case ELiveCodingCompileResult::CompileStillActive:
            return AlreadyRunningReply(TEXT("live_coding"), TEXT("Live Coding is still busy with an earlier compile; nothing was started."));
        case ELiveCodingCompileResult::NotStarted:
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Live Coding did not start, so nothing was compiled. %s"), *LiveCodingBlocker()));
        default:
            // Compile() waits only when asked to; any other answer is a finished compile.
            CurrentCompile = Job;
            FinishJob(*Job, Result == ELiveCodingCompileResult::Success ? TEXT("success")
                : Result == ELiveCodingCompileResult::NoChanges ? TEXT("no_changes")
                : Result == ELiveCodingCompileResult::Cancelled ? TEXT("cancelled") : TEXT("failure"));
            return StartedReply(*Job, TEXT("Live Coding finished at once; read get_cpp_compile_status."));
        }
#else
        return mcp_common::MakeErrorReply(LiveCodingBlocker());
#endif
    }

    TSharedPtr<FJsonObject> StartHotReload()
    {
#if WITH_LIVE_CODING
        if (ILiveCodingModule* LiveCoding = GetLiveCoding())
        {
            if (LiveCoding->HasStarted())
            {
                return mcp_common::MakeErrorReply(TEXT("Live Coding has started in this session, and hot reload cannot run alongside it. Use mode='live_coding' (or 'auto')."));
            }
        }
#endif
        IHotReloadInterface* HotReload = FModuleManager::LoadModulePtr<IHotReloadInterface>(TEXT("HotReload"));
        if (!HotReload)
        {
            return mcp_common::MakeErrorReply(TEXT("The HotReload module is not available in this editor."));
        }
        if (HotReload->IsCurrentlyCompiling())
        {
            return AlreadyRunningReply(TEXT("hot_reload"), TEXT("Hot reload is already compiling; nothing was started."));
        }
        if (!HotReload->IsAnyGameModuleLoaded())
        {
            return mcp_common::MakeErrorReply(TEXT("No game module of this project is loaded, so hot reload has nothing to compile."));
        }
        if (!HotReloadFinishedHandle.IsValid())
        {
            HotReloadFinishedHandle = HotReload->OnModuleCompilerFinished().AddStatic(&OnHotReloadFinished);
        }
        TSharedPtr<FCppCompileJob> Job = NewJob(TEXT("hot_reload"));
        CurrentCompile = Job;
        HotReload->DoHotReloadFromEditor(EHotReloadFlags::None);
        return StartedReply(*Job, TEXT("Hot reload started. Poll get_cpp_compile_status. Hot reload can corrupt the defaults of Blueprints built on the reloaded classes; check them, or restart the editor."));
    }
}

// ---------------------------------------------------------------------------
// start_cpp_compile
// ---------------------------------------------------------------------------
// MCP:start_cpp_compile - Compile the C++ modules of the running editor and patch them in, as the editor's Live Coding compile button does; returns as soon as the compile is requested, so poll get_cpp_compile_status for the result and the compiler's errors.
//   @para One compile at a time: a call while one runs answers already_running true and starts nothing. Live Coding has no cancel. A patch reinstances live objects of the changed classes, so it is refused during Play-in-Editor without force.
//   @para mode "auto" (default) uses Live Coding when it has started in this session and refuses otherwise with the reason; "live_coding" also starts Live Coding first, like the button; "hot_reload" compiles without Live Coding, only when Live Coding has not started, and can corrupt the defaults of Blueprints built on the reloaded classes, so auto never picks it.
//   @param mode - Optional "auto" | "live_coding" | "hot_reload". Default "auto".
//   @returns { compile_started: bool, already_running: bool, mode: string, log_cursor_before?: int, note: string }
//   @hints code-exec destructive not-idempotent
TSharedPtr<FJsonObject> HandleStartCppCompile(const TSharedPtr<FJsonObject>& Params)
{
    FString Mode = TEXT("auto");
    Params->TryGetStringField(TEXT("mode"), Mode);
    Mode = Mode.TrimStartAndEnd().ToLower();
    if (Mode != TEXT("auto") && Mode != TEXT("live_coding") && Mode != TEXT("hot_reload"))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Unknown mode '%s': use \"auto\", \"live_coding\" or \"hot_reload\"."), *Mode));
    }

    if (CurrentCompile.IsValid())
    {
        CodeCommandsPrivate::UpdateJob(*CurrentCompile);
        if (!CurrentCompile->bFinished)
        {
            return CodeCommandsPrivate::AlreadyRunningReply(CurrentCompile->Mode,
                TEXT("A compile started by start_cpp_compile is still running; nothing was started. Poll get_cpp_compile_status."));
        }
    }

    return Mode == TEXT("hot_reload") ? CodeCommandsPrivate::StartHotReload()
        : CodeCommandsPrivate::StartLiveCoding(Mode == TEXT("live_coding"));
}

// ---------------------------------------------------------------------------
// get_cpp_compile_status
// ---------------------------------------------------------------------------
// MCP:get_cpp_compile_status - The state of the last compile start_cpp_compile requested in this editor session: running or finished, the result, and the compiler's errors and warnings with file, line and column, plus the state of Live Coding. With no compile started it answers has_compile false and still reports live_coding.
//   @para result is "success", "no_changes", "failure" or "cancelled" once finished. errors and warnings come from the UnrealBuildTool log of that compile (Live Coding's console keeps the compiler output out of the editor log), or from hot reload's output; output_tail holds what they were read from, and ubt_log_path the file, for the lines the parser does not know.
//   @param max_results - Optional cap on the entries of errors and of warnings. Default 100.
//   @returns { has_compile: bool, running?: bool, finished?: bool, result?: string, mode?: string, errors?: [{file, line, column, code, message}], warnings?: [{file, line, column, code, message}], error_count?: int, warning_count?: int, elapsed_seconds?: number, output_tail?: string, ubt_log_path?: string, log_cursor_before?: int, note?: string, live_coding: {available, enabled, started, compiling, enabled_by_default?, reason?} }
//   @hints read-only
TSharedPtr<FJsonObject> HandleGetCppCompileStatus(const TSharedPtr<FJsonObject>& Params)
{
    int32 MaxResults = 100;
    {
        double Tmp = 0.0;
        if (Params->TryGetNumberField(TEXT("max_results"), Tmp) && Tmp >= 0.0)
        {
            MaxResults = FMath::Clamp(static_cast<int32>(Tmp), 0, 10000);
        }
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetObjectField(TEXT("live_coding"), CodeCommandsPrivate::LiveCodingState());
    if (!CurrentCompile.IsValid())
    {
        Data->SetBoolField(TEXT("has_compile"), false);
        Data->SetStringField(TEXT("result"), TEXT("not_started"));
        return mcp_common::MakeOkReply(Data);
    }

    FCppCompileJob& Job = *CurrentCompile;
    CodeCommandsPrivate::UpdateJob(Job);
    const FDateTime End = Job.bFinished ? Job.EndUtc : FDateTime::UtcNow();

    Data->SetBoolField(TEXT("has_compile"), true);
    Data->SetBoolField(TEXT("running"), !Job.bFinished);
    Data->SetBoolField(TEXT("finished"), Job.bFinished);
    Data->SetStringField(TEXT("mode"), Job.Mode);
    Data->SetNumberField(TEXT("elapsed_seconds"), (End - Job.StartUtc).GetTotalSeconds());
    Data->SetNumberField(TEXT("log_cursor_before"), static_cast<double>(Job.LogCursorBefore));
    if (Job.bFinished)
    {
        Data->SetStringField(TEXT("result"), Job.Result);
        Data->SetArrayField(TEXT("errors"), CodeCommandsPrivate::DiagnosticsJson(Job.Errors, MaxResults));
        Data->SetArrayField(TEXT("warnings"), CodeCommandsPrivate::DiagnosticsJson(Job.Warnings, MaxResults));
        Data->SetNumberField(TEXT("error_count"), Job.Errors.Num());
        Data->SetNumberField(TEXT("warning_count"), Job.Warnings.Num());
        Data->SetStringField(TEXT("output_tail"), Job.OutputTail);
        if (!Job.UbtLogPath.IsEmpty())
        {
            Data->SetStringField(TEXT("ubt_log_path"), Job.UbtLogPath);
        }
        FString Note = Job.Note;
        if (Job.Result == TEXT("failure") && Job.Errors.Num() == 0)
        {
            Note += (Note.IsEmpty() ? TEXT("") : TEXT(" "));
            Note += Job.UbtLogPath.IsEmpty() && Job.Mode == TEXT("live_coding")
                ? TEXT("No UnrealBuildTool log of this compile was found, so the errors are only in the Live Coding console window.")
                : TEXT("The compile failed without a diagnostic the parser reads; see output_tail.");
        }
        if (!Note.IsEmpty())
        {
            Data->SetStringField(TEXT("note"), Note);
        }
    }
    else
    {
        Data->SetStringField(TEXT("result"), TEXT("running"));
    }
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_live_coding_enabled
// ---------------------------------------------------------------------------
// MCP:set_live_coding_enabled - Turn Live Coding on or off for this editor session, as the Live Coding toggle does; on starts its console the first time, which start_cpp_compile needs. The project's default (Editor Preferences) is not changed.
//   @param enabled - Required. true to enable Live Coding for this session, false to disable it.
//   @returns { enabled: bool, started: bool, note: string }
//   @hints idempotent
TSharedPtr<FJsonObject> HandleSetLiveCodingEnabled(const TSharedPtr<FJsonObject>& Params)
{
    bool bEnable = false;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredBool(Params, TEXT("enabled"), bEnable))
    {
        return Missing;
    }
#if WITH_LIVE_CODING
    ILiveCodingModule* LiveCoding = CodeCommandsPrivate::GetLiveCoding();
    if (!LiveCoding)
    {
        return mcp_common::MakeErrorReply(CodeCommandsPrivate::LiveCodingBlocker());
    }
    if (bEnable && !LiveCoding->CanEnableForSession())
    {
        return mcp_common::MakeErrorReply(CodeCommandsPrivate::LiveCodingBlocker());
    }
    LiveCoding->EnableForSession(bEnable);

    const bool bEnabled = LiveCoding->IsEnabledForSession();
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("enabled"), bEnabled);
    Data->SetBoolField(TEXT("started"), LiveCoding->HasStarted());
    FString Note;
    if (bEnabled != bEnable)
    {
        Note = CodeCommandsPrivate::LiveCodingBlocker();
    }
    else if (!bEnable && LiveCoding->HasStarted())
    {
        Note = TEXT("Live Coding is disabled for this session; its console keeps running until the editor closes.");
    }
    else
    {
        Note = bEnable ? TEXT("Live Coding is enabled for this session; start_cpp_compile can compile.") : TEXT("Live Coding is disabled for this session.");
    }
    Data->SetStringField(TEXT("note"), Note);
    return mcp_common::MakeOutcomeReply(Data, bEnabled == bEnable, Note);
#else
    return mcp_common::MakeErrorReply(CodeCommandsPrivate::LiveCodingBlocker());
#endif
}

namespace CodeCommandsPrivate
{
    /** The parent class the caller named: "Actor", "AActor", "/Script/Engine.Actor". */
    UClass* ResolveParentClass(const FString& Name)
    {
        if (Name.Contains(TEXT(".")) || Name.StartsWith(TEXT("/")))
        {
            return FindObject<UClass>(nullptr, *Name);
        }
        if (UClass* Found = UClass::TryFindTypeSlow<UClass>(Name, EFindFirstObjectOptions::ExactClass))
        {
            return Found;
        }
        if (Name.Len() > 1 && (Name[0] == TEXT('U') || Name[0] == TEXT('A')) && FChar::IsUpper(Name[1]))
        {
            return UClass::TryFindTypeSlow<UClass>(Name.Mid(1), EFindFirstObjectOptions::ExactClass);
        }
        return nullptr;
    }

    /** sub_path inside the module: relative, without "..", with forward slashes and no outer slashes. */
    bool CleanSubPath(FString In, FString& Out)
    {
        In.ReplaceInline(TEXT("\\"), TEXT("/"));
        In.TrimStartAndEndInline();
        while (In.StartsWith(TEXT("/")))
        {
            In.RightChopInline(1);
        }
        while (In.EndsWith(TEXT("/")))
        {
            In.LeftChopInline(1);
        }
        TArray<FString> Parts;
        In.ParseIntoArray(Parts, TEXT("/"), /*bCullEmpty*/ true);
        for (const FString& Part : Parts)
        {
            if (Part == TEXT("..") || Part == TEXT(".") || Part.Contains(TEXT(":")))
            {
                return false;
            }
        }
        Out = FString::Join(Parts, TEXT("/"));
        return true;
    }

    FString ModuleNames(const TArray<FModuleContextInfo>& Modules)
    {
        TArray<FString> Names;
        for (const FModuleContextInfo& M : Modules)
        {
            Names.Add(M.ModuleName);
        }
        return FString::Join(Names, TEXT(", "));
    }
}

// ---------------------------------------------------------------------------
// create_cpp_class
// ---------------------------------------------------------------------------
// MCP:create_cpp_class - Write the header and source of a new UCLASS into one of the project's C++ modules, derived from a native class (default Object). It writes the two files and nothing else: no project-file regeneration, no compile, no reload.
//   @para Refuses a class name already in use (in memory or as a header of the module), an unknown module, a parent that is unknown, a Blueprint class, an interface or not exported to other modules, and a project without a C++ module (a Blueprint-only project gets its first module from the editor's New C++ Class). Refused during Play-in-Editor without force.
//   @para The class is usable only once the module is built: needs_restart is true because a new UCLASS patched in by Live Coding is not reliable. Close the editor, build the editor target, and reopen it.
//   @param class_name - Required. The class name without its U or A prefix, e.g. "MyActor".
//   @param parent_class - Optional native parent class: "Object", "Actor", "ActorComponent", "AActor", or a path like "/Script/Engine.Actor". Default "Object".
//   @param module_name - Optional project module to add the class to. Default the primary game module (the one named after the project).
//   @param class_location - Optional "public" (header under Public, exported with MODULE_API) | "private" (both files under Private). Default "public". A module without Public and Private folders keeps both files in its folder.
//   @param sub_path - Optional folder under Public/Private (or the module folder), e.g. "Gameplay/Items".
//   @returns { created: bool, class_name: string, parent_class: string, module_name: string, header_path: string, source_path: string, needs_compile: bool, needs_restart: bool, note: string }
//   @hints additive
TSharedPtr<FJsonObject> HandleCreateCppClass(const TSharedPtr<FJsonObject>& Params)
{
    FString ClassName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("class_name"), ClassName))
    {
        return Missing;
    }
    ClassName.TrimStartAndEndInline();
    FString ParentName = TEXT("Object");
    Params->TryGetStringField(TEXT("parent_class"), ParentName);
    ParentName.TrimStartAndEndInline();
    if (ParentName.IsEmpty())
    {
        ParentName = TEXT("Object");
    }
    FString ModuleName;
    Params->TryGetStringField(TEXT("module_name"), ModuleName);
    ModuleName.TrimStartAndEndInline();
    FString Location = TEXT("public");
    Params->TryGetStringField(TEXT("class_location"), Location);
    Location = Location.TrimStartAndEnd().ToLower();
    if (Location.IsEmpty())
    {
        Location = TEXT("public");
    }
    if (Location != TEXT("public") && Location != TEXT("private"))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Unknown class_location '%s': use \"public\" or \"private\"."), *Location));
    }
    FString SubPath;
    {
        FString RawSubPath;
        Params->TryGetStringField(TEXT("sub_path"), RawSubPath);
        if (!CodeCommandsPrivate::CleanSubPath(RawSubPath, SubPath))
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("sub_path '%s' must be a relative folder inside the module, without '..'."), *RawSubPath));
        }
    }

    // The project and its module.
    const FProjectDescriptor* Project = IProjectManager::Get().GetCurrentProject();
    if (!Project || Project->Modules.Num() == 0 || !GameProjectUtils::ProjectHasCodeFiles())
    {
        return mcp_common::MakeErrorReply(TEXT("This project has no C++ module (it is Blueprint-only), and create_cpp_class adds a class to an existing module. Add the first class from the editor (Tools > New C++ Class), which creates the module; then build and restart."));
    }
    const TArray<FModuleContextInfo>& Modules = GameProjectUtils::GetCurrentProjectModules();
    const FModuleContextInfo* Module = nullptr;
    if (!ModuleName.IsEmpty())
    {
        Module = Modules.FindByPredicate([&](const FModuleContextInfo& M) { return M.ModuleName.Equals(ModuleName, ESearchCase::IgnoreCase); });
        if (!Module)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Unknown module '%s'. The project's C++ modules: %s."), *ModuleName, *CodeCommandsPrivate::ModuleNames(Modules)));
        }
    }
    else
    {
        Module = Modules.FindByPredicate([](const FModuleContextInfo& M) { return M.ModuleName == FApp::GetProjectName(); });
        if (!Module)
        {
            Module = Modules.FindByPredicate([](const FModuleContextInfo& M) { return M.ModuleType == EHostType::Runtime; });
        }
        if (!Module && Modules.Num() > 0)
        {
            Module = &Modules[0];
        }
        if (!Module)
        {
            return mcp_common::MakeErrorReply(TEXT("The project lists modules, but none has a .Build.cs under Source/."));
        }
    }

    // The parent.
    UClass* Parent = CodeCommandsPrivate::ResolveParentClass(ParentName);
    if (!Parent)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Unknown parent class '%s'. Name a native class, e.g. \"Object\", \"Actor\" or \"/Script/Engine.Actor\"."), *ParentName));
    }
    if (!Parent->HasAnyClassFlags(CLASS_Native) || !GameProjectUtils::IsValidBaseClassForCreation(Parent, *Module))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("'%s' cannot be the parent of a C++ class in module %s: it is a Blueprint class, an interface, or not exported to other modules (no MODULE_API or MinimalAPI)."),
            *Parent->GetPathName(), *Module->ModuleName));
    }
    FString ParentInclude = Parent == UObject::StaticClass() ? FString(TEXT("UObject/Object.h")) : Parent->GetMetaData(TEXT("IncludePath"));
    if (ParentInclude.IsEmpty())
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("The header of '%s' is not known (its class has no IncludePath metadata), so the new header could not include it."), *Parent->GetPathName()));
    }

    // The name.
    FText Reason;
    if (!GameProjectUtils::IsValidClassNameForCreation(ClassName, *Module, TSet<FString>(), Reason))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("class_name '%s' is refused: %s"), *ClassName, *Reason.ToString()));
    }

    // Where the files go.
    const FString ModuleDir = Module->ModuleSourcePath;
    const bool bSplitLayout = IFileManager::Get().DirectoryExists(*FPaths::Combine(ModuleDir, TEXT("Public")))
        || IFileManager::Get().DirectoryExists(*FPaths::Combine(ModuleDir, TEXT("Private")));
    const bool bPublic = Location == TEXT("public");
    FString Base = bSplitLayout ? FPaths::Combine(ModuleDir, bPublic ? TEXT("Public") : TEXT("Private")) : ModuleDir;
    if (!SubPath.IsEmpty())
    {
        Base = FPaths::Combine(Base, SubPath);
    }
    FString HeaderDir, SourceDir;
    if (!GameProjectUtils::CalculateSourcePaths(Base, *Module, HeaderDir, SourceDir, &Reason))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("No valid place for the files in module %s: %s"), *Module->ModuleName, *Reason.ToString()));
    }
    const FString HeaderPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(HeaderDir, ClassName + TEXT(".h")));
    const FString SourcePath = FPaths::ConvertRelativePathToFull(FPaths::Combine(SourceDir, ClassName + TEXT(".cpp")));
    if (IFileManager::Get().FileExists(*HeaderPath) || IFileManager::Get().FileExists(*SourcePath))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("A file of that name is already there: %s or %s."), *HeaderPath, *SourcePath));
    }

    // The include of the header from the source: next to it, or relative to the include root it sits under.
    FString HeaderInclude = ClassName + TEXT(".h");
    if (FPaths::GetPath(HeaderPath) != FPaths::GetPath(SourcePath))
    {
        FString Rel = HeaderPath;
        for (const TCHAR* Root : { TEXT("Public"), TEXT("Classes"), TEXT("Private") })
        {
            const FString RootDir = FPaths::ConvertRelativePathToFull(FPaths::Combine(ModuleDir, Root)) + TEXT("/");
            if (Rel.StartsWith(RootDir))
            {
                Rel.RightChopInline(RootDir.Len());
                break;
            }
        }
        HeaderInclude = Rel;
    }

    const FString Prefix = Parent->IsChildOf(AActor::StaticClass()) ? TEXT("A") : TEXT("U");
    const FString CppName = Prefix + ClassName;
    const FString ParentCppName = FString(Parent->GetPrefixCPP()) + Parent->GetName();
    const FString ApiMacro = bPublic && bSplitLayout ? Module->ModuleName.ToUpper() + TEXT("_API ") : FString();
    const FString Copyright = GetDefault<UGeneralProjectSettings>()->CopyrightNotice.TrimStartAndEnd();
    const FString CopyrightLine = Copyright.IsEmpty() ? FString() : FString::Printf(TEXT("// %s\n\n"), *Copyright);

    const FString HeaderText = FString::Printf(
        TEXT("%s#pragma once\n\n#include \"CoreMinimal.h\"\n#include \"%s\"\n#include \"%s.generated.h\"\n\nUCLASS()\nclass %s%s : public %s\n{\n\tGENERATED_BODY()\n};\n"),
        *CopyrightLine, *ParentInclude, *ClassName, *ApiMacro, *CppName, *ParentCppName);
    const FString SourceText = FString::Printf(TEXT("%s#include \"%s\"\n"), *CopyrightLine, *HeaderInclude);

    const bool bHeaderWritten = FFileHelper::SaveStringToFile(HeaderText, *HeaderPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    const bool bSourceWritten = bHeaderWritten && FFileHelper::SaveStringToFile(SourceText, *SourcePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    if (bHeaderWritten && !bSourceWritten)
    {
        IFileManager::Get().Delete(*HeaderPath);
    }

    // Measured: both files are on disk and declare the class.
    FString ReadHeader, ReadSource;
    const bool bCreated = FFileHelper::LoadFileToString(ReadHeader, *HeaderPath) && FFileHelper::LoadFileToString(ReadSource, *SourcePath)
        && ReadHeader.Contains(CppName) && ReadSource.Contains(HeaderInclude);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("created"), bCreated);
    Data->SetStringField(TEXT("class_name"), CppName);
    Data->SetStringField(TEXT("parent_class"), Parent->GetPathName());
    Data->SetStringField(TEXT("module_name"), Module->ModuleName);
    Data->SetStringField(TEXT("header_path"), HeaderPath);
    Data->SetStringField(TEXT("source_path"), SourcePath);
    Data->SetBoolField(TEXT("needs_compile"), bCreated);
    Data->SetBoolField(TEXT("needs_restart"), bCreated);
    FString Note = bCreated
        ? TEXT("The class exists on disk only; project files were not regenerated. A new UCLASS patched in by Live Coding is not reliable: close the editor, build the editor target, and reopen it.")
        : TEXT("The files could not be written.");
    if (bCreated && !bSplitLayout)
    {
        Note += TEXT(" The module has no Public or Private folder, so both files are in its folder.");
    }
    Data->SetStringField(TEXT("note"), Note);
    return mcp_common::MakeOutcomeReply(Data, bCreated, mcp_common::MeasuredOutcomeError(TEXT("create_cpp_class"), TEXT("created")));
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("create_cpp_class"), &HandleCreateCppClass },
    { TEXT("get_cpp_compile_status"), &HandleGetCppCompileStatus },
    { TEXT("set_live_coding_enabled"), &HandleSetLiveCodingEnabled },
    { TEXT("start_cpp_compile"), &HandleStartCppCompile },
};
} // namespace UEAgentCodeCommands
