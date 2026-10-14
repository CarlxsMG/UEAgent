#pragma once

/**
 * The plugin's face in the editor: the status-bar indicator showing "MCP" and the port, its
 * menu (status, connect Claude Code, restart, contract self-test, settings) and the hint shown
 * on startup when the project is not connected. The .mcp.json logic lives in
 * Mcp/McpClaudeCode.h; this is only Slate.
 */
namespace McpEditorUI
{
	void Startup();
	void Shutdown();
}
