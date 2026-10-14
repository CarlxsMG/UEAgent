#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

// UnrealUSDWrapper headless USD API (UE::FUsdStage / FUsdPrim / FUsdAttribute / FVtValue).
// Full type includes — forward decls are insufficient for member function
// signatures returning TArray<UE::FUsdAttribute>.
#include "UnrealUSDWrapper.h"
#include "Commands/USD/UEAgentUsdRegistry.h"  // stage registry + helpers shared by the USD handlers
#include "UsdWrappers/UsdStage.h"
#include "UsdWrappers/UsdPrim.h"
#include "UsdWrappers/UsdAttribute.h"
#include "UsdWrappers/VtValue.h"
#include "UsdWrappers/SdfPath.h"
#include "UsdWrappers/ForwardDeclarations.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonTypes.h"
#include "Templates/SharedPointer.h"

/**
 * USD attribute CRUD + time-sample commands.
 *
 * Reads, creates, removes, gets/sets, lists time samples and clears USD prim
 * attributes via the UnrealUSDWrapper headless API (no UObject, no World,
 * no PIE). All ops run on stages previously opened/created by the USD stage
 * commands, resolved through the C++17 inline `UEAgent::UsdRegistry::GStageNameCache`
 * shared by the USD families (see UEAgentUsdRegistry.h); this family declares no
 * cache of its own.
 *
 * Engine APIs (module UnrealUSDWrapper, Public/UsdWrappers/):
 *   - UE::FUsdPrim::CreateAttribute / GetAttributes / GetAttribute /
 *     HasAttribute / RemoveProperty (UsdPrim.h)
 *   - UE::FUsdAttribute::Get / Set / Clear / ClearAtTime / HasValue /
 *     HasAuthoredValue / GetTimeSamples / GetName / GetTypeName /
 *     GetCPPTypeName (UsdAttribute.h)
 *   - UE::FVtValue (VtValue.h)
 *
 * Gotchas (see the implementation for the why):
 *   - All pxr/USD types used here live in the `UE::` namespace, so call
 *     sites need the `UE::` prefix.
 *   - USD type names are tokens ("float", "float3", "token", ...) — normalize to
 *     lowercase before dispatch (the dispatch whitelist covers the common
 *     types; unsupported types fall back to the type-name string).
 *   - `Attr.Set(VtValue)` with the wrong type is a SILENT FAIL in pxr::.
 *     `UEAgent::UsdCommon::VtValueFromJson` enforces a type whitelist so the LLM
 *     never sends a malformed payload; mismatched types are reported honestly
 *     via `set:false`.
 *   - `HasAuthoredValue` differs from `HasValue` — the latter also returns
 *     true for fallback values. We report both where useful.
 *   - `Time` parameter is optional (`TOptional<double>` default =
 *     `UsdTimeCode::Default()` = unset time).
 */
namespace UEAgentUsdAttributeCommands
{

// MCP-DOMAIN: USD

namespace
{
	// The registered stage StageName names; an entry whose stage has gone invalid is dropped.
	UE::FUsdStage ResolveStageForAttribute(const FString& StageName)
	{
		if (UE::FUsdStage* Found = UEAgent::UsdRegistry::GStageNameCache.Find(StageName))
		{
			if (Found->operator bool())
			{
				return *Found;
			}
			UEAgent::UsdRegistry::GStageNameCache.Remove(StageName);
		}
		return UE::FUsdStage();
	}

	// The required stage_name and prim_path params, the prim path validated: null, or the error reply of the
	// first that is missing or invalid.
	TSharedPtr<FJsonObject> UsdAttribute_ReadStageAndPrimPath(const TSharedPtr<FJsonObject>& Params, FString& OutStageName,
		FString& OutPrimPath)
	{
		if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("stage_name"), OutStageName)) { return Missing; }
		if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("prim_path"), OutPrimPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
		FString PathError;
		if (!UEAgent::UsdCommon::IsValidPrimPath(OutPrimPath, PathError))
		{
			return mcp_common::MakeErrorReply(PathError);
		}
		return nullptr;
	}

	// The prim at PrimPath on the registered stage StageName: null, or the error reply when the stage is not
	// registered (StageHint ends that message) or has no such prim.
	TSharedPtr<FJsonObject> UsdAttribute_FindPrim(const FString& StageName, const FString& PrimPath, UE::FUsdPrim& OutPrim,
		const TCHAR* StageHint = TEXT(""))
	{
		UE::FUsdStage Stage = ResolveStageForAttribute(StageName);
		if (!Stage)
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("USD stage '%s' not found in cache%s"), *StageName, StageHint));
		}
		OutPrim = Stage.GetPrimAtPath(UE::FSdfPath(*PrimPath));
		if (!OutPrim.IsValid())
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("prim '%s' does not exist on stage '%s'"), *PrimPath, *StageName));
		}
		return nullptr;
	}

	// A reply payload naming the attribute: prim_path and attr_name.
	TSharedPtr<FJsonObject> UsdAttribute_NewPayload(const FString& PrimPath, const FString& AttrName)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("prim_path"), PrimPath);
		Data->SetStringField(TEXT("attr_name"), AttrName);
		return Data;
	}

	// The optional time param: unset when absent, which pxr:: reads as the default time (the attribute's
	// static default value, no time sample).
	TOptional<double> UsdAttribute_ReadTime(const TSharedPtr<FJsonObject>& Params)
	{
		TOptional<double> Time;
		double T = 0.0;
		if (Params->TryGetNumberField(TEXT("time"), T))
		{
			Time = T;
		}
		return Time;
	}

	// {"type", "value"} of Value; a type with no JSON scalar form gets a null "value" and a "note" naming it,
	// so the type name stays visible.
	TSharedPtr<FJsonObject> UsdAttribute_VtValueToJson(const UE::FVtValue& Value)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		if (!UEAgent::UsdCommon::VtValueToJson(Value, *Out))
		{
			Out->SetField(TEXT("value"), MakeShared<FJsonValueNull>());
			Out->SetStringField(TEXT("note"),
				FString::Printf(TEXT("type '%s' not in dispatch whitelist (no string repr)"), *Value.GetTypeName()));
		}
		return Out;
	}
}

// ---------------------------------------------------------------------------
// list_usd_prim_attributes
// ---------------------------------------------------------------------------
// MCP:list_usd_prim_attributes - List all USD attributes on a prim
//   @param stage_name [string!] - Stage cache key
//   @param prim_path [string!] - Target prim path
//   @returns { prim_path: string, attributes: [{name, type_name, cpp_type, has_value, has_authored_value, time_sample_count}], count: int }

TSharedPtr<FJsonObject> HandleListUsdPrimAttributes(const TSharedPtr<FJsonObject>& Params)
{
	FString StageName;
	FString PrimPath;
	if (TSharedPtr<FJsonObject> Error = UsdAttribute_ReadStageAndPrimPath(Params, StageName, PrimPath))
	{
		return Error;
	}

	UE::FUsdPrim Prim;
	if (TSharedPtr<FJsonObject> Error = UsdAttribute_FindPrim(StageName, PrimPath, Prim, TEXT(" (open or create it first)")))
	{
		return Error;
	}

	const TArray<UE::FUsdAttribute> Attrs = Prim.GetAttributes();

	TArray<TSharedPtr<FJsonValue>> AttrArr;
	AttrArr.Reserve(Attrs.Num());
	for (const UE::FUsdAttribute& A : Attrs)
	{
		if (!A) { continue; }

		TArray<double> SampleTimes;
		A.GetTimeSamples(SampleTimes);

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), A.GetName().ToString());
		Entry->SetStringField(TEXT("type_name"), A.GetTypeName().ToString());
		Entry->SetStringField(TEXT("cpp_type"), A.GetCPPTypeName());
		Entry->SetBoolField(TEXT("has_value"), A.HasValue());
		Entry->SetBoolField(TEXT("has_authored_value"), A.HasAuthoredValue());
		Entry->SetNumberField(TEXT("time_sample_count"), SampleTimes.Num());
		AttrArr.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("prim_path"), PrimPath);
	Data->SetArrayField(TEXT("attributes"), AttrArr);
	Data->SetNumberField(TEXT("count"), AttrArr.Num());
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// create_usd_attribute
// ---------------------------------------------------------------------------
// MCP:create_usd_attribute - Create (or retrieve) a USD attribute on a prim
//   @param stage_name [string!] - Stage cache key
//   @param prim_path [string!] - Target prim path
//   @param attr_name [string!] - Attribute name (e.g. "size", "displayColor")
//   @param type_name [string!] - USD type name (e.g. "float", "float3", "token", "asset")
//   @returns { prim_path: string, name: string, type_name: string, cpp_type: string, created: bool }

TSharedPtr<FJsonObject> HandleCreateUsdAttribute(const TSharedPtr<FJsonObject>& Params)
{
	FString StageName;
	FString PrimPath;
	if (TSharedPtr<FJsonObject> Error = UsdAttribute_ReadStageAndPrimPath(Params, StageName, PrimPath))
	{
		return Error;
	}

	FString AttrName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("attr_name"), AttrName)) { return Missing; }

	FString TypeName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("type_name"), TypeName)) { return Missing; }

	UE::FUsdPrim Prim;
	if (TSharedPtr<FJsonObject> Error = UsdAttribute_FindPrim(StageName, PrimPath, Prim))
	{
		return Error;
	}

	// CreateAttribute is idempotent in pxr:: — if the attribute already exists,
	// it returns the existing one (no error, no duplicate). We surface
	// `created:true` only when the attribute did not exist before.
	const bool bAlreadyExisted = Prim.HasAttribute(*AttrName);
	UE::FUsdAttribute Attr = Prim.CreateAttribute(*AttrName, FName(*TypeName));
	if (!Attr)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("CreateAttribute('%s', '%s') returned an invalid attribute"), *AttrName, *TypeName));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("prim_path"), PrimPath);
	Data->SetStringField(TEXT("name"), Attr.GetName().ToString());
	Data->SetStringField(TEXT("type_name"), Attr.GetTypeName().ToString());
	Data->SetStringField(TEXT("cpp_type"), Attr.GetCPPTypeName());
	Data->SetBoolField(TEXT("created"), !bAlreadyExisted);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// remove_usd_attribute
// ---------------------------------------------------------------------------
// MCP:remove_usd_attribute - Remove a USD attribute (or any property) from a prim
//   @param stage_name [string!] - Stage cache key
//   @param prim_path [string!] - Target prim path
//   @param attr_name [string!] - Attribute name to remove
//   @returns { prim_path: string, attr_name: string, removed: bool, present: bool }

TSharedPtr<FJsonObject> HandleRemoveUsdAttribute(const TSharedPtr<FJsonObject>& Params)
{
	FString StageName;
	FString PrimPath;
	if (TSharedPtr<FJsonObject> Error = UsdAttribute_ReadStageAndPrimPath(Params, StageName, PrimPath))
	{
		return Error;
	}

	FString AttrName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("attr_name"), AttrName)) { return Missing; }

	UE::FUsdPrim Prim;
	if (TSharedPtr<FJsonObject> Error = UsdAttribute_FindPrim(StageName, PrimPath, Prim))
	{
		return Error;
	}

	// Idempotent: if the property does not exist, surface removed:false with
	// present:false rather than an error — pxr::RemoveProperty returns false
	// for "nothing to remove" which is a no-op from the caller's POV.
	const bool bWasPresent = Prim.HasAttribute(*AttrName);
	const bool Removed = bWasPresent ? Prim.RemoveProperty(FName(*AttrName)) : false;

	TSharedPtr<FJsonObject> Data = UsdAttribute_NewPayload(PrimPath, AttrName);
	Data->SetBoolField(TEXT("removed"), Removed);
	Data->SetBoolField(TEXT("present"), bWasPresent);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_usd_attribute_value
// ---------------------------------------------------------------------------
// MCP:get_usd_attribute_value - Read a USD attribute's value at a time code
//   @param stage_name [string!] - Stage cache key
//   @param prim_path [string!] - Target prim path
//   @param attr_name [string!] - Attribute name to read
//   @param time [number] - Optional time code (default: unset = static default)
//   @returns { present: bool, has_value: bool, has_authored_value: bool, type: string, type_name: string, value: object }

TSharedPtr<FJsonObject> HandleGetUsdAttributeValue(const TSharedPtr<FJsonObject>& Params)
{
	FString StageName;
	FString PrimPath;
	if (TSharedPtr<FJsonObject> Error = UsdAttribute_ReadStageAndPrimPath(Params, StageName, PrimPath))
	{
		return Error;
	}

	FString AttrName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("attr_name"), AttrName)) { return Missing; }

	const TOptional<double> TimeOpt = UsdAttribute_ReadTime(Params);

	UE::FUsdPrim Prim;
	if (TSharedPtr<FJsonObject> Error = UsdAttribute_FindPrim(StageName, PrimPath, Prim))
	{
		return Error;
	}

	UE::FUsdAttribute Attr = Prim.GetAttribute(*AttrName);
	if (!Attr)
	{
		TSharedPtr<FJsonObject> Data = UsdAttribute_NewPayload(PrimPath, AttrName);
		Data->SetBoolField(TEXT("present"), false);
		return mcp_common::MakeOkReply(Data);
	}

	const bool bHasValue = Attr.HasValue();
	const bool bHasAuthored = Attr.HasAuthoredValue();
	const FString TypeName = Attr.GetTypeName().ToString();
	const FString TypeLow = TypeName.ToLower();

	TSharedPtr<FJsonObject> Data = UsdAttribute_NewPayload(PrimPath, AttrName);
	Data->SetStringField(TEXT("type"), TypeLow);
	Data->SetStringField(TEXT("type_name"), TypeName);
	Data->SetBoolField(TEXT("present"), true);
	Data->SetBoolField(TEXT("has_value"), bHasValue);
	Data->SetBoolField(TEXT("has_authored_value"), bHasAuthored);

	if (TimeOpt.IsSet())
	{
		Data->SetNumberField(TEXT("time"), TimeOpt.GetValue());
	}

	if (!bHasValue)
	{
		// The attribute exists but has no value (not even a fallback). Surface
		// this honestly so the LLM doesn't think Get failed silently.
		Data->SetField(TEXT("value"), MakeShared<FJsonValueNull>());
		Data->SetStringField(TEXT("note"), TEXT("attribute has no value (no default and no authored opinion)"));
		return mcp_common::MakeOkReply(Data);
	}

	// Read into a FVtValue — this is the typed-read escape hatch that the
	// typed templates (Get<T>) also use internally. We always use the FVtValue
	// path so we can surface the USD type name even when the value is a type
	// we don't have a typed-read for.
	UE::FVtValue Vt;
	const bool bGot = Attr.Get(Vt, TimeOpt);

	if (!bGot)
	{
		Data->SetField(TEXT("value"), MakeShared<FJsonValueNull>());
		Data->SetStringField(TEXT("note"), TEXT("Attr.Get returned false (no value at the requested time)"));
		return mcp_common::MakeOkReply(Data);
	}

	Data->SetObjectField(TEXT("value"), UsdAttribute_VtValueToJson(Vt));
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_usd_attribute_value
// ---------------------------------------------------------------------------
// MCP:set_usd_attribute_value - Author a value on a USD attribute (creating it if missing)
//   @param stage_name [string!] - Stage cache key
//   @param prim_path [string!] - Target prim path
//   @param attr_name [string!] - Attribute name to set
//   @param type_name [string] - USD type name; REQUIRED when the attribute does not exist yet (used to CreateAttribute). When the attribute already exists this is ignored.
//   @param value [object!] - {type: "float"|"int"|"string"|"token"|"asset"|..., value: <scalar>}; explicit type preferred; implicit by JSON runtime type otherwise.
//   @param time [number] - Optional time code (default: unset = static default)
//   @returns { prim_path: string, attr_name: string, set: bool, type_name: string, type: string, created: bool, time: number? }

TSharedPtr<FJsonObject> HandleSetUsdAttributeValue(const TSharedPtr<FJsonObject>& Params)
{
	FString StageName;
	FString PrimPath;
	if (TSharedPtr<FJsonObject> Error = UsdAttribute_ReadStageAndPrimPath(Params, StageName, PrimPath))
	{
		return Error;
	}

	FString AttrName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("attr_name"), AttrName)) { return Missing; }

	FString TypeName;
	Params->TryGetStringField(TEXT("type_name"), TypeName);

	// A raw scalar under "value" is promoted to a {value: ...} object, so the same
	// VtValueFromJson dispatch applies (as in set_usd_prim_metadata).
	TSharedPtr<FJsonObject> ValueObj = UEAgent::UsdCommon::TypedValueObject(Params->TryGetField(TEXT("value")));
	if (!ValueObj.IsValid())
	{
		return mcp_common::MakeErrorReply(
			TEXT("value is required (object {type, value} or scalar)"));
	}

	// If the value object did not specify an explicit "type", inherit from
	// the caller's "type_name" param (which is normally used for the
	// CreateAttribute path) so the same JSON shape works for both create +
	// set in one call.
	if (!ValueObj->HasField(TEXT("type")) && !TypeName.IsEmpty())
	{
		ValueObj->SetStringField(TEXT("type"), TypeName);
	}

	const TOptional<double> TimeOpt = UsdAttribute_ReadTime(Params);

	UE::FUsdPrim Prim;
	if (TSharedPtr<FJsonObject> Error = UsdAttribute_FindPrim(StageName, PrimPath, Prim))
	{
		return Error;
	}

	// Create-if-missing. The type_name required-when-missing check uses the
	// caller's explicit type_name param, or the explicit "type" on the value
	// object, or the value's JSON runtime type via the implicit dispatch in
	// VtValueFromJson.
	bool bCreated = false;
	UE::FUsdAttribute Attr = Prim.GetAttribute(*AttrName);
	if (!Attr)
	{
		if (TypeName.IsEmpty())
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("attribute '%s' does not exist on prim '%s'; provide type_name to create it"),
					*AttrName, *PrimPath));
		}
		Attr = Prim.CreateAttribute(*AttrName, FName(*TypeName));
		bCreated = true;
		if (!Attr)
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("CreateAttribute('%s', '%s') returned an invalid attribute"),
					*AttrName, *TypeName));
		}
	}

	UE::FVtValue Vt;
	FString VtErr;
	if (!UEAgent::UsdCommon::VtValueFromJson(ValueObj, Vt, VtErr))
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("value is not convertible to a supported USD type: %s"), *VtErr));
	}

	// Attr.Set returns false silently on type mismatch in pxr::. We surface
	// set:false honestly rather than throwing.
	const bool Set = Attr.Set(Vt, TimeOpt);

	const FString ActualTypeName = Attr.GetTypeName().ToString();

	TSharedPtr<FJsonObject> Data = UsdAttribute_NewPayload(PrimPath, AttrName);
	Data->SetStringField(TEXT("type_name"), ActualTypeName);
	Data->SetStringField(TEXT("type"), ActualTypeName.ToLower());
	Data->SetBoolField(TEXT("set"), Set);
	Data->SetBoolField(TEXT("created"), bCreated);
	if (TimeOpt.IsSet())
	{
		Data->SetNumberField(TEXT("time"), TimeOpt.GetValue());
	}
	if (!Set)
	{
		Data->SetStringField(TEXT("note"),
			TEXT("Attr.Set returned false (likely type mismatch -- the JSON value's UE type does not match the USD attribute's declared type)"));
	}
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_usd_attribute_time_samples
// ---------------------------------------------------------------------------
// MCP:get_usd_attribute_time_samples - List the time codes at which a USD attribute has authored samples
//   @param stage_name [string!] - Stage cache key
//   @param prim_path [string!] - Target prim path
//   @param attr_name [string!] - Attribute name
//   @returns { prim_path: string, attr_name: string, time_samples: [number], count: int }

TSharedPtr<FJsonObject> HandleGetUsdAttributeTimeSamples(const TSharedPtr<FJsonObject>& Params)
{
	FString StageName;
	FString PrimPath;
	if (TSharedPtr<FJsonObject> Error = UsdAttribute_ReadStageAndPrimPath(Params, StageName, PrimPath))
	{
		return Error;
	}

	FString AttrName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("attr_name"), AttrName)) { return Missing; }

	UE::FUsdPrim Prim;
	if (TSharedPtr<FJsonObject> Error = UsdAttribute_FindPrim(StageName, PrimPath, Prim))
	{
		return Error;
	}

	UE::FUsdAttribute Attr = Prim.GetAttribute(*AttrName);
	if (!Attr)
	{
		TSharedPtr<FJsonObject> Data = UsdAttribute_NewPayload(PrimPath, AttrName);
		Data->SetBoolField(TEXT("present"), false);
		Data->SetArrayField(TEXT("time_samples"), TArray<TSharedPtr<FJsonValue>>());
		Data->SetNumberField(TEXT("count"), 0);
		return mcp_common::MakeOkReply(Data);
	}

	TArray<double> Times;
	Attr.GetTimeSamples(Times);

	TArray<TSharedPtr<FJsonValue>> TimesArr;
	TimesArr.Reserve(Times.Num());
	for (const double T : Times)
	{
		TimesArr.Add(MakeShared<FJsonValueNumber>(T));
	}

	TSharedPtr<FJsonObject> Data = UsdAttribute_NewPayload(PrimPath, AttrName);
	Data->SetBoolField(TEXT("present"), true);
	Data->SetArrayField(TEXT("time_samples"), TimesArr);
	Data->SetNumberField(TEXT("count"), TimesArr.Num());
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// clear_usd_attribute
// ---------------------------------------------------------------------------
// MCP:clear_usd_attribute - Clear a USD attribute's authored value(s)
//   @param stage_name [string!] - Stage cache key
//   @param prim_path [string!] - Target prim path
//   @param attr_name [string!] - Attribute name
//   @param time [number] - Optional time code; when omitted clears ALL times (Attr.Clear), when present clears just that time (Attr.ClearAtTime)
//   @returns { prim_path: string, attr_name: string, cleared: bool, present: bool, time: number? }

TSharedPtr<FJsonObject> HandleClearUsdAttribute(const TSharedPtr<FJsonObject>& Params)
{
	FString StageName;
	FString PrimPath;
	if (TSharedPtr<FJsonObject> Error = UsdAttribute_ReadStageAndPrimPath(Params, StageName, PrimPath))
	{
		return Error;
	}

	FString AttrName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("attr_name"), AttrName)) { return Missing; }

	const TOptional<double> TimeOpt = UsdAttribute_ReadTime(Params);

	UE::FUsdPrim Prim;
	if (TSharedPtr<FJsonObject> Error = UsdAttribute_FindPrim(StageName, PrimPath, Prim))
	{
		return Error;
	}

	UE::FUsdAttribute Attr = Prim.GetAttribute(*AttrName);
	if (!Attr)
	{
		TSharedPtr<FJsonObject> Data = UsdAttribute_NewPayload(PrimPath, AttrName);
		Data->SetBoolField(TEXT("present"), false);
		Data->SetBoolField(TEXT("cleared"), false);
		return mcp_common::MakeOkReply(Data);
	}

	bool Cleared = false;
	if (TimeOpt.IsSet())
	{
		Cleared = Attr.ClearAtTime(TimeOpt.GetValue());
	}
	else
	{
		Cleared = Attr.Clear();
	}

	TSharedPtr<FJsonObject> Data = UsdAttribute_NewPayload(PrimPath, AttrName);
	Data->SetBoolField(TEXT("present"), true);
	Data->SetBoolField(TEXT("cleared"), Cleared);
	if (TimeOpt.IsSet())
	{
		Data->SetNumberField(TEXT("time"), TimeOpt.GetValue());
	}
	else
	{
		Data->SetStringField(TEXT("note"),
			TEXT("no 'time' specified -- cleared ALL authored values (Attr.Clear)"));
	}
	return mcp_common::MakeOkReply(Data);
}

const McpCommandFamilies::FRegistrar Registration =
{
	{ TEXT("clear_usd_attribute"), &HandleClearUsdAttribute },
	{ TEXT("create_usd_attribute"), &HandleCreateUsdAttribute },
	{ TEXT("get_usd_attribute_time_samples"), &HandleGetUsdAttributeTimeSamples },
	{ TEXT("get_usd_attribute_value"), &HandleGetUsdAttributeValue },
	{ TEXT("list_usd_prim_attributes"), &HandleListUsdPrimAttributes },
	{ TEXT("remove_usd_attribute"), &HandleRemoveUsdAttribute },
	{ TEXT("set_usd_attribute_value"), &HandleSetUsdAttributeValue },
};
} // namespace UEAgentUsdAttributeCommands
