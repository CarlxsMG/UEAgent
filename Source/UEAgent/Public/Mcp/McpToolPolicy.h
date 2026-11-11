// Copyright (c) 2026 CarlxsMG.

#pragma once

#include "CoreMinimal.h"
#include "Mcp/McpToolCatalog.h"

class FJsonObject;

/**
 * Which tools the user lets an agent run (Editor Preferences > Plugins > UEAgent > Permissions): read-only mode, code
 * execution, and the domains and tools turned off. A tool that is off is left out of tools/list and find_tools and is
 * refused when called.
 *
 * An immutable snapshot: built on the game thread from UUEAgentSettings (Rebuild) and swapped whole, like the catalog,
 * so the network threads read it through GetCurrent() without touching the settings object. Each snapshot carries a
 * generation; the server tells a session its list changed when the generation it last listed with is not the current one.
 *
 * Classes, from the catalog (Tools/tool_hints.py derives them):
 *   reads           readOnlyHint                 allowed in read-only mode
 *   observes        `observes` (writes only an image under Saved)   allowed in read-only mode
 *   code execution  `codeExec`                   refused when code execution is off, and in read-only mode
 *   the rest        writes                       refused in read-only mode
 * execute_batch is a container: it is allowed, and each item is judged on its own when it is dispatched. The meta-tools
 * (find_tools, describe_tool, call_tool, enable_toolset) are not catalog tools and are always allowed.
 */
class UEAGENT_API FMcpToolPolicy
{
public:
	/** Why a tool is off; Allowed when it is not. The order is the order the checks run in. */
	enum class EVerdict : uint8
	{
		Allowed,
		DisabledTool,
		DisabledDomain,
		CodeExecution,
		ReadOnly,
	};

	struct FSettings
	{
		bool bReadOnly = false;
		/** What turned read-only mode on ("settings", "-McpReadOnly", "UEAGENT_READONLY"); empty when it is off. */
		FString ReadOnlySource;
		bool bAllowCodeExecution = true;
		TArray<FString> DisabledDomains;
		TArray<FString> DisabledTools;
	};

	/** A snapshot of Settings with the next generation. */
	static TSharedRef<const FMcpToolPolicy> Make(const FSettings& Settings);

	/** The current snapshot; never null (everything allowed until the first Rebuild). Any thread. */
	static TSharedRef<const FMcpToolPolicy> GetCurrent();
	static void SetCurrent(TSharedRef<const FMcpToolPolicy> NewPolicy);
	/** Builds a snapshot from UUEAgentSettings (with the command line and the environment) and makes it current when it
	 *  differs from the current one. Returns whether it changed. Game thread. */
	static bool Rebuild();

	const FSettings& GetSettings() const { return Settings; }
	uint32 GetGeneration() const { return Generation; }
	bool IsReadOnly() const { return Settings.bReadOnly; }
	/** Whether any tool can be off: read-only mode, code execution off, or a domain or tool turned off. */
	bool IsRestrictive() const;

	/** Whether Tool runs, called as RequestedName (an old name of a renamed tool counts as the tool for DisabledTools). */
	EVerdict Evaluate(const FMcpToolCatalog::FTool& Tool, const FString& RequestedName = FString()) const;
	/** Evaluate by name through Catalog (a renamed tool's old name resolves to it). A name the catalog does not know is
	 *  refused in read-only mode, since nothing says it only reads, and allowed otherwise (the dispatch answers it). */
	EVerdict EvaluateName(const FMcpToolCatalog* Catalog, const FString& Name) const;
	/** Whether Tool is listed and found: Evaluate is Allowed. */
	bool IsAllowed(const FMcpToolCatalog::FTool& Tool) const { return Evaluate(Tool) == EVerdict::Allowed; }

	/** "read_only", "code_execution", "disabled_domain", "disabled_tool"; empty for Allowed. */
	static const TCHAR* VerdictName(EVerdict Verdict);

	/** The refusal's detail: { refused, refused_by: "policy", policy, command, domain, hint }. */
	TSharedRef<FJsonObject> MakeRefusalData(EVerdict Verdict, const FString& Command, const FString& Domain) const;
	/** The refusal's message, which tells the agent not to retry and to ask the user. */
	FString MakeRefusalMessage(EVerdict Verdict, const FString& Command, const FString& Domain) const;
	/** The bridge's envelope of a refusal: { status: "error", error, result: MakeRefusalData }. */
	TSharedRef<FJsonObject> MakeRefusalEnvelope(EVerdict Verdict, const FString& Command, const FString& Domain) const;
	/** What the model sees for that envelope (as McpReadback::Unwrap gives it): { success: false, error, result }. */
	TSharedRef<FJsonObject> MakeRefusalPayload(EVerdict Verdict, const FString& Command, const FString& Domain) const;

	/** One line for mcp.http.status and the status bar menu: "read-only (settings), code execution off, 2 domains off". */
	FString Describe() const;
	/** The sentence initialize's instructions start with when a tool can be off; empty otherwise. */
	FString Instructions() const;

	/** The tools Catalog lists that are off under this policy: what a filtered list leaves out. */
	int32 CountOff(const FMcpToolCatalog& Catalog) const;

private:
	FSettings Settings;
	TSet<FString> DisabledDomainSet;
	TSet<FString> DisabledToolSet;
	uint32 Generation = 0;
};
