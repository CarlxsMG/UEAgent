// Copyright (c) 2026 CarlxsMG.

#pragma once

#include "CoreMinimal.h"
#include "Mcp/McpReadback.h"
#include "../McpReadbackText.h"

/**
 * Shared by the READBACK check families: the Python-style shortcuts that almost every check uses
 * (`a or b`, joining without duplicates, `int(x)`), the text and pin readers several families
 * need, the builders of the finding shapes they return, and the warnings one family uses as the
 * fallback of another.
 *
 * NOTE: the contract test vectors (Resources/ContractTests) compare each check's output field by
 * field, so a change in a check's logic must update its vectors in the same change.
 */
namespace McpReadbackUtil
{
	using McpPy::FV;
	using McpPy::FO;

	/** `a or b`: the first value if truthy, otherwise the second. */
	inline FV Or(const FV& A, const FV& B) { return McpPy::Truthy(A) ? A : B; }
	inline FV Or(const FV& A, const FV& B, const FV& C) { return Or(Or(A, B), C); }

	/** `isinstance(x, int)` WITHOUT excluding bool (in Python, True is an int). */
	inline bool IsIntOrBool(const FV& Value) { return McpPy::IsInt(Value) || McpPy::IsBool(Value); }

	/** `int(x)` on a number: truncates toward zero. */
	inline FV PyInt(const FV& Value) { return McpPy::N(FMath::TruncToDouble(McpPy::AsNum(Value))); }

	/** `x.strip().lower() if isinstance(x, str) else ''` */
	inline FString LowerStrip(const FV& Value) { return McpPy::IsStr(Value) ? McpPy::Lower(McpPy::Strip(Value->AsString())) : FString(); }

	/** The first non-empty trimmed text of the two. */
	inline FString FirstNonEmptyText(const FV& A, const FV& B)
	{
		const FString FirstText = McpPy::TextOf(A);
		return FirstText.IsEmpty() ? McpPy::TextOf(B) : FirstText;
	}

	/** The trimmed text of a field, as a value. */
	inline FV TextField(const FO& Object, const TCHAR* Key)
	{
		return McpPy::S(McpPy::TextOf(McpPy::Get(Object, Key)));
	}

	/** Joins the parts without duplicates, in order of appearance. */
	inline FString JoinUnique(const TArray<FString>& Parts, const TCHAR* Separator)
	{
		TArray<FString> Unique;
		for (const FString& Part : Parts)
		{
			if (!McpPy::InList(Unique, Part))
			{
				Unique.Add(Part);
			}
		}
		return McpPy::Join(Unique, Separator);
	}

	/** `list[:n]` */
	template <typename T>
	inline TArray<T> Head(const TArray<T>& Values, int32 Count)
	{
		TArray<T> Out;
		for (int32 Index = 0; Index < Values.Num() && Index < Count; ++Index)
		{
			Out.Add(Values[Index]);
		}
		return Out;
	}

	/** `', '.join(str(a) for a in values)` */
	inline FString JoinStr(const TArray<FV>& Values, const TCHAR* Separator)
	{
		TArray<FString> Parts;
		for (const FV& Value : Values)
		{
			Parts.Add(McpPy::Str(Value));
		}
		return McpPy::Join(Parts, Separator);
	}

	/** `key in dict` */
	inline bool Has(const FO& Object, const TCHAR* Key) { return Object.IsValid() && Object->HasField(Key); }

	// --- pins --------------------------------------------------------------------------------
	/** The category of a pin, struct member or variable (its "category", else its "type"), lowercased. */
	inline FString PinCategory(const FV& Pin)
	{
		for (const TCHAR* Key : { TEXT("category"), TEXT("type") })
		{
			const FV Value = McpPy::Get(Pin, Key);
			if (McpPy::IsStr(Value) && !McpPy::AsStr(Value).IsEmpty())
			{
				return McpPy::Lower(McpPy::Strip(McpPy::AsStr(Value)));
			}
		}
		return FString();
	}

	/** Whether the pin is an input pin. */
	inline bool IsInputPin(const FV& Pin)
	{
		const FV Direction = McpPy::Get(Pin, TEXT("direction"));
		return McpPy::IsStr(Direction) && McpPy::StrEq(McpPy::AsStr(Direction), TEXT("input"));
	}

	// --- findings ----------------------------------------------------------------------------
	/** Verdict for an echoed write: "wrote_nothing" if a non-empty request stored nothing, "unchanged" if
	 *  a requested bool reads back as the opposite value, "" otherwise. */
	inline FString CheckNonEmptyStored(const FO& Params, const FV& Stored, const FString& RequestKey)
	{
		const FV Requested = McpPy::Get(Params, RequestKey);
		if (McpPy::IsEmpty(Requested))
		{
			return FString();
		}
		if (McpPy::IsEmpty(Stored))
		{
			return TEXT("wrote_nothing");
		}
		if (McpPy::IsBool(Requested) && McpPy::IsBool(Stored) && Requested->AsBool() != Stored->AsBool())
		{
			return TEXT("unchanged");
		}
		return FString();
	}

	/** A finding that degrades `success`: `{verdict, requested, stored, how, note}`. */
	inline FO DegradingFinding(const FString& Verdict, const FV& Requested, const FV& Stored, const FString& How, const FString& Note)
	{
		return McpPy::Dict({
			{ TEXT("verdict"), McpPy::S(Verdict) },
			{ TEXT("requested"), Requested },
			{ TEXT("stored"), Stored },
			{ TEXT("how"), McpPy::S(How) },
			{ TEXT("note"), McpPy::S(Note) },
		});
	}

	/** A degrading finding whose error text replaces the generic one:
	 *  `{verdict, requested, stored, how, error, note}`. */
	inline FO DegradingFindingWithError(const FString& Verdict, const FV& Requested, const FV& Stored, const FString& How, const FString& Error,
		const FString& Note)
	{
		return McpPy::Dict({
			{ TEXT("verdict"), McpPy::S(Verdict) },
			{ TEXT("requested"), Requested },
			{ TEXT("stored"), Stored },
			{ TEXT("how"), McpPy::S(How) },
			{ TEXT("error"), McpPy::S(Error) },
			{ TEXT("note"), McpPy::S(Note) },
		});
	}

	/** A one-line warning finding, which informs without touching `success`:
	 *  `{verdict, degrade: False, requested, stored, how, note}`. */
	inline FO WarningFinding(const FString& Verdict, const FV& Requested, const FV& Stored, const FString& How, const FString& Note)
	{
		return McpPy::Dict({
			{ TEXT("verdict"), McpPy::S(Verdict) },
			{ TEXT("degrade"), McpPy::B(false) },
			{ TEXT("requested"), Requested },
			{ TEXT("stored"), Stored },
			{ TEXT("how"), McpPy::S(How) },
			{ TEXT("note"), McpPy::S(Note) },
		});
	}

	/** A warning finding whose note joins the warnings, or null when there are none. */
	inline FO JoinedWarningFinding(const FString& Verdict, const FV& Requested, const FV& Stored, const FString& How,
		const TArray<FString>& Warnings)
	{
		return Warnings.Num() == 0 ? nullptr : WarningFinding(Verdict, Requested, Stored, How, McpPy::Join(Warnings, TEXT(" | ")));
	}

	// --- warnings one family uses as another's fallback (each defined in its own file) -------
	FO WarnArrayNotPropagated(const FO& Params, const FO& Result);
	FO WarnCollisionIni(const FO& Params, const FO& Result);
	FO WarnArrayRotation(const FO& Params, const FO& Result);
	FO CheckPhysicalMaterialWithoutComplex(const FO& Params, const FO& Result);
}
