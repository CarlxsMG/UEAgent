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
	 * does: bool, number (as double) or string. False, with OutError, for a null object, a missing "value" or
	 * a value no rule converts. Setting a value whose UE type does not match the attribute's USD type fails
	 * silently in pxr::, so the caller checks what Set returns.
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
			Out.Set(FName(*RawVal->AsString()));
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

	/**
	 * Writes Value into Out as "type" (its USD type name) and "value" (bool, number or string) and returns
	 * true; an empty value is "type": "empty" with a null "value". A value of any other type only gets its
	 * "type", and this returns false so the caller says how to show it.
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
		else if (TypeName == TEXT("int") || TypeName == TEXT("int32"))
		{
			Out.SetNumberField(TEXT("value"), static_cast<double>(Value.Get<int32>()));
		}
		else if (TypeName == TEXT("int64"))
		{
			Out.SetNumberField(TEXT("value"), static_cast<double>(Value.Get<int64>()));
		}
		else if (TypeName == TEXT("float") || TypeName == TEXT("double"))
		{
			Out.SetNumberField(TEXT("value"), Value.Get<double>());
		}
		else if (TypeName == TEXT("string") || TypeName == TEXT("asset") || TypeName == TEXT("token"))
		{
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
