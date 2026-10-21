// Copyright (c) 2026 CarlxsMG.

#pragma once

#include "CoreMinimal.h"

/**
 * Claude Code's connection to this server: the entry in the project's `.mcp.json`.
 *
 * No Slate on purpose: it is used by the status bar, the console (`mcp.claude.status`,
 * `mcp.claude.connect`) and the `UEAgent.ClaudeCode` test, which exercises it on temporary
 * files without any UI.
 *
 * The file belongs to the TEAM (it travels in the repo) and may hold other servers: only our own
 * entry is touched, everything else is kept -- unknown fields of our entry included -- and the
 * existing formatting (indentation, line endings) is respected, so the diff is only the change.
 */
namespace McpClaudeCode
{
	/** Entry name: it becomes the tool prefix in Claude Code (`mcp__unreal__*`). */
	extern UEAGENT_API const TCHAR* const ServerName;
	/** Environment variable that carries the token when the server requires one: never written in clear text. */
	extern UEAGENT_API const TCHAR* const TokenEnvVar;

	enum class EState : uint8
	{
		Connected,      // an entry points at this server
		MissingFile,    // there is no .mcp.json
		MissingEntry,   // there is a .mcp.json, but no entry points here
		WrongTarget,    // the "unreal" entry exists and points elsewhere (another port, stdio...)
		MissingAuth,    // points here, but the server requires a token and the entry sends no header
		Unreadable,     // cannot be read or is not valid JSON: left untouched
	};

	/**
	 * What Claude Code decided about the entry, according to `.claude/settings.json` and
	 * `.claude/settings.local.json` next to the .mcp.json. A project server does not start until
	 * the user approves it, and a rejection wins over any approval (Claude Code settings
	 * documentation: `disabledMcpjsonServers`).
	 */
	enum class EApproval : uint8
	{
		Pending,   // nothing here approves it: Claude Code will ask the first time (or the user's ~/.claude approves it)
		Approved,  // named by enabledMcpjsonServers, or enableAllProjectMcpServers is set
		Rejected,  // named by disabledMcpjsonServers: it will not start
	};

	struct FStatus
	{
		EState State = EState::MissingFile;
		EApproval Approval = EApproval::Pending;
		/** Absolute path of the .mcp.json. */
		FString Path;
		/** The entry that counts (empty if there is none). */
		FString EntryName;
		/** One line for the user. */
		FString Detail;
		/** The settings file that rejects it (only with Approval == Rejected). */
		FString RejectedIn;

		/** Connected and not rejected: Claude Code will reach this server. */
		bool IsUsable() const { return State == EState::Connected && Approval != EApproval::Rejected; }
	};

	UEAGENT_API FString ServerUrl(uint16 Port);
	/** `<ProjectDir>/.mcp.json`: Claude Code reads it from the project root, where it is opened. */
	UEAGENT_API FString ProjectConfigPath();

	/** The port this editor serves on (the configured one if stopped) and whether it requires a token. */
	UEAGENT_API uint16 CurrentPort();
	UEAGENT_API bool CurrentRequiresToken();

	UEAGENT_API FStatus Inspect(const FString& Path, uint16 Port, bool bRequiresToken);
	/** Inspect on the project's .mcp.json, with this editor's port and token. */
	UEAGENT_API FStatus InspectProject();
	UEAGENT_API const TCHAR* StateName(EState State);

	/**
	 * Leaves an entry pointing at this server: creates it, or fixes the existing one. Does not
	 * write if already connected or if the file cannot be read. Returns true if the file is
	 * connected when RE-READ -- the proof is the re-read, not the write -- and OutMessage says
	 * what changed.
	 */
	UEAGENT_API bool Connect(const FString& Path, uint16 Port, bool bRequiresToken, FString& OutMessage);

	/** The equivalent command for whoever prefers not to touch the .mcp.json (local scope, per user). */
	UEAGENT_API FString AddCommand(uint16 Port, bool bRequiresToken);
}
