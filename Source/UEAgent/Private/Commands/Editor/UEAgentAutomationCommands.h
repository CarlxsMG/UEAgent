// Copyright (c) 2026 CarlxsMG.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace UEAgentAutomationCommands
{
    /** Starts capturing the editor log for get_output_log_lines and the commands that report the lines they caused. */
    void StartLogCapture();

    /** Stops it, before the module unloads. */
    void StopLogCapture();

    /** A line of that capture, as another family reads it. */
    struct FCapturedLogLine
    {
        int64 SequenceId = 0;
        FName Category;
        ELogVerbosity::Type Verbosity = ELogVerbosity::Log;
        FString Message;
    };

    /** The sequence id of the newest captured line: the cursor to read only what comes after it. */
    int64 GetLogCursor();

    /** Up to MaxLines captured lines newer than SinceSequenceId, oldest first. */
    TArray<FCapturedLogLine> PeekLogSince(int64 SinceSequenceId, int32 MaxLines);

    /** The test run_automation_test or run_functional_test_in_pie left running, in words; empty when none runs. */
    FString DescribeRunningTest();

    /** The refusal Tool answers while such a test runs (it names the test), or null when none runs. */
    TSharedPtr<FJsonObject> RefuseWhileTestRuns(const TCHAR* Tool);
}
