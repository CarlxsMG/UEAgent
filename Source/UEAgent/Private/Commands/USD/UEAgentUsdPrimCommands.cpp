#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

#include "UsdWrappers/ForwardDeclarations.h"
#include "UsdWrappers/UsdPrim.h"
#include "UsdWrappers/UsdStage.h"
#include "UsdWrappers/SdfPath.h"
#include "UsdWrappers/SdfLayer.h"  // UE::FSdfLayer full type (forward-declared only in ForwardDeclarations.h)
#include "UsdWrappers/VtValue.h"
#include "UsdWrappers/UsdVariantSets.h"
#include "UnrealUSDWrapper.h"
#include "Commands/USD/UEAgentUsdRegistry.h"  // stage registry + helpers shared by the USD handlers

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonTypes.h"
#include "Templates/SharedPointer.h"
#include "Algo/Reverse.h"

/**
 * USD prim CRUD + metadata + schema commands.
 *
 * All ops run on USD stages previously opened/created by the USD stage commands
 * and stored in the shared stage-name cache under a stable
 * stage_name. Headless 100% — no UObject, no World, no PIE.
 *
 * Engine APIs (module UnrealUSDWrapper, Public/UsdWrappers/):
 *   - UE::FUsdStage::DefinePrim(Path, TypeName) / OverridePrim(Path) /
 *     RemovePrim(Path) / GetPrimAtPath(Path) (UsdStage.h)
 *   - UE::FUsdPrim::SetSpecifier / IsActive / SetActive / IsValid / GetName /
 *     GetPrimPath / GetTypeName / SetTypeName / GetParent / GetChildren /
 *     GetFilteredChildren / GetVariantSets / IsA / HasAPI / CanApplyAPI /
 *     ApplyAPI / RemoveAPI / SetInstanceable / GetMetadata / SetMetadata /
 *     GetAllMetadata / SetDisplayName (UsdPrim.h)
 *   - UE::FSdfPath (SdfPath.h)
 *   - UE::FVtValue (Set<T>() for typed storage — used by Set/GetMetadata; VtValue.h)
 *   - UE::ESdfSpecifier { Def, Over, Class, Num } (UsdPrim.h)
 */

// LLM-facing stage index — see UEAgentUsdRegistry.h.
// Both this family and UEAgentUsdStageCommands consult the SAME
// `UEAgent::UsdRegistry::GStageNameCache` (C++17 inline variable).
namespace UEAgentUsdPrimCommands
{

// MCP-DOMAIN: USD

namespace
{
	// Parse a "Def" | "Over" | "Class" string (case-insensitive) into UE::ESdfSpecifier: false,
	// with Out untouched, for anything else.
	bool TryParseSpecifier(const FString& In, UE::ESdfSpecifier& Out)
	{
		if (In.Equals(TEXT("Def"), ESearchCase::IgnoreCase)) { Out = UE::ESdfSpecifier::Def; return true; }
		if (In.Equals(TEXT("Over"), ESearchCase::IgnoreCase)) { Out = UE::ESdfSpecifier::Over; return true; }
		if (In.Equals(TEXT("Class"), ESearchCase::IgnoreCase)) { Out = UE::ESdfSpecifier::Class; return true; }
		return false;
	}

	// The canonical USD spelling of a specifier, as the replies report it.
	const TCHAR* SpecifierName(UE::ESdfSpecifier Spec)
	{
		switch (Spec)
		{
		case UE::ESdfSpecifier::Over: return TEXT("Over");
		case UE::ESdfSpecifier::Class: return TEXT("Class");
		default: return TEXT("Def");
		}
	}

	// Cap on how many metadata entries we surface in one get_usd_prim_metadata
	// (no key) call. Beyond this, we return a "prim has N entries, request a
	// specific key" note instead of enumerating everything.
	constexpr int32 MaxMetadataEntriesReported = 100;

	// The stage the stage_name param names (UEAgent::UsdRegistry::FindNamedStage): null, or the error reply.
	TSharedPtr<FJsonObject> UsdPrim_ResolveStage(const TSharedPtr<FJsonObject>& Params, UE::FUsdStage& OutStage,
		FString& OutStageName)
	{
		OutStage = UE::FUsdStage();
		OutStageName.Reset();

		if (!Params.IsValid())
		{
			return mcp_common::MakeErrorReply(TEXT("params object is null"));
		}

		FString StageName;
		Params->TryGetStringField(TEXT("stage_name"), StageName);
		const FString LookupError = UEAgent::UsdRegistry::FindNamedStage(StageName, TEXT("staged"), OutStage);
		if (!LookupError.IsEmpty())
		{
			return mcp_common::MakeErrorReply(LookupError);
		}
		OutStageName = StageName;
		return nullptr;
	}

	// UsdPrim_ResolveStage, then the required prim_path, validated: null, or the error reply of the first that fails.
	TSharedPtr<FJsonObject> UsdPrim_ResolveStageAndPrimPath(const TSharedPtr<FJsonObject>& Params, UE::FUsdStage& OutStage,
		FString& OutStageName, FString& OutPrimPath)
	{
		if (TSharedPtr<FJsonObject> Error = UsdPrim_ResolveStage(Params, OutStage, OutStageName))
		{
			return Error;
		}
		if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("prim_path"), OutPrimPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
		FString PathError;
		if (!UEAgent::UsdCommon::IsValidPrimPath(OutPrimPath, PathError))
		{
			return mcp_common::MakeErrorReply(PathError);
		}
		return nullptr;
	}

	// The prim at PrimPath on Stage: null, or the error reply naming StageName when there is none (Hint ends it).
	TSharedPtr<FJsonObject> UsdPrim_FindPrim(const UE::FUsdStage& Stage, const FString& StageName, const FString& PrimPath,
		UE::FUsdPrim& OutPrim, const TCHAR* Hint = TEXT(""))
	{
		OutPrim = Stage.GetPrimAtPath(UE::FSdfPath(*PrimPath));
		if (!OutPrim.IsValid())
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("prim '%s' does not exist on stage '%s'%s"), *PrimPath, *StageName, Hint));
		}
		return nullptr;
	}

	/**
	 * The type of the metadata field Key on Prim, spelled as VtValueForMetadataType takes it, read from the value the
	 * field resolves to (the authored one, or the fallback a registered field has): a scalar, vector, matrix or array
	 * as its Sdf value type ("token", "double3", "string[]"), a dictionary as "dictionary", a list op as its held type
	 * ("SdfListOp<TfToken>"). A field that resolves to nothing (no prim definition gives kind, comment, apiSchemas...
	 * a fallback), or to a type the converters do not author, takes the type USD registers for it when it is a standard
	 * prim field (UsdWellKnownMetadataType); else empty.
	 */
	FString UsdPrim_MetadataDeclaredType(const UE::FUsdPrim& Prim, const FString& Key)
	{
		UE::FVtValue Current;
		if (Prim.GetMetadata(*Key, Current))
		{
			const FString Declared = UEAgent::UsdCommon::UsdDeclaredTypeOf(Current);
			if (!Declared.IsEmpty())
			{
				return Declared;
			}
		}
		return UEAgent::UsdCommon::UsdWellKnownMetadataType(Key);
	}

	// {"type", "value"} of Value (UEAgent::UsdCommon::VtValueToJson); a type with no JSON form gets a null "value" and a
	// "note" naming it.
	TSharedPtr<FJsonObject> UsdPrim_VtValueToJson(const UE::FVtValue& Value)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		UEAgent::UsdCommon::VtValueToJson(Value, *Out);
		return Out;
	}
}

// MCP:define_usd_prim - Define a prim (specifier def) of type_name at prim_path on a registered USD stage, at its current edit target, defining missing ancestors as typeless prims. On an existing prim of another type it replaces the type, so leaving type_name out retypes it to Xform. The requested specifier is then authored at the edit target (specifier_set_ok); specifier reports it only when that write succeeded, and a failed write fails the call. An unknown specifier is refused before anything is authored. In memory only: save_usd_stage writes it. override_usd_prim authors an over instead; set_usd_prim_metadata with key "typeName" changes only the type.
//   @param stage_name [string!] - Name the stage was registered under
//   @param prim_path [string!] - Absolute SdfPath, e.g. /World/Cube
//   @param type_name [string] - USD type name (Xform, Mesh, Cube, Scope, Camera, DistantLight...). Default: Xform
//   @param specifier [string] - "Def" | "Over" | "Class", case-insensitive; anything else is refused. Default: Def
//   @returns { prim_path: string, type_name: string, specifier?: string, is_valid: bool, specifier_set_ok: bool, requested?: object }
TSharedPtr<FJsonObject> HandleDefineUsdPrim(const TSharedPtr<FJsonObject>& Params)
{
	UE::FUsdStage Stage;
	FString StageName;
	FString PrimPath;
	if (TSharedPtr<FJsonObject> Error = UsdPrim_ResolveStageAndPrimPath(Params, Stage, StageName, PrimPath))
	{
		return Error;
	}

	FString TypeName = TEXT("Xform");
	Params->TryGetStringField(TEXT("type_name"), TypeName);

	FString SpecStr = TEXT("Def");
	Params->TryGetStringField(TEXT("specifier"), SpecStr);
	if (SpecStr.IsEmpty())
	{
		SpecStr = TEXT("Def");
	}
	UE::ESdfSpecifier Spec = UE::ESdfSpecifier::Def;
	if (!TryParseSpecifier(SpecStr, Spec))
	{
		return mcp_common::MakeErrorReply(FString::Printf(
			TEXT("Unknown specifier '%s': expected Def, Over or Class. Nothing was authored."), *SpecStr));
	}

	const UE::FSdfPath Path(*PrimPath);
	UE::FUsdPrim Prim = Stage.DefinePrim(Path, *TypeName);
	if (!Prim.IsValid())
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("DefinePrim('%s','%s') returned an invalid prim on stage '%s'"),
				*PrimPath, *TypeName, *StageName));
	}

	// DefinePrim leaves an already-defined prim of the same type as it is, and a class prim is
	// "defined" too, so Def is authored explicitly like Over and Class: the specifier reported is
	// then the one this call wrote at the edit target. The wrapper has no specifier getter, so the
	// write's own result is the only evidence available.
	const bool SpecSetOk = Prim.SetSpecifier(Spec);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("prim_path"), PrimPath);
	Data->SetStringField(TEXT("type_name"), Prim.GetTypeName().ToString());
	if (SpecSetOk)
	{
		Data->SetStringField(TEXT("specifier"), SpecifierName(Spec));
	}
	Data->SetBoolField(TEXT("is_valid"), Prim.IsValid());
	Data->SetBoolField(TEXT("specifier_set_ok"), SpecSetOk);
	mcp_common::AddRequestedEcho(Data, Params, { TEXT("specifier") });
	return mcp_common::MakeOutcomeReply(Data, SpecSetOk, mcp_common::MeasuredOutcomeError(TEXT("define_usd_prim"), TEXT("specifier_set_ok")));
}

// MCP:override_usd_prim - Author an over (specifier over, no type) at prim_path on a registered USD stage, at its current edit target, with overs for missing ancestors, to add opinions to a prim without defining it. A prim that already exists there is returned untouched (already_existed true, no specifier reported, since nothing was authored); specifier Over comes back only for an over this call authored. is_valid=false means USD refused; a missing stage or a bad path is an error. In memory only: save_usd_stage writes it. define_usd_prim creates a typed def; set_usd_prim_specifier changes the specifier of an existing prim.
//   @param stage_name [string!] - Name the stage was registered under by create_usd_stage or open_usd_stage; a stage already in the USD cache also matches by the end of its root layer identifier.
//   @param prim_path [string!] - Absolute SdfPath to author the over at, e.g. "/World/Cube"
//   @returns { prim_path: string, is_valid: bool, already_existed: bool, specifier?: string }
//   @hints additive idempotent
TSharedPtr<FJsonObject> HandleOverrideUsdPrim(const TSharedPtr<FJsonObject>& Params)
{
	UE::FUsdStage Stage;
	FString StageName;
	FString PrimPath;
	if (TSharedPtr<FJsonObject> Error = UsdPrim_ResolveStageAndPrimPath(Params, Stage, StageName, PrimPath))
	{
		return Error;
	}

	const UE::FSdfPath Path(*PrimPath);
	// UsdStage::OverridePrim returns a prim that already exists at the path as it is, whatever its
	// specifier, and authors an over only when there is none, so only that case is known to be Over.
	const bool bAlreadyExisted = Stage.GetPrimAtPath(Path).IsValid();
	UE::FUsdPrim Prim = Stage.OverridePrim(Path);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("prim_path"), PrimPath);
	Data->SetBoolField(TEXT("is_valid"), Prim.IsValid());
	Data->SetBoolField(TEXT("already_existed"), bAlreadyExisted);
	if (Prim.IsValid() && !bAlreadyExisted)
	{
		Data->SetStringField(TEXT("specifier"), TEXT("Over"));
	}
	return mcp_common::MakeOkReply(Data);
}

// MCP:remove_usd_prim - Remove a prim and its subtree from the current edit target layer of a registered USD stage, returning removed. Opinions in other layers or references can keep it composed; set_usd_prim_metadata with key "active" and value false is the reversible way to hide a prim. Not saved to disk.
//   @param stage_name [string!] - Name the stage was registered under by create_usd_stage or open_usd_stage; a stage already in the USD cache also matches by the end of its root layer identifier.
//   @param prim_path [string!] - Absolute SdfPath of the prim to remove, e.g. "/World/Cube"; its descendants go with it.
//   @returns { removed: bool, prim_path: string }
TSharedPtr<FJsonObject> HandleRemoveUsdPrim(const TSharedPtr<FJsonObject>& Params)
{
	UE::FUsdStage Stage;
	FString StageName;
	FString PrimPath;
	if (TSharedPtr<FJsonObject> Error = UsdPrim_ResolveStageAndPrimPath(Params, Stage, StageName, PrimPath))
	{
		return Error;
	}

	const UE::FSdfPath Path(*PrimPath);
	const bool Removed = Stage.RemovePrim(Path);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("prim_path"), PrimPath);
	Data->SetBoolField(TEXT("removed"), Removed);
	return mcp_common::MakeOkReply(Data);
}

// MCP:set_usd_prim_specifier - Change the specifier ("Def", "Over" or "Class") of an existing prim on a registered USD stage, at the current edit target. An unknown specifier is refused before anything is authored. specifier reports the one written, only when the write succeeded (set); a failed write fails the call. Not saved to disk.
//   @param stage_name [string!] - Name the stage was registered under by create_usd_stage or open_usd_stage; a stage already in the USD cache also matches by the end of its root layer identifier.
//   @param prim_path [string!] - Absolute SdfPath of an existing prim, e.g. "/World/Cube".
//   @param specifier [string!] - "Def", "Over" or "Class", case-insensitive; anything else is refused.
//   @returns { set: bool, specifier?: string, requested?: object }
TSharedPtr<FJsonObject> HandleSetUsdPrimSpecifier(const TSharedPtr<FJsonObject>& Params)
{
	UE::FUsdStage Stage;
	FString StageName;
	FString PrimPath;
	if (TSharedPtr<FJsonObject> Error = UsdPrim_ResolveStageAndPrimPath(Params, Stage, StageName, PrimPath))
	{
		return Error;
	}

	FString SpecStr;
	if (!Params->TryGetStringField(TEXT("specifier"), SpecStr) || SpecStr.IsEmpty())
	{
		return mcp_common::MakeErrorReply(
			TEXT("specifier is required (Def|Over|Class)"));
	}
	UE::ESdfSpecifier Spec = UE::ESdfSpecifier::Def;
	if (!TryParseSpecifier(SpecStr, Spec))
	{
		return mcp_common::MakeErrorReply(FString::Printf(
			TEXT("Unknown specifier '%s': expected Def, Over or Class. Nothing was authored."), *SpecStr));
	}

	UE::FUsdPrim Prim;
	if (TSharedPtr<FJsonObject> Error = UsdPrim_FindPrim(Stage, StageName, PrimPath, Prim))
	{
		return Error;
	}

	// The wrapper has no specifier getter, so the write's own result is the only evidence of the
	// specifier now authored at the edit target.
	const bool SetOk = Prim.SetSpecifier(Spec);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	if (SetOk)
	{
		Data->SetStringField(TEXT("specifier"), SpecifierName(Spec));
	}
	Data->SetBoolField(TEXT("set"), SetOk);
	mcp_common::AddRequestedEcho(Data, Params, { TEXT("specifier") });
	return mcp_common::MakeOutcomeReply(Data, SetOk, mcp_common::MeasuredOutcomeError(TEXT("set_usd_prim_specifier"), TEXT("set")));
}

// MCP:set_usd_prim_metadata - Author one metadata field (e.g. "comment", "kind", "customData", "apiSchemas", or the well-known "active" (bool; false deactivates the prim and its subtree, the reversible alternative to remove_usd_prim), "instanceable" (bool, USD native instancing), "displayName" (string UI label; the prim name does not change) and "typeName" (token, written as given, not checked against the schema registry)) on an existing prim of a registered USD stage, at the current edit target. The value is converted to the field's type, read from its resolved value or, for a standard field resolving to nothing (kind, comment, hidden, customData, apiSchemas, references...), the type Sdf registers: scalars, vectors, matrices and arrays as set_usd_attribute_value converts them; a dictionary from an object of entries; a list op from {explicit} or {prepended, appended, deleted, ordered, added} arrays of item texts, or of whole numbers for an integer list op such as inactiveIds. Any other field takes the type value names, else its JSON type. A value that does not fit writes nothing; if USD refuses the write, success is false with set=false and its errors. In memory: save_usd_stage writes it; get_usd_prim_metadata reads it back as USD resolves it.
//   @param stage_name [string!] - Name the stage was registered under by create_usd_stage or open_usd_stage; a stage already in the USD cache also matches by the end of its root layer identifier.
//   @param prim_path [string!] - Absolute SdfPath of an existing prim, e.g. "/World/Cube".
//   @param key [string!] - Registered USD prim metadata field, e.g. "comment", "kind", "customData" or "apiSchemas"; a key USD does not register as prim metadata fails with success false and set=false.
//   @param value [object!] - The value itself (a scalar, an array, or an object of dictionary entries) or {type?, value}. It is converted to the field's type when that is known, and type is then not used; else type, or the JSON type, decides (bool, number as double, string, object as dictionary). A list op is {"explicit": [...]} or {"prepended": [...], "appended", "deleted", "ordered", "added"}, a reference item "@a.usda@</Prim>", an integer list op item a whole number.
//   @returns { set: bool, prim_path: string, key: string, value: object, declared_type?: string, errors?: [string] }
TSharedPtr<FJsonObject> HandleSetUsdPrimMetadata(const TSharedPtr<FJsonObject>& Params)
{
	UE::FUsdStage Stage;
	FString StageName;
	FString PrimPath;
	if (TSharedPtr<FJsonObject> Error = UsdPrim_ResolveStageAndPrimPath(Params, Stage, StageName, PrimPath))
	{
		return Error;
	}

	FString Key;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("key"), Key)) { return Missing; }

	// The "value" param: an object with a "value" member is {type?, value}; any other object is the value itself, the
	// entries of a dictionary; a scalar or an array is the value.
	const TSharedPtr<FJsonValue> ValueParam = Params->TryGetField(TEXT("value"));
	if (!ValueParam.IsValid() || ValueParam->IsNull())
	{
		return mcp_common::MakeErrorReply(
			TEXT("value is required (a scalar, an array, an object of dictionary entries, or an object {type?, value})"));
	}
	TSharedPtr<FJsonValue> RawValue = ValueParam;
	FString NamedType;
	const TSharedPtr<FJsonObject>* ValueObject = nullptr;
	if (ValueParam->TryGetObject(ValueObject) && ValueObject != nullptr && ValueObject->IsValid() && (*ValueObject)->HasField(TEXT("value")))
	{
		RawValue = (*ValueObject)->TryGetField(TEXT("value"));
		(*ValueObject)->TryGetStringField(TEXT("type"), NamedType);
	}

	UE::FUsdPrim Prim;
	if (TSharedPtr<FJsonObject> Error = UsdPrim_FindPrim(Stage, StageName, PrimPath, Prim))
	{
		return Error;
	}

	// USD authors nothing for a value whose type is not the field's own (a string for the token field "kind"), so the
	// value is built in the field's type when it can be read; only a field whose type cannot be read falls back to the
	// type the value object names, or the one its JSON type implies.
	const FString DeclaredType = UsdPrim_MetadataDeclaredType(Prim, Key);
	const FString Type = !DeclaredType.IsEmpty() ? DeclaredType
		: !NamedType.IsEmpty() ? NamedType
		: UEAgent::UsdCommon::UsdTypeOfJson(RawValue);
	if (Type.IsEmpty())
	{
		return mcp_common::MakeErrorReply(FString::Printf(
			TEXT("the type of metadata '%s' on %s cannot be read and the value, %s, implies none: give it as {\"type\": \"double[]\", ")
			TEXT("\"value\": [...]}. Nothing was written."), *Key, *PrimPath, *UEAgent::UsdCommon::JsonShapeName(RawValue)));
	}
	UE::FVtValue Vt;
	FString VtError;
	if (!UEAgent::UsdCommon::VtValueForMetadataType(Type, RawValue, Vt, VtError, 0))
	{
		return mcp_common::MakeErrorReply(!DeclaredType.IsEmpty()
			? FString::Printf(TEXT("value cannot be authored as metadata '%s' on %s, a field of type %s: %s. Nothing was written."),
				*Key, *PrimPath, *DeclaredType, *VtError)
			: FString::Printf(TEXT("value cannot be authored as metadata '%s' on %s as type %s: %s. Nothing was written."),
				*Key, *PrimPath, *Type, *VtError));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	bool SetOk = false;
	// The USD errors the write raises are the reason a refused write gives; they land in Data["errors"].
	UEAgent::UsdCommon::CaptureUsdErrors([&Prim, &Key, &Vt, &SetOk]() { SetOk = Prim.SetMetadata(*Key, Vt); }, Data);

	Data->SetStringField(TEXT("prim_path"), PrimPath);
	Data->SetStringField(TEXT("key"), Key);
	Data->SetObjectField(TEXT("value"), UsdPrim_VtValueToJson(Vt));
	if (!DeclaredType.IsEmpty())
	{
		Data->SetStringField(TEXT("declared_type"), DeclaredType);
	}
	Data->SetBoolField(TEXT("set"), SetOk);
	if (!SetOk)
	{
		FString Reason = TEXT("an unregistered key, a value of another type than the field's, or an edit target that cannot be edited");
		const TArray<TSharedPtr<FJsonValue>>* Errors = nullptr;
		if (Data->TryGetArrayField(TEXT("errors"), Errors) && Errors && Errors->Num() > 0)
		{
			Reason = (*Errors)[0]->AsString();
		}
		return mcp_common::MakeOutcomeReply(Data, false, FString::Printf(
			TEXT("set_usd_prim_metadata: USD did not author metadata '%s' on %s (UsdPrim::SetMetadata returned false: %s). Nothing was written."),
			*Key, *PrimPath, *Reason));
	}
	return mcp_common::MakeOkReply(Data);
}

// MCP:get_usd_prim_metadata - Read the metadata of a prim on a registered USD stage as USD resolves it: one key (present plus value as {type, value}) or, with no key, all entries (authored and fallback) plus count; composition arcs such as references are read by key only, as their strongest opinion. Values read as get_usd_attribute_value reads them, an Sdf enum such as specifier as its number; a dictionary (customData, with the schema's fallback entries) as an object of {type, value} entries; a list op as {explicit} or {prepended, appended, deleted, ordered?, added?} arrays of item texts (numbers for an integer list op), apiSchemas, inactiveIds and other non-arc list ops composed into one explicit list; both with length and truncated. One reply holds about 30000 characters of values: past that, values are cut and further entries listed in omitted_keys. Above 100 entries only count and a note return. Read-only. Author with set_usd_prim_metadata; prim summary: get_usd_prim_info.
//   @param stage_name [string!] - Name the stage was registered under by create_usd_stage or open_usd_stage; a stage already in the USD cache also matches by the end of its root layer identifier.
//   @param prim_path [string!] - Absolute SdfPath of an existing prim, e.g. "/World/Cube".
//   @param key [string] - Metadata field to read, e.g. "documentation"; empty or omitted (default) returns all entries.
//   @returns { present: bool, key: string, value: { type: string, value: any, length?: number, truncated?: bool, note?: string }, metadata: object, count: number, truncated?: bool, omitted_keys?: [string], note?: string }
TSharedPtr<FJsonObject> HandleGetUsdPrimMetadata(const TSharedPtr<FJsonObject>& Params)
{
	UE::FUsdStage Stage;
	FString StageName;
	FString PrimPath;
	if (TSharedPtr<FJsonObject> Error = UsdPrim_ResolveStageAndPrimPath(Params, Stage, StageName, PrimPath))
	{
		return Error;
	}

	FString Key;
	Params->TryGetStringField(TEXT("key"), Key);

	UE::FUsdPrim Prim;
	if (TSharedPtr<FJsonObject> Error = UsdPrim_FindPrim(Stage, StageName, PrimPath, Prim))
	{
		return Error;
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("prim_path"), PrimPath);

	if (!Key.IsEmpty())
	{
		// One key, as UsdObject::GetMetadata resolves it: a dictionary merged across layers and with the prim
		// definition's fallback entries, a list op such as apiSchemas composed into one explicit list, a composition arc
		// (references, payload, inheritPaths, specializes, variantSetNames) only its strongest opinion.
		UE::FVtValue Vt;
		const bool Got = Prim.GetMetadata(*Key, Vt);
		Data->SetStringField(TEXT("key"), Key);
		Data->SetBoolField(TEXT("present"), Got);
		Data->SetObjectField(TEXT("value"), Got ? UsdPrim_VtValueToJson(Vt) : MakeShared<FJsonObject>());
		return mcp_common::MakeOkReply(Data);
	}

	// All-metadata read with size gate.
	const TMap<FString, UE::FVtValue> All = Prim.GetAllMetadata();
	const int32 Count = All.Num();
	Data->SetNumberField(TEXT("count"), Count);

	if (Count > MaxMetadataEntriesReported)
	{
		// Big prim — don't enumerate. Tell the caller to ask by key.
		Data->SetBoolField(TEXT("present"), false);
		Data->SetStringField(TEXT("note"),
			FString::Printf(TEXT("prim has %d metadata entries (>%d); call get_usd_prim_metadata with key=... for specific reads"),
				Count, MaxMetadataEntriesReported));
		return mcp_common::MakeOkReply(Data);
	}

	// One size budget for the whole reply, so it stays under the bridge's offload threshold: the containers among the
	// fields are cut within it, and the first field that does not fit what is left is left out, with every field after it.
	UEAgent::UsdCommon::FUsdJsonBudget Budget;
	TSharedPtr<FJsonObject> MetaObj = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Omitted;
	for (const TPair<FString, UE::FVtValue>& Entry : All)
	{
		TSharedPtr<FJsonObject> EntryJson = MakeShared<FJsonObject>();
		if (Omitted.Num() == 0
			&& Budget.Take(UEAgent::UsdCommon::UsdJsonEntryCost(Entry.Key, Entry.Value.GetTypeName(), 0))
			&& UEAgent::UsdCommon::UsdValueToJson(Entry.Value, *EntryJson, Budget, 0, true) != UEAgent::UsdCommon::EUsdJsonRead::OverBudget)
		{
			MetaObj->SetObjectField(Entry.Key, EntryJson);
			continue;
		}
		Omitted.Add(MakeShared<FJsonValueString>(Entry.Key));
	}
	Data->SetObjectField(TEXT("metadata"), MetaObj);
	Data->SetBoolField(TEXT("present"), Count > 0);
	if (Omitted.Num() > 0)
	{
		Data->SetBoolField(TEXT("truncated"), true);
		Data->SetArrayField(TEXT("omitted_keys"), Omitted);
		Data->SetStringField(TEXT("note"), FString::Printf(
			TEXT("%d of %d fields are left out (omitted_keys): with them the reply would pass the size one reply may take; read each with key=..."),
			Omitted.Num(), Count));
	}
	return mcp_common::MakeOkReply(Data);
}

// MCP:list_usd_prims - List the prims under one prim (default the pseudo root "/") on a registered USD stage, each with prim_path, type_name and active, plus count: its direct children, or with recursive=true its whole subtree in depth-first order. Only active, loaded, defined, non-abstract prims appear, and instance proxies are not traversed. Read-only.
//   @param stage_name [string!] - Name the stage was registered under by create_usd_stage or open_usd_stage; a stage already in the USD cache also matches by the end of its root layer identifier.
//   @param prim_path_prefix [string] - SdfPath of the prim whose children are listed; it must exist. Default "/" (the pseudo root).
//   @param recursive [bool] - false (default) lists the direct children only; true lists every descendant, each parent before its children.
//   @returns { prims: [{ prim_path, type_name, active }], count: number }
TSharedPtr<FJsonObject> HandleListUsdPrims(const TSharedPtr<FJsonObject>& Params)
{
	UE::FUsdStage Stage;
	FString StageName;
	if (TSharedPtr<FJsonObject> Error = UsdPrim_ResolveStage(Params, Stage, StageName))
	{
		return Error;
	}

	FString Prefix = TEXT("/");
	Params->TryGetStringField(TEXT("prim_path_prefix"), Prefix);
	FString PathError;
	if (!UEAgent::UsdCommon::IsValidPrimPath(Prefix, PathError))
	{
		return mcp_common::MakeErrorReply(PathError);
	}

	bool Recursive = false;
	Params->TryGetBoolField(TEXT("recursive"), Recursive);

	UE::FUsdPrim Root;
	if (TSharedPtr<FJsonObject> Error = UsdPrim_FindPrim(Stage, StageName, Prefix, Root))
	{
		return Error;
	}

	// GetChildren and GetFilteredChildren(false) both apply the USD default predicate and stop at
	// the direct children, so the subtree is walked here: a stack in reverse child order keeps the
	// output depth-first, each parent before its children and siblings in stage order.
	TArray<TSharedPtr<FJsonValue>> Out;
	TArray<UE::FUsdPrim> Pending = Root.GetFilteredChildren(/*bTraverseInstanceProxies*/ false);
	Algo::Reverse(Pending);
	while (Pending.Num() > 0)
	{
		const UE::FUsdPrim Prim = Pending.Pop(EAllowShrinking::No);
		if (!Prim.IsValid()) { continue; }
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("prim_path"), Prim.GetPrimPath().GetString());
		O->SetStringField(TEXT("type_name"), Prim.GetTypeName().ToString());
		O->SetBoolField(TEXT("active"), Prim.IsActive());
		Out.Add(MakeShared<FJsonValueObject>(O));
		if (Recursive)
		{
			TArray<UE::FUsdPrim> Children = Prim.GetFilteredChildren(/*bTraverseInstanceProxies*/ false);
			Algo::Reverse(Children);
			Pending.Append(MoveTemp(Children));
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("prims"), Out);
	Data->SetNumberField(TEXT("count"), Out.Num());
	return mcp_common::MakeOkReply(Data);
}

// MCP:get_usd_prim_info - Read one prim of a registered USD stage: name, type_name, active, parent_path, direct children_count, applied_schemas, variant_sets and an is_a map for nine common types (Xform, Mesh, Cube, Scope, Camera, four lights). A missing prim returns is_valid=false with a note, not an error. Read-only.
//   @param stage_name [string!] - Name the stage was registered under by create_usd_stage or open_usd_stage; a stage already in the USD cache also matches by the end of its root layer identifier.
//   @param prim_path [string!] - Absolute SdfPath of the prim to read, e.g. "/World/Cube".
//   @returns { name: string, prim_path: string, type_name: string, active: bool, is_valid: bool, is_pseudo_root: bool, parent_path: string, children_count: number, applied_schemas: [string], variant_sets: [string], is_a: { Xform, Mesh, Cube, Scope, Camera, DistantLight, SphereLight, RectLight, MeshLight: bool } }, or { prim_path, is_valid: false, note } when the prim does not exist
TSharedPtr<FJsonObject> HandleGetUsdPrimInfo(const TSharedPtr<FJsonObject>& Params)
{
	UE::FUsdStage Stage;
	FString StageName;
	FString PrimPath;
	if (TSharedPtr<FJsonObject> Error = UsdPrim_ResolveStageAndPrimPath(Params, Stage, StageName, PrimPath))
	{
		return Error;
	}

	UE::FUsdPrim Prim = Stage.GetPrimAtPath(UE::FSdfPath(*PrimPath));
	if (!Prim.IsValid())
	{
		// Honest headless: report is_valid=false rather than crashing.
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("prim_path"), PrimPath);
		Data->SetBoolField(TEXT("is_valid"), false);
		Data->SetStringField(TEXT("note"),
			FString::Printf(TEXT("prim '%s' does not exist on stage '%s'"), *PrimPath, *StageName));
		return mcp_common::MakeOkReply(Data);
	}

	UE::FUsdPrim Parent = Prim.GetParent();
	const FString ParentPath = Parent.IsValid() ? Parent.GetPrimPath().GetString() : FString();

	const TArray<UE::FUsdPrim> Children = Prim.GetChildren();
	const TArray<FName> AppliedSchemas = Prim.GetAppliedSchemas();

	TArray<TSharedPtr<FJsonValue>> SchemasArr;
	SchemasArr.Reserve(AppliedSchemas.Num());
	for (const FName& SchemaName : AppliedSchemas)
	{
		SchemasArr.Add(MakeShared<FJsonValueString>(SchemaName.ToString()));
	}

	TArray<TSharedPtr<FJsonValue>> VariantSetNamesArr;
	UE::FUsdVariantSets VSets = Prim.GetVariantSets();
	if (VSets.operator bool())
	{
		VariantSetNamesArr = mcp_common::StringsToJson(VSets.GetNames());
	}

	// is_a map for common USD types (limited whitelist — covers the typical
	// "is this prim a Mesh / Camera / Cube?" check the LLM reaches for).
	TSharedPtr<FJsonObject> IsAMap = MakeShared<FJsonObject>();
	{
		static const TArray<FString> CommonSchemaChecks = {
			TEXT("Xform"), TEXT("Mesh"), TEXT("Cube"), TEXT("Scope"),
			TEXT("Camera"), TEXT("DistantLight"), TEXT("SphereLight"),
			TEXT("RectLight"), TEXT("MeshLight")
		};
		for (const FString& Name : CommonSchemaChecks)
		{
			IsAMap->SetBoolField(Name, Prim.IsA(FName(*Name)));
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("name"), Prim.GetName().ToString());
	Data->SetStringField(TEXT("prim_path"), Prim.GetPrimPath().GetString());
	Data->SetStringField(TEXT("type_name"), Prim.GetTypeName().ToString());
	Data->SetBoolField(TEXT("active"), Prim.IsActive());
	Data->SetBoolField(TEXT("is_valid"), Prim.IsValid());
	Data->SetBoolField(TEXT("is_pseudo_root"), Prim.IsPseudoRoot());
	Data->SetStringField(TEXT("parent_path"), ParentPath);
	Data->SetNumberField(TEXT("children_count"), Children.Num());
	Data->SetArrayField(TEXT("applied_schemas"), SchemasArr);
	Data->SetArrayField(TEXT("variant_sets"), VariantSetNamesArr);
	Data->SetObjectField(TEXT("is_a"), IsAMap);
	return mcp_common::MakeOkReply(Data);
}

// MCP:apply_usd_prim_schema - Apply a USD API schema (e.g. "MaterialBindingAPI") to an existing prim of a registered USD stage, on the current edit target. Checks CanApplyAPI first: a schema that cannot apply returns applied=false with the USD reason in why_not instead of an error. Not saved to disk.
//   @param stage_name [string!] - Name the stage was registered under by create_usd_stage or open_usd_stage; a stage already in the USD cache also matches by the end of its root layer identifier.
//   @param prim_path [string!] - Absolute SdfPath of an existing prim, e.g. "/World/Cube".
//   @param schema_name [string!] - Schema identifier (not the C++ class name), e.g. "MaterialBindingAPI", "GeomModelAPI"
//   @returns { applied: bool, schema_name: string, why_not?: string }
TSharedPtr<FJsonObject> HandleApplyUsdPrimSchema(const TSharedPtr<FJsonObject>& Params)
{
	UE::FUsdStage Stage;
	FString StageName;
	FString PrimPath;
	if (TSharedPtr<FJsonObject> Error = UsdPrim_ResolveStageAndPrimPath(Params, Stage, StageName, PrimPath))
	{
		return Error;
	}

	FString SchemaName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("schema_name"), SchemaName)) { return Missing; }

	UE::FUsdPrim Prim;
	if (TSharedPtr<FJsonObject> Error = UsdPrim_FindPrim(Stage, StageName, PrimPath, Prim))
	{
		return Error;
	}

	FString WhyNot;
	const bool CanApply = Prim.CanApplyAPI(FName(*SchemaName), &WhyNot);
	if (!CanApply)
	{
		// Surface the pxr:: reason instead of failing silently — caller decides
		// whether to retype the prim or pick a different schema.
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("schema_name"), SchemaName);
		Data->SetBoolField(TEXT("applied"), false);
		Data->SetStringField(TEXT("why_not"), WhyNot.IsEmpty()
			? FString(TEXT("CanApplyAPI returned false with no reason"))
			: WhyNot);
		return mcp_common::MakeOkReply(Data);
	}

	const bool Applied = Prim.ApplyAPI(FName(*SchemaName));

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("schema_name"), SchemaName);
	Data->SetBoolField(TEXT("applied"), Applied);
	if (!Applied)
	{
		Data->SetStringField(TEXT("why_not"), TEXT("ApplyAPI returned false after CanApplyAPI succeeded (race or schema-registry mismatch)"));
	}
	return mcp_common::MakeOkReply(Data);
}

// MCP:remove_usd_prim_schema - Remove an applied API schema (e.g. "MaterialBindingAPI") from an existing prim of a registered USD stage, on the current edit target. A schema the prim does not have returns removed=false and was_applied=false instead of an error. Not saved to disk; inverse of apply_usd_prim_schema.
//   @param stage_name [string!] - Name the stage was registered under by create_usd_stage or open_usd_stage; a stage already in the USD cache also matches by the end of its root layer identifier.
//   @param prim_path [string!] - Absolute SdfPath of an existing prim, e.g. "/World/Cube".
//   @param schema_name [string!] - Schema identifier as listed in applied_schemas by get_usd_prim_info, e.g. "MaterialBindingAPI".
//   @returns { removed: bool, schema_name: string }
TSharedPtr<FJsonObject> HandleRemoveUsdPrimSchema(const TSharedPtr<FJsonObject>& Params)
{
	UE::FUsdStage Stage;
	FString StageName;
	FString PrimPath;
	if (TSharedPtr<FJsonObject> Error = UsdPrim_ResolveStageAndPrimPath(Params, Stage, StageName, PrimPath))
	{
		return Error;
	}

	FString SchemaName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("schema_name"), SchemaName)) { return Missing; }

	UE::FUsdPrim Prim;
	if (TSharedPtr<FJsonObject> Error = UsdPrim_FindPrim(Stage, StageName, PrimPath, Prim))
	{
		return Error;
	}

	// Idempotent guard: HasAPI returns false if the schema isn't applied, in
	// which case RemoveAPI would no-op anyway. We surface removed=false with
	// the same shape as a successful remove so callers can gate on it.
	const bool HadIt = Prim.HasAPI(FName(*SchemaName));
	const bool Removed = HadIt ? Prim.RemoveAPI(FName(*SchemaName)) : false;

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("schema_name"), SchemaName);
	Data->SetBoolField(TEXT("removed"), Removed);
	Data->SetBoolField(TEXT("was_applied"), HadIt);
	return mcp_common::MakeOkReply(Data);
}

const McpCommandFamilies::FRegistrar Registration =
{
	{ TEXT("apply_usd_prim_schema"), &HandleApplyUsdPrimSchema },
	{ TEXT("define_usd_prim"), &HandleDefineUsdPrim },
	{ TEXT("get_usd_prim_info"), &HandleGetUsdPrimInfo },
	{ TEXT("get_usd_prim_metadata"), &HandleGetUsdPrimMetadata },
	{ TEXT("list_usd_prims"), &HandleListUsdPrims },
	{ TEXT("override_usd_prim"), &HandleOverrideUsdPrim },
	{ TEXT("remove_usd_prim"), &HandleRemoveUsdPrim },
	{ TEXT("remove_usd_prim_schema"), &HandleRemoveUsdPrimSchema },
	{ TEXT("set_usd_prim_metadata"), &HandleSetUsdPrimMetadata },
	{ TEXT("set_usd_prim_specifier"), &HandleSetUsdPrimSpecifier },
};
} // namespace UEAgentUsdPrimCommands
