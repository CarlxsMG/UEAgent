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

    /** The test run_automation_test or run_functional_test_in_pie left running, in words; empty when none runs. */
    FString DescribeRunningTest();

    /** The refusal Tool answers while such a test runs (it names the test), or null when none runs. */
    TSharedPtr<FJsonObject> RefuseWhileTestRuns(const TCHAR* Tool);
}
