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
 *   - `Attr.Set(VtValue)` with the wrong type is a SILENT FAIL in pxr::: it does
 *     not convert a double into a float attribute. set_usd_attribute_value builds
 *     the value in the attribute's declared type with
 *     `UEAgent::UsdCommon::VtValueForDeclaredType` before anything is written.
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
// MCP:list_usd_prim_attributes - List every attribute of a prim on an open USD stage without changing it. Returns count and attributes [{name, type_name, cpp_type, has_value, has_authored_value, time_sample_count}]; has_value also counts fallback values, has_authored_value only authored opinions.
//   @param stage_name [string!] - Name the stage was registered under by open_usd_stage (default the file basename) or create_usd_stage (default "Untitled").
//   @param prim_path [string!] - Absolute SdfPath of an existing prim on that stage, e.g. "/World/Cube".
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
// MCP:create_usd_attribute - Create an attribute of a given USD type on a prim of an open stage, or return the existing one with created false. Edits the stage in memory; save_usd_stage writes it. Returns name, type_name, cpp_type and created; author its value with set_usd_attribute_value.
//   @param stage_name [string!] - Name the stage was registered under by open_usd_stage (default the file basename) or create_usd_stage (default "Untitled").
//   @param prim_path [string!] - Absolute SdfPath of an existing prim on that stage, e.g. "/World/Cube".
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

	// The type name becomes an FName, and an FName of NAME_SIZE characters or more fails a check().
	if (TypeName.Len() >= NAME_SIZE)
	{
		return mcp_common::MakeErrorReply(FString::Printf(TEXT("type_name is %d characters long, longer than any USD type name"), TypeName.Len()));
	}

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
// MCP:remove_usd_attribute - Remove one attribute from a prim on a registered USD stage by deleting its scene description in the current edit target layer only, so opinions from other layers or references can keep it composed. Idempotent: an absent attribute gives present=false and removed=false, not an error, and a relationship of that name counts as absent. A stage not registered under stage_name or a missing prim is an error. In memory only: save_usd_stage writes it. To keep the attribute and drop its values use clear_usd_attribute.
//   @param stage_name [string!] - Name the stage was registered under by open_usd_stage (default the file basename) or create_usd_stage (default "Untitled").
//   @param prim_path [string!] - Absolute SdfPath of an existing prim on that stage, e.g. "/World/Cube".
//   @param attr_name [string!] - Name of the attribute to remove, e.g. "size"
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
// MCP:get_usd_attribute_value - Read the composed value of one attribute on a prim of a USD stage opened with open_usd_stage or create_usd_stage, at the time code given in time or, when it is omitted, at the default time, which gives the static default value rather than a time sample. The value may come from any layer or be a schema fallback (has_value true, has_authored_value false). value is an object {type, value}, converted only for bool, integer, floating-point, string, token and asset values; other types such as float3 or arrays, and an attribute with no value, give a null value with a note. A missing attribute is a success with present=false; an unregistered stage, an invalid prim path or a missing prim is an error. Read-only. Every attribute: list_usd_prim_attributes; sampled times: get_usd_attribute_time_samples; author a value: set_usd_attribute_value.
//   @param stage_name [string!] - Name the stage was registered under by open_usd_stage (default the file basename) or create_usd_stage (default "Untitled").
//   @param prim_path [string!] - Absolute SdfPath of an existing prim on that stage, e.g. "/World/Cube".
//   @param attr_name [string!] - Attribute name to read, e.g. "size"
//   @param time [number] - Optional USD time code to read at, e.g. 24; when omitted the default time is read (the static default value, not a sample)
//   @returns { prim_path: string, attr_name: string, present: bool, has_value?: bool, has_authored_value?: bool, type?: string, type_name?: string, time?: number, value?: { type: string, value: any, note?: string } | null, note?: string }

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
// MCP:set_usd_attribute_value - Author one value on an attribute of a prim on a registered USD stage, at the current edit target: a time sample at time, or the default value when time is omitted; a missing attribute is created with type_name. The JSON value is converted to the attribute's declared scalar type before anything is written: bool takes a boolean; uchar, int, uint, int64 and uint64 a whole number in range; half, float, double and timecode a number; string, token and asset a string. Another type (float3, arrays), a value that does not fit, a missing attribute without type_name, an unregistered stage or a missing prim is an error and writes nothing. If USD still refuses the value, success is false and an attribute this call created is taken out again. In memory only: save_usd_stage writes it. Read it back with get_usd_attribute_value.
//   @param stage_name [string!] - Name the stage was registered under by open_usd_stage (default the file basename) or create_usd_stage (default "Untitled").
//   @param prim_path [string!] - Absolute SdfPath of an existing prim on that stage, e.g. "/World/Cube".
//   @param attr_name [string!] - Attribute name to set, e.g. "size"
//   @param type_name [string] - USD type name of the attribute to create when it does not exist yet, e.g. "float" or "token"; required in that case, and ignored (with a note when it differs) for an existing attribute, whose declared type decides.
//   @param value [object!] - The value: a JSON scalar, e.g. 2.5, "inherited" or true, or an object {"value": <scalar>} whose "type" key, if any, is not used; it is converted to the declared type of the attribute.
//   @param time [number] - Optional USD time code to author a time sample at, e.g. 24; when omitted the default value is authored
//   @returns { prim_path: string, attr_name: string, set: bool, type_name: string, type: string, created: bool, time?: number, note?: string }

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

	// A raw scalar under "value" is promoted to a {value: ...} object; only its "value" is read.
	TSharedPtr<FJsonObject> ValueObj = UEAgent::UsdCommon::TypedValueObject(Params->TryGetField(TEXT("value")));
	if (!ValueObj.IsValid())
	{
		return mcp_common::MakeErrorReply(
			TEXT("value is required (a scalar, or an object {value})"));
	}

	const TOptional<double> TimeOpt = UsdAttribute_ReadTime(Params);

	UE::FUsdPrim Prim;
	if (TSharedPtr<FJsonObject> Error = UsdAttribute_FindPrim(StageName, PrimPath, Prim))
	{
		return Error;
	}

	// The declared type decides the C++ type of the value: UsdAttribute::Set authors nothing for a value of another
	// type (a double for a float attribute, an int for a uint one). It is converted BEFORE a missing attribute is
	// created, so a value that does not fit leaves the stage as it was.
	UE::FUsdAttribute Attr = Prim.GetAttribute(*AttrName);
	const bool bExisted = static_cast<bool>(Attr);
	if (!bExisted && TypeName.IsEmpty())
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("attribute '%s' does not exist on prim '%s'; provide type_name to create it"),
				*AttrName, *PrimPath));
	}
	const FString DeclaredType = bExisted ? Attr.GetTypeName().ToString() : TypeName;

	UE::FVtValue Vt;
	FString VtErr;
	if (!UEAgent::UsdCommon::VtValueForDeclaredType(DeclaredType, ValueObj->TryGetField(TEXT("value")), Vt, VtErr))
	{
		return mcp_common::MakeErrorReply(FString::Printf(
			TEXT("value cannot be authored on %s.%s: %s. Nothing was written."), *PrimPath, *AttrName, *VtErr));
	}

	bool bCreated = false;
	if (!bExisted)
	{
		Attr = Prim.CreateAttribute(*AttrName, FName(*TypeName));
		if (!Attr)
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("CreateAttribute('%s', '%s') returned an invalid attribute; nothing was written"),
					*AttrName, *TypeName));
		}
		bCreated = true;
	}

	const bool bSet = Attr.Set(Vt, TimeOpt);
	const FString ActualTypeName = Attr.GetTypeName().ToString();

	// Nothing is left half-written: an attribute created for a value USD then refused is taken out again.
	bool bRemovedAgain = false;
	if (!bSet && bCreated)
	{
		bRemovedAgain = Prim.RemoveProperty(FName(*AttrName));
	}

	TSharedPtr<FJsonObject> Data = UsdAttribute_NewPayload(PrimPath, AttrName);
	Data->SetStringField(TEXT("type_name"), ActualTypeName);
	Data->SetStringField(TEXT("type"), ActualTypeName.ToLower());
	Data->SetBoolField(TEXT("set"), bSet);
	Data->SetBoolField(TEXT("created"), bCreated && !bRemovedAgain);
	if (TimeOpt.IsSet())
	{
		Data->SetNumberField(TEXT("time"), TimeOpt.GetValue());
	}
	if (bExisted && !TypeName.IsEmpty() && !TypeName.Equals(ActualTypeName, ESearchCase::IgnoreCase))
	{
		Data->SetStringField(TEXT("note"), FString::Printf(
			TEXT("type_name %s was not used: the attribute already exists as %s"), *TypeName, *ActualTypeName));
	}
	if (!bSet)
	{
		return mcp_common::MakeOutcomeReply(Data, false, FString::Printf(
			TEXT("set_usd_attribute_value: USD did not author the %s value on %s.%s (UsdAttribute::Set returned false; the ")
			TEXT("edit target may not be editable)%s"), *ActualTypeName, *PrimPath, *AttrName,
			bCreated ? (bRemovedAgain ? TEXT("; the attribute this call created was removed again")
			                         : TEXT("; the attribute this call created could not be removed and stays, without a value"))
			         : TEXT("")));
	}
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_usd_attribute_time_samples
// ---------------------------------------------------------------------------
// MCP:get_usd_attribute_time_samples - List the time codes at which a USD attribute on a prim of an open stage has authored time samples, without changing the stage. Returns time_samples, count and present; a missing attribute is not an error but present false with an empty list.
//   @param stage_name [string!] - Name the stage was registered under by open_usd_stage (default the file basename) or create_usd_stage (default "Untitled").
//   @param prim_path [string!] - Absolute SdfPath of an existing prim on that stage, e.g. "/World/Cube".
//   @param attr_name [string!] - Name of the attribute to inspect, e.g. "xformOp:translate".
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
// MCP:clear_usd_attribute - Clear authored values of a USD attribute on a prim of an open stage: with time, only the sample at that time code; without it, the default value and every time sample. Edits the stage in memory; save_usd_stage writes it. Returns present and cleared, present false when the attribute is missing.
//   @param stage_name [string!] - Name the stage was registered under by open_usd_stage (default the file basename) or create_usd_stage (default "Untitled").
//   @param prim_path [string!] - Absolute SdfPath of an existing prim on that stage, e.g. "/World/Cube".
//   @param attr_name [string!] - Name of the attribute to clear, e.g. "size".
//   @param time [number] - Time code of the single sample to clear (ClearAtTime); omit it to clear the default value and all samples (Clear).
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
