#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/**
 * The tool catalog served by the native MCP server (FMcpHttpServer).
 *
 * Loaded from <Plugin>/Resources/command_catalog.json -- the JSON that
 * Tools/codegen_command_catalog.py generates from the handlers' `// MCP:` and `@param`
 * annotations -- and the `tools/list` answer is built HERE, in C++:
 *
 *   - per-parameter types and descriptions (straight from the catalog)
 *   - closed enums deduced from the description (a STRICT heuristic)
 *   - `force` on the commands that are unsafe with PIE running (from McpPieSafety)
 *   - parameter aliases + relaxed canonical names (the catalog's `aliasOf` and McpParamAliases)
 *   - rotation: the schema also accepts the {roll,pitch,yaw} OBJECT
 *   - the `@returns` shape, appended to the description
 *   - the MCP `annotations` (read-only, destructive, idempotent, open world), from the catalog
 *
 * Whatever is not in the schema cannot be sent by the client, so the three escape hatches
 * above (`force`, aliases, rotation object) are declared in the schema itself (otherwise they
 * would be unreachable).
 *
 * Immutable once built. Network threads read it without locks through GetCurrent(), which
 * returns a TSharedPtr<const ...>; reloading = replacing the whole pointer.
 */
class UEAGENT_API FMcpToolCatalog
{
public:
	struct FParam
	{
		FString Name;
		/** string | number | integer | boolean | array | object | any */
		FString JsonType;
		bool bRequired = false;
		FString Description;
		/** Closed set of values (strings only). Empty = not a provable enum. */
		TArray<FString> EnumValues;
		/** Array-typed rotation parameter: its schema also accepts the object form. */
		bool bIsRotation = false;
		/** The parameter this one is another spelling of (`aliasOf` in the catalog): the handler
		 *  reads either, as in `!TryGet(a) && !TryGet(b)`. Empty for every other parameter. */
		FString AliasOf;
		/** A number the handler reads only into integer variables (`intRange` in the catalog): out
		 *  of [IntRangeMin, IntRangeMax] the engine's read fails and the handler would go on with
		 *  its default, so McpPreconditions refuses such a value before the handler runs. */
		bool bHasIntRange = false;
		double IntRangeMin = 0.0;
		double IntRangeMax = 0.0;
	};

	struct FTool
	{
		FString Name;
		FString Domain;
		FString Description;
		TArray<FParam> Params;
		/** Canonical names that were required and become optional in the schema because they
		 *  accept aliases (the handler's own, FParam::AliasOf, and the dispatcher's): CheckRequired
		 *  enforces the requirement, counting any spelling. */
		TMap<FString, TArray<FString>> RelaxedRequired;
		/** The `inputSchema` tools/list carries for it (describe_tool returns it as is). */
		TSharedPtr<FJsonObject> InputSchema;
		/** The annotation's `@returns`: what a successful call answers. */
		FString Returns;
		/** The MCP `annotations` (readOnlyHint, destructiveHint, idempotentHint, openWorldHint) that
		 *  Tools/tool_hints.py derives from the handler's code. Null when the catalog has none. */
		TSharedPtr<FJsonObject> Annotations;

		const FParam* FindParam(const FString& ParamName) const;
		/** The description tools/list and describe_tool publish: Description, then "Returns: " and
		 *  Returns, so an agent knows the reply's shape before it calls. */
		FString PublishedDescription() const;
	};

	/** Reads and builds. Returns null and fills OutError if the file is missing or cannot be parsed. */
	static TSharedPtr<const FMcpToolCatalog> LoadFromFile(const FString& Path, FString& OutError);

	/** <Plugin>/Resources/command_catalog.json, as an absolute path. */
	static FString DefaultCatalogPath();

	/** The current catalog (may be null if it could not be loaded). Safe from any thread. */
	static TSharedPtr<const FMcpToolCatalog> GetCurrent();
	static void SetCurrent(TSharedPtr<const FMcpToolCatalog> NewCatalog);

	const FTool* FindTool(const FString& ToolName) const;
	const TArray<FTool>& GetTools() const { return Tools; }
	int32 NumTools() const { return Tools.Num(); }
	int32 NumParams() const { return TotalParams; }
	const FString& GetSourcePath() const { return SourcePath; }

	/** The `{"tools":[...]}` object already serialized to UTF-8: tools/list sends it as is. */
	const TArray<uint8>& GetToolsListUtf8() const { return ToolsListUtf8; }
	/** The same for the compact list: the meta-tools of Mcp/McpMetaTools.h instead of the catalog. */
	const TArray<uint8>& GetCompactToolsListUtf8() const { return CompactToolsListUtf8; }

	/**
	 * Missing required parameters (a null counts as absent). Returns the error message, or empty
	 * if everything is present. For a relaxed canonical name, any of its spellings counts.
	 */
	FString CheckRequired(const FTool& Tool, const TSharedPtr<FJsonObject>& Args) const;

	/**
	 * The first number argument outside its parameter's integer range (FParam::bHasIntRange), as
	 * an error message naming the parameter, the value and the range; empty when every one fits
	 * or the catalog does not know CommandType. Game thread (McpPreconditions::Check).
	 */
	static FString CheckIntegerRanges(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

	/** Does the parameter NAME read as a rotation: rot, *rotation or *rotator? */
	static bool LooksLikeRotationName(const FString& ParamName);

	/**
	 * Rewrites in place the {roll,pitch,yaw} object form into the wire array [pitch, yaw, roll]
	 * for CommandType's rotation parameters. Only touches objects whose keys are ALL axes and whose
	 * values are numbers: any other shape is left intact for the handler to decide. Game thread
	 * (called by DispatchSingleCommand, so it also covers `batch`). Returns how many parameters
	 * it rewrote.
	 */
	static int32 NormalizeRotationParams(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
	TArray<FTool> Tools;
	TMap<FString, int32> IndexByName;
	TArray<uint8> ToolsListUtf8;
	TArray<uint8> CompactToolsListUtf8;
	FString SourcePath;
	int32 TotalParams = 0;
};
