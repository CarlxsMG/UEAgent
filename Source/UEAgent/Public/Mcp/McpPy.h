#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/**
 * Python value semantics over the engine's JSON values, used by the readback and precondition
 * contract.
 *
 * The contract's messages quote values (`str(x)`, `repr(x)`, `%s`, positional templates) and its
 * test vectors compare the output byte for byte, so turning a value into text follows Python's
 * rules, not Unreal's JSON printer. The predicates follow Python too (`isinstance(x, int)`,
 * `x is False`, truthiness) and keep Python's names.
 *
 * NOTE: an integral JSON number is treated as a Python `int`: the bridge writes integral doubles
 * without decimals ("4071"), so that is how they read back.
 */
namespace McpPy
{
	using FV = TSharedPtr<FJsonValue>;
	using FO = TSharedPtr<FJsonObject>;

	// --- construction ---------------------------------------------------------------------
	UEAGENT_API FV None();
	UEAGENT_API FV S(const FString& Value);
	UEAGENT_API FV N(double Value);
	UEAGENT_API FV B(bool Value);
	UEAGENT_API FV L(const TArray<FV>& Values);
	UEAGENT_API FV L(const TArray<FString>& Values);
	UEAGENT_API FV O(const FO& Object);
	/** A dict in insertion order: `Dict({{TEXT("verdict"), S(...)}, ...})`. */
	UEAGENT_API FO Dict(std::initializer_list<TPair<FString, FV>> Fields);

	// --- types (isinstance) -----------------------------------------------------------------
	UEAGENT_API bool IsNone(const FV& Value);
	UEAGENT_API bool IsStr(const FV& Value);
	UEAGENT_API bool IsBool(const FV& Value);
	/** isinstance(x, (int, float)) and not isinstance(x, bool) */
	UEAGENT_API bool IsNum(const FV& Value);
	/** isinstance(x, int) and not isinstance(x, bool): an integral number. */
	UEAGENT_API bool IsInt(const FV& Value);
	UEAGENT_API bool IsList(const FV& Value);
	UEAGENT_API bool IsDict(const FV& Value);
	/** `x is True` / `x is False` */
	UEAGENT_API bool IsTrue(const FV& Value);
	UEAGENT_API bool IsFalse(const FV& Value);

	// --- access -------------------------------------------------------------------------------
	/** dict.get(key): None if missing or if the value is not a dict. */
	UEAGENT_API FV Get(const FO& Object, const FString& Key);
	UEAGENT_API FV Get(const FV& Object, const FString& Key);
	/** The dict, or null if the value is not one. */
	UEAGENT_API FO AsDict(const FV& Value);
	/** The items, or empty if the value is not a list. */
	UEAGENT_API TArray<FV> AsList(const FV& Value);
	UEAGENT_API FString AsStr(const FV& Value);
	UEAGENT_API double AsNum(const FV& Value);

	// --- semantics ----------------------------------------------------------------------------
	/** bool(x) */
	UEAGENT_API bool Truthy(const FV& Value);
	/** len(x) for strings, lists and dicts (0 otherwise). */
	UEAGENT_API int32 Len(const FV& Value);
	/** str(x) */
	UEAGENT_API FString Str(const FV& Value);
	/** repr(x) */
	UEAGENT_API FString Repr(const FV& Value);
	UEAGENT_API FString ReprStr(const FString& Value);
	/** Python's repr(float): the shortest text that round-trips to the same double, with Python's notation rules. */
	UEAGENT_API FString FloatRepr(double Value);
	/** Python equality (1 == 1.0; lists and dicts compare by content). */
	UEAGENT_API bool Equals(const FV& A, const FV& B);

	// --- text ----------------------------------------------------------------------------------
	/** A positional template: `{0}`, `{1}`... are replaced; `{{` and `}}` are literal braces. */
	UEAGENT_API FString F(const TCHAR* Template, const TArray<FString>& Args);
	/** `template % args`: %s %r %d %i %f %.Nf %x %%. */
	UEAGENT_API FString Percent(const TCHAR* Template, const TArray<FV>& Args);
	UEAGENT_API FString Join(const TArray<FString>& Parts, const FString& Separator);
	/** sorted() for strings: code-point order, case-sensitive. */
	UEAGENT_API TArray<FString> Sorted(TArray<FString> Values);
	/** str.split(sep) / str.split(sep, maxsplit) */
	UEAGENT_API TArray<FString> Split(const FString& Value, const FString& Separator, int32 MaxSplit = -1);
	/** str.rsplit(sep, maxsplit) */
	UEAGENT_API TArray<FString> RSplit(const FString& Value, const FString& Separator, int32 MaxSplit = -1);
	/** Python string comparisons: case-sensitive. */
	UEAGENT_API bool StrEq(const FString& A, const FString& B);
	UEAGENT_API bool StartsWith(const FString& Value, const FString& Prefix);
	UEAGENT_API bool EndsWith(const FString& Value, const FString& Suffix);
	UEAGENT_API bool Contains(const FString& Value, const FString& Needle);
	UEAGENT_API bool InList(const TArray<FString>& Values, const FString& Value);
	/** Python's str.strip() (Unicode whitespace). */
	UEAGENT_API FString Strip(const FString& Value);
	UEAGENT_API FString Lower(const FString& Value);

	// --- contract helpers ----------------------------------------------------------
	/**
	 * The command payload: the bridge wraps SOME responses in `data` and not others, so both are
	 * accepted -- except a `data` object that is ONLY the bridge's parameter report (aliased_params /
	 * unknown_params): that is not the payload, and it can appear on any response.
	 */
	UEAGENT_API FO Payload(const FO& Reply);
	UEAGENT_API FO Payload(const FV& Reply);
	/** The trimmed string, or "" if the value is not a string. */
	UEAGENT_API FString TextOf(const FV& Value);
	/** Empty: None, an empty collection, or "", "()", "none", "null". 0 and False are NOT empty. */
	UEAGENT_API bool IsEmpty(const FV& Value);

	/** Deep copy. */
	UEAGENT_API FV Clone(const FV& Value);
	UEAGENT_API FO Clone(const FO& Value);
	/** Compact JSON with sorted keys, to compare values regardless of key order. */
	UEAGENT_API FString Canonical(const FV& Value);
}
