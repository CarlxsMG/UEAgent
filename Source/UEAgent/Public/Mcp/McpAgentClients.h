// Copyright (c) 2026 CarlxsMG.

#pragma once

#include "CoreMinimal.h"

/**
 * The connection to this server of the AI clients that read their MCP servers from a file in the
 * project, other than Claude Code (McpClaudeCode): Cursor, VS Code and Antigravity CLI. Codex keeps
 * its servers in ~/.codex/config.toml, outside the project, so it gets the command instead.
 *
 * Same rules as McpClaudeCode: the file may belong to the team and hold other servers, so only our
 * entry is touched, everything else is kept with its layout, and the proof is re-reading the file.
 * No Slate: used by the status bar, the console (`mcp.clients.status`, `mcp.clients.connect`) and the
 * `UEAgent.AgentClients` test.
 */
namespace McpAgentClients
{
	enum class EClient : uint8
	{
		Cursor,      // .cursor/mcp.json, "mcpServers", "url"
		VSCode,      // .vscode/mcp.json, "servers", "type": "http", "url"
		Antigravity, // .agents/mcp_config.json, "mcpServers", "serverUrl"
	};

	enum class EState : uint8
	{
		Connected,       // an entry points at this server
		MissingFile,     // the client's file does not exist
		MissingEntry,    // the file exists, but no entry points here
		WrongTarget,     // the "unreal" entry exists and points elsewhere
		MissingAuth,     // points here, but the server requires a token and the entry sends none
		AuthUnsupported, // the server requires a token and this client's file has no documented way to send it
		Unreadable,      // cannot be read or is not valid JSON: left untouched
	};

	struct FStatus
	{
		EClient Client = EClient::Cursor;
		EState State = EState::MissingFile;
		/** Absolute path of the client's file. */
		FString Path;
		/** The entry that counts (empty if there is none). */
		FString EntryName;
		/** One line for the user. */
		FString Detail;

		bool IsUsable() const { return State == EState::Connected; }
	};

	UEAGENT_API TArray<EClient> All();
	/** "Cursor", "VS Code", "Antigravity CLI". */
	UEAGENT_API const TCHAR* DisplayName(EClient Client);
	/** "cursor", "vscode", "antigravity": the console's argument. */
	UEAGENT_API const TCHAR* Id(EClient Client);
	UEAGENT_API bool FromId(const FString& Id, EClient& OutClient);
	/** The client's file relative to the project folder, e.g. ".cursor/mcp.json". */
	UEAGENT_API const TCHAR* RelativePath(EClient Client);
	UEAGENT_API FString ProjectConfigPath(EClient Client);
	UEAGENT_API const TCHAR* StateName(EState State);

	/** The URL these clients get: the compact tool list (three meta-tools), since they load every tool up front. */
	UEAGENT_API FString ClientUrl(uint16 Port);

	UEAGENT_API FStatus Inspect(EClient Client, const FString& Path, uint16 Port, bool bRequiresToken);
	/** Inspect on the project's file, with this editor's port and token. */
	UEAGENT_API FStatus InspectProject(EClient Client);

	/**
	 * Leaves an entry pointing at this server: creates the file or the entry, or fixes the entry.
	 * Writes nothing if already connected, if the file cannot be read, or if the server requires a
	 * token this client cannot send. Returns true if the file is connected when RE-READ, and
	 * OutMessage says what changed.
	 */
	UEAGENT_API bool Connect(EClient Client, const FString& Path, uint16 Port, bool bRequiresToken, FString& OutMessage);

	/** `codex mcp add ...`: Codex keeps its servers in ~/.codex/config.toml, for every project. */
	UEAGENT_API FString CodexAddCommand(uint16 Port, bool bRequiresToken);
}
