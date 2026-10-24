// Copyright (c) 2026 CarlxsMG.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/**
 * The skills this plugin ships (Resources/Skills, Mcp/McpSkills.h), served over MCP as well as copied into a project:
 *
 *   prompts    one per skill, named like it. The client offers them to the user, typically as slash commands, and
 *              prompts/get returns the skill as one user message, with the user's request when there is one.
 *   resources  ueagent://guide, the ueagent-tool-map skill (which tool, pack or domain covers a task, and the order of
 *              a task); ueagent://skills/<name> for every other skill, and ueagent://skills/<name>/<file> for the
 *              other files of a skill.
 *
 * Both read the same SKILL.md files, on every request, so there is no second copy to keep in step. The server answers
 * them on the network thread: they only read files. Tools/catalog_server.py serves the same prompts and resources to a
 * directory that inspects the server without an editor; `--compare` checks that the two agree.
 */
namespace McpPrompts
{
	/** The tool map's URI, the resource the server instructions point every agent to. */
	extern const TCHAR* const GuideUri;
	/** The skill the guide serves. */
	extern const TCHAR* const GuideSkill;

	/** prompts/list's result: every shipped skill. */
	TSharedRef<FJsonObject> ListPrompts(const FString& SkillsDir);

	/**
	 * prompts/get's result. Null when the call names no prompt or one that is not shipped: OutCode and OutError are then
	 * the JSON-RPC error (-32602, Invalid params, with the prompts' names).
	 */
	TSharedPtr<FJsonObject> GetPrompt(const FString& SkillsDir, const TSharedPtr<FJsonObject>& Params, int32& OutCode, FString& OutError);

	/** resources/list's result: the guide, the other skills and their files. */
	TSharedRef<FJsonObject> ListResources(const FString& SkillsDir);

	/**
	 * resources/read's result. Null when the call names no uri (OutCode -32602) or one that is not listed (-32002,
	 * "Resource not found", with OutUri for the error's data).
	 */
	TSharedPtr<FJsonObject> ReadResource(const FString& SkillsDir, const TSharedPtr<FJsonObject>& Params, int32& OutCode, FString& OutError, FString& OutUri);
}
