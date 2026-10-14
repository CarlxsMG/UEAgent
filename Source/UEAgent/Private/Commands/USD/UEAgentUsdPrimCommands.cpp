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
	// Parse a "Def" | "Over" | "Class" string into UE::ESdfSpecifier. Def is the
	// USD default. We are case-sensitive here (pxr:: specifier token form) —
	// "def" lowercase is also accepted for caller convenience.
	UE::ESdfSpecifier ParseSpecifier(const FString& In)
	{
		const FString S = In.IsEmpty() ? TEXT("Def") : In;
		if (S.Equals(TEXT("Def"), ESearchCase::IgnoreCase)) { return UE::ESdfSpecifier::Def; }
		if (S.Equals(TEXT("Over"), ESearchCase::IgnoreCase)) { return UE::ESdfSpecifier::Over; }
		if (S.Equals(TEXT("Class"), ESearchCase::IgnoreCase)) { return UE::ESdfSpecifier::Class; }
		return UE::ESdfSpecifier::Def;
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

	// {"type", "value"} of Value; a type with no JSON scalar form gets a placeholder string as its "value".
	TSharedPtr<FJsonObject> UsdPrim_VtValueToJson(const UE::FVtValue& Value)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		if (!UEAgent::UsdCommon::VtValueToJson(Value, *Out))
		{
			Out->SetStringField(TEXT("value"),
				FString::Printf(TEXT("<unsupported type '%s' -- string repr not available>"), *Value.GetTypeName()));
		}
		return Out;
	}
}

// MCP:define_usd_prim - Define (or re-author) a prim at the given path with a USD type
//   @param stage_name [string!] - Name the stage was registered under
//   @param prim_path [string!] - Absolute SdfPath, e.g. /World/Cube
//   @param type_name [string] - USD type name (Xform, Mesh, Cube, Scope, Camera, DistantLight...). Default: Xform
//   @param specifier [string] - "Def" | "Over" | "Class". Default: Def
//   @returns { prim_path: string, type_name: string, specifier: string, is_valid: bool }
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
	const UE::ESdfSpecifier Spec = ParseSpecifier(SpecStr);

	const UE::FSdfPath Path(*PrimPath);
	UE::FUsdPrim Prim = Stage.DefinePrim(Path, *TypeName);
	if (!Prim.IsValid())
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("DefinePrim('%s','%s') returned an invalid prim on stage '%s'"),
				*PrimPath, *TypeName, *StageName));
	}

	bool SpecSetOk = true;
	if (Spec != UE::ESdfSpecifier::Def)
	{
		SpecSetOk = Prim.SetSpecifier(Spec);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("prim_path"), PrimPath);
	Data->SetStringField(TEXT("type_name"), Prim.GetTypeName().ToString());
	Data->SetStringField(TEXT("specifier"), SpecStr);
	Data->SetBoolField(TEXT("is_valid"), Prim.IsValid());
	Data->SetBoolField(TEXT("specifier_set_ok"), SpecSetOk);
	return mcp_common::MakeOkReply(Data);
}

// MCP:override_usd_prim - Create (or retrieve) an Over specifier at the given path
//   @param stage_name [string!] - Stage cache key
//   @param prim_path [string!] - SdfPath to author an Over for
//   @returns { prim_path: string, is_valid: bool }
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
	UE::FUsdPrim Prim = Stage.OverridePrim(Path);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("prim_path"), PrimPath);
	Data->SetBoolField(TEXT("is_valid"), Prim.IsValid());
	if (Prim.IsValid())
	{
		Data->SetStringField(TEXT("specifier"), TEXT("Over"));
	}
	return mcp_common::MakeOkReply(Data);
}

// MCP:remove_usd_prim - Remove a prim (and its descendants) from the stage
//   @param stage_name [string!] - Stage cache key
//   @param prim_path [string!] - SdfPath to remove
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

// MCP:set_usd_prim_type - Change the typeName of an EXISTING prim
//   @param stage_name [string!] - Stage cache key
//   @param prim_path [string!] - SdfPath of the prim to retype
//   @param type_name [string!] - New USD type name (e.g. Mesh, Xform, Cube)
//   @returns { set: bool, type_name: string }
TSharedPtr<FJsonObject> HandleSetUsdPrimType(const TSharedPtr<FJsonObject>& Params)
{
	UE::FUsdStage Stage;
	FString StageName;
	FString PrimPath;
	if (TSharedPtr<FJsonObject> Error = UsdPrim_ResolveStageAndPrimPath(Params, Stage, StageName, PrimPath))
	{
		return Error;
	}

	FString TypeName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("type_name"), TypeName)) { return Missing; }

	UE::FUsdPrim Prim;
	if (TSharedPtr<FJsonObject> Error = UsdPrim_FindPrim(Stage, StageName, PrimPath, Prim, TEXT(" (DefinePrim first)")))
	{
		return Error;
	}

	const bool SetOk = Prim.SetTypeName(FName(*TypeName));

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("type_name"), TypeName);
	Data->SetBoolField(TEXT("set"), SetOk);
	return mcp_common::MakeOkReply(Data);
}

// MCP:set_usd_prim_active - Activate or deactivate a prim
//   @param stage_name [string!] - Stage cache key
//   @param prim_path [string!] - Target prim path
//   @param active [bool!] - true to activate, false to deactivate
//   @returns { active: bool, prim_path: string }
TSharedPtr<FJsonObject> HandleSetUsdPrimActive(const TSharedPtr<FJsonObject>& Params)
{
	UE::FUsdStage Stage;
	FString StageName;
	FString PrimPath;
	if (TSharedPtr<FJsonObject> Error = UsdPrim_ResolveStageAndPrimPath(Params, Stage, StageName, PrimPath))
	{
		return Error;
	}

	bool Active = true;
	Params->TryGetBoolField(TEXT("active"), Active);

	UE::FUsdPrim Prim;
	if (TSharedPtr<FJsonObject> Error = UsdPrim_FindPrim(Stage, StageName, PrimPath, Prim))
	{
		return Error;
	}

	const bool SetOk = Prim.SetActive(Active);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	// Report the EFFECTIVE active flag after the call (Prim.IsActive reflects
	// the current truth). If SetActive returned false, the effective flag is
	// still the prior value, but we report the request intent for clarity.
	Data->SetStringField(TEXT("prim_path"), PrimPath);
	Data->SetBoolField(TEXT("active"), SetOk ? Active : Prim.IsActive());
	Data->SetBoolField(TEXT("set"), SetOk);
	return mcp_common::MakeOkReply(Data);
}

// MCP:set_usd_prim_specifier - Change a prim's specifier (Def/Over/Class)
//   @param stage_name [string!] - Stage cache key
//   @param prim_path [string!] - Target prim path
//   @param specifier [string!] - "Def" | "Over" | "Class"
//   @returns { set: bool, specifier: string }
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

	UE::FUsdPrim Prim;
	if (TSharedPtr<FJsonObject> Error = UsdPrim_FindPrim(Stage, StageName, PrimPath, Prim))
	{
		return Error;
	}

	const UE::ESdfSpecifier Spec = ParseSpecifier(SpecStr);
	const bool SetOk = Prim.SetSpecifier(Spec);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("specifier"), SpecStr);
	Data->SetBoolField(TEXT("set"), SetOk);
	return mcp_common::MakeOkReply(Data);
}

// MCP:set_usd_prim_metadata - Author a metadata key/value on a prim
//   @param stage_name [string!] - Stage cache key
//   @param prim_path [string!] - Target prim path
//   @param key [string!] - Metadata key (e.g. "custom:author", "comment", "documentation")
//   @param value [object!] - {type: "bool|int|float|double|string|token", value: <scalar>} OR {value: <scalar>} (type inferred from the JSON runtime type)
//   @returns { set: bool, key: string, value: object }
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

	// The "value" param carries either a primitive (implicit type) or an
	// object {"type": "...", "value": ...} (explicit type). We promote raw
	// primitives to {value: <primitive>} so the same dispatch path applies.
	TSharedPtr<FJsonObject> ValueObj = UEAgent::UsdCommon::TypedValueObject(Params->TryGetField(TEXT("value")));

	if (!ValueObj.IsValid())
	{
		return mcp_common::MakeErrorReply(
			TEXT("value is required (object {type, value} or scalar)"));
	}

	UE::FUsdPrim Prim;
	if (TSharedPtr<FJsonObject> Error = UsdPrim_FindPrim(Stage, StageName, PrimPath, Prim))
	{
		return Error;
	}

	UE::FVtValue Vt;
	FString VtError;
	if (!UEAgent::UsdCommon::VtValueFromJson(ValueObj, Vt, VtError))
	{
		return mcp_common::MakeErrorReply(
			TEXT("value is not convertible to a supported USD type (bool|int|int64|float|double|string|token)"));
	}

	const bool SetOk = Prim.SetMetadata(*Key, Vt);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("key"), Key);
	Data->SetObjectField(TEXT("value"), UsdPrim_VtValueToJson(Vt));
	Data->SetBoolField(TEXT("set"), SetOk);
	return mcp_common::MakeOkReply(Data);
}

// MCP:get_usd_prim_metadata - Read metadata for a prim (specific key or all)
//   @param stage_name [string!] - Stage cache key
//   @param prim_path [string!] - Target prim path
//   @param key [string] - Specific key to read. Empty (default) returns GetAllMetadata
//   @returns { present: bool, key: string, value: object, metadata: object, count: number, note?: string }
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
		// Specific-key read.
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

	TSharedPtr<FJsonObject> MetaObj = MakeShared<FJsonObject>();
	for (const TPair<FString, UE::FVtValue>& Entry : All)
	{
		MetaObj->SetObjectField(Entry.Key, UsdPrim_VtValueToJson(Entry.Value));
	}
	Data->SetObjectField(TEXT("metadata"), MetaObj);
	Data->SetBoolField(TEXT("present"), Count > 0);
	return mcp_common::MakeOkReply(Data);
}

// MCP:set_usd_prim_display_name - Set the human-readable displayName metadata
//   @param stage_name [string!] - Stage cache key
//   @param prim_path [string!] - Target prim path
//   @param display_name [string!] - New display name
//   @returns { set: bool, display_name: string }
TSharedPtr<FJsonObject> HandleSetUsdPrimDisplayName(const TSharedPtr<FJsonObject>& Params)
{
	UE::FUsdStage Stage;
	FString StageName;
	FString PrimPath;
	if (TSharedPtr<FJsonObject> Error = UsdPrim_ResolveStageAndPrimPath(Params, Stage, StageName, PrimPath))
	{
		return Error;
	}

	FString DisplayName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("display_name"), DisplayName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

	UE::FUsdPrim Prim;
	if (TSharedPtr<FJsonObject> Error = UsdPrim_FindPrim(Stage, StageName, PrimPath, Prim))
	{
		return Error;
	}

	const bool SetOk = Prim.SetDisplayName(DisplayName);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("display_name"), DisplayName);
	Data->SetBoolField(TEXT("set"), SetOk);
	return mcp_common::MakeOkReply(Data);
}

// MCP:list_usd_prims - List child prims under a prefix (recursive or direct)
//   @param stage_name [string!] - Stage cache key
//   @param prim_path_prefix [string] - Path whose children to enumerate. Default: "/" (pseudo root)
//   @param recursive [bool] - If true (default), use GetFilteredChildren (traverse instance proxies). If false, GetChildren (direct only).
//   @returns { prims: [object], count: number }
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

	bool Recursive = true;
	Params->TryGetBoolField(TEXT("recursive"), Recursive);

	UE::FUsdPrim Root;
	if (TSharedPtr<FJsonObject> Error = UsdPrim_FindPrim(Stage, StageName, Prefix, Root))
	{
		return Error;
	}

	TArray<UE::FUsdPrim> Children = Recursive
		? Root.GetFilteredChildren(/*bTraverseInstanceProxies*/ false)
		: Root.GetChildren();

	TArray<TSharedPtr<FJsonValue>> Out;
	Out.Reserve(Children.Num());
	for (const UE::FUsdPrim& Child : Children)
	{
		if (!Child.IsValid()) { continue; }
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("prim_path"), Child.GetPrimPath().GetString());
		O->SetStringField(TEXT("type_name"), Child.GetTypeName().ToString());
		O->SetBoolField(TEXT("active"), Child.IsActive());
		Out.Add(MakeShared<FJsonValueObject>(O));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("prims"), Out);
	Data->SetNumberField(TEXT("count"), Out.Num());
	return mcp_common::MakeOkReply(Data);
}

// MCP:get_usd_prim_info - Read identity + relationships for a prim
//   @param stage_name [string!] - Stage cache key
//   @param prim_path [string!] - Target prim path
//   @returns { name: string, prim_path: string, type_name: string, active: bool, is_valid: bool, is_pseudo_root: bool,
//             parent_path: string, children_count: number, applied_schemas: [string],
//             variant_sets: [string], is_a: object<string,bool> }
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

// MCP:apply_usd_prim_schema - Apply a USD schema (API schema) to a prim
//   @param stage_name [string!] - Stage cache key
//   @param prim_path [string!] - Target prim path
//   @param schema_name [string!] - Schema identifier, e.g. "MaterialBindingAPI", "UsdGeomModelAPI"
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

// MCP:remove_usd_prim_schema - Remove a previously applied schema from a prim
//   @param stage_name [string!] - Stage cache key
//   @param prim_path [string!] - Target prim path
//   @param schema_name [string!] - Schema identifier (e.g. "MaterialBindingAPI")
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

// MCP:set_usd_prim_instanceable - Mark a prim as instanceable (or clear the flag)
//   @param stage_name [string!] - Stage cache key
//   @param prim_path [string!] - Target prim path
//   @param instanceable [bool!] - true to mark instanceable, false to clear
//   @returns { instanceable: bool, prim_path: string }
TSharedPtr<FJsonObject> HandleSetUsdPrimInstanceable(const TSharedPtr<FJsonObject>& Params)
{
	UE::FUsdStage Stage;
	FString StageName;
	FString PrimPath;
	if (TSharedPtr<FJsonObject> Error = UsdPrim_ResolveStageAndPrimPath(Params, Stage, StageName, PrimPath))
	{
		return Error;
	}

	bool Instanceable = true;
	Params->TryGetBoolField(TEXT("instanceable"), Instanceable);

	UE::FUsdPrim Prim;
	if (TSharedPtr<FJsonObject> Error = UsdPrim_FindPrim(Stage, StageName, PrimPath, Prim))
	{
		return Error;
	}

	const bool SetOk = Prim.SetInstanceable(Instanceable);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("prim_path"), PrimPath);
	Data->SetBoolField(TEXT("instanceable"), SetOk ? Instanceable : Prim.IsInstanceable());
	Data->SetBoolField(TEXT("set"), SetOk);
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
	{ TEXT("set_usd_prim_active"), &HandleSetUsdPrimActive },
	{ TEXT("set_usd_prim_display_name"), &HandleSetUsdPrimDisplayName },
	{ TEXT("set_usd_prim_instanceable"), &HandleSetUsdPrimInstanceable },
	{ TEXT("set_usd_prim_metadata"), &HandleSetUsdPrimMetadata },
	{ TEXT("set_usd_prim_specifier"), &HandleSetUsdPrimSpecifier },
	{ TEXT("set_usd_prim_type"), &HandleSetUsdPrimType },
};
} // namespace UEAgentUsdPrimCommands
