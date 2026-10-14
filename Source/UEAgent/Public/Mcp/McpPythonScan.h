#pragma once

#include "CoreMinimal.h"

/**
 * STATIC reading of the Python code sent through `execute_python`, without running it.
 *
 * Used by two families of checks: the refusals that prevent a crash (positional Rotator,
 * delete+rename of subobjects, attribute_set_types, merge_meshes, TextureExporterPNG,
 * request_auto_rigging) and the readback warnings for execute_python.
 *
 * It is not a Python parser: it is a TOKENIZER (strings, comments, numbers, operators, brackets)
 * plus a few reads over the tokens. That is enough because what is looked for is always CALLS
 * (`a.b.c(...)` and their arguments) and assignments to an attribute, and the tokenizer already
 * separates code from strings and comments -- which is why a grep would not do: a
 * `rename_subobject` inside a string is a literal, not a call.
 *
 * Known limits compared with a real parser:
 *   - expressions INSIDE an f-string are not inspected (the f-string is opaque);
 *   - a script with a syntax error that neither unbalances brackets nor leaves a string open is
 *     analysed anyway. It would not run either way.
 * If the text cannot be tokenized (unterminated string, unbalanced brackets) the reads return
 * empty: such code does not compile, so it does not run.
 */
namespace McpPythonScan
{
	enum class ETokenType : uint8
	{
		Name,
		Number,
		String,
		Op,
		Newline,
	};

	struct FToken
	{
		ETokenType Type = ETokenType::Op;
		/** The token's source text (for a string, the whole literal with prefix and quotes). */
		FString Text;
		/** Strings: the decoded content (without prefix or quotes). */
		FString StringValue;
		bool bBytes = false;
		bool bFString = false;
		/** Range in the source code: [Begin, End). */
		int32 Begin = 0;
		int32 End = 0;
	};

	/** false if the text cannot be tokenized: it would not compile, so it would not run. */
	UEAGENT_API bool Tokenize(const FString& Code, TArray<FToken>& OutTokens);

	// NOTE: Python is case-sensitive and FString::operator== is NOT (nor TSet<FString>): everything
	// here compares with ESearchCase::CaseSensitive. That is why these return arrays, not sets.

	/** Names of the functions the script CALLS: for `a.b.c(...)` the last segment; for `f(...)`,
	 *  f. Reading an attribute is NOT a call. No duplicates. */
	UEAGENT_API TArray<FString> CalledNames(const FString& Code);

	/** String literals passed AS AN ARGUMENT to a call. No duplicates. */
	UEAGENT_API TArray<FString> CallStringArguments(const FString& Code);

	/** Is Name in Names (case-sensitive)? */
	UEAGENT_API bool ContainsExact(const TArray<FString>& Names, const FString& Name);

	/** Calls to `unreal.Rotator(...)` / `Rotator(...)` with POSITIONAL arguments that are not all
	 *  zero, as they appear in the source. */
	UEAGENT_API TArray<FString> PositionalRotatorCalls(const FString& Code);

	/** The places where the script WRITES `PropertyName`: set_editor_property with a positional or
	 *  named argument, and assignment to the attribute. Reading does not count. */
	UEAGENT_API TArray<FString> PropertyWrites(const FString& Code, const FString& PropertyName);
}
