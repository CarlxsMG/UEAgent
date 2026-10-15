#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameters.h"
#include "Engine/Texture.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Components/MeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "UEAgentMaterialDynamic"

/**
 * Material Instance Dynamic (MID) commands.
 *
 * MIDs are runtime-only material instances, created in the transient package (or
 * owned by a mesh component, for create_dynamic_material_on_actor). They are NOT
 * saved as assets and do not persist across editor restarts.
 *
 * Commands:
 *   - create_material_instance_dynamic: Create a MID from a parent material
 *   - set_mid_scalar_param: Set a scalar parameter value
 *   - set_mid_vector_param: Set a vector parameter value
 *   - get_mid_all_params: Get all parameters from the parent material
 *   - copy_mid_params_from_source: Copy parameters from another material
 *   - set_mid_texture_param: Set a texture parameter value
 *   - create_dynamic_material_on_actor: Create a component-owned MID on an actor
 */
namespace UEAgentMaterialDynamicCommands
{

// MCP-DOMAIN: Material

namespace
{
	/**
	 * Resolve a UMaterialInstanceDynamic by object path or from the transient registry.
	 * MIDs live in /Transient so we try LoadObject first, then scan transient package.
	 */
	UMaterialInstanceDynamic* ResolveMID(const FString& PathOrName)
	{
		if (PathOrName.IsEmpty())
		{
			return nullptr;
		}

		// Try direct load (works for /Transient.MID_Name paths)
		UMaterialInstanceDynamic* MID = LoadObject<UMaterialInstanceDynamic>(nullptr, *PathOrName);
		if (MID)
		{
			return MID;
		}

		// Try finding by name in the transient package
		UPackage* TransientPkg = GetTransientPackage();
		if (TransientPkg)
		{
			UObject* Found = StaticFindObject(UMaterialInstanceDynamic::StaticClass(), TransientPkg, *PathOrName);
			MID = Cast<UMaterialInstanceDynamic>(Found);
			if (MID)
			{
				return MID;
			}
		}

		// Component-owned MIDs (from CreateDynamicMaterialInstance) live inside the
		// level package, not /Transient, so LoadObject above fails for them. Scan all
		// live MID objects and match by full path name or short name.
		for (TObjectIterator<UMaterialInstanceDynamic> It; It; ++It)
		{
			UMaterialInstanceDynamic* Candidate = *It;
			if (Candidate && (Candidate->GetPathName() == PathOrName || Candidate->GetName() == PathOrName))
			{
				return Candidate;
			}
		}
		return nullptr;
	}

	/** ResolveMID: null once OutMID holds the MID, otherwise the error reply. */
	TSharedPtr<FJsonObject> MaterialDynamic_ResolveMIDOrError(const FString& MIDPath, UMaterialInstanceDynamic*& OutMID)
	{
		OutMID = ResolveMID(MIDPath);
		if (!OutMID)
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("Cannot resolve MID: %s"), *MIDPath));
		}
		return nullptr;
	}
}

// ---------------------------------------------------------------------------
// create_material_instance_dynamic
// ---------------------------------------------------------------------------
// MCP:create_material_instance_dynamic - Create a runtime-only Material Instance Dynamic (MID) from a parent material
//   @param parent_material_path - Content path to the parent material (e.g. /Game/MyGame/M_Example.M_Example)
//   @param name - Optional name for the MID (default auto-generated)
//   @returns { mid_object_path, mid_name, parent_material_path, parent_material_name, created }
TSharedPtr<FJsonObject> HandleCreateMaterialInstanceDynamic(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ParentPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("parent_material_path"), ParentPath)) { return Missing; }

	// The parent the dynamic instance derives from.
	UMaterialInterface* ParentMat = mcp_common::LoadAssetOrSoftPath<UMaterialInterface>(ParentPath);
	if (!ParentMat)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Cannot load parent material: %s"), *ParentPath));
	}

	// Optional name
	FString MIDName;
	Params->TryGetStringField(TEXT("name"), MIDName);
	FName InstanceName = MIDName.IsEmpty() ? NAME_None : FName(*MIDName);

	// Create MID in transient package
	UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(ParentMat, GetTransientPackage(), InstanceName);
	if (!MID)
	{
		return mcp_common::MakeErrorReply(TEXT("Failed to create Material Instance Dynamic"));
	}

	// Get the object path for later reference
	FString ObjectPath = MID->GetPathName();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("mid_object_path"), ObjectPath);
	Data->SetStringField(TEXT("mid_name"), MID->GetName());
	Data->SetStringField(TEXT("parent_material_path"), ParentMat->GetPathName());
	Data->SetStringField(TEXT("parent_material_name"), ParentMat->GetName());
	Data->SetBoolField(TEXT("created"), true);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_mid_scalar_param
// ---------------------------------------------------------------------------
// MCP:set_mid_scalar_param - Set a scalar parameter override on a transient Material Instance Dynamic (MID), found by object path or name as create_material_instance_dynamic or create_dynamic_material_on_actor return it; it takes effect at once and is never saved. An unknown param_name is not rejected: the override is stored and read back, so set is true even though no material input uses it -- check names with get_mid_all_params. old_value is 0 for a parameter without a value. Vectors and textures: set_mid_vector_param, set_mid_texture_param.
//   @param mid_object_path - Object path of the MID (from create_material_instance_dynamic)
//   @param param_name - Name of the scalar parameter
//   @param value - New float value
//   @returns { mid_object_path, param_name, old_value, new_value, set }
TSharedPtr<FJsonObject> HandleSetMidScalarParam(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MIDPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("mid_object_path"), MIDPath)) { return Missing; }

	FString ParamName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("param_name"), ParamName)) { return Missing; }

	double ValueD = 0.0;
	if (!Params->TryGetNumberField(TEXT("value"), ValueD))
	{
		return mcp_common::MakeErrorReply(TEXT("value (float) is required"));
	}
	float Value = (float)ValueD;

	UMaterialInstanceDynamic* MID = nullptr;
	if (TSharedPtr<FJsonObject> NotFound = MaterialDynamic_ResolveMIDOrError(MIDPath, MID)) { return NotFound; }

	// Read old value
	float OldValue = MID->K2_GetScalarParameterValue(FName(*ParamName));

	// Set new value
	MID->SetScalarParameterValue(FName(*ParamName), Value);

	// Read back to confirm
	float NewValue = MID->K2_GetScalarParameterValue(FName(*ParamName));

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("mid_object_path"), MID->GetPathName());
	Data->SetStringField(TEXT("param_name"), ParamName);
	Data->SetNumberField(TEXT("old_value"), OldValue);
	Data->SetNumberField(TEXT("new_value"), NewValue);
	Data->SetBoolField(TEXT("set"), FMath::Abs(NewValue - Value) < KINDA_SMALL_NUMBER);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_mid_vector_param
// ---------------------------------------------------------------------------
// MCP:set_mid_vector_param - Set a vector/color parameter value on a Material Instance Dynamic.
//   @param mid_object_path - Object path of the MID
//   @param param_name - Name of the vector parameter
//   @param r [number] - Red component (0-1)
//   @param g [number] - Green component (0-1)
//   @param b [number] - Blue component (0-1)
//   @param value - Color object {r, g, b, a} components (a defaults to 1)
//   @returns { mid_object_path, param_name, r, g, b, a, set }
TSharedPtr<FJsonObject> HandleSetMidVectorParam(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MIDPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("mid_object_path"), MIDPath)) { return Missing; }

	FString ParamName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("param_name"), ParamName)) { return Missing; }

	// Parse value {r, g, b, a}
	const TSharedPtr<FJsonObject>* ValueObj = nullptr;
	if (!Params->TryGetObjectField(TEXT("value"), ValueObj) || !ValueObj || !*ValueObj)
	{
		return mcp_common::MakeErrorReply(TEXT("value object {r,g,b,a} is required"));
	}

	FLinearColor NewColor(0, 0, 0, 1);
	(*ValueObj)->TryGetNumberField(TEXT("r"), NewColor.R);
	(*ValueObj)->TryGetNumberField(TEXT("g"), NewColor.G);
	(*ValueObj)->TryGetNumberField(TEXT("b"), NewColor.B);
	(*ValueObj)->TryGetNumberField(TEXT("a"), NewColor.A);

	UMaterialInstanceDynamic* MID = nullptr;
	if (TSharedPtr<FJsonObject> NotFound = MaterialDynamic_ResolveMIDOrError(MIDPath, MID)) { return NotFound; }

	// Read old value
	FLinearColor OldColor = MID->K2_GetVectorParameterValue(FName(*ParamName));

	// Set new value
	MID->SetVectorParameterValue(FName(*ParamName), NewColor);

	// Read back
	FLinearColor ReadBack = MID->K2_GetVectorParameterValue(FName(*ParamName));

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("mid_object_path"), MID->GetPathName());
	Data->SetStringField(TEXT("param_name"), ParamName);

	TSharedPtr<FJsonObject> OldObj = mcp_common::LinearColorToJsonObject(OldColor);
	Data->SetObjectField(TEXT("old_value"), OldObj);

	TSharedPtr<FJsonObject> NewObj = mcp_common::LinearColorToJsonObject(ReadBack);
	Data->SetObjectField(TEXT("new_value"), NewObj);

	bool bSet = FMath::Abs(ReadBack.R - NewColor.R) < KINDA_SMALL_NUMBER &&
				FMath::Abs(ReadBack.G - NewColor.G) < KINDA_SMALL_NUMBER &&
				FMath::Abs(ReadBack.B - NewColor.B) < KINDA_SMALL_NUMBER &&
				FMath::Abs(ReadBack.A - NewColor.A) < KINDA_SMALL_NUMBER;
	Data->SetBoolField(TEXT("set"), bSet);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_mid_all_params
// ---------------------------------------------------------------------------
// MCP:get_mid_all_params - Get all parameters and their current values from a Material Instance Dynamic
//   @param mid_object_path - Object path of the MID
//   @returns { name, value, r, g, b, a, texture_path, texture_name, mid_object_path, mid_name, parent_material, scalar_params, vector_params, texture_params, scalar_count, vector_count, texture_count }
TSharedPtr<FJsonObject> HandleGetMidAllParams(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MIDPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("mid_object_path"), MIDPath)) { return Missing; }

	UMaterialInstanceDynamic* MID = nullptr;
	if (TSharedPtr<FJsonObject> NotFound = MaterialDynamic_ResolveMIDOrError(MIDPath, MID)) { return NotFound; }

	// Use GetAllParametersOfType on the MID (inherited from UMaterialInterface)
	// to enumerate all parameters from the parent hierarchy
	TMap<FMaterialParameterInfo, FMaterialParameterMetadata> ScalarParams;
	MID->GetAllParametersOfType(EMaterialParameterType::Scalar, ScalarParams);

	TMap<FMaterialParameterInfo, FMaterialParameterMetadata> VectorParams;
	MID->GetAllParametersOfType(EMaterialParameterType::Vector, VectorParams);

	TMap<FMaterialParameterInfo, FMaterialParameterMetadata> TextureParams;
	MID->GetAllParametersOfType(EMaterialParameterType::Texture, TextureParams);

	// Build scalar params array with current MID values
	TArray<TSharedPtr<FJsonValue>> ScalarArr;
	for (const auto& Pair : ScalarParams)
	{
		TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
		ParamObj->SetStringField(TEXT("name"), Pair.Key.Name.ToString());
		// Read current value from MID
		float Val = MID->K2_GetScalarParameterValue(Pair.Key.Name);
		ParamObj->SetNumberField(TEXT("value"), Val);
		ScalarArr.Add(MakeShared<FJsonValueObject>(ParamObj));
	}

	// Build vector params array
	TArray<TSharedPtr<FJsonValue>> VectorArr;
	for (const auto& Pair : VectorParams)
	{
		TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
		ParamObj->SetStringField(TEXT("name"), Pair.Key.Name.ToString());
		FLinearColor Val = MID->K2_GetVectorParameterValue(Pair.Key.Name);
		ParamObj->SetNumberField(TEXT("r"), Val.R);
		ParamObj->SetNumberField(TEXT("g"), Val.G);
		ParamObj->SetNumberField(TEXT("b"), Val.B);
		ParamObj->SetNumberField(TEXT("a"), Val.A);
		VectorArr.Add(MakeShared<FJsonValueObject>(ParamObj));
	}

	// Build texture params array
	TArray<TSharedPtr<FJsonValue>> TextureArr;
	for (const auto& Pair : TextureParams)
	{
		TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
		ParamObj->SetStringField(TEXT("name"), Pair.Key.Name.ToString());
		UTexture* Tex = MID->K2_GetTextureParameterValue(Pair.Key.Name);
		ParamObj->SetStringField(TEXT("texture_path"), Tex ? Tex->GetPathName() : TEXT(""));
		ParamObj->SetStringField(TEXT("texture_name"), Tex ? Tex->GetName() : TEXT(""));
		TextureArr.Add(MakeShared<FJsonValueObject>(ParamObj));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("mid_object_path"), MID->GetPathName());
	Data->SetStringField(TEXT("mid_name"), MID->GetName());
	Data->SetStringField(TEXT("parent_material"), MID->Parent ? MID->Parent->GetPathName() : TEXT(""));
	Data->SetArrayField(TEXT("scalar_params"), ScalarArr);
	Data->SetArrayField(TEXT("vector_params"), VectorArr);
	Data->SetArrayField(TEXT("texture_params"), TextureArr);
	Data->SetNumberField(TEXT("scalar_count"), ScalarArr.Num());
	Data->SetNumberField(TEXT("vector_count"), VectorArr.Num());
	Data->SetNumberField(TEXT("texture_count"), TextureArr.Num());
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// copy_mid_params_from_source
// ---------------------------------------------------------------------------
// MCP:copy_mid_params_from_source - Copy parameter values from a source material or material instance into a transient Material Instance Dynamic (MID), found by object path or name as create_material_instance_dynamic returns it. The MID's current overrides are cleared first, then every non-static parameter the MID exposes takes the source's value where the source has that parameter; static switches are not copied. Takes effect at once, nothing saved, no undo. copied is always true, so verify with get_mid_all_params. For saved Material Instance Constant assets use copy_material_instance_params.
//   @param mid_object_path - Object path of the MID
//   @param source_material_path - Content path to the source material to copy from
//   @returns { mid_object_path, source_material_path, source_material_name, copied }
TSharedPtr<FJsonObject> HandleCopyMidParamsFromSource(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MIDPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("mid_object_path"), MIDPath)) { return Missing; }

	FString SourcePath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("source_material_path"), SourcePath)) { return Missing; }

	UMaterialInstanceDynamic* MID = nullptr;
	if (TSharedPtr<FJsonObject> NotFound = MaterialDynamic_ResolveMIDOrError(MIDPath, MID)) { return NotFound; }

	UMaterialInterface* SourceMat = mcp_common::LoadAssetOrSoftPath<UMaterialInterface>(SourcePath);
	if (!SourceMat)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Cannot load source material: %s"), *SourcePath));
	}

	// Copy parameters from source to MID
	MID->K2_CopyMaterialInstanceParameters(SourceMat, false);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("mid_object_path"), MID->GetPathName());
	Data->SetStringField(TEXT("source_material_path"), SourceMat->GetPathName());
	Data->SetStringField(TEXT("source_material_name"), SourceMat->GetName());
	Data->SetBoolField(TEXT("copied"), true);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_mid_texture_param
// ---------------------------------------------------------------------------
// MCP:set_mid_texture_param - Set a texture parameter value on a Material Instance Dynamic
//   @param mid_path - Object path of the MID (transient or component-owned full path)
//   @param parameter_name - Name of the texture parameter
//   @param texture_path - Content path to the texture to assign
//   @returns { mid_path, parameter_name, old_texture_path, new_texture_path, requested_texture_path, set }
TSharedPtr<FJsonObject> HandleSetMidTextureParam(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MIDPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("mid_path"), MIDPath)) { return Missing; }

	FString ParamName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("parameter_name"), ParamName)) { return Missing; }

	FString TexturePath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("texture_path"), TexturePath)) { return Missing; }

	UMaterialInstanceDynamic* MID = nullptr;
	if (TSharedPtr<FJsonObject> NotFound = MaterialDynamic_ResolveMIDOrError(MIDPath, MID)) { return NotFound; }

	UTexture* NewTexture = mcp_common::LoadAssetOrSoftPath<UTexture>(TexturePath);
	if (!NewTexture)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Cannot load texture: %s"), *TexturePath));
	}

	// Read old value
	UTexture* OldTexture = MID->K2_GetTextureParameterValue(FName(*ParamName));

	// Set new value
	MID->SetTextureParameterValue(FName(*ParamName), NewTexture);

	// Read back to confirm
	UTexture* ReadBack = MID->K2_GetTextureParameterValue(FName(*ParamName));

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("mid_path"), MID->GetPathName());
	Data->SetStringField(TEXT("parameter_name"), ParamName);
	Data->SetStringField(TEXT("old_texture_path"), OldTexture ? OldTexture->GetPathName() : TEXT(""));
	Data->SetStringField(TEXT("new_texture_path"), ReadBack ? ReadBack->GetPathName() : TEXT(""));
	Data->SetStringField(TEXT("requested_texture_path"), NewTexture->GetPathName());
	Data->SetBoolField(TEXT("set"), ReadBack == NewTexture);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// create_dynamic_material_on_actor
// ---------------------------------------------------------------------------
// MCP:create_dynamic_material_on_actor - Create a component-owned Material Instance Dynamic on a scene actor
//   @param actor_name - Name or label of the actor in the editor world
//   @param element_index - Material slot index on the mesh component (default 0)
//   @param parent_material_path - Optional source material for the MID (default slot's current material)
//   @returns { actor_name, actor_label, component_name, element_index, mid_object_path, mid_name, parent_material_path, verified, created }
TSharedPtr<FJsonObject> HandleCreateDynamicMaterialOnActor(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ActorName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName)) { return Missing; }

	int32 ElementIndex = 0;
	Params->TryGetNumberField(TEXT("element_index"), ElementIndex);

	// Optional parent material to use as the MID source (defaults to the slot's
	// existing material if omitted).
	FString ParentPath;
	UMaterialInterface* ParentMat = nullptr;
	if (Params->TryGetStringField(TEXT("parent_material_path"), ParentPath) && !ParentPath.IsEmpty())
	{
		ParentMat = mcp_common::LoadAssetOrSoftPath<UMaterialInterface>(ParentPath);
		if (!ParentMat)
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("Cannot load parent material: %s"), *ParentPath));
		}
	}

	UWorld* World = nullptr;
	if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World, TEXT("Failed to get editor world"))) { return NoWorld; }

	AActor* Actor = mcp_common::FindActorByName(World, ActorName);
	if (!Actor)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Actor not found: %s"), *ActorName));
	}

	UMeshComponent* MeshComp = Actor->FindComponentByClass<UStaticMeshComponent>();
	if (!MeshComp)
	{
		MeshComp = Actor->FindComponentByClass<UMeshComponent>();
	}
	if (!MeshComp)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Actor '%s' has no mesh component"), *ActorName));
	}

	const int32 NumMaterials = MeshComp->GetNumMaterials();
	if (ElementIndex < 0 || ElementIndex >= NumMaterials)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("element_index %d out of range (component has %d material slots)"),
				ElementIndex, NumMaterials));
	}

	const FScopedTransaction Transaction(LOCTEXT("CreateDynamicMaterialOnActor", "MCP: Create Dynamic Material On Actor"));
	MeshComp->Modify();

	// CreateDynamicMaterialInstance creates a component-owned MID and assigns it to
	// the given element. With a null SourceMaterial it derives from the current slot.
	UMaterialInstanceDynamic* MID = MeshComp->CreateDynamicMaterialInstance(ElementIndex, ParentMat);
	if (!MID)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("CreateDynamicMaterialInstance failed for element %d (slot may have no source material)"), ElementIndex));
	}

	// Verify the slot now holds the MID we created.
	UMaterialInterface* AssignedMat = MeshComp->GetMaterial(ElementIndex);
	const bool bVerified = (AssignedMat == MID);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_name"), Actor->GetName());
	Data->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
	Data->SetStringField(TEXT("component_name"), MeshComp->GetName());
	Data->SetNumberField(TEXT("element_index"), ElementIndex);
	Data->SetStringField(TEXT("mid_object_path"), MID->GetPathName());
	Data->SetStringField(TEXT("mid_name"), MID->GetName());
	Data->SetStringField(TEXT("parent_material_path"), MID->Parent ? MID->Parent->GetPathName() : TEXT(""));
	Data->SetBoolField(TEXT("verified"), bVerified);
	Data->SetBoolField(TEXT("created"), true);
	return mcp_common::MakeOkReply(Data);
}

const McpCommandFamilies::FRegistrar Registration =
{
	{ TEXT("copy_mid_params_from_source"), &HandleCopyMidParamsFromSource },
	{ TEXT("create_dynamic_material_on_actor"), &HandleCreateDynamicMaterialOnActor },
	{ TEXT("create_material_instance_dynamic"), &HandleCreateMaterialInstanceDynamic },
	{ TEXT("get_mid_all_params"), &HandleGetMidAllParams },
	{ TEXT("set_mid_scalar_param"), &HandleSetMidScalarParam },
	{ TEXT("set_mid_texture_param"), &HandleSetMidTextureParam },
	{ TEXT("set_mid_vector_param"), &HandleSetMidVectorParam },
};
} // namespace UEAgentMaterialDynamicCommands

#undef LOCTEXT_NAMESPACE
