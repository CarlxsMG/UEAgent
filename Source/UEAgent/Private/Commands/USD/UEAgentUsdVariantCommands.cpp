// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

#include "UsdWrappers/ForwardDeclarations.h"
#include "UsdWrappers/UsdPrim.h"
#include "UsdWrappers/UsdStage.h"
#include "UsdWrappers/UsdVariantSets.h"
#include "UsdWrappers/SdfPath.h"
#include "UsdWrappers/SdfPrimSpec.h"  // UE::FSdfPrimSpec — for Prim.GetPrimStack().Last() (edit-target guard)
#include "UnrealUSDWrapper.h"           // EUsdListPosition enum
#include "Commands/USD/UEAgentUsdRegistry.h"  // stage registry + helpers shared by the USD handlers
#include "UEAgentUsdSdk.h"                  // remove_usd_variant(_set): the spec edits the wrapper has no API for

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonTypes.h"
#include "Templates/SharedPointer.h"

/**
 * USD variant-set commands.
 *
 * Reads, creates, mutates and selects variant sets on USD prims via the
 * UnrealUSDWrapper headless API (no UObject, no World, no PIE).
 *
 * Engine APIs (module UnrealUSDWrapper, Public/UsdWrappers/):
 *   - UE::FUsdVariantSets::AddVariantSet / GetNames / GetVariantSet /
 *     HasVariantSet / GetVariantSelection / SetSelection /
 *     GetAllVariantSelections (UsdVariantSets.h)
 *   - UE::FUsdVariantSet::AddVariant / GetVariantNames / ClearVariantSelection /
 *     IsValid (UsdVariantSets.h)
 *   - UE::FUsdPrim::GetVariantSets() (UsdPrim.h)
 *   - UE::FUsdStage::SetEditTarget(Layer) (UsdStage.h)
 *
 * Plugin: USDCore (must be enabled).
 *
 * Construction note: UE::FUsdVariantSets and UE::FUsdVariantSet can ONLY be
 * constructed by UE::FUsdPrim (friend classes). The wrapper enforces this by
 * deleting the implicit copy ctor — the code must use the chain
 * `Prim.GetVariantSets().GetVariantSet(name)` and then call FUsdVariantSet
 * methods on the result. Building the FUsdVariantSet from a raw name string
 * is not possible without a Prim handle.
 *
 * Edit-target guard: mutations to variant
 * sets (AddVariantSet / AddVariant / SetSelection / ClearVariantSelection)
 * write to the stage's current edit target layer. Commands that author
 * variant data first call UsdVariant_TargetStrongestPrimLayer, which sets the
 * edit target to the layer of `Prim.GetPrimStack().Last()`, so the mutation
 * lands on the strongest spec for the prim (rather than the session layer or
 * a weaker sublayer).
 */
namespace UEAgentUsdVariantCommands
{

// MCP-DOMAIN: USD

namespace
{
	// Build a {variant_set_name: variant_name} JSON object from a TMap.
	TSharedPtr<FJsonObject> SelectionsToJson(const TMap<FString, FString>& Selections)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		for (const TPair<FString, FString>& Pair : Selections)
		{
			Obj->SetStringField(Pair.Key, Pair.Value);
		}
		return Obj;
	}

	// The stage the stage_name param names (UEAgent::UsdRegistry::FindNamedStage): null, or the error reply.
	TSharedPtr<FJsonObject> UsdVariant_ResolveStage(const TSharedPtr<FJsonObject>& Params, UE::FUsdStage& OutStage,
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
		const FString LookupError = UEAgent::UsdRegistry::FindNamedStage(StageName, TEXT("cached"), OutStage);
		if (!LookupError.IsEmpty())
		{
			return mcp_common::MakeErrorReply(LookupError);
		}
		OutStageName = StageName;
		return nullptr;
	}

	// UsdVariant_ResolveStage, then the required prim_path, validated: null, or the error reply of the first that fails.
	TSharedPtr<FJsonObject> UsdVariant_ResolveStageAndPrimPath(const TSharedPtr<FJsonObject>& Params, UE::FUsdStage& OutStage,
		FString& OutStageName, FString& OutPrimPath)
	{
		if (TSharedPtr<FJsonObject> Error = UsdVariant_ResolveStage(Params, OutStage, OutStageName))
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

	// The prim at PrimPath on Stage: null, or the error reply naming StageName when there is none.
	TSharedPtr<FJsonObject> UsdVariant_FindPrim(const UE::FUsdStage& Stage, const FString& StageName, const FString& PrimPath,
		UE::FUsdPrim& OutPrim)
	{
		OutPrim = Stage.GetPrimAtPath(UE::FSdfPath(*PrimPath));
		if (!OutPrim.IsValid())
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("prim '%s' does not exist on stage '%s' (DefinePrim first)"),
					*PrimPath, *StageName));
		}
		return nullptr;
	}

	// The variant set VariantSetName of VSets: null, or the error reply when the prim at PrimPath does not author
	// it (Hint ends that message) or it fails to construct.
	TSharedPtr<FJsonObject> UsdVariant_GetVariantSet(const UE::FUsdVariantSets& VSets, const FString& VariantSetName,
		const FString& PrimPath, const TCHAR* Hint, UE::FUsdVariantSet& OutSet)
	{
		if (!VSets.HasVariantSet(VariantSetName))
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("variant set '%s' not found on prim '%s'%s"), *VariantSetName, *PrimPath, Hint));
		}
		OutSet = VSets.GetVariantSet(VariantSetName);
		if (!OutSet.IsValid())
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("variant set '%s' reported present but failed to construct"), *VariantSetName));
		}
		return nullptr;
	}

	// Edit-target guard: points Stage at the strongest layer that authors Prim, so what is written next lands
	// next to the prim spec rather than on the session layer or a weaker sublayer. GetPrimStack returns the
	// per-prim spec list; its last entry is the strongest spec, the "strongest layer where the prim is defined".
	void UsdVariant_TargetStrongestPrimLayer(UE::FUsdStage& Stage, const UE::FUsdPrim& Prim)
	{
		const TArray<UE::FSdfPrimSpec> PrimStack = Prim.GetPrimStack();
		if (PrimStack.Num() > 0)
		{
			UE::FSdfLayer EditLayer = PrimStack.Last().GetLayer();
			if (EditLayer)
			{
				Stage.SetEditTarget(EditLayer);
			}
		}
	}
} // namespace

// ---------------------------------------------------------------------------
// list_usd_variant_sets
// ---------------------------------------------------------------------------
// MCP:list_usd_variant_sets - List the variant sets a prim of a cached USD stage has, without changing anything: their names, each set's variant names (variants) and the selected variant of each set that has one (selections, empty when none is authored). With variant_set_name only that set is reported, also as variant_set_name, current_selection (empty when none) and variant_count, and a set the prim does not have is an error. Use it before set_usd_variant_selection or clear_usd_variant_selection, which take these names; author sets and variants with add_usd_variant_set and add_usd_variant, delete them with remove_usd_variant_set and remove_usd_variant. An unknown stage, an invalid prim path or a prim that does not exist is an error; open a stage first with open_usd_stage.
//   @param stage_name [string!] - Name the stage was registered under by open_usd_stage or create_usd_stage (list_usd_stages shows it), or the end of a cached stage's root layer identifier, case-insensitive, e.g. "Shot010"
//   @param prim_path [string!] - Absolute SdfPath of the existing prim to inspect, e.g. "/World/Car"
//   @param variant_set_name - Optional name of the one variant set to report, e.g. "color"; omitted or empty reports every set
//   @returns { prim_path: string, names: [string], selections: object, variants: object (set name -> [variant name]), count: int, variant_set_name?: string, current_selection?: string, variant_count?: int }

TSharedPtr<FJsonObject> HandleListUsdVariantSets(const TSharedPtr<FJsonObject>& Params)
{
	UE::FUsdStage Stage;
	FString StageName;
	FString PrimPath;
	if (TSharedPtr<FJsonObject> Error = UsdVariant_ResolveStageAndPrimPath(Params, Stage, StageName, PrimPath))
	{
		return Error;
	}

	UE::FUsdPrim Prim;
	if (TSharedPtr<FJsonObject> Error = UsdVariant_FindPrim(Stage, StageName, PrimPath, Prim))
	{
		return Error;
	}

	// FUsdVariantSets is constructed only by FUsdPrim (friend access). For a
	// valid prim, GetVariantSets() always returns a non-invalid instance.
	UE::FUsdVariantSets VSets = Prim.GetVariantSets();
	TArray<FString> Names = VSets.GetNames();
	TMap<FString, FString> Selections = VSets.GetAllVariantSelections();

	// One named set: HasVariantSet is the explicit guard (the bool conversion is well-defined on a missing
	// set in pxr::, so a missing name must not read as an empty set).
	FString OnlySet;
	const bool bOneSet = Params->TryGetStringField(TEXT("variant_set_name"), OnlySet) && !OnlySet.IsEmpty();
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	if (bOneSet)
	{
		UE::FUsdVariantSet VSet;
		if (TSharedPtr<FJsonObject> Error = UsdVariant_GetVariantSet(VSets, OnlySet, PrimPath, TEXT(""), VSet))
		{
			return Error;
		}
		const TArray<FString> Variants = VSet.GetVariantNames();
		const FString Selection = VSet.GetVariantSelection();
		Names = { OnlySet };
		Selections.Empty();
		if (!Selection.IsEmpty())
		{
			Selections.Add(OnlySet, Selection);
		}
		Data->SetStringField(TEXT("variant_set_name"), OnlySet);
		Data->SetStringField(TEXT("current_selection"), Selection);
		Data->SetNumberField(TEXT("variant_count"), Variants.Num());
	}

	// Each listed set's variant names.
	TSharedPtr<FJsonObject> VariantsBySet = MakeShared<FJsonObject>();
	for (const FString& SetName : Names)
	{
		VariantsBySet->SetArrayField(SetName, mcp_common::StringsToJson(VSets.GetVariantSet(SetName).GetVariantNames()));
	}

	Data->SetStringField(TEXT("prim_path"), PrimPath);
	Data->SetArrayField(TEXT("names"), mcp_common::StringsToJson(Names));
	Data->SetObjectField(TEXT("selections"), SelectionsToJson(Selections));
	Data->SetObjectField(TEXT("variants"), VariantsBySet);
	Data->SetNumberField(TEXT("count"), Names.Num());
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// add_usd_variant_set
// ---------------------------------------------------------------------------
// MCP:add_usd_variant_set - Author a new (empty) variant set on a prim at the requested list position; idempotent (returns already_existed:true when the set is already present). Inverse: remove_usd_variant_set.
//   @param stage_name [string!] - Stage cache key
//   @param prim_path [string!] - SdfPath of the prim that will author the variant set
//   @param variant_set_name [string!] - Name of the variant set to create
//   @param position [string] - One of FrontOfPrependList / BackOfPrependList / FrontOfAppendList / BackOfAppendList (default BackOfPrependList, matching the USD wrapper default)
//   @returns { prim_path: string, variant_set_name: string, created: bool, already_existed: bool, position: string }

TSharedPtr<FJsonObject> HandleAddUsdVariantSet(const TSharedPtr<FJsonObject>& Params)
{
	UE::FUsdStage Stage;
	FString StageName;
	FString PrimPath;
	if (TSharedPtr<FJsonObject> Error = UsdVariant_ResolveStageAndPrimPath(Params, Stage, StageName, PrimPath))
	{
		return Error;
	}

	FString VariantSetName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("variant_set_name"), VariantSetName)) { return Missing; }

	FString PositionStr = TEXT("BackOfPrependList");
	Params->TryGetStringField(TEXT("position"), PositionStr);
	const EUsdListPosition Position = UEAgent::UsdCommon::ParseListPosition(PositionStr);

	UE::FUsdPrim Prim;
	if (TSharedPtr<FJsonObject> Error = UsdVariant_FindPrim(Stage, StageName, PrimPath, Prim))
	{
		return Error;
	}

	UE::FUsdVariantSets VSets = Prim.GetVariantSets();

	// Idempotency: HasVariantSet guard. If the set is already present,
	// return already_existed:true WITHOUT calling AddVariantSet again — the
	// USD wrapper's AddVariantSet is destructive when called twice (it
	// re-orders the list op on the same name).
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("prim_path"), PrimPath);
	Data->SetStringField(TEXT("variant_set_name"), VariantSetName);
	Data->SetStringField(TEXT("position"), PositionStr);

	if (VSets.HasVariantSet(VariantSetName))
	{
		Data->SetBoolField(TEXT("created"), false);
		Data->SetBoolField(TEXT("already_existed"), true);
		return mcp_common::MakeOkReply(Data);
	}

	// The new variant set lands next to the prim spec.
	UsdVariant_TargetStrongestPrimLayer(Stage, Prim);

	UE::FUsdVariantSet NewSet = VSets.AddVariantSet(VariantSetName, Position);
	Data->SetBoolField(TEXT("created"), NewSet.IsValid());
	Data->SetBoolField(TEXT("already_existed"), false);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// add_usd_variant
// ---------------------------------------------------------------------------
// MCP:add_usd_variant - Add a new variant to an existing variant set at the requested list position; idempotent (returns already_existed:true when the variant is already present). Inverse: remove_usd_variant.
//   @param stage_name [string!] - Stage cache key
//   @param prim_path [string!] - SdfPath of the prim that authors the variant set
//   @param variant_set_name [string!] - Variant set that will receive the new variant
//   @param variant_name [string!] - Name of the variant to add
//   @param position [string] - One of FrontOfPrependList / BackOfPrependList / FrontOfAppendList / BackOfAppendList (default BackOfPrependList)
//   @returns { prim_path: string, variant_set_name: string, variant_name: string, added: bool, already_existed: bool, position: string }

TSharedPtr<FJsonObject> HandleAddUsdVariant(const TSharedPtr<FJsonObject>& Params)
{
	UE::FUsdStage Stage;
	FString StageName;
	FString PrimPath;
	if (TSharedPtr<FJsonObject> Error = UsdVariant_ResolveStageAndPrimPath(Params, Stage, StageName, PrimPath))
	{
		return Error;
	}

	FString VariantSetName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("variant_set_name"), VariantSetName)) { return Missing; }

	FString VariantName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("variant_name"), VariantName)) { return Missing; }

	FString PositionStr = TEXT("BackOfPrependList");
	Params->TryGetStringField(TEXT("position"), PositionStr);
	const EUsdListPosition Position = UEAgent::UsdCommon::ParseListPosition(PositionStr);

	UE::FUsdPrim Prim;
	if (TSharedPtr<FJsonObject> Error = UsdVariant_FindPrim(Stage, StageName, PrimPath, Prim))
	{
		return Error;
	}

	UE::FUsdVariantSets VSets = Prim.GetVariantSets();
	UE::FUsdVariantSet VSet;
	if (TSharedPtr<FJsonObject> Error = UsdVariant_GetVariantSet(VSets, VariantSetName, PrimPath, TEXT(" (add_usd_variant_set first)"), VSet))
	{
		return Error;
	}

	// Idempotency: explicit contains check before AddVariant. The wrapper's
	// HasAuthoredVariant is also fine here — it returns true when the variant
	// has been authored in ANY spec, which is what we care about.
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("prim_path"), PrimPath);
	Data->SetStringField(TEXT("variant_set_name"), VariantSetName);
	Data->SetStringField(TEXT("variant_name"), VariantName);
	Data->SetStringField(TEXT("position"), PositionStr);

	if (VSet.HasAuthoredVariant(VariantName))
	{
		Data->SetBoolField(TEXT("added"), false);
		Data->SetBoolField(TEXT("already_existed"), true);
		return mcp_common::MakeOkReply(Data);
	}

	UsdVariant_TargetStrongestPrimLayer(Stage, Prim);

	const bool Added = VSet.AddVariant(VariantName, Position);
	Data->SetBoolField(TEXT("added"), Added);
	Data->SetBoolField(TEXT("already_existed"), false);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_usd_variant_selection
// ---------------------------------------------------------------------------
// MCP:set_usd_variant_selection - Select the active variant of one variant set on a prim of a cached USD stage, so the prim composes that variant's opinions; set is the engine's answer. Read the sets, their variants and current selections with list_usd_variant_sets first; create them with add_usd_variant_set and add_usd_variant; clear_usd_variant_selection removes the selection instead. The selection is authored on the stage's current edit target layer (set_usd_edit_target) and stays in memory until save_usd_stage; no undo step. Idempotent: selecting the selected variant again changes nothing. The variant name is not checked against the set's variants, so name one list_usd_variant_sets reports. An unknown stage, a prim that does not exist, or a variant set the prim does not author is an error, checked before anything is written (the guard also keeps the engine's variant-set handle from going invalid).
//   @param stage_name [string!] - Name the stage was registered under by open_usd_stage or create_usd_stage (list_usd_stages shows it), or the end of a cached stage's root layer identifier, case-insensitive, e.g. "Shot010"
//   @param prim_path [string!] - Absolute SdfPath of the existing prim that authors the variant set, e.g. "/World/Car"
//   @param variant_set_name [string!] - Variant set to select a variant from, as list_usd_variant_sets reports it, e.g. "color"
//   @param variant_name [string!] - Variant to activate, e.g. "red" (must already be present — use add_usd_variant first)
//   @returns { prim_path: string, variant_set_name: string, variant_name: string, set: bool }

TSharedPtr<FJsonObject> HandleSetUsdVariantSelection(const TSharedPtr<FJsonObject>& Params)
{
	UE::FUsdStage Stage;
	FString StageName;
	FString PrimPath;
	if (TSharedPtr<FJsonObject> Error = UsdVariant_ResolveStageAndPrimPath(Params, Stage, StageName, PrimPath))
	{
		return Error;
	}

	FString VariantSetName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("variant_set_name"), VariantSetName)) { return Missing; }

	FString VariantName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("variant_name"), VariantName)) { return Missing; }

	UE::FUsdPrim Prim;
	if (TSharedPtr<FJsonObject> Error = UsdVariant_FindPrim(Stage, StageName, PrimPath, Prim))
	{
		return Error;
	}

	// HasVariantSet guard before calling SetSelection — known gotcha:
	// SetSelection on a missing set leaves the prim invalid in pxr::.
	UE::FUsdVariantSets VSets = Prim.GetVariantSets();
	if (!VSets.HasVariantSet(VariantSetName))
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("variant set '%s' not found on prim '%s' (add_usd_variant_set first)"),
				*VariantSetName, *PrimPath));
	}

	// SetSelection lives on FUsdVariantSets (single-shot API across variant
	// sets — it does NOT require constructing an FUsdVariantSet first).
	const bool Set = VSets.SetSelection(VariantSetName, VariantName);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("prim_path"), PrimPath);
	Data->SetStringField(TEXT("variant_set_name"), VariantSetName);
	Data->SetStringField(TEXT("variant_name"), VariantName);
	Data->SetBoolField(TEXT("set"), Set);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// clear_usd_variant_selection
// ---------------------------------------------------------------------------
// MCP:clear_usd_variant_selection - Clear the selected variant of one variant set on a prim of a cached USD stage, so no variant of that set is selected there; the set and its variants stay. It first moves the stage's edit target to the strongest layer that defines the prim and clears the selection on that layer, so a selection authored on another layer can still apply: check with list_usd_variant_sets. The edit stays in memory until save_usd_stage and has no undo step. A repeat call changes nothing more; cleared is the engine's answer. An unknown stage, an invalid or missing prim, or a variant set the prim does not have is an error and nothing is written. To pick another variant instead use set_usd_variant_selection; to delete the set itself, remove_usd_variant_set.
//   @param stage_name [string!] - Name the stage was registered under by open_usd_stage or create_usd_stage (list_usd_stages shows it), or the end of a cached stage's root layer identifier, case-insensitive, e.g. "Shot010"
//   @param prim_path [string!] - Absolute SdfPath of the existing prim that has the variant set, e.g. "/World/Car"
//   @param variant_set_name [string!] - Variant set whose selection is cleared, as list_usd_variant_sets reports it, e.g. "color"
//   @returns { prim_path: string, variant_set_name: string, cleared: bool }

TSharedPtr<FJsonObject> HandleClearUsdVariantSelection(const TSharedPtr<FJsonObject>& Params)
{
	UE::FUsdStage Stage;
	FString StageName;
	FString PrimPath;
	if (TSharedPtr<FJsonObject> Error = UsdVariant_ResolveStageAndPrimPath(Params, Stage, StageName, PrimPath))
	{
		return Error;
	}

	FString VariantSetName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("variant_set_name"), VariantSetName)) { return Missing; }

	UE::FUsdPrim Prim;
	if (TSharedPtr<FJsonObject> Error = UsdVariant_FindPrim(Stage, StageName, PrimPath, Prim))
	{
		return Error;
	}

	// ClearVariantSelection lives on FUsdVariantSet (not FUsdVariantSets) —
	// unlike SetSelection, this API does require constructing the per-set
	// handle. Friend access from VSets is fine.
	UE::FUsdVariantSets VSets = Prim.GetVariantSets();
	UE::FUsdVariantSet VSet;
	if (TSharedPtr<FJsonObject> Error = UsdVariant_GetVariantSet(VSets, VariantSetName, PrimPath, TEXT(" (add_usd_variant_set first)"), VSet))
	{
		return Error;
	}

	// ClearVariantSelection writes to the edit target.
	UsdVariant_TargetStrongestPrimLayer(Stage, Prim);

	const bool Cleared = VSet.ClearVariantSelection();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("prim_path"), PrimPath);
	Data->SetStringField(TEXT("variant_set_name"), VariantSetName);
	Data->SetBoolField(TEXT("cleared"), Cleared);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// remove_usd_variant_set / remove_usd_variant: the inverses of add_usd_variant_set and add_usd_variant. The
// engine's USD wrapper has no remover, so the edit runs in the UEAgentUsdSdk module on the prim's specs
// (SdfPrimSpec::RemoveVariantSet, SdfVariantSetSpec::RemoveVariant).
// ---------------------------------------------------------------------------
// MCP:remove_usd_variant_set - Delete a variant set from a prim of a cached USD stage, as the variant set editor does: on every spec of the prim in the stage's local layer stack (root, sublayers, session layer) the variant set with all its variants goes, with its name in the variant set list and its selection. Inverse of add_usd_variant_set; clear_usd_variant_selection only clears the selection. Opinions authored on a layer outside the local stack (one brought in by a reference or payload) are left as they are and counted in specs_skipped, so the set can still show (still_present). A prim that does not author the set is an honest no-op (removed=false); a missing stage or prim is an error. Irreversible, like every USD composition edit here (no undo); save with save_usd_stage. Read: list_usd_variant_sets.
//   @param stage_name [string!] - Stage cache key
//   @param prim_path [string!] - SdfPath of the prim that authors the variant set
//   @param variant_set_name [string!] - Name of the variant set to delete
//   @returns { prim_path: string, variant_set_name: string, removed: bool, specs_edited: int, specs_skipped: int, still_present: bool }

TSharedPtr<FJsonObject> HandleRemoveUsdVariantSet(const TSharedPtr<FJsonObject>& Params)
{
	UE::FUsdStage Stage;
	FString StageName;
	FString PrimPath;
	if (TSharedPtr<FJsonObject> Error = UsdVariant_ResolveStageAndPrimPath(Params, Stage, StageName, PrimPath))
	{
		return Error;
	}
	FString VariantSetName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("variant_set_name"), VariantSetName)) { return Missing; }

	UE::FUsdPrim Prim;
	if (TSharedPtr<FJsonObject> Error = UsdVariant_FindPrim(Stage, StageName, PrimPath, Prim))
	{
		return Error;
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("prim_path"), PrimPath);
	Data->SetStringField(TEXT("variant_set_name"), VariantSetName);
	if (!Prim.GetVariantSets().HasVariantSet(VariantSetName))
	{
		Data->SetBoolField(TEXT("removed"), false);
		Data->SetNumberField(TEXT("specs_edited"), 0);
		Data->SetNumberField(TEXT("specs_skipped"), 0);
		Data->SetBoolField(TEXT("still_present"), false);
		return mcp_common::MakeOkReply(Data);
	}

	UEAgent::UsdSdk::FVariantEditResult Edit;
	FString EditError;
	if (!UEAgent::UsdSdk::RemoveVariantSet(Stage, PrimPath, VariantSetName, Edit, EditError))
	{
		return mcp_common::MakeErrorReply(FString::Printf(TEXT("Could not remove variant set '%s' from '%s': %s"), *VariantSetName, *PrimPath, *EditError));
	}

	// The re-read, through the wrapper: does the composed prim still have the set?
	const UE::FUsdPrim After = Stage.GetPrimAtPath(UE::FSdfPath(*PrimPath));
	const bool bStillPresent = After.IsValid() && After.GetVariantSets().HasVariantSet(VariantSetName);
	Data->SetBoolField(TEXT("removed"), Edit.SpecsEdited > 0);
	Data->SetNumberField(TEXT("specs_edited"), Edit.SpecsEdited);
	Data->SetNumberField(TEXT("specs_skipped"), Edit.SpecsSkipped);
	Data->SetBoolField(TEXT("still_present"), bStillPresent);
	return mcp_common::MakeOutcomeReply(Data, Edit.SpecsEdited > 0, Edit.SpecsEdited > 0 ? FString() :
		FString::Printf(TEXT("variant set '%s' is authored only on layers outside the stage's local layer stack; nothing was removed"), *VariantSetName));
}

// MCP:remove_usd_variant - Delete one variant from a variant set of a prim of a cached USD stage: on every spec of the prim in the stage's local layer stack the variant spec goes (with the opinions authored inside it), and a selection of that variant is cleared. Inverse of add_usd_variant. Opinions on a layer outside the local stack (a reference or payload) are left as they are and counted in specs_skipped. A set the prim does not have is an error; a variant the set does not have is an honest no-op (removed=false). Irreversible, like every USD composition edit here (no undo); save with save_usd_stage. Read: list_usd_variant_sets; the whole set: remove_usd_variant_set.
//   @param stage_name [string!] - Stage cache key
//   @param prim_path [string!] - SdfPath of the prim that authors the variant set
//   @param variant_set_name [string!] - Variant set that holds the variant
//   @param variant_name [string!] - Name of the variant to delete
//   @returns { prim_path: string, variant_set_name: string, variant_name: string, removed: bool, specs_edited: int, specs_skipped: int, still_present: bool, variant_names: [string] }

TSharedPtr<FJsonObject> HandleRemoveUsdVariant(const TSharedPtr<FJsonObject>& Params)
{
	UE::FUsdStage Stage;
	FString StageName;
	FString PrimPath;
	if (TSharedPtr<FJsonObject> Error = UsdVariant_ResolveStageAndPrimPath(Params, Stage, StageName, PrimPath))
	{
		return Error;
	}
	FString VariantSetName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("variant_set_name"), VariantSetName)) { return Missing; }
	FString VariantName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("variant_name"), VariantName)) { return Missing; }

	UE::FUsdPrim Prim;
	if (TSharedPtr<FJsonObject> Error = UsdVariant_FindPrim(Stage, StageName, PrimPath, Prim))
	{
		return Error;
	}
	UE::FUsdVariantSets VSets = Prim.GetVariantSets();
	UE::FUsdVariantSet VSet;
	if (TSharedPtr<FJsonObject> Error = UsdVariant_GetVariantSet(VSets, VariantSetName, PrimPath, TEXT(""), VSet))
	{
		return Error;
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("prim_path"), PrimPath);
	Data->SetStringField(TEXT("variant_set_name"), VariantSetName);
	Data->SetStringField(TEXT("variant_name"), VariantName);
	if (!VSet.HasAuthoredVariant(VariantName))
	{
		Data->SetBoolField(TEXT("removed"), false);
		Data->SetNumberField(TEXT("specs_edited"), 0);
		Data->SetNumberField(TEXT("specs_skipped"), 0);
		Data->SetBoolField(TEXT("still_present"), false);
		Data->SetArrayField(TEXT("variant_names"), mcp_common::StringsToJson(VSet.GetVariantNames()));
		return mcp_common::MakeOkReply(Data);
	}

	UEAgent::UsdSdk::FVariantEditResult Edit;
	FString EditError;
	if (!UEAgent::UsdSdk::RemoveVariant(Stage, PrimPath, VariantSetName, VariantName, Edit, EditError))
	{
		return mcp_common::MakeErrorReply(FString::Printf(TEXT("Could not remove variant '%s' of set '%s' from '%s': %s"),
			*VariantName, *VariantSetName, *PrimPath, *EditError));
	}

	// The re-read, through the wrapper.
	const UE::FUsdPrim After = Stage.GetPrimAtPath(UE::FSdfPath(*PrimPath));
	TArray<FString> Names;
	bool bStillPresent = false;
	if (After.IsValid() && After.GetVariantSets().HasVariantSet(VariantSetName))
	{
		const UE::FUsdVariantSet SetAfter = After.GetVariantSets().GetVariantSet(VariantSetName);
		Names = SetAfter.GetVariantNames();
		bStillPresent = SetAfter.HasAuthoredVariant(VariantName);
	}
	Data->SetBoolField(TEXT("removed"), Edit.SpecsEdited > 0);
	Data->SetNumberField(TEXT("specs_edited"), Edit.SpecsEdited);
	Data->SetNumberField(TEXT("specs_skipped"), Edit.SpecsSkipped);
	Data->SetBoolField(TEXT("still_present"), bStillPresent);
	Data->SetArrayField(TEXT("variant_names"), mcp_common::StringsToJson(Names));
	return mcp_common::MakeOutcomeReply(Data, Edit.SpecsEdited > 0, Edit.SpecsEdited > 0 ? FString() :
		FString::Printf(TEXT("variant '%s' is authored only on layers outside the stage's local layer stack; nothing was removed"), *VariantName));
}

const McpCommandFamilies::FRegistrar Registration =
{
	{ TEXT("add_usd_variant"), &HandleAddUsdVariant },
	{ TEXT("add_usd_variant_set"), &HandleAddUsdVariantSet },
	{ TEXT("clear_usd_variant_selection"), &HandleClearUsdVariantSelection },
	{ TEXT("list_usd_variant_sets"), &HandleListUsdVariantSets },
	{ TEXT("remove_usd_variant"), &HandleRemoveUsdVariant },
	{ TEXT("remove_usd_variant_set"), &HandleRemoveUsdVariantSet },
	{ TEXT("set_usd_variant_selection"), &HandleSetUsdVariantSelection },
};
} // namespace UEAgentUsdVariantCommands
