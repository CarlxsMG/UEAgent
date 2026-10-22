// Copyright (c) 2026 CarlxsMG.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;
class FMcpToolCatalog;

/**
 * The meta-tools that reach the whole catalog without listing it, for clients that cap how many tools a server may
 * list or that pay for every listed tool in context.
 *
 *   find_tools      searches the catalog by words and/or domain
 *   describe_tool   one tool's description and input schema, as the full tools/list carries them
 *   call_tool       runs a catalog tool by name
 *   enable_toolset  adds a domain's tools to this connection's list (core list only: the server answers it, with the
 *                   connection's session, in McpHttpServer.cpp)
 *
 * The core list, the default, carries the core tools and the four of them. The compact list carries the first three
 * alone: UEAGENT_TOOLS=compact, or one client whose URL asks for it (/mcp?tools=compact). The full list (?tools=all)
 * carries every tool and none of them, and the server answers them in every mode. find_tools and describe_tool read
 * the immutable catalog on the network thread; call_tool only unwraps the call, so the validation, guards and
 * readback are those of the tool it names.
 */
namespace McpMetaTools
{
	extern const TCHAR* const FindTools;
	extern const TCHAR* const DescribeTool;
	extern const TCHAR* const CallTool;
	extern const TCHAR* const EnableToolset;

	/** Which list the meta-tools are for: the compact one has no enable_toolset, and its texts say so. */
	enum class EList : uint8
	{
		Compact,
		Core,
	};

	/** The meta-tools of List, as the tools/list entries the server sends. */
	TArray<TSharedPtr<FJsonValue>> BuildMetaTools(const FMcpToolCatalog& Catalog, EList List);

	/** The compact `{"tools":[...]}` object, serialized to UTF-8. */
	TArray<uint8> BuildToolsListUtf8(const FMcpToolCatalog& Catalog);

	/** The paragraph the compact mode puts before the agent instructions that `initialize` carries. */
	FString Instructions(const FMcpToolCatalog& Catalog);

	/** The same for the core list. */
	FString CoreInstructions(const FMcpToolCatalog& Catalog);

	/**
	 * enable_toolset's `domains` (or a single `domain`), each as the catalog spells it. Returns false with OutError, and
	 * bOutIsError, when the call names no domain or one the catalog does not have (the error lists the domains).
	 */
	bool ReadToolsetDomains(const FMcpToolCatalog& Catalog, const TSharedPtr<FJsonObject>& Args, TArray<FString>& OutDomains, FString& OutError);

	/**
	 * enable_toolset's reply: the domains, the tools each one adds (the core tools are already listed) and, when the
	 * connection has a session, that the list changed; without one, how to reach them anyway.
	 */
	TSharedRef<FJsonObject> ToolsetReply(const FMcpToolCatalog& Catalog, const TArray<FString>& Domains, const TSet<FString>& EnabledBefore, bool bHasSession);

	/**
	 * Answers find_tools or describe_tool; returns null for any other name. bOutIsError marks a
	 * call the model has to correct (an unknown tool or domain).
	 */
	TSharedPtr<FJsonObject> Answer(const FMcpToolCatalog& Catalog, const FString& ToolName, const TSharedPtr<FJsonObject>& Args, bool& bOutIsError);

	/**
	 * call_tool's {name, arguments} -> the tool it names and that tool's arguments (nulls dropped,
	 * and `arguments` also accepted as a JSON string). Returns false with OutError if the call
	 * names no tool or its arguments are not an object.
	 */
	bool UnwrapCall(const TSharedPtr<FJsonObject>& Args, FString& OutToolName, TSharedPtr<FJsonObject>& OutArguments, FString& OutError);

	/** Up to MaxNames catalog tools closest to an unknown name: near spellings first, then shared words. */
	TArray<FString> SuggestNames(const FMcpToolCatalog& Catalog, const FString& UnknownName, int32 MaxNames = 5);
}
