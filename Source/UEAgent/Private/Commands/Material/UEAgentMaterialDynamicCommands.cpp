// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameters.h"
#include "Engine/Texture.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"
#include "UObject/GCObject.h"   // FGCObject: keeps the MIDs create_material_instance_dynamic makes

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
 *   - set_material_instance_dynamic_parameter: Set a scalar, vector or texture parameter value
 *   - copy_material_instance_dynamic_parameters: Copy parameters from another material
 * (get_material_parameters, in the material instance family, reads a MID's values.)
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

	/**
	 * Null when ParamName is a global parameter of Type in the MID's material, otherwise the error
	 * reply listing that type's global parameters. The Set*ParameterValue setters store an override
	 * under ANY name, and the MID then reads that override back, so the read-back alone cannot catch
	 * a typo. The material's own parameter set (GetAllParametersOfType takes its names from the
	 * cached expression data and only lays overrides over names it already has) is the reference.
	 * Setting by name addresses the global association
	 * only, so a name that exists only inside a material layer is refused too. TypeLabel names the
	 * type in lower case ("scalar").
	 */
	TSharedPtr<FJsonObject> MaterialDynamic_RefuseUnknownParam(const UMaterialInstanceDynamic* MID, EMaterialParameterType Type,
		const FString& ParamName, const TCHAR* TypeLabel)
	{
		TMap<FMaterialParameterInfo, FMaterialParameterMetadata> TypeParams;
		MID->GetAllParametersOfType(Type, TypeParams);
		const FMaterialParameterInfo WantedInfo{ FName(*ParamName) };
		if (TypeParams.Contains(WantedInfo))
		{
			return nullptr;
		}
		TArray<FString> Known;
		for (const TPair<FMaterialParameterInfo, FMaterialParameterMetadata>& Pair : TypeParams)
		{
			if (Pair.Key.Association == EMaterialParameterAssociation::GlobalParameter)
			{
				Known.Add(Pair.Key.Name.ToString());
			}
		}
		Known.Sort();
		FString ListLabel(TypeLabel);
		ListLabel[0] = FChar::ToUpper(ListLabel[0]);
		return mcp_common::MakeErrorReply(FString::Printf(
			TEXT("'%s' is not a global %s parameter of the material of MID %s; nothing was set. %s parameters: [%s]"),
			*ParamName, TypeLabel, *MID->GetPathName(), *ListLabel, *FString::Join(Known, TEXT(", "))));
	}

	/**
	 * Holds every MID create_material_instance_dynamic makes. Nothing else references such a MID (it lives in the
	 * transient package and belongs to no actor), so without this the next garbage collection could free it before
	 * the set_material_instance_dynamic_parameter calls that address it by path. No tool releases a MID, so they stay until the editor closes.
	 */
	class FCreatedMidKeeper final : public FGCObject
	{
	public:
		void Keep(UMaterialInstanceDynamic* MID)
		{
			Mids.Add(MID);
		}

		virtual void AddReferencedObjects(FReferenceCollector& Collector) override
		{
			Collector.AddReferencedObjects(Mids);
		}

		virtual FString GetReferencerName() const override
		{
			return TEXT("UEAgent create_material_instance_dynamic");
		}

	private:
		TArray<TObjectPtr<UMaterialInstanceDynamic>> Mids;
	};

	FCreatedMidKeeper& CreatedMidKeeper()
	{
		static FCreatedMidKeeper Keeper;
		return Keeper;
	}

	/** The MID's non-static parameters measured against a copy source. */
	struct FMidCopyTally
	{
		/** Parameters the source has a value for: the ones CopyMaterialInstanceParameters copies. */
		int32 InSource = 0;
		/** Of those, how many the MID overrides with exactly the source's value. */
		int32 Matching = 0;
		/** Parameters the source has no value for; the copy leaves them at the parent's value. */
		int32 NotInSource = 0;
		/** Names of the InSource parameters whose MID override is missing or differs. */
		TArray<FString> Mismatched;
	};

	/**
	 * Walks the same parameter set CopyMaterialInstanceParameters walks (every non-static type of
	 * the MID's material) and reads, per parameter, the source's value and the MID's own override.
	 */
	FMidCopyTally TallyMidAgainstSource(const UMaterialInstanceDynamic* MID, const UMaterialInterface* Source)
	{
		FMidCopyTally Tally;
		TArray<FMaterialParameterInfo> Infos;
		TArray<FGuid> Ids;
		for (int32 TypeIndex = 0; TypeIndex < NumMaterialParameterTypes; ++TypeIndex)
		{
			const EMaterialParameterType Type = (EMaterialParameterType)TypeIndex;
			if (IsStaticMaterialParameter(Type))
			{
				continue;
			}
			MID->GetAllParameterInfoOfType(Type, Infos, Ids);
			for (const FMaterialParameterInfo& Info : Infos)
			{
				FMaterialParameterMetadata SourceValue;
				if (!Source->GetParameterValue(Type, Info, SourceValue))
				{
					++Tally.NotInSource;
					continue;
				}
				++Tally.InSource;
				FMaterialParameterMetadata MidValue;
				if (MID->GetParameterOverrideValue(Type, Info, MidValue) && MidValue.Value == SourceValue.Value)
				{
					++Tally.Matching;
				}
				else
				{
					Tally.Mismatched.Add(Info.Name.ToString());
				}
			}
		}
		return Tally;
	}
}

// ---------------------------------------------------------------------------
// create_material_instance_dynamic
// ---------------------------------------------------------------------------
// MCP:create_material_instance_dynamic - Create a Material Instance Dynamic (MID) of a parent material in the transient package: a runtime-only object that is never saved, belongs to no actor and has no undo step. The plugin keeps a reference to every MID it makes this way, so garbage collection does not free it; no tool releases one, so each lives until the editor closes. Set its parameters with set_material_instance_dynamic_parameter, which takes its mid_object_path, or use create_dynamic_material_on_actor to have a mesh component own a MID it renders. A name another MID holds gets a numbered suffix, so keep mid_name and mid_object_path from the reply; each call creates another MID. A parent that cannot be loaded, or a name held by an object of another class, is an error. Read its values with get_material_parameters (material_instance_path = its mid_object_path); a saved, reusable instance asset: create_material_instance.
//   @param parent_material_path - Content path to the parent material (e.g. /Game/MyGame/M_Example.M_Example); a material or a material instance
//   @param name - Optional name for the MID (default auto-generated from MID_ and the parent's name)
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

	// UMaterialInstanceDynamic::Create gives a name another MID holds a numbered suffix, but NewObject over an object
	// of another class with that name aborts the editor ("Cannot replace existing object of a different class").
	if (InstanceName != NAME_None)
	{
		const UObject* Occupant = StaticFindObjectFast(UObject::StaticClass(), GetTransientPackage(), InstanceName);
		if (Occupant != nullptr && Occupant->GetClass() != UMaterialInstanceDynamic::StaticClass())
		{
			return mcp_common::MakeErrorReply(FString::Printf(
				TEXT("The name '%s' is taken in the transient package by a %s; nothing was created. Pick another name or omit it."),
				*MIDName, *Occupant->GetClass()->GetName()));
		}
	}

	// Create MID in transient package
	UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(ParentMat, GetTransientPackage(), InstanceName);
	if (!MID)
	{
		return mcp_common::MakeErrorReply(TEXT("Failed to create Material Instance Dynamic"));
	}
	CreatedMidKeeper().Keep(MID);

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
// set_material_instance_dynamic_parameter
// ---------------------------------------------------------------------------
// MCP:set_material_instance_dynamic_parameter - Set a scalar, vector (color) or texture parameter override on a transient Material Instance Dynamic (MID), found by object path or name as create_material_instance_dynamic or create_dynamic_material_on_actor return it; it takes effect at once and is never saved. param_type picks the kind, or it is inferred from value: a number is a scalar, an {r,g,b,a} object a vector, a string a texture path. param_name must be a global parameter of that kind of the MID's material (as get_material_parameters lists it for the MID); any other name is refused and nothing is written. set is the read-back of the new value.
//   @param mid_object_path - Object path of the MID (from create_material_instance_dynamic)
//   @param mid_path - Alias of mid_object_path
//   @param param_name - Name of a parameter of the MID's material, of the kind written
//   @param parameter_name - Alias of param_name
//   @param param_type - Optional "scalar", "vector" or "texture"; default inferred from value (number -> scalar, object -> vector, string -> texture). Any other value, or one that does not match the value's shape, is an error
//   @param value - New value: a float for a scalar, a color object {r, g, b, a} for a vector (a missing component is 0, a missing a is 1), a texture content path for a texture. Required unless texture_path is given
//   @param texture_path - Alias of value for a texture parameter (a texture content path)
//   @returns { mid_object_path, param_name, param_type, old_value, new_value (numbers for a scalar, {r, g, b, a} for a vector), old_texture_path?, new_texture_path?, requested_texture_path? (texture), set }
TSharedPtr<FJsonObject> HandleSetMidScalarParam(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MIDPath;
	if (!Params->TryGetStringField(TEXT("mid_object_path"), MIDPath) && !Params->TryGetStringField(TEXT("mid_path"), MIDPath))
	{
		return mcp_common::MissingParameterReply(TEXT("mid_object_path"));
	}

	FString ParamName;
	if (!Params->TryGetStringField(TEXT("param_name"), ParamName) && !Params->TryGetStringField(TEXT("parameter_name"), ParamName))
	{
		return mcp_common::MissingParameterReply(TEXT("param_name"));
	}

	FString ParamType;
	Params->TryGetStringField(TEXT("param_type"), ParamType);
	ParamType = ParamType.ToLower();

	// value is polymorphic; a texture may also come as texture_path.
	TSharedPtr<FJsonValue> RawValue = Params->TryGetField(TEXT("value"));
	FString TexturePathAlias;
	if ((!RawValue.IsValid() || RawValue->Type == EJson::Null) && Params->TryGetStringField(TEXT("texture_path"), TexturePathAlias))
	{
		RawValue = MakeShared<FJsonValueString>(TexturePathAlias);
	}
	if (!RawValue.IsValid() || RawValue->Type == EJson::None || RawValue->Type == EJson::Null)
	{
		return mcp_common::MissingParameterReply(TEXT("value"));
	}

	// The kind: param_type when given, else the value's JSON shape.
	const TCHAR* InferredType = RawValue->Type == EJson::Number ? TEXT("scalar")
		: RawValue->Type == EJson::Object ? TEXT("vector")
		: RawValue->Type == EJson::String ? TEXT("texture")
		: nullptr;
	if (ParamType.IsEmpty())
	{
		if (InferredType == nullptr)
		{
			return mcp_common::MakeErrorReply(TEXT("value must be a number (scalar), an {r,g,b,a} object (vector) or a texture path string (texture)"));
		}
		ParamType = InferredType;
	}
	if (ParamType != TEXT("scalar") && ParamType != TEXT("vector") && ParamType != TEXT("texture"))
	{
		return mcp_common::MakeErrorReply(FString::Printf(TEXT("Unknown param_type: %s (expected scalar, vector or texture)"), *ParamType));
	}
	if (InferredType == nullptr || ParamType != InferredType)
	{
		return mcp_common::MakeErrorReply(FString::Printf(
			TEXT("param_type %s needs value as %s"), *ParamType,
			ParamType == TEXT("scalar") ? TEXT("a number") : ParamType == TEXT("vector") ? TEXT("an {r,g,b,a} object") : TEXT("a texture path string")));
	}

	UMaterialInstanceDynamic* MID = nullptr;
	if (TSharedPtr<FJsonObject> NotFound = MaterialDynamic_ResolveMIDOrError(MIDPath, MID)) { return NotFound; }

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("mid_object_path"), MID->GetPathName());
	Data->SetStringField(TEXT("param_name"), ParamName);
	Data->SetStringField(TEXT("param_type"), ParamType);
	const FName ParamFName(*ParamName);
	bool bSet = false;

	if (ParamType == TEXT("scalar"))
	{
		if (TSharedPtr<FJsonObject> Unknown = MaterialDynamic_RefuseUnknownParam(MID, EMaterialParameterType::Scalar, ParamName, TEXT("scalar"))) { return Unknown; }
		const float Value = static_cast<float>(RawValue->AsNumber());
		const float OldValue = MID->K2_GetScalarParameterValue(ParamFName);
		MID->SetScalarParameterValue(ParamFName, Value);
		const float NewValue = MID->K2_GetScalarParameterValue(ParamFName);
		Data->SetNumberField(TEXT("old_value"), OldValue);
		Data->SetNumberField(TEXT("new_value"), NewValue);
		bSet = FMath::Abs(NewValue - Value) < KINDA_SMALL_NUMBER;
	}
	else if (ParamType == TEXT("vector"))
	{
		if (TSharedPtr<FJsonObject> Unknown = MaterialDynamic_RefuseUnknownParam(MID, EMaterialParameterType::Vector, ParamName, TEXT("vector"))) { return Unknown; }
		const TSharedPtr<FJsonObject> ColorObj = RawValue->AsObject();
		FLinearColor NewColor(0, 0, 0, 1);
		ColorObj->TryGetNumberField(TEXT("r"), NewColor.R);
		ColorObj->TryGetNumberField(TEXT("g"), NewColor.G);
		ColorObj->TryGetNumberField(TEXT("b"), NewColor.B);
		ColorObj->TryGetNumberField(TEXT("a"), NewColor.A);
		const FLinearColor OldColor = MID->K2_GetVectorParameterValue(ParamFName);
		MID->SetVectorParameterValue(ParamFName, NewColor);
		const FLinearColor ReadBack = MID->K2_GetVectorParameterValue(ParamFName);
		Data->SetObjectField(TEXT("old_value"), mcp_common::LinearColorToJsonObject(OldColor));
		Data->SetObjectField(TEXT("new_value"), mcp_common::LinearColorToJsonObject(ReadBack));
		bSet = FMath::Abs(ReadBack.R - NewColor.R) < KINDA_SMALL_NUMBER &&
			FMath::Abs(ReadBack.G - NewColor.G) < KINDA_SMALL_NUMBER &&
			FMath::Abs(ReadBack.B - NewColor.B) < KINDA_SMALL_NUMBER &&
			FMath::Abs(ReadBack.A - NewColor.A) < KINDA_SMALL_NUMBER;
	}
	else
	{
		if (TSharedPtr<FJsonObject> Unknown = MaterialDynamic_RefuseUnknownParam(MID, EMaterialParameterType::Texture, ParamName, TEXT("texture"))) { return Unknown; }
		const FString TexturePath = RawValue->AsString();
		UTexture* NewTexture = mcp_common::LoadAssetOrSoftPath<UTexture>(TexturePath);
		if (!NewTexture)
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("Cannot load texture: %s"), *TexturePath));
		}
		UTexture* OldTexture = MID->K2_GetTextureParameterValue(ParamFName);
		MID->SetTextureParameterValue(ParamFName, NewTexture);
		UTexture* ReadBackTexture = MID->K2_GetTextureParameterValue(ParamFName);
		Data->SetStringField(TEXT("old_texture_path"), OldTexture ? OldTexture->GetPathName() : TEXT(""));
		Data->SetStringField(TEXT("new_texture_path"), ReadBackTexture ? ReadBackTexture->GetPathName() : TEXT(""));
		Data->SetStringField(TEXT("requested_texture_path"), NewTexture->GetPathName());
		bSet = (ReadBackTexture == NewTexture);
	}

	Data->SetBoolField(TEXT("set"), bSet);
	return mcp_common::MakeOutcomeReply(Data, bSet, mcp_common::MeasuredOutcomeError(TEXT("set_material_instance_dynamic_parameter"), TEXT("set")));
}

// ---------------------------------------------------------------------------
// copy_material_instance_dynamic_parameters
// ---------------------------------------------------------------------------
// MCP:copy_material_instance_dynamic_parameters - Copy parameter values from a source material or material instance into a transient Material Instance Dynamic (MID), found by object path or name as create_material_instance_dynamic returns it. The MID's current overrides are cleared first, then every non-static parameter the MID exposes takes the source's value where the source has that parameter; static switches are not copied. Takes effect at once, nothing saved, no undo. Refused, with the MID untouched, when the source is the MID itself or has none of its parameters. copied is measured: every copyable parameter read back on the MID with the source's value. For saved Material Instance Constant assets use copy_material_instance_parameters.
//   @param mid_object_path - Object path of the MID
//   @param source_material_path - Content path to the source material to copy from
//   @returns { mid_object_path, source_material_path, source_material_name, params_in_source, params_copied, params_not_in_source, mismatched_params, copied }
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

	// CopyMaterialInstanceParameters does nothing when the source is the MID itself.
	if (SourceMat == MID)
	{
		return mcp_common::MakeErrorReply(FString::Printf(
			TEXT("source_material_path resolves to the MID itself (%s): nothing to copy"), *MID->GetPathName()));
	}

	// The copy clears every override before copying, so a source that shares no parameter with
	// the MID would only wipe it. Refuse that before touching the MID.
	const FMidCopyTally Before = TallyMidAgainstSource(MID, SourceMat);
	if (Before.InSource == 0)
	{
		return mcp_common::MakeErrorReply(FString::Printf(
			TEXT("Source %s has none of the %d non-static parameters of MID %s: nothing to copy, the MID was left untouched"),
			*SourceMat->GetPathName(), Before.NotInSource, *MID->GetPathName()));
	}

	// Copy parameters from source to MID
	MID->K2_CopyMaterialInstanceParameters(SourceMat, false);

	// Read back: every parameter the source has must now be a MID override with the source's value.
	const FMidCopyTally After = TallyMidAgainstSource(MID, SourceMat);
	const bool bCopied = After.InSource > 0 && After.Mismatched.Num() == 0;

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("mid_object_path"), MID->GetPathName());
	Data->SetStringField(TEXT("source_material_path"), SourceMat->GetPathName());
	Data->SetStringField(TEXT("source_material_name"), SourceMat->GetName());
	Data->SetNumberField(TEXT("params_in_source"), After.InSource);
	Data->SetNumberField(TEXT("params_copied"), After.Matching);
	Data->SetNumberField(TEXT("params_not_in_source"), After.NotInSource);
	Data->SetArrayField(TEXT("mismatched_params"), mcp_common::StringsToJson(After.Mismatched));
	Data->SetBoolField(TEXT("copied"), bCopied);
	return mcp_common::MakeOutcomeReply(Data, bCopied, mcp_common::MeasuredOutcomeError(TEXT("copy_material_instance_dynamic_parameters"), TEXT("copied")));
}

// ---------------------------------------------------------------------------
// create_dynamic_material_on_actor
// ---------------------------------------------------------------------------
// MCP:create_dynamic_material_on_actor - Give one material slot of a mesh component on an editor-world actor its own Material Instance Dynamic (UPrimitiveComponent::CreateDynamicMaterialInstance), which replaces the slot's material with the MID, in one undo step that marks the level dirty; no asset is created or saved. The actor is matched by object name or label (first match), the component is its first StaticMeshComponent, else its first mesh component. Without parent_material_path the MID wraps the slot's current material, and a slot already holding a MID returns that same MID with created false, changing nothing; with it, the slot is first set to that material. Pass mid_object_path to set_material_instance_dynamic_parameter, or to get_material_parameters as material_instance_path; verified confirms the slot holds the MID. A missing actor, mesh component or material, or an out-of-range element_index, is an error. Unowned MID: create_material_instance_dynamic; saved asset: create_material_instance.
//   @param actor_name - Name or label of the actor in the editor world; the first actor whose object name or label matches is used
//   @param element_index - Material slot index on the mesh component (default 0); must be below the component's slot count
//   @param parent_material_path - Optional source material for the MID (default slot's current material); when given it is assigned to the slot before the MID is made from it
//   @returns { actor_name, actor_label, component_name, element_index, mid_object_path, mid_name, parent_material_path, verified, created, note? }
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

	// Without a source material, CreateDynamicMaterialInstance returns a MID the slot already holds instead of making
	// one; that MID is answered as it is, with nothing modified or recorded.
	UMaterialInstanceDynamic* const SlotMID = Cast<UMaterialInstanceDynamic>(MeshComp->GetMaterial(ElementIndex));
	const bool bReused = ParentMat == nullptr && SlotMID != nullptr;
	UMaterialInstanceDynamic* MID = nullptr;
	bool bCreated = false;
	if (bReused)
	{
		MID = SlotMID;
	}
	else
	{
		// A slot with no material and no source material gives the engine nothing to derive a MID from.
		if (ParentMat == nullptr && MeshComp->GetMaterial(ElementIndex) == nullptr)
		{
			return mcp_common::MakeErrorReply(FString::Printf(
				TEXT("Material slot %d has no material and no parent_material_path was given; nothing was changed"), ElementIndex));
		}
		const FScopedTransaction Transaction(LOCTEXT("CreateDynamicMaterialOnActor", "MCP: Create Dynamic Material On Actor"));
		MeshComp->Modify();

		// CreateDynamicMaterialInstance creates a component-owned MID and assigns it to
		// the given element. With a null SourceMaterial it derives from the current slot.
		MID = MeshComp->CreateDynamicMaterialInstance(ElementIndex, ParentMat);
		if (!MID)
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("CreateDynamicMaterialInstance failed for element %d (slot may have no source material)"), ElementIndex));
		}
		// A source material that is itself a MID is assigned to the slot and returned, not created.
		bCreated = MID != SlotMID && MID != ParentMat;
	}

	// Verify the slot now holds the MID.
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
	Data->SetBoolField(TEXT("created"), bCreated);
	if (bReused)
	{
		Data->SetStringField(TEXT("note"), TEXT("The slot already holds this MID; nothing was created or changed."));
	}
	return mcp_common::MakeOkReply(Data);
}

const McpCommandFamilies::FRegistrar Registration =
{
	{ TEXT("copy_material_instance_dynamic_parameters"), &HandleCopyMidParamsFromSource },
	{ TEXT("create_dynamic_material_on_actor"), &HandleCreateDynamicMaterialOnActor },
	{ TEXT("create_material_instance_dynamic"), &HandleCreateMaterialInstanceDynamic },
	{ TEXT("set_material_instance_dynamic_parameter"), &HandleSetMidScalarParam },
};
} // namespace UEAgentMaterialDynamicCommands

#undef LOCTEXT_NAMESPACE
