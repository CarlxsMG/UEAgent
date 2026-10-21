// Copyright (c) 2026 CarlxsMG.

#pragma once

/**
 * The Activity panel: a dockable tab listing the last tool calls (Mcp/McpActivity.h), newest first,
 * with their time, duration, outcome and detail. Opened from the MCP status-bar menu.
 */
namespace McpActivityPanel
{
	void Register();
	void Unregister();
	void Open();
}
