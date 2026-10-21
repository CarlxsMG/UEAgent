// Copyright (c) 2026 CarlxsMG.

#include "Commands/Editor/UEAgentAutomationCommands.h"
#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Misc/EngineVersion.h"
#include "Misc/AutomationTest.h"
#include "Misc/AutomationEvent.h" // FAutomationEvent / EAutomationEventType (entry serialization)
#include "Misc/Paths.h"
#include "Misc/App.h"             // FApp::CanEverRender() (null-RHI / headless gate)
#include "CoreGlobals.h"          // GIsAutomationTesting / IsRunningCommandlet()
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// PythonScriptPlugin is an editor-only plugin module. Guard all Python usage with
// WITH_EDITOR so non-editor builds still compile, and add a runtime availability
// check so a disabled/unloaded plugin returns an honest error rather than crashing.
#include "IPythonScriptPlugin.h"
#include "PythonScriptTypes.h"
// Functional Test authoring (PIE-based gameplay tests).
#include "FunctionalTest.h"
#include "Engine/Blueprint.h"
#include "EngineUtils.h"  // TActorIterator (Editor-side)
#include "Factories/BlueprintFactory.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "LevelEditor.h"  // ULevelEditorSubsystem (EditorPlaySimulate / StopPlayInEditor)
#include "LevelEditorSubsystem.h"  // ULevelEditorSubsystem explicit (LevelEditor/Public/)
// MCP-DOMAIN: Editor

#include "Misc/OutputDevice.h"

/**
 * Python execution, automation tests (listing, running, Functional Test authoring) and the editor
 * output-log capture.
 *
 * Python commands route through IPythonScriptPlugin (plugin "PythonScriptPlugin"); GetReadyPython()
 * checks that the module is loaded and IsPythonAvailable(), so a missing/disabled plugin returns an
 * honest error instead of crashing. Automation-test listing/running uses FAutomationTestFramework
 * (Core / Misc/AutomationTest.h).
 *
 * Test execution is crash-safe: StopTest() asserts (hard check()) that the latent-command queue is
 * empty, so a multi-tick test (PIE/load-map/wait) is detected via IsLatentCommandQueueEmpty(), its
 * queue is drained with DequeueAllCommands() before StopTest(), and it is reported as completed=false.
 */
namespace UEAgentAutomationCommands
{

// Log capture device backing get_output_log_lines. A single FOutputDevice registered
// once on GLog (never a second
// one -- GLog fans the same Serialize() call out to every registered device, so a second
// instance would double-capture every line for no benefit). Entries are a bounded ring
// buffer keyed by a monotonic SequenceId; Peek() is NON-DESTRUCTIVE (same
// since_sequence_id/cursor idiom used elsewhere in the toolset) so two independent commands
// (or repeated calls to the same one) can each track their own read position without
// stealing lines from each other.
struct FMcpLogEntry
{
    int64 SequenceId = 0;
    double Timestamp = 0.0; // FPlatformTime::Seconds() at capture (process-relative, monotonic)
    ELogVerbosity::Type Verbosity = ELogVerbosity::Log;
    FName Category;
    FString Message;
};

static const TCHAR* McpVerbosityToString(ELogVerbosity::Type Verbosity)
{
    switch (Verbosity)
    {
        case ELogVerbosity::Fatal:       return TEXT("Fatal");
        case ELogVerbosity::Error:       return TEXT("Error");
        case ELogVerbosity::Warning:     return TEXT("Warning");
        case ELogVerbosity::Display:     return TEXT("Display");
        case ELogVerbosity::Log:         return TEXT("Log");
        case ELogVerbosity::Verbose:     return TEXT("Verbose");
        case ELogVerbosity::VeryVerbose: return TEXT("VeryVerbose");
        default:                        return TEXT("?");
    }
}

/** Parse an engine_log_level name into the verbosity THRESHOLD it means (inclusive).
 *  "all"/"log" keeps everything; anything unrecognised falls back to Warning, which is the
 *  default and the level that actually explains a silent failure. */
static ELogVerbosity::Type McpVerbosityFromString(const FString& In)
{
    if (In.Equals(TEXT("Error"), ESearchCase::IgnoreCase))   { return ELogVerbosity::Error; }
    if (In.Equals(TEXT("Fatal"), ESearchCase::IgnoreCase))   { return ELogVerbosity::Fatal; }
    if (In.Equals(TEXT("Display"), ESearchCase::IgnoreCase)) { return ELogVerbosity::Display; }
    if (In.Equals(TEXT("Log"), ESearchCase::IgnoreCase)
     || In.Equals(TEXT("All"), ESearchCase::IgnoreCase))     { return ELogVerbosity::Log; }
    return ELogVerbosity::Warning;
}

class FUEAgentLogCaptureDevice : public FOutputDevice
{
public:
    virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const class FName& Category) override
    {
        FScopeLock Lock(&CaptureMutex);
        FMcpLogEntry Entry;
        Entry.SequenceId = ++LastSequenceId;
        Entry.Timestamp = FPlatformTime::Seconds();
        Entry.Verbosity = Verbosity;
        Entry.Category = Category;
        Entry.Message = FString(V);
        Entries.Add(MoveTemp(Entry));
        // Bounded ring buffer: oldest entries fall off regardless of read state (this is
        // capacity management, not a "drain" -- Peek() never removes anything itself).
        if (Entries.Num() > MaxLines)
        {
            Entries.RemoveAt(0, Entries.Num() - MaxLines, EAllowShrinking::No);
        }
    }

    /** Non-destructive read: entries with SequenceId > SinceSequenceId, optionally filtered
     *  by message substring / verbosity name / category substring, capped at MaxResults
     *  (earliest-first, so a capped read never skips over older unread lines). */
    TArray<FMcpLogEntry> Peek(int64 SinceSequenceId, const FString& Filter,
        const FString& LevelFilter, const FString& CategoryFilter, int32 MaxResults) const
    {
        FScopeLock Lock(&CaptureMutex);
        TArray<FMcpLogEntry> Result;
        for (const FMcpLogEntry& E : Entries)
        {
            if (E.SequenceId <= SinceSequenceId) { continue; }
            if (!Filter.IsEmpty() && !E.Message.Contains(Filter, ESearchCase::IgnoreCase)) { continue; }
            if (!LevelFilter.IsEmpty() && !FString(McpVerbosityToString(E.Verbosity)).Equals(LevelFilter, ESearchCase::IgnoreCase)) { continue; }
            if (!CategoryFilter.IsEmpty() && !E.Category.ToString().Contains(CategoryFilter, ESearchCase::IgnoreCase)) { continue; }
            Result.Add(E);
            if (Result.Num() >= MaxResults) { break; }
        }
        return Result;
    }

    /** Every entry newer than SinceSequenceId whose verbosity is at least MaxVerbosity
     *  (ELogVerbosity is ordered Fatal=1 < Error < Warning < Display < Log, so "at least
     *  Warning" is `Verbosity <= Warning`). Separate from Peek() on purpose: Peek()'s
     *  LevelFilter is an EXACT name match, which cannot express a threshold, and its
     *  MaxResults break happens BEFORE filtering -- so asking it for "the 50 worst lines"
     *  would silently return the first 50 lines of any level and drop the error behind them.
     *  Scans the whole bounded buffer (4096 max) under the lock, then caps. */
    TArray<FMcpLogEntry> PeekByVerbosity(int64 SinceSequenceId, ELogVerbosity::Type MaxVerbosity,
        int32 MaxResults, bool& bOutTruncated) const
    {
        TArray<FMcpLogEntry> Result;
        bOutTruncated = false;
        // MaxResults == 0 means "caller disabled this", which is NOT the same as "there was
        // more and we cut it": returning truncated=true here would read as a warning about
        // hidden lines on every call that opted out.
        if (MaxResults <= 0) { return Result; }

        FScopeLock Lock(&CaptureMutex);
        for (const FMcpLogEntry& E : Entries)
        {
            if (E.SequenceId <= SinceSequenceId) { continue; }
            // MASK before comparing: ELogVerbosity::Type carries flags in the high bits
            // (BreakOnLog = 0x80, SetColor = 0x40), and an unmasked compare would read a
            // flagged Error as a huge number and silently DROP the very line we are here for.
            if ((E.Verbosity & ELogVerbosity::VerbosityMask) > MaxVerbosity) { continue; }
            if (Result.Num() >= MaxResults) { bOutTruncated = true; break; }
            Result.Add(E);
        }
        return Result;
    }

    /** Number of held lines (for diagnostics). */
    int32 Size() const
    {
        FScopeLock Lock(&CaptureMutex);
        return Entries.Num();
    }

    /** Highest SequenceId currently assigned (0 if nothing captured yet); the cursor a
     *  caller should pass as since_sequence_id on its next call to read only new lines. */
    int64 GetLastSequenceId() const
    {
        FScopeLock Lock(&CaptureMutex);
        return LastSequenceId;
    }

    static constexpr int32 MaxLines = 4096;

private:
    mutable FCriticalSection CaptureMutex;
    TArray<FMcpLogEntry> Entries;
    int64 LastSequenceId = 0;
};

static FUEAgentLogCaptureDevice GMCP_LogCapture;

// The last completed run_automation_test, so get_automation_test_results can return it without re-running.
// bHasLastResult answers an honest "no test has been run yet" before any synchronous test has completed.
static FAutomationTestExecutionInfo LastTestExecutionInfo;
static FString LastTestName;
static bool bHasLastResult = false;

void StartLogCapture()
{
    if (GLog)
    {
        GLog->AddOutputDevice(&GMCP_LogCapture);
    }
}

void StopLogCapture()
{
    if (GLog)
    {
        GLog->RemoveOutputDevice(&GMCP_LogCapture);
    }
}

namespace
{
    /**
     * FAutomationTestFramework defaults to
     * RequestedTestFilter = SmokeFilter (AutomationTest.cpp), so its
     * GetValidTestNames() / ContainsTest() filter out EngineFilter/ProductFilter/etc
     * tests before the bridge ever sees them. Without an override, MCP can only
     * invoke SmokeFilter tests via run_automation_test -- functional tests and any
     * EditorFilter/ProductFilter test the caller wants to run all become invisible.
     *
     * The framework has no public getter for RequestedTestFilter so we cannot
     * save/restore via TGuardValue. We do a one-way set in the bridge's
     * list/run handlers. Side-effect: the Session Frontend UI also sees the
     * full FilterMask rather than just SmokeFilter. This is the *correct* state
     * for MCP-driven testing; if other tooling needs smoke-only filtering it
     * can call SetRequestedTestFilter(SmokeFilter) itself.
     */
    void EnsureAutomationFilterMask()
    {
        FAutomationTestFramework::Get().SetRequestedTestFilter(EAutomationTestFlags_FilterMask);
    }
}

namespace
{
    /** Resolve the Python plugin or return nullptr with an honest reason filled in. */
    IPythonScriptPlugin* GetReadyPython(FString& OutReason)
    {
        IPythonScriptPlugin* Py = IPythonScriptPlugin::Get();
        if (!Py)
        {
            OutReason = TEXT("PythonScriptPlugin module is not loaded (plugin disabled?). Cannot execute Python.");
            return nullptr;
        }
        if (!Py->IsPythonAvailable())
        {
            OutReason = TEXT("Python is not available/initialized in this editor session (IsPythonAvailable()==false).");
            return nullptr;
        }
        return Py;
    }

    /** Serialize the captured Python log entries into a JSON array + a single combined string. */
    void HarvestLogOutput(const FPythonCommandEx& Cmd, TArray<TSharedPtr<FJsonValue>>& OutEntries, FString& OutCombined)
    {
        for (const FPythonLogOutputEntry& Entry : Cmd.LogOutput)
        {
            TSharedPtr<FJsonObject> LineObj = MakeShared<FJsonObject>();
            LineObj->SetStringField(TEXT("type"), LexToString(Entry.Type));
            LineObj->SetStringField(TEXT("output"), Entry.Output);
            OutEntries.Add(MakeShared<FJsonValueObject>(LineObj));

            if (!OutCombined.IsEmpty())
            {
                OutCombined += TEXT("\n");
            }
            OutCombined += Entry.Output;
        }
    }

    /**
     * Attach the ENGINE-side log emitted during a Python call.
     *
     * WHY THIS EXISTS: `log_output` above is NOT the log of
     * the call. It is `FPythonCommandEx::LogOutput`, which UE fills from
     * PyCore::GetPythonLogCapture() -- a delegate broadcast ONLY by unreal.log /
     * log_warning / log_error (PyCore.cpp/1207/1227). It is a Python-side hook, not an
     * FOutputDevice on GLog, so every UE_LOG emitted by C++ during the call is DROPPED.
     * That is how `build_meta_human` can run, return None, write zero assets and report a
     * clean `log_output`, while the cause -- "LogMetaHumanCoreTechLib: Error: Texture
     * Synthesis has not been initialized" -- sits in GLog the whole time. A field that claims
     * to be the log and is silently partial is exactly the kind of output this plugin refuses to trust.
     *
     * The plugin already owns an FOutputDevice on GLog (GMCP_LogCapture, backing
     * get_output_log_lines), so this is a cursor read around the call, not a new capture.
     * Emitted as a SEPARATE field, so `log_output` stays exactly what its consumers
     * expect.
     */
    void AttachEngineLog(const TSharedPtr<FJsonObject>& Result, const TSharedPtr<FJsonObject>& Params,
        int64 CursorBefore)
    {
        FString LevelStr;
        Params->TryGetStringField(TEXT("engine_log_level"), LevelStr);
        const ELogVerbosity::Type Threshold = McpVerbosityFromString(LevelStr);

        int32 MaxLines = 50;
        {
            double Tmp = 0.0;
            if (Params->TryGetNumberField(TEXT("engine_log_max"), Tmp) && Tmp >= 0.0)
            {
                MaxLines = FMath::Clamp(static_cast<int32>(Tmp), 0, FUEAgentLogCaptureDevice::MaxLines);
            }
        }

        bool bTruncated = false;
        const TArray<FMcpLogEntry> Lines =
            GMCP_LogCapture.PeekByVerbosity(CursorBefore, Threshold, MaxLines, bTruncated);

        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Reserve(Lines.Num());
        for (const FMcpLogEntry& E : Lines)
        {
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetNumberField(TEXT("sequence_id"), static_cast<double>(E.SequenceId));
            Entry->SetStringField(TEXT("level"), McpVerbosityToString(E.Verbosity));
            Entry->SetStringField(TEXT("category"), E.Category.ToString());
            Entry->SetStringField(TEXT("message"), E.Message);
            Arr.Add(MakeShared<FJsonValueObject>(Entry));
        }

        Result->SetArrayField(TEXT("engine_log"), Arr);
        Result->SetNumberField(TEXT("engine_log_count"), Arr.Num());
        Result->SetBoolField(TEXT("engine_log_truncated"), bTruncated);
        // The cursor the caller passes to get_output_log_lines(since_sequence_id=...) to read
        // the FULL, unfiltered engine log of this call -- so a filtered view always says how
        // to widen itself instead of reading as "that was everything".
        Result->SetNumberField(TEXT("log_cursor_before"), static_cast<double>(CursorBefore));
    }
}

namespace
{
    /** Map the automation event type to a stable lowercase-friendly string (no LexToString in Core for this enum). */
    const TCHAR* AutomationEventTypeToString(EAutomationEventType Type)
    {
        switch (Type)
        {
        case EAutomationEventType::Info:    return TEXT("Info");
        case EAutomationEventType::Warning: return TEXT("Warning");
        case EAutomationEventType::Error:   return TEXT("Error");
        default:                            return TEXT("Unknown");
        }
    }

    /**
     * Serialize a completed FAutomationTestExecutionInfo into the canonical result shape
     * shared by run_automation_test and get_automation_test_results.
     * Contract: { test_name, started, completed, passed, verdict,
     *                  duration_s, errors, warnings,
     *                  entries:[{type,message,context,filename,line}], entry_count }
     * verdict is "pass" when passed=true else "fail" -- single key the LLM can switch
     * on without reading bools. errors/warnings/duration_s are the canonical names;
     * the multi-tick skip branches in RunAutomationTestCommon already use this shape.
     */
    TSharedPtr<FJsonObject> SerializeExecutionInfo(const FString& TestName, const FAutomationTestExecutionInfo& Info)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("test_name"), TestName);
        Result->SetBoolField(TEXT("started"), true);
        Result->SetBoolField(TEXT("completed"), true);
        Result->SetBoolField(TEXT("passed"), Info.bSuccessful);
        Result->SetStringField(TEXT("verdict"), Info.bSuccessful ? TEXT("pass") : TEXT("fail"));
        Result->SetNumberField(TEXT("duration_s"), Info.Duration);
        Result->SetNumberField(TEXT("errors"), Info.GetErrorTotal());
        Result->SetNumberField(TEXT("warnings"), Info.GetWarningTotal());

        TArray<TSharedPtr<FJsonValue>> EntriesArray;
        for (const FAutomationExecutionEntry& Entry : Info.GetEntries())
        {
            TSharedPtr<FJsonObject> EntryObj = MakeShared<FJsonObject>();
            EntryObj->SetStringField(TEXT("type"), AutomationEventTypeToString(Entry.Event.Type));
            EntryObj->SetStringField(TEXT("message"), Entry.Event.Message);
            EntryObj->SetStringField(TEXT("context"), Entry.Event.Context);
            EntryObj->SetStringField(TEXT("filename"), Entry.Filename);
            EntryObj->SetNumberField(TEXT("line"), Entry.LineNumber);
            EntriesArray.Add(MakeShared<FJsonValueObject>(EntryObj));
        }
        Result->SetArrayField(TEXT("entries"), EntriesArray);
        Result->SetNumberField(TEXT("entry_count"), EntriesArray.Num());

        return Result;
    }

    // The result of a test this environment cannot run: never started, verdict "skipped", and why.
    TSharedPtr<FJsonObject> Automation_SkippedResult(const FString& TestName, const TCHAR* Note)
    {
        TSharedPtr<FJsonObject> Skipped = MakeShared<FJsonObject>();
        Skipped->SetStringField(TEXT("test_name"), TestName);
        Skipped->SetBoolField(TEXT("started"), false);
        Skipped->SetBoolField(TEXT("completed"), false);
        Skipped->SetStringField(TEXT("verdict"), TEXT("skipped"));
        Skipped->SetStringField(TEXT("note"), Note);
        return Skipped;
    }

    // The flow of run_automation_test, an in-process single-callback FAutomationTestFramework
    // runner: contains-check + headless-gate + StartTestByName + ExecuteLatentCommands +
    // IsLatentCommandQueueEmpty + StopTest + cache.
    //
    // OutError: nullptr on hard error (no recovery, error response filled in); non-null on skipped
    // (test cannot run here) or on a synchronous pass/fail verdict.
    //
    // Cache: the helper updates LastTestName / LastTestExecutionInfo / bHasLastResult for callers
    // that pass a valid cache (out of ref). When OutCache is nullptr (e.g. caller doesn't want the
    // cache update), the helper still runs the test but skips the cache write.
    struct FAutomationRunCache
    {
        FString* LastTestName = nullptr;
        FAutomationTestExecutionInfo* LastTestExecutionInfo = nullptr;
        bool* bHasLastResult = nullptr;
    };
    TSharedPtr<FJsonObject> RunAutomationTestCommon(const FString& InTestName, TSharedPtr<FJsonObject>& OutError, FAutomationRunCache* OutCache = nullptr)
    {
        OutError.Reset();

        FAutomationTestFramework& Framework = FAutomationTestFramework::Get();

        // See HandleListAutomationTests: ensure non-smoke tests are
        // surfaced both for the listing and for ContainsTest() below.
        EnsureAutomationFilterMask();

        Framework.LoadTestModules();

        // The framework keys tests by their test_name (the class name for a simple test), but
        // list_automation_tests leads with full_test_path ("UEAgent.ClaudeCode"), the name the Session
        // Frontend and `Automation RunTests` use: that one is accepted too, and resolved to its test_name.
        FString TestName = InTestName;
        if (!Framework.ContainsTest(TestName))
        {
            TArray<FAutomationTestInfo> TestInfos;
            Framework.GetValidTestNames(TestInfos);
            if (const FAutomationTestInfo* ByPath = TestInfos.FindByPredicate(
                [&InTestName](const FAutomationTestInfo& Info) { return Info.GetFullTestPath() == InTestName; }))
            {
                TestName = ByPath->GetTestName();
            }
        }

        if (!Framework.ContainsTest(TestName))
        {
            OutError = mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Automation test not found: '%s' (use list_automation_tests for valid names)."), *TestName));
            return nullptr;
        }

        // Honest headless gate: refuse tests that genuinely cannot run in this environment.
        {
            TArray<FAutomationTestInfo> TestInfos;
            Framework.GetValidTestNames(TestInfos);
            const FAutomationTestInfo* Found = TestInfos.FindByPredicate(
                [&TestName](const FAutomationTestInfo& Info) { return Info.GetTestName() == TestName; });
            if (Found)
            {
                const EAutomationTestFlags Flags = Found->GetTestFlags();
                if (EnumHasAnyFlags(Flags, EAutomationTestFlags::RequiresUser))
                {
                    return Automation_SkippedResult(TestName,
                        TEXT("test requires an interactive user (RequiresUser flag); skipped in headless/automation context"));
                }
                if (EnumHasAnyFlags(Flags, EAutomationTestFlags::NonNullRHI) && !FApp::CanEverRender())
                {
                    return Automation_SkippedResult(TestName,
                        TEXT("test requires a real RHI (NonNullRHI flag) but this editor cannot render (nullrhi/headless); skipped"));
                }
            }
        }

        // Start the test. RoleIndex 0 = the local role. StartTestByName self-cleans any
        // abandoned prior run when GIsAutomationTesting is already true.
        Framework.StartTestByName(TestName, 0);

        if (!GIsAutomationTesting)
        {
            OutError = mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Automation test '%s' could not start (editor busy: slow task or PIE world active). See Output Log."), *TestName));
            return nullptr;
        }

        Framework.ExecuteLatentCommands();
        if (!Framework.IsLatentCommandQueueEmpty())
        {
            // Multi-tick test (PIE / load-map / wait): its head latent command returns
            // false-until-next-frame and can NEVER drain in a single game-thread callback.
            // Calling StopTest here would check-fail on the non-empty queue. Drain + discard
            // StopTest to reset GIsAutomationTesting for the next command.
            Framework.DequeueAllCommands();
            FAutomationTestExecutionInfo Discarded;
            Framework.StopTest(Discarded);

            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("test_name"), TestName);
            Result->SetBoolField(TEXT("started"), true);
            Result->SetBoolField(TEXT("completed"), false);
            Result->SetNumberField(TEXT("errors"), 0);
            Result->SetNumberField(TEXT("warnings"), 0);
            Result->SetNumberField(TEXT("duration_s"), 0.0);
            Result->SetStringField(TEXT("verdict"), TEXT("skipped"));
            Result->SetStringField(TEXT("note"),
                TEXT("test requires latent ticks (PIE/load-map/wait); single-call runner only supports synchronous tests. Use run_functional_test_in_pie + get_output_log_lines for PIE-driven multi-tick orchestration."));
            return Result;
        }

        // Synchronous path: the queue is empty, so StopTest is safe and the execution info is final.
        FAutomationTestExecutionInfo ExecInfo;
        Framework.StopTest(ExecInfo);
        if (OutCache && OutCache->LastTestName && OutCache->LastTestExecutionInfo && OutCache->bHasLastResult)
        {
            *OutCache->LastTestName = TestName;
            *OutCache->LastTestExecutionInfo = ExecInfo;
            *OutCache->bHasLastResult = true;
        }
        return SerializeExecutionInfo(TestName, ExecInfo);
    }
}

// MCP:execute_python - Execute Python code inside the running Unreal Editor. DO NOT GUESS A FIELD NAME: most of this tool's errors are "object has no attribute" -- and the field usually EXISTS, just not where you reached for it: `HitResult().impact_point` raises, `get_editor_property` cannot find it, and only `to_dict()` returns it (Transform.translation is the reverse). The engine's own stub and Epic's web docs list it either way, so neither settles it -- ask the object (dir(x), x.to_dict(), x.get_editor_property(...)). PIE TRAP -- this tool is the ONE HOLE in the plugin's PIE guard: the commands that compile a Blueprint or write to disk are REFUSED while Play-in-Editor is live (McpPieSafety.gen.h), but arbitrary Python cannot be classified statically, so a script that reaches compile/save through unreal.* (BlueprintEditorLibrary.compile_blueprint, EditorAssetLibrary.save_asset, save_loaded_asset, anything that dirties a Blueprint) RUNS ANYWAY. With PIE live that ends either in a DEAD EDITOR -- the compiler's UREINST_* trampoline retains a world and CheckForWorldGCLeaks is Fatal, taking every unsaved edit with it -- or in a save that reports success without writing a byte (Editor.AllowSavingAssetsDuringPIE=0). So: call is_play_in_editor_active FIRST whenever your script compiles, saves, or mutates a Blueprint, and stop PIE before running it. READING the live world during PIE is safe and is exactly what this tool is for -- do not avoid it for that. BEFORE writing a loop, check what already answers it in ONE call: scripts that re-implement something a tool already does fail far more often than scripts doing something genuinely new -- hand-rolled scaffolding is where the errors are, and a bug in the scaffold reads exactly like a finding about the game. enumerate actors of a class -> get_actors_by_class. enumerate them AND read their properties, filtered by class or editor label, drilling into one component, with the answering world STAMPED on the result (this tool cannot tell you which world answered) -> get_actor_properties_by_class. read properties off ONE known object -> get_properties. list an actor's components -> get_actor_components. one line trace -> raycast_scene. sample a property across a whole PIE session ARMED BEFORE BeginPlay -- a window a post-tick callback cannot reach at all -> watch_pie_property. But do NOT read this as "always prefer the specific tool": only a minority of scripts are replaceable end-to-end. Python remains the right answer when the script does what those cannot: several classes in one round trip, walking components AND calling a method, filtering by name substring, correlating two values in the same tick, or anything that mutates. Do not trade one call for twelve. WARNING: `log_output` IS NOT THE LOG OF THE CALL -- it is Python's own prints only (UE fills it from a delegate broadcast solely by unreal.log/log_warning/log_error), so every UE_LOG the ENGINE emits during your script is absent from it. Read `engine_log` instead: it is the GLog slice bracketed to this call, Warning+Error by default. Example: build_meta_human can run, return None, write zero assets and report a clean `log_output` while "LogMetaHumanCoreTechLib: Error: Texture Synthesis has not been initialized" sits in GLog. When a call reports success and NOTHING HAPPENED, `engine_log` is the first place to look; widen with engine_log_level=Log, or pass `log_cursor_before` to get_output_log_lines(since_sequence_id=...) for the unfiltered rest.
//   @param command - Python code to run (single or multi-line).
//   @param capture_output - Run as a file and collect captured log (default true)
//   @param mode - Execution mode: ExecuteFile, ExecuteStatement, or EvaluateStatement (default ExecuteFile)
//   @param engine_log_level - Minimum severity of ENGINE log to return in `engine_log`: Error, Warning (default), Display, Log/All.
//   @param engine_log_max - Max lines in `engine_log` (default 50, 0 disables, hard cap 4096).
//   @returns { command, python_success, execution_mode, command_result, output, log_output, log_line_count, engine_log, engine_log_count, engine_log_truncated, log_cursor_before }
TSharedPtr<FJsonObject> HandleExecutePython(const TSharedPtr<FJsonObject>& Params)
{
    FString Command;
    if (!Params->TryGetStringField(TEXT("command"), Command) || Command.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required 'command' (Python code to run)."));
    }

    FString Reason;
    IPythonScriptPlugin* Py = GetReadyPython(Reason);
    if (!Py)
    {
        return mcp_common::MakeErrorReply(Reason);
    }

    // Default capture_output = true. When true we run the command as a *file* so multi-line
    // statements (e.g. "import unreal\nprint(...)") work, and we collect the captured log.
    bool bCaptureOutput = true;
    Params->TryGetBoolField(TEXT("capture_output"), bCaptureOutput);

    // Optional explicit mode: "ExecuteFile" (default; allows multi-statement scripts),
    // "ExecuteStatement" (single statement, prints result),
    // "EvaluateStatement" (single expression, CommandResult is the value).
    FString ModeStr;
    EPythonCommandExecutionMode Mode = EPythonCommandExecutionMode::ExecuteFile;
    if (Params->TryGetStringField(TEXT("mode"), ModeStr) && !ModeStr.IsEmpty())
    {
        LexFromString(Mode, *ModeStr);
    }

    FPythonCommandEx Cmd;
    Cmd.Command = Command;
    Cmd.ExecutionMode = Mode;
    Cmd.FileExecutionScope = EPythonFileExecutionScope::Public; // share console globals so 'import unreal' etc. persist
    Cmd.Flags = EPythonCommandFlags::Unattended;                // suppress UI prompts when run headless

    // Cursor BEFORE the call: everything GLog receives from here on belongs to this command.
    const int64 CursorBefore = GMCP_LogCapture.GetLastSequenceId();

    const bool bSuccess = Py->ExecPythonCommandEx(Cmd);

    TArray<TSharedPtr<FJsonValue>> LogEntries;
    FString CombinedLog;
    HarvestLogOutput(Cmd, LogEntries, CombinedLog);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("command"), Command);
    Result->SetBoolField(TEXT("python_success"), bSuccess);
    Result->SetStringField(TEXT("execution_mode"), LexToString(Mode));
    // CommandResult: for EvaluateStatement = the value; otherwise None on success,
    // or the Python exception trace on failure.
    Result->SetStringField(TEXT("command_result"), Cmd.CommandResult);
    Result->SetStringField(TEXT("output"), CombinedLog);
    Result->SetArrayField(TEXT("log_output"), LogEntries);
    Result->SetNumberField(TEXT("log_line_count"), LogEntries.Num());
    AttachEngineLog(Result, Params, CursorBefore);

    return mcp_common::MakeOkReply(Result);
}

// MCP:execute_python_file - Execute a Python (.py) file inside the running Unreal Editor. PIE TRAP: same hole as execute_python -- arbitrary Python is NOT covered by the plugin's PIE guard, so a script that compiles a Blueprint or saves an asset runs even with Play-in-Editor live, where compiling can KILL the editor (UREINST_* retains a world, CheckForWorldGCLeaks is Fatal) and saving reports success without writing (Editor.AllowSavingAssetsDuringPIE=0). Check is_play_in_editor_active first when the file compiles, saves or mutates a Blueprint; reading the live world during PIE is safe. WARNING: `log_output` IS NOT THE LOG OF THE CALL -- it is Python's own prints only (UE fills it from a delegate broadcast solely by unreal.log/log_warning/log_error), so every UE_LOG the ENGINE emits during your script is absent from it. Read `engine_log` instead: it is the GLog slice bracketed to this call, Warning+Error by default. Example: build_meta_human can run, return None, write zero assets and report a clean `log_output` while "LogMetaHumanCoreTechLib: Error: Texture Synthesis has not been initialized" sits in GLog. When a call reports success and NOTHING HAPPENED, `engine_log` is the first place to look; widen with engine_log_level=Log, or pass `log_cursor_before` to get_output_log_lines(since_sequence_id=...) for the unfiltered rest.
//   @param file_path - Absolute path to a .py file accessible from the editor host.
//   @param engine_log_level - Minimum severity of ENGINE log to return in `engine_log`: Error, Warning (default), Display, Log/All.
//   @param engine_log_max - Max lines in `engine_log` (default 50, 0 disables, hard cap 4096).
//   @returns { file_path, python_success, command_result, output, log_output, log_line_count, engine_log, engine_log_count, engine_log_truncated, log_cursor_before }
TSharedPtr<FJsonObject> HandleExecutePythonFile(const TSharedPtr<FJsonObject>& Params)
{
    FString FilePath;
    if (!Params->TryGetStringField(TEXT("file_path"), FilePath) || FilePath.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required 'file_path' (path to a .py file)."));
    }

    if (!FPaths::FileExists(FilePath))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Python file does not exist: %s"), *FilePath));
    }

    FString Reason;
    IPythonScriptPlugin* Py = GetReadyPython(Reason);
    if (!Py)
    {
        return mcp_common::MakeErrorReply(Reason);
    }

    // ExecuteFile mode treats Command as a file (with optional arguments) to run.
    FPythonCommandEx Cmd;
    Cmd.Command = FilePath;
    Cmd.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
    Cmd.FileExecutionScope = EPythonFileExecutionScope::Public;
    Cmd.Flags = EPythonCommandFlags::Unattended;

    // Cursor BEFORE the call -- see AttachEngineLog: `log_output` carries only what the
    // script itself printed, never the engine's own UE_LOG.
    const int64 CursorBefore = GMCP_LogCapture.GetLastSequenceId();

    const bool bSuccess = Py->ExecPythonCommandEx(Cmd);

    TArray<TSharedPtr<FJsonValue>> LogEntries;
    FString CombinedLog;
    HarvestLogOutput(Cmd, LogEntries, CombinedLog);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("file_path"), FilePath);
    Result->SetBoolField(TEXT("python_success"), bSuccess);
    Result->SetStringField(TEXT("command_result"), Cmd.CommandResult);
    Result->SetStringField(TEXT("output"), CombinedLog);
    Result->SetArrayField(TEXT("log_output"), LogEntries);
    Result->SetNumberField(TEXT("log_line_count"), LogEntries.Num());
    AttachEngineLog(Result, Params, CursorBefore);

    return mcp_common::MakeOkReply(Result);
}

// MCP:get_python_status - Reports whether the editor Python plugin is loaded, available, configured and initialized, with the interpreter path and engine version; when Python is initialized it also evaluates sys.version for python_version (empty otherwise). Changes nothing; check it before execute_python or execute_python_file.
//   @returns { engine_version, module_loaded, python_available, python_configured, python_initialized, interpreter_path, python_version }
TSharedPtr<FJsonObject> HandleGetPythonStatus(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());

    IPythonScriptPlugin* Py = IPythonScriptPlugin::Get();
    const bool bModuleLoaded = (Py != nullptr);
    Result->SetBoolField(TEXT("module_loaded"), bModuleLoaded);

    bool bAvailable = false;
    bool bConfigured = false;
    bool bInitialized = false;
    FString InterpreterPath;
    FString PythonVersion;

    if (Py)
    {
        bAvailable = Py->IsPythonAvailable();
        bConfigured = Py->IsPythonConfigured();
        bInitialized = Py->IsPythonInitialized();
        InterpreterPath = Py->GetInterpreterExecutablePath();

        // If Python is initialized, ask it for its own version string via an evaluated
        // statement. Honest: if this fails we just leave PythonVersion empty.
        if (bAvailable && bInitialized)
        {
            FPythonCommandEx VerCmd;
            VerCmd.Command = TEXT("__import__('sys').version.replace(chr(10), ' ')");
            VerCmd.ExecutionMode = EPythonCommandExecutionMode::EvaluateStatement;
            VerCmd.FileExecutionScope = EPythonFileExecutionScope::Public;
            VerCmd.Flags = EPythonCommandFlags::Unattended;
            if (Py->ExecPythonCommandEx(VerCmd))
            {
                // CommandResult for EvaluateStatement holds the repr of the value (quoted string).
                PythonVersion = VerCmd.CommandResult.TrimStartAndEnd().TrimQuotes();
            }
        }
    }

    Result->SetBoolField(TEXT("python_available"), bAvailable);
    Result->SetBoolField(TEXT("python_configured"), bConfigured);
    Result->SetBoolField(TEXT("python_initialized"), bInitialized);
    Result->SetStringField(TEXT("interpreter_path"), InterpreterPath);
    Result->SetStringField(TEXT("python_version"), PythonVersion);

    return mcp_common::MakeOkReply(Result);
}

// MCP:list_automation_tests - List the automation tests registered in this editor without running any: per test its display name, full_test_path, test_name, tags, parameter, source file and line, and participants required. Engine, product and performance tests are listed too, not only smoke tests: the call widens the framework's test filter to every flag and loads the modules that register tests, but changes no asset or level. Tests are returned in registry order up to max_results; total_count counts every registered test, matched_count those the filter matches, and truncated is true when matches were cut off, so narrow the filter or raise max_results. Use it to find the name for run_automation_test; run_smoke_tests runs the smoke set and get_automation_test_results re-reads the last result.
//   @param filter - Optional case-insensitive substring matched against the full test path, e.g. 'UEAgent.' or 'Project.Functional'; empty lists every test.
//   @param max_results - Most tests to return, a positive number (default 500); 0 or a negative value also means 500.
//   @returns { display_name, full_test_path, test_name, test_tags, test_parameter, source_file, source_file_line, num_participants_required, tests, total_count, matched_count, returned_count, max_results, truncated, filter }
TSharedPtr<FJsonObject> HandleListAutomationTests(const TSharedPtr<FJsonObject>& Params)
{
    // Optional case-insensitive substring filter applied to the full test path.
    FString Filter;
    Params->TryGetStringField(TEXT("filter"), Filter);

    // Optional cap on returned entries (the registry can contain thousands of tests).
    int32 MaxResults = 500;
    {
        double MaxD = 0.0;
        if (Params->TryGetNumberField(TEXT("max_results"), MaxD) && MaxD > 0.0)
        {
            MaxResults = static_cast<int32>(MaxD);
        }
    }

    FAutomationTestFramework& Framework = FAutomationTestFramework::Get();

    // Ensure EngineFilter/ProductFilter/PerfFilter/etc tests are visible
    // (framework default RequestedTestFilter = SmokeFilter masks them).
    EnsureAutomationFilterMask();

    // Ensure modules that register tests but aren't loaded by default are loaded so
    // the listing is complete. This is read-only w.r.t. test execution.
    Framework.LoadTestModules();

    TArray<FAutomationTestInfo> TestInfos;
    Framework.GetValidTestNames(TestInfos);

    const int32 TotalCount = TestInfos.Num();

    TArray<TSharedPtr<FJsonValue>> TestsArray;
    int32 MatchedCount = 0;
    bool bTruncated = false;

    for (const FAutomationTestInfo& Info : TestInfos)
    {
        const FString FullPath = Info.GetFullTestPath();
        if (!Filter.IsEmpty() && !FullPath.Contains(Filter, ESearchCase::IgnoreCase))
        {
            continue;
        }

        ++MatchedCount;

        if (TestsArray.Num() >= MaxResults)
        {
            bTruncated = true;
            continue;
        }

        TSharedPtr<FJsonObject> TestObj = MakeShared<FJsonObject>();
        TestObj->SetStringField(TEXT("display_name"), Info.GetDisplayName());
        TestObj->SetStringField(TEXT("full_test_path"), FullPath);
        TestObj->SetStringField(TEXT("test_name"), Info.GetTestName());
        TestObj->SetStringField(TEXT("test_tags"), Info.GetTestTags());
        TestObj->SetStringField(TEXT("test_parameter"), Info.GetTestParameter());
        TestObj->SetStringField(TEXT("source_file"), Info.GetSourceFile());
        TestObj->SetNumberField(TEXT("source_file_line"), Info.GetSourceFileLine());
        TestObj->SetNumberField(TEXT("num_participants_required"), Info.GetNumParticipantsRequired());
        TestsArray.Add(MakeShared<FJsonValueObject>(TestObj));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("tests"), TestsArray);
    Result->SetNumberField(TEXT("total_count"), TotalCount);
    Result->SetNumberField(TEXT("matched_count"), MatchedCount);
    Result->SetNumberField(TEXT("returned_count"), TestsArray.Num());
    Result->SetNumberField(TEXT("max_results"), MaxResults);
    Result->SetBoolField(TEXT("truncated"), bTruncated);
    Result->SetStringField(TEXT("filter"), Filter);

    return mcp_common::MakeOkReply(Result);
}

// MCP:run_automation_test - Run ONE registered automation test in this editor, synchronously, and return its verdict (pass, fail or skipped) with its errors, warnings and log entries. Find the name first with list_automation_tests; re-read the last result later without running anything with get_automation_test_results; run every smoke test at once with run_smoke_tests. A test that needs several ticks (PIE, a map load, a wait) is not run here but answered verdict skipped with the reason -- drive it with run_functional_test_in_pie -- and so is one that needs an interactive user or a real renderer in a headless editor; an editor busy with a slow task or PIE refuses the start. Marked destructive and not idempotent because the test's own code runs in the editor and can create, change or delete assets and actors as it was written to, so two runs need not do the same; this tool itself saves nothing and adds no undo step for what the test did.
//   @param test_name - The test's test_name or its full_test_path, exactly as list_automation_tests gives them (e.g. "UEAgent.ClaudeCode")
//   @returns { test_name, started, completed, passed, verdict, duration_s, errors, warnings, entries:[{type,message,context,filename,line}], entry_count, note }
TSharedPtr<FJsonObject> HandleRunAutomationTest(const TSharedPtr<FJsonObject>& Params)
{
    FString TestName;
    if (!Params->TryGetStringField(TEXT("test_name"), TestName) || TestName.IsEmpty())
    {
        return mcp_common::MakeErrorReply(
            TEXT("Missing required 'test_name' (exact registered test name from list_automation_tests)."));
    }

    // The shared flow lives in RunAutomationTestCommon. We pass our cache
    // (members) so get_automation_test_results can still read the last result.
    TSharedPtr<FJsonObject> Error;
    FAutomationRunCache Cache(&LastTestName, &LastTestExecutionInfo, &bHasLastResult);
    TSharedPtr<FJsonObject> Result = RunAutomationTestCommon(TestName, Error, &Cache);
    if (Error.IsValid()) { return Error; }
    if (Result.IsValid()) { return mcp_common::MakeOkReply(Result); }
    return mcp_common::MakeErrorReply(TEXT("RunAutomationTestCommon returned no result (internal error)"));
}

// MCP:run_smoke_tests - Runs every registered automation test with the smoke filter, synchronously in the editor, and returns only the aggregate: ran, all_passed and a note. Under a commandlet the engine skips them, so ran and all_passed come back false. For one test with its log entries use run_automation_test.
//   @returns { ran, all_passed, note }
TSharedPtr<FJsonObject> HandleRunSmokeTests(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

    // Honest gate: RunSmokeTests() SKIPS execution (and still returns true) when running
    // as a commandlet. Detect that up front and report a skip instead of a false-green
    // "all_passed" with nothing actually run.
    if (IsRunningCommandlet())
    {
        Result->SetBoolField(TEXT("ran"), false);
        Result->SetBoolField(TEXT("all_passed"), false);
        Result->SetStringField(TEXT("note"),
            TEXT("smoke tests are skipped by the engine when running as a commandlet (RunSmokeTests early-outs); not run"));
        return mcp_common::MakeOkReply(Result);
    }

    FAutomationTestFramework& Framework = FAutomationTestFramework::Get();
    const bool bAllPassed = Framework.RunSmokeTests();

    Result->SetBoolField(TEXT("ran"), true);
    Result->SetBoolField(TEXT("all_passed"), bAllPassed);
    Result->SetStringField(TEXT("note"),
        TEXT("aggregate result of all smoke-filter tests; per-test detail is not exposed by RunSmokeTests"));
    return mcp_common::MakeOkReply(Result);
}

// MCP:get_automation_test_results - Returns the cached result of the last synchronous test completed through run_automation_test, without running anything: test_name, passed, verdict, duration_s, errors, warnings and entries. Skipped or multi-tick tests leave the cache unchanged; fails when no test has completed this editor session.
//   @returns { test_name, started, completed, passed, verdict, duration_s, errors, warnings, entries:[{type,message,context,filename,line}], entry_count }
TSharedPtr<FJsonObject> HandleGetAutomationTestResults(const TSharedPtr<FJsonObject>& Params)
{
    if (!bHasLastResult)
    {
        return mcp_common::MakeErrorReply(
            TEXT("No test has been run yet (call run_automation_test on a synchronous test first)."));
    }

    TSharedPtr<FJsonObject> Result = SerializeExecutionInfo(LastTestName, LastTestExecutionInfo);
    return mcp_common::MakeOkReply(Result);
}
// ============================================================================
// Functional Test authoring
// ============================================================================
//
// The automation harness is headless and cannot validate gameplay at runtime;
// these tools expose PIE-based gameplay testing:
//   * create_functional_test    -- BP subclass of AFunctionalTest, the canonical
//                                 UE pattern for a multiplayer-friendly,
//                                 Blueprint-scriptable test actor.
//   * run_functional_test_in_pie -- start a PIE session that runs the named
//                                 functional test, return its verdict.
//
// Gauntlet proper is a separate runtime harness (a dedicated UnrealEditor-Cmd.exe
// -Gauntlet=<controller> subprocess) that this server does not spawn; run_automation_test
// gives the in-editor verdict of a registered automation test.
//
// Both are Editor-only (FunctionalTestingEditor is an editor module).

// MCP:create_functional_test - Create a Blueprint subclass of AFunctionalTest (an actor placed in a test map that runs scripted checks and reports pass or fail to the automation framework) at package_path/asset_name, then save it; there is no undo step. Use it to author a level-based gameplay test; for an ordinary actor Blueprint use create_blueprint, and to run registered tests use run_automation_test (list them with list_automation_tests). Idempotent: when a Blueprint already exists at that path it is returned as it is, with already_existed true, and saved again; its parent is not checked and parent_class echoes the requested one. A parent that is not found or is not a Functional Test, or an asset of another type at the path, is an error and nothing changes. A failed save makes success false, with saved false and the Blueprint only in memory. Refused while Play-in-Editor runs unless force=true. Then script it with add_blueprint_function and add_graph_node, and place it in a map with spawn_actor.
//   @param asset_name - Asset name of the new Blueprint without folder, e.g. "BP_FT_PlayerHealth".
//   @param package_path - Content folder for the asset, e.g. "/Game/Tests" (default /Game/MCP).
//   @param parent_class - Optional parent: a class deriving from AFunctionalTest, by name without prefix (e.g. "FunctionalTest", the default) or by /Script/ path; a Blueprint parent must be loaded and named by its generated class (e.g. "BP_FT_Base_C").
//   @returns { asset_name, asset_path, package_class, parent_class, already_existed, saved }
TSharedPtr<FJsonObject> HandleCreateFunctionalTest(
    const TSharedPtr<FJsonObject>& Params)
{
    FString AssetName, ParentClassName, PackagePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_name"), AssetName)) { return Missing; }
    if (!Params->TryGetStringField(TEXT("parent_class"), ParentClassName) || ParentClassName.IsEmpty())
    {
        ParentClassName = TEXT("FunctionalTest");
    }
    Params->TryGetStringField(TEXT("package_path"), PackagePath);
    if (PackagePath.IsEmpty()) { PackagePath = mcp_common::DefaultAssetFolder(); }

    // Resolve the parent class by short name (e.g. "FunctionalTest" -> /Script/FunctionalTesting.FunctionalTest).
    FString ResolvedPath = ParentClassName;
    if (!ResolvedPath.StartsWith(TEXT("/Script/")))
    {
        ResolvedPath = TEXT("/Script/FunctionalTesting.") + ParentClassName;
    }
    UClass* ParentClass = LoadObject<UClass>(nullptr, *ResolvedPath);
    if (!ParentClass)
    {
        // Fallback: native-path style ("FunctionalTesting.FunctionalTest").
        ParentClass = FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/%s.%s"), TEXT("FunctionalTesting"), *ParentClassName));
    }
    if (!ParentClass)
    {
        // Last-resort scan: look in any package for a class named FunctionalTest (or whatever subclass).
        for (TObjectIterator<UClass> It; It; ++It)
        {
            if (It->GetName() == ParentClassName && It->IsChildOf(AFunctionalTest::StaticClass()))
            {
                ParentClass = *It;
                break;
            }
        }
    }
    if (!ParentClass || !ParentClass->IsChildOf(AFunctionalTest::StaticClass()))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Parent class not found or not AFunctionalTest-derivative: %s"), *ParentClassName));
    }

    if (const FString AssetConflict = mcp_common::AssetNameConflict(PackagePath / AssetName, AssetName, UBlueprint::StaticClass()); !AssetConflict.IsEmpty())
    {
        return mcp_common::MakeErrorReply(AssetConflict);
    }
    UPackage* Package = CreatePackage(*(PackagePath / AssetName));
    if (!Package) { return mcp_common::MakeErrorReply(TEXT("Failed to create package")); }
    Package->FullyLoad();

    // Idempotency: FKismetEditorUtilities::CreateBlueprint assert()-crashes if the BP
    // already exists in the package.
    UBlueprint* BP = FindObject<UBlueprint>(Package, *AssetName);
    const bool bAlreadyExisted = (BP != nullptr);
    if (!BP)
    {
        BP = Cast<UBlueprint>(
            FKismetEditorUtilities::CreateBlueprint(
                ParentClass, Package, FName(*AssetName), BPTYPE_Normal,
                FName(TEXT("CreateFunctionalTest"))));
        if (!BP) { return mcp_common::MakeErrorReply(TEXT("Failed to create Blueprint")); }
        FAssetRegistryModule::AssetCreated(BP);
        BP->MarkPackageDirty();
    }

    FString SaveErr;
    const bool bSaved = mcp_common::SaveAssetPackage(Package, BP, SaveErr);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_name"), BP->GetName());
    Data->SetStringField(TEXT("asset_path"), BP->GetPathName());
    Data->SetStringField(TEXT("package_class"),
        BP->GeneratedClass ? BP->GeneratedClass->GetPathName() : FString());
    Data->SetStringField(TEXT("parent_class"), ParentClass->GetName());
    Data->SetBoolField(TEXT("already_existed"), bAlreadyExisted);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("create_functional_test"), SaveErr);
}

// MCP:run_functional_test_in_pie - Place one instance of a Functional Test Blueprint class in the editor level (unless one is already there), then ask the editor to start a Simulate-in-Editor session and, with auto_stop_pie (default), to end it again. It does not wait for the test or read its result: verdict is "pass" whenever a session is running right after the request and "not_run" otherwise, so it is no test verdict -- read the outcome in the output log (get_output_log_lines), or run a registered test with run_automation_test, whose verdict is real. The start request is deferred (and with no level viewport, headless, nothing starts), so on a first call pie_started is usually false and the session starts after the reply without being stopped: end it with stop_play_in_editor. With a session already running it answers pass and, with auto_stop_pie, ends that session. The placed actor stays in the level, unsaved and with no undo step. Create the test first with create_functional_test; a class that is not found, or is not a Functional Test, is an error.
//   @param test_asset_path - Functional Test Blueprint, as an asset path ("/Game/Tests/BP_FT_PlayerHealth"), object path or generated-class path ("/Game/Tests/BP_FT_PlayerHealth.BP_FT_PlayerHealth_C"); when none loads, the first loaded Functional Test class whose name contains the last path segment is used.
//   @param auto_stop_pie - Optional bool (default true): request the end of the session right after it is seen running, including a session that was already running before the call.
//   @returns { test_asset_path, pie_started, verdict: "pass"|"not_run", pie_stopped?, already_in_pie?, spawned?, already_in_world?, note? }
TSharedPtr<FJsonObject> HandleRunFunctionalTestInPie(
    const TSharedPtr<FJsonObject>& Params)
{
    FString TestPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("test_asset_path"), TestPath)) { return Missing; }
    bool bAutoStopPie = true;
    Params->TryGetBoolField(TEXT("auto_stop_pie"), bAutoStopPie);

// Resolve the test class. AFunctionalTest BPs are reached via their
    // GENERATED class (suffix `_C`), e.g. `/Game/Foo/BP_Foo.BP_Foo_C`. We try
    // the path as-is first; if the path ends in the BP's outer (no `_C`), try
    // appending it. If still missing, scan the package for any AFunctionalTest
    // subclass -- the dev may have used an unusual naming convention.
    UClass* TestClass = LoadObject<UClass>(nullptr, *TestPath);
    if (!TestClass)
    {
        FString FullPath = TestPath;
        if (!FullPath.Contains(TEXT(".")))
        {
            // No dot at all -- treat the path as a package path and append <name>.BP_<name>_C.
            const FString AssetName = FPaths::GetCleanFilename(FullPath);
            FullPath = TestPath + TEXT(".") + AssetName + TEXT("_C");
        }
        else
        {
            // Has a dot but no `_C` -- replace the trailing name with `_C`.
            FString PackagePath, ObjectName;
            TestPath.Split(TEXT("."), &PackagePath, &ObjectName);
            if (!ObjectName.EndsWith(TEXT("_C")))
            {
                FullPath = PackagePath + TEXT(".") + ObjectName + TEXT("_C");
            }
        }
        TestClass = LoadObject<UClass>(nullptr, *FullPath);
    }
    if (!TestClass || !TestClass->IsChildOf(AFunctionalTest::StaticClass()))
    {
        // Last-resort scan: search all packages for any UClass whose name ends
        // with the user-provided stem (stripped of /Game/ prefix and any _C).
        FString Stem = TestPath;
        Stem.Split(TEXT("."), nullptr, &Stem);
        Stem = FPaths::GetBaseFilename(Stem);  // last component, no ext
        if (Stem.EndsWith(TEXT("_C"))) { Stem.LeftChopInline(2); }
        for (TObjectIterator<UClass> It; It; ++It)
        {
            if (It->IsChildOf(AFunctionalTest::StaticClass()) && It->GetName().Contains(Stem))
            {
                TestClass = *It;
                break;
            }
        }
    }
    if (!TestClass || !TestClass->IsChildOf(AFunctionalTest::StaticClass()))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Functional Test class not found: %s"), *TestPath));
    }

    // Place one instance of the test in the current editor world (so PIE picks it up via
    // GameplayDebugger / FunctionalTest runner). Idempotent: only spawn if no instance
    // already exists for this class.
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World)) { return NoWorld; }
    bool bSpawned = false;
    bool bAlreadyInWorld = false;
    for (TActorIterator<AFunctionalTest> It(World); It; ++It)
    {
        if (It->GetClass() == TestClass)
        {
            bAlreadyInWorld = true;
            break;
        }
    }
    if (!bAlreadyInWorld)
    {
        FActorSpawnParameters ParamsSP;
        ParamsSP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        AFunctionalTest* FT = World->SpawnActor<AFunctionalTest>(TestClass, FVector::ZeroVector, FRotator::ZeroRotator, ParamsSP);
        if (!FT)
        {
            return mcp_common::MakeErrorReply(
                TEXT("Failed to spawn AFunctionalTest instance in editor world"));
        }
        bSpawned = true;
    }

    // Start PIE.
    ULevelEditorSubsystem* LevelEditorSubsystem = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>();
    if (!LevelEditorSubsystem)
    {
        return mcp_common::MakeErrorReply(TEXT("Level Editor Subsystem not found"));
    }
    const bool bAlreadyInPIE = LevelEditorSubsystem->IsInPlayInEditor();
    if (!bAlreadyInPIE)
    {
        LevelEditorSubsystem->EditorPlaySimulate();
    }
    const bool bPieStarted = LevelEditorSubsystem->IsInPlayInEditor();
    if (!bPieStarted)
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("test_asset_path"), TestPath);
        Data->SetBoolField(TEXT("pie_started"), false);
        Data->SetStringField(TEXT("verdict"), TEXT("not_run"));
        Data->SetStringField(TEXT("note"), TEXT("PIE did not start; check Output Log for engine errors"));
        return mcp_common::MakeOkReply(Data);
    }

// Stop PIE if requested.
    bool bPieStopped = false;
    if (bAutoStopPie)
    {
        LevelEditorSubsystem->EditorRequestEndPlay();
        bPieStopped = !LevelEditorSubsystem->IsInPlayInEditor();
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("test_asset_path"), TestPath);
    Data->SetBoolField(TEXT("pie_started"), true);
    Data->SetStringField(TEXT("verdict"), TEXT("pass"));  // optimistic; precise verdict requires per-test inspection
    Data->SetBoolField(TEXT("pie_stopped"), bPieStopped);
    Data->SetBoolField(TEXT("already_in_pie"), bAlreadyInPIE);
    Data->SetBoolField(TEXT("spawned"), bSpawned);
    Data->SetBoolField(TEXT("already_in_world"), bAlreadyInWorld);
    return mcp_common::MakeOkReply(Data);
}

// MCP:get_output_log_lines - Read the single per-editor-session log buffer UEAgent attaches to GLog (general editor output, not PIE-only -- Serialize() captures every category/verbosity that passes through GLog, regardless of PIE state; usable for PIE Functional Test / wait_for_pie_completion orchestration too). NON-DESTRUCTIVE cursor read (since_sequence_id, same idiom as poll_gameplay_messages), structured entries. Supports all three filters (message text, verbosity level, category).
//   @param count - Optional int32 cap on the number of entries returned (default 200, max 4096).
//   @param filter - Optional case-insensitive substring on the message text.
//   @param level_filter - Optional exact verbosity name filter (Fatal|Error|Warning|Display|Log|Verbose|VeryVerbose), case-insensitive.
//   @param category_filter - Optional case-insensitive substring on the log category name.
//   @param since_sequence_id - Optional int64 cursor; only entries with a higher sequence_id are returned (default 0 = everything currently retained).
//   @returns { lines: [{sequence_id, ts, level, category, message}], count, held_total, last_sequence_id }
TSharedPtr<FJsonObject> HandleGetOutputLogLines(
    const TSharedPtr<FJsonObject>& Params)
{
    int32 Count = 200;
    {
        double Tmp = 0.0;
        if (Params->TryGetNumberField(TEXT("count"), Tmp) && Tmp > 0.0)
        {
            Count = FMath::Clamp(static_cast<int32>(Tmp), 1, FUEAgentLogCaptureDevice::MaxLines);
        }
    }
    FString Filter, LevelFilter, CategoryFilter;
    Params->TryGetStringField(TEXT("filter"), Filter);
    Params->TryGetStringField(TEXT("level_filter"), LevelFilter);
    Params->TryGetStringField(TEXT("category_filter"), CategoryFilter);
    int64 SinceSequenceId = 0;
    {
        double Tmp = 0.0;
        if (Params->TryGetNumberField(TEXT("since_sequence_id"), Tmp) && Tmp > 0.0)
        {
            SinceSequenceId = static_cast<int64>(Tmp);
        }
    }

    const int32 HeldTotal = GMCP_LogCapture.Size();
    TArray<FMcpLogEntry> Entries = GMCP_LogCapture.Peek(SinceSequenceId, Filter, LevelFilter, CategoryFilter, Count);

    TArray<TSharedPtr<FJsonValue>> Arr;
    Arr.Reserve(Entries.Num());
    int64 LastSeen = SinceSequenceId;
    for (const FMcpLogEntry& E : Entries)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetNumberField(TEXT("sequence_id"), static_cast<double>(E.SequenceId));
        Entry->SetNumberField(TEXT("ts"), E.Timestamp);
        Entry->SetStringField(TEXT("level"), McpVerbosityToString(E.Verbosity));
        Entry->SetStringField(TEXT("category"), E.Category.ToString());
        Entry->SetStringField(TEXT("message"), E.Message);
        Arr.Add(MakeShared<FJsonValueObject>(Entry));
        LastSeen = E.SequenceId;
    }

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetArrayField(TEXT("lines"), Arr);
    Out->SetNumberField(TEXT("count"), Arr.Num());
    Out->SetNumberField(TEXT("held_total"), HeldTotal);
    Out->SetNumberField(TEXT("last_sequence_id"), static_cast<double>(LastSeen));
    return mcp_common::MakeOkReply(Out);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("create_functional_test"), &HandleCreateFunctionalTest },
    { TEXT("execute_python"), &HandleExecutePython },
    { TEXT("execute_python_file"), &HandleExecutePythonFile },
    { TEXT("get_automation_test_results"), &HandleGetAutomationTestResults },
    { TEXT("get_output_log_lines"), &HandleGetOutputLogLines },
    { TEXT("get_python_status"), &HandleGetPythonStatus },
    { TEXT("list_automation_tests"), &HandleListAutomationTests },
    { TEXT("run_automation_test"), &HandleRunAutomationTest },
    { TEXT("run_functional_test_in_pie"), &HandleRunFunctionalTestInPie },
    { TEXT("run_smoke_tests"), &HandleRunSmokeTests },
};
} // namespace UEAgentAutomationCommands
