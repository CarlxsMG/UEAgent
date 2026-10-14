#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

#include "Editor.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInstance.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameters.h"
#include "Materials/MaterialLayersFunctions.h"
#include "Materials/MaterialInstanceBasePropertyOverrides.h"
#include "StaticParameterSet.h"   // FStaticParameterSet (GetStaticParameters)
#include "MaterialEditingLibrary.h"   // GetStaticSwitchParameterNames / *ParameterOverride (MaterialEditor module, editor-gated in Build.cs)
#include "Engine/Texture.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"   // FAssetRegistryModule::AssetCreated

namespace UEAgentMaterialInstanceCommands
{

// MCP-DOMAIN: Material

namespace
{
	/** Load a UMaterialInstanceConstant from a content path. */
	UMaterialInstanceConstant* LoadMICFlexible(const FString& Path)
	{
		if (Path.IsEmpty())
		{
			return nullptr;
		}
		UMaterialInstanceConstant* MIC = LoadObject<UMaterialInstanceConstant>(nullptr, *Path);
		if (!MIC)
		{
			// Try loading as UMaterialInstance and casting
			UMaterialInstance* MI = LoadObject<UMaterialInstance>(nullptr, *Path);
			MIC = Cast<UMaterialInstanceConstant>(MI);
		}
		return MIC;
	}

	/** Save a material instance package to disk. */
	bool SaveMICPackage(UMaterialInstanceConstant* MIC, FString& OutError)
	{
		if (!MIC)
		{
			OutError = TEXT("Invalid material instance for save");
			return false;
		}
		return mcp_common::SaveAssetPackage(MIC->GetOutermost(), MIC, OutError);
	}
}

namespace UEAgentMaterialInstanceImpl
{
	// The instance at MIPath; the error reply when it does not load.
	TSharedPtr<FJsonObject> FindInstance(const FString& MIPath, UMaterialInstanceConstant*& OutMIC)
	{
		OutMIC = LoadMICFlexible(MIPath);
		if (!OutMIC)
		{
			return mcp_common::MakeErrorReply(FString::Printf(TEXT("Material instance not found: %s"), *MIPath));
		}
		return nullptr;
	}

	// Reads the required material_instance_path and loads that instance; the error reply when either fails.
	TSharedPtr<FJsonObject> ReadInstance(const TSharedPtr<FJsonObject>& Params, FString& OutPath, UMaterialInstanceConstant*& OutMIC)
	{
		if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_instance_path"), OutPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
		return FindInstance(OutPath, OutMIC);
	}

	// The material (base or instance) at Path; the error reply when it does not load.
	TSharedPtr<FJsonObject> FindMaterial(const FString& Path, UMaterialInterface*& OutMaterial)
	{
		OutMaterial = mcp_common::LoadAssetOrSoftPath<UMaterialInterface>(Path);
		if (!OutMaterial)
		{
			return mcp_common::MakeErrorReply(FString::Printf(TEXT("Material not found: %s"), *Path));
		}
		return nullptr;
	}

	// PostEditChange on the instance, then saves its package; the error reply when the save fails.
	TSharedPtr<FJsonObject> CommitInstance(UMaterialInstanceConstant* MIC)
	{
		MIC->PostEditChange();
		FString SaveError;
		if (!mcp_common::SaveAssetPackage(MIC->GetOutermost(), MIC, SaveError))
		{
			return mcp_common::MakeErrorReply(SaveError);
		}
		return nullptr;
	}

	// A reply payload that starts with material_instance_path.
	TSharedPtr<FJsonObject> NewPayload(const FString& MIPath)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("material_instance_path"), MIPath);
		return Data;
	}
}

// MCP:get_material_instance_params - List the parameter values a material instance has actually OVERRIDDEN (scalars, vectors, textures) -- it walks the instance's own override arrays, so it answers "what has been calibrated here", NOT "what can I tune". Two omissions to keep in mind: it never returns STATIC SWITCH parameters, which are the flags that switch whole shader branches on or off (an instance can read perfectly green and still render without a layer because a switch is false in the parent), and it hides every parameter the parent exposes but this instance has not touched -- those are not hard-wired, they keep the parent's value. For the full tunable contract use get_material_all_parameters, and for the switches get_material_static_switch_parameters / get_material_instance_static_switch.
//   @param material_instance_path - Content path to the UMaterialInstanceConstant.
//   @returns { material_instance_path: string, parent_material: string, scalar_params: [{name, value}], scalar_count: int, vector_params: [{name, r, g, b, a}], vector_count: int, texture_params: [{name, texture_path}], texture_count: int }

TSharedPtr<FJsonObject> HandleGetMaterialInstanceParams(const TSharedPtr<FJsonObject>& Params)
{
	FString MIPath;
	UMaterialInstanceConstant* MIC = nullptr;
	if (TSharedPtr<FJsonObject> Error = UEAgentMaterialInstanceImpl::ReadInstance(Params, MIPath, MIC)) { return Error; }

	TSharedPtr<FJsonObject> Data = UEAgentMaterialInstanceImpl::NewPayload(MIPath);

	// Parent material name
	if (MIC->Parent)
	{
		Data->SetStringField(TEXT("parent_material"), MIC->Parent->GetName());
	}
	else
	{
		Data->SetStringField(TEXT("parent_material"), TEXT("None"));
	}

	// Scalar parameters
	TArray<TSharedPtr<FJsonValue>> ScalarArray;
	for (const FScalarParameterValue& SPV : MIC->ScalarParameterValues)
	{
		TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
		ParamObj->SetStringField(TEXT("name"), SPV.ParameterInfo.Name.ToString());
		ParamObj->SetNumberField(TEXT("value"), SPV.ParameterValue);
		ScalarArray.Add(MakeShared<FJsonValueObject>(ParamObj));
	}
	Data->SetArrayField(TEXT("scalar_params"), ScalarArray);
	Data->SetNumberField(TEXT("scalar_count"), MIC->ScalarParameterValues.Num());

	// Vector parameters
	TArray<TSharedPtr<FJsonValue>> VectorArray;
	for (const FVectorParameterValue& VPV : MIC->VectorParameterValues)
	{
		TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
		ParamObj->SetStringField(TEXT("name"), VPV.ParameterInfo.Name.ToString());
		ParamObj->SetNumberField(TEXT("r"), VPV.ParameterValue.R);
		ParamObj->SetNumberField(TEXT("g"), VPV.ParameterValue.G);
		ParamObj->SetNumberField(TEXT("b"), VPV.ParameterValue.B);
		ParamObj->SetNumberField(TEXT("a"), VPV.ParameterValue.A);
		VectorArray.Add(MakeShared<FJsonValueObject>(ParamObj));
	}
	Data->SetArrayField(TEXT("vector_params"), VectorArray);
	Data->SetNumberField(TEXT("vector_count"), MIC->VectorParameterValues.Num());

	// Texture parameters
	TArray<TSharedPtr<FJsonValue>> TextureArray;
	for (const FTextureParameterValue& TPV : MIC->TextureParameterValues)
	{
		TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
		ParamObj->SetStringField(TEXT("name"), TPV.ParameterInfo.Name.ToString());
		if (TPV.ParameterValue)
		{
			ParamObj->SetStringField(TEXT("texture_path"), TPV.ParameterValue->GetPathName());
		}
		else
		{
			ParamObj->SetStringField(TEXT("texture_path"), TEXT("None"));
		}
		TextureArray.Add(MakeShared<FJsonValueObject>(ParamObj));
	}
	Data->SetArrayField(TEXT("texture_params"), TextureArray);
	Data->SetNumberField(TEXT("texture_count"), MIC->TextureParameterValues.Num());

	return mcp_common::MakeOkReply(Data);
}

// MCP:get_material_instance_parent - Which material is this instance derived from: the parent's asset path, name and class. Use it to trace an instance back to the base material whose graph and parameter contract actually govern it -- the parent is where static switch defaults and unexposed values live, so it is the next asset to inspect when an instance behaves unexpectedly. set_material_instance_parent changes it.
//   @param material_instance_path - Content path to the material instance.
//   @returns { material_instance_path: string, parent_material_path: string, parent_material_name: string, parent_material_class: string }

TSharedPtr<FJsonObject> HandleGetMaterialInstanceParent(const TSharedPtr<FJsonObject>& Params)
{
	FString MIPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_instance_path"), MIPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

	UMaterialInstance* MI = LoadObject<UMaterialInstance>(nullptr, *MIPath);
	if (!MI)
		return mcp_common::MakeErrorReply(FString::Printf(TEXT("Material instance not found: %s"), *MIPath));

	TSharedPtr<FJsonObject> Data = UEAgentMaterialInstanceImpl::NewPayload(MIPath);

	if (MI->Parent)
	{
		Data->SetStringField(TEXT("parent_material_path"), MI->Parent->GetPathName());
		Data->SetStringField(TEXT("parent_material_name"), MI->Parent->GetName());
		Data->SetStringField(TEXT("parent_material_class"), MI->Parent->GetClass()->GetName());
	}
	else
	{
		Data->SetStringField(TEXT("parent_material_path"), TEXT("None"));
		Data->SetStringField(TEXT("parent_material_name"), TEXT("None"));
		Data->SetStringField(TEXT("parent_material_class"), TEXT("None"));
	}

	return mcp_common::MakeOkReply(Data);
}

// MCP:set_material_instance_parent - Re-parent a material instance onto a different base material -- how you swap an instance from a vendor master to your own without recreating it and re-pointing every referencer. Overrides whose parameter names do not exist on the new parent become orphaned and silently stop applying, so re-read with get_material_all_parameters afterwards and expect to re-check the static switches, whose defaults come from the new parent.
//   @param material_instance_path - Content path to the UMaterialInstanceConstant.
//   @param new_parent_path - Content path of the new parent UMaterialInterface.
//   @returns { material_instance_path: string, old_parent: string, new_parent: string, changed: bool }

TSharedPtr<FJsonObject> HandleSetMaterialInstanceParent(const TSharedPtr<FJsonObject>& Params)
{
	FString MIPath, NewParentPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_instance_path"), MIPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("new_parent_path"), NewParentPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

	UMaterialInstanceConstant* MIC = nullptr;
	if (TSharedPtr<FJsonObject> Error = UEAgentMaterialInstanceImpl::FindInstance(MIPath, MIC)) { return Error; }

	UMaterialInterface* NewParent = mcp_common::LoadAssetOrSoftPath<UMaterialInterface>(NewParentPath);
	if (!NewParent)
		return mcp_common::MakeErrorReply(FString::Printf(TEXT("New parent material not found: %s"), *NewParentPath));

	FString OldParentName = MIC->Parent ? MIC->Parent->GetName() : TEXT("None");

	MIC->Modify();
	MIC->SetParentEditorOnly(NewParent, true);
	if (TSharedPtr<FJsonObject> SaveFailed = UEAgentMaterialInstanceImpl::CommitInstance(MIC)) { return SaveFailed; }

	// Read back to confirm
	FString NewParentRead = MIC->Parent ? MIC->Parent->GetName() : TEXT("None");

	TSharedPtr<FJsonObject> Data = UEAgentMaterialInstanceImpl::NewPayload(MIPath);
	Data->SetStringField(TEXT("old_parent"), OldParentName);
	Data->SetStringField(TEXT("new_parent"), NewParentRead);
	Data->SetBoolField(TEXT("changed"), !OldParentName.Equals(NewParentRead));

	return mcp_common::MakeOkReply(Data);
}

// MCP:copy_material_instance_params - Bulk-copy every scalar, vector and texture OVERRIDE from one material instance to another -- the way to clone a calibration onto a sibling instance instead of re-setting parameters one by one. Copies overrides only, so anything the source inherited untouched is not carried, and STATIC SWITCHES are not copied at all (set those with set_material_instance_static_switch). Read the copied counts in the reply and verify with get_material_all_parameters; diff_material_instances shows what still differs.
//   @param source_path - Content path of the source UMaterialInstanceConstant.
//   @param destination_path - Content path of the destination UMaterialInstanceConstant.
//   @returns { source_path: string, destination_path: string, scalars_copied: int, vectors_copied: int, textures_copied: int }

TSharedPtr<FJsonObject> HandleCopyMaterialInstanceParams(const TSharedPtr<FJsonObject>& Params)
{
	FString SourcePath, DestPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("source_path"), SourcePath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("destination_path"), DestPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

	UMaterialInstanceConstant* SourceMIC = LoadMICFlexible(SourcePath);
	if (!SourceMIC)
		return mcp_common::MakeErrorReply(FString::Printf(TEXT("Source material instance not found: %s"), *SourcePath));

	UMaterialInstanceConstant* DestMIC = LoadMICFlexible(DestPath);
	if (!DestMIC)
		return mcp_common::MakeErrorReply(FString::Printf(TEXT("Destination material instance not found: %s"), *DestPath));

	DestMIC->Modify();

	int32 ScalarsCopied = 0;
	int32 VectorsCopied = 0;
	int32 TexturesCopied = 0;

	// Copy scalar parameters
	for (const FScalarParameterValue& SPV : SourceMIC->ScalarParameterValues)
	{
		DestMIC->SetScalarParameterValueEditorOnly(SPV.ParameterInfo, SPV.ParameterValue);
		ScalarsCopied++;
	}

	// Copy vector parameters
	for (const FVectorParameterValue& VPV : SourceMIC->VectorParameterValues)
	{
		DestMIC->SetVectorParameterValueEditorOnly(VPV.ParameterInfo, VPV.ParameterValue);
		VectorsCopied++;
	}

	// Copy texture parameters
	for (const FTextureParameterValue& TPV : SourceMIC->TextureParameterValues)
	{
		DestMIC->SetTextureParameterValueEditorOnly(TPV.ParameterInfo, TPV.ParameterValue);
		TexturesCopied++;
	}

	if (TSharedPtr<FJsonObject> SaveFailed = UEAgentMaterialInstanceImpl::CommitInstance(DestMIC)) { return SaveFailed; }

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("source_path"), SourcePath);
	Data->SetStringField(TEXT("destination_path"), DestPath);
	Data->SetNumberField(TEXT("scalars_copied"), ScalarsCopied);
	Data->SetNumberField(TEXT("vectors_copied"), VectorsCopied);
	Data->SetNumberField(TEXT("textures_copied"), TexturesCopied);

	return mcp_common::MakeOkReply(Data);
}

// MCP:get_material_param_info - Read ONE named parameter's effective value off a material instance (param_type scalar | vector | texture), with a found flag so a misspelled name is distinguishable from a zero value. The cheap targeted read-back after set_material_instance_param_override -- use get_material_all_parameters when you want the whole list, and get_material_instance_param_overridden to tell "inherited from the parent" from "locally overridden", which this does not.
//   @param material_instance_path - Content path to the UMaterialInstanceConstant.
//   @param param_name - Name of the parameter to read.
//   @param param_type - One of "scalar", "vector", "texture".
//   @returns { param_name: string, param_type: string, found: bool, value: float | {r,g,b,a} | string }

TSharedPtr<FJsonObject> HandleGetMaterialParamInfo(const TSharedPtr<FJsonObject>& Params)
{
	FString MIPath, ParamName, ParamType;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_instance_path"), MIPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("param_name"), ParamName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("param_type"), ParamType, mcp_common::EAllowEmpty::Yes)) { return Missing; }

	UMaterialInstanceConstant* MIC = nullptr;
	if (TSharedPtr<FJsonObject> Error = UEAgentMaterialInstanceImpl::FindInstance(MIPath, MIC)) { return Error; }

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("param_name"), ParamName);
	Data->SetStringField(TEXT("param_type"), ParamType);

	const FName ParamFName(*ParamName);

	if (ParamType.Equals(TEXT("scalar"), ESearchCase::IgnoreCase))
	{
		for (const FScalarParameterValue& SPV : MIC->ScalarParameterValues)
		{
			if (SPV.ParameterInfo.Name == ParamFName)
			{
				Data->SetNumberField(TEXT("value"), SPV.ParameterValue);
				Data->SetBoolField(TEXT("found"), true);
				return mcp_common::MakeOkReply(Data);
			}
		}
	}
	else if (ParamType.Equals(TEXT("vector"), ESearchCase::IgnoreCase))
	{
		for (const FVectorParameterValue& VPV : MIC->VectorParameterValues)
		{
			if (VPV.ParameterInfo.Name == ParamFName)
			{
				Data->SetObjectField(TEXT("value"), mcp_common::LinearColorToJsonObject(VPV.ParameterValue));
				Data->SetBoolField(TEXT("found"), true);
				return mcp_common::MakeOkReply(Data);
			}
		}
	}
	else if (ParamType.Equals(TEXT("texture"), ESearchCase::IgnoreCase))
	{
		for (const FTextureParameterValue& TPV : MIC->TextureParameterValues)
		{
			if (TPV.ParameterInfo.Name == ParamFName)
			{
				if (TPV.ParameterValue)
				{
					Data->SetStringField(TEXT("value"), TPV.ParameterValue->GetPathName());
				}
				else
				{
					Data->SetStringField(TEXT("value"), TEXT("None"));
				}
				Data->SetBoolField(TEXT("found"), true);
				return mcp_common::MakeOkReply(Data);
			}
		}
	}
	else
	{
		return mcp_common::MakeErrorReply(FString::Printf(TEXT("Unknown param_type: %s (expected scalar/vector/texture)"), *ParamType));
	}

	// Not found
	Data->SetBoolField(TEXT("found"), false);
	return mcp_common::MakeOkReply(Data);
}

// ============================================================================
// Material Parameter Overrides
// ============================================================================

namespace
{
	/** Helper: build a FMaterialParameterInfo for a layer-scoped parameter. */
	FMaterialParameterInfo MakeLayerParamInfo(const FName& ParamName, int32 LayerIndex)
	{
		FMaterialParameterInfo Info;
		Info.Name = ParamName;
		Info.Association = EMaterialParameterAssociation::LayerParameter;
		Info.Index = LayerIndex;
		return Info;
	}

	/** Helper: convert EMaterialParameterAssociation to a string. */
	FString AssociationToString(EMaterialParameterAssociation Assoc)
	{
		switch (Assoc)
		{
		case EMaterialParameterAssociation::GlobalParameter:  return TEXT("Global");
		case EMaterialParameterAssociation::LayerParameter:   return TEXT("Layer");
		case EMaterialParameterAssociation::BlendParameter:   return TEXT("Blend");
		default:                                              return TEXT("Unknown");
		}
	}
}

// ---------------------------------------------------------------------------
// set_material_layer_parameter
// ---------------------------------------------------------------------------
// MCP:set_material_layer_parameter - Write a scalar / vector / texture parameter scoped to ONE layer of a layered material instance (addressed by zero-based layer_index), so each layer of a blend keeps its own tiling, tint or texture. Different from set_material_instance_param_override, which targets the Global association and would hit the wrong slot on a layered material. Do not trust the success flag -- the engine's setters are known to report failure while doing the work; re-read with get_material_layer_parameter.
//   @param material_instance_path - Content path to the MIC, e.g. /Game/MyGame/MI_Example
//   @param parameter_name - Name of the parameter to set
//   @param parameter_type - One of "scalar", "vector", "texture"
//   @param layer_index - Zero-based layer index (for LayerParameter association)
//   @param value - For scalar: a float. For vector: dict {r, g, b, a}. For texture: a content path string.
//   @returns { material_instance_path: string, parameter_name: string, layer_index: int, success: bool }

TSharedPtr<FJsonObject> HandleSetMaterialLayerParameter(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MIPath, ParamName, ParamType;
	int32 LayerIndex = 0;

	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_instance_path"), MIPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("parameter_name"), ParamName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("parameter_type"), ParamType, mcp_common::EAllowEmpty::Yes)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("layer_index"), LayerIndex)) { return Missing; }

	UMaterialInstanceConstant* MIC = nullptr;
	if (TSharedPtr<FJsonObject> Error = UEAgentMaterialInstanceImpl::FindInstance(MIPath, MIC)) { return Error; }

	const FName ParamFName(*ParamName);
	const FMaterialParameterInfo ParamInfo = MakeLayerParamInfo(ParamFName, LayerIndex);

	MIC->Modify();

	if (ParamType.Equals(TEXT("scalar"), ESearchCase::IgnoreCase))
	{
		float Value = 0.0f;
		if (!Params->TryGetNumberField(TEXT("value"), Value))
			return mcp_common::MakeErrorReply(TEXT("Missing required parameter: value (number) for scalar type"));
		MIC->SetScalarParameterValueEditorOnly(ParamInfo, Value);
	}
	else if (ParamType.Equals(TEXT("vector"), ESearchCase::IgnoreCase))
	{
		const TSharedPtr<FJsonObject>* VecObj;
		if (!Params->TryGetObjectField(TEXT("value"), VecObj) || !VecObj || !*VecObj)
			return mcp_common::MakeErrorReply(TEXT("Missing required parameter: value (object with r,g,b,a) for vector type"));
		FLinearColor Color;
		(*VecObj)->TryGetNumberField(TEXT("r"), Color.R);
		(*VecObj)->TryGetNumberField(TEXT("g"), Color.G);
		(*VecObj)->TryGetNumberField(TEXT("b"), Color.B);
		(*VecObj)->TryGetNumberField(TEXT("a"), Color.A);
		MIC->SetVectorParameterValueEditorOnly(ParamInfo, Color);
	}
	else if (ParamType.Equals(TEXT("texture"), ESearchCase::IgnoreCase))
	{
		FString TexturePath;
		if (!Params->TryGetStringField(TEXT("value"), TexturePath))
			return mcp_common::MakeErrorReply(TEXT("Missing required parameter: value (texture path string) for texture type"));
		UTexture* Tex = LoadObject<UTexture>(nullptr, *TexturePath);
		if (!Tex)
			return mcp_common::MakeErrorReply(FString::Printf(TEXT("Texture not found: %s"), *TexturePath));
		MIC->SetTextureParameterValueEditorOnly(ParamInfo, Tex);
	}
	else
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Unknown parameter_type: %s (expected scalar/vector/texture)"), *ParamType));
	}

	if (TSharedPtr<FJsonObject> SaveFailed = UEAgentMaterialInstanceImpl::CommitInstance(MIC)) { return SaveFailed; }

	TSharedPtr<FJsonObject> Data = UEAgentMaterialInstanceImpl::NewPayload(MIPath);
	Data->SetStringField(TEXT("parameter_name"), ParamName);
	Data->SetNumberField(TEXT("layer_index"), LayerIndex);
	Data->SetBoolField(TEXT("success"), true);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_material_layer_parameter
// ---------------------------------------------------------------------------
// MCP:get_material_layer_parameter - Read one parameter's value scoped to a specific LAYER of a layered material instance (by zero-based layer_index), with a found flag. The read-back oracle for set_material_layer_parameter, and the way to tell which layer a value actually belongs to -- get_material_all_parameters lists every layer's parameters together with their association, which is broader but noisier when you already know the slot.
//   @param material_instance_path - Content path to the MIC
//   @param parameter_name - Name of the parameter to read
//   @param parameter_type - One of "scalar", "vector", "texture"
//   @param layer_index - Zero-based layer index
//   @returns { material_instance_path: string, parameter_name: string, layer_index: int, found: bool, value: float | {r,g,b,a} | string }

TSharedPtr<FJsonObject> HandleGetMaterialLayerParameter(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MIPath, ParamName, ParamType;
	int32 LayerIndex = 0;

	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_instance_path"), MIPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("parameter_name"), ParamName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("parameter_type"), ParamType, mcp_common::EAllowEmpty::Yes)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("layer_index"), LayerIndex)) { return Missing; }

	UMaterialInstanceConstant* MIC = nullptr;
	if (TSharedPtr<FJsonObject> Error = UEAgentMaterialInstanceImpl::FindInstance(MIPath, MIC)) { return Error; }

	const FName ParamFName(*ParamName);
	const FMaterialParameterInfo ParamInfo = MakeLayerParamInfo(ParamFName, LayerIndex);

	TSharedPtr<FJsonObject> Data = UEAgentMaterialInstanceImpl::NewPayload(MIPath);
	Data->SetStringField(TEXT("parameter_name"), ParamName);
	Data->SetNumberField(TEXT("layer_index"), LayerIndex);

	if (ParamType.Equals(TEXT("scalar"), ESearchCase::IgnoreCase))
	{
		float Value = 0.0f;
		if (MIC->GetScalarParameterValue(ParamInfo, Value))
		{
			Data->SetNumberField(TEXT("value"), Value);
			Data->SetBoolField(TEXT("found"), true);
		}
		else
		{
			Data->SetBoolField(TEXT("found"), false);
		}
	}
	else if (ParamType.Equals(TEXT("vector"), ESearchCase::IgnoreCase))
	{
		FLinearColor Value;
		if (MIC->GetVectorParameterValue(ParamInfo, Value))
		{
			TSharedPtr<FJsonObject> VecObj = mcp_common::LinearColorToJsonObject(Value);
			Data->SetObjectField(TEXT("value"), VecObj);
			Data->SetBoolField(TEXT("found"), true);
		}
		else
		{
			Data->SetBoolField(TEXT("found"), false);
		}
	}
	else if (ParamType.Equals(TEXT("texture"), ESearchCase::IgnoreCase))
	{
		UTexture* Value = nullptr;
		if (MIC->GetTextureParameterValue(ParamInfo, Value))
		{
			Data->SetStringField(TEXT("value"), Value ? Value->GetPathName() : TEXT("None"));
			Data->SetBoolField(TEXT("found"), true);
		}
		else
		{
			Data->SetBoolField(TEXT("found"), false);
		}
	}
	else
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Unknown parameter_type: %s (expected scalar/vector/texture)"), *ParamType));
	}

	return mcp_common::MakeOkReply(Data);
}

namespace UEAgentMaterialInstanceImpl
{
	// name / type / association / layer_index head shared by every get_material_all_parameters entry.
	TSharedPtr<FJsonObject> MakeParameterEntry(const FMaterialParameterInfo& Info, const TCHAR* Kind)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Info.Name.ToString());
		Entry->SetStringField(TEXT("type"), Kind);
		Entry->SetStringField(TEXT("association"), AssociationToString(Info.Association));
		Entry->SetNumberField(TEXT("layer_index"), Info.Index);
		return Entry;
	}

	// Appends the function each { function_path } entry names, skipping the rest; false, with OutError set, when one does not load.
	bool AppendStackFunctions(const TArray<TSharedPtr<FJsonValue>>& Entries, const TCHAR* Kind,
		TArray<TObjectPtr<UMaterialFunctionInterface>>& OutFunctions, FString& OutError)
	{
		for (const TSharedPtr<FJsonValue>& Entry : Entries)
		{
			const TSharedPtr<FJsonObject>* EntryObj;
			if (!Entry->TryGetObject(EntryObj) || !EntryObj || !*EntryObj)
				continue;

			FString FuncPath;
			(*EntryObj)->TryGetStringField(TEXT("function_path"), FuncPath);
			if (FuncPath.IsEmpty())
				continue;

			UMaterialFunctionInterface* Func = LoadObject<UMaterialFunctionInterface>(nullptr, *FuncPath);
			if (!Func)
			{
				OutError = FString::Printf(TEXT("%s function not found: %s"), Kind, *FuncPath);
				return false;
			}
			OutFunctions.Add(Func);
		}
		return true;
	}
}

// ---------------------------------------------------------------------------
// get_material_all_parameters
// ---------------------------------------------------------------------------
// MCP:get_material_all_parameters - What can I tune on this material without recompiling it: EVERY scalar, vector and texture parameter the material or its parent exposes, each with its effective value, layer association and layer index. This is the contract, and the one to prefer over get_material_instance_params, which only shows the subset an instance already overrode and so makes untouched parameters look unavailable. Works on a base material or an instance. Covers scalar/vector/texture ONLY -- static switches are a separate list (get_material_static_switch_parameters) and they are what decide whether whole shader branches run at all.
//   @param material_instance_path - Content path to the material (UMaterialInterface)
//   @param parameter_type - Optional filter: "scalar", "vector", or "texture" (omit for all).
//   @returns { material_instance_path: string, parameters: [{name, type, association, layer_index, value?}], parameter_count: int }

TSharedPtr<FJsonObject> HandleGetMaterialAllParameters(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MIPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_instance_path"), MIPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

	// Optional filter by type
	FString TypeFilter;
	Params->TryGetStringField(TEXT("parameter_type"), TypeFilter);

	UMaterialInterface* Mat = nullptr;
	if (TSharedPtr<FJsonObject> Error = UEAgentMaterialInstanceImpl::FindMaterial(MIPath, Mat)) { return Error; }

	TSharedPtr<FJsonObject> Data = UEAgentMaterialInstanceImpl::NewPayload(MIPath);

	TArray<TSharedPtr<FJsonValue>> ParametersArray;

	const bool bIncludeScalar = TypeFilter.IsEmpty() || TypeFilter.Equals(TEXT("scalar"), ESearchCase::IgnoreCase);
	const bool bIncludeVector = TypeFilter.IsEmpty() || TypeFilter.Equals(TEXT("vector"), ESearchCase::IgnoreCase);
	const bool bIncludeTexture = TypeFilter.IsEmpty() || TypeFilter.Equals(TEXT("texture"), ESearchCase::IgnoreCase);

	// Every entry starts with the same name/type/association/layer_index head; "value" is only added
	// when the material reports a value for that parameter. Kinds are listed scalar, vector, texture.
	if (bIncludeScalar)
	{
		TArray<FMaterialParameterInfo> ScalarInfo;
		TArray<FGuid> ScalarIds;
		Mat->GetAllScalarParameterInfo(ScalarInfo, ScalarIds);
		for (const FMaterialParameterInfo& Info : ScalarInfo)
		{
			TSharedPtr<FJsonObject> Entry = UEAgentMaterialInstanceImpl::MakeParameterEntry(Info, TEXT("scalar"));
			float Scalar = 0.0f;
			if (Mat->GetScalarParameterValue(Info, Scalar))
			{
				Entry->SetNumberField(TEXT("value"), Scalar);
			}
			ParametersArray.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}

	if (bIncludeVector)
	{
		TArray<FMaterialParameterInfo> VectorInfo;
		TArray<FGuid> VectorIds;
		Mat->GetAllVectorParameterInfo(VectorInfo, VectorIds);
		for (const FMaterialParameterInfo& Info : VectorInfo)
		{
			TSharedPtr<FJsonObject> Entry = UEAgentMaterialInstanceImpl::MakeParameterEntry(Info, TEXT("vector"));
			FLinearColor Color;
			if (Mat->GetVectorParameterValue(Info, Color))
			{
				Entry->SetObjectField(TEXT("value"), mcp_common::LinearColorToJsonObject(Color));
			}
			ParametersArray.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}

	if (bIncludeTexture)
	{
		TArray<FMaterialParameterInfo> TexInfo;
		TArray<FGuid> TexIds;
		Mat->GetAllTextureParameterInfo(TexInfo, TexIds);
		for (const FMaterialParameterInfo& Info : TexInfo)
		{
			TSharedPtr<FJsonObject> Entry = UEAgentMaterialInstanceImpl::MakeParameterEntry(Info, TEXT("texture"));
			UTexture* Texture = nullptr;
			if (Mat->GetTextureParameterValue(Info, Texture))
			{
				Entry->SetStringField(TEXT("value"), Texture ? Texture->GetPathName() : TEXT("None"));
			}
			ParametersArray.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}

	Data->SetArrayField(TEXT("parameters"), ParametersArray);
	Data->SetNumberField(TEXT("parameter_count"), ParametersArray.Num());

	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_material_layer_stack
// ---------------------------------------------------------------------------
// MCP:set_material_layer_stack - Define which material-layer and blend functions a layered material instance stacks, in order -- how you compose a surface (base + dirt + snow) out of reusable layer functions instead of editing the base material graph. Order is significant and layers are addressed downstream by index (set_material_layer_parameter), so re-stacking renumbers existing per-layer values. Read the result back with get_material_layer_stack.
//   @param material_instance_path - Content path to the MIC
//   @param layers - Optional list of {function_path} dicts naming layer material functions.
//   @param blends - Optional list of {function_path} dicts naming blend material functions.
//   @returns { material_instance_path: string, layer_count: int, blend_count: int, success: bool }

TSharedPtr<FJsonObject> HandleSetMaterialLayerStack(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MIPath;
	UMaterialInstanceConstant* MIC = nullptr;
	if (TSharedPtr<FJsonObject> Error = UEAgentMaterialInstanceImpl::ReadInstance(Params, MIPath, MIC)) { return Error; }

	FMaterialLayersFunctions LayersFunctions;
	FString StackError;

	const TArray<TSharedPtr<FJsonValue>>* LayersArr = nullptr;
	if (Params->TryGetArrayField(TEXT("layers"), LayersArr) && LayersArr
		&& !UEAgentMaterialInstanceImpl::AppendStackFunctions(*LayersArr, TEXT("Layer"), LayersFunctions.Layers, StackError))
	{
		return mcp_common::MakeErrorReply(StackError);
	}

	const TArray<TSharedPtr<FJsonValue>>* BlendsArr = nullptr;
	if (Params->TryGetArrayField(TEXT("blends"), BlendsArr) && BlendsArr
		&& !UEAgentMaterialInstanceImpl::AppendStackFunctions(*BlendsArr, TEXT("Blend"), LayersFunctions.Blends, StackError))
	{
		return mcp_common::MakeErrorReply(StackError);
	}

	MIC->Modify();

	const bool bSuccess = MIC->SetMaterialLayers(LayersFunctions);
	if (!bSuccess)
		return mcp_common::MakeErrorReply(TEXT("SetMaterialLayers failed"));

	if (TSharedPtr<FJsonObject> SaveFailed = UEAgentMaterialInstanceImpl::CommitInstance(MIC)) { return SaveFailed; }

	TSharedPtr<FJsonObject> Data = UEAgentMaterialInstanceImpl::NewPayload(MIPath);
	Data->SetNumberField(TEXT("layer_count"), LayersFunctions.Layers.Num());
	Data->SetNumberField(TEXT("blend_count"), LayersFunctions.Blends.Num());
	Data->SetBoolField(TEXT("success"), true);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_material_layer_stack
// ---------------------------------------------------------------------------
// MCP:get_material_layer_stack - Read a material's layer stack: the layer functions and blend functions it uses, in order, with has_layers telling you whether the material is layered at all. Check this before any set_material_layer_parameter call -- on a non-layered material the layer indices mean nothing, and here is where you learn which index is which surface.
//   @param material_instance_path - Content path to the material
//   @returns { material_instance_path: string, layers: [{name, function_path}], blends: [{name, function_path}], layer_count: int, blend_count: int, has_layers: bool }

TSharedPtr<FJsonObject> HandleGetMaterialLayerStack(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MIPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_instance_path"), MIPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

	UMaterialInterface* Mat = nullptr;
	if (TSharedPtr<FJsonObject> Error = UEAgentMaterialInstanceImpl::FindMaterial(MIPath, Mat)) { return Error; }

	FMaterialLayersFunctions LayersFunctions;
	const bool bHasLayers = Mat->GetMaterialLayers(LayersFunctions);

	TSharedPtr<FJsonObject> Data = UEAgentMaterialInstanceImpl::NewPayload(MIPath);

	// Layers array
	TArray<TSharedPtr<FJsonValue>> LayersArr;
	for (int32 i = 0; i < LayersFunctions.Layers.Num(); ++i)
	{
		TSharedPtr<FJsonObject> LayerObj = MakeShared<FJsonObject>();
		UMaterialFunctionInterface* Func = LayersFunctions.Layers[i];
		LayerObj->SetStringField(TEXT("name"), Func ? Func->GetName() : TEXT("None"));
		LayerObj->SetStringField(TEXT("function_path"), Func ? Func->GetPathName() : TEXT("None"));
		LayersArr.Add(MakeShared<FJsonValueObject>(LayerObj));
	}
	Data->SetArrayField(TEXT("layers"), LayersArr);

	// Blends array
	TArray<TSharedPtr<FJsonValue>> BlendsArr;
	for (int32 i = 0; i < LayersFunctions.Blends.Num(); ++i)
	{
		TSharedPtr<FJsonObject> BlendObj = MakeShared<FJsonObject>();
		UMaterialFunctionInterface* Func = LayersFunctions.Blends[i];
		BlendObj->SetStringField(TEXT("name"), Func ? Func->GetName() : TEXT("None"));
		BlendObj->SetStringField(TEXT("function_path"), Func ? Func->GetPathName() : TEXT("None"));
		BlendsArr.Add(MakeShared<FJsonValueObject>(BlendObj));
	}
	Data->SetArrayField(TEXT("blends"), BlendsArr);

	Data->SetNumberField(TEXT("layer_count"), LayersFunctions.Layers.Num());
	Data->SetNumberField(TEXT("blend_count"), LayersFunctions.Blends.Num());
	Data->SetBoolField(TEXT("has_layers"), bHasLayers);

	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// swap_material_layer_parameters
// ---------------------------------------------------------------------------
// MCP:swap_material_layer_parameters - Exchange two layer slots on a layered material instance -- reorder which surface sits above which without rebuilding the whole stack with set_material_layer_stack. Every per-layer parameter moves with the slot, so any index you recorded earlier now points at the other layer; re-read get_material_layer_stack afterwards.
//   @param material_instance_path - Content path to the MIC
//   @param original_index - The original layer parameter index
//   @param new_index - The new layer parameter index to swap with
//   @returns { material_instance_path: string, original_index: int, new_index: int, success: bool }

TSharedPtr<FJsonObject> HandleSwapMaterialLayerParameters(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MIPath;
	int32 OriginalIndex = 0, NewIndex = 0;

	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_instance_path"), MIPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("original_index"), OriginalIndex)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("new_index"), NewIndex)) { return Missing; }

	UMaterialInstanceConstant* MIC = nullptr;
	if (TSharedPtr<FJsonObject> Error = UEAgentMaterialInstanceImpl::FindInstance(MIPath, MIC)) { return Error; }

	// Guard: SwapLayerParameterIndices indexes the material layers array with no
	// internal bounds check — on a material instance with no (or too few) layers
	// this access-violates and kills the editor. Reject out-of-range indices with
	// a clean error instead.
	FMaterialLayersFunctions LayersOut;
	const bool bHasLayers = MIC->GetMaterialLayers(LayersOut);
	const int32 NumLayers = bHasLayers ? LayersOut.Layers.Num() : 0;
	if (OriginalIndex < 0 || NewIndex < 0 || OriginalIndex >= NumLayers || NewIndex >= NumLayers)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Layer index out of range: material instance has %d layer(s) "
				"(requested original_index=%d, new_index=%d)"), NumLayers, OriginalIndex, NewIndex));
	}

	MIC->Modify();
	MIC->SwapLayerParameterIndices(OriginalIndex, NewIndex);
	if (TSharedPtr<FJsonObject> SaveFailed = UEAgentMaterialInstanceImpl::CommitInstance(MIC)) { return SaveFailed; }

	TSharedPtr<FJsonObject> Data = UEAgentMaterialInstanceImpl::NewPayload(MIPath);
	Data->SetNumberField(TEXT("original_index"), OriginalIndex);
	Data->SetNumberField(TEXT("new_index"), NewIndex);
	Data->SetBoolField(TEXT("success"), true);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// clear_material_parameter_overrides
// ---------------------------------------------------------------------------
// MCP:clear_material_parameter_overrides - Clear all parameter overrides on a Material Instance Constant.  Resets all scalar, vector, and texture parameter values to their defaults (inherited from the parent material).
//   @param material_instance_path - Content path to the MIC
//   @returns { material_instance_path: string, cleared: bool }

TSharedPtr<FJsonObject> HandleClearMaterialParameterOverrides(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MIPath;
	UMaterialInstanceConstant* MIC = nullptr;
	if (TSharedPtr<FJsonObject> Error = UEAgentMaterialInstanceImpl::ReadInstance(Params, MIPath, MIC)) { return Error; }

	MIC->Modify();
	MIC->ClearParameterValuesEditorOnly();
	if (TSharedPtr<FJsonObject> SaveFailed = UEAgentMaterialInstanceImpl::CommitInstance(MIC)) { return SaveFailed; }

	TSharedPtr<FJsonObject> Data = UEAgentMaterialInstanceImpl::NewPayload(MIPath);
	Data->SetBoolField(TEXT("cleared"), true);
	return mcp_common::MakeOkReply(Data);
}

// ============================================================================
// Material Instance Base Property Overrides
// ============================================================================

namespace
{
	/** Resolve an EBlendMode value from a name string (accepts "Translucent" or "BLEND_Translucent"). */
	bool ResolveBlendMode(const FString& Name, EBlendMode& OutMode)
	{
		const FString Full = Name.StartsWith(TEXT("BLEND_")) ? Name : (TEXT("BLEND_") + Name);
		const int64 Value = StaticEnum<EBlendMode>()->GetValueByNameString(Full);
		if (Value == INDEX_NONE)
		{
			return false;
		}
		OutMode = static_cast<EBlendMode>(Value);
		return true;
	}

	/** Resolve an EMaterialShadingModel value from a name string (accepts "Unlit" or "MSM_Unlit"). */
	bool ResolveShadingModel(const FString& Name, EMaterialShadingModel& OutModel)
	{
		const FString Full = Name.StartsWith(TEXT("MSM_")) ? Name : (TEXT("MSM_") + Name);
		const int64 Value = StaticEnum<EMaterialShadingModel>()->GetValueByNameString(Full);
		if (Value == INDEX_NONE)
		{
			return false;
		}
		OutModel = static_cast<EMaterialShadingModel>(Value);
		return true;
	}

	/** Emit the full set of base-property-override flags + values into a JSON object (shared by set/get). */
	void EmitBaseOverrides(const FMaterialInstanceBasePropertyOverrides& Ov, const TSharedPtr<FJsonObject>& Data)
	{
		Data->SetBoolField(TEXT("override_blend_mode"), Ov.bOverride_BlendMode != 0);
		Data->SetStringField(TEXT("blend_mode"),
			StaticEnum<EBlendMode>()->GetNameStringByValue(static_cast<int64>(Ov.BlendMode.GetValue())));

		Data->SetBoolField(TEXT("override_two_sided"), Ov.bOverride_TwoSided != 0);
		Data->SetBoolField(TEXT("two_sided"), Ov.TwoSided != 0);

		Data->SetBoolField(TEXT("override_shading_model"), Ov.bOverride_ShadingModel != 0);
		Data->SetStringField(TEXT("shading_model"),
			StaticEnum<EMaterialShadingModel>()->GetNameStringByValue(static_cast<int64>(Ov.ShadingModel.GetValue())));

		Data->SetBoolField(TEXT("override_opacity_mask_clip_value"), Ov.bOverride_OpacityMaskClipValue != 0);
		Data->SetNumberField(TEXT("opacity_mask_clip_value"), Ov.OpacityMaskClipValue);
	}
}

// ---------------------------------------------------------------------------
// set_material_instance_base_overrides
// ---------------------------------------------------------------------------
// MCP:set_material_instance_base_overrides - Set one or more base property overrides (BasePropertyOverrides) on a Material Instance Constant. Each provided axis sets BOTH its bOverride_ flag and value; omitted axes are left untouched. Persists to the asset.
//   @param material_instance_path - Content path to the UMaterialInstanceConstant, e.g. /Game/MyGame/MI_Example
//   @param blend_mode - Optional EBlendMode name ("Translucent" or "BLEND_Translucent"): Opaque/Masked/Translucent/Additive/Modulate/AlphaComposite/AlphaHoldout.
//   @param two_sided - Optional bool to override the two-sided flag.
//   @param shading_model - Optional EMaterialShadingModel name ("Unlit" or "MSM_Unlit"): Unlit/DefaultLit/Subsurface/ClearCoat/...
//   @param opacity_mask_clip_value - Optional float to override the opacity mask clip value.
//   @returns { material_instance_path: string, applied_count: int, saved: bool, override_blend_mode: bool, blend_mode: string, override_two_sided: bool, two_sided: bool, override_shading_model: bool, shading_model: string, override_opacity_mask_clip_value: bool, opacity_mask_clip_value: float, blend_mode_warning?: string, shading_model_warning?: string }

TSharedPtr<FJsonObject> HandleSetMaterialInstanceBaseOverrides(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MIPath;
	UMaterialInstanceConstant* MIC = nullptr;
	if (TSharedPtr<FJsonObject> Error = UEAgentMaterialInstanceImpl::ReadInstance(Params, MIPath, MIC)) { return Error; }

	int32 AppliedCount = 0;
	FString BlendModeWarning, ShadingModelWarning;

	MIC->Modify();

	// BlendMode
	FString BlendModeName;
	if (Params->TryGetStringField(TEXT("blend_mode"), BlendModeName))
	{
		EBlendMode Mode;
		if (ResolveBlendMode(BlendModeName, Mode))
		{
			MIC->BasePropertyOverrides.bOverride_BlendMode = true;
			MIC->BasePropertyOverrides.BlendMode = Mode;
			++AppliedCount;
			UE_LOG(LogTemp, Verbose, TEXT("set_material_instance_base_overrides: BlendMode=%s"), *BlendModeName);
		}
		else
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("Unrecognized blend_mode: %s"), *BlendModeName));
		}
	}

	// TwoSided (uint8:1 bitfield, NOT a bool)
	bool bTwoSided = false;
	if (Params->TryGetBoolField(TEXT("two_sided"), bTwoSided))
	{
		MIC->BasePropertyOverrides.bOverride_TwoSided = true;
		MIC->BasePropertyOverrides.TwoSided = bTwoSided ? 1 : 0;
		++AppliedCount;
		UE_LOG(LogTemp, Verbose, TEXT("set_material_instance_base_overrides: TwoSided=%d"), bTwoSided ? 1 : 0);
	}

	// ShadingModel
	FString ShadingModelName;
	if (Params->TryGetStringField(TEXT("shading_model"), ShadingModelName))
	{
		EMaterialShadingModel Model;
		if (ResolveShadingModel(ShadingModelName, Model))
		{
			MIC->BasePropertyOverrides.bOverride_ShadingModel = true;
			MIC->BasePropertyOverrides.ShadingModel = Model;
			++AppliedCount;
			UE_LOG(LogTemp, Verbose, TEXT("set_material_instance_base_overrides: ShadingModel=%s"), *ShadingModelName);
		}
		else
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("Unrecognized shading_model: %s"), *ShadingModelName));
		}
	}

	// OpacityMaskClipValue
	double OpacityMaskClipValue = 0.0;
	if (Params->TryGetNumberField(TEXT("opacity_mask_clip_value"), OpacityMaskClipValue))
	{
		MIC->BasePropertyOverrides.bOverride_OpacityMaskClipValue = true;
		MIC->BasePropertyOverrides.OpacityMaskClipValue = static_cast<float>(OpacityMaskClipValue);
		++AppliedCount;
		UE_LOG(LogTemp, Verbose, TEXT("set_material_instance_base_overrides: OpacityMaskClipValue=%f"), OpacityMaskClipValue);
	}

	// Routes to PostEditChangeProperty -> UpdateStaticPermutation (recompiles only if changed).
	MIC->PostEditChange();

	FString SaveError;
	const bool bSaved = SaveMICPackage(MIC, SaveError);
	if (!bSaved)
		return mcp_common::MakeErrorReply(SaveError);

	TSharedPtr<FJsonObject> Data = UEAgentMaterialInstanceImpl::NewPayload(MIPath);
	Data->SetNumberField(TEXT("applied_count"), AppliedCount);
	EmitBaseOverrides(MIC->BasePropertyOverrides, Data);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_material_instance_base_overrides"));
}

// ---------------------------------------------------------------------------
// get_material_instance_base_overrides
// ---------------------------------------------------------------------------
// MCP:get_material_instance_base_overrides - Read a material instance's base property overrides: blend mode, two-sided, shading model and opacity mask clip value, each with its own bOverride_ flag. These are the surface-level settings that decide whether something renders opaque, masked or translucent, and they are separate from parameters -- so a material that looks wrong despite correct parameters is often a blend-mode override. The flag matters as much as the value: an override that is false means the parent's setting wins whatever value is stored. set_material_instance_base_overrides writes them.
//   @param material_instance_path - Content path to the UMaterialInstanceConstant.
//   @returns { material_instance_path: string, override_blend_mode: bool, blend_mode: string, override_two_sided: bool, two_sided: bool, override_shading_model: bool, shading_model: string, override_opacity_mask_clip_value: bool, opacity_mask_clip_value: float }

TSharedPtr<FJsonObject> HandleGetMaterialInstanceBaseOverrides(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MIPath;
	UMaterialInstanceConstant* MIC = nullptr;
	if (TSharedPtr<FJsonObject> Error = UEAgentMaterialInstanceImpl::ReadInstance(Params, MIPath, MIC)) { return Error; }

	TSharedPtr<FJsonObject> Data = UEAgentMaterialInstanceImpl::NewPayload(MIPath);
	EmitBaseOverrides(MIC->BasePropertyOverrides, Data);
	return mcp_common::MakeOkReply(Data);
}

// ============================================================================
// Static-parameter surface (static switch / component mask / override flags)
// ----------------------------------------------------------------------------
// The MIC static-parameter surface (static switch bools, static component masks,
// per-parameter override flags) is authored ONLY via the WITH_EDITORONLY_DATA
// *EditorOnly setters on UMaterialInstance; nothing renders headless but the
// values persist via SavePackage. The set-by-value path for the
// static switch deliberately AVOIDS UMaterialEditingLibrary::SetMaterialInstance-
// StaticSwitchParameterValue (verified bug: its body declares bResult=false and
// never sets it true -> ALWAYS returns false even on success). We call the ENGINE
// setter directly and confirm via the getter (never key idempotency on a return).
// ============================================================================

namespace
{
	/** Resolve an EMaterialParameterAssociation from a name. GlobalParameter implies Index=INDEX_NONE; Layer/Blend take an index. */
	bool ResolveAssociation(const FString& Name, EMaterialParameterAssociation& OutAssoc)
	{
		if (Name.IsEmpty() || Name.Equals(TEXT("Global"), ESearchCase::IgnoreCase) || Name.Equals(TEXT("GlobalParameter"), ESearchCase::IgnoreCase))
		{
			OutAssoc = EMaterialParameterAssociation::GlobalParameter;
			return true;
		}
		if (Name.Equals(TEXT("Layer"), ESearchCase::IgnoreCase) || Name.Equals(TEXT("LayerParameter"), ESearchCase::IgnoreCase))
		{
			OutAssoc = EMaterialParameterAssociation::LayerParameter;
			return true;
		}
		if (Name.Equals(TEXT("Blend"), ESearchCase::IgnoreCase) || Name.Equals(TEXT("BlendParameter"), ESearchCase::IgnoreCase))
		{
			OutAssoc = EMaterialParameterAssociation::BlendParameter;
			return true;
		}
		return false;
	}

	/** Reads the optional "association" param into OutAssoc; the error reply when it names no association. */
	TSharedPtr<FJsonObject> MaterialInstance_ReadAssociation(const TSharedPtr<FJsonObject>& Params, EMaterialParameterAssociation& OutAssoc)
	{
		FString AssocName;
		Params->TryGetStringField(TEXT("association"), AssocName);
		if (!ResolveAssociation(AssocName, OutAssoc))
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("Unrecognized association: %s (expected Global/Layer/Blend)"), *AssocName));
		}
		return nullptr;
	}

	/**
	 * Build a FMaterialParameterInfo for a static parameter from optional "association"/"layer_index"
	 * fields on the request; the error reply for an unknown association. Mirrors UMaterialEditingLibrary:
	 * GlobalParameter -> Index=INDEX_NONE.
	 */
	TSharedPtr<FJsonObject> BuildStaticParamInfo(const TSharedPtr<FJsonObject>& Params, const FName& ParamFName, FMaterialParameterInfo& OutInfo, EMaterialParameterAssociation& OutAssoc)
	{
		if (TSharedPtr<FJsonObject> Error = MaterialInstance_ReadAssociation(Params, OutAssoc)) { return Error; }

		int32 LayerIndex = INDEX_NONE;
		if (OutAssoc == EMaterialParameterAssociation::GlobalParameter)
		{
			// The library forces Index=INDEX_NONE for global parameters; mirror it.
			LayerIndex = INDEX_NONE;
		}
		else
		{
			// Layer/Blend require a concrete index (defaults to 0 if omitted).
			LayerIndex = 0;
			Params->TryGetNumberField(TEXT("layer_index"), LayerIndex);
		}

		OutInfo = FMaterialParameterInfo(ParamFName, OutAssoc, LayerIndex);
		return nullptr;
	}
}

namespace UEAgentMaterialInstanceImpl
{
	// Reads the required material_instance_path and parameter_name and loads that instance; the error reply when one fails.
	TSharedPtr<FJsonObject> ReadInstanceParameter(const TSharedPtr<FJsonObject>& Params, FString& OutPath, FString& OutParamName,
		UMaterialInstanceConstant*& OutMIC)
	{
		if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_instance_path"), OutPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
		if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("parameter_name"), OutParamName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
		return FindInstance(OutPath, OutMIC);
	}

	// A reply payload that starts with material_instance_path, parameter_name and association.
	TSharedPtr<FJsonObject> NewParameterPayload(const FString& MIPath, const FString& ParamName, EMaterialParameterAssociation Assoc)
	{
		TSharedPtr<FJsonObject> Data = NewPayload(MIPath);
		Data->SetStringField(TEXT("parameter_name"), ParamName);
		Data->SetStringField(TEXT("association"), AssociationToString(Assoc));
		return Data;
	}

	// The static component mask ParamInfo names on MIC as { r, g, b, a } (all false when bOutFound is false).
	TSharedPtr<FJsonObject> ReadStaticComponentMask(UMaterialInstanceConstant* MIC, const FMaterialParameterInfo& ParamInfo, bool& bOutFound)
	{
		// AsStaticComponentMask() has an internal check(Type==StaticComponentMask),
		// so guard on the metadata Type before touching it.
		bOutFound = false;
		FStaticComponentMaskValue ReadMask;
		FMaterialParameterMetadata Meta;
		if (MIC->GetParameterValue(EMaterialParameterType::StaticComponentMask, ParamInfo, Meta)
			&& Meta.Value.Type == EMaterialParameterType::StaticComponentMask)
		{
			ReadMask = Meta.Value.AsStaticComponentMask();
			bOutFound = true;
		}

		TSharedPtr<FJsonObject> ValueObj = MakeShared<FJsonObject>();
		ValueObj->SetBoolField(TEXT("r"), ReadMask.R);
		ValueObj->SetBoolField(TEXT("g"), ReadMask.G);
		ValueObj->SetBoolField(TEXT("b"), ReadMask.B);
		ValueObj->SetBoolField(TEXT("a"), ReadMask.A);
		return ValueObj;
	}
}

// ---------------------------------------------------------------------------
// get_material_static_switch_parameters
// ---------------------------------------------------------------------------
// MCP:get_material_static_switch_parameters - List the STATIC SWITCH parameter names a material or instance exposes -- the on/off flags that compile whole branches of the shader in or out (beach, snow, detail normals, grass output). Always check these when a material looks correctly configured and still renders wrong: get_material_instance_params does not show them at all, so an instance with every texture and scalar right can still be missing a feature because a switch is false in the parent. Names only; read a value with get_material_instance_static_switch and write it with set_material_instance_static_switch. Editor-only.
//   @param material_instance_path - Content path to a UMaterialInterface (base material or instance).
//   @returns { material_instance_path: string, static_switch_parameters: [string], static_switch_count: int }

TSharedPtr<FJsonObject> HandleGetMaterialStaticSwitchParameters(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MIPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_instance_path"), MIPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

	UMaterialInterface* Mat = nullptr;
	if (TSharedPtr<FJsonObject> Error = UEAgentMaterialInstanceImpl::FindMaterial(MIPath, Mat)) { return Error; }

	TArray<FName> Names;
	UMaterialEditingLibrary::GetStaticSwitchParameterNames(Mat, Names);
	UE_LOG(LogTemp, Verbose, TEXT("get_material_static_switch_parameters: %s -> %d switch(es)"), *MIPath, Names.Num());

	TArray<TSharedPtr<FJsonValue>> NamesArray;
	for (const FName& N : Names)
	{
		NamesArray.Add(MakeShared<FJsonValueString>(N.ToString()));
	}

	TSharedPtr<FJsonObject> Data = UEAgentMaterialInstanceImpl::NewPayload(MIPath);
	Data->SetArrayField(TEXT("static_switch_parameters"), NamesArray);
	Data->SetNumberField(TEXT("static_switch_count"), Names.Num());
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_material_instance_static_switch
// ---------------------------------------------------------------------------
// MCP:set_material_instance_static_switch - Set a static-switch (static bool) parameter override on a Material Instance Constant, then rebuild the static permutation and persist. Reads the value back via the getter (the engine value-setter wrapper always returns false, so its return is never trusted). Editor-only.
//   @param material_instance_path - Content path to the UMaterialInstanceConstant.
//   @param parameter_name - Name of the static-switch parameter.
//   @param value - Bool value to assign to the switch.
//   @param association - Optional EMaterialParameterAssociation: "Global" (default), "Layer", or "Blend".
//   @param layer_index - Optional zero-based index for Layer/Blend association (ignored for Global).
//   @returns { material_instance_path: string, parameter_name: string, association: string, requested_value: bool, value: bool, found: bool, saved: bool }

TSharedPtr<FJsonObject> HandleSetMaterialInstanceStaticSwitch(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MIPath, ParamName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_instance_path"), MIPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("parameter_name"), ParamName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

	bool bValue = false;
	if (!Params->TryGetBoolField(TEXT("value"), bValue))
		return mcp_common::MakeErrorReply(TEXT("Missing required parameter: value (bool)"));

	UMaterialInstanceConstant* MIC = nullptr;
	if (TSharedPtr<FJsonObject> Error = UEAgentMaterialInstanceImpl::FindInstance(MIPath, MIC)) { return Error; }

	const FName ParamFName(*ParamName);
	FMaterialParameterInfo ParamInfo;
	EMaterialParameterAssociation Assoc;
	if (TSharedPtr<FJsonObject> Error = BuildStaticParamInfo(Params, ParamFName, ParamInfo, Assoc)) { return Error; }

	MIC->Modify();
	// ENGINE setter (UMaterialInstance::SetStaticSwitchParameterValueEditorOnly): real body, de-dups by ParameterInfo.
	// We bypass the broken library value-setter wrapper entirely.
	MIC->SetStaticSwitchParameterValueEditorOnly(ParamInfo, bValue);

	// Apply the static permutation (UpdateStaticPermutation) BEFORE saving.
	UMaterialEditingLibrary::UpdateMaterialInstance(MIC);
	MIC->PostEditChange();
	UE_LOG(LogTemp, Verbose, TEXT("set_material_instance_static_switch: %s.%s [%s] = %d"),
		*MIPath, *ParamName, *AssociationToString(Assoc), bValue ? 1 : 0);

	FString SaveError;
	const bool bSaved = SaveMICPackage(MIC, SaveError);
	if (!bSaved)
		return mcp_common::MakeErrorReply(SaveError);

	// Read back via the getter (never trust the value-setter return).
	const bool bReadValue = UMaterialEditingLibrary::GetMaterialInstanceStaticSwitchParameterValue(MIC, ParamFName, Assoc);
	const bool bFound = UMaterialEditingLibrary::IsMaterialInstanceParameterOverridden(MIC, ParamFName, Assoc);

	TSharedPtr<FJsonObject> Data = UEAgentMaterialInstanceImpl::NewParameterPayload(MIPath, ParamName, Assoc);
	Data->SetBoolField(TEXT("requested_value"), bValue);
	Data->SetBoolField(TEXT("value"), bReadValue);
	Data->SetBoolField(TEXT("found"), bFound);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_material_instance_static_switch"));
}

// ---------------------------------------------------------------------------
// get_material_instance_static_switch
// ---------------------------------------------------------------------------
// MCP:get_material_instance_static_switch - Read a static-switch (static bool) parameter value from a Material Instance Constant (via UMaterialEditingLibrary::GetMaterialInstanceStaticSwitchParameterValue). Editor-only.
//   @param material_instance_path - Content path to the UMaterialInstanceConstant.
//   @param parameter_name - Name of the static-switch parameter.
//   @param association - Optional EMaterialParameterAssociation: "Global" (default), "Layer", or "Blend".
//   @param layer_index - Optional zero-based index for Layer/Blend association (ignored for Global).
//   @returns { material_instance_path: string, parameter_name: string, association: string, value: bool, overridden: bool }

TSharedPtr<FJsonObject> HandleGetMaterialInstanceStaticSwitch(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MIPath, ParamName;
	UMaterialInstanceConstant* MIC = nullptr;
	if (TSharedPtr<FJsonObject> Error = UEAgentMaterialInstanceImpl::ReadInstanceParameter(Params, MIPath, ParamName, MIC)) { return Error; }

	const FName ParamFName(*ParamName);
	FMaterialParameterInfo ParamInfo;
	EMaterialParameterAssociation Assoc;
	if (TSharedPtr<FJsonObject> Error = BuildStaticParamInfo(Params, ParamFName, ParamInfo, Assoc)) { return Error; }

	const bool bValue = UMaterialEditingLibrary::GetMaterialInstanceStaticSwitchParameterValue(MIC, ParamFName, Assoc);
	const bool bOverridden = UMaterialEditingLibrary::IsMaterialInstanceParameterOverridden(MIC, ParamFName, Assoc);

	TSharedPtr<FJsonObject> Data = UEAgentMaterialInstanceImpl::NewParameterPayload(MIPath, ParamName, Assoc);
	Data->SetBoolField(TEXT("value"), bValue);
	Data->SetBoolField(TEXT("overridden"), bOverridden);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_material_instance_static_component_mask
// ---------------------------------------------------------------------------
// MCP:set_material_instance_static_component_mask - Set a static component-mask (R/G/B/A bools) parameter override on a Material Instance Constant, rebuild the static permutation and persist. Reads back via GetParameterValue. Editor-only.
//   @param material_instance_path - Content path to the UMaterialInstanceConstant.
//   @param parameter_name - Name of the static component-mask parameter.
//   @param r - Optional bool channel mask (default false).
//   @param g - Optional bool channel mask (default false).
//   @param b - Optional bool channel mask (default false).
//   @param a - Optional bool channel mask (default false).
//   @param association - Optional EMaterialParameterAssociation: "Global" (default), "Layer", or "Blend".
//   @param layer_index - Optional zero-based index for Layer/Blend association (ignored for Global).
//   @returns { material_instance_path: string, parameter_name: string, association: string, requested: {r,g,b,a}, value: {r,g,b,a}, found: bool, saved: bool }

TSharedPtr<FJsonObject> HandleSetMaterialInstanceStaticComponentMask(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MIPath, ParamName;
	UMaterialInstanceConstant* MIC = nullptr;
	if (TSharedPtr<FJsonObject> Error = UEAgentMaterialInstanceImpl::ReadInstanceParameter(Params, MIPath, ParamName, MIC)) { return Error; }

	// Channels default to false; each provided channel sets its bit.
	bool bR = false, bG = false, bB = false, bA = false;
	Params->TryGetBoolField(TEXT("r"), bR);
	Params->TryGetBoolField(TEXT("g"), bG);
	Params->TryGetBoolField(TEXT("b"), bB);
	Params->TryGetBoolField(TEXT("a"), bA);

	const FName ParamFName(*ParamName);
	FMaterialParameterInfo ParamInfo;
	EMaterialParameterAssociation Assoc;
	if (TSharedPtr<FJsonObject> Error = BuildStaticParamInfo(Params, ParamFName, ParamInfo, Assoc)) { return Error; }

	MIC->Modify();
	// ENGINE setter takes FStaticComponentMaskValue by value (NO 4-bool overload on the setter); de-dups by ParameterInfo.
	MIC->SetStaticComponentMaskParameterValueEditorOnly(ParamInfo, FStaticComponentMaskValue(bR, bG, bB, bA));

	UMaterialEditingLibrary::UpdateMaterialInstance(MIC);
	MIC->PostEditChange();
	UE_LOG(LogTemp, Verbose, TEXT("set_material_instance_static_component_mask: %s.%s [%s] = R%d G%d B%d A%d"),
		*MIPath, *ParamName, *AssociationToString(Assoc), bR ? 1 : 0, bG ? 1 : 0, bB ? 1 : 0, bA ? 1 : 0);

	FString SaveError;
	const bool bSaved = SaveMICPackage(MIC, SaveError);
	if (!bSaved)
		return mcp_common::MakeErrorReply(SaveError);

	// Read back the resolved mask.
	bool bFound = false;
	TSharedPtr<FJsonObject> ValueObj = UEAgentMaterialInstanceImpl::ReadStaticComponentMask(MIC, ParamInfo, bFound);

	TSharedPtr<FJsonObject> Requested = MakeShared<FJsonObject>();
	Requested->SetBoolField(TEXT("r"), bR);
	Requested->SetBoolField(TEXT("g"), bG);
	Requested->SetBoolField(TEXT("b"), bB);
	Requested->SetBoolField(TEXT("a"), bA);

	TSharedPtr<FJsonObject> Data = UEAgentMaterialInstanceImpl::NewParameterPayload(MIPath, ParamName, Assoc);
	Data->SetObjectField(TEXT("requested"), Requested);
	Data->SetObjectField(TEXT("value"), ValueObj);
	Data->SetBoolField(TEXT("found"), bFound);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_material_instance_static_component_mask"));
}

// ---------------------------------------------------------------------------
// get_material_instance_static_component_mask
// ---------------------------------------------------------------------------
// MCP:get_material_instance_static_component_mask - Read a static component-mask parameter (the R/G/B/A channel checkboxes) on a material instance -- which channels of a packed mask/ORM texture the shader actually samples. Reading the wrong channel is a classic silent look bug: nothing errors, the surface is just uniformly wrong. Supports Global / Layer / Blend association with a layer index. set_material_instance_static_component_mask writes it. Editor-only.
//   @param material_instance_path - Content path to the UMaterialInstanceConstant.
//   @param parameter_name - Name of the static component-mask parameter.
//   @param association - Optional EMaterialParameterAssociation: "Global" (default), "Layer", or "Blend".
//   @param layer_index - Optional zero-based index for Layer/Blend association (ignored for Global).
//   @returns { material_instance_path: string, parameter_name: string, association: string, value: {r,g,b,a}, found: bool }

TSharedPtr<FJsonObject> HandleGetMaterialInstanceStaticComponentMask(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MIPath, ParamName;
	UMaterialInstanceConstant* MIC = nullptr;
	if (TSharedPtr<FJsonObject> Error = UEAgentMaterialInstanceImpl::ReadInstanceParameter(Params, MIPath, ParamName, MIC)) { return Error; }

	const FName ParamFName(*ParamName);
	FMaterialParameterInfo ParamInfo;
	EMaterialParameterAssociation Assoc;
	if (TSharedPtr<FJsonObject> Error = BuildStaticParamInfo(Params, ParamFName, ParamInfo, Assoc)) { return Error; }

	bool bFound = false;
	TSharedPtr<FJsonObject> ValueObj = UEAgentMaterialInstanceImpl::ReadStaticComponentMask(MIC, ParamInfo, bFound);

	TSharedPtr<FJsonObject> Data = UEAgentMaterialInstanceImpl::NewParameterPayload(MIPath, ParamName, Assoc);
	Data->SetObjectField(TEXT("value"), ValueObj);
	Data->SetBoolField(TEXT("found"), bFound);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_material_instance_param_override
// ---------------------------------------------------------------------------
// MCP:set_material_instance_param_override - Toggle the per-parameter override flag on a Material Instance Constant (any of the 11 parameter types, including static switch). bOverride=true keeps/enables the local override; bOverride=false REVERTS the parameter to the parent material's value (removes the override). Runs UpdateMaterialInstance and persists. Returns honestly: success=false when the parameter is not found on the instance. Editor-only.
//   @param material_instance_path - Content path to the UMaterialInstanceConstant.
//   @param parameter_name - Name of the parameter whose override flag to toggle.
//   @param override - Bool: true = keep local override; false = revert to parent (remove the value).
//   @param association - Optional EMaterialParameterAssociation: "Global" (default), "Layer", or "Blend".
//   @returns { material_instance_path: string, parameter_name: string, association: string, requested_override: bool, applied: bool, overridden: bool, saved: bool }

TSharedPtr<FJsonObject> HandleSetMaterialInstanceParamOverride(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MIPath, ParamName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_instance_path"), MIPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("parameter_name"), ParamName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

	bool bOverride = false;
	if (!Params->TryGetBoolField(TEXT("override"), bOverride))
		return mcp_common::MakeErrorReply(TEXT("Missing required parameter: override (bool)"));

	UMaterialInstanceConstant* MIC = nullptr;
	if (TSharedPtr<FJsonObject> Error = UEAgentMaterialInstanceImpl::FindInstance(MIPath, MIC)) { return Error; }

	EMaterialParameterAssociation Assoc;
	if (TSharedPtr<FJsonObject> Error = MaterialInstance_ReadAssociation(Params, Assoc)) { return Error; }

	const FName ParamFName(*ParamName);

	MIC->Modify();
	// SetMaterialInstanceParameterOverride handles all 11 parameter types, removes the value when
	// bOverride=false (reverts to parent), runs UpdateMaterialInstance internally, and returns
	// false honestly when the parameter is not found on the instance.
	const bool bApplied = UMaterialEditingLibrary::SetMaterialInstanceParameterOverride(MIC, ParamFName, bOverride, Assoc);
	MIC->PostEditChange();
	UE_LOG(LogTemp, Verbose, TEXT("set_material_instance_param_override: %s.%s [%s] override=%d -> applied=%d"),
		*MIPath, *ParamName, *AssociationToString(Assoc), bOverride ? 1 : 0, bApplied ? 1 : 0);

	if (!bApplied)
	{
		// Honest: the parameter was not found on this instance (or null). Surface a clean failure.
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Parameter override not applied (parameter '%s' not found on instance %s)"), *ParamName, *MIPath));
	}

	FString SaveError;
	const bool bSaved = SaveMICPackage(MIC, SaveError);
	if (!bSaved)
		return mcp_common::MakeErrorReply(SaveError);

	// Read back the resulting override flag.
	const bool bOverridden = UMaterialEditingLibrary::IsMaterialInstanceParameterOverridden(MIC, ParamFName, Assoc);

	TSharedPtr<FJsonObject> Data = UEAgentMaterialInstanceImpl::NewParameterPayload(MIPath, ParamName, Assoc);
	Data->SetBoolField(TEXT("requested_override"), bOverride);
	Data->SetBoolField(TEXT("applied"), bApplied);
	Data->SetBoolField(TEXT("overridden"), bOverridden);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_material_instance_param_override"));
}

// ---------------------------------------------------------------------------
// get_material_instance_param_overridden
// ---------------------------------------------------------------------------
// MCP:get_material_instance_param_overridden - Is this parameter set on the INSTANCE, or just inherited from the parent? Returns the override flag for one named parameter -- the discriminator that a plain value read cannot give you, since an inherited value and a locally-set identical value read the same. Use it to confirm a write actually landed on the child (and, in a read-only vendor material, to prove you edited your instance and not the library asset). Editor-only.
//   @param material_instance_path - Content path to the UMaterialInstanceConstant.
//   @param parameter_name - Name of the parameter to query.
//   @param association - Optional EMaterialParameterAssociation: "Global" (default), "Layer", or "Blend".
//   @returns { material_instance_path: string, parameter_name: string, association: string, overridden: bool }

TSharedPtr<FJsonObject> HandleGetMaterialInstanceParamOverridden(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MIPath, ParamName;
	UMaterialInstanceConstant* MIC = nullptr;
	if (TSharedPtr<FJsonObject> Error = UEAgentMaterialInstanceImpl::ReadInstanceParameter(Params, MIPath, ParamName, MIC)) { return Error; }

	EMaterialParameterAssociation Assoc;
	if (TSharedPtr<FJsonObject> Error = MaterialInstance_ReadAssociation(Params, Assoc)) { return Error; }

	const FName ParamFName(*ParamName);
	const bool bOverridden = UMaterialEditingLibrary::IsMaterialInstanceParameterOverridden(MIC, ParamFName, Assoc);

	TSharedPtr<FJsonObject> Data = UEAgentMaterialInstanceImpl::NewParameterPayload(MIPath, ParamName, Assoc);
	Data->SetBoolField(TEXT("overridden"), bOverridden);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// Create + inspect composition.
// ---------------------------------------------------------------------------
// MCP:create_material_instance_constant - Create a material instance (MIC) asset under a /Game path, optionally parented to a base material on creation. The right way to re-skin a vendor or engine material: the instance is yours, the parent stays untouched and updatable, and only the parameters you override are stored. Idempotent -- an existing asset comes back with already_existed=true. Same operation as create_material_instance, which addresses the target by folder plus name instead of a full path.
//   @param name - Name of the new material instance asset.
//   @param package_path - Target content folder; defaults to /Game/MCP.
//   @param parent_material_path - Optional content path of a UMaterialInterface to set as Parent (InitialParent on the factory).
//   @returns { material_instance_path: string, material_instance_name: string, parent_material_path: string, created: bool, already_existed: bool, saved: bool, save_error?: string }
TSharedPtr<FJsonObject> HandleCreateMaterialInstanceConstant(
	const TSharedPtr<FJsonObject>& Params)
{
	FString Name;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name)) { return Missing; }
	FString PackagePath = mcp_common::DefaultAssetFolder();
	Params->TryGetStringField(TEXT("package_path"), PackagePath);
	while (PackagePath.EndsWith(TEXT("/"))) { PackagePath.LeftChopInline(1, EAllowShrinking::No); }

	FString ParentMaterialPath;
	Params->TryGetStringField(TEXT("parent_material_path"), ParentMaterialPath);

	const FString FullPath = FString::Printf(TEXT("%s/%s"), *PackagePath, *Name);
	if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(FullPath)) { return Invalid; }
	const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *FullPath, *Name);

	// Idempotent load-or-create.
	if (UMaterialInstanceConstant* Existing = LoadMICFlexible(ObjectPath))
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("material_instance_path"), Existing->GetPathName());
		Data->SetStringField(TEXT("material_instance_name"), Existing->GetName());
		Data->SetStringField(TEXT("parent_material_path"), Existing->Parent ? Existing->Parent->GetPathName() : TEXT("None"));
		Data->SetBoolField(TEXT("created"), false);
		Data->SetBoolField(TEXT("already_existed"), true);
		return mcp_common::MakeOkReply(Data);
	}

	FString CreateError;
	UMaterialInstanceConstant* MIC = mcp_common::NewAssetInPackage<UMaterialInstanceConstant>(FullPath, Name,
		*FString::Printf(TEXT("Failed to create UMaterialInstanceConstant: %s"), *Name), CreateError);
	if (!MIC)
	{
		return mcp_common::MakeErrorReply(CreateError);
	}

	// Seed parent if requested — use the editor-only setter; this also triggers an
	// internal recompile so that the parameter lists resolve from the parent.
	if (!ParentMaterialPath.IsEmpty())
	{
		UMaterialInterface* Parent = mcp_common::LoadAssetOrSoftPath<UMaterialInterface>(ParentMaterialPath);
		if (!Parent)
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("Parent material not found: %s"), *ParentMaterialPath));
		}
		MIC->SetParentEditorOnly(Parent, /*RecacheShader=*/ true);
	}

	FAssetRegistryModule::AssetCreated(MIC);
	MIC->MarkPackageDirty();

	FString SaveError;
	const bool bSaved = mcp_common::SaveAssetPackage(MIC->GetPackage(), MIC, SaveError);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("material_instance_path"), MIC->GetPathName());
	Data->SetStringField(TEXT("material_instance_name"), MIC->GetName());
	Data->SetStringField(TEXT("parent_material_path"), MIC->Parent ? MIC->Parent->GetPathName() : TEXT("None"));
	Data->SetBoolField(TEXT("created"), true);
	Data->SetBoolField(TEXT("already_existed"), false);
	if (!SaveError.IsEmpty())
	{
		Data->SetStringField(TEXT("save_error"), SaveError);
	}
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("create_material_instance_constant"));
}

// MCP:inspect_material_instance_constant - One-call overview of a material instance: parent material and class, plus COUNTS of scalar/vector/texture overrides, base property overrides, static switches and static component masks. A triage read that tells you which of the detailed tools to reach for next -- it returns counts, not values, so it can say "there are 14 static switches here" but never which ones are on. For values use get_material_all_parameters, get_material_instance_static_switch and get_material_instance_base_overrides.
//   @param material_instance_path - Content path to the UMaterialInstanceConstant.
//   @returns { material_instance_path: string, material_instance_class: string, parent_material_path: string, parent_material_class: string, scalar_param_count: int, vector_param_count: int, texture_param_count: int, base_override_count: int, static_switch_count: int, static_component_mask_count: int, asset_registry_path: string }
TSharedPtr<FJsonObject> HandleInspectMaterialInstanceConstant(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MIPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_instance_path"), MIPath)) { return Missing; }
	UMaterialInstanceConstant* MIC = nullptr;
	if (TSharedPtr<FJsonObject> Error = UEAgentMaterialInstanceImpl::FindInstance(MIPath, MIC)) { return Error; }

	int32 ScalarCount = MIC->ScalarParameterValues.Num();
	int32 VectorCount = MIC->VectorParameterValues.Num();
	int32 TextureCount = MIC->TextureParameterValues.Num();

	int32 StaticSwitchCount = 0;
	int32 StaticComponentMaskCount = 0;
	int32 BaseOverrideCount = 0;
	{
		// UE 5.8: GetStaticParameters() returns FStaticParameterSet by value (composes runtime + editor-only data).
		const FStaticParameterSet RuntimeParams = MIC->GetStaticParameters();
		StaticSwitchCount = RuntimeParams.StaticSwitchParameters.Num();
		// StaticComponentMaskParameters lives only on the editor-only-data side (deprecated in FStaticParameterSet);
		// the canonical access is via the FStaticParameterSetEditorOnlyData struct.
		const FStaticParameterSetEditorOnlyData& EditorParams = MIC->GetEditorOnlyStaticParameters();
		StaticComponentMaskCount = EditorParams.StaticComponentMaskParameters.Num();
		// Walk the bOverride_* flags via reflection — counts how many base-override toggles are set.
		// FMaterialInstanceBasePropertyOverrides is a USTRUCT (not a UObject), so use StaticStruct().
		for (TFieldIterator<FProperty> It(FMaterialInstanceBasePropertyOverrides::StaticStruct()); It; ++It)
		{
			FProperty* Prop = *It;
			if (!Prop->GetName().StartsWith(TEXT("bOverride_")))
			{
				continue;
			}
			if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
			{
				if (BoolProp->GetPropertyValue_InContainer(&MIC->BasePropertyOverrides))
				{
					++BaseOverrideCount;
				}
			}
		}
	}

	TSharedPtr<FJsonObject> Data = UEAgentMaterialInstanceImpl::NewPayload(MIPath);
	Data->SetStringField(TEXT("material_instance_class"), MIC->GetClass()->GetName());
	Data->SetStringField(TEXT("parent_material_path"), MIC->Parent ? MIC->Parent->GetPathName() : TEXT("None"));
	Data->SetStringField(TEXT("parent_material_class"), MIC->Parent ? MIC->Parent->GetClass()->GetName() : TEXT("None"));
	Data->SetNumberField(TEXT("scalar_param_count"), ScalarCount);
	Data->SetNumberField(TEXT("vector_param_count"), VectorCount);
	Data->SetNumberField(TEXT("texture_param_count"), TextureCount);
	Data->SetNumberField(TEXT("static_switch_count"), StaticSwitchCount);
	Data->SetNumberField(TEXT("static_component_mask_count"), StaticComponentMaskCount);
	Data->SetNumberField(TEXT("base_override_count"), BaseOverrideCount);
	Data->SetStringField(TEXT("asset_registry_path"), MIC->GetPathName());
	return mcp_common::MakeOkReply(Data);
}

namespace UEAgentMaterialInstanceImpl
{
	// Composite key so a global param and a layer/blend-scoped param that share a name never collide.
	FString DiffParameterKey(const FMaterialParameterInfo& Info)
	{
		return FString::Printf(TEXT("%s|%d|%d"), *Info.Name.ToString(),
			static_cast<int32>(Info.Association), Info.Index);
	}

	// Emit the parameter identity (name + association/layer for non-global scopes) into a JSON object.
	void EmitDiffParameterName(const FMaterialParameterInfo& Info, const TSharedPtr<FJsonObject>& Obj)
	{
		Obj->SetStringField(TEXT("name"), Info.Name.ToString());
		if (Info.Association != EMaterialParameterAssociation::GlobalParameter)
		{
			Obj->SetStringField(TEXT("association"), AssociationToString(Info.Association));
			Obj->SetNumberField(TEXT("layer_index"), Info.Index);
		}
	}

	// { changed, only_in_a, only_in_b } and their counts over the entries Include accepts, matched by ParameterInfo.
	template <typename ParameterType, typename IncludeFn, typename ValueFn, typename DiffersFn>
	TSharedPtr<FJsonObject> DiffParameterLists(const TArray<ParameterType>& ListA, const TArray<ParameterType>& ListB,
		IncludeFn Include, ValueFn ToValue, DiffersFn Differs, int32& InOutTotal)
	{
		TMap<FString, int32> IdxA, IdxB;
		for (int32 i = 0; i < ListA.Num(); ++i)
		{
			if (Include(ListA[i])) { IdxA.Add(DiffParameterKey(ListA[i].ParameterInfo), i); }
		}
		for (int32 i = 0; i < ListB.Num(); ++i)
		{
			if (Include(ListB[i])) { IdxB.Add(DiffParameterKey(ListB[i].ParameterInfo), i); }
		}

		TArray<TSharedPtr<FJsonValue>> Changed, OnlyA, OnlyB;
		for (const TPair<FString, int32>& Pair : IdxA)
		{
			const ParameterType& EntryA = ListA[Pair.Value];
			if (const int32* Bi = IdxB.Find(Pair.Key))
			{
				const ParameterType& EntryB = ListB[*Bi];
				if (Differs(EntryA, EntryB))
				{
					TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
					EmitDiffParameterName(EntryA.ParameterInfo, O);
					O->SetField(TEXT("value_a"), ToValue(EntryA));
					O->SetField(TEXT("value_b"), ToValue(EntryB));
					Changed.Add(MakeShared<FJsonValueObject>(O));
				}
			}
			else
			{
				TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
				EmitDiffParameterName(EntryA.ParameterInfo, O);
				O->SetField(TEXT("value"), ToValue(EntryA));
				OnlyA.Add(MakeShared<FJsonValueObject>(O));
			}
		}
		for (const TPair<FString, int32>& Pair : IdxB)
		{
			if (IdxA.Contains(Pair.Key)) { continue; }
			const ParameterType& EntryB = ListB[Pair.Value];
			TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
			EmitDiffParameterName(EntryB.ParameterInfo, O);
			O->SetField(TEXT("value"), ToValue(EntryB));
			OnlyB.Add(MakeShared<FJsonValueObject>(O));
		}
		InOutTotal += Changed.Num() + OnlyA.Num() + OnlyB.Num();

		TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
		Diff->SetArrayField(TEXT("changed"), Changed);
		Diff->SetArrayField(TEXT("only_in_a"), OnlyA);
		Diff->SetArrayField(TEXT("only_in_b"), OnlyB);
		Diff->SetNumberField(TEXT("changed_count"), Changed.Num());
		Diff->SetNumberField(TEXT("only_in_a_count"), OnlyA.Num());
		Diff->SetNumberField(TEXT("only_in_b_count"), OnlyB.Num());
		return Diff;
	}

	// { index, function_a, function_b } per layer or blend slot whose function differs ("(absent)" past one side's end).
	TArray<TSharedPtr<FJsonValue>> DiffStackSlots(const TArray<TObjectPtr<UMaterialFunctionInterface>>& SlotsA,
		const TArray<TObjectPtr<UMaterialFunctionInterface>>& SlotsB)
	{
		auto FuncPathAt = [](const TArray<TObjectPtr<UMaterialFunctionInterface>>& Arr, int32 Index) -> FString
		{
			if (!Arr.IsValidIndex(Index)) { return TEXT("(absent)"); }
			return Arr[Index] ? Arr[Index]->GetPathName() : TEXT("None");
		};

		TArray<TSharedPtr<FJsonValue>> Diffs;
		const int32 MaxSlots = FMath::Max(SlotsA.Num(), SlotsB.Num());
		for (int32 i = 0; i < MaxSlots; ++i)
		{
			const FString Fa = FuncPathAt(SlotsA, i);
			const FString Fb = FuncPathAt(SlotsB, i);
			if (!Fa.Equals(Fb))
			{
				TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
				O->SetNumberField(TEXT("index"), i);
				O->SetStringField(TEXT("function_a"), Fa);
				O->SetStringField(TEXT("function_b"), Fb);
				Diffs.Add(MakeShared<FJsonValueObject>(O));
			}
		}
		return Diffs;
	}
}

// ---------------------------------------------------------------------------
// diff_material_instances
// ---------------------------------------------------------------------------
// MCP:diff_material_instances - Diff two Material Instance Constants: what side B overrides differ from side A (parent + scalar/vector/texture/static-switch overrides + material layer stack). Answers "what did this fork change vs its parent (or the original)". Read-only; nothing is saved. Editor-only.
//   @param material_instance_path_a - Content path to the first (baseline) UMaterialInstanceConstant.
//   @param material_instance_path_b - Content path to the second (fork) UMaterialInstanceConstant.
//   @returns { material_instance_path_a: string, material_instance_path_b: string, parent: {a, b, same}, scalars: {changed: [{name, association?, layer_index?, value_a, value_b}], only_in_a: [{name, value}], only_in_b: [{name, value}], changed_count, only_in_a_count, only_in_b_count}, vectors: {changed: [{name, value_a:{r,g,b,a}, value_b:{r,g,b,a}}], only_in_a, only_in_b, ...counts}, textures: {changed: [{name, value_a, value_b}], only_in_a, only_in_b, ...counts}, static_switches: {changed: [{name, value_a, value_b}], only_in_a, only_in_b, ...counts}, layer_stack: {layer_count_a, layer_count_b, blend_count_a, blend_count_b, layer_diffs: [{index, function_a, function_b}], blend_diffs: [{index, function_a, function_b}]}, total_differences: int, identical: bool }
TSharedPtr<FJsonObject> HandleDiffMaterialInstances(
	const TSharedPtr<FJsonObject>& Params)
{
	FString PathA, PathB;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_instance_path_a"), PathA)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_instance_path_b"), PathB)) { return Missing; }

	UMaterialInstanceConstant* A = LoadMICFlexible(PathA);
	if (!A)
		return mcp_common::MakeErrorReply(FString::Printf(TEXT("Material instance not found (a): %s"), *PathA));
	UMaterialInstanceConstant* B = LoadMICFlexible(PathB);
	if (!B)
		return mcp_common::MakeErrorReply(FString::Printf(TEXT("Material instance not found (b): %s"), *PathB));

	int32 TotalDifferences = 0;

	// --- Parent ---
	const FString ParentA = A->Parent ? A->Parent->GetPathName() : TEXT("None");
	const FString ParentB = B->Parent ? B->Parent->GetPathName() : TEXT("None");
	const bool bParentSame = ParentA.Equals(ParentB);
	if (!bParentSame) { ++TotalDifferences; }

	TSharedPtr<FJsonObject> ParentObj = MakeShared<FJsonObject>();
	ParentObj->SetStringField(TEXT("a"), ParentA);
	ParentObj->SetStringField(TEXT("b"), ParentB);
	ParentObj->SetBoolField(TEXT("same"), bParentSame);

	// Each parameter kind is matched by ParameterInfo and compared by value.
	const auto EveryEntry = [](const auto&) { return true; };

	// --- Scalars ---
	TSharedPtr<FJsonObject> ScalarsObj = UEAgentMaterialInstanceImpl::DiffParameterLists(
		A->ScalarParameterValues, B->ScalarParameterValues, EveryEntry,
		[](const FScalarParameterValue& P) -> TSharedPtr<FJsonValue> { return MakeShared<FJsonValueNumber>(P.ParameterValue); },
		[](const FScalarParameterValue& X, const FScalarParameterValue& Y) { return X.ParameterValue != Y.ParameterValue; },
		TotalDifferences);

	// --- Vectors ---
	TSharedPtr<FJsonObject> VectorsObj = UEAgentMaterialInstanceImpl::DiffParameterLists(
		A->VectorParameterValues, B->VectorParameterValues, EveryEntry,
		[](const FVectorParameterValue& P) -> TSharedPtr<FJsonValue> { return MakeShared<FJsonValueObject>(mcp_common::LinearColorToJsonObject(P.ParameterValue)); },
		[](const FVectorParameterValue& X, const FVectorParameterValue& Y) { return X.ParameterValue != Y.ParameterValue; },
		TotalDifferences);

	// --- Textures (compared by content path) ---
	auto TexPath = [](UTexture* T) -> FString { return T ? T->GetPathName() : TEXT("None"); };
	TSharedPtr<FJsonObject> TexturesObj = UEAgentMaterialInstanceImpl::DiffParameterLists(
		A->TextureParameterValues, B->TextureParameterValues, EveryEntry,
		[&TexPath](const FTextureParameterValue& P) -> TSharedPtr<FJsonValue> { return MakeShared<FJsonValueString>(TexPath(P.ParameterValue)); },
		[&TexPath](const FTextureParameterValue& X, const FTextureParameterValue& Y) { return !TexPath(X.ParameterValue).Equals(TexPath(Y.ParameterValue)); },
		TotalDifferences);

	// --- Static switches (only actually-overridden entries, i.e. bOverride == true) ---
	// GetStaticParameters() returns FStaticParameterSet by value; StaticSwitchParameters carries
	// per-entry bOverride flags. We diff only entries the instance actually overrides.
	const FStaticParameterSet StaticA = A->GetStaticParameters();
	const FStaticParameterSet StaticB = B->GetStaticParameters();
	TSharedPtr<FJsonObject> SwitchesObj = UEAgentMaterialInstanceImpl::DiffParameterLists(
		StaticA.StaticSwitchParameters, StaticB.StaticSwitchParameters,
		[](const FStaticSwitchParameter& P) { return P.bOverride; },
		[](const FStaticSwitchParameter& P) -> TSharedPtr<FJsonValue> { return MakeShared<FJsonValueBoolean>(P.Value); },
		[](const FStaticSwitchParameter& X, const FStaticSwitchParameter& Y) { return X.Value != Y.Value; },
		TotalDifferences);

	// --- Material layer stack (per-slot function-path comparison) ---
	TSharedPtr<FJsonObject> LayerStackObj = MakeShared<FJsonObject>();
	{
		FMaterialLayersFunctions La, Lb;
		A->GetMaterialLayers(La);
		B->GetMaterialLayers(Lb);

		const TArray<TSharedPtr<FJsonValue>> LayerDiffs = UEAgentMaterialInstanceImpl::DiffStackSlots(La.Layers, Lb.Layers);
		const TArray<TSharedPtr<FJsonValue>> BlendDiffs = UEAgentMaterialInstanceImpl::DiffStackSlots(La.Blends, Lb.Blends);

		TotalDifferences += LayerDiffs.Num() + BlendDiffs.Num();
		LayerStackObj->SetNumberField(TEXT("layer_count_a"), La.Layers.Num());
		LayerStackObj->SetNumberField(TEXT("layer_count_b"), Lb.Layers.Num());
		LayerStackObj->SetNumberField(TEXT("blend_count_a"), La.Blends.Num());
		LayerStackObj->SetNumberField(TEXT("blend_count_b"), Lb.Blends.Num());
		LayerStackObj->SetArrayField(TEXT("layer_diffs"), LayerDiffs);
		LayerStackObj->SetArrayField(TEXT("blend_diffs"), BlendDiffs);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("material_instance_path_a"), PathA);
	Data->SetStringField(TEXT("material_instance_path_b"), PathB);
	Data->SetObjectField(TEXT("parent"), ParentObj);
	Data->SetObjectField(TEXT("scalars"), ScalarsObj);
	Data->SetObjectField(TEXT("vectors"), VectorsObj);
	Data->SetObjectField(TEXT("textures"), TexturesObj);
	Data->SetObjectField(TEXT("static_switches"), SwitchesObj);
	Data->SetObjectField(TEXT("layer_stack"), LayerStackObj);
	Data->SetNumberField(TEXT("total_differences"), TotalDifferences);
	Data->SetBoolField(TEXT("identical"), TotalDifferences == 0);
	return mcp_common::MakeOkReply(Data);
}

const McpCommandFamilies::FRegistrar Registration =
{
	{ TEXT("clear_material_parameter_overrides"), &HandleClearMaterialParameterOverrides },
	{ TEXT("copy_material_instance_params"), &HandleCopyMaterialInstanceParams },
	{ TEXT("create_material_instance_constant"), &HandleCreateMaterialInstanceConstant },
	{ TEXT("diff_material_instances"), &HandleDiffMaterialInstances },
	{ TEXT("get_material_all_parameters"), &HandleGetMaterialAllParameters },
	{ TEXT("get_material_instance_base_overrides"), &HandleGetMaterialInstanceBaseOverrides },
	{ TEXT("get_material_instance_param_overridden"), &HandleGetMaterialInstanceParamOverridden },
	{ TEXT("get_material_instance_params"), &HandleGetMaterialInstanceParams },
	{ TEXT("get_material_instance_parent"), &HandleGetMaterialInstanceParent },
	{ TEXT("get_material_instance_static_component_mask"), &HandleGetMaterialInstanceStaticComponentMask },
	{ TEXT("get_material_instance_static_switch"), &HandleGetMaterialInstanceStaticSwitch },
	{ TEXT("get_material_layer_parameter"), &HandleGetMaterialLayerParameter },
	{ TEXT("get_material_layer_stack"), &HandleGetMaterialLayerStack },
	{ TEXT("get_material_param_info"), &HandleGetMaterialParamInfo },
	{ TEXT("get_material_static_switch_parameters"), &HandleGetMaterialStaticSwitchParameters },
	{ TEXT("inspect_material_instance_constant"), &HandleInspectMaterialInstanceConstant },
	{ TEXT("set_material_instance_base_overrides"), &HandleSetMaterialInstanceBaseOverrides },
	{ TEXT("set_material_instance_param_override"), &HandleSetMaterialInstanceParamOverride },
	{ TEXT("set_material_instance_parent"), &HandleSetMaterialInstanceParent },
	{ TEXT("set_material_instance_static_component_mask"), &HandleSetMaterialInstanceStaticComponentMask },
	{ TEXT("set_material_instance_static_switch"), &HandleSetMaterialInstanceStaticSwitch },
	{ TEXT("set_material_layer_parameter"), &HandleSetMaterialLayerParameter },
	{ TEXT("set_material_layer_stack"), &HandleSetMaterialLayerStack },
	{ TEXT("swap_material_layer_parameters"), &HandleSwapMaterialLayerParameters },
};
} // namespace UEAgentMaterialInstanceCommands
