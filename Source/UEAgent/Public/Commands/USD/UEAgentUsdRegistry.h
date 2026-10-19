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

	/** Whether InPath can name a prim: false, with OutError, when it is empty or not a valid SdfPath string. */
	inline bool IsValidPrimPath(const FString& InPath, FString& OutError)
	{
		if (InPath.IsEmpty())
		{
			OutError = TEXT("prim_path is empty");
			return false;
		}
		FString SdfError;
		if (!UE::FSdfPath::IsValidPathString(*InPath, &SdfError))
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

	/**
	 * The FVtValue a {"type"?, "value"} object describes. An explicit "type" picks the UE type: bool/boolean,
	 * int/integer/int32, int64, float/double/number (as double), string, token/name (as FName) or asset (the
	 * raw string; resolving it needs the USD asset resolver context). Without one, the JSON type of "value"
	 * does: bool, number (as double) or string. False, with OutError, for a null object, a missing "value", a
	 * token of NAME_SIZE characters or more, or a value no rule converts. Setting a value whose UE type does not
	 * match the USD type of the field fails silently in pxr::, so the caller checks what Set returns; an
	 * attribute value is built with VtValueForDeclaredType instead.
	 */
	inline bool VtValueFromJson(const TSharedPtr<FJsonObject>& TypedValue, UE::FVtValue& Out, FString& OutError)
	{
		OutError.Reset();
		if (!TypedValue.IsValid())
		{
			OutError = TEXT("value object is null");
			return false;
		}

		FString Type;
		TypedValue->TryGetStringField(TEXT("type"), Type);

		TSharedPtr<FJsonValue> RawVal = TypedValue->TryGetField(TEXT("value"));
		if (!RawVal.IsValid())
		{
			OutError = TEXT("missing 'value' field");
			return false;
		}

		const FString TypeLow = Type.ToLower();
		if (TypeLow == TEXT("bool") || TypeLow == TEXT("boolean"))
		{
			Out.Set(RawVal->AsBool());
			return true;
		}
		if (TypeLow == TEXT("int") || TypeLow == TEXT("integer") || TypeLow == TEXT("int32"))
		{
			Out.Set(static_cast<int32>(RawVal->AsNumber()));
			return true;
		}
		if (TypeLow == TEXT("int64"))
		{
			Out.Set(static_cast<int64>(RawVal->AsNumber()));
			return true;
		}
		if (TypeLow == TEXT("float") || TypeLow == TEXT("double") || TypeLow == TEXT("number"))
		{
			Out.Set(RawVal->AsNumber());
			return true;
		}
		if (TypeLow == TEXT("string"))
		{
			Out.Set(RawVal->AsString());
			return true;
		}
		if (TypeLow == TEXT("token") || TypeLow == TEXT("name"))
		{
			const FString Token = RawVal->AsString();
			// An FName of NAME_SIZE characters or more fails a check() and kills the editor.
			if (Token.Len() >= NAME_SIZE)
			{
				OutError = FString::Printf(TEXT("a token is limited to %d characters, got %d"), NAME_SIZE - 1, Token.Len());
				return false;
			}
			Out.Set(FName(*Token));
			return true;
		}
		if (TypeLow == TEXT("asset"))
		{
			Out.Set(RawVal->AsString());
			return true;
		}

		// A string stays a USD "string": a token has to be asked for with {"type": "token"}.
		switch (RawVal->Type)
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
			case EJson::Null:
			case EJson::None:
			case EJson::Array:
			case EJson::Object:
			default:
				OutError = FString::Printf(
					TEXT("unsupported value type (type='%s', json=%d)"), *Type, (int32)RawVal->Type);
				return false;
		}
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
	 * those are built by the engine's UnrealToUsd::ConvertValue. False, with OutError, for opaque, group or
	 * pathExpression, an unknown type, or a value of another shape or that does not fit.
	 */
	inline bool VtValueForDeclaredType(const FString& DeclaredType, const TSharedPtr<FJsonValue>& RawVal, UE::FVtValue& Out,
		FString& OutError)
	{
		OutError.Reset();
		if (!RawVal.IsValid() || RawVal->IsNull())
		{
			OutError = TEXT("the value is missing or null");
			return false;
		}
		const FString Type = DeclaredType.ToLower();

		const auto Mismatch = [&RawVal, &DeclaredType, &OutError](const TCHAR* Wanted)
		{
			OutError = FString::Printf(TEXT("an attribute of type %s takes %s, not %s"), *DeclaredType, Wanted, JsonTypeName(RawVal->Type));
			return false;
		};
		// A whole JSON number in [Min, MaxExclusive): the range of the integer type it is stored as.
		const auto ReadWhole = [&RawVal, &DeclaredType, &OutError](double Min, double MaxExclusive, double& OutNumber)
		{
			OutNumber = RawVal->AsNumber();
			if (OutNumber != FMath::FloorToDouble(OutNumber) || OutNumber < Min || OutNumber >= MaxExclusive)
			{
				OutError = FString::Printf(TEXT("%.17g does not fit an attribute of type %s: it takes a whole number in the range of that type"),
					OutNumber, *DeclaredType);
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
				TEXT("an attribute of type %s is not one this tool can author: the declared type must be a scalar (bool, uchar, int, uint, ")
				TEXT("int64, uint64, half, float, double, timecode, string, token, asset), a vector, point, normal, color or texCoord, ")
				TEXT("a quaternion, a matrix, or an array of one of them"), *DeclaredType);
			return false;
		}

		UsdUtils::FConvertedVtValue Converted;
		Converted.SourceType = Shape->BasicType;
		Converted.bIsArrayValued = bArray;
		Converted.bIsEmpty = false;
		const auto Subject = [&DeclaredType]() { return FString::Printf(TEXT("an attribute of type %s"), *DeclaredType); };
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

	/**
	 * Writes Value into Out as "type" (the type name USD gives the held value) and "value" and returns true: a
	 * scalar as a bool, number or string; a vector or quaternion as an array of numbers (a quaternion x, y, z, then the
	 * real part w); a matrix as an array of rows; an array as a JSON array of those, plus "length" (its element count)
	 * and "truncated" (true when only its first elements were read: MaxArrayElementsRead, fewer for strings past
	 * MaxArrayStringCharsRead); an Sdf enum such as
	 * SdfSpecifier as its number. An empty value is "type": "empty" with a null "value". A value of any other type (a
	 * dictionary, a list op, a path) only gets its "type", and this returns false so the caller says how to show it.
	 *
	 * Each scalar type is read with its own C++ type: FVtValue::Get<T> of a T the value does not hold raises a USD
	 * coding error and returns a default T, so a float read as a double comes back 0. The type name is the
	 * demangled C++ type of the held value ("float", "pxr_half::half", "__int64", "TfToken"), not the Sdf
	 * value type name; both spellings are accepted, as the engine's own USD metadata translation does. Every other
	 * type goes through the engine's UsdToUnreal::ConvertValue, which dispatches on the held type itself.
	 */
	inline bool VtValueToJson(const UE::FVtValue& Value, FJsonObject& Out)
	{
		if (Value.IsEmpty())
		{
			Out.SetStringField(TEXT("type"), TEXT("empty"));
			Out.SetField(TEXT("value"), MakeShared<FJsonValueNull>());
			return true;
		}

		const FString TypeName = Value.GetTypeName();
		Out.SetStringField(TEXT("type"), TypeName);

		if (TypeName == TEXT("bool"))
		{
			Out.SetBoolField(TEXT("value"), Value.Get<bool>());
		}
		else if (TypeName == TEXT("unsigned char") || TypeName == TEXT("uchar"))
		{
			Out.SetNumberField(TEXT("value"), static_cast<double>(Value.Get<uint8>()));
		}
		else if (TypeName == TEXT("int") || TypeName == TEXT("int32"))
		{
			Out.SetNumberField(TEXT("value"), static_cast<double>(Value.Get<int32>()));
		}
		else if (TypeName == TEXT("unsigned int") || TypeName == TEXT("uint"))
		{
			Out.SetNumberField(TEXT("value"), static_cast<double>(Value.Get<uint32>()));
		}
		else if (TypeName == TEXT("__int64") || TypeName == TEXT("int64"))
		{
			Out.SetNumberField(TEXT("value"), static_cast<double>(Value.Get<int64>()));
		}
		else if (TypeName == TEXT("unsigned __int64") || TypeName == TEXT("uint64"))
		{
			Out.SetNumberField(TEXT("value"), static_cast<double>(Value.Get<uint64>()));
		}
		else if (TypeName == TEXT("pxr_half::half") || TypeName == TEXT("half"))
		{
			Out.SetNumberField(TEXT("value"), static_cast<double>(Value.Get<FFloat16>().GetFloat()));
		}
		else if (TypeName == TEXT("float"))
		{
			Out.SetNumberField(TEXT("value"), static_cast<double>(Value.Get<float>()));
		}
		else if (TypeName == TEXT("double"))
		{
			Out.SetNumberField(TEXT("value"), Value.Get<double>());
		}
		else if (TypeName == TEXT("SdfTimeCode") || TypeName == TEXT("timecode"))
		{
			Out.SetNumberField(TEXT("value"), Value.Get<FSdfTimeCode>().Time);
		}
		else if (TypeName == TEXT("SdfAssetPath") || TypeName == TEXT("asset"))
		{
			// The authored path; the string form of the value would wrap it in @ delimiters.
			Out.SetStringField(TEXT("value"), Value.Get<FSdfAssetPath>().AssetPath);
		}
		else if (TypeName == TEXT("string") || TypeName == TEXT("TfToken") || TypeName == TEXT("token"))
		{
			// Get<FString> reads a string as it is and writes any other value, a token included, in its
			// USD string form, so a token keeps its exact case (an FName would not).
			Out.SetStringField(TEXT("value"), Value.Get<FString>());
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

			// Vectors, quaternions, matrices and arrays: the engine converts the value into one entry of components per
			// value (per element of an array). A non-empty ExtraTypeInfo marks a list op (apiSchemas, references...),
			// whose entries are its six item lists rather than values, so it is left unconverted.
			UsdUtils::FConvertedVtValue Converted;
			if (!UsdToUnreal::ConvertValue(Head.IsEmpty() ? Value : Head, Converted) || Converted.bIsEmpty || !Converted.ExtraTypeInfo.IsEmpty())
			{
				return false;
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
					if (bStrings)
					{
						int32 Cost = 0;
						for (const UsdUtils::FConvertedVtValueComponent& Component : Entry)
						{
							if (const FString* Text = Component.TryGet<FString>())
							{
								Cost += UsdJsonStringCost(*Text);
							}
						}
						if (StringChars + Cost > MaxArrayStringCharsRead)
						{
							break;
						}
						StringChars += Cost;
					}
					Items.Add(UsdEntryToJson(Entry, Converted.SourceType));
				}
				Out.SetArrayField(TEXT("value"), Items);
				Out.SetNumberField(TEXT("length"), Total);
				Out.SetBoolField(TEXT("truncated"), Shown < Total);
			}
			else if (Converted.Entries.Num() == 1)
			{
				Out.SetField(TEXT("value"), UsdEntryToJson(Converted.Entries[0], Converted.SourceType));
			}
			else
			{
				return false;
			}
		}
		return true;
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
