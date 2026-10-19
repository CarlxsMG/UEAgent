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
	/** The value a vector reader takes (`vectorShape` in the catalog: "vector", "vector2d", "rotator"). */
	enum class EVectorShape : uint8
	{
		None,
		/** [X, Y, Z], read by mcp_common::ReadJsonVector. */
		Vector,
		/** [X, Y], read by mcp_common::ReadJsonVector2D. */
		Vector2D,
		/** [Pitch, Yaw, Roll] or {"roll", "pitch", "yaw"}, read by mcp_common::ReadJsonRotator. */
		Rotator,
	};

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
		/** A string, or array of strings, the engine makes a name of (`nameLike` in the catalog): an FName
		 *  holds fewer than NAME_SIZE characters and building a longer one aborts the editor, so
		 *  McpPreconditions refuses such a value of NameLengthLimit characters or more before the handler runs. */
		bool bNameLike = false;
		/** A param every read of which is one vector reader (`vectorShape` in the catalog): the reader turns any
		 *  other value into zero, so McpPreconditions refuses a value of another shape before the handler runs. */
		EVectorShape VectorShape = EVectorShape::None;
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

	/** Characters a tool may add around a name argument before it builds an FName from it: a class prefix ("A",
	 *  "U"), a folder ("/Game/Blueprints/"), a module path ("/Script/Niagara.NiagaraDataInterface", 36, the longest
	 *  the commands add), a suffix ("_Sub", "_Font", the "_N" of a unique name)... A name of NameLengthLimit
	 *  characters or more cannot be made a name safely. */
	static constexpr int32 NameAffixMargin = 64;
	static constexpr int32 NameLengthLimit = NAME_SIZE - NameAffixMargin;

	/** Characters the path of an object a tool creates inside an asset may take past the asset's object path and the
	 *  name the call gives it: "_C:" + "_GEN_VARIABLE" around a component template (16), ":WidgetTree." before a
	 *  widget (12, "_C:WidgetTree." in the compiled class), a slot after a named panel (33), the nodes of a named
	 *  graph (47), and the graphs and nodes around a named anim state machine, the longest (126 with "_0" indices).
	 *  The same as mcp_common::ObjectPathSubobjectMargin, which keeps an asset's own subobjects in the limit. */
	static constexpr int32 SubobjectPathMargin = 160;

	/**
	 * The first name argument of NameLengthLimit characters or more, as an error message naming where it sits, its
	 * length and the limit: a name-like parameter (FParam::bNameLike) that is a string or holds one as an array
	 * element, or a string at any depth inside an argument under an object key IsNameLikeKey passes, or a string
	 * element of an array under such a key ("renames[0].new_name"). Then the pair a subobject is named after: the
	 * longest name-like string argument that is a content path (it starts with "/"; an object path counts up to its
	 * asset, a package path as given) plus the longest other name-like string (or string element) plus
	 * SubobjectPathMargin, which must stay under NAME_SIZE. A batch's items are left to their own dispatch. Empty when
	 * every one fits or the catalog does not know CommandType. Game thread (McpPreconditions::Check).
	 */
	static FString CheckNameLengths(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

	/**
	 * Whether a key of an object inside an argument names a name, by the rule the catalog generator marks name-like
	 * params with (`nameLikeRule` in the catalog): the key split at "_" and at each camelCase hump ("newName",
	 * "AssetPath", "HTTPServer" is http + server), in lower case, without a trailing number or single letter, ends in
	 * a name word and holds no disk word; in a domain whose paths are not names (USD) only an actor key passes. False
	 * when the catalog carries no rule.
	 */
	bool IsNameLikeKey(const FString& Key, const FString& Domain) const;

	/**
	 * The first argument of a vector parameter (FParam::VectorShape) that does not have its shape: null, not an array
	 * (for a rotator, not an array or an object), an array of another length, an element that is not a finite JSON
	 * number within the float range (the array form is read through float), or a rotator object with a key other
	 * than roll, pitch or yaw or an axis that is not a finite JSON number. A null is refused because the handlers
	 * that test the param with HasField read it as present, as 0, 0, 0; tools/call and call_tool drop a null argument
	 * before the dispatch, so a null reaches this check from a batch item's params or an in-process call. Returned as
	 * an error message naming the parameter, what was given and the shape expected; empty when every one has its
	 * shape or is absent, or the catalog does not know CommandType. Game thread (McpPreconditions::Check).
	 */
	static FString CheckVectorShapes(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

	/** Does the parameter NAME read as a rotation: rot, *rotation or *rotator? */
	static bool LooksLikeRotationName(const FString& ParamName);

	/**
	 * Rewrites in place the {roll,pitch,yaw} object form into the wire array [pitch, yaw, roll]
	 * for CommandType's rotation parameters. Only touches objects whose keys are ALL axes and whose
	 * values are numbers: any other shape is left intact for the handler to decide. Game thread
	 * (called by DispatchSingleCommand, so it also covers `execute_batch`). Returns how many parameters
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
	/** The catalog's `nameLikeRule` (IsNameLikeKey); bHasNameRule is false when the catalog has none. */
	bool bHasNameRule = false;
	TSet<FString> NameWords;
	TSet<FString> NameDiskWords;
	TArray<FString> NameDiskSuffixes;
	TSet<FString> NotNameDomains;
	FString NotNameDomainKeepWord;
};
