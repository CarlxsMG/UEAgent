#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FMcpToolCatalog;

/**
 * The compact tool list: three meta-tools that reach the whole catalog without listing it, for
 * clients that cap how many tools a server may list or that pay for every listed tool in context.
 *
 *   find_tools     searches the catalog by words and/or domain
 *   describe_tool  one tool's description and input schema, as the full tools/list carries them
 *   call_tool      runs a catalog tool by name
 *
 * The server lists them instead of the catalog when UEAGENT_TOOLS=compact, or for one client
 * whose URL asks for it (/mcp?tools=compact), and answers them in either mode. find_tools and
 * describe_tool read the immutable catalog on the network thread; call_tool only unwraps the
 * call, so the validation, guards and readback are those of the tool it names.
 */
namespace McpMetaTools
{
	extern const TCHAR* const FindTools;
	extern const TCHAR* const DescribeTool;
	extern const TCHAR* const CallTool;

	/** The compact `{"tools":[...]}` object, serialized to UTF-8. */
	TArray<uint8> BuildToolsListUtf8(const FMcpToolCatalog& Catalog);

	/** The paragraph the compact mode puts before the agent instructions that `initialize` carries. */
	FString Instructions(const FMcpToolCatalog& Catalog);

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
