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
#include "Templates/Function.h"
#include "UnrealUSDWrapper.h"
#include "UsdWrappers/SdfLayer.h"
#include "UsdWrappers/SdfPath.h"
#include "UsdWrappers/UsdStage.h"
#include "UsdWrappers/VtValue.h"

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
	 * The FVtValue holding exactly the C++ type of the scalar USD value type DeclaredType (an attribute's
	 * SdfValueTypeName, compared ignoring case) that the JSON value RawVal gives: UsdAttribute::Set authors
	 * nothing when the held type is not the attribute's own, so a double for a float attribute or an int for a
	 * uint one fails. bool takes a JSON boolean; uchar, int, uint, int64 and uint64 a whole number in their
	 * range; half, float, double and timecode a number (half and float within their range); string, token and
	 * asset a string. False, with OutError, for any other declared type or a value that does not fit.
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

		OutError = FString::Printf(
			TEXT("an attribute of type %s is not one this tool can author: the declared type must be bool, uchar, int, uint, int64, ")
			TEXT("uint64, half, float, double, timecode, string, token or asset"), *DeclaredType);
		return false;
	}

	/**
	 * Writes Value into Out as "type" (the type name USD gives the held value) and "value" (bool, number or
	 * string) and returns true; an empty value is "type": "empty" with a null "value". A value of any other
	 * type only gets its "type", and this returns false so the caller says how to show it.
	 *
	 * Each type is read with its own C++ type: FVtValue::Get<T> of a T the value does not hold raises a USD
	 * coding error and returns a default T, so a float read as a double comes back 0. The type name is the
	 * demangled C++ type of the held value ("float", "pxr_half::half", "__int64", "TfToken"), not the Sdf
	 * value type name; both spellings are accepted, as the engine's own USD metadata translation does.
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
			return false;
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
