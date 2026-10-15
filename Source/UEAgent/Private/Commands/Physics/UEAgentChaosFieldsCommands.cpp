#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

#include "Editor.h"
#include "Engine/World.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "AssetRegistry/AssetRegistryModule.h"

// FieldSystemEngine (engine Runtime/Experimental): asset, authoring U-nodes, actor, component.
#include "Field/FieldSystemAsset.h"      // UFieldSystem
#include "Field/FieldSystemObjects.h"    // URadialFalloff, UUniformVector, ..., FFieldObjectCommands
#include "Field/FieldSystemActor.h"      // AFieldSystemActor
#include "Field/FieldSystemComponent.h"  // UFieldSystemComponent

// Chaos field core: FFieldSystemCommand, EFieldPhysicsType / EFieldFalloffType + name lookups.
#include "Field/FieldSystem.h"           // FFieldSystemCommand
#include "Field/FieldSystemTypes.h"      // EFieldPhysicsType, GetFieldPhysicsType, GetFieldPhysicsName

/**
 * Chaos Physics Field authoring commands.
 *
 * Authors UFieldSystem assets (saved, idempotent), appends and removes FFieldSystemCommand
 * nodes built from transient authoring U-nodes (URadialFalloff, UUniformVector,
 * URadialVector, UUniformScalar, UUniformInteger, ...), spawns AFieldSystemActor
 * in the editor world and assigns the asset, and reads field-command metadata back.
 *
 * HONEST NOTE: runtime field dispatch (the actual force/strain application by the
 * Chaos solver) is PIE-only and not exposed here — these commands are all
 * headless-safe asset/actor authoring + introspection.
 */
namespace UEAgentChaosFieldsCommands
{

// MCP-DOMAIN: Physics

namespace
{
	/** The UFieldSystem at Path (/Game/X or /Game/X.X): null once OutFS holds it, otherwise the error reply. */
	TSharedPtr<FJsonObject> ChaosFields_LoadFieldSystem(const FString& Path, UFieldSystem*& OutFS)
	{
		OutFS = mcp_common::LoadAssetOrSoftPath<UFieldSystem>(Path);
		if (!OutFS)
		{
			return mcp_common::MakeErrorReply(FString::Printf(TEXT("Could not load UFieldSystem: %s"), *Path));
		}
		return nullptr;
	}

	/** The fields every saving command's reply ends with: command_count, success, and save_error when the save
	 *  failed with a reason. */
	void ChaosFields_AddCountAndSaveError(const TSharedPtr<FJsonObject>& Data, const UFieldSystem& FS, bool bSaved, const FString& SaveError)
	{
		Data->SetNumberField(TEXT("command_count"), FS.Commands.Num());
		Data->SetBoolField(TEXT("success"), true);
		if (!bSaved && !SaveError.IsEmpty())
		{
			Data->SetStringField(TEXT("save_error"), SaveError);
		}
	}

	/** Parse a falloff-type string into the EFieldFalloffType enum. Defaults to Linear. */
	EFieldFalloffType ParseFalloff(const FString& Raw)
	{
		const FString S = Raw.ToLower();
		if (S == TEXT("none")) { return EFieldFalloffType::Field_FallOff_None; }
		if (S == TEXT("inverse")) { return EFieldFalloffType::Field_Falloff_Inverse; }
		if (S == TEXT("squared")) { return EFieldFalloffType::Field_Falloff_Squared; }
		if (S == TEXT("logarithmic")) { return EFieldFalloffType::Field_Falloff_Logarithmic; }
		return EFieldFalloffType::Field_Falloff_Linear;
	}

	/** Parse a wave-function string into the EWaveFunctionType enum. Defaults to Cosine. */
	EWaveFunctionType ParseWaveFunction(const FString& Raw)
	{
		const FString S = Raw.ToLower();
		if (S == TEXT("gaussian")) { return EWaveFunctionType::Field_Wave_Gaussian; }
		if (S == TEXT("falloff")) { return EWaveFunctionType::Field_Wave_Falloff; }
		if (S == TEXT("decay")) { return EWaveFunctionType::Field_Wave_Decay; }
		return EWaveFunctionType::Field_Wave_Cosine;
	}

	/** Parse a mask-condition string into the ESetMaskConditionType enum. Defaults to Always. */
	ESetMaskConditionType ParseMaskCondition(const FString& Raw)
	{
		const FString S = Raw.ToLower();
		if (S == TEXT("mergeinterior") || S == TEXT("notinterior")) { return ESetMaskConditionType::Field_Set_IFF_NOT_Interior; }
		if (S == TEXT("mergeexterior") || S == TEXT("notexterior")) { return ESetMaskConditionType::Field_Set_IFF_NOT_Exterior; }
		return ESetMaskConditionType::Field_Set_Always;
	}
}

// ---------------------------------------------------------------------------
// 1. create_field_system_asset
// ---------------------------------------------------------------------------
// MCP:create_field_system_asset - Create (or load) a saved UFieldSystem asset on /Game.  Idempotent: if the asset already exists it is loaded and the same payload shape is returned. Otherwise a new UFieldSystem is created, registered with the asset registry, and saved to disk.
//   @param field_system_name - Asset name (e.g. "FS_Example")
//   @param package_path - Optional content folder; defaults to /Game/MCP
//   @returns { field_system_path: string, field_system_name: string, command_count: int, success: bool, saved: bool, save_error?: string }

TSharedPtr<FJsonObject> HandleCreateFieldSystemAsset(
	const TSharedPtr<FJsonObject>& Params)
{
	FString Name;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("field_system_name"), Name)) { return Missing; }

	FString PackagePath = mcp_common::DefaultAssetFolder();
	Params->TryGetStringField(TEXT("package_path"), PackagePath);
	PackagePath.RemoveFromEnd(TEXT("/"));

	const FString FullPath = FString::Printf(TEXT("%s/%s"), *PackagePath, *Name);
	if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(FullPath)) { return Invalid; }
	const FString DotName = FString::Printf(TEXT("%s.%s"), *FullPath, *Name);

	// Idempotent: load-if-exists and return the same payload shape.
	if (UFieldSystem* Existing = mcp_common::LoadAssetOrSoftPath<UFieldSystem>(DotName))
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("field_system_path"), FullPath);
		Data->SetStringField(TEXT("field_system_name"), Name);
		Data->SetNumberField(TEXT("command_count"), Existing->Commands.Num());
		Data->SetBoolField(TEXT("success"), true);
		Data->SetBoolField(TEXT("saved"), true);
		return mcp_common::MakeOkReply(Data);
	}

	FString CreateError;
	UFieldSystem* FS = mcp_common::NewAssetInPackage<UFieldSystem>(FullPath, Name, TEXT("Failed to create UFieldSystem"), CreateError);
	if (!FS)
	{
		return mcp_common::MakeErrorReply(CreateError);
	}

	FAssetRegistryModule::AssetCreated(FS);

	FString SaveError;
	const bool bSaved = mcp_common::SaveAssetPackage(FS->GetPackage(), FS, SaveError);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("field_system_path"), FullPath);
	Data->SetStringField(TEXT("field_system_name"), Name);
	ChaosFields_AddCountAndSaveError(Data, *FS, bSaved, SaveError);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("create_field_system_asset"));
}

// ---------------------------------------------------------------------------
// 2. add_field_node
// ---------------------------------------------------------------------------
// MCP:add_field_node - Author a field command node into a UFieldSystem and persist it.  Builds a transient authoring field U-node (selected by node_type), resolves the physics target to an EFieldPhysicsType, constructs the runtime FFieldSystemCommand via FFieldObjectCommands::CreateFieldCommand, appends it to UFieldSystem.Commands, and saves the asset.  node_type accepts "RadialFalloff", "PlaneFalloff", "BoxFalloff", "WaveScalar", "UniformScalar", "NoiseField", "UniformInteger", "RadialIntMask", "UniformVector", "RadialVector", "RotatedRadialVector", "RandomVector". Vector nodes pair with vector targets (LinearVelocity/LinearForce); scalar with strain/threshold; integer with state (DynamicState).  target is a physics target attribute name like "LinearVelocity", "ExternalClusterStrain", "DynamicState".  falloff_type accepts "Inverse", "Squared", "Logarithmic", or "None". Defaults to "Linear".
//   @param field_system_path - Asset path of the UFieldSystem (e.g. "/Game/MyGame/FS_Example")
//   @param node_type - One of "RadialFalloff", "PlaneFalloff", "BoxFalloff", "WaveScalar", "UniformScalar", "NoiseField", "UniformInteger", "RadialIntMask", "UniformVector", "RadialVector", "RotatedRadialVector", "RandomVector"
//   @param target - Physics target attribute name, e.g. "LinearForce", "LinearVelocity", "ExternalClusterStrain", "DynamicState"
//   @param magnitude - Optional scalar magnitude; defaults to 1.0
//   @param radius - Optional falloff / mask radius; defaults to 100.0
//   @param position - Optional [x, y, z] world / center position
//   @param direction - Optional [x, y, z]; uniform-vector direction, plane normal (PlaneFalloff), or rotation axis (RotatedRadialVector)
//   @param falloff_type - Optional EFieldFalloffType; defaults to "Linear"
//   @param min_range - Optional remap lower bound for falloff/noise nodes; defaults to 0.0
//   @param max_range - Optional remap upper bound for falloff/noise nodes; defaults to 1.0
//   @param default_value - Optional out-of-range value for falloff nodes; defaults to 0.0
//   @param distance - Optional plane distance limit for PlaneFalloff; defaults to 0.0
//   @param wavelength - Optional WaveScalar wavelength; defaults to 10000.0
//   @param period - Optional WaveScalar period; defaults to 1.0
//   @param wave_function - Optional WaveScalar function "Cosine", "Gaussian", "Falloff", "Decay"; defaults to "Cosine"
//   @param interior_value - Optional RadialIntMask interior integer; defaults to 1
//   @param exterior_value - Optional RadialIntMask exterior integer; defaults to 0
//   @param mask_condition - Optional RadialIntMask condition "Always", "MergeInterior", "MergeExterior"; defaults to "Always"
//   @param angle - Optional RotatedRadialVector rotation angle in degrees; defaults to 0.0
//   @returns { field_system_path: string, node_type: string, target: string, physics_type_name: string, command_count: int, success: bool, saved: bool, save_error?: string }

TSharedPtr<FJsonObject> HandleAddFieldNode(
	const TSharedPtr<FJsonObject>& Params)
{
	FString FieldSystemPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("field_system_path"), FieldSystemPath)) { return Missing; }
	FString NodeType;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("node_type"), NodeType)) { return Missing; }
	FString Target;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("target"), Target)) { return Missing; }

	UFieldSystem* FS = nullptr;
	if (TSharedPtr<FJsonObject> LoadError = ChaosFields_LoadFieldSystem(FieldSystemPath, FS)) { return LoadError; }

	// Optional numeric/vector params.
	double Magnitude = 1.0;
	Params->TryGetNumberField(TEXT("magnitude"), Magnitude);
	double Radius = 100.0;
	Params->TryGetNumberField(TEXT("radius"), Radius);

	FVector Position(0, 0, 0);
	if (Params->HasField(TEXT("position")))
	{
		Position = mcp_common::ReadJsonVector(Params, TEXT("position"));
	}
	FVector Direction(0, 0, 1);
	if (Params->HasField(TEXT("direction")))
	{
		Direction = mcp_common::ReadJsonVector(Params, TEXT("direction"));
	}

	FString FalloffStr = TEXT("Linear");
	Params->TryGetStringField(TEXT("falloff_type"), FalloffStr);
	const EFieldFalloffType FalloffType = ParseFalloff(FalloffStr);

	// Range remap shared by the falloff/noise nodes (engine defaults: 0..1, Default 0).
	double MinRange = 0.0;
	Params->TryGetNumberField(TEXT("min_range"), MinRange);
	double MaxRange = 1.0;
	Params->TryGetNumberField(TEXT("max_range"), MaxRange);
	double DefaultValue = 0.0;
	Params->TryGetNumberField(TEXT("default_value"), DefaultValue);

	// PlaneFalloff-specific (engine default Distance 0; Normal reuses 'direction').
	double Distance = 0.0;
	Params->TryGetNumberField(TEXT("distance"), Distance);

	// WaveScalar-specific (engine defaults: Wavelength 10000, Period 1, Cosine).
	double Wavelength = 10000.0;
	Params->TryGetNumberField(TEXT("wavelength"), Wavelength);
	double Period = 1.0;
	Params->TryGetNumberField(TEXT("period"), Period);
	FString WaveFunctionStr = TEXT("Cosine");
	Params->TryGetStringField(TEXT("wave_function"), WaveFunctionStr);

	// RadialIntMask-specific (engine defaults: Interior 1, Exterior 0, Always).
	int32 InteriorValue = 1;
	Params->TryGetNumberField(TEXT("interior_value"), InteriorValue);
	int32 ExteriorValue = 0;
	Params->TryGetNumberField(TEXT("exterior_value"), ExteriorValue);
	FString MaskConditionStr = TEXT("Always");
	Params->TryGetStringField(TEXT("mask_condition"), MaskConditionStr);

	// RotatedRadialVector-specific (engine default Angle 0; Axis reuses 'direction').
	double Angle = 0.0;
	Params->TryGetNumberField(TEXT("angle"), Angle);

	// Resolve the physics target -> EFieldPhysicsType.
	const FName TargetName(*Target);
	const EFieldPhysicsType PhysicsType = GetFieldPhysicsType(TargetName);
	if (PhysicsType == EFieldPhysicsType::Field_None)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Unknown field target '%s' (valid: LinearForce, LinearVelocity, "
				"ExternalClusterStrain, DynamicState, etc.)"), *Target));
	}

	// Build the transient authoring U-node from node_type, set its public members directly.
	UPackage* TransientPkg = GetTransientPackage();
	UFieldNodeBase* RootNode = nullptr;
	const FString NodeKey = NodeType.ToLower();

	if (NodeKey == TEXT("radialfalloff"))
	{
		URadialFalloff* Node = NewObject<URadialFalloff>(TransientPkg);
		Node->Magnitude = static_cast<float>(Magnitude);
		Node->Radius    = static_cast<float>(Radius);
		Node->Position  = Position;
		Node->Falloff   = FalloffType;
		RootNode = Node;
	}
	else if (NodeKey == TEXT("uniformvector"))
	{
		UUniformVector* Node = NewObject<UUniformVector>(TransientPkg);
		Node->Magnitude = static_cast<float>(Magnitude);
		Node->Direction = Direction;
		RootNode = Node;
	}
	else if (NodeKey == TEXT("radialvector"))
	{
		URadialVector* Node = NewObject<URadialVector>(TransientPkg);
		Node->Magnitude = static_cast<float>(Magnitude);
		Node->Position  = Position;
		RootNode = Node;
	}
	else if (NodeKey == TEXT("uniformscalar"))
	{
		UUniformScalar* Node = NewObject<UUniformScalar>(TransientPkg);
		Node->Magnitude = static_cast<float>(Magnitude);
		RootNode = Node;
	}
	else if (NodeKey == TEXT("uniforminteger"))
	{
		UUniformInteger* Node = NewObject<UUniformInteger>(TransientPkg);
		Node->Magnitude = static_cast<int32>(Magnitude);
		RootNode = Node;
	}
	else if (NodeKey == TEXT("boxfalloff"))
	{
		UBoxFalloff* Node = NewObject<UBoxFalloff>(TransientPkg);
		Node->Magnitude = static_cast<float>(Magnitude);
		// Author the full range set: the 0..1 falloff value is remapped into
		// [MinRange, MaxRange] then multiplied by Magnitude, and Default is the
		// value used outside the box. Omitting them (the original bug) silently
		// left an under-authored node that ignored these inputs.
		Node->MinRange  = static_cast<float>(MinRange);
		Node->MaxRange  = static_cast<float>(MaxRange);
		Node->Default   = static_cast<float>(DefaultValue);
		Node->Transform = FTransform(Position);
		Node->Falloff   = FalloffType;
		RootNode = Node;
	}
	else if (NodeKey == TEXT("planefalloff"))
	{
		UPlaneFalloff* Node = NewObject<UPlaneFalloff>(TransientPkg);
		Node->Magnitude = static_cast<float>(Magnitude);
		Node->MinRange  = static_cast<float>(MinRange);
		Node->MaxRange  = static_cast<float>(MaxRange);
		Node->Default   = static_cast<float>(DefaultValue);
		Node->Distance  = static_cast<float>(Distance);
		Node->Position  = Position;
		Node->Normal    = Direction;
		Node->Falloff   = FalloffType;
		RootNode = Node;
	}
	else if (NodeKey == TEXT("wavescalar"))
	{
		UWaveScalar* Node = NewObject<UWaveScalar>(TransientPkg);
		Node->Magnitude  = static_cast<float>(Magnitude);
		Node->Position   = Position;
		Node->Wavelength = static_cast<float>(Wavelength);
		Node->Period     = static_cast<float>(Period);
		Node->Function   = ParseWaveFunction(WaveFunctionStr);
		Node->Falloff    = FalloffType;
		RootNode = Node;
	}
	else if (NodeKey == TEXT("radialintmask"))
	{
		URadialIntMask* Node = NewObject<URadialIntMask>(TransientPkg);
		Node->Radius           = static_cast<float>(Radius);
		Node->Position         = Position;
		Node->InteriorValue    = InteriorValue;
		Node->ExteriorValue    = ExteriorValue;
		Node->SetMaskCondition = ParseMaskCondition(MaskConditionStr);
		RootNode = Node;
	}
	else if (NodeKey == TEXT("rotatedradialvector"))
	{
		URotatedRadialVector* Node = NewObject<URotatedRadialVector>(TransientPkg);
		Node->Magnitude = static_cast<float>(Magnitude);
		Node->Position  = Position;
		Node->Angle     = static_cast<float>(Angle);
		Node->Axis      = Direction;
		RootNode = Node;
	}
	else if (NodeKey == TEXT("randomvector"))
	{
		URandomVector* Node = NewObject<URandomVector>(TransientPkg);
		Node->Magnitude = static_cast<float>(Magnitude);
		RootNode = Node;
	}
	else if (NodeKey == TEXT("noisefield"))
	{
		UNoiseField* Node = NewObject<UNoiseField>(TransientPkg);
		Node->Transform = FTransform(Position);
		RootNode = Node;
	}
	else
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Unknown node_type '%s' (valid: RadialFalloff, PlaneFalloff, BoxFalloff, "
				"WaveScalar, UniformScalar, NoiseField, UniformInteger, RadialIntMask, UniformVector, "
				"RadialVector, RotatedRadialVector, RandomVector)"), *NodeType));
	}

	// Build the runtime command and append it to the asset's persisted command array.
	FFieldSystemCommand Command = FFieldObjectCommands::CreateFieldCommand(PhysicsType, RootNode, nullptr);
	if (!Command.RootNode)
	{
		return mcp_common::MakeErrorReply(
			TEXT("Failed to build field command evaluation graph from the authoring node"));
	}
	Command.TargetAttribute = TargetName;
	if (Command.CommandName.IsNone())
	{
		Command.CommandName = TargetName;
	}
	FS->Commands.Add(Command);

	UPackage* Package = FS->GetPackage();
	FString SaveError;
	const bool bSaved = Package ? mcp_common::SaveAssetPackage(Package, FS, SaveError) : false;

	const FName PhysicsName = GetFieldPhysicsName(PhysicsType);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("field_system_path"), FieldSystemPath);
	Data->SetStringField(TEXT("node_type"), NodeType);
	Data->SetStringField(TEXT("target"), Target);
	Data->SetStringField(TEXT("physics_type_name"), PhysicsName.ToString());
	ChaosFields_AddCountAndSaveError(Data, *FS, bSaved, SaveError);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("add_field_node"));
}

// ---------------------------------------------------------------------------
// 3. remove_field_node
// ---------------------------------------------------------------------------
// MCP:remove_field_node - Remove a field command node from a UFieldSystem by index and persist.  Inverse of add_field_node: selects the FFieldSystemCommand at the given index in UFieldSystem.Commands, removes it (RemoveAt frees the command's owned RootNode graph), and saves the asset.  Idempotent: an out-of-range index is a clean no-op (removed:false, note:"index out of range") so re-running after the node is already gone is safe.
//   @param field_system_path - Asset path of the UFieldSystem (e.g. "/Game/MyGame/FS_Example")
//   @param index - Zero-based index of the field command to remove
//   @returns { field_system_path: string, removed: bool, removed_index?: int, removed_target?: string, command_count: int, success: bool, saved?: bool, save_error?: string, note?: string }

TSharedPtr<FJsonObject> HandleRemoveFieldNode(
	const TSharedPtr<FJsonObject>& Params)
{
	FString FieldSystemPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("field_system_path"), FieldSystemPath)) { return Missing; }
	int32 Index = 0;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("index"), Index)) { return Missing; }

	UFieldSystem* FS = nullptr;
	if (TSharedPtr<FJsonObject> LoadError = ChaosFields_LoadFieldSystem(FieldSystemPath, FS)) { return LoadError; }

	// Out-of-range index is an honest no-op: nothing removed, asset untouched.
	// Re-running after the node is already gone stays safe (idempotent).
	if (Index < 0 || Index >= FS->Commands.Num())
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("field_system_path"), FieldSystemPath);
		Data->SetBoolField(TEXT("removed"), false);
		Data->SetNumberField(TEXT("command_count"), FS->Commands.Num());
		Data->SetBoolField(TEXT("success"), true);
		Data->SetStringField(TEXT("note"), TEXT("index out of range"));
		return mcp_common::MakeOkReply(Data);
	}

	// Capture the target attribute before removal. RemoveAt destroys the command's
	// TUniquePtr<FFieldNodeBase> RootNode, freeing the owned graph — no manual delete.
	const FString RemovedTarget = FS->Commands[Index].TargetAttribute.ToString();
	FS->Commands.RemoveAt(Index);

	UPackage* Package = FS->GetPackage();
	FString SaveError;
	const bool bSaved = Package ? mcp_common::SaveAssetPackage(Package, FS, SaveError) : false;

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("field_system_path"), FieldSystemPath);
	Data->SetBoolField(TEXT("removed"), true);
	Data->SetNumberField(TEXT("removed_index"), Index);
	Data->SetStringField(TEXT("removed_target"), RemovedTarget);
	ChaosFields_AddCountAndSaveError(Data, *FS, bSaved, SaveError);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("remove_field_node"));
}

// ---------------------------------------------------------------------------
// 4. spawn_field_system_actor
// ---------------------------------------------------------------------------
// MCP:spawn_field_system_actor - Spawn an AFieldSystemActor in the editor world and assign a UFieldSystem.  Spawns the actor at the given location, assigns the asset via UFieldSystemComponent::SetFieldSystem, and sets the actor label.
//   @param actor_label - Editor label for the spawned actor
//   @param field_system_path - Asset path of the UFieldSystem to assign
//   @param location - Optional [x, y, z] world location; defaults to (0,0,0)
//   @returns { actor_label: string, field_system_path: string, actor_name: string, has_field_system: bool, success: bool }

TSharedPtr<FJsonObject> HandleSpawnFieldSystemActor(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ActorLabel;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_label"), ActorLabel)) { return Missing; }
	FString FieldSystemPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("field_system_path"), FieldSystemPath)) { return Missing; }

	UFieldSystem* FS = nullptr;
	if (TSharedPtr<FJsonObject> LoadError = ChaosFields_LoadFieldSystem(FieldSystemPath, FS)) { return LoadError; }

	// Spawned at the origin unless the caller gives a location; never rotated.
	const FVector SpawnPoint = Params->HasField(TEXT("location"))
		? mcp_common::ReadJsonVector(Params, TEXT("location"))
		: FVector::ZeroVector;

	if (!GEditor)
	{
		return mcp_common::MakeErrorReply(TEXT("GEditor is unavailable"));
	}
	UWorld* EditorWorld = nullptr;
	if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(EditorWorld)) { return NoWorld; }

	AFieldSystemActor* Actor = EditorWorld->SpawnActor<AFieldSystemActor>(SpawnPoint, FRotator::ZeroRotator);
	if (!Actor)
	{
		return mcp_common::MakeErrorReply(TEXT("Failed to spawn AFieldSystemActor"));
	}

	bool bHasFieldSystem = false;
	if (UFieldSystemComponent* Comp = Actor->GetFieldSystemComponent())
	{
		Comp->SetFieldSystem(FS);
		bHasFieldSystem = (Comp->GetFieldSystem() != nullptr);
	}

	Actor->SetActorLabel(ActorLabel);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_label"), ActorLabel);
	Data->SetStringField(TEXT("field_system_path"), FieldSystemPath);
	Data->SetStringField(TEXT("actor_name"), Actor->GetName());
	Data->SetBoolField(TEXT("has_field_system"), bHasFieldSystem);
	Data->SetBoolField(TEXT("success"), true);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// 5. get_field_system_info
// ---------------------------------------------------------------------------
// MCP:get_field_system_info - Read the field commands authored in a UFieldSystem asset, in order: each with physics_type, target_attribute, command_name and max_magnitude (not its node graph), plus command_count. Read-only; the array index is what remove_field_node takes, and add_field_node authors new commands.
//   @param field_system_path - Asset path of the UFieldSystem, e.g. "/Game/MyGame/FS_Example" or "/Game/MyGame/FS_Example.FS_Example".
//   @returns { field_system_path: string, command_count: int, commands: [{ physics_type, target_attribute, command_name, max_magnitude }], success: bool }

TSharedPtr<FJsonObject> HandleGetFieldSystemInfo(
	const TSharedPtr<FJsonObject>& Params)
{
	FString FieldSystemPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("field_system_path"), FieldSystemPath)) { return Missing; }

	UFieldSystem* FS = nullptr;
	if (TSharedPtr<FJsonObject> LoadError = ChaosFields_LoadFieldSystem(FieldSystemPath, FS)) { return LoadError; }

	TArray<TSharedPtr<FJsonValue>> Commands;
	for (const FFieldSystemCommand& Cmd : FS->Commands)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		const FName PhysicsName = GetFieldPhysicsName(Cmd.PhysicsType);
		Entry->SetStringField(TEXT("physics_type"), PhysicsName.ToString());
		Entry->SetStringField(TEXT("target_attribute"), Cmd.TargetAttribute.ToString());
		Entry->SetStringField(TEXT("command_name"), Cmd.CommandName.ToString());
		Entry->SetNumberField(TEXT("max_magnitude"), Cmd.MaxMagnitude);
		Commands.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("field_system_path"), FieldSystemPath);
	Data->SetNumberField(TEXT("command_count"), FS->Commands.Num());
	Data->SetArrayField(TEXT("commands"), Commands);
	Data->SetBoolField(TEXT("success"), true);
	return mcp_common::MakeOkReply(Data);
}

const McpCommandFamilies::FRegistrar Registration =
{
	{ TEXT("add_field_node"), &HandleAddFieldNode },
	{ TEXT("create_field_system_asset"), &HandleCreateFieldSystemAsset },
	{ TEXT("get_field_system_info"), &HandleGetFieldSystemInfo },
	{ TEXT("remove_field_node"), &HandleRemoveFieldNode },
	{ TEXT("spawn_field_system_actor"), &HandleSpawnFieldSystemActor },
};
} // namespace UEAgentChaosFieldsCommands
