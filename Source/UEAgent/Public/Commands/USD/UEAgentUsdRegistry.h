//
// State and helpers shared by the USD MCP handlers.
//
// GStageNameCache maps the stage_name a command names to its UE::FUsdStage. It is a C++17 inline
// variable, so every handler that includes this header reads and writes the same map: a stage that
// create_usd_stage or open_usd_stage registers is the one define_usd_prim and the rest find.
//
// Thread-safety: USD stages are owned by the editor's main thread, where the bridge runs every command
// (FTSTicker on the game thread), so the map needs no synchronization.

#pragma once

#include "CoreMinimal.h"
#include "Misc/EngineVersionComparison.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Math/Vector2DHalf.h"
#include "Templates/Function.h"
#include "UnrealUSDWrapper.h"
#include "UsdWrappers/SdfLayer.h"
#include "UsdWrappers/SdfPath.h"
#include "UsdWrappers/UsdStage.h"
#include "UsdWrappers/VtValue.h"
#include "USDValueConversion.h"
#include "UEAgentUsdSdk.h"  // the values the wrapper cannot convert (integer list ops, path expressions), through the USD SDK

// UsdUtils::StartMonitoringErrors / GetErrorsAndStopMonitoring are deprecated, yet they are the only API
// that returns the text of the captured USD errors.
PRAGMA_DISABLE_DEPRECATION_WARNINGS
#include "USDErrorUtils.h"
PRAGMA_ENABLE_DEPRECATION_WARNINGS

namespace UEAgent::UsdRegistry
{
	inline TMap<FString, UE::FUsdStage> GStageNameCache{};

	/**
	 * The stage StageName names: its registry entry or, when the registry has none, the stage of the
	 * wrapper's own cache whose root layer identifier ends with or equals StageName (ignoring case), which
	 * is then registered under StageName. Empty on success; otherwise why there is no stage: no name, an
	 * entry whose stage has gone invalid (the entry is dropped; StaleLabel opens that message), or no match.
	 */
	inline FString FindNamedStage(const FString& StageName, const TCHAR* StaleLabel, UE::FUsdStage& OutStage)
	{
		if (StageName.IsEmpty())
		{
			return TEXT("stage_name is required (register the stage with create_usd_stage/open_usd_stage first)");
		}

		if (UE::FUsdStage* Found = GStageNameCache.Find(StageName))
		{
			if (Found->operator bool())
			{
				OutStage = *Found;
				return FString();
			}
			GStageNameCache.Remove(StageName);
			return FString::Printf(TEXT("%s USD stage '%s' is invalid (was it closed?)"), StaleLabel, *StageName);
		}

		// Covers a stage another caller (Python, another C++ module) opened.
		const TArray<UE::FUsdStage> CachedStages = UnrealUSDWrapper::GetAllStagesFromCache();
		for (const UE::FUsdStage& Cached : CachedStages)
		{
			if (!Cached.operator bool())
			{
				continue;
			}
			UE::FSdfLayer Root = Cached.GetRootLayer();
			if (!Root.operator bool())
			{
				continue;
			}
			const FString Identifier = Root.GetIdentifier();
			if (Identifier.EndsWith(StageName) || Identifier.Equals(StageName, ESearchCase::IgnoreCase))
			{
				GStageNameCache.Add(StageName, Cached);
				OutStage = Cached;
				return FString();
			}
		}

		return FString::Printf(TEXT("USD stage '%s' not found in cache (open or create it first)"), *StageName);
	}
}

namespace UEAgent::UsdCommon
{
	/**
	 * Runs Op under the USD error monitor and lists the messages it captures in OutResult["errors"]. They do
	 * not fail the call: the op's own result does.
	 */
	inline void CaptureUsdErrors(const TFunction<void()>& Op, const TSharedPtr<FJsonObject>& OutResult)
	{
		TArray<TSharedPtr<FJsonValue>> ErrorArr;
		PRAGMA_DISABLE_DEPRECATION_WARNINGS
		UsdUtils::StartMonitoringErrors();
		Op();
		const TArray<FString> Errors = UsdUtils::GetErrorsAndStopMonitoring();
		PRAGMA_ENABLE_DEPRECATION_WARNINGS
		for (const FString& E : Errors)
		{
			ErrorArr.Add(MakeShared<FJsonValueString>(E));
		}
		if (ErrorArr.Num() > 0)
		{
			OutResult->SetArrayField(TEXT("errors"), ErrorArr);
		}
	}

	/** Marks Result failed -- "success": false, then "error": Message -- and returns it. */
	inline TSharedPtr<FJsonObject> FailReply(const TSharedPtr<FJsonObject>& Result, const FString& Message)
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), Message);
		return Result;
	}

#if UE_VERSION_OLDER_THAN(5, 8, 0)
	/**
	 * 5.7 has no UE::FSdfPath::IsValidPathString. A string is a valid path exactly when the FSdfPath built from it is
	 * not empty (the contract USD states for SdfPath::IsValidPathString), so that is the test. 5.7 cannot return USD's
	 * parse error (USD raises it as a Tf error, which the engine logs), so OutErrorMessage gets a generic reason.
	 */
	inline bool IsValidSdfPathString(const TCHAR* PathString, FString* OutErrorMessage = nullptr)
	{
		const bool bIsValid = !UE::FSdfPath(PathString).IsEmpty();
		if (OutErrorMessage)
		{
			*OutErrorMessage = bIsValid ? FString() : FString(TEXT("not a well-formed path (Unreal Engine 5.7 does not return USD's parse error)"));
		}
		return bIsValid;
	}
#endif

	/** Whether InPath can name a prim: false, with OutError, when it is empty or not a valid SdfPath string. */
	inline bool IsValidPrimPath(const FString& InPath, FString& OutError)
	{
		if (InPath.IsEmpty())
		{
			OutError = TEXT("prim_path is empty");
			return false;
		}
		FString SdfError;
#if UE_VERSION_OLDER_THAN(5, 8, 0)
		if (!IsValidSdfPathString(*InPath, &SdfError))
#else
		if (!UE::FSdfPath::IsValidPathString(*InPath, &SdfError))
#endif
		{
			OutError = FString::Printf(TEXT("invalid SdfPath '%s': %s"), *InPath, *SdfError);
			return false;
		}
		return true;
	}

	/**
	 * The list position "FrontOfPrependList", "BackOfPrependList", "FrontOfAppendList" or "BackOfAppendList"
	 * (ignoring case) names; BackOfPrependList, the wrapper's own default, for anything else.
	 */
	inline EUsdListPosition ParseListPosition(const FString& InValue)
	{
		if (InValue.Equals(TEXT("FrontOfPrependList"), ESearchCase::IgnoreCase))
		{
			return EUsdListPosition::FrontOfPrependList;
		}
		if (InValue.Equals(TEXT("FrontOfAppendList"), ESearchCase::IgnoreCase))
		{
			return EUsdListPosition::FrontOfAppendList;
		}
		if (InValue.Equals(TEXT("BackOfAppendList"), ESearchCase::IgnoreCase))
		{
			return EUsdListPosition::BackOfAppendList;
		}
		return EUsdListPosition::BackOfPrependList;
	}

	/** The name of a JSON value type, for error messages. */
	inline const TCHAR* JsonTypeName(EJson Type)
	{
		switch (Type)
		{
			case EJson::Boolean: return TEXT("a boolean");
			case EJson::Number:  return TEXT("a number");
			case EJson::String:  return TEXT("a string");
			case EJson::Array:   return TEXT("an array");
			case EJson::Object:  return TEXT("an object");
			default:             return TEXT("null");
		}
	}

	/**
	 * One Sdf value type: its name as an attribute declares it (lowercase, without the "[]" of an array), the basic type
	 * its values hold (a role such as color3f or point3f holds the same GfVec3f as float3) and their shape.
	 */
	struct FUsdValueShape
	{
		const TCHAR* Name;
		UsdUtils::EUsdBasicDataTypes BasicType;
		int32 Components;  // numbers in one value: 1 for a scalar, 2 to 4 for a vector or quaternion, Rows * Rows for a matrix
		int32 Rows;        // rows of a matrix; 0 for any other type
	};

	/** Every Sdf value type but opaque, group and pathExpression, the basic type of each before its roles. */
	inline TConstArrayView<FUsdValueShape> UsdValueShapes()
	{
		using EType = UsdUtils::EUsdBasicDataTypes;
		static const FUsdValueShape Shapes[] = {
			{ TEXT("bool"), EType::Bool, 1, 0 }, { TEXT("uchar"), EType::Uchar, 1, 0 }, { TEXT("int"), EType::Int, 1, 0 },
			{ TEXT("uint"), EType::Uint, 1, 0 }, { TEXT("int64"), EType::Int64, 1, 0 }, { TEXT("uint64"), EType::Uint64, 1, 0 },
			{ TEXT("half"), EType::Half, 1, 0 }, { TEXT("float"), EType::Float, 1, 0 }, { TEXT("double"), EType::Double, 1, 0 },
			{ TEXT("timecode"), EType::Timecode, 1, 0 },
			{ TEXT("string"), EType::String, 1, 0 }, { TEXT("token"), EType::Token, 1, 0 }, { TEXT("asset"), EType::Asset, 1, 0 },
			{ TEXT("int2"), EType::Int2, 2, 0 }, { TEXT("int3"), EType::Int3, 3, 0 }, { TEXT("int4"), EType::Int4, 4, 0 },
			{ TEXT("half2"), EType::Half2, 2, 0 }, { TEXT("half3"), EType::Half3, 3, 0 }, { TEXT("half4"), EType::Half4, 4, 0 },
			{ TEXT("float2"), EType::Float2, 2, 0 }, { TEXT("float3"), EType::Float3, 3, 0 }, { TEXT("float4"), EType::Float4, 4, 0 },
			{ TEXT("double2"), EType::Double2, 2, 0 }, { TEXT("double3"), EType::Double3, 3, 0 }, { TEXT("double4"), EType::Double4, 4, 0 },
			{ TEXT("quath"), EType::Quath, 4, 0 }, { TEXT("quatf"), EType::Quatf, 4, 0 }, { TEXT("quatd"), EType::Quatd, 4, 0 },
			{ TEXT("matrix2d"), EType::Matrix2d, 4, 2 }, { TEXT("matrix3d"), EType::Matrix3d, 9, 3 },
			{ TEXT("matrix4d"), EType::Matrix4d, 16, 4 }, { TEXT("frame4d"), EType::Matrix4d, 16, 4 },
			{ TEXT("point3h"), EType::Half3, 3, 0 }, { TEXT("point3f"), EType::Float3, 3, 0 }, { TEXT("point3d"), EType::Double3, 3, 0 },
			{ TEXT("vector3h"), EType::Half3, 3, 0 }, { TEXT("vector3f"), EType::Float3, 3, 0 }, { TEXT("vector3d"), EType::Double3, 3, 0 },
			{ TEXT("normal3h"), EType::Half3, 3, 0 }, { TEXT("normal3f"), EType::Float3, 3, 0 }, { TEXT("normal3d"), EType::Double3, 3, 0 },
			{ TEXT("color3h"), EType::Half3, 3, 0 }, { TEXT("color3f"), EType::Float3, 3, 0 }, { TEXT("color3d"), EType::Double3, 3, 0 },
			{ TEXT("color4h"), EType::Half4, 4, 0 }, { TEXT("color4f"), EType::Float4, 4, 0 }, { TEXT("color4d"), EType::Double4, 4, 0 },
			{ TEXT("texcoord2h"), EType::Half2, 2, 0 }, { TEXT("texcoord2f"), EType::Float2, 2, 0 }, { TEXT("texcoord2d"), EType::Double2, 2, 0 },
			{ TEXT("texcoord3h"), EType::Half3, 3, 0 }, { TEXT("texcoord3f"), EType::Float3, 3, 0 }, { TEXT("texcoord3d"), EType::Double3, 3, 0 },
		};
		return TConstArrayView<FUsdValueShape>(Shapes);
	}

	/** The shape of the Sdf value type LowerName (lowercase, no "[]"); null when there is none. */
	inline const FUsdValueShape* FindUsdValueShape(const FString& LowerName)
	{
		for (const FUsdValueShape& Shape : UsdValueShapes())
		{
			if (LowerName.Equals(Shape.Name, ESearchCase::CaseSensitive))
			{
				return &Shape;
			}
		}
		return nullptr;
	}

	/** The shape of a value of BasicType, as the engine's UsdToUnreal::ConvertValue reports it; null for None. */
	inline const FUsdValueShape* FindUsdValueShape(UsdUtils::EUsdBasicDataTypes BasicType)
	{
		for (const FUsdValueShape& Shape : UsdValueShapes())
		{
			if (Shape.BasicType == BasicType)
			{
				return &Shape;
			}
		}
		return nullptr;
	}

	/** The C++ type one number of a value is carried in by UsdUtils::FConvertedVtValue (a half travels as a float). */
	enum class EUsdComponentKind : uint8 { Bool, UChar, Int, UInt, Int64, UInt64, Half, Float, Double, String };

	inline EUsdComponentKind UsdComponentKind(UsdUtils::EUsdBasicDataTypes BasicType)
	{
		using EType = UsdUtils::EUsdBasicDataTypes;
		switch (BasicType)
		{
			case EType::Bool: return EUsdComponentKind::Bool;
			case EType::Uchar: return EUsdComponentKind::UChar;
			case EType::Int: case EType::Int2: case EType::Int3: case EType::Int4: return EUsdComponentKind::Int;
			case EType::Uint: return EUsdComponentKind::UInt;
			case EType::Int64: return EUsdComponentKind::Int64;
			case EType::Uint64: return EUsdComponentKind::UInt64;
			case EType::Half: case EType::Half2: case EType::Half3: case EType::Half4: case EType::Quath: return EUsdComponentKind::Half;
			case EType::Float: case EType::Float2: case EType::Float3: case EType::Float4: case EType::Quatf: return EUsdComponentKind::Float;
			case EType::String: case EType::Token: case EType::Asset: return EUsdComponentKind::String;
			default: return EUsdComponentKind::Double;  // double, timecode, the double vectors, quatd and the matrices
		}
	}

	/** The JSON form of RawVal for an error message: its JSON type, and the length of an array. */
	inline FString JsonShapeName(const TSharedPtr<FJsonValue>& RawVal)
	{
		if (!RawVal.IsValid())
		{
			return TEXT("nothing");
		}
		if (RawVal->Type == EJson::Array)
		{
			return FString::Printf(TEXT("an array of %d"), RawVal->AsArray().Num());
		}
		return JsonTypeName(RawVal->Type);
	}

	/**
	 * Adds to OutEntry the component of kind Kind that the JSON value RawVal gives: a boolean for Bool; a whole number in
	 * the range of the type for the integer kinds; a number for Half, Float and Double (half and float within their
	 * range); a string for String. False, with OutError naming Subject (the part of the value it is), for anything else.
	 * Subject is only built for an error, so a long array costs no string formatting per component.
	 */
	inline bool AddUsdComponentFromJson(const TSharedPtr<FJsonValue>& RawVal, EUsdComponentKind Kind, TFunctionRef<FString()> Subject,
		UsdUtils::FConvertedVtValueEntry& OutEntry, FString& OutError)
	{
		const EJson JsonType = RawVal.IsValid() ? RawVal->Type : EJson::None;
		if (Kind == EUsdComponentKind::Bool)
		{
			if (JsonType != EJson::Boolean)
			{
				OutError = FString::Printf(TEXT("%s takes a boolean, not %s"), *Subject(), *JsonShapeName(RawVal));
				return false;
			}
			OutEntry.Emplace(TInPlaceType<bool>(), RawVal->AsBool());
			return true;
		}
		if (Kind == EUsdComponentKind::String)
		{
			if (JsonType != EJson::String)
			{
				OutError = FString::Printf(TEXT("%s takes a string, not %s"), *Subject(), *JsonShapeName(RawVal));
				return false;
			}
			OutEntry.Emplace(TInPlaceType<FString>(), RawVal->AsString());
			return true;
		}
		if (JsonType != EJson::Number)
		{
			OutError = FString::Printf(TEXT("%s takes a number, not %s"), *Subject(), *JsonShapeName(RawVal));
			return false;
		}

		const double Number = RawVal->AsNumber();
		// A whole number in [Min, MaxExclusive): the range of the integer type it is stored as.
		const auto Whole = [Number, &Subject, &OutError](double Min, double MaxExclusive, const TCHAR* TypeName)
		{
			if (Number != FMath::FloorToDouble(Number) || Number < Min || Number >= MaxExclusive)
			{
				OutError = FString::Printf(TEXT("%.17g does not fit %s: it takes a whole number in the range of %s"), Number, *Subject(), TypeName);
				return false;
			}
			return true;
		};
		const auto Within = [Number, &Subject, &OutError](double Limit, const TCHAR* TypeName)
		{
			if (FMath::Abs(Number) > Limit)
			{
				OutError = FString::Printf(TEXT("%.17g does not fit %s: it is out of the range of %s (largest magnitude %.9g)"),
					Number, *Subject(), TypeName, Limit);
				return false;
			}
			return true;
		};
		switch (Kind)
		{
			case EUsdComponentKind::UChar:
				if (!Whole(0.0, 256.0, TEXT("uchar"))) { return false; }
				OutEntry.Emplace(TInPlaceType<uint8>(), static_cast<uint8>(Number));
				return true;
			case EUsdComponentKind::Int:
				if (!Whole(-2147483648.0, 2147483648.0, TEXT("int"))) { return false; }
				OutEntry.Emplace(TInPlaceType<int32>(), static_cast<int32>(Number));
				return true;
			case EUsdComponentKind::UInt:
				if (!Whole(0.0, 4294967296.0, TEXT("uint"))) { return false; }
				OutEntry.Emplace(TInPlaceType<uint32>(), static_cast<uint32>(Number));
				return true;
			case EUsdComponentKind::Int64:
				if (!Whole(-9223372036854775808.0, 9223372036854775808.0, TEXT("int64"))) { return false; }
				OutEntry.Emplace(TInPlaceType<int64>(), static_cast<int64>(Number));
				return true;
			case EUsdComponentKind::UInt64:
				if (!Whole(0.0, 18446744073709551616.0, TEXT("uint64"))) { return false; }
				OutEntry.Emplace(TInPlaceType<uint64>(), static_cast<uint64>(Number));
				return true;
			case EUsdComponentKind::Half:
				if (!Within(65504.0, TEXT("half"))) { return false; }
				OutEntry.Emplace(TInPlaceType<float>(), static_cast<float>(Number));
				return true;
			case EUsdComponentKind::Float:
				if (!Within(static_cast<double>(TNumericLimits<float>::Max()), TEXT("float"))) { return false; }
				OutEntry.Emplace(TInPlaceType<float>(), static_cast<float>(Number));
				return true;
			default:
				OutEntry.Emplace(TInPlaceType<double>(), Number);
				return true;
		}
	}

	/**
	 * The converted USD value entry (its components, in the order UnrealToUsd::ConvertValue reads them) that the JSON
	 * value RawVal gives for one value of Shape: the scalar itself; for a vector or quaternion an array of exactly
	 * Components numbers (a quaternion as x, y, z, then the real part w, the engine's order); for a matrix an array of
	 * Rows rows of Rows numbers each. False, with OutError naming Subject, for a value of another shape or that does not fit.
	 */
	inline bool UsdEntryFromJson(const TSharedPtr<FJsonValue>& RawVal, const FUsdValueShape& Shape, TFunctionRef<FString()> Subject,
		UsdUtils::FConvertedVtValueEntry& OutEntry, FString& OutError)
	{
		const EUsdComponentKind Kind = UsdComponentKind(Shape.BasicType);
		OutEntry.Reset();
		if (Shape.Components == 1)
		{
			return AddUsdComponentFromJson(RawVal, Kind, Subject, OutEntry, OutError);
		}

		const int32 Count = Shape.Rows > 0 ? Shape.Rows : Shape.Components;
		if (!RawVal.IsValid() || RawVal->Type != EJson::Array || RawVal->AsArray().Num() != Count)
		{
			OutError = Shape.Rows > 0
				? FString::Printf(TEXT("%s takes an array of %d rows of %d numbers each, not %s"), *Subject(), Count, Count, *JsonShapeName(RawVal))
				: FString::Printf(TEXT("%s takes an array of %d numbers, not %s"), *Subject(), Count, *JsonShapeName(RawVal));
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>& Items = RawVal->AsArray();
		OutEntry.Reserve(Shape.Components);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			if (Shape.Rows == 0)
			{
				const auto ComponentSubject = [&Subject, Index]() { return FString::Printf(TEXT("component %d of %s"), Index, *Subject()); };
				if (!AddUsdComponentFromJson(Items[Index], Kind, ComponentSubject, OutEntry, OutError))
				{
					return false;
				}
				continue;
			}
			// A matrix: its rows in order, which is the row-major order of the GfMatrix data the engine fills.
			const auto RowSubject = [&Subject, Index]() { return FString::Printf(TEXT("row %d of %s"), Index, *Subject()); };
			const TSharedPtr<FJsonValue>& Row = Items[Index];
			if (!Row.IsValid() || Row->Type != EJson::Array || Row->AsArray().Num() != Count)
			{
				OutError = FString::Printf(TEXT("%s takes an array of %d numbers, not %s"), *RowSubject(), Count, *JsonShapeName(Row));
				return false;
			}
			for (int32 Column = 0; Column < Count; ++Column)
			{
				const auto CellSubject = [&RowSubject, Column]() { return FString::Printf(TEXT("component %d of %s"), Column, *RowSubject()); };
				if (!AddUsdComponentFromJson(Row->AsArray()[Column], Kind, CellSubject, OutEntry, OutError))
				{
					return false;
				}
			}
		}
		return true;
	}

	/** The JSON form of one component of a converted USD value: a boolean, a number or a string. */
	inline TSharedPtr<FJsonValue> UsdComponentToJson(const UsdUtils::FConvertedVtValueComponent& Component)
	{
		if (const bool* AsBool = Component.TryGet<bool>()) { return MakeShared<FJsonValueBoolean>(*AsBool); }
		if (const FString* AsText = Component.TryGet<FString>()) { return MakeShared<FJsonValueString>(*AsText); }
		if (const uint8* AsU8 = Component.TryGet<uint8>()) { return MakeShared<FJsonValueNumber>(static_cast<double>(*AsU8)); }
		if (const int32* AsI32 = Component.TryGet<int32>()) { return MakeShared<FJsonValueNumber>(static_cast<double>(*AsI32)); }
		if (const uint32* AsU32 = Component.TryGet<uint32>()) { return MakeShared<FJsonValueNumber>(static_cast<double>(*AsU32)); }
		if (const int64* AsI64 = Component.TryGet<int64>()) { return MakeShared<FJsonValueNumber>(static_cast<double>(*AsI64)); }
		if (const uint64* AsU64 = Component.TryGet<uint64>()) { return MakeShared<FJsonValueNumber>(static_cast<double>(*AsU64)); }
		if (const float* AsF32 = Component.TryGet<float>()) { return MakeShared<FJsonValueNumber>(static_cast<double>(*AsF32)); }
		if (const double* AsF64 = Component.TryGet<double>()) { return MakeShared<FJsonValueNumber>(*AsF64); }
		return MakeShared<FJsonValueNull>();
	}

	/**
	 * The JSON form of one value (Entry) of BasicType as UsdToUnreal::ConvertValue converted it: a scalar as a boolean,
	 * number or string; a vector or quaternion as an array of numbers (a quaternion x, y, z, then the real part w); a
	 * matrix as an array of rows.
	 */
	inline TSharedPtr<FJsonValue> UsdEntryToJson(const UsdUtils::FConvertedVtValueEntry& Entry, UsdUtils::EUsdBasicDataTypes BasicType)
	{
		const FUsdValueShape* Shape = FindUsdValueShape(BasicType);
		if (Shape == nullptr || Shape->Components == 1)
		{
			if (Entry.Num() == 0)
			{
				return MakeShared<FJsonValueNull>();
			}
			return UsdComponentToJson(Entry[0]);
		}
		TArray<TSharedPtr<FJsonValue>> Numbers;
		Numbers.Reserve(Entry.Num());
		for (const UsdUtils::FConvertedVtValueComponent& Component : Entry)
		{
			Numbers.Add(UsdComponentToJson(Component));
		}
		if (Shape->Rows == 0)
		{
			return MakeShared<FJsonValueArray>(Numbers);
		}
		// The engine lists a matrix row-major (GfMatrix::GetArray).
		TArray<TSharedPtr<FJsonValue>> Rows;
		Rows.Reserve(Shape->Rows);
		for (int32 Row = 0; Row < Shape->Rows; ++Row)
		{
			TArray<TSharedPtr<FJsonValue>> RowNumbers;
			for (int32 Column = 0; Column < Shape->Rows && Row * Shape->Rows + Column < Numbers.Num(); ++Column)
			{
				RowNumbers.Add(Numbers[Row * Shape->Rows + Column]);
			}
			Rows.Add(MakeShared<FJsonValueArray>(RowNumbers));
		}
		return MakeShared<FJsonValueArray>(Rows);
	}

	/**
	 * The most numbers (or strings) an array value is read with: an array is cut to its first
	 * MaxArrayValuesRead / Components elements (600 scalars, 200 float3, 37 matrix4d). The bridge offloads a reply
	 * whose pretty-printed JSON passes 40000 characters (McpOversize, mcp.OversizeResponseChars) and keeps only its
	 * top-level scalars, which would drop length and truncated with the rest. There a number takes up to about 47
	 * characters (24 for "%.17g", its own line with tabs and a comma, and in a matrix2d its share of the row
	 * brackets), so the worst capped reply, with long prim and attribute names, stays near 29000 characters.
	 */
	constexpr int32 MaxArrayValuesRead = 600;

	/**
	 * The characters the strings of one array value may take (UsdJsonStringCost): a string, token or asset array stops
	 * before the element that would pass it, even its first, since a string has no length limit of its own.
	 */
	constexpr int32 MaxArrayStringCharsRead = 24000;

	/** The characters Text takes as an array element of the bridge's JSON: escaped, quoted, on its own line with a comma. */
	inline int32 UsdJsonStringCost(const FString& Text)
	{
		int32 Cost = Text.Len() + 2 + 8;  // the quotes; CRLF, up to five tabs and the comma
		for (const TCHAR Char : Text)
		{
			if (Char == TEXT('\\') || Char == TEXT('"') || Char == TEXT('\n') || Char == TEXT('\t') || Char == TEXT('\b')
				|| Char == TEXT('\f') || Char == TEXT('\r'))
			{
				Cost += 1;  // a two-character escape
			}
			else if (Char < TEXT(' '))
			{
				Cost += 5;  // \u00XX
			}
		}
		return Cost;
	}

	/** The elements of an array of Shape that are read: MaxArrayValuesRead numbers' worth, at least one. */
	inline int32 MaxArrayElementsRead(const FUsdValueShape& Shape)
	{
		return FMath::Max(1, MaxArrayValuesRead / FMath::Max(1, Shape.Components));
	}

	/**
	 * The element count of the array Value, read as TArray<T> through the wrapper (a compact copy, no per-element
	 * conversion); when it has more than MaxElements, its first MaxElements go into OutHead as the same USD type.
	 */
	template <typename T>
	int32 UsdArrayHeadAs(const UE::FVtValue& Value, int32 MaxElements, UE::FVtValue& OutHead)
	{
		const TArray<T> All = Value.Get<TArray<T>>();
		if (All.Num() > MaxElements)
		{
			OutHead.Set(TArray<T>(All.GetData(), MaxElements));
		}
		return All.Num();
	}

	/**
	 * UsdArrayHeadAs for the element type the array Value of BasicType holds, so a huge array is cut before the
	 * engine converts it element by element. INDEX_NONE for a token array, whose only typed read goes through FName
	 * (case-folding, and a check() past NAME_SIZE characters): the caller converts that one whole and cuts the result.
	 */
	inline int32 UsdArrayHead(const UE::FVtValue& Value, UsdUtils::EUsdBasicDataTypes BasicType, int32 MaxElements, UE::FVtValue& OutHead)
	{
		using EType = UsdUtils::EUsdBasicDataTypes;
		switch (BasicType)
		{
			case EType::Bool: return UsdArrayHeadAs<bool>(Value, MaxElements, OutHead);
			case EType::Uchar: return UsdArrayHeadAs<uint8>(Value, MaxElements, OutHead);
			case EType::Int: return UsdArrayHeadAs<int32>(Value, MaxElements, OutHead);
			case EType::Uint: return UsdArrayHeadAs<uint32>(Value, MaxElements, OutHead);
			case EType::Int64: return UsdArrayHeadAs<int64>(Value, MaxElements, OutHead);
			case EType::Uint64: return UsdArrayHeadAs<uint64>(Value, MaxElements, OutHead);
			case EType::Half: return UsdArrayHeadAs<FFloat16>(Value, MaxElements, OutHead);
			case EType::Float: return UsdArrayHeadAs<float>(Value, MaxElements, OutHead);
			case EType::Double: return UsdArrayHeadAs<double>(Value, MaxElements, OutHead);
			case EType::Timecode: return UsdArrayHeadAs<FSdfTimeCode>(Value, MaxElements, OutHead);
			case EType::String: return UsdArrayHeadAs<FString>(Value, MaxElements, OutHead);
			case EType::Asset: return UsdArrayHeadAs<FSdfAssetPath>(Value, MaxElements, OutHead);
			case EType::Matrix2d: return UsdArrayHeadAs<FMatrix2D>(Value, MaxElements, OutHead);
			case EType::Matrix3d: return UsdArrayHeadAs<FMatrix3D>(Value, MaxElements, OutHead);
			case EType::Matrix4d: return UsdArrayHeadAs<FMatrix44d>(Value, MaxElements, OutHead);
			case EType::Quatd: return UsdArrayHeadAs<FQuat4d>(Value, MaxElements, OutHead);
			case EType::Quatf: return UsdArrayHeadAs<FQuat4f>(Value, MaxElements, OutHead);
			case EType::Quath: return UsdArrayHeadAs<FQuat4h>(Value, MaxElements, OutHead);
			case EType::Double2: return UsdArrayHeadAs<FVector2d>(Value, MaxElements, OutHead);
			case EType::Float2: return UsdArrayHeadAs<FVector2f>(Value, MaxElements, OutHead);
			case EType::Half2: return UsdArrayHeadAs<FVector2DHalf>(Value, MaxElements, OutHead);
			case EType::Int2: return UsdArrayHeadAs<FIntPoint>(Value, MaxElements, OutHead);
			case EType::Double3: return UsdArrayHeadAs<FVector3d>(Value, MaxElements, OutHead);
			case EType::Float3: return UsdArrayHeadAs<FVector3f>(Value, MaxElements, OutHead);
			case EType::Half3: return UsdArrayHeadAs<FVector3h>(Value, MaxElements, OutHead);
			case EType::Int3: return UsdArrayHeadAs<FIntVector>(Value, MaxElements, OutHead);
			case EType::Double4: return UsdArrayHeadAs<FVector4d>(Value, MaxElements, OutHead);
			case EType::Float4: return UsdArrayHeadAs<FVector4f>(Value, MaxElements, OutHead);
			case EType::Half4: return UsdArrayHeadAs<FVector4h>(Value, MaxElements, OutHead);
			case EType::Int4: return UsdArrayHeadAs<FIntRect>(Value, MaxElements, OutHead);
			default: return INDEX_NONE;
		}
	}

	/**
	 * The FVtValue holding exactly the C++ type of the USD value type DeclaredType (an attribute's SdfValueTypeName,
	 * compared ignoring case) that the JSON value RawVal gives: UsdAttribute::Set authors nothing when the held type is
	 * not the attribute's own, so a double for a float attribute or an int for a uint one fails. bool takes a JSON
	 * boolean; uchar, int, uint, int64 and uint64 a whole number in their range; half, float, double and timecode a
	 * number (half and float within their range); string, token and asset a string. A vector, color, point, normal,
	 * texCoord or quaternion type takes an array of exactly its count of such numbers (a quaternion x, y, z, then the real
	 * part w), a matrix an array of rows, and an array type ("float3[]") a JSON array of values of its element type;
	 * those are built by the engine's UnrealToUsd::ConvertValue. pathExpression takes the text of a path expression and
	 * pathExpression[] a JSON array of such texts, which USD parses (UEAgent::UsdSdk; an empty text is the empty
	 * expression). False, with OutError, for opaque or group (USD gives them no value to author), a text that does not
	 * parse as a path expression (with USD's parse error), an unknown type, or a value of another shape or that does not
	 * fit. Holder names what holds the value in the messages ("an attribute").
	 */
	inline bool VtValueForDeclaredType(const FString& DeclaredType, const TSharedPtr<FJsonValue>& RawVal, UE::FVtValue& Out,
		FString& OutError, const TCHAR* Holder = TEXT("an attribute"))
	{
		OutError.Reset();
		const FString Type = DeclaredType.ToLower();
		if (Type == TEXT("opaque") || Type == TEXT("group"))
		{
			OutError = FString::Printf(TEXT("%s of type %s is not one this tool can author: opaque and group types hold no value by design, ")
				TEXT("USD gives them none to author"), Holder, *DeclaredType);
			return false;
		}
		if (!RawVal.IsValid() || RawVal->IsNull())
		{
			OutError = TEXT("the value is missing or null");
			return false;
		}

		const auto Mismatch = [&RawVal, &DeclaredType, &OutError, Holder](const TCHAR* Wanted)
		{
			OutError = FString::Printf(TEXT("%s of type %s takes %s, not %s"), Holder, *DeclaredType, Wanted, JsonTypeName(RawVal->Type));
			return false;
		};

		// The engine's wrapper has no way to build a path expression; USD parses the text, and its parse error refuses one
		// that does not parse.
		if (Type == TEXT("pathexpression"))
		{
			if (RawVal->Type != EJson::String) { return Mismatch(TEXT("a string, the text of a path expression")); }
#if UE_VERSION_OLDER_THAN(5, 8, 0)
			OutError = TEXT("authoring a path expression needs Unreal Engine 5.8 or later: on Unreal Engine 5.7 (USD 25.08) writing a value built outside the engine's USD wrapper closes the editor, so it is refused; reading one works");
			return false;
#else
			FString ParseError;
			if (!UEAgent::UsdSdk::MakePathExpression(RawVal->AsString(), Out, ParseError))
			{
				OutError = FString::Printf(TEXT("'%s' does not parse as a path expression: %s"), *RawVal->AsString(), *ParseError);
				return false;
			}
			return true;
#endif
		}
		if (Type == TEXT("pathexpression[]"))
		{
			if (RawVal->Type != EJson::Array) { return Mismatch(TEXT("a JSON array of path expression texts")); }
			const TArray<TSharedPtr<FJsonValue>>& Items = RawVal->AsArray();
			TArray<FString> Texts;
			Texts.Reserve(Items.Num());
			for (int32 Index = 0; Index < Items.Num(); ++Index)
			{
				if (!Items[Index].IsValid() || Items[Index]->Type != EJson::String)
				{
					OutError = FString::Printf(TEXT("element %d of %s of type %s takes a string, not %s"), Index, Holder, *DeclaredType,
						*JsonShapeName(Items[Index]));
					return false;
				}
				Texts.Add(Items[Index]->AsString());
			}
#if UE_VERSION_OLDER_THAN(5, 8, 0)
			OutError = TEXT("authoring path expressions needs Unreal Engine 5.8 or later: on Unreal Engine 5.7 (USD 25.08) writing a value built outside the engine's USD wrapper closes the editor, so it is refused; reading one works");
			return false;
#else
			return UEAgent::UsdSdk::MakePathExpressionArray(Texts, Out, OutError);
#endif
		}
		// A whole JSON number in [Min, MaxExclusive): the range of the integer type it is stored as.
		const auto ReadWhole = [&RawVal, &DeclaredType, &OutError, Holder](double Min, double MaxExclusive, double& OutNumber)
		{
			OutNumber = RawVal->AsNumber();
			if (OutNumber != FMath::FloorToDouble(OutNumber) || OutNumber < Min || OutNumber >= MaxExclusive)
			{
				OutError = FString::Printf(TEXT("%.17g does not fit %s of type %s: it takes a whole number in the range of that type"),
					OutNumber, Holder, *DeclaredType);
				return false;
			}
			return true;
		};

		if (Type == TEXT("bool"))
		{
			if (RawVal->Type != EJson::Boolean) { return Mismatch(TEXT("a boolean")); }
			Out.Set(RawVal->AsBool());
			return true;
		}

		const bool bInteger = Type == TEXT("uchar") || Type == TEXT("int") || Type == TEXT("uint") || Type == TEXT("int64")
			|| Type == TEXT("uint64");
		const bool bReal = Type == TEXT("half") || Type == TEXT("float") || Type == TEXT("double") || Type == TEXT("timecode");
		if (bInteger || bReal)
		{
			if (RawVal->Type != EJson::Number) { return Mismatch(TEXT("a number")); }
			double Number = 0.0;
			if (Type == TEXT("uchar"))
			{
				if (!ReadWhole(0.0, 256.0, Number)) { return false; }
				Out.Set(static_cast<uint8>(Number));
			}
			else if (Type == TEXT("int"))
			{
				if (!ReadWhole(-2147483648.0, 2147483648.0, Number)) { return false; }
				Out.Set(static_cast<int32>(Number));
			}
			else if (Type == TEXT("uint"))
			{
				if (!ReadWhole(0.0, 4294967296.0, Number)) { return false; }
				Out.Set(static_cast<uint32>(Number));
			}
			else if (Type == TEXT("int64"))
			{
				if (!ReadWhole(-9223372036854775808.0, 9223372036854775808.0, Number)) { return false; }
				Out.Set(static_cast<int64>(Number));
			}
			else if (Type == TEXT("uint64"))
			{
				if (!ReadWhole(0.0, 18446744073709551616.0, Number)) { return false; }
				Out.Set(static_cast<uint64>(Number));
			}
			else
			{
				Number = RawVal->AsNumber();
				const double Limit = Type == TEXT("half") ? 65504.0
					: Type == TEXT("float") ? static_cast<double>(TNumericLimits<float>::Max()) : 0.0;
				if (Limit > 0.0 && FMath::Abs(Number) > Limit)
				{
					OutError = FString::Printf(TEXT("%.17g is out of the range of type %s (largest magnitude %.9g)"),
						Number, *DeclaredType, Limit);
					return false;
				}
				if (Type == TEXT("half"))
				{
					Out.Set(FFloat16(static_cast<float>(Number)));
				}
				else if (Type == TEXT("float"))
				{
					Out.Set(static_cast<float>(Number));
				}
				else if (Type == TEXT("double"))
				{
					Out.Set(Number);
				}
				else
				{
					Out.Set(FSdfTimeCode(Number));
				}
			}
			return true;
		}

		if (Type == TEXT("string") || Type == TEXT("token") || Type == TEXT("asset"))
		{
			if (RawVal->Type != EJson::String) { return Mismatch(TEXT("a string")); }
			const FString Text = RawVal->AsString();
			if (Type == TEXT("string"))
			{
				Out.Set(Text);
			}
			else if (Type == TEXT("token"))
			{
				// The token is built through an FName, and an FName of NAME_SIZE characters or more fails a check().
				if (Text.Len() >= NAME_SIZE)
				{
					OutError = FString::Printf(TEXT("a token is limited to %d characters, got %d"), NAME_SIZE - 1, Text.Len());
					return false;
				}
				Out.Set(FName(*Text));
			}
			else
			{
				Out.Set(FSdfAssetPath(Text));
			}
			return true;
		}

		// Vectors, roles, quaternions, matrices and arrays: the JSON is checked against the shape of the type, then the
		// engine's own conversion builds the GfVec / GfQuat / GfMatrix / VtArray value from the converted components.
		const bool bArray = Type.EndsWith(TEXT("[]"));
		const FUsdValueShape* Shape = FindUsdValueShape(bArray ? Type.LeftChop(2) : Type);
		if (Shape == nullptr)
		{
			OutError = FString::Printf(
				TEXT("%s of type %s is not one this tool can author: the declared type must be a scalar (bool, uchar, int, uint, ")
				TEXT("int64, uint64, half, float, double, timecode, string, token, asset), a pathExpression, a vector, point, normal, color ")
				TEXT("or texCoord, a quaternion, a matrix, or an array of one of them"), Holder, *DeclaredType);
			return false;
		}

		UsdUtils::FConvertedVtValue Converted;
		Converted.SourceType = Shape->BasicType;
		Converted.bIsArrayValued = bArray;
		Converted.bIsEmpty = false;
		const auto Subject = [&DeclaredType, Holder]() { return FString::Printf(TEXT("%s of type %s"), Holder, *DeclaredType); };
		if (bArray)
		{
			if (RawVal->Type != EJson::Array) { return Mismatch(TEXT("a JSON array of values")); }
			const TArray<TSharedPtr<FJsonValue>>& Items = RawVal->AsArray();
			Converted.Entries.Reserve(Items.Num());
			for (int32 Index = 0; Index < Items.Num(); ++Index)
			{
				UsdUtils::FConvertedVtValueEntry& Entry = Converted.Entries.AddDefaulted_GetRef();
				const auto ElementSubject = [&Subject, Index]() { return FString::Printf(TEXT("element %d of %s"), Index, *Subject()); };
				if (!UsdEntryFromJson(Items[Index], *Shape, ElementSubject, Entry, OutError))
				{
					return false;
				}
			}
		}
		else if (!UsdEntryFromJson(RawVal, *Shape, Subject, Converted.Entries.AddDefaulted_GetRef(), OutError))
		{
			return false;
		}

		// ConvertValue leaves the value empty when it converts nothing, and an empty array still converts to an empty VtArray.
		Out = UE::FVtValue();
		if (!UnrealToUsd::ConvertValue(Converted, Out) || Out.IsEmpty())
		{
			OutError = FString::Printf(TEXT("the value could not be converted to the USD type %s"), *DeclaredType);
			return false;
		}
		return true;
	}

	// ---------------------------------------------------------------------------------------------------------------
	// Dictionaries and list ops: the names of their types and the JSON forms they are written from.
	// ---------------------------------------------------------------------------------------------------------------

	/** The items a list op holds, which decides how they are checked and converted. */
	enum class EUsdListOpItems : uint8 { Token, String, Path, Arc, Int, UInt, Int64, UInt64 };

	/**
	 * One list op type: the Sdf name of its metadata type, the name USD gives the held C++ type (the engine keys its
	 * conversion on it, FConvertedVtValue::ExtraTypeInfo) and its items. The list ops of tokens, strings, paths, references
	 * and payloads go through the engine's value conversion; those of integers, which it has no conversion for, through
	 * UEAgent::UsdSdk.
	 */
	struct FUsdListOpKind
	{
		const TCHAR* SdfName;
		const TCHAR* TypeName;
		EUsdListOpItems Items;
	};

	inline TConstArrayView<FUsdListOpKind> UsdListOpKinds()
	{
		// The held type names are spelled as USD's MSVC build demangles them (the plugin is built for Win64 only).
		static const FUsdListOpKind Kinds[] = {
			{ TEXT("tokenlistop"), TEXT("SdfListOp<TfToken>"), EUsdListOpItems::Token },
			{ TEXT("stringlistop"), TEXT("SdfListOp<string>"), EUsdListOpItems::String },  // the name a clipSets read reports
			{ TEXT("pathlistop"), TEXT("SdfListOp<SdfPath>"), EUsdListOpItems::Path },
			{ TEXT("referencelistop"), TEXT("SdfListOp<SdfReference>"), EUsdListOpItems::Arc },
			{ TEXT("payloadlistop"), TEXT("SdfListOp<SdfPayload>"), EUsdListOpItems::Arc },
			{ TEXT("intlistop"), TEXT("SdfListOp<int>"), EUsdListOpItems::Int },
			{ TEXT("uintlistop"), TEXT("SdfListOp<unsigned int>"), EUsdListOpItems::UInt },
			{ TEXT("int64listop"), TEXT("SdfListOp<__int64>"), EUsdListOpItems::Int64 },
			{ TEXT("uint64listop"), TEXT("SdfListOp<unsigned __int64>"), EUsdListOpItems::UInt64 },
		};
		return TConstArrayView<FUsdListOpKind>(Kinds);
	}

	/** The UEAgent::UsdSdk item type of a list op of Items: None for a list op that does not hold integers. */
	inline UEAgent::UsdSdk::EIntListOpItems UsdIntListOpItems(EUsdListOpItems Items)
	{
		using ESdkItems = UEAgent::UsdSdk::EIntListOpItems;
		switch (Items)
		{
			case EUsdListOpItems::Int: return ESdkItems::Int;
			case EUsdListOpItems::UInt: return ESdkItems::UInt;
			case EUsdListOpItems::Int64: return ESdkItems::Int64;
			case EUsdListOpItems::UInt64: return ESdkItems::UInt64;
			default: return ESdkItems::None;
		}
	}

	/** The kind of the integer list op whose items are SdkItems; null for None. */
	inline const FUsdListOpKind* UsdIntListOpKind(UEAgent::UsdSdk::EIntListOpItems SdkItems)
	{
		for (const FUsdListOpKind& Kind : UsdListOpKinds())
		{
			if (SdkItems != UEAgent::UsdSdk::EIntListOpItems::None && UsdIntListOpItems(Kind.Items) == SdkItems)
			{
				return &Kind;
			}
		}
		return nullptr;
	}

	/** The items of the list op held as TypeName: those of its kind, else (a name only a read gave) strings. */
	inline EUsdListOpItems UsdListOpItemsOf(const FString& TypeName)
	{
		for (const FUsdListOpKind& Kind : UsdListOpKinds())
		{
			if (TypeName.Equals(Kind.TypeName, ESearchCase::CaseSensitive))
			{
				return Kind.Items;
			}
		}
		return EUsdListOpItems::String;
	}

	/** The lists of a list op as indices into the engine's converted entries, in SdfListOpType's order. */
	constexpr int32 UsdListOpExplicit = 0;
	constexpr int32 UsdListOpAdded = 1;
	constexpr int32 UsdListOpDeleted = 2;
	constexpr int32 UsdListOpOrdered = 3;
	constexpr int32 UsdListOpPrepended = 4;
	constexpr int32 UsdListOpAppended = 5;
	constexpr int32 UsdListOpListCount = 6;
	static_assert(UsdListOpExplicit == static_cast<int32>(UEAgent::UsdSdk::EListOpList::Explicit)
		&& UsdListOpAdded == static_cast<int32>(UEAgent::UsdSdk::EListOpList::Added)
		&& UsdListOpDeleted == static_cast<int32>(UEAgent::UsdSdk::EListOpList::Deleted)
		&& UsdListOpOrdered == static_cast<int32>(UEAgent::UsdSdk::EListOpList::Ordered)
		&& UsdListOpPrepended == static_cast<int32>(UEAgent::UsdSdk::EListOpList::Prepended)
		&& UsdListOpAppended == static_cast<int32>(UEAgent::UsdSdk::EListOpList::Appended)
		&& UsdListOpListCount == UEAgent::UsdSdk::ListOpListCount, "the list indices are UEAgent::UsdSdk's");

	/** The JSON name of the list List, one of the UsdListOp* indices. */
	inline const TCHAR* UsdListOpListName(int32 List)
	{
		static const TCHAR* const Names[UsdListOpListCount] = {
			TEXT("explicit"), TEXT("added"), TEXT("deleted"), TEXT("ordered"), TEXT("prepended"), TEXT("appended") };
		return Names[List];
	}

	/** The most dictionaries one value nests, read or written: a deeper one reads empty and is refused on write. */
	constexpr int32 MaxUsdDictionaryDepth = 8;

	/**
	 * The spelling the converters take for the USD type Name names (compared ignoring case): an Sdf value type name as
	 * it is ("float3", "token[]"); an alias (boolean for bool, integer and int32 for int, number for double, name for
	 * token); the name of a held C++ type as get_usd_attribute_value and get_usd_prim_metadata report it ("TfToken",
	 * "pxr_half::half", "unsigned __int64", "SdfAssetPath", "GfVec3f", "GfQuatd", "GfMatrix4d", "VtArray<float>") as its
	 * Sdf value type; "dictionary" for VtDictionary; a list op as the held type name of its kind ("tokenlistop",
	 * "SdfListOp<TfToken>" and "SdfListOp<token>" give "SdfListOp<TfToken>", "int64listop" and "SdfListOp<int64>" give
	 * "SdfListOp<__int64>"), and any other SdfListOp name as it is. Any other name comes back lowercased, for the
	 * converter to refuse.
	 */
	inline FString CanonicalUsdTypeName(const FString& Name)
	{
		const FString Trimmed = Name.TrimStartAndEnd();
		const FString Lower = Trimmed.ToLower();
		for (const FUsdListOpKind& Kind : UsdListOpKinds())
		{
			if (Lower.Equals(Kind.SdfName, ESearchCase::CaseSensitive) || Trimmed.Equals(Kind.TypeName, ESearchCase::IgnoreCase))
			{
				return Kind.TypeName;
			}
		}
		if (Lower.StartsWith(TEXT("sdflistop<"), ESearchCase::CaseSensitive))
		{
			// The item type in any spelling this function takes names the same kind: SdfListOp<int64> is an int64listop.
			if (Lower.EndsWith(TEXT(">"), ESearchCase::CaseSensitive))
			{
				const FString ItemSdfName = CanonicalUsdTypeName(Trimmed.Mid(10, Trimmed.Len() - 11)) + TEXT("listop");
				for (const FUsdListOpKind& Kind : UsdListOpKinds())
				{
					if (ItemSdfName.Equals(Kind.SdfName, ESearchCase::CaseSensitive))
					{
						return Kind.TypeName;
					}
				}
			}
			return Trimmed;
		}
		if (Lower.Equals(TEXT("dictionary"), ESearchCase::CaseSensitive) || Lower.Equals(TEXT("vtdictionary"), ESearchCase::CaseSensitive))
		{
			return TEXT("dictionary");
		}
		if (Lower.StartsWith(TEXT("vtarray<"), ESearchCase::CaseSensitive) && Lower.EndsWith(TEXT(">"), ESearchCase::CaseSensitive))
		{
			return CanonicalUsdTypeName(Trimmed.Mid(8, Trimmed.Len() - 9)) + TEXT("[]");
		}
		static const TCHAR* const Aliases[][2] = {
			{ TEXT("boolean"), TEXT("bool") }, { TEXT("integer"), TEXT("int") }, { TEXT("int32"), TEXT("int") },
			{ TEXT("number"), TEXT("double") }, { TEXT("name"), TEXT("token") }, { TEXT("unsigned char"), TEXT("uchar") },
			{ TEXT("unsigned int"), TEXT("uint") }, { TEXT("__int64"), TEXT("int64") }, { TEXT("unsigned __int64"), TEXT("uint64") },
			{ TEXT("pxr_half::half"), TEXT("half") }, { TEXT("sdftimecode"), TEXT("timecode") }, { TEXT("sdfassetpath"), TEXT("asset") },
			{ TEXT("tftoken"), TEXT("token") }, { TEXT("sdfpathexpression"), TEXT("pathexpression") } };
		for (const auto& Alias : Aliases)
		{
			if (Lower.Equals(Alias[0], ESearchCase::CaseSensitive))
			{
				return Alias[1];
			}
		}
		// GfVec<2-4><d|f|h|i>, GfQuat<d|f|h> and GfMatrix<2-4>d: the vector, quaternion and matrix value types.
		const auto Scalar = [](TCHAR Suffix) -> const TCHAR*
		{
			return Suffix == TEXT('d') ? TEXT("double") : Suffix == TEXT('f') ? TEXT("float") : Suffix == TEXT('h') ? TEXT("half")
				: Suffix == TEXT('i') ? TEXT("int") : nullptr;
		};
		const auto IsSize = [](TCHAR Digit) { return Digit >= TEXT('2') && Digit <= TEXT('4'); };
		if (Lower.Len() == 7 && Lower.StartsWith(TEXT("gfvec"), ESearchCase::CaseSensitive) && IsSize(Lower[5]) && Scalar(Lower[6]) != nullptr)
		{
			return FString::Printf(TEXT("%s%c"), Scalar(Lower[6]), Lower[5]);
		}
		if (Lower.Len() == 7 && Lower.StartsWith(TEXT("gfquat"), ESearchCase::CaseSensitive) && Lower[6] != TEXT('i') && Scalar(Lower[6]) != nullptr)
		{
			return FString::Printf(TEXT("quat%c"), Lower[6]);
		}
		if (Lower.Len() == 10 && Lower.StartsWith(TEXT("gfmatrix"), ESearchCase::CaseSensitive) && IsSize(Lower[8]) && Lower[9] == TEXT('d'))
		{
			return FString::Printf(TEXT("matrix%cd"), Lower[8]);
		}
		return Lower;
	}

	/** The type a JSON value is authored in when nothing names one: bool, double, string, or a dictionary for an object; empty for an array or null. */
	inline FString UsdTypeOfJson(const TSharedPtr<FJsonValue>& RawVal)
	{
		switch (RawVal.IsValid() ? RawVal->Type : EJson::None)
		{
			case EJson::Boolean: return TEXT("bool");
			case EJson::Number: return TEXT("double");
			case EJson::String: return TEXT("string");
			case EJson::Object: return TEXT("dictionary");
			default: return FString();
		}
	}

	/** Whether Text is exactly one number (a sign, digits with an optional fraction, an optional exponent): its value in OutNumber. */
	inline bool TryParseUsdNumber(const FString& Text, double& OutNumber)
	{
		const int32 Len = Text.Len();
		int32 Index = 0;
		const auto SkipDigits = [&Text, Len, &Index]()
		{
			const int32 Start = Index;
			while (Index < Len && FChar::IsDigit(Text[Index]))
			{
				++Index;
			}
			return Index - Start;
		};
		if (Index < Len && (Text[Index] == TEXT('+') || Text[Index] == TEXT('-')))
		{
			++Index;
		}
		int32 Digits = SkipDigits();
		if (Index < Len && Text[Index] == TEXT('.'))
		{
			++Index;
			Digits += SkipDigits();
		}
		if (Digits == 0)
		{
			return false;
		}
		if (Index < Len && (Text[Index] == TEXT('e') || Text[Index] == TEXT('E')))
		{
			++Index;
			if (Index < Len && (Text[Index] == TEXT('+') || Text[Index] == TEXT('-')))
			{
				++Index;
			}
			if (SkipDigits() == 0)
			{
				return false;
			}
		}
		if (Index != Len)
		{
			return false;
		}
		OutNumber = FCString::Atod(*Text);
		return FMath::IsFinite(OutNumber);
	}

	/**
	 * The delimiter of the engine's text of a reference or payload, between its asset path, prim path, layer offset and
	 * scale (UsdToUnreal::ConvertValue writes it, UnrealToUsd::ConvertValue reads it).
	 */
	inline const TCHAR* UsdArcDelimiter()
	{
		return TEXT("\x1c");
	}

	/**
	 * The text of a reference or payload from the engine's text of it: as a .usda file writes one,
	 * @asset@</prim/path> (offset = 10; scale = 2), each part only when it is set (an asset path holding @ between @@@,
	 * the offset and scale with the engine's six decimals, trailing zeros dropped). Not the engine's form: as it is.
	 */
	inline FString UsdArcText(const FString& EngineText)
	{
		TArray<FString> Parts;
		EngineText.ParseIntoArray(Parts, UsdArcDelimiter(), false);
		if (Parts.Num() != 4)
		{
			return EngineText;
		}
		FString Text;
		if (!Parts[0].IsEmpty())
		{
			Text += Parts[0].Contains(TEXT("@")) ? TEXT("@@@") + Parts[0] + TEXT("@@@") : TEXT("@") + Parts[0] + TEXT("@");
		}
		if (!Parts[1].IsEmpty())
		{
			Text += TEXT("<") + Parts[1] + TEXT(">");
		}
		const auto Short = [](FString Number)
		{
			if (Number.Contains(TEXT(".")))
			{
				while (Number.EndsWith(TEXT("0")))
				{
					Number.LeftChopInline(1);
				}
				Number.RemoveFromEnd(TEXT("."));
			}
			return Number;
		};
		TArray<FString> LayerOffset;
		if (FCString::Atod(*Parts[2]) != 0.0)
		{
			LayerOffset.Add(TEXT("offset = ") + Short(Parts[2]));
		}
		if (FCString::Atod(*Parts[3]) != 1.0)
		{
			LayerOffset.Add(TEXT("scale = ") + Short(Parts[3]));
		}
		if (LayerOffset.Num() > 0)
		{
			Text += (Text.IsEmpty() ? TEXT("(") : TEXT(" (")) + FString::Join(LayerOffset, TEXT("; ")) + TEXT(")");
		}
		return Text;
	}

	/**
	 * The engine's text of the reference or payload Text gives in UsdArcText's syntax, [@asset@][</prim/path>]
	 * [(offset = N; scale = M)] with every part optional (no asset path: the same layer stack; no prim path: the default
	 * prim). False, with OutError, for a delimiter left open, an invalid prim path, a layer offset that is not offset = N
	 * and scale = M, or anything after it.
	 */
	inline bool UsdArcFromText(const FString& Text, FString& OutEngineText, FString& OutError)
	{
		FString Rest = Text.TrimStartAndEnd();
		FString Asset;
		static const TCHAR* const Delimiters[] = { TEXT("@@@"), TEXT("@") };
		for (const TCHAR* Delimiter : Delimiters)
		{
			if (Rest.StartsWith(Delimiter, ESearchCase::CaseSensitive))
			{
				const int32 Open = FCString::Strlen(Delimiter);
				const int32 Close = Rest.Find(Delimiter, ESearchCase::CaseSensitive, ESearchDir::FromStart, Open);
				if (Close == INDEX_NONE)
				{
					OutError = FString::Printf(TEXT("the asset path opened with %s is not closed"), Delimiter);
					return false;
				}
				Asset = Rest.Mid(Open, Close - Open);
				Rest = Rest.Mid(Close + Open).TrimStart();
				break;
			}
		}
		FString PrimPath;
		if (Rest.StartsWith(TEXT("<"), ESearchCase::CaseSensitive))
		{
			const int32 Close = Rest.Find(TEXT(">"), ESearchCase::CaseSensitive);
			if (Close == INDEX_NONE)
			{
				OutError = TEXT("the prim path opened with < is not closed");
				return false;
			}
			PrimPath = Rest.Mid(1, Close - 1);
			Rest = Rest.Mid(Close + 1).TrimStart();
			FString PathError;
#if UE_VERSION_OLDER_THAN(5, 8, 0)
			if (!PrimPath.IsEmpty() && !IsValidSdfPathString(*PrimPath, &PathError))
#else
			if (!PrimPath.IsEmpty() && !UE::FSdfPath::IsValidPathString(*PrimPath, &PathError))
#endif
			{
				OutError = FString::Printf(TEXT("<%s> is not a valid prim path: %s"), *PrimPath, *PathError);
				return false;
			}
		}
		double Offset = 0.0;
		double Scale = 1.0;
		if (!Rest.IsEmpty())
		{
			if (!Rest.StartsWith(TEXT("("), ESearchCase::CaseSensitive) || !Rest.EndsWith(TEXT(")"), ESearchCase::CaseSensitive))
			{
				OutError = FString::Printf(TEXT("'%s' follows the asset and prim paths, where only a layer offset (offset = N; scale = M) may"), *Rest);
				return false;
			}
			TArray<FString> Settings;
			Rest.Mid(1, Rest.Len() - 2).ParseIntoArray(Settings, TEXT(";"), true);
			for (const FString& Setting : Settings)
			{
				FString Name;
				FString Number;
				double Parsed = 0.0;
				const bool bSplit = Setting.Split(TEXT("="), &Name, &Number);
				Name.TrimStartAndEndInline();
				const bool bOffset = Name.Equals(TEXT("offset"), ESearchCase::CaseSensitive);
				if (!bSplit || !(bOffset || Name.Equals(TEXT("scale"), ESearchCase::CaseSensitive))
					|| !TryParseUsdNumber(Number.TrimStartAndEnd(), Parsed))
				{
					OutError = FString::Printf(TEXT("'%s' in the layer offset is not offset = N or scale = M"), *Setting.TrimStartAndEnd());
					return false;
				}
				(bOffset ? Offset : Scale) = Parsed;
			}
		}
		if (Asset.Contains(UsdArcDelimiter()))
		{
			OutError = TEXT("the asset path holds the control character 0x1C, which the engine's conversion splits on");
			return false;
		}
		OutEngineText = FString::Join(TArray<FString>{ Asset, PrimPath, FString::Printf(TEXT("%.17g"), Offset),
			FString::Printf(TEXT("%.17g"), Scale) }, UsdArcDelimiter());
		return true;
	}

	/** The engine's text of one list op item of Items from its JSON text: an arc as UsdArcFromText reads it, a valid path, a non-empty token, any string. */
	inline bool UsdListOpItemFromText(EUsdListOpItems Items, const FString& Text, FString& OutEngineText, FString& OutError)
	{
		if (Items == EUsdListOpItems::Arc)
		{
			return UsdArcFromText(Text, OutEngineText, OutError);
		}
		FString PathError;
#if UE_VERSION_OLDER_THAN(5, 8, 0)
		if (Items == EUsdListOpItems::Path && !IsValidSdfPathString(*Text, &PathError))
#else
		if (Items == EUsdListOpItems::Path && !UE::FSdfPath::IsValidPathString(*Text, &PathError))
#endif
		{
			OutError = FString::Printf(TEXT("not a valid path: %s"), *PathError);
			return false;
		}
		if (Items == EUsdListOpItems::Token && Text.IsEmpty())
		{
			OutError = TEXT("a token cannot be empty");
			return false;
		}
		OutEngineText = Text;
		return true;
	}

	/**
	 * Adds the whole number the JSON value RawVal gives, item Index of list List, to that list of OutListOp, in the range
	 * of its item type: int [-2^31, 2^31), uint [0, 2^32), int64 [-2^63, 2^63), uint64 [0, 2^64). Seen holds the items
	 * of that list so far. False, with OutError, for anything else or a number already in the list.
	 */
	inline bool AddUsdListOpInteger(const TSharedPtr<FJsonValue>& RawVal, int32 List, int32 Index, TSet<uint64>& Seen,
		UEAgent::UsdSdk::FIntListOp& OutListOp, FString& OutError)
	{
		using ESdkItems = UEAgent::UsdSdk::EIntListOpItems;
		const TCHAR* const ListName = UsdListOpListName(List);
		if (!RawVal.IsValid() || RawVal->Type != EJson::Number)
		{
			OutError = FString::Printf(TEXT("item %d of list '%s' takes a whole number, not %s"), Index, ListName, *JsonShapeName(RawVal));
			return false;
		}
		const double Number = RawVal->AsNumber();
		const bool bSigned = OutListOp.Items == ESdkItems::Int || OutListOp.Items == ESdkItems::Int64;
		const TCHAR* const ItemType = OutListOp.Items == ESdkItems::Int ? TEXT("int") : OutListOp.Items == ESdkItems::UInt ? TEXT("uint")
			: OutListOp.Items == ESdkItems::Int64 ? TEXT("int64") : TEXT("uint64");
		const double Min = OutListOp.Items == ESdkItems::Int ? -2147483648.0 : OutListOp.Items == ESdkItems::Int64 ? -9223372036854775808.0 : 0.0;
		const double MaxExclusive = OutListOp.Items == ESdkItems::Int ? 2147483648.0 : OutListOp.Items == ESdkItems::UInt ? 4294967296.0
			: OutListOp.Items == ESdkItems::Int64 ? 9223372036854775808.0 : 18446744073709551616.0;
		if (Number != FMath::FloorToDouble(Number) || Number < Min || Number >= MaxExclusive)
		{
			OutError = FString::Printf(TEXT("item %d of list '%s' (%.17g) does not fit a list op of %s: it takes a whole number in the range of %s"),
				Index, ListName, Number, ItemType, ItemType);
			return false;
		}
		// Seen keys an item by its 64 bits, which tell any two values of one item type apart.
		const int64 SignedItem = bSigned ? static_cast<int64>(Number) : 0;
		const uint64 UnsignedItem = bSigned ? static_cast<uint64>(SignedItem) : static_cast<uint64>(Number);
		bool bAlreadySeen = false;
		Seen.Add(UnsignedItem, &bAlreadySeen);
		if (bAlreadySeen)
		{
			OutError = FString::Printf(TEXT("item %d of list '%s' (%.17g) is already in that list: a list op holds an item once per list"),
				Index, ListName, Number);
			return false;
		}
		if (bSigned)
		{
			OutListOp.Signed[List].Add(SignedItem);
		}
		else
		{
			OutListOp.Unsigned[List].Add(UnsignedItem);
		}
		return true;
	}

	/**
	 * The list op held as TypeName (an SdfListOp name, see UsdListOpKinds) that the JSON object RawVal gives: "explicit"
	 * alone, or any of "prepended", "appended", "deleted", "ordered" and "added"; each an array of items as
	 * get_usd_prim_metadata reads them. A list op of integers takes whole numbers in the range of its item type and is
	 * built by UEAgent::UsdSdk, an explicit empty one ({"explicit": []}) included; any other takes item texts (a token,
	 * string or path; a reference or payload in UsdArcText's syntax) and is built by the engine's
	 * UnrealToUsd::ConvertValue. False, with OutError, for another shape, a list of another name, explicit beside another
	 * list, an item twice in one list (SdfListOp would drop it unsaid), an invalid item, a list op of texts without items
	 * (the engine's conversion builds none, an explicit empty one included) or of integers with neither items nor an
	 * explicit list, or a value USD does not hold as TypeName.
	 */
	inline bool UsdListOpFromJson(const FString& TypeName, const TSharedPtr<FJsonValue>& RawVal, UE::FVtValue& Out, FString& OutError)
	{
		if (!RawVal.IsValid() || RawVal->Type != EJson::Object)
		{
			OutError = FString::Printf(TEXT("a list op of type %s takes an object of item lists, {\"explicit\": [...]} or {\"prepended\": [...], ")
				TEXT("\"appended\": [...], \"deleted\": [...], \"ordered\": [...], \"added\": [...]}, not %s"), *TypeName, *JsonShapeName(RawVal));
			return false;
		}
		const EUsdListOpItems Items = UsdListOpItemsOf(TypeName);
		UEAgent::UsdSdk::FIntListOp IntListOp;
		IntListOp.Items = UsdIntListOpItems(Items);
		const bool bIntegers = IntListOp.Items != UEAgent::UsdSdk::EIntListOpItems::None;
		UsdUtils::FConvertedVtValue Converted;
		Converted.SourceType = Items == EUsdListOpItems::Token ? UsdUtils::EUsdBasicDataTypes::Token : UsdUtils::EUsdBasicDataTypes::String;
		Converted.ExtraTypeInfo = TypeName;
		Converted.bIsArrayValued = true;
		Converted.bIsEmpty = false;
		Converted.Entries.SetNum(UsdListOpListCount);

		bool bExplicitGiven = false;
		bool bOtherItems = false;
		int32 ItemCount = 0;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Member : RawVal->AsObject()->Values)
		{
			int32 List = INDEX_NONE;
			for (int32 Candidate = 0; Candidate < UsdListOpListCount; ++Candidate)
			{
				if (Member.Key.Equals(UsdListOpListName(Candidate), ESearchCase::CaseSensitive))
				{
					List = Candidate;
				}
			}
			if (List == INDEX_NONE)
			{
				OutError = FString::Printf(TEXT("a list op has no list '%s': its lists are explicit, prepended, appended, deleted, ordered and added"),
					*Member.Key);
				return false;
			}
			if (!Member.Value.IsValid() || Member.Value->Type != EJson::Array)
			{
				OutError = FString::Printf(TEXT("list '%s' takes an array of %s, not %s"), *Member.Key,
					bIntegers ? TEXT("whole numbers") : TEXT("strings"), *JsonShapeName(Member.Value));
				return false;
			}
			const TArray<TSharedPtr<FJsonValue>>& Values = Member.Value->AsArray();
			bExplicitGiven |= List == UsdListOpExplicit;
			bOtherItems |= List != UsdListOpExplicit && Values.Num() > 0;
			if (bIntegers)
			{
				TSet<uint64> SeenNumbers;
				for (int32 Index = 0; Index < Values.Num(); ++Index)
				{
					if (!AddUsdListOpInteger(Values[Index], List, Index, SeenNumbers, IntListOp, OutError))
					{
						return false;
					}
					++ItemCount;
				}
				continue;
			}
			TSet<FString> Seen;
			for (int32 Index = 0; Index < Values.Num(); ++Index)
			{
				if (!Values[Index].IsValid() || Values[Index]->Type != EJson::String)
				{
					OutError = FString::Printf(TEXT("item %d of list '%s' takes a string, not %s"), Index, *Member.Key, *JsonShapeName(Values[Index]));
					return false;
				}
				const FString Text = Values[Index]->AsString();
				FString EngineText;
				FString ItemError;
				if (!UsdListOpItemFromText(Items, Text, EngineText, ItemError))
				{
					OutError = FString::Printf(TEXT("item %d of list '%s' ('%s'): %s"), Index, *Member.Key, *Text, *ItemError);
					return false;
				}
				bool bAlreadySeen = false;
				Seen.Add(EngineText, &bAlreadySeen);
				if (bAlreadySeen)
				{
					OutError = FString::Printf(TEXT("item %d of list '%s' ('%s') is already in that list: a list op holds an item once per list"),
						Index, *Member.Key, *Text);
					return false;
				}
				Converted.Entries[List].Emplace(TInPlaceType<FString>(), EngineText);
				++ItemCount;
			}
		}
		if (bExplicitGiven && bOtherItems)
		{
			OutError = TEXT("explicit replaces the whole list, so it cannot stand beside prepended, appended, deleted, ordered or added items");
			return false;
		}
		if (bIntegers)
		{
			if (ItemCount == 0 && !bExplicitGiven)
			{
				OutError = TEXT("a list op without items edits nothing: give items, or {\"explicit\": []} for an explicit empty list");
				return false;
			}
			IntListOp.bExplicit = bExplicitGiven;
#if UE_VERSION_OLDER_THAN(5, 8, 0)
			OutError = FString::Printf(TEXT("authoring a list op of type %s needs Unreal Engine 5.8 or later: %s"), *TypeName,
				TEXT("on Unreal Engine 5.7 (USD 25.08) writing a value built outside the engine's USD wrapper closes the editor, so it is refused; reading one works"));
			return false;
#else
			FString SdkError;
			if (!UEAgent::UsdSdk::MakeIntListOp(IntListOp, Out, SdkError))
			{
				OutError = FString::Printf(TEXT("the items could not be built into a list op of type %s: %s"), *TypeName, *SdkError);
				return false;
			}
			return true;
#endif
		}
		if (ItemCount == 0)
		{
			OutError = TEXT("a list op without items cannot be authored here: the engine's conversion builds none, not even an explicit empty one");
			return false;
		}

		Out = UE::FVtValue();
		if (!UnrealToUsd::ConvertValue(Converted, Out) || !Out.GetTypeName().Equals(TypeName, ESearchCase::CaseSensitive))
		{
			OutError = FString::Printf(TEXT("the items could not be converted to a list op of type %s (the conversion gave %s)"), *TypeName,
				Out.IsEmpty() ? TEXT("nothing") : *Out.GetTypeName());
			return false;
		}
		return true;
	}

	inline bool VtValueForMetadataType(const FString& Type, const TSharedPtr<FJsonValue>& RawVal, UE::FVtValue& Out, FString& OutError, int32 Depth);
	inline bool UsdDictionaryEntryFromJson(const TSharedPtr<FJsonValue>& RawVal, int32 Depth, UE::FVtValue& Out, FString& OutError);

	/**
	 * The VtDictionary the JSON object RawVal gives, Depth dictionaries down: each member an entry, converted by
	 * UsdDictionaryEntryFromJson. False, with OutError naming the entry, for another JSON type, a dictionary nested deeper
	 * than MaxUsdDictionaryDepth, or an entry that does not convert.
	 */
	inline bool UsdDictionaryFromJson(const TSharedPtr<FJsonValue>& RawVal, int32 Depth, UE::FVtValue& Out, FString& OutError)
	{
		if (!RawVal.IsValid() || RawVal->Type != EJson::Object)
		{
			OutError = FString::Printf(TEXT("a dictionary takes a JSON object of entries, not %s"), *JsonShapeName(RawVal));
			return false;
		}
		if (Depth >= MaxUsdDictionaryDepth)
		{
			OutError = FString::Printf(TEXT("dictionaries nest at most %d deep"), MaxUsdDictionaryDepth);
			return false;
		}
		TMap<FString, UE::FVtValue> Entries;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Member : RawVal->AsObject()->Values)
		{
			UE::FVtValue Entry;
			FString EntryError;
			if (!UsdDictionaryEntryFromJson(Member.Value, Depth + 1, Entry, EntryError))
			{
				OutError = FString::Printf(TEXT("entry '%s': %s"), *Member.Key, *EntryError);
				return false;
			}
			Entries.Add(Member.Key, MoveTemp(Entry));
		}
		Out = UE::FVtValue();
		Out.Set(Entries);
		return true;
	}

	/**
	 * The value of one dictionary entry, Depth dictionaries down, that the JSON value RawVal gives: a boolean as bool, a
	 * number as double, a string as string; an object with a "value" member as {type?, value} (the shape
	 * get_usd_prim_metadata reads an entry in), its value converted to its type by VtValueForMetadataType or, without
	 * one, by these rules (an object as a dictionary); any other object as a nested dictionary. False, with OutError, for
	 * null, an array without a type (it names no element type), or a {type?, value} marked truncated (a cut read).
	 */
	inline bool UsdDictionaryEntryFromJson(const TSharedPtr<FJsonValue>& RawVal, int32 Depth, UE::FVtValue& Out, FString& OutError)
	{
		Out = UE::FVtValue();
		switch (RawVal.IsValid() ? RawVal->Type : EJson::None)
		{
			case EJson::Boolean:
				Out.Set(RawVal->AsBool());
				return true;
			case EJson::Number:
				Out.Set(RawVal->AsNumber());
				return true;
			case EJson::String:
				Out.Set(RawVal->AsString());
				return true;
			case EJson::Array:
				OutError = TEXT("an array needs its type, as {\"type\": \"double[]\", \"value\": [...]}");
				return false;
			case EJson::Object:
			{
				const TSharedPtr<FJsonObject> Object = RawVal->AsObject();
				const TSharedPtr<FJsonValue> Inner = Object->TryGetField(TEXT("value"));
				if (!Inner.IsValid())
				{
					return UsdDictionaryFromJson(RawVal, Depth, Out, OutError);
				}
				bool bTruncated = false;
				if (Object->TryGetBoolField(TEXT("truncated"), bTruncated) && bTruncated)
				{
					OutError = TEXT("it is marked truncated, a cut read that cannot be written back whole");
					return false;
				}
				FString Type;
				if (Object->TryGetStringField(TEXT("type"), Type) && !Type.IsEmpty())
				{
					return VtValueForMetadataType(Type, Inner, Out, OutError, Depth);
				}
				if (Inner->Type == EJson::Object)
				{
					return UsdDictionaryFromJson(Inner, Depth, Out, OutError);
				}
				return UsdDictionaryEntryFromJson(Inner, Depth, Out, OutError);
			}
			default:
				OutError = TEXT("null has no USD value");
				return false;
		}
	}

	/**
	 * The FVtValue a metadata field or dictionary entry of type Type (any spelling CanonicalUsdTypeName takes) holds for
	 * the JSON value RawVal, Depth dictionaries down: a dictionary from a JSON object (UsdDictionaryFromJson), a list op
	 * from an object of item lists (UsdListOpFromJson), any other type through VtValueForDeclaredType. False, with
	 * OutError, as those are.
	 */
	inline bool VtValueForMetadataType(const FString& Type, const TSharedPtr<FJsonValue>& RawVal, UE::FVtValue& Out, FString& OutError,
		int32 Depth)
	{
		OutError.Reset();
		const FString Canonical = CanonicalUsdTypeName(Type);
		if (Canonical.Equals(TEXT("dictionary"), ESearchCase::CaseSensitive))
		{
			return UsdDictionaryFromJson(RawVal, Depth, Out, OutError);
		}
		if (Canonical.StartsWith(TEXT("SdfListOp<"), ESearchCase::IgnoreCase))
		{
			return UsdListOpFromJson(Canonical, RawVal, Out, OutError);
		}
		return VtValueForDeclaredType(Canonical, RawVal, Out, OutError, Depth == 0 ? TEXT("a metadata field") : TEXT("a value"));
	}

	// ---------------------------------------------------------------------------------------------------------------
	// Values read: the JSON form of any value, within the size one reply may take.
	// ---------------------------------------------------------------------------------------------------------------

	/**
	 * The characters the values of one reply may take in the bridge's pretty-printed JSON, shared by every value the reply
	 * converts: the entries of a dictionary, the items of a list op, the fields of a whole-prim metadata read. The bridge
	 * offloads a reply past 40000 characters (McpOversize) and keeps only its top-level scalars, so this leaves 10000 for
	 * the rest of the reply. One array alone stays within its own caps first (MaxArrayValuesRead numbers take 28200
	 * characters, MaxArrayStringCharsRead is 24000), so the budget binds only where one reply holds several values.
	 */
	constexpr int32 MaxReplyValueChars = 30000;

	/** What is left of MaxReplyValueChars while one reply is built. */
	struct FUsdJsonBudget
	{
		int32 Chars = MaxReplyValueChars;

		/** Takes Cost characters when that many are left: false, with nothing taken, when they are not. */
		bool Take(int32 Cost)
		{
			if (Cost > Chars)
			{
				return false;
			}
			Chars -= Cost;
			return true;
		}

		/** Takes Cost characters of what is left, for a value that is sent whole anyway. */
		void Spend(int32 Cost)
		{
			Chars = FMath::Max(0, Chars - Cost);
		}
	};

	/** The characters one number takes Depth dictionaries down: 47 at the top (see MaxArrayValuesRead), two tabs more per level. */
	inline int32 UsdJsonNumberCost(int32 Depth)
	{
		return 47 + 2 * Depth;
	}

	/**
	 * The characters a dictionary entry or a metadata field named Key, holding a value of type TypeName, takes Depth
	 * dictionaries down before its value: the key, the braces of its {type, value} object, the type name, and the length
	 * and truncated a container adds.
	 */
	inline int32 UsdJsonEntryCost(const FString& Key, const FString& TypeName, int32 Depth)
	{
		return UsdJsonStringCost(Key) + UsdJsonStringCost(TypeName) + 80 + 6 * Depth;
	}

	/** How UsdValueToJson ended. */
	enum class EUsdJsonRead : uint8
	{
		Converted,   // "value" holds the value, or its first part with "truncated" true
		NoJsonForm,  // "value" is null and a "note" says the type has no JSON form
		OverBudget,  // a value sent only whole did not fit what is left of the budget: Out is to be dropped
	};

	inline EUsdJsonRead UsdValueToJson(const UE::FVtValue& Value, FJsonObject& Out, FUsdJsonBudget& Budget, int32 Depth, bool bWholeMustFit);

	/** Writes the null "value" and the "note" saying TypeName has no JSON form into Out. */
	inline EUsdJsonRead UsdNoJsonForm(FJsonObject& Out, const FString& TypeName, FUsdJsonBudget& Budget)
	{
		const FString Note = FString::Printf(TEXT("type '%s' has no JSON form here: scalars, vectors, quaternions, matrices, path ")
			TEXT("expressions and arrays of those, dictionaries, and list ops of tokens, strings, paths, references, payloads or integers ")
			TEXT("are converted"), *TypeName);
		Budget.Spend(UsdJsonStringCost(Note));
		Out.SetField(TEXT("value"), MakeShared<FJsonValueNull>());
		Out.SetStringField(TEXT("note"), Note);
		return EUsdJsonRead::NoJsonForm;
	}

	/**
	 * Writes the item lists of a list op into Out: "value" is {explicit} when bExplicit, else {prepended, appended,
	 * deleted} plus ordered and added when they hold items, each a JSON array of the items Item gives (ItemCount(List)
	 * of them, Item(List, Index, OutCost) setting the characters each takes); "length" counts their items and "truncated"
	 * says whether all were sent. The items are cut, in that order, at MaxArrayValuesRead items, MaxChars characters, or
	 * what is left of Budget.
	 */
	inline void UsdListOpListsToJson(bool bExplicit, TFunctionRef<int32(int32)> ItemCount,
		TFunctionRef<TSharedPtr<FJsonValue>(int32, int32, int32&)> Item, int32 MaxChars, FJsonObject& Out, FUsdJsonBudget& Budget, int32 Depth)
	{
		TArray<int32, TInlineAllocator<UsdListOpListCount>> Lists;
		if (bExplicit)
		{
			Lists.Add(UsdListOpExplicit);
		}
		else
		{
			Lists.Append({ UsdListOpPrepended, UsdListOpAppended, UsdListOpDeleted });
			for (const int32 Optional : { UsdListOpOrdered, UsdListOpAdded })
			{
				if (ItemCount(Optional) > 0)
				{
					Lists.Add(Optional);
				}
			}
		}

		TSharedPtr<FJsonObject> ListsJson = MakeShared<FJsonObject>();
		int32 Total = 0;
		int32 Shown = 0;
		int32 Chars = 0;
		bool bCut = false;
		for (const int32 List : Lists)
		{
			Total += ItemCount(List);
			Budget.Spend(20 + 2 * Depth);  // the list's name and brackets
			TArray<TSharedPtr<FJsonValue>> Items;
			for (int32 Index = 0; !bCut && Index < ItemCount(List); ++Index)
			{
				int32 Cost = 0;
				TSharedPtr<FJsonValue> ItemJson = Item(List, Index, Cost);
				if (Shown >= MaxArrayValuesRead || Chars + Cost > MaxChars || !Budget.Take(Cost))
				{
					bCut = true;
					break;
				}
				Chars += Cost;
				++Shown;
				Items.Add(MoveTemp(ItemJson));
			}
			ListsJson->SetArrayField(UsdListOpListName(List), Items);
		}
		Out.SetObjectField(TEXT("value"), ListsJson);
		Out.SetNumberField(TEXT("length"), Total);
		Out.SetBoolField(TEXT("truncated"), Shown < Total);
	}

	/**
	 * Writes the list op Value, which the engine converted into Converted (its six item lists), into Out as
	 * UsdListOpListsToJson does, each item as its text (a reference or payload as UsdArcText writes it), cut at
	 * MaxArrayStringCharsRead characters.
	 */
	inline void UsdListOpToJson(const UE::FVtValue& Value, const UsdUtils::FConvertedVtValue& Converted, FJsonObject& Out,
		FUsdJsonBudget& Budget, int32 Depth)
	{
		const auto ItemCount = [&Converted](int32 List) { return Converted.Entries.IsValidIndex(List) ? Converted.Entries[List].Num() : 0; };
		int32 AllItems = 0;
		for (int32 List = 0; List < UsdListOpListCount; ++List)
		{
			AllItems += ItemCount(List);
		}
		// The engine fills only the explicit list of an explicit list op, so one without items is told apart by USD's own
		// text of the value, "SdfTokenListOp(Explicit Items: [])".
		const bool bExplicit = ItemCount(UsdListOpExplicit) > 0
			|| (AllItems == 0 && Value.Get<FString>().Contains(TEXT("(Explicit Items: ["), ESearchCase::CaseSensitive));

		const bool bArcs = UsdListOpItemsOf(Converted.ExtraTypeInfo) == EUsdListOpItems::Arc;
		const auto Item = [&Converted, bArcs, Depth](int32 List, int32 Index, int32& OutCost) -> TSharedPtr<FJsonValue>
		{
			const FString* Raw = Converted.Entries[List][Index].TryGet<FString>();
			const FString Text = Raw == nullptr ? FString() : bArcs ? UsdArcText(*Raw) : *Raw;
			OutCost = UsdJsonStringCost(Text) + 2 * Depth;
			return MakeShared<FJsonValueString>(Text);
		};
		UsdListOpListsToJson(bExplicit, ItemCount, Item, MaxArrayStringCharsRead, Out, Budget, Depth);
	}

	/**
	 * Writes the list op of integers ListOp, as UEAgent::UsdSdk read it, into Out as UsdListOpListsToJson does, each
	 * item as a JSON number.
	 */
	inline void UsdIntListOpToJson(const UEAgent::UsdSdk::FIntListOp& ListOp, FJsonObject& Out, FUsdJsonBudget& Budget, int32 Depth)
	{
		const bool bSigned = ListOp.Items == UEAgent::UsdSdk::EIntListOpItems::Int || ListOp.Items == UEAgent::UsdSdk::EIntListOpItems::Int64;
		const auto ItemCount = [&ListOp, bSigned](int32 List) { return bSigned ? ListOp.Signed[List].Num() : ListOp.Unsigned[List].Num(); };
		const auto Item = [&ListOp, bSigned, Depth](int32 List, int32 Index, int32& OutCost) -> TSharedPtr<FJsonValue>
		{
			OutCost = UsdJsonNumberCost(Depth);
			return MakeShared<FJsonValueNumber>(bSigned ? static_cast<double>(ListOp.Signed[List][Index])
				: static_cast<double>(ListOp.Unsigned[List][Index]));
		};
		UsdListOpListsToJson(ListOp.bExplicit, ItemCount, Item, MAX_int32, Out, Budget, Depth);
	}

	/**
	 * Writes the array of path expressions Value into Out: "value" is a JSON array of the text of each expression, cut
	 * at MaxArrayValuesRead elements, MaxArrayStringCharsRead characters or what is left of Budget; "length" counts the
	 * elements and "truncated" says whether all were sent. False, with Out untouched, when UEAgent::UsdSdk cannot read it.
	 */
	inline bool UsdPathExpressionArrayToJson(const UE::FVtValue& Value, FJsonObject& Out, FUsdJsonBudget& Budget, int32 Depth)
	{
		TArray<FString> Texts;
		int32 Length = 0;
		FString ReadError;
		if (!UEAgent::UsdSdk::ReadPathExpressionArray(Value, MaxArrayValuesRead, Texts, Length, ReadError))
		{
			return false;
		}
		TArray<TSharedPtr<FJsonValue>> Items;
		Items.Reserve(Texts.Num());
		int32 Chars = 0;
		for (const FString& Text : Texts)
		{
			const int32 Cost = UsdJsonStringCost(Text) + 2 * Depth;
			if (Chars + Cost > MaxArrayStringCharsRead || !Budget.Take(Cost))
			{
				break;
			}
			Chars += Cost;
			Items.Add(MakeShared<FJsonValueString>(Text));
		}
		Out.SetArrayField(TEXT("value"), Items);
		Out.SetNumberField(TEXT("length"), Length);
		Out.SetBoolField(TEXT("truncated"), Items.Num() < Length);
		return true;
	}

	/**
	 * Writes the dictionary Value, Depth dictionaries down, into Out: "value" is an object of its entries in key order,
	 * each {type, value} as UsdValueToJson writes it one level down; "length" counts the entries and "truncated" says
	 * whether all were sent. Entries stop at the first that does not fit what is left of Budget; one nested deeper than
	 * MaxUsdDictionaryDepth is sent without entries.
	 */
	inline void UsdDictionaryToJson(const UE::FVtValue& Value, FJsonObject& Out, FUsdJsonBudget& Budget, int32 Depth)
	{
		const TMap<FString, UE::FVtValue> Entries = Value.Get<TMap<FString, UE::FVtValue>>();
		TSharedPtr<FJsonObject> EntriesJson = MakeShared<FJsonObject>();
		int32 Shown = 0;
		if (Depth < MaxUsdDictionaryDepth)
		{
			for (const TPair<FString, UE::FVtValue>& Entry : Entries)
			{
				TSharedPtr<FJsonObject> EntryJson = MakeShared<FJsonObject>();
				if (!Budget.Take(UsdJsonEntryCost(Entry.Key, Entry.Value.GetTypeName(), Depth + 1))
					|| UsdValueToJson(Entry.Value, *EntryJson, Budget, Depth + 1, true) == EUsdJsonRead::OverBudget)
				{
					break;
				}
				EntriesJson->SetObjectField(Entry.Key, EntryJson);
				++Shown;
			}
		}
		Out.SetObjectField(TEXT("value"), EntriesJson);
		Out.SetNumberField(TEXT("length"), Entries.Num());
		Out.SetBoolField(TEXT("truncated"), Shown < Entries.Num());
	}

	/**
	 * Writes Value into Out as "type" (the type name USD gives the held value) and "value", Depth dictionaries down: a
	 * scalar as a bool, number or string; a path expression as its text; a vector or quaternion as an array of numbers
	 * (a quaternion x, y, z, then the real part w); a matrix as an array of rows; an Sdf enum such as SdfSpecifier as its
	 * number; an array as a JSON array of those (an array of path expressions as their texts); a dictionary as an object
	 * of {type, value} entries (UsdDictionaryToJson); a list op as its item lists (UsdListOpToJson), a list op of integers
	 * with numbers for items (UsdIntListOpToJson). A container adds "length" and "truncated": an array is cut at
	 * MaxArrayElementsRead elements, fewer for strings past MaxArrayStringCharsRead, and every container at what is left
	 * of Budget. An empty value is "type": "empty" with a null "value"; any other type gets a null "value" and a "note"
	 * (NoJsonForm). A value sent whole (a scalar, one vector or matrix) takes its characters from Budget: with
	 * bWholeMustFit only when they are left (else OverBudget), without it always.
	 *
	 * Each scalar type is read with its own C++ type: FVtValue::Get<T> of a T the value does not hold raises a USD
	 * coding error and returns a default T, so a float read as a double comes back 0. The type name is the
	 * demangled C++ type of the held value ("float", "pxr_half::half", "__int64", "TfToken"), not the Sdf
	 * value type name; both spellings are accepted, as the engine's own USD metadata translation does. A list op of
	 * integers and an array of path expressions, which the engine has no conversion for, are read by UEAgent::UsdSdk;
	 * every other type but a dictionary and a path expression goes through the engine's UsdToUnreal::ConvertValue, which
	 * dispatches on the held type itself.
	 */
	inline EUsdJsonRead UsdValueToJson(const UE::FVtValue& Value, FJsonObject& Out, FUsdJsonBudget& Budget, int32 Depth, bool bWholeMustFit)
	{
		if (Value.IsEmpty())
		{
			Out.SetStringField(TEXT("type"), TEXT("empty"));
			Out.SetField(TEXT("value"), MakeShared<FJsonValueNull>());
			return EUsdJsonRead::Converted;
		}

		const FString TypeName = Value.GetTypeName();
		Out.SetStringField(TEXT("type"), TypeName);

		// A value sent whole and the characters it takes.
		TSharedPtr<FJsonValue> Whole;
		int32 Cost = UsdJsonNumberCost(Depth);
		const auto SetText = [&Whole, &Cost, Depth](const FString& Text)
		{
			Whole = MakeShared<FJsonValueString>(Text);
			Cost = UsdJsonStringCost(Text) + 2 * Depth;
		};
		if (TypeName == TEXT("bool"))
		{
			Whole = MakeShared<FJsonValueBoolean>(Value.Get<bool>());
		}
		else if (TypeName == TEXT("unsigned char") || TypeName == TEXT("uchar"))
		{
			Whole = MakeShared<FJsonValueNumber>(static_cast<double>(Value.Get<uint8>()));
		}
		else if (TypeName == TEXT("int") || TypeName == TEXT("int32"))
		{
			Whole = MakeShared<FJsonValueNumber>(static_cast<double>(Value.Get<int32>()));
		}
		else if (TypeName == TEXT("unsigned int") || TypeName == TEXT("uint"))
		{
			Whole = MakeShared<FJsonValueNumber>(static_cast<double>(Value.Get<uint32>()));
		}
		else if (TypeName == TEXT("__int64") || TypeName == TEXT("int64"))
		{
			Whole = MakeShared<FJsonValueNumber>(static_cast<double>(Value.Get<int64>()));
		}
		else if (TypeName == TEXT("unsigned __int64") || TypeName == TEXT("uint64"))
		{
			Whole = MakeShared<FJsonValueNumber>(static_cast<double>(Value.Get<uint64>()));
		}
		else if (TypeName == TEXT("pxr_half::half") || TypeName == TEXT("half"))
		{
			Whole = MakeShared<FJsonValueNumber>(static_cast<double>(Value.Get<FFloat16>().GetFloat()));
		}
		else if (TypeName == TEXT("float"))
		{
			Whole = MakeShared<FJsonValueNumber>(static_cast<double>(Value.Get<float>()));
		}
		else if (TypeName == TEXT("double"))
		{
			Whole = MakeShared<FJsonValueNumber>(Value.Get<double>());
		}
		else if (TypeName == TEXT("SdfTimeCode") || TypeName == TEXT("timecode"))
		{
			Whole = MakeShared<FJsonValueNumber>(Value.Get<FSdfTimeCode>().Time);
		}
		else if (TypeName == TEXT("SdfAssetPath") || TypeName == TEXT("asset"))
		{
			// The authored path; the string form of the value would wrap it in @ delimiters.
			SetText(Value.Get<FSdfAssetPath>().AssetPath);
		}
		else if (TypeName == TEXT("string") || TypeName == TEXT("TfToken") || TypeName == TEXT("token"))
		{
			// Get<FString> reads a string as it is and writes any other value, a token included, in its
			// USD string form, so a token keeps its exact case (an FName would not).
			SetText(Value.Get<FString>());
		}
		else if (TypeName == TEXT("SdfPathExpression"))
		{
			// The engine has no conversion for a path expression; Get<FString> writes it in its USD text form, the expression.
			SetText(Value.Get<FString>());
		}
		else if (TypeName == TEXT("VtDictionary"))
		{
			UsdDictionaryToJson(Value, Out, Budget, Depth);
			return EUsdJsonRead::Converted;
		}
		else if (UEAgent::UsdSdk::GetIntListOpItems(Value) != UEAgent::UsdSdk::EIntListOpItems::None)
		{
			// The engine has no conversion for a list op of integers (inactiveIds...); the USD SDK reads its lists.
			UEAgent::UsdSdk::FIntListOp ListOp;
			FString ReadError;
			if (!UEAgent::UsdSdk::ReadIntListOp(Value, ListOp, ReadError))
			{
				return UsdNoJsonForm(Out, TypeName, Budget);
			}
			UsdIntListOpToJson(ListOp, Out, Budget, Depth);
			return EUsdJsonRead::Converted;
		}
		else if (UEAgent::UsdSdk::IsPathExpressionArray(Value))
		{
			// The text of the whole array cannot be split into its elements (an expression may hold commas); the USD SDK
			// gives the text of each.
			if (!UsdPathExpressionArrayToJson(Value, Out, Budget, Depth))
			{
				return UsdNoJsonForm(Out, TypeName, Budget);
			}
			return EUsdJsonRead::Converted;
		}
		else
		{
			// An array longer than MaxArrayElementsRead is cut first, so only its head is converted and sent; Length
			// keeps the full count. INDEX_NONE: not measured here (a token array), the converted entries count it.
			UE::FVtValue Head;
			int32 Length = INDEX_NONE;
			if (Value.IsArrayValued())
			{
				FString ElementType = UsdUtils::GetImpliedTypeName(Value).ToLower();
				const FUsdValueShape* ElementShape = ElementType.RemoveFromEnd(TEXT("[]")) ? FindUsdValueShape(ElementType) : nullptr;
				if (ElementShape != nullptr)
				{
					Length = UsdArrayHead(Value, ElementShape->BasicType, MaxArrayElementsRead(*ElementShape), Head);
				}
			}

			// Vectors, quaternions, matrices, Sdf enums and arrays: the engine converts the value into one entry of
			// components per value (per element of an array). A non-empty ExtraTypeInfo marks a list op (apiSchemas,
			// references...), whose entries are its six item lists rather than values.
			UsdUtils::FConvertedVtValue Converted;
			if (!UsdToUnreal::ConvertValue(Head.IsEmpty() ? Value : Head, Converted) || Converted.bIsEmpty)
			{
				return UsdNoJsonForm(Out, TypeName, Budget);
			}
			if (!Converted.ExtraTypeInfo.IsEmpty())
			{
				UsdListOpToJson(Value, Converted, Out, Budget, Depth);
				return EUsdJsonRead::Converted;
			}
			if (Converted.bIsArrayValued)
			{
				const FUsdValueShape* ElementShape = FindUsdValueShape(Converted.SourceType);
				const int32 MaxElements = ElementShape != nullptr ? MaxArrayElementsRead(*ElementShape) : MaxArrayValuesRead;
				const int32 Total = Length != INDEX_NONE ? Length : Converted.Entries.Num();
				const int32 Limit = FMath::Min(Converted.Entries.Num(), MaxElements);
				const bool bStrings = UsdComponentKind(Converted.SourceType) == EUsdComponentKind::String;
				int32 StringChars = 0;
				int32 Shown = 0;
				TArray<TSharedPtr<FJsonValue>> Items;
				Items.Reserve(Limit);
				for (; Shown < Limit; ++Shown)
				{
					const UsdUtils::FConvertedVtValueEntry& Entry = Converted.Entries[Shown];
					int32 EntryCost = Entry.Num() * UsdJsonNumberCost(Depth);
					if (bStrings)
					{
						EntryCost = 0;
						for (const UsdUtils::FConvertedVtValueComponent& Component : Entry)
						{
							if (const FString* Text = Component.TryGet<FString>())
							{
								EntryCost += UsdJsonStringCost(*Text) + 2 * Depth;
							}
						}
						if (StringChars + EntryCost > MaxArrayStringCharsRead)
						{
							break;
						}
					}
					if (!Budget.Take(EntryCost))
					{
						break;
					}
					StringChars += EntryCost;
					Items.Add(UsdEntryToJson(Entry, Converted.SourceType));
				}
				Out.SetArrayField(TEXT("value"), Items);
				Out.SetNumberField(TEXT("length"), Total);
				Out.SetBoolField(TEXT("truncated"), Shown < Total);
				return EUsdJsonRead::Converted;
			}
			if (Converted.Entries.Num() != 1)
			{
				return UsdNoJsonForm(Out, TypeName, Budget);
			}
			Whole = UsdEntryToJson(Converted.Entries[0], Converted.SourceType);
			Cost = FMath::Max(1, Converted.Entries[0].Num()) * UsdJsonNumberCost(Depth);
		}

		if (bWholeMustFit)
		{
			if (!Budget.Take(Cost))
			{
				return EUsdJsonRead::OverBudget;
			}
		}
		else
		{
			Budget.Spend(Cost);
		}
		Out.SetField(TEXT("value"), Whole);
		return EUsdJsonRead::Converted;
	}

	/**
	 * Writes Value into Out as UsdValueToJson does for the one value of a reply, with the whole budget: a scalar is
	 * always sent. True, unless its type has no JSON form (a null "value" and a "note").
	 */
	inline bool VtValueToJson(const UE::FVtValue& Value, FJsonObject& Out)
	{
		FUsdJsonBudget Budget;
		return UsdValueToJson(Value, Out, Budget, 0, false) != EUsdJsonRead::NoJsonForm;
	}

	/**
	 * The type the held Value is authored back in, spelled as VtValueForMetadataType takes it: a scalar, vector,
	 * quaternion, matrix or array as its Sdf value type ("token", "double3", "string[]"), a path expression as
	 * "pathexpression" (an array "pathexpression[]"), a dictionary as "dictionary", a list op the engine or
	 * UEAgent::UsdSdk converts as the held type name of its kind ("SdfListOp<TfToken>", "SdfListOp<__int64>"). Empty for
	 * an empty value or any other type (an Sdf enum such as SdfSpecifier).
	 */
	inline FString UsdDeclaredTypeOf(const UE::FVtValue& Value)
	{
		if (Value.IsEmpty())
		{
			return FString();
		}
		const FString Canonical = CanonicalUsdTypeName(Value.GetTypeName());
		if (Canonical.Equals(TEXT("dictionary"), ESearchCase::CaseSensitive) || Canonical.Equals(TEXT("pathexpression"), ESearchCase::CaseSensitive)
			|| Canonical.Equals(TEXT("pathexpression[]"), ESearchCase::CaseSensitive))
		{
			return Canonical;
		}
		if (const FUsdListOpKind* IntKind = UsdIntListOpKind(UEAgent::UsdSdk::GetIntListOpItems(Value)))
		{
			return IntKind->TypeName;
		}
		if (Canonical.StartsWith(TEXT("SdfListOp<"), ESearchCase::IgnoreCase))
		{
			// Only a list op the engine converts can be written back; its ExtraTypeInfo is the name the converters key on.
			UsdUtils::FConvertedVtValue Converted;
			return UsdToUnreal::ConvertValue(Value, Converted) ? Converted.ExtraTypeInfo : FString();
		}
		FString Base = Canonical;
		Base.RemoveFromEnd(TEXT("[]"));
		if (FindUsdValueShape(Base) != nullptr)
		{
			return Canonical;
		}
		// The Sdf value type USD finds for the held type, for a C++ name CanonicalUsdTypeName has no spelling for.
		const FString Implied = UsdUtils::GetImpliedTypeName(Value).ToLower();
		FString ImpliedBase = Implied;
		ImpliedBase.RemoveFromEnd(TEXT("[]"));
		return !ImpliedBase.IsEmpty() && FindUsdValueShape(ImpliedBase) != nullptr ? Implied : FString();
	}

	/**
	 * The type USD registers for a standard prim metadata field, which a prim resolves to nothing until it is authored
	 * (no prim definition gives these fields a fallback): the Sdf prim fields as SdfPrimSpec types them (kind, typeName
	 * and symmetryFunction tokens; comment, documentation, displayName, prefix, suffix and symmetricPeer strings; active,
	 * hidden and instanceable bools; the dictionaries; the list ops of tokens, strings, paths and arcs) and the prim
	 * fields the shipped plugins register (apiSchemas, sdrMetadata, uiHints, payloadAssetDependencies, and UsdGeom's
	 * inactiveIds, a list op of int64). Fields the converters cannot author (specifier and permission enums, primOrder
	 * and propertyOrder token vectors) are left out. Empty for any other field.
	 */
	inline FString UsdWellKnownMetadataType(const FString& Key)
	{
		static const TCHAR* const Fields[][2] = {
			{ TEXT("active"), TEXT("bool") }, { TEXT("hidden"), TEXT("bool") }, { TEXT("instanceable"), TEXT("bool") },
			{ TEXT("kind"), TEXT("token") }, { TEXT("typeName"), TEXT("token") }, { TEXT("symmetryFunction"), TEXT("token") },
			{ TEXT("comment"), TEXT("string") }, { TEXT("documentation"), TEXT("string") }, { TEXT("displayName"), TEXT("string") },
			{ TEXT("prefix"), TEXT("string") }, { TEXT("suffix"), TEXT("string") }, { TEXT("symmetricPeer"), TEXT("string") },
			{ TEXT("customData"), TEXT("dictionary") }, { TEXT("assetInfo"), TEXT("dictionary") }, { TEXT("clips"), TEXT("dictionary") },
			{ TEXT("prefixSubstitutions"), TEXT("dictionary") }, { TEXT("suffixSubstitutions"), TEXT("dictionary") },
			{ TEXT("symmetryArguments"), TEXT("dictionary") }, { TEXT("sdrMetadata"), TEXT("dictionary") },
			{ TEXT("uiHints"), TEXT("dictionary") }, { TEXT("payloadAssetDependencies"), TEXT("asset[]") },
			{ TEXT("apiSchemas"), TEXT("SdfListOp<TfToken>") }, { TEXT("clipSets"), TEXT("SdfListOp<string>") },
			{ TEXT("variantSetNames"), TEXT("SdfListOp<string>") }, { TEXT("references"), TEXT("SdfListOp<SdfReference>") },
			{ TEXT("payload"), TEXT("SdfListOp<SdfPayload>") }, { TEXT("inheritPaths"), TEXT("SdfListOp<SdfPath>") },
			{ TEXT("specializes"), TEXT("SdfListOp<SdfPath>") }, { TEXT("inactiveIds"), TEXT("SdfListOp<__int64>") } };
		for (const auto& Field : Fields)
		{
			if (Key.Equals(Field[0], ESearchCase::CaseSensitive))
			{
				return Field[1];
			}
		}
		return FString();
	}

	/**
	 * The {"type"?, "value"} object a "value" param carries: the param itself when it is an object, else the
	 * raw value wrapped as {"value": <value>}. Null when the param is absent.
	 */
	inline TSharedPtr<FJsonObject> TypedValueObject(const TSharedPtr<FJsonValue>& RawValue)
	{
		if (!RawValue.IsValid())
		{
			return nullptr;
		}
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (RawValue->TryGetObject(Object) && Object && Object->IsValid())
		{
			return *Object;
		}
		TSharedPtr<FJsonObject> Wrapped = MakeShared<FJsonObject>();
		Wrapped->SetField(TEXT("value"), RawValue);
		return Wrapped;
	}
}
