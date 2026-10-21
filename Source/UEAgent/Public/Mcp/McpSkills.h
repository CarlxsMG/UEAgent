// Copyright (c) 2026 CarlxsMG.

#pragma once

#include "CoreMinimal.h"

/**
 * The agent skills this plugin ships (Resources/Skills/<name>/SKILL.md, in the Agent Skills format)
 * and their copies in the project, where the AI clients look for them:
 *   - .claude/skills  Claude Code;
 *   - .agents/skills  Codex, Cursor, VS Code, Antigravity CLI and Gemini CLI.
 *
 * Each copy is stamped with the plugin version (`metadata: ueagent-version`): that marks it as ours
 * and tells an old copy from a current one. A folder without the stamp is the user's and is never
 * touched, even with one of our names. They ship with the plugin so they always name the tools of
 * the version that is installed.
 *
 * No Slate: used by the status bar, the console (`mcp.skills.*`) and the `UEAgent.Skills` test.
 */
namespace McpSkills
{
	enum class ETarget : uint8
	{
		ClaudeCode, // .claude/skills
		Agents,     // .agents/skills
	};

	UEAGENT_API TArray<ETarget> AllTargets();
	/** "claude", "agents": the console's argument. */
	UEAGENT_API const TCHAR* TargetId(ETarget Target);
	UEAGENT_API bool TargetFromId(const FString& Id, ETarget& OutTarget);
	/** ".claude/skills", ".agents/skills". */
	UEAGENT_API const TCHAR* TargetRelativeDir(ETarget Target);
	/** The clients that read that folder, for the user. */
	UEAGENT_API const TCHAR* TargetClients(ETarget Target);
	UEAGENT_API FString ProjectTargetDir(ETarget Target);

	/** <plugin>/Resources/Skills. */
	UEAGENT_API FString ShippedDir();
	/** The plugin's VersionName, the stamp of every copy. */
	UEAGENT_API FString PluginVersion();
	/** The skills in ShippedDir: the folders with a SKILL.md, sorted. */
	UEAGENT_API TArray<FString> ShippedNames(const FString& Dir);

	enum class ESkillState : uint8
	{
		Missing,  // not in the project
		Current,  // our copy, of this version
		Outdated, // our copy, of another version
		Foreign,  // a folder with that name that we did not install: left alone
	};

	struct FSkill
	{
		FString Name;
		ESkillState State = ESkillState::Missing;
		/** The stamp of our copy (empty if Missing or Foreign). */
		FString InstalledVersion;
	};

	struct FStatus
	{
		/** Absolute path of the target folder. */
		FString Dir;
		/** The version the copies should have. */
		FString Version;
		/** One per shipped skill. */
		TArray<FSkill> Skills;
		/** Our copies of skills this version no longer ships. */
		TArray<FString> Retired;

		UEAGENT_API int32 Count(ESkillState State) const;
		/** At least one of our copies is there. */
		UEAGENT_API bool AnyInstalled() const;
		/** Our copies are there and some is old, missing or retired. */
		UEAGENT_API bool NeedsUpdate() const;
		/** One line for the user. */
		UEAGENT_API FString Summary() const;
	};

	UEAGENT_API FStatus Inspect(const FString& ShippedDir, const FString& TargetDir, const FString& Version);
	/** Inspect on the project's folder, with this plugin's skills and version. */
	UEAGENT_API FStatus InspectProject(ETarget Target);

	/**
	 * Copies every shipped skill into TargetDir, stamped with Version. An older copy of ours is
	 * replaced whole, a copy of a skill no longer shipped is deleted, and a folder that is not ours is
	 * left untouched (and reported). True when re-reading the folder shows every shipped skill current,
	 * Foreign ones aside; OutMessage says what changed.
	 */
	UEAGENT_API bool Install(const FString& ShippedDir, const FString& TargetDir, const FString& Version, FString& OutMessage);

	/** Deletes every skill folder in TargetDir that this plugin installed, and nothing else. */
	UEAGENT_API bool Remove(const FString& TargetDir, FString& OutMessage);
}
