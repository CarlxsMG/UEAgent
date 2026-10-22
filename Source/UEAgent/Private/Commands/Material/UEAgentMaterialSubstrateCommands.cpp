// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

#include "RenderUtils.h"                 // Substrate::IsSubstrateEnabled (RENDERCORE_API)
#include "HAL/IConsoleManager.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionSubstrate.h"
#include "MaterialGraph/MaterialGraphNode.h"   // UMaterialGraphNode::GetShortenPinName -- the pin names ConnectMaterialExpressions matches
#include "SceneTypes.h"                  // EMaterialProperty / MP_FrontMaterial
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"

/**
 * Substrate (Strata) material authoring commands.
 *
 * HONEST NOTE: Substrate may be DISABLED in the project (r.Substrate=0). Enabling it
 * requires editing the project/engine config and RESTARTING the editor. Substrate
 * node authoring (CreateMaterialExpression) still SUCCEEDS while it is disabled,
 * but the resulting material will not compile/render until Substrate is on. These
 * commands perform REAL node authoring and report the live Substrate state so the
 * caller knows whether the authored graph will actually compile.
 *
 * Commands:
 *   - is_substrate_enabled           : report Substrate::IsSubstrateEnabled() + r.Substrate cvar
 *                                          (always works)
 *   - create_substrate_slab_node         : add a SubstrateSlabBSDF node to a UMaterial asset
 *   - set_substrate_slab_scalar          : drive a slab pin (e.g. Roughness) with a Constant node
 *   - connect_substrate_slab_to_material : connect slab output to the material Front Material input
 *                                          (gated by Substrate)
 */
namespace UEAgentMaterialSubstrateCommands
{

// MCP-DOMAIN: Material

namespace
{
	/** Find the first SubstrateSlabBSDF node already present on the material (idempotency). */
	UMaterialExpressionSubstrateSlabBSDF* FindSlabNode(UMaterial* Material)
	{
		if (!Material)
		{
			return nullptr;
		}
		for (UMaterialExpression* Expr : Material->GetExpressions())
		{
			if (UMaterialExpressionSubstrateSlabBSDF* Slab = Cast<UMaterialExpressionSubstrateSlabBSDF>(Expr))
			{
				return Slab;
			}
		}
		return nullptr;
	}

	/** The UMaterial at MaterialPath: null once OutMaterial holds it, otherwise the error reply. */
	TSharedPtr<FJsonObject> Substrate_LoadMaterial(const FString& MaterialPath, UMaterial*& OutMaterial)
	{
		OutMaterial = mcp_common::LoadAssetOrSoftPath<UMaterial>(MaterialPath);
		if (!OutMaterial)
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("Cannot load material asset: %s"), *MaterialPath));
		}
		return nullptr;
	}

	/** Substrate_LoadMaterial, then the material's slab node: null once both are found, otherwise the error reply. */
	TSharedPtr<FJsonObject> Substrate_LoadMaterialSlab(const FString& MaterialPath, UMaterial*& OutMaterial,
		UMaterialExpressionSubstrateSlabBSDF*& OutSlab)
	{
		if (TSharedPtr<FJsonObject> LoadError = Substrate_LoadMaterial(MaterialPath, OutMaterial))
		{
			return LoadError;
		}
		OutSlab = FindSlabNode(OutMaterial);
		if (!OutSlab)
		{
			return mcp_common::MakeErrorReply(
				TEXT("No SubstrateSlabBSDF node found on material; call create_substrate_slab_node first"));
		}
		return nullptr;
	}

	/** Save the material's package to disk so the authored node persists across runs. */
	bool SaveMaterialPackage(UMaterial* Material)
	{
		return mcp_common::SaveAssetPackage(Material ? Material->GetPackage() : nullptr, Material);
	}

	/**
	 * The input of Expression named PinName, matched the way UMaterialEditingLibrary::ConnectMaterialExpressions
	 * matches it (each input's name shortened by UMaterialGraphNode::GetShortenPinName, compared as an FName, so
	 * without case); null when there is none, with OutNames listing the names it has.
	 */
	FExpressionInput* Substrate_FindInput(UMaterialExpression* Expression, const FString& PinName, TArray<FString>& OutNames)
	{
		const FName Wanted(*PinName);
		for (FExpressionInputIterator It{ Expression }; It; ++It)
		{
			const FName InputName = UMaterialGraphNode::GetShortenPinName(Expression->GetInputName(It.Index));
			if (InputName == Wanted)
			{
				return It.Input;
			}
			if (!InputName.IsNone())
			{
				OutNames.Add(InputName.ToString());
			}
		}
		return nullptr;
	}

	/** Report the live Substrate state into a Json object. Never fails. */
	void FillSubstrateState(const TSharedPtr<FJsonObject>& Obj)
	{
		const bool bEnabled = Substrate::IsSubstrateEnabled();
		Obj->SetBoolField(TEXT("substrate_enabled"), bEnabled);

		int32 CVarValue = -1;
		if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Substrate")))
		{
			CVarValue = CVar->GetInt();
		}
		Obj->SetNumberField(TEXT("r_substrate_cvar"), CVarValue);

		if (!bEnabled)
		{
			Obj->SetStringField(TEXT("note"),
				TEXT("Substrate is DISABLED (r.Substrate=0). Node authoring succeeds, but the ")
				TEXT("material will not compile/render until r.Substrate is enabled and the editor restarts."));
		}
	}
}

// ---------------------------------------------------------------------------
// is_substrate_enabled  (ALWAYS works)
// ---------------------------------------------------------------------------
// MCP:is_substrate_enabled - Report whether Substrate (Strata) materials are enabled in this editor.  Always works. Returns substrate_enabled (from Substrate::IsSubstrateEnabled), the r.Substrate cvar value, and a note when disabled. Enabling Substrate requires editing config and restarting the editor.
//   @renamed-from detect_substrate_enabled
//   @returns { substrate_enabled: bool, r_substrate_cvar: int, note?: string, detected: bool }

TSharedPtr<FJsonObject> HandleDetectSubstrateEnabled(
	const TSharedPtr<FJsonObject>& /*Params*/)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	FillSubstrateState(Data);
	Data->SetBoolField(TEXT("detected"), true);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// create_substrate_slab_node  (succeeds even when Substrate disabled)
// ---------------------------------------------------------------------------
// MCP:create_substrate_slab_node - Add a Substrate Slab BSDF node to a UMaterial asset.  Performs a real CreateMaterialExpression on the material graph and saves the package. Idempotent: reuses an existing slab node on re-run. Node creation succeeds even when Substrate is disabled (substrate_enabled=false), but the material will not compile/render until Substrate is enabled and the editor restarts.
//   @retired add_graph_node - asset_path=<material>, node_type="/Script/Engine.MaterialExpressionSubstrateSlabBSDF", position_x=<x>, position_y=<y>; then save_asset. It always adds a node: get_graph_nodes shows an existing slab first
//   @param material_path - Content path to the UMaterial (e.g. /Game/MyGame/M_Example)
//   @param node_pos_x - Optional graph editor X position; defaults to -400
//   @param node_pos_y - Optional graph editor Y position; defaults to 0
//   @returns { material_path: string, material_name: string, node_id: string, node_class: string, created: bool, already_existed: bool, substrate_enabled: bool, r_substrate_cvar: int, note?: string }

TSharedPtr<FJsonObject> HandleCreateSubstrateSlabNode(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MaterialPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_path"), MaterialPath)) { return Missing; }

	UMaterial* Material = nullptr;
	if (TSharedPtr<FJsonObject> LoadError = Substrate_LoadMaterial(MaterialPath, Material)) { return LoadError; }

	double NodePosX = -400.0, NodePosY = 0.0;
	Params->TryGetNumberField(TEXT("node_pos_x"), NodePosX);
	Params->TryGetNumberField(TEXT("node_pos_y"), NodePosY);

	// Idempotency: reuse an existing slab node if one is already present.
	bool bAlreadyExisted = false;
	UMaterialExpressionSubstrateSlabBSDF* Slab = FindSlabNode(Material);
	if (Slab)
	{
		bAlreadyExisted = true;
	}
	else
	{
		UMaterialExpression* NewExpr = UMaterialEditingLibrary::CreateMaterialExpression(
			Material, UMaterialExpressionSubstrateSlabBSDF::StaticClass(),
			(int32)NodePosX, (int32)NodePosY);
		Slab = Cast<UMaterialExpressionSubstrateSlabBSDF>(NewExpr);
		if (!Slab)
		{
			return mcp_common::MakeErrorReply(
				TEXT("Failed to create SubstrateSlabBSDF expression"));
		}
		Slab->PostEditChange();
		Material->PostEditChange();
		Material->MarkPackageDirty();
		SaveMaterialPackage(Material);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("material_path"), Material->GetPathName());
	Data->SetStringField(TEXT("material_name"), Material->GetName());
	Data->SetStringField(TEXT("node_id"), Slab->GetName());
	Data->SetStringField(TEXT("node_class"), Slab->GetClass()->GetName());
	Data->SetBoolField(TEXT("created"), true);
	Data->SetBoolField(TEXT("already_existed"), bAlreadyExisted);
	FillSubstrateState(Data);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_substrate_slab_scalar  (drives a slab pin via a Constant node; works regardless of Substrate)
// ---------------------------------------------------------------------------
// MCP:set_substrate_slab_scalar - Drives one input pin of the Substrate Slab BSDF node of a material with a constant: adds a NEW Constant expression holding value to the material graph and wires it into the named pin of the material's first Slab node, replacing whatever fed that pin, then saves the material. Each call adds another Constant node, even for the same pin and value; a constant that fed the pin before stays in the graph, unwired. One undo step removes the new constant and restores what fed the pin. A failed save (a read-only file, PIE running) makes success false with saved=false and save_error, the change kept in memory. Works whether Substrate is enabled or not (substrate_enabled reports it). The material needs a Slab node (add_graph_node with node_type=/Script/Engine.MaterialExpressionSubstrateSlabBSDF); connect_pins to @output, pin FrontMaterial, wires it to the output. A pin name the Slab node does not have is an error that names its pins, and nothing is added. To change a constant already wired, use set_properties on its object_path (from get_graph_nodes); remove stray nodes with remove_graph_node.
//   @hints not-idempotent
//   @param material_path - Package or object path of the UMaterial holding the slab node (e.g. "/Game/Materials/M_Substrate").
//   @param pin_name - Slab input pin to drive, by its input name as get_graph_nodes lists it (input_name), without case: e.g. "Roughness", "Anisotropy", "F0" or "Diffuse Albedo" (a color pin receives the scalar on all channels).
//   @param value - Scalar the Constant node holds (e.g. 0.35 for Roughness); not range-checked.
//   @returns { material_path: string, slab_node_id: string, constant_node_id: string, pin_name: string, value: number, connected: bool, saved: bool, save_error?: string, substrate_enabled: bool, r_substrate_cvar: int, note?: string } -- connected is read back from the slab pin

TSharedPtr<FJsonObject> HandleSetSubstrateSlabScalar(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MaterialPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_path"), MaterialPath)) { return Missing; }

	FString PinName;
	if (!Params->TryGetStringField(TEXT("pin_name"), PinName) || PinName.IsEmpty())
	{
		return mcp_common::MakeErrorReply(
			TEXT("pin_name is required (e.g. Roughness, F0, Diffuse Albedo)"));
	}

	double ValueD = 0.0;
	if (!Params->TryGetNumberField(TEXT("value"), ValueD))
	{
		return mcp_common::MakeErrorReply(TEXT("value (float) is required"));
	}

	UMaterial* Material = nullptr;
	UMaterialExpressionSubstrateSlabBSDF* Slab = nullptr;
	if (TSharedPtr<FJsonObject> LoadError = Substrate_LoadMaterialSlab(MaterialPath, Material, Slab)) { return LoadError; }

	// The pin is resolved before anything is created, so a name the Slab node does not have changes nothing.
	TArray<FString> PinNames;
	FExpressionInput* SlabInput = Substrate_FindInput(Slab, PinName, PinNames);
	if (!SlabInput)
	{
		return mcp_common::MakeErrorReply(FString::Printf(
			TEXT("The Slab node has no input pin '%s'; nothing was added. Its pins: %s"),
			*PinName, *FString::Join(PinNames, TEXT(", "))));
	}

	// The material (with its editor-only data, which holds the expression collection) and the slab are recorded
	// for undo before the new node and the new wire; the Constant is created transactional.
	Material->SetFlags(RF_Transactional);
	if (UMaterialEditorOnlyData* EditorOnly = Material->GetEditorOnlyData())
	{
		EditorOnly->SetFlags(RF_Transactional);
	}
	Slab->SetFlags(RF_Transactional);
	Material->Modify();
	Slab->Modify();

	// Create a Constant node carrying the scalar value, then connect it to the named slab pin.
	UMaterialExpression* ConstExpr = UMaterialEditingLibrary::CreateMaterialExpression(
		Material, UMaterialExpressionConstant::StaticClass(), -700, 0);
	UMaterialExpressionConstant* Constant = Cast<UMaterialExpressionConstant>(ConstExpr);
	if (!Constant)
	{
		return mcp_common::MakeErrorReply(TEXT("Failed to create Constant node"));
	}
	Constant->R = (float)ValueD;
	Constant->PostEditChange();

	UMaterialEditingLibrary::ConnectMaterialExpressions(Constant, FString(), Slab, PinName);
	// Read back from the pin itself.
	const bool bConnected = SlabInput->Expression == Constant;

	Slab->PostEditChange();
	Material->PostEditChange();
	Material->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("material_path"), Material->GetPathName());
	Data->SetStringField(TEXT("slab_node_id"), Slab->GetName());
	Data->SetStringField(TEXT("constant_node_id"), Constant->GetName());
	Data->SetStringField(TEXT("pin_name"), PinName);
	Data->SetNumberField(TEXT("value"), ValueD);
	Data->SetBoolField(TEXT("connected"), bConnected);
	FillSubstrateState(Data);
	if (!bConnected)
	{
		// The unwired Constant goes back out, so a failed call leaves the graph as it was.
		UMaterialEditingLibrary::DeleteMaterialExpression(Material, Constant);
		Data->SetBoolField(TEXT("saved"), false);
		return mcp_common::MakeOutcomeReply(Data, false, FString::Printf(
			TEXT("set_substrate_slab_scalar: slab pin '%s' does not read back as fed by the new Constant, so the Constant was ")
			TEXT("deleted again and nothing was saved."), *PinName));
	}

	FString SaveError;
	const bool bSaved = mcp_common::SaveAssetPackage(Material->GetPackage(), Material, SaveError);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_substrate_slab_scalar"), SaveError);
}

// ---------------------------------------------------------------------------
// connect_substrate_slab_to_material  (gated by Substrate; honest warn when disabled)
// ---------------------------------------------------------------------------
// MCP:connect_substrate_slab_to_material - Connect the Slab BSDF output to the material's Front Material input.  GATED by Substrate: when Substrate is disabled this returns a clear error ("Substrate disabled: cannot connect slab to Front Material...") because the Front Material input is only meaningful with Substrate enabled. When enabled, connects via ConnectMaterialProperty(MP_FrontMaterial) and recompiles. Unwire: disconnect_to_material_output output_property=FrontMaterial.
//   @retired connect_pins - asset_path=<material>, source_node_id=<slab expression name, from get_graph_nodes>, source_pin="", target_node_id="@output", target_pin="FrontMaterial" (refused while Substrate is disabled); then compile_material and save_asset
//   @param material_path - Content path to the UMaterial holding the slab node
//   @returns { material_path: string, slab_node_id: string, connected_to: string, connected: bool, substrate_enabled: bool, r_substrate_cvar: int, note?: string } | (error: "Substrate disabled: ...")

TSharedPtr<FJsonObject> HandleConnectSubstrateSlabToMaterial(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MaterialPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_path"), MaterialPath)) { return Missing; }

	UMaterial* Material = nullptr;
	UMaterialExpressionSubstrateSlabBSDF* Slab = nullptr;
	if (TSharedPtr<FJsonObject> LoadError = Substrate_LoadMaterialSlab(MaterialPath, Material, Slab)) { return LoadError; }

	const bool bSubstrateEnabled = Substrate::IsSubstrateEnabled();

	// The Front Material input only exists / is meaningful while Substrate is enabled.
	// Honest gate: refuse with a clear, explanatory error when Substrate is off so the
	// caller does not believe a connection was made that the compiler will reject.
	if (!bSubstrateEnabled)
	{
		return mcp_common::MakeErrorReply(
			TEXT("Substrate disabled: cannot connect slab to Front Material. Enable r.Substrate ")
			TEXT("and restart the editor; the slab node was authored but the material will not compile."));
	}

	const bool bConnected = UMaterialEditingLibrary::ConnectMaterialProperty(
		Slab, FString(), MP_FrontMaterial);
	if (!bConnected)
	{
		return mcp_common::MakeErrorReply(
			TEXT("ConnectMaterialProperty to Front Material failed"));
	}

	Slab->PostEditChange();
	Material->PostEditChange();
	Material->MarkPackageDirty();
	UMaterialEditingLibrary::RecompileMaterial(Material);
	SaveMaterialPackage(Material);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("material_path"), Material->GetPathName());
	Data->SetStringField(TEXT("slab_node_id"), Slab->GetName());
	Data->SetStringField(TEXT("connected_to"), TEXT("FrontMaterial"));
	Data->SetBoolField(TEXT("connected"), true);
	FillSubstrateState(Data);
	return mcp_common::MakeOkReply(Data);
}

const McpCommandFamilies::FRegistrar Registration =
{
	{ TEXT("connect_substrate_slab_to_material"), &HandleConnectSubstrateSlabToMaterial },
	{ TEXT("create_substrate_slab_node"), &HandleCreateSubstrateSlabNode },
	{ TEXT("is_substrate_enabled"), &HandleDetectSubstrateEnabled },
	{ TEXT("set_substrate_slab_scalar"), &HandleSetSubstrateSlabScalar },
};
} // namespace UEAgentMaterialSubstrateCommands
