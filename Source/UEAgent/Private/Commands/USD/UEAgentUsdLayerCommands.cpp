// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

// UnrealUSDWrapper headless USD API (UE::FUsdStage / FUsdPrim / FSdfLayer /
// FSdfReference / FSdfPayload / FUsdReferences / FUsdPayloads). All
// UNREALUSDWRAPPER_API.
#include "UnrealUSDWrapper.h"
#include "Commands/USD/UEAgentUsdRegistry.h"  // stage registry + helpers shared by the USD handlers
#include "UsdWrappers/UsdStage.h"
#include "UsdWrappers/UsdPrim.h"
#include "UsdWrappers/SdfLayer.h"
#include "UsdWrappers/SdfPath.h"
#include "UsdWrappers/UsdReferences.h"
#include "UsdWrappers/UsdPayloads.h"
#include "UsdWrappers/ForwardDeclarations.h"

/**
 * USD layer-composition commands.
 *
 * Reads and mutates the USD composition graph: layer-stack enumeration, sub-layer
 * insert/remove, references (external/internal), payloads, muting, and the
 * current edit target. Backed by the UnrealUSDWrapper headless API (no UObject,
 * no World, no PIE).
 *
 * Engine APIs (module UnrealUSDWrapper, Public/UsdWrappers/):
 *   - UE::FUsdStage::GetLayerStack(bool) / SetEditTarget(Layer) / MuteAndUnmuteLayers (UsdStage.h)
 *   - UE::FSdfLayer::GetSubLayerPaths / InsertSubLayerPath / RemoveSubLayerPath / Save /
 *     IsDirty / GetIdentifier / GetRealPath / IsMuted (SdfLayer.h)
 *   - UE::FSdfReference (struct: AssetPath + PrimPath + LayerOffset; SdfLayer.h)
 *   - UE::FUsdReferences::AddReference / AddInternalReference / ClearReferences (UsdReferences.h)
 *   - UE::FUsdPayloads::AddPayload (UsdPayloads.h)
 *   - UE::FUsdPrim::GetReferences / GetPayloads (UsdPrim.h)
 *
 * Modules: UnrealUSDWrapper, USDUtilities (Runtime; USD error capture via UEAgentUsdRegistry.h).
 * Plugin:  USDCore (EnabledByDefault:false; UEAgent.uplugin enables it).
 */
namespace UEAgentUsdLayerCommands
{

// MCP-DOMAIN: USD

namespace
{
	// Read a JSON string-array field from Params. Missing / non-array -> empty
	// TArray (NOT an error — handlers decide whether absence is meaningful).
	TArray<FString> UsdLayer_ReadStringArray(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName)
	{
		TArray<FString> Out;
		if (!Params.IsValid()) { return Out; }
		const TArray<TSharedPtr<FJsonValue>>* ArrPtr = nullptr;
		if (!Params->TryGetArrayField(FieldName, ArrPtr) || !ArrPtr) { return Out; }
		Out.Reserve(ArrPtr->Num());
		for (const TSharedPtr<FJsonValue>& V : *ArrPtr)
		{
			if (!V.IsValid()) { continue; }
			FString S;
			if (V->TryGetString(S) && !S.IsEmpty())
			{
				Out.Add(S);
			}
		}
		return Out;
	}

	// The stage registered as StageName (valid or not), or an empty stage when the registry has no such entry.
	UE::FUsdStage UsdLayer_FindStage(const FString& StageName)
	{
		if (UE::FUsdStage* Found = UEAgent::UsdRegistry::GStageNameCache.Find(StageName))
		{
			return *Found;
		}
		return UE::FUsdStage();
	}

	// The prim at PrimPath on the stage registered as StageName; invalid when either does not resolve.
	UE::FUsdPrim UsdLayer_FindPrim(const FString& StageName, const FString& PrimPath)
	{
		UE::FUsdStage Stage = UsdLayer_FindStage(StageName);
		if (!Stage)
		{
			return UE::FUsdPrim();
		}
		return Stage.GetPrimAtPath(UE::FSdfPath(*PrimPath));
	}

	// stage_name and prim_path of the prim-editing commands, both required and non-empty: the error
	// reply for the first one missing, or null.
	TSharedPtr<FJsonObject> UsdLayer_ReadStageAndPrim(const TSharedPtr<FJsonObject>& Params, FString& OutStageName, FString& OutPrimPath)
	{
		if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("stage_name"), OutStageName)) { return Missing; }
		if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("prim_path"), OutPrimPath)) { return Missing; }
		return nullptr;
	}

	// What add_usd_reference and add_usd_payload read: stage, prim and asset (required), then the
	// optional target prim and list position.
	struct FUsdLayerArcRequest
	{
		FString StageName;
		FString PrimPath;
		FString AssetPath;
		FString PrimPathForTarget;
		FString PositionStr;

		// The reply both commands open with: stage, prim, asset and list position (BackOfPrependList when
		// none is given).
		TSharedPtr<FJsonObject> NewResult() const
		{
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("stage_name"), StageName);
			Result->SetStringField(TEXT("prim_path"), PrimPath);
			Result->SetStringField(TEXT("asset_path"), AssetPath);
			Result->SetStringField(TEXT("position"), PositionStr.IsEmpty() ? TEXT("BackOfPrependList") : PositionStr);
			return Result;
		}

		// The prim of the target layer to import: prim_path_for_target, or the layer's pseudo-root '/' when it
		// is empty (the wrapper has no overload without a target SdfPath).
		UE::FSdfPath TargetPath() const
		{
			const FString Target = PrimPathForTarget.IsEmpty() ? TEXT("/") : PrimPathForTarget;
			return UE::FSdfPath(*Target);
		}
	};

	// Fills Out from Params: the error reply for the first required field missing, or null.
	TSharedPtr<FJsonObject> UsdLayer_ReadArcRequest(const TSharedPtr<FJsonObject>& Params, FUsdLayerArcRequest& Out)
	{
		if (TSharedPtr<FJsonObject> Missing = UsdLayer_ReadStageAndPrim(Params, Out.StageName, Out.PrimPath))
		{
			return Missing;
		}
		if (!Params->TryGetStringField(TEXT("asset_path"), Out.AssetPath) || Out.AssetPath.IsEmpty())
		{
			return mcp_common::MakeErrorReply(TEXT("Missing required 'asset_path' parameter"));
		}
		Params->TryGetStringField(TEXT("prim_path_for_target"), Out.PrimPathForTarget);
		Params->TryGetStringField(TEXT("position"), Out.PositionStr);
		return nullptr;
	}

	// After a prim edit that changed nothing (bChanged false) and raised no USD error: when the stage StageName,
	// or the prim at PrimPath in it, does not resolve, Result is marked failed with the reason and this returns
	// true. It tells "stage/prim not found" apart from "the wrapper returned false".
	bool UsdLayer_PrimEditFailed(bool bChanged, const FString& StageName, const FString& PrimPath,
		const TSharedPtr<FJsonObject>& Result)
	{
		if (bChanged || Result->HasField(TEXT("errors")))
		{
			return false;
		}
		const UE::FUsdStage Stage = UsdLayer_FindStage(StageName);
		if (!Stage)
		{
			UEAgent::UsdCommon::FailReply(Result, FString::Printf(TEXT("Stage '%s' could not be resolved"), *StageName));
			return true;
		}
		if (!Stage.GetPrimAtPath(UE::FSdfPath(*PrimPath)))
		{
			UEAgent::UsdCommon::FailReply(Result, FString::Printf(TEXT("Prim '%s' not found in stage '%s'"), *PrimPath, *StageName));
			return true;
		}
		return false;
	}
}

// ---------------------------------------------------------------------------
// list_usd_stage_layers
// ---------------------------------------------------------------------------
// MCP:list_usd_stage_layers - List the layer stack of a USD stage opened with open_usd_stage or create_usd_stage, read-only: each layer's index in the stack, identifier, real_path, is_muted, dirty and sublayer paths, plus count and any captured USD errors; with layer_index only that layer. An unregistered stage_name, a layer_index outside the stack or an invalid layer at it is an error.
//   @param stage_name [string!] - Name the stage was registered under via open_usd_stage or create_usd_stage.
//   @param include_session_layers [bool] - When true (default) the returned stack also includes the stage's session/auto-generated layers; pass false to get only the persistent root + sublayers. layer_index counts in this stack.
//   @param layer_index - Optional 0-based index into the layer stack (the root layer is 0); report only that layer
//   @returns { stage_name: string, include_session_layers: bool, count: int, layers: [{ index: int, identifier: string, real_path: string, is_muted: bool, dirty: bool, sub_layer_paths: [string], sub_layer_count: int }], layer_index?: int, errors: [string]? }

TSharedPtr<FJsonObject> HandleListUsdStageLayers(const TSharedPtr<FJsonObject>& Params)
{
	FString StageName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("stage_name"), StageName)) { return Missing; }

	bool bIncludeSession = true;
	Params->TryGetBoolField(TEXT("include_session_layers"), bIncludeSession);
	int32 OnlyIndex = INDEX_NONE;
	const bool bIndexIsNumber = Params->TryGetNumberField(TEXT("layer_index"), OnlyIndex);
	const bool bOneLayer = Params->HasField(TEXT("layer_index"));
	if (bOneLayer && !bIndexIsNumber)
	{
		return mcp_common::MakeErrorReply(TEXT("layer_index must be an integer"));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("stage_name"), StageName);
	Result->SetBoolField(TEXT("include_session_layers"), bIncludeSession);

	TArray<UE::FSdfLayer> Stack;
	UEAgent::UsdCommon::CaptureUsdErrors(
		[&Stack, &StageName, bIncludeSession]()
		{
			if (UE::FUsdStage Stage = UsdLayer_FindStage(StageName))
			{
				Stack = Stage.GetLayerStack(bIncludeSession);
			}
		},
		Result);

	if (Stack.Num() == 0)
	{
		// Stack could be empty either because the stage wasn't found OR because
		// GetLayerStack genuinely returned an empty array (rare; a stage with no
		// root layer is malformed). Distinguish via the registry lookup.
		if (!UsdLayer_FindStage(StageName))
		{
			return UEAgent::UsdCommon::FailReply(Result,
				FString::Printf(TEXT("Stage '%s' could not be resolved (not in GStageNameCache)"), *StageName));
		}
	}

	if (bOneLayer)
	{
		Result->SetNumberField(TEXT("layer_index"), OnlyIndex);
		if (OnlyIndex < 0 || OnlyIndex >= Stack.Num())
		{
			return UEAgent::UsdCommon::FailReply(Result,
				FString::Printf(TEXT("layer_index %d out of range [0, %d)"), OnlyIndex, Stack.Num()));
		}
		if (!Stack[OnlyIndex])
		{
			return UEAgent::UsdCommon::FailReply(Result, FString::Printf(TEXT("layer at index %d is invalid"), OnlyIndex));
		}
	}

	// index is the position in GetLayerStack(include_session_layers), so an invalid layer leaves a gap.
	TArray<TSharedPtr<FJsonValue>> LayerArr;
	for (int32 Index = 0; Index < Stack.Num(); ++Index)
	{
		const UE::FSdfLayer& Layer = Stack[Index];
		if (!Layer || (bOneLayer && Index != OnlyIndex)) { continue; }

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetNumberField(TEXT("index"), Index);
		Entry->SetStringField(TEXT("identifier"), Layer.GetIdentifier());
		Entry->SetStringField(TEXT("real_path"),  Layer.GetRealPath());
		Entry->SetBoolField(TEXT("is_muted"),    Layer.IsMuted());
		Entry->SetBoolField(TEXT("dirty"),       Layer.IsDirty());
		const TArray<TSharedPtr<FJsonValue>> SubArr = mcp_common::StringsToJson(Layer.GetSubLayerPaths());
		Entry->SetArrayField(TEXT("sub_layer_paths"), SubArr);
		Entry->SetNumberField(TEXT("sub_layer_count"), SubArr.Num());
		LayerArr.Add(MakeShared<FJsonValueObject>(Entry));
	}

	Result->SetArrayField(TEXT("layers"), LayerArr);
	Result->SetNumberField(TEXT("count"), LayerArr.Num());
	return mcp_common::MakeOkReply(Result);
}

// ---------------------------------------------------------------------------
// insert_usd_sublayer
// ---------------------------------------------------------------------------
// MCP:insert_usd_sublayer - Add a sublayer path to the stage's root layer's composition stack (idempotent — re-running with the same path returns already_present without erroring).
//   @param stage_name [string!] - Name the stage was registered under.
//   @param sublayer_path [string!] - Path or USD identifier of the layer to insert (e.g. an absolute .usd path or a "//sub.usd" relative identifier).
//   @param index [integer] - 0-based insertion position; -1 (default) appends to the back of the prepend list.
//   @returns { stage_name: string, sublayer_path: string, index: int, inserted: bool, already_present: bool, new_count: int, saved: bool, errors: [string]? }

TSharedPtr<FJsonObject> HandleInsertUsdSublayer(const TSharedPtr<FJsonObject>& Params)
{
	FString StageName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("stage_name"), StageName)) { return Missing; }

	FString SublayerPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sublayer_path"), SublayerPath)) { return Missing; }

	int32 Index = -1;
	Params->TryGetNumberField(TEXT("index"), Index);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("stage_name"), StageName);
	Result->SetStringField(TEXT("sublayer_path"), SublayerPath);
	Result->SetNumberField(TEXT("index"), Index);

	bool bInserted = false;
	bool bAlreadyPresent = false;
	int32 NewCount = 0;
	bool bSaved = false;

	UEAgent::UsdCommon::CaptureUsdErrors(
		[&StageName, &SublayerPath, Index, &bInserted, &bAlreadyPresent, &NewCount, &bSaved]()
		{
			UE::FUsdStage Stage = UsdLayer_FindStage(StageName);
			if (!Stage) { return; }

			UE::FSdfLayer Root = Stage.GetRootLayer();
			if (!Root) { return; }

			const TArray<FString> Current = Root.GetSubLayerPaths();
			if (Current.Contains(SublayerPath))
			{
				// Idempotent: the path is already there — no mutation, no save.
				bAlreadyPresent = true;
				NewCount = Current.Num();
				return;
			}

			Root.InsertSubLayerPath(SublayerPath, Index);
			bInserted = true;
			NewCount = Root.GetSubLayerPaths().Num();
			// Save(false) is idempotent — for an in-memory stage the root layer
			// is anonymous, so Save() is a no-op (returns false, NOT an error).
			bSaved = Root.Save(/*bForce*/ false);
		},
		Result);

	if (!Result->HasField(TEXT("errors")) && !bInserted && !bAlreadyPresent)
	{
		// Stage resolved to nothing — flag honestly rather than returning
		// success with empty values.
		return UEAgent::UsdCommon::FailReply(Result,
			FString::Printf(TEXT("Stage '%s' could not be resolved or root layer invalid"), *StageName));
	}

	Result->SetBoolField(TEXT("inserted"), bInserted);
	Result->SetBoolField(TEXT("already_present"), bAlreadyPresent);
	Result->SetNumberField(TEXT("new_count"), NewCount);
	return mcp_common::MakeSavedReply(Result, bSaved, TEXT("insert_usd_sublayer"));
}

// ---------------------------------------------------------------------------
// remove_usd_sublayer
// ---------------------------------------------------------------------------
// MCP:remove_usd_sublayer - Remove a sublayer by index from the stage's root layer's composition stack.
//   @param stage_name [string!] - Name the stage was registered under.
//   @param sublayer_index [integer!] - 0-based index into GetSubLayerPaths() of the root layer.
//   @returns { stage_name: string, sublayer_index: int, removed: bool, new_count: int, errors: [string]? }

TSharedPtr<FJsonObject> HandleRemoveUsdSublayer(const TSharedPtr<FJsonObject>& Params)
{
	FString StageName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("stage_name"), StageName)) { return Missing; }

	int32 SublayerIndex = 0;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("sublayer_index"), SublayerIndex)) { return Missing; }

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("stage_name"), StageName);
	Result->SetNumberField(TEXT("sublayer_index"), SublayerIndex);

	bool bRemoved = false;
	int32 NewCount = 0;

	UEAgent::UsdCommon::CaptureUsdErrors(
		[&StageName, SublayerIndex, &bRemoved, &NewCount]()
		{
			UE::FUsdStage Stage = UsdLayer_FindStage(StageName);
			if (!Stage) { return; }

			UE::FSdfLayer Root = Stage.GetRootLayer();
			if (!Root) { return; }

			const TArray<FString> Current = Root.GetSubLayerPaths();
			if (SublayerIndex < 0 || SublayerIndex >= Current.Num())
			{
				return; // surfaced via the post-call branch below
			}

			Root.RemoveSubLayerPath(SublayerIndex);
			bRemoved = true;
			NewCount = Root.GetSubLayerPaths().Num();
		},
		Result);

	if (!bRemoved)
	{
		// Best-effort: report whether the stage was unresolved vs the index was OOR.
		return UEAgent::UsdCommon::FailReply(Result, !UsdLayer_FindStage(StageName)
			? FString::Printf(TEXT("Stage '%s' could not be resolved"), *StageName)
			: FString::Printf(TEXT("sublayer_index %d out of range"), SublayerIndex));
	}

	Result->SetBoolField(TEXT("removed"), bRemoved);
	Result->SetNumberField(TEXT("new_count"), NewCount);
	return mcp_common::MakeOkReply(Result);
}

// ---------------------------------------------------------------------------
// add_usd_reference
// ---------------------------------------------------------------------------
// MCP:add_usd_reference - Add an external reference composition arc to a prim (asset_path -> target prim, optionally re-targeted by prim_path_for_target).
//   @param stage_name [string!] - Name the stage was registered under.
//   @param prim_path [string!] - Prim that owns the reference (target of the composition op on the consumer side).
//   @param asset_path [string!] - Identifier of the layer to reference (absolute file path or USD-style relative identifier).
//   @param prim_path_for_target [string] - Prim inside the referenced layer to import. Empty (default) means the referenced layer's pseudo-root '/'. The default matches the common "import the whole layer" use case.
//   @param position [string] - One of "FrontOfPrependList" / "BackOfPrependList" / "FrontOfAppendList" / "BackOfAppendList" (default BackOfPrependList — pxr::'s default for the strongest-anonymous opinion).
//   @returns { stage_name: string, prim_path: string, asset_path: string, added: bool, position: string, errors: [string]? }

TSharedPtr<FJsonObject> HandleAddUsdReference(const TSharedPtr<FJsonObject>& Params)
{
	FUsdLayerArcRequest Request;
	if (TSharedPtr<FJsonObject> Missing = UsdLayer_ReadArcRequest(Params, Request))
	{
		return Missing;
	}
	const EUsdListPosition Pos = UEAgent::UsdCommon::ParseListPosition(Request.PositionStr);

	TSharedPtr<FJsonObject> Result = Request.NewResult();

	bool bAdded = false;

	UEAgent::UsdCommon::CaptureUsdErrors(
		[&Request, Pos, &bAdded]()
		{
			UE::FUsdPrim Prim = UsdLayer_FindPrim(Request.StageName, Request.PrimPath);
			if (!Prim) { return; }

			const UE::FSdfReference Ref(Request.AssetPath, Request.TargetPath());
			bAdded = Prim.GetReferences().AddReference(Ref, Pos);
		},
		Result);

	if (UsdLayer_PrimEditFailed(bAdded, Request.StageName, Request.PrimPath, Result))
	{
		return Result;
	}

	Result->SetBoolField(TEXT("added"), bAdded);
	return mcp_common::MakeOkReply(Result);
}

// ---------------------------------------------------------------------------
// add_usd_internal_reference
// ---------------------------------------------------------------------------
// MCP:add_usd_internal_reference - Add an internal reference composition arc (references another prim inside the SAME stage; no asset_path needed).
//   @param stage_name [string!] - Name the stage was registered under.
//   @param prim_path [string!] - Prim that owns the reference (the consumer).
//   @param target_prim_path [string!] - Prim inside the same stage to reference.
//   @param position [string] - List position (same vocabulary as add_usd_reference). Default "BackOfPrependList".
//   @returns { stage_name: string, prim_path: string, target_prim_path: string, added: bool, position: string, errors: [string]? }

TSharedPtr<FJsonObject> HandleAddUsdInternalReference(const TSharedPtr<FJsonObject>& Params)
{
	FString StageName;
	FString PrimPath;
	if (TSharedPtr<FJsonObject> Missing = UsdLayer_ReadStageAndPrim(Params, StageName, PrimPath))
	{
		return Missing;
	}

	FString TargetPrimPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("target_prim_path"), TargetPrimPath)) { return Missing; }

	FString PositionStr;
	Params->TryGetStringField(TEXT("position"), PositionStr);
	const EUsdListPosition Pos = UEAgent::UsdCommon::ParseListPosition(PositionStr);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("stage_name"), StageName);
	Result->SetStringField(TEXT("prim_path"), PrimPath);
	Result->SetStringField(TEXT("target_prim_path"), TargetPrimPath);
	Result->SetStringField(TEXT("position"), PositionStr.IsEmpty() ? TEXT("BackOfPrependList") : PositionStr);

	bool bAdded = false;

	UEAgent::UsdCommon::CaptureUsdErrors(
		[&StageName, &PrimPath, &TargetPrimPath, Pos, &bAdded]()
		{
			UE::FUsdPrim Prim = UsdLayer_FindPrim(StageName, PrimPath);
			if (!Prim) { return; }

			// AddInternalReference takes the target SdfPath; LayerOffset
			// defaults to (offset=0, scale=1) so we omit it.
			bAdded = Prim.GetReferences().AddInternalReference(
				UE::FSdfPath(*TargetPrimPath),
				{},
				Pos);
		},
		Result);

	if (UsdLayer_PrimEditFailed(bAdded, StageName, PrimPath, Result))
	{
		return Result;
	}

	Result->SetBoolField(TEXT("added"), bAdded);
	return mcp_common::MakeOkReply(Result);
}

// ---------------------------------------------------------------------------
// clear_usd_references
// ---------------------------------------------------------------------------
// MCP:clear_usd_references - Remove every reference from a prim's reference list (irreversible — there is no "undo" in USD composition authoring).
//   @param stage_name [string!] - Name the stage was registered under.
//   @param prim_path [string!] - Prim whose references should be cleared.
//   @returns { stage_name: string, prim_path: string, cleared: bool, errors: [string]? }

TSharedPtr<FJsonObject> HandleClearUsdReferences(const TSharedPtr<FJsonObject>& Params)
{
	FString StageName;
	FString PrimPath;
	if (TSharedPtr<FJsonObject> Missing = UsdLayer_ReadStageAndPrim(Params, StageName, PrimPath))
	{
		return Missing;
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("stage_name"), StageName);
	Result->SetStringField(TEXT("prim_path"), PrimPath);

	bool bCleared = false;

	UEAgent::UsdCommon::CaptureUsdErrors(
		[&StageName, &PrimPath, &bCleared]()
		{
			UE::FUsdPrim Prim = UsdLayer_FindPrim(StageName, PrimPath);
			if (!Prim) { return; }

			bCleared = Prim.GetReferences().ClearReferences();
		},
		Result);

	if (UsdLayer_PrimEditFailed(bCleared, StageName, PrimPath, Result))
	{
		return Result;
	}

	Result->SetBoolField(TEXT("cleared"), bCleared);
	return mcp_common::MakeOkReply(Result);
}

// ---------------------------------------------------------------------------
// add_usd_payload
// ---------------------------------------------------------------------------
// MCP:add_usd_payload - Add a payload composition arc to a prim (load-on-demand; payloads are NOT loaded by the stage unless an explicit Load rule says so).
//   @param stage_name [string!] - Name the stage was registered under.
//   @param prim_path [string!] - Prim that owns the payload.
//   @param asset_path [string!] - Identifier of the layer containing the payload.
//   @param prim_path_for_target [string] - Prim inside the referenced layer to import. Empty (default) means '/'.
//   @param position [string] - List position (same vocabulary as add_usd_reference). Default "BackOfPrependList".
//   @returns { stage_name: string, prim_path: string, asset_path: string, added: bool, position: string, errors: [string]? }

TSharedPtr<FJsonObject> HandleAddUsdPayload(const TSharedPtr<FJsonObject>& Params)
{
	FUsdLayerArcRequest Request;
	if (TSharedPtr<FJsonObject> Missing = UsdLayer_ReadArcRequest(Params, Request))
	{
		return Missing;
	}
	const EUsdListPosition Pos = UEAgent::UsdCommon::ParseListPosition(Request.PositionStr);

	TSharedPtr<FJsonObject> Result = Request.NewResult();

	bool bAdded = false;

	UEAgent::UsdCommon::CaptureUsdErrors(
		[&Request, Pos, &bAdded]()
		{
			UE::FUsdPrim Prim = UsdLayer_FindPrim(Request.StageName, Request.PrimPath);
			if (!Prim) { return; }

			const UE::FSdfPayload Payload(Request.AssetPath, Request.TargetPath());
			bAdded = Prim.GetPayloads().AddPayload(Payload, Pos);
		},
		Result);

	if (UsdLayer_PrimEditFailed(bAdded, Request.StageName, Request.PrimPath, Result))
	{
		return Result;
	}

	Result->SetBoolField(TEXT("added"), bAdded);
	return mcp_common::MakeOkReply(Result);
}

// ---------------------------------------------------------------------------
// mute_usd_layer
// ---------------------------------------------------------------------------
// MCP:mute_usd_layer - Toggle mute state on a set of layers by their identifiers (mute + unmute are passed in the same call — pxr:: applies them atomically).
//   @param stage_name [string!] - Name the stage was registered under.
//   @param mute_paths [array] - Identifiers of layers to mute. Empty/missing is allowed (no-op on the mute side).
//   @param unmute_paths [array] - Identifiers of layers to unmute. Empty/missing is allowed (no-op on the unmute side).
//   @returns { stage_name: string, muted: [string], unmuted: [string], errors: [string]? }

TSharedPtr<FJsonObject> HandleMuteUsdLayer(const TSharedPtr<FJsonObject>& Params)
{
	FString StageName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("stage_name"), StageName)) { return Missing; }

	const TArray<FString> MutePaths   = UsdLayer_ReadStringArray(Params, TEXT("mute_paths"));
	const TArray<FString> UnmutePaths = UsdLayer_ReadStringArray(Params, TEXT("unmute_paths"));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("stage_name"), StageName);
	Result->SetArrayField(TEXT("muted"),   mcp_common::StringsToJson(MutePaths));
	Result->SetArrayField(TEXT("unmuted"), mcp_common::StringsToJson(UnmutePaths));

	bool bStageOk = false;
	UEAgent::UsdCommon::CaptureUsdErrors(
		[&StageName, &MutePaths, &UnmutePaths, &bStageOk]()
		{
			UE::FUsdStage Stage = UsdLayer_FindStage(StageName);
			if (!Stage) { return; }

			// MuteAndUnmuteLayers operates on the full list atomically — caller
			// is responsible for composing the desired mute+unmute set in ONE
			// call (per the pxr:: API contract).
			Stage.MuteAndUnmuteLayers(MutePaths, UnmutePaths);
			bStageOk = true;
		},
		Result);

	if (!bStageOk && !Result->HasField(TEXT("errors")))
	{
		return UEAgent::UsdCommon::FailReply(Result, FString::Printf(TEXT("Stage '%s' could not be resolved"), *StageName));
	}

	return mcp_common::MakeOkReply(Result);
}

// ---------------------------------------------------------------------------
// set_usd_edit_target
// ---------------------------------------------------------------------------
// MCP:set_usd_edit_target - Point the stage's edit target at a specific layer in the stack (mutations after this call land on the chosen layer, not the root).
//   @param stage_name [string!] - Name the stage was registered under.
//   @param layer_index [integer] - 0-based index into GetLayerStack(true) of the new edit target. Defaults to 0 (the root layer).
//   @returns { stage_name: string, layer_index: int, edit_target: string, edit_target_real_path: string, errors: [string]? }

TSharedPtr<FJsonObject> HandleSetUsdEditTarget(const TSharedPtr<FJsonObject>& Params)
{
	FString StageName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("stage_name"), StageName)) { return Missing; }

	int32 LayerIndex = 0;
	Params->TryGetNumberField(TEXT("layer_index"), LayerIndex);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("stage_name"), StageName);
	Result->SetNumberField(TEXT("layer_index"), LayerIndex);

	bool bApplied = false;

	UEAgent::UsdCommon::CaptureUsdErrors(
		[&StageName, LayerIndex, &bApplied]()
		{
			UE::FUsdStage Stage = UsdLayer_FindStage(StageName);
			if (!Stage) { return; }

			const TArray<UE::FSdfLayer> Stack = Stage.GetLayerStack(true);
			if (LayerIndex < 0 || LayerIndex >= Stack.Num()) { return; }

			const UE::FSdfLayer Layer = Stack[LayerIndex];
			if (!Layer) { return; }

			Stage.SetEditTarget(Layer);
			bApplied = true;
		},
		Result);

	if (!bApplied)
	{
		UE::FUsdStage Stage = UsdLayer_FindStage(StageName);
		if (!Stage)
		{
			return UEAgent::UsdCommon::FailReply(Result, FString::Printf(TEXT("Stage '%s' could not be resolved"), *StageName));
		}
		const TArray<UE::FSdfLayer> Stack = Stage.GetLayerStack(true);
		return UEAgent::UsdCommon::FailReply(Result,
			FString::Printf(TEXT("layer_index %d out of range [0, %d)"), LayerIndex, Stack.Num()));
	}

	// Report the chosen target — re-resolve so the LLM can pivot on the new
	// edit target's identifier without a follow-up call.
	if (UE::FUsdStage Stage = UsdLayer_FindStage(StageName))
	{
		const TArray<UE::FSdfLayer> Stack = Stage.GetLayerStack(true);
		if (LayerIndex >= 0 && LayerIndex < Stack.Num() && Stack[LayerIndex])
		{
			Result->SetStringField(TEXT("edit_target"),           Stack[LayerIndex].GetIdentifier());
			Result->SetStringField(TEXT("edit_target_real_path"), Stack[LayerIndex].GetRealPath());
		}
	}

	return mcp_common::MakeOkReply(Result);
}

const McpCommandFamilies::FRegistrar Registration =
{
	{ TEXT("add_usd_internal_reference"), &HandleAddUsdInternalReference },
	{ TEXT("add_usd_payload"), &HandleAddUsdPayload },
	{ TEXT("add_usd_reference"), &HandleAddUsdReference },
	{ TEXT("clear_usd_references"), &HandleClearUsdReferences },
	{ TEXT("insert_usd_sublayer"), &HandleInsertUsdSublayer },
	{ TEXT("list_usd_stage_layers"), &HandleListUsdStageLayers },
	{ TEXT("mute_usd_layer"), &HandleMuteUsdLayer },
	{ TEXT("remove_usd_sublayer"), &HandleRemoveUsdSublayer },
	{ TEXT("set_usd_edit_target"), &HandleSetUsdEditTarget },
};
} // namespace UEAgentUsdLayerCommands
