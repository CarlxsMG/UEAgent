#include "UEAgentUsdSdk.h"

#include "Containers/StringConv.h"
#include "Modules/ModuleManager.h"
#include "USDMemory.h"

#if USE_USD_SDK
#include "USDIncludesStart.h"
#include "pxr/base/vt/array.h"
#include "pxr/base/vt/value.h"
#include "pxr/usd/sdf/listOp.h"
#include "pxr/usd/sdf/pathExpression.h"
#include "USDIncludesEnd.h"
#endif	// USE_USD_SDK

namespace UEAgent::UsdSdk
{
#if USE_USD_SDK
	namespace Private
	{
		static_assert(static_cast<int32>(EListOpList::Explicit) == pxr::SdfListOpTypeExplicit, "EListOpList follows SdfListOpType");
		static_assert(static_cast<int32>(EListOpList::Added) == pxr::SdfListOpTypeAdded, "EListOpList follows SdfListOpType");
		static_assert(static_cast<int32>(EListOpList::Deleted) == pxr::SdfListOpTypeDeleted, "EListOpList follows SdfListOpType");
		static_assert(static_cast<int32>(EListOpList::Ordered) == pxr::SdfListOpTypeOrdered, "EListOpList follows SdfListOpType");
		static_assert(static_cast<int32>(EListOpList::Prepended) == pxr::SdfListOpTypePrepended, "EListOpList follows SdfListOpType");
		static_assert(static_cast<int32>(EListOpList::Appended) == pxr::SdfListOpTypeAppended, "EListOpList follows SdfListOpType");

		const TCHAR* ListName(int32 List)
		{
			static const TCHAR* const Names[ListOpListCount] = {
				TEXT("explicit"), TEXT("added"), TEXT("deleted"), TEXT("ordered"), TEXT("prepended"), TEXT("appended") };
			return Names[List];
		}

		FString ToUnreal(const std::string& Text)
		{
			return FString(StringCast<TCHAR>(reinterpret_cast<const UTF8CHAR*>(Text.c_str())).Get());
		}

		std::string ToUsd(const FString& Text)
		{
			const auto Utf8 = StringCast<UTF8CHAR>(*Text);
			return std::string(reinterpret_cast<const char*>(Utf8.Get()), Utf8.Length());
		}

		/** Copies the lists of ListOp into OutLists, each item widened to the 64-bit type of its signedness. */
		template <typename ItemType, typename WideType>
		void ReadLists(const pxr::SdfListOp<ItemType>& ListOp, TArray<WideType> (&OutLists)[ListOpListCount])
		{
			for (int32 List = 0; List < ListOpListCount; ++List)
			{
				const std::vector<ItemType>& Items = ListOp.GetItems(static_cast<pxr::SdfListOpType>(List));
				OutLists[List].Reset(static_cast<int32>(Items.size()));
				for (const ItemType Item : Items)
				{
					OutLists[List].Add(static_cast<WideType>(Item));
				}
			}
		}

		/**
		 * The list op of ItemType that Lists give: the Explicit list in explicit mode, else the five others, each narrowed
		 * from its 64-bit type. False, with OutError, for an item outside the range of ItemType, a list holding an item
		 * twice, or a list the mode does not have.
		 */
		template <typename ItemType, typename WideType>
		bool MakeListOp(const TArray<WideType> (&Lists)[ListOpListCount], bool bExplicit, pxr::VtValue& OutValue, FString& OutError)
		{
			pxr::SdfListOp<ItemType> ListOp;
			if (bExplicit)
			{
				ListOp.ClearAndMakeExplicit();
			}
			for (int32 List = 0; List < ListOpListCount; ++List)
			{
				const bool bExplicitList = List == static_cast<int32>(EListOpList::Explicit);
				if (bExplicitList != bExplicit)
				{
					if (Lists[List].Num() > 0)
					{
						OutError = FString::Printf(TEXT("a list op %s explicit mode has no %s items"), bExplicit ? TEXT("in") : TEXT("out of"),
							ListName(List));
						return false;
					}
					continue;
				}
				std::vector<ItemType> Items;
				Items.reserve(Lists[List].Num());
				for (int32 Index = 0; Index < Lists[List].Num(); ++Index)
				{
					const WideType Item = Lists[List][Index];
					if (static_cast<WideType>(static_cast<ItemType>(Item)) != Item)
					{
						OutError = FString::Printf(TEXT("item %d of list '%s' is outside the range of the list op's items"), Index, ListName(List));
						return false;
					}
					Items.push_back(static_cast<ItemType>(Item));
				}
				if (Items.empty())
				{
					continue;  // an explicit list op keeps its empty explicit list
				}
				std::string Error;
				if (!ListOp.SetItems(Items, static_cast<pxr::SdfListOpType>(List), &Error))
				{
					OutError = FString::Printf(TEXT("list '%s': %s"), ListName(List), *ToUnreal(Error));
					return false;
				}
			}
			OutValue = pxr::VtValue(MoveTemp(ListOp));
			return true;
		}

		/** The path expression Text parses to. False, with USD's parse error in OutError, when it does not parse. */
		bool ParsePathExpression(const FString& Text, pxr::SdfPathExpression& OutExpression, FString& OutError)
		{
			OutExpression = pxr::SdfPathExpression(ToUsd(Text));
			if (!OutExpression.GetParseError().empty())
			{
				OutError = ToUnreal(OutExpression.GetParseError());
				return false;
			}
			return true;
		}
	}

	EIntListOpItems GetIntListOpItems(const UE::FVtValue& Value)
	{
		const pxr::VtValue& UsdValue = Value.GetUsdValue();
		if (UsdValue.IsHolding<pxr::SdfIntListOp>())
		{
			return EIntListOpItems::Int;
		}
		if (UsdValue.IsHolding<pxr::SdfUIntListOp>())
		{
			return EIntListOpItems::UInt;
		}
		if (UsdValue.IsHolding<pxr::SdfInt64ListOp>())
		{
			return EIntListOpItems::Int64;
		}
		if (UsdValue.IsHolding<pxr::SdfUInt64ListOp>())
		{
			return EIntListOpItems::UInt64;
		}
		return EIntListOpItems::None;
	}

	bool ReadIntListOp(const UE::FVtValue& Value, FIntListOp& Out, FString& OutError)
	{
		FScopedUsdAllocs UsdAllocs;
		const pxr::VtValue& UsdValue = Value.GetUsdValue();
		Out = FIntListOp();
		Out.Items = GetIntListOpItems(Value);
		switch (Out.Items)
		{
			case EIntListOpItems::Int:
				Out.bExplicit = UsdValue.UncheckedGet<pxr::SdfIntListOp>().IsExplicit();
				Private::ReadLists(UsdValue.UncheckedGet<pxr::SdfIntListOp>(), Out.Signed);
				return true;
			case EIntListOpItems::UInt:
				Out.bExplicit = UsdValue.UncheckedGet<pxr::SdfUIntListOp>().IsExplicit();
				Private::ReadLists(UsdValue.UncheckedGet<pxr::SdfUIntListOp>(), Out.Unsigned);
				return true;
			case EIntListOpItems::Int64:
				Out.bExplicit = UsdValue.UncheckedGet<pxr::SdfInt64ListOp>().IsExplicit();
				Private::ReadLists(UsdValue.UncheckedGet<pxr::SdfInt64ListOp>(), Out.Signed);
				return true;
			case EIntListOpItems::UInt64:
				Out.bExplicit = UsdValue.UncheckedGet<pxr::SdfUInt64ListOp>().IsExplicit();
				Private::ReadLists(UsdValue.UncheckedGet<pxr::SdfUInt64ListOp>(), Out.Unsigned);
				return true;
			default:
				OutError = FString::Printf(TEXT("the value, of type %s, is not a list op of integers"), *Value.GetTypeName());
				return false;
		}
	}

	bool MakeIntListOp(const FIntListOp& In, UE::FVtValue& Out, FString& OutError)
	{
		FScopedUsdAllocs UsdAllocs;
		pxr::VtValue UsdValue;
		bool bMade = false;
		switch (In.Items)
		{
			case EIntListOpItems::Int: bMade = Private::MakeListOp<int>(In.Signed, In.bExplicit, UsdValue, OutError); break;
			case EIntListOpItems::UInt: bMade = Private::MakeListOp<unsigned int>(In.Unsigned, In.bExplicit, UsdValue, OutError); break;
			case EIntListOpItems::Int64: bMade = Private::MakeListOp<int64_t>(In.Signed, In.bExplicit, UsdValue, OutError); break;
			case EIntListOpItems::UInt64: bMade = Private::MakeListOp<uint64_t>(In.Unsigned, In.bExplicit, UsdValue, OutError); break;
			default: OutError = TEXT("no item type was given for the list op"); break;
		}
		if (bMade)
		{
			Out = MoveTemp(UsdValue);
		}
		return bMade;
	}

	bool IsPathExpressionArray(const UE::FVtValue& Value)
	{
		return Value.GetUsdValue().IsHolding<pxr::VtArray<pxr::SdfPathExpression>>();
	}

	bool ReadPathExpressionArray(const UE::FVtValue& Value, int32 MaxElements, TArray<FString>& OutTexts, int32& OutLength, FString& OutError)
	{
		FScopedUsdAllocs UsdAllocs;
		OutTexts.Reset();
		OutLength = 0;
		const pxr::VtValue& UsdValue = Value.GetUsdValue();
		if (!UsdValue.IsHolding<pxr::VtArray<pxr::SdfPathExpression>>())
		{
			OutError = FString::Printf(TEXT("the value, of type %s, is not an array of path expressions"), *Value.GetTypeName());
			return false;
		}
		const pxr::VtArray<pxr::SdfPathExpression>& Expressions = UsdValue.UncheckedGet<pxr::VtArray<pxr::SdfPathExpression>>();
		OutLength = static_cast<int32>(FMath::Min<size_t>(Expressions.size(), static_cast<size_t>(MAX_int32)));
		const int32 Count = FMath::Min(OutLength, FMath::Max(0, MaxElements));
		OutTexts.Reserve(Count);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			OutTexts.Add(Private::ToUnreal(Expressions.cdata()[Index].GetText()));
		}
		return true;
	}

	bool MakePathExpression(const FString& Text, UE::FVtValue& Out, FString& OutError)
	{
		FScopedUsdAllocs UsdAllocs;
		pxr::SdfPathExpression Expression;
		if (!Private::ParsePathExpression(Text, Expression, OutError))
		{
			return false;
		}
		Out = pxr::VtValue(MoveTemp(Expression));
		return true;
	}

	bool MakePathExpressionArray(const TArray<FString>& Texts, UE::FVtValue& Out, FString& OutError)
	{
		FScopedUsdAllocs UsdAllocs;
		pxr::VtArray<pxr::SdfPathExpression> Expressions;
		Expressions.reserve(Texts.Num());
		for (int32 Index = 0; Index < Texts.Num(); ++Index)
		{
			pxr::SdfPathExpression Expression;
			FString ParseError;
			if (!Private::ParsePathExpression(Texts[Index], Expression, ParseError))
			{
				OutError = FString::Printf(TEXT("element %d ('%s') does not parse as a path expression: %s"), Index, *Texts[Index], *ParseError);
				return false;
			}
			Expressions.push_back(MoveTemp(Expression));
		}
		Out = pxr::VtValue(MoveTemp(Expressions));
		return true;
	}

#else  // USE_USD_SDK

	namespace Private
	{
		const TCHAR* const NoSdk = TEXT("USD SDK not available");
	}

	EIntListOpItems GetIntListOpItems(const UE::FVtValue& Value)
	{
		return EIntListOpItems::None;
	}

	bool ReadIntListOp(const UE::FVtValue& Value, FIntListOp& Out, FString& OutError)
	{
		OutError = Private::NoSdk;
		return false;
	}

	bool MakeIntListOp(const FIntListOp& In, UE::FVtValue& Out, FString& OutError)
	{
		OutError = Private::NoSdk;
		return false;
	}

	bool IsPathExpressionArray(const UE::FVtValue& Value)
	{
		return false;
	}

	bool ReadPathExpressionArray(const UE::FVtValue& Value, int32 MaxElements, TArray<FString>& OutTexts, int32& OutLength, FString& OutError)
	{
		OutTexts.Reset();
		OutLength = 0;
		OutError = Private::NoSdk;
		return false;
	}

	bool MakePathExpression(const FString& Text, UE::FVtValue& Out, FString& OutError)
	{
		OutError = Private::NoSdk;
		return false;
	}

	bool MakePathExpressionArray(const TArray<FString>& Texts, UE::FVtValue& Out, FString& OutError)
	{
		OutError = Private::NoSdk;
		return false;
	}

#endif  // USE_USD_SDK
}

// IMPLEMENT_MODULE_USD gives the module the operator new and delete the USD SDK's allocations need where the engine
// requires them (see USDMemory.h); elsewhere it is IMPLEMENT_MODULE.
IMPLEMENT_MODULE_USD(FDefaultModuleImpl, UEAgentUsdSdk);
