//
// The USD values that UE::FVtValue and the engine's value conversion (UsdToUnreal/UnrealToUsd::ConvertValue) cannot
// read or build, handled with the USD SDK itself: list ops of int, uint, int64 and uint64 (SdfIntListOp,
// SdfUIntListOp, SdfInt64ListOp, SdfUInt64ListOp), path expressions built from text (SdfPathExpression) and arrays of
// path expressions (VtArray<SdfPathExpression>), read element by element.
//
// Only UE types cross this API, so a module built without RTTI (which cannot include a pxr header) can call it. When
// the engine is built without the USD SDK (USE_USD_SDK 0) every function fails with "USD SDK not available".
//

#pragma once

#include "CoreMinimal.h"
#include "UsdWrappers/VtValue.h"

namespace UEAgent::UsdSdk
{
	/** The item type of a list op of integers; None for any other value. */
	enum class EIntListOpItems : uint8
	{
		None,
		Int,     // SdfIntListOp, SdfListOp<int>
		UInt,    // SdfUIntListOp, SdfListOp<unsigned int>
		Int64,   // SdfInt64ListOp, SdfListOp<int64_t>
		UInt64,  // SdfUInt64ListOp, SdfListOp<uint64_t>
	};

	/** The item lists of a list op, in the order of SdfListOpType. */
	enum class EListOpList : uint8
	{
		Explicit,
		Added,
		Deleted,
		Ordered,
		Prepended,
		Appended,
		Num
	};

	constexpr int32 ListOpListCount = static_cast<int32>(EListOpList::Num);

	/**
	 * A list op of integers as UE arrays. Each item is held in a 64-bit array of its signedness, which holds every
	 * value of the item type exactly: a list op of int or int64 keeps its items in Signed, one of uint or uint64 in
	 * Unsigned, both indexed by EListOpList, and the other set is empty. A list op in explicit mode (bExplicit) has
	 * only its Explicit list, possibly empty; any other has the five other lists.
	 */
	struct FIntListOp
	{
		EIntListOpItems Items = EIntListOpItems::None;
		bool bExplicit = false;
		TArray<int64> Signed[ListOpListCount];
		TArray<uint64> Unsigned[ListOpListCount];
	};

	/** The item type of the integer list op Value holds: None for an empty value or a value of any other type. */
	UEAGENTUSDSDK_API EIntListOpItems GetIntListOpItems(const UE::FVtValue& Value);

	/** Reads the integer list op Value holds into Out (its item type, mode and lists). False, with OutError, for any other value. */
	UEAGENTUSDSDK_API bool ReadIntListOp(const UE::FVtValue& Value, FIntListOp& Out, FString& OutError);

	/**
	 * Builds into Out the list op of In.Items from In's lists: the Explicit list in explicit mode, else the five others.
	 * False, with OutError and Out unchanged, for In.Items None, an item outside the range of the item type, a list
	 * that holds an item twice (USD would drop the repeat; its message is reported), or explicit items in a list op
	 * that is not explicit (or other items in one that is).
	 */
	UEAGENTUSDSDK_API bool MakeIntListOp(const FIntListOp& In, UE::FVtValue& Out, FString& OutError);

	/** Whether Value holds an array of path expressions (VtArray<SdfPathExpression>). */
	UEAGENTUSDSDK_API bool IsPathExpressionArray(const UE::FVtValue& Value);

	/**
	 * The text (SdfPathExpression::GetText, which parses back to the same expression) of each of the first MaxElements
	 * path expressions of the array Value holds, in OutTexts, and the element count of the whole array in OutLength.
	 * False, with OutError, when Value holds no array of path expressions.
	 */
	UEAGENTUSDSDK_API bool ReadPathExpressionArray(const UE::FVtValue& Value, int32 MaxElements, TArray<FString>& OutTexts,
		int32& OutLength, FString& OutError);

	/**
	 * Parses Text into a path expression held in Out; an empty text is the empty expression, which matches nothing.
	 * False, with USD's parse error in OutError and Out unchanged, for a text that does not parse.
	 */
	UEAGENTUSDSDK_API bool MakePathExpression(const FString& Text, UE::FVtValue& Out, FString& OutError);

	/**
	 * Parses each of Texts into one path expression of an array (VtArray<SdfPathExpression>) held in Out. False, with
	 * OutError naming the index of the first text that does not parse and USD's parse error, and Out unchanged.
	 */
	UEAGENTUSDSDK_API bool MakePathExpressionArray(const TArray<FString>& Texts, UE::FVtValue& Out, FString& OutError);
}
