// Copyright (c) 2026 CarlxsMG.

#pragma once

namespace UEAgentAutomationCommands
{
    /** Starts capturing the editor log for get_output_log_lines and the commands that report the lines they caused. */
    void StartLogCapture();

    /** Stops it, before the module unloads. */
    void StopLogCapture();
}
