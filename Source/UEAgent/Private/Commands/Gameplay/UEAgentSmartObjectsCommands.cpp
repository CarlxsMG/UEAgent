#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Common/UEAgentGraphUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameplayTagContainer.h"
#include "GameplayTagsManager.h"
#include "Misc/PackageName.h"
#include "SmartObjectComponent.h"
#include "SmartObjectDefinition.h"
#include "Annotations/SmartObjectSlotEntranceAnnotation.h"   // Slot entrance annotations
#include "SmartObjectPersistentCollection.h"
#include "SmartObjectRequestTypes.h"   // FSmartObjectRequest / Filter / RequestResult (spatial find)
#include "SmartObjectRuntime.h"
#include "SmartObjectSubsystem.h"
#include "SmartObjectTypes.h"
#include "ScopedTransaction.h"      // FScopedTransaction: the slot setters and removers are undoable
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"   // FArrayProperty / FScriptArrayHelper for private-member reflection

/**
 * Smart Objects commands.
 *
 * Wraps the engine's SmartObjects plugin (USmartObjectDefinition, USmartObjectComponent,
 * USmartObjectSubsystem) so an LLM can author definition assets and their slots, spawn actors with the SO
 * component, claim/release via the subsystem, and read back info.
 */
namespace UEAgentSmartObjectsCommands
{

// MCP-DOMAIN: SmartObjects

namespace
{
	// --- SmartObject-authoring helpers -----------------------------

	// Parse a JSON string-array of tag names into a FGameplayTagContainer
	// (tags not registered in the project are skipped, not errors).
	FGameplayTagContainer ParseTagContainer(const TArray<TSharedPtr<FJsonValue>>* TagsArr)
	{
		FGameplayTagContainer Container;
		if (!TagsArr) { return Container; }
		for (const TSharedPtr<FJsonValue>& V : *TagsArr)
		{
			FString TagStr;
			if (V.IsValid() && V->TryGetString(TagStr) && !TagStr.IsEmpty())
			{
				const FGameplayTag Tag = UGameplayTagsManager::Get().RequestGameplayTag(
					FName(*TagStr), /*ErrorIfNotFound*/ false);
				if (Tag.IsValid()) { Container.AddTag(Tag); }
			}
		}
		return Container;
	}

	// ESmartObjectEntrancePriority <-> string (entrance selection priority).
	bool ParseEntrancePriority(const FString& In, ESmartObjectEntrancePriority& Out)
	{
		static const TMap<FString, ESmartObjectEntrancePriority> Map = {
			{ TEXT("lowest"),      ESmartObjectEntrancePriority::Lowest },
			{ TEXT("lower"),       ESmartObjectEntrancePriority::Lower },
			{ TEXT("low"),         ESmartObjectEntrancePriority::Low },
			{ TEXT("belownormal"), ESmartObjectEntrancePriority::BelowNormal },
			{ TEXT("normal"),      ESmartObjectEntrancePriority::Normal },
			{ TEXT("abovenormal"), ESmartObjectEntrancePriority::AboveNormal },
			{ TEXT("high"),        ESmartObjectEntrancePriority::High },
			{ TEXT("higher"),      ESmartObjectEntrancePriority::Higher },
			{ TEXT("highest"),     ESmartObjectEntrancePriority::Highest },
		};
		if (const ESmartObjectEntrancePriority* Found = Map.Find(In.ToLower()))
		{
			Out = *Found;
			return true;
		}
		return false;
	}

	const TCHAR* EntrancePriorityName(ESmartObjectEntrancePriority V)
	{
		switch (V)
		{
			case ESmartObjectEntrancePriority::Lowest:      return TEXT("Lowest");
			case ESmartObjectEntrancePriority::Lower:       return TEXT("Lower");
			case ESmartObjectEntrancePriority::Low:         return TEXT("Low");
			case ESmartObjectEntrancePriority::BelowNormal: return TEXT("BelowNormal");
			case ESmartObjectEntrancePriority::Normal:      return TEXT("Normal");
			case ESmartObjectEntrancePriority::AboveNormal: return TEXT("AboveNormal");
			case ESmartObjectEntrancePriority::High:        return TEXT("High");
			case ESmartObjectEntrancePriority::Higher:      return TEXT("Higher");
			case ESmartObjectEntrancePriority::Highest:     return TEXT("Highest");
			default:                                        return TEXT("Normal");
		}
	}

	// Build a user-tag query from a container + match mode ("any"|"all"|"none").
	FGameplayTagQuery BuildTagQuery(const FGameplayTagContainer& Tags, const FString& Match)
	{
		if (Match.Equals(TEXT("all"), ESearchCase::IgnoreCase))
		{
			return FGameplayTagQuery::MakeQuery_MatchAllTags(Tags);
		}
		if (Match.Equals(TEXT("none"), ESearchCase::IgnoreCase))
		{
			return FGameplayTagQuery::MakeQuery_MatchNoTags(Tags);
		}
		return FGameplayTagQuery::MakeQuery_MatchAnyTags(Tags);
	}

	FString MergingPolicyToString(const ESmartObjectTagMergingPolicy P)
	{
		return P == ESmartObjectTagMergingPolicy::Override ? TEXT("Override") : TEXT("Combine");
	}
	bool ParseMergingPolicy(const FString& S, ESmartObjectTagMergingPolicy& Out)
	{
		if (S.Equals(TEXT("Combine"), ESearchCase::IgnoreCase))  { Out = ESmartObjectTagMergingPolicy::Combine;  return true; }
		if (S.Equals(TEXT("Override"), ESearchCase::IgnoreCase)) { Out = ESmartObjectTagMergingPolicy::Override; return true; }
		return false;
	}
	FString FilteringPolicyToString(const ESmartObjectTagFilteringPolicy P)
	{
		switch (P)
		{
			case ESmartObjectTagFilteringPolicy::Combine:  return TEXT("Combine");
			case ESmartObjectTagFilteringPolicy::Override: return TEXT("Override");
			default:                                       return TEXT("NoFilter");
		}
	}
	bool ParseFilteringPolicy(const FString& S, ESmartObjectTagFilteringPolicy& Out)
	{
		if (S.Equals(TEXT("NoFilter"), ESearchCase::IgnoreCase)) { Out = ESmartObjectTagFilteringPolicy::NoFilter; return true; }
		if (S.Equals(TEXT("Combine"), ESearchCase::IgnoreCase))  { Out = ESmartObjectTagFilteringPolicy::Combine;  return true; }
		if (S.Equals(TEXT("Override"), ESearchCase::IgnoreCase)) { Out = ESmartObjectTagFilteringPolicy::Override; return true; }
		return false;
	}

	// Resolve a UClass from a name — full /Script path or short native name
	// (mirrors UEAgentGameFrameworkCommands::ResolveClassByName).
	UClass* ResolveBehaviorClass(const FString& Name)
	{
		return FUEAgentGraphUtils::ResolveClass(Name);
	}

	/** Reflect the size of the private USmartObjectDefinition::DefaultBehaviorDefinitions array. */
	int32 GetDefaultBehaviorCount(USmartObjectDefinition* Def)
	{
		if (!Def) { return 0; }
		FArrayProperty* P = CastField<FArrayProperty>(
			USmartObjectDefinition::StaticClass()->FindPropertyByName(TEXT("DefaultBehaviorDefinitions")));
		if (!P) { return 0; }
		return FScriptArrayHelper(P, P->ContainerPtrToValuePtr<void>(Def)).Num();
	}

	/** Load the USmartObjectDefinition at DefinitionPath; on failure returns nullptr and fills OutError. */
	USmartObjectDefinition* SmartObjects_LoadDefinitionAt(const FString& DefinitionPath, TSharedPtr<FJsonObject>& OutError)
	{
		USmartObjectDefinition* Def = LoadObject<USmartObjectDefinition>(nullptr, *DefinitionPath);
		if (!Def)
		{
			OutError = mcp_common::MakeErrorReply(
				FString::Printf(TEXT("Cannot load definition: %s"), *DefinitionPath));
		}
		return Def;
	}

	/** SmartObjects_LoadDefinitionAt for the required definition_path param. */
	USmartObjectDefinition* SmartObjects_LoadDefinition(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutError)
	{
		FString DefinitionPath;
		if (!Params->TryGetStringField(TEXT("definition_path"), DefinitionPath) || DefinitionPath.IsEmpty())
		{
			OutError = mcp_common::MakeErrorReply(TEXT("definition_path is required"));
			return nullptr;
		}
		return SmartObjects_LoadDefinitionAt(DefinitionPath, OutError);
	}

	/** SmartObjects_LoadDefinition, then the slot_index param, which must name one of its slots. */
	USmartObjectDefinition* SmartObjects_LoadDefinitionSlot(const TSharedPtr<FJsonObject>& Params, int32& OutSlotIndex,
		TSharedPtr<FJsonObject>& OutError)
	{
		USmartObjectDefinition* Def = SmartObjects_LoadDefinition(Params, OutError);
		if (!Def) { return nullptr; }
		OutSlotIndex = -1;
		{ double D = -1.0; if (Params->TryGetNumberField(TEXT("slot_index"), D)) { OutSlotIndex = static_cast<int32>(D); } }
		if (!Def->IsValidSlotIndex(OutSlotIndex))
		{
			OutError = mcp_common::MakeErrorReply(
				FString::Printf(TEXT("Invalid slot_index %d (definition has %d slots)"), OutSlotIndex, Def->GetSlots().Num()));
			return nullptr;
		}
		return Def;
	}

	/** Marks Def's package dirty and saves it; false when the save fails. */
	bool SmartObjects_SaveDefinition(USmartObjectDefinition* Def)
	{
		Def->MarkPackageDirty();
		return mcp_common::SaveAssetPackage(Def->GetPackage(), Def);
	}

	/** A reply payload that starts with Def's definition_path. */
	TSharedPtr<FJsonObject> SmartObjects_NewDefinitionData(const USmartObjectDefinition* Def)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("definition_path"), Def->GetPathName());
		return Data;
	}

	/** SmartObjects_NewDefinitionData followed by slot_index. */
	TSharedPtr<FJsonObject> SmartObjects_NewSlotData(const USmartObjectDefinition* Def, int32 SlotIndex)
	{
		TSharedPtr<FJsonObject> Data = SmartObjects_NewDefinitionData(Def);
		Data->SetNumberField(TEXT("slot_index"), SlotIndex);
		return Data;
	}

	/** The editor world's USmartObjectSubsystem; on failure returns nullptr and fills OutError. */
	USmartObjectSubsystem* SmartObjects_GetSubsystem(UWorld*& OutWorld, TSharedPtr<FJsonObject>& OutError)
	{
		if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(OutWorld))
		{
			OutError = NoWorld;
			return nullptr;
		}
		USmartObjectSubsystem* Sub = USmartObjectSubsystem::GetCurrent(OutWorld);
		if (!Sub)
		{
			OutError = mcp_common::MakeErrorReply(TEXT("USmartObjectSubsystem is not available"));
		}
		return Sub;
	}

	/**
	 * The USmartObjectComponent of the World actor labelled ActorLabel; on failure returns nullptr and fills OutError
	 * ("<NotFoundPrefix>: <ActorLabel>" when no actor has that label).
	 */
	USmartObjectComponent* SmartObjects_FindComponent(UWorld* World, const FString& ActorLabel, const TCHAR* NotFoundPrefix,
		TSharedPtr<FJsonObject>& OutError)
	{
		AActor* Actor = mcp_common::FindActorByLabel(World, ActorLabel);
		if (!Actor)
		{
			OutError = mcp_common::MakeErrorReply(FString::Printf(TEXT("%s: %s"), NotFoundPrefix, *ActorLabel));
			return nullptr;
		}
		USmartObjectComponent* Comp = Actor->FindComponentByClass<USmartObjectComponent>();
		if (!Comp)
		{
			OutError = mcp_common::MakeErrorReply(
				FString::Printf(TEXT("Actor %s has no USmartObjectComponent"), *ActorLabel));
		}
		return Comp;
	}

	/**
	 * The class BehaviorClassName names, which must derive from USmartObjectBehaviorDefinition; on failure returns
	 * nullptr and fills OutError (NotFoundError when no class has that name).
	 */
	UClass* SmartObjects_ResolveBehaviorClass(const FString& BehaviorClassName, const FString& NotFoundError,
		TSharedPtr<FJsonObject>& OutError)
	{
		UClass* BehaviorClass = ResolveBehaviorClass(BehaviorClassName);
		if (!BehaviorClass)
		{
			OutError = mcp_common::MakeErrorReply(NotFoundError);
			return nullptr;
		}
		if (!BehaviorClass->IsChildOf(USmartObjectBehaviorDefinition::StaticClass()))
		{
			OutError = mcp_common::MakeErrorReply(
				FString::Printf(TEXT("%s does not derive from USmartObjectBehaviorDefinition"), *BehaviorClass->GetName()));
			return nullptr;
		}
		return BehaviorClass;
	}

	/** A new behavior outered to Def, of the concrete class BehaviorClassName names; on failure returns nullptr and fills OutError. */
	USmartObjectBehaviorDefinition* SmartObjects_NewBehavior(USmartObjectDefinition* Def, const FString& BehaviorClassName,
		const FString& NotFoundError, TSharedPtr<FJsonObject>& OutError)
	{
		UClass* BehaviorClass = SmartObjects_ResolveBehaviorClass(BehaviorClassName, NotFoundError, OutError);
		if (!BehaviorClass) { return nullptr; }
		if (BehaviorClass->HasAnyClassFlags(CLASS_Abstract))
		{
			OutError = mcp_common::MakeErrorReply(
				FString::Printf(TEXT("%s is abstract and cannot be instantiated"), *BehaviorClass->GetName()));
			return nullptr;
		}
		USmartObjectBehaviorDefinition* Behavior = NewObject<USmartObjectBehaviorDefinition>(
			Def, BehaviorClass, NAME_None, RF_Transactional);
		if (!Behavior)
		{
			OutError = mcp_common::MakeErrorReply(
				FString::Printf(TEXT("Failed to instantiate %s"), *BehaviorClass->GetName()));
		}
		return Behavior;
	}

	bool SmartObjects_IsEntrance(const FSmartObjectDefinitionDataProxy& Proxy)
	{
		const UScriptStruct* DS = Proxy.Data.GetScriptStruct();
		return DS && DS->IsChildOf(FSmartObjectSlotEntranceAnnotation::StaticStruct());
	}

	int32 SmartObjects_CountEntrances(const FSmartObjectSlotDefinition& Slot)
	{
		int32 Count = 0;
		for (const FSmartObjectDefinitionDataProxy& Proxy : Slot.DefinitionData)
		{
			if (SmartObjects_IsEntrance(Proxy)) { ++Count; }
		}
		return Count;
	}

	/**
	 * Read the optional offset [x, y, z] and rotation [pitch, yaw, roll] params into OutOffset and OutRotation; each
	 * keeps its value when its param is absent or has fewer than 3 elements.
	 */
	void SmartObjects_ReadOffsetRotation(const TSharedPtr<FJsonObject>& Params, FVector3f& OutOffset, FRotator3f& OutRotation)
	{
		const TArray<TSharedPtr<FJsonValue>>* OffArr = nullptr;
		if (Params->TryGetArrayField(TEXT("offset"), OffArr) && OffArr && OffArr->Num() >= 3)
		{
			OutOffset = FVector3f(
				static_cast<float>((*OffArr)[0]->AsNumber()),
				static_cast<float>((*OffArr)[1]->AsNumber()),
				static_cast<float>((*OffArr)[2]->AsNumber()));
		}
		const TArray<TSharedPtr<FJsonValue>>* RotArr = nullptr;
		if (Params->TryGetArrayField(TEXT("rotation"), RotArr) && RotArr && RotArr->Num() >= 3)
		{
			OutRotation = FRotator3f(
				static_cast<float>((*RotArr)[0]->AsNumber()),
				static_cast<float>((*RotArr)[1]->AsNumber()),
				static_cast<float>((*RotArr)[2]->AsNumber()));
		}
	}
}

namespace UEAgentSmartObjectsImpl
{
	// The first slot of the list that the subsystem currently reports as free; an invalid
	// handle when every slot is taken.
	FSmartObjectSlotHandle FirstFreeSlot(USmartObjectSubsystem& Subsystem, const TArray<FSmartObjectSlotHandle>& Slots)
	{
		for (const FSmartObjectSlotHandle& Candidate : Slots)
		{
			if (Subsystem.GetSlotState(Candidate) == ESmartObjectSlotState::Free)
			{
				return Candidate;
			}
		}
		return FSmartObjectSlotHandle();
	}

	// The first free slot of Component's registered smart object; an invalid handle, with OutError filled, when the
	// component is not registered, its object has no slots, or none of them is free (NoFreeSlotError).
	FSmartObjectSlotHandle FirstFreeSlotOf(USmartObjectSubsystem& Subsystem, const USmartObjectComponent& Component,
		const TCHAR* NoFreeSlotError, TSharedPtr<FJsonObject>& OutError)
	{
		const FSmartObjectHandle SOHandle = Component.GetRegisteredHandle();
		if (!SOHandle.IsValid())
		{
			OutError = mcp_common::MakeErrorReply(TEXT("Component's SmartObject handle is invalid (not registered?)"));
			return FSmartObjectSlotHandle();
		}
		TArray<FSmartObjectSlotHandle> SlotHandles;
		Subsystem.GetAllSlots(SOHandle, SlotHandles);
		if (SlotHandles.Num() == 0)
		{
			OutError = mcp_common::MakeErrorReply(TEXT("Definition has no slots; call add_smart_object_slot first"));
			return FSmartObjectSlotHandle();
		}
		const FSmartObjectSlotHandle FreeSlot = FirstFreeSlot(Subsystem, SlotHandles);
		if (!FreeSlot.IsValid())
		{
			OutError = mcp_common::MakeErrorReply(NoFreeSlotError);
		}
		return FreeSlot;
	}
}

// ---------------------------------------------------------------------------
// create_smart_object_definition
// ---------------------------------------------------------------------------
// MCP:create_smart_object_definition - Create a USmartObjectDefinition asset at package_path/definition_name, or load the one already there, add default slots (enabled, no tags) until it has num_activity_slots, then save it. Slots already there are kept and never taken away, so a smaller count adds nothing and a repeat call is idempotent; created says whether the asset is new. A name taken by another asset class, or a path that is not an asset path, is an error; a failed save, as during PIE, makes success false with saved=false. No undo step is recorded. Named slots: add_smart_object_slot; slot fields: set_smart_object_slot_activity_tags or set_smart_object_slot_enabled; a behavior, which register_smart_object_actor needs: add_smart_object_definition_default_behavior or add_smart_object_slot_behavior_definition; read back: get_smart_object_definition_summary.
//   @param definition_name - Name for the new Smart Object definition, also its asset name
//   @param package_path - Content folder; defaults to /Game/MCP
//   @param num_activity_slots - Slot count to reach, default 1: slots are added until the definition has this many, none are taken away
//   @returns { definition_path: string, definition_name: string, num_slots: int, created: bool, saved: bool }

TSharedPtr<FJsonObject> HandleCreateSmartObjectDefinition(
	const TSharedPtr<FJsonObject>& Params)
{
	FString DefinitionName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("definition_name"), DefinitionName)) { return Missing; }
	FString PackagePath = mcp_common::DefaultAssetFolder();
	Params->TryGetStringField(TEXT("package_path"), PackagePath);
	int32 NumActivitySlots = 1;
	Params->TryGetNumberField(TEXT("num_activity_slots"), NumActivitySlots);

	const FString FullPath = PackagePath / DefinitionName;
	FString CreateError;
	bool bCreated = false;
	USmartObjectDefinition* Def = mcp_common::CreateOrLoadAsset<USmartObjectDefinition>(FullPath, CreateError, &bCreated);
	if (!Def)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Failed to create or load smart object definition: %s"), *CreateError));
	}

	// Add slots via the public DebugAddSlot() API (the only canonical way
	// in 5.7 to append slots from outside the editor; GetMutableSlots
	// returns a read-only TArrayView).
	const int32 Existing = Def->GetSlots().Num();
	while (Def->GetSlots().Num() < NumActivitySlots)
	{
		FSmartObjectSlotDefinition& NewSlot = Def->DebugAddSlot();
		NewSlot.bEnabled = true;
	}
	if (Def->GetSlots().Num() != Existing)
	{
		Def->MarkPackageDirty();
	}
	const bool bSaved = mcp_common::SaveAssetPackage(Def->GetPackage(), Def);

	TSharedPtr<FJsonObject> Data = SmartObjects_NewDefinitionData(Def);
	Data->SetStringField(TEXT("definition_name"), Def->GetName());
	Data->SetNumberField(TEXT("num_slots"), Def->GetSlots().Num());
	Data->SetBoolField(TEXT("created"), bCreated);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("create_smart_object_definition"));
}

// ---------------------------------------------------------------------------
// add_smart_object_slot
// ---------------------------------------------------------------------------
// MCP:add_smart_object_slot - Append a named activity slot to a USmartObjectDefinition (idempotent on name).
//   @param definition_path - Path to the USmartObjectDefinition asset
//   @param slot_name - Unique name for the new slot
//   @param activity_tag - Optional FGameplayTag to attach to the slot; defaults to "Default"
//   @returns { definition_path: string, slot_name: string, activity_tag: string, slot_count: int, added?: bool, already_existed?: bool, saved: bool }

TSharedPtr<FJsonObject> HandleAddSmartObjectSlot(
	const TSharedPtr<FJsonObject>& Params)
{
	FString DefinitionPath, SlotName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("definition_path"), DefinitionPath)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("slot_name"), SlotName)) { return Missing; }
	FString ActivityTag = TEXT("Default");
	Params->TryGetStringField(TEXT("activity_tag"), ActivityTag);

	TSharedPtr<FJsonObject> Err;
	USmartObjectDefinition* Def = SmartObjects_LoadDefinitionAt(DefinitionPath, Err);
	if (!Def) { return Err; }

	for (const FSmartObjectSlotDefinition& Slot : Def->GetSlots())
	{
		if (Slot.Name == FName(*SlotName))
		{
			TSharedPtr<FJsonObject> Data = SmartObjects_NewDefinitionData(Def);
			Data->SetStringField(TEXT("slot_name"), SlotName);
			Data->SetNumberField(TEXT("slot_count"), Def->GetSlots().Num());
			Data->SetBoolField(TEXT("already_existed"), true);
			return mcp_common::MakeOkReply(Data);
		}
	}

	// Add the slot via DebugAddSlot (public API; the underlying field
	// SlotName is WITH_EDITORONLY_DATA so it is set but ignored in cooked
	// builds — this is the engine's documented behaviour).
	FSmartObjectSlotDefinition& NewSlot = Def->DebugAddSlot();
	NewSlot.Name = FName(*SlotName);
	NewSlot.bEnabled = true;
	if (!ActivityTag.IsEmpty())
	{
		FGameplayTag Tag = UGameplayTagsManager::Get().RequestGameplayTag(
			FName(*ActivityTag), /*ErrorIfNotFound*/ false);
		if (Tag.IsValid())
		{
			NewSlot.ActivityTags.AddTag(Tag);
		}
	}
	const bool bSaved = SmartObjects_SaveDefinition(Def);

	TSharedPtr<FJsonObject> Data = SmartObjects_NewDefinitionData(Def);
	Data->SetStringField(TEXT("slot_name"), SlotName);
	Data->SetStringField(TEXT("activity_tag"), ActivityTag);
	Data->SetNumberField(TEXT("slot_count"), Def->GetSlots().Num());
	Data->SetBoolField(TEXT("added"), true);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("add_smart_object_slot"));
}

// ---------------------------------------------------------------------------
// register_smart_object_actor
// ---------------------------------------------------------------------------
// MCP:register_smart_object_actor - Spawn an empty actor in the editor world, attach a USmartObjectComponent pointing at the given definition, and register it with USmartObjectSubsystem. Note: the definition must already have a behavior definition (per-slot or default) or the subsystem silently rejects the handle (see handle_valid).
//   @param actor_label - Editor label for the new actor
//   @param definition_path - Path to the USmartObjectDefinition asset
//   @param location - Optional [x, y, z] world location; defaults to (0,0,0)
//   @returns { actor_label: string, actor_name: string, definition_path: string, has_component: bool, registered: bool, handle_valid: bool }

TSharedPtr<FJsonObject> HandleRegisterSmartObjectActor(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ActorLabel;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_label"), ActorLabel)) { return Missing; }

	TSharedPtr<FJsonObject> Err;
	USmartObjectDefinition* Def = SmartObjects_LoadDefinition(Params, Err);
	if (!Def) { return Err; }
	UWorld* World = nullptr;
	if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World, TEXT("No editor world available; cannot spawn actor"))) { return NoWorld; }
	USmartObjectSubsystem* Sub = USmartObjectSubsystem::GetCurrent(World);
	if (!Sub)
	{
		return mcp_common::MakeErrorReply(
			TEXT("USmartObjectSubsystem is not available in this world"));
	}

	FVector Location = FVector::ZeroVector;
	const TArray<TSharedPtr<FJsonValue>>* LocArr = nullptr;
	if (Params->TryGetArrayField(TEXT("location"), LocArr) && LocArr && LocArr->Num() == 3)
	{
		Location.X = (*LocArr)[0]->AsNumber();
		Location.Y = (*LocArr)[1]->AsNumber();
		Location.Z = (*LocArr)[2]->AsNumber();
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), Location, FRotator::ZeroRotator, SpawnParams);
	if (!Actor)
	{
		return mcp_common::MakeErrorReply(TEXT("Failed to spawn actor"));
	}
	Actor->SetActorLabel(ActorLabel);

	USmartObjectComponent* Comp = NewObject<USmartObjectComponent>(Actor);
	Comp->SetDefinition(Def);
	Actor->AddInstanceComponent(Comp);
	Comp->RegisterComponent();
	// RegisterSmartObjectActor iterates AActor::GetComponents which can miss
	// instance components that were just NewObject'd. The single-component
	// overload (TNotNull<USmartObjectComponent*>) is more reliable. The
	// component's OnRegister already ran RegisterToSubsystem (called from
	// RegisterComponent above for the editor world), so the handle should
	// be populated by the time we return.
	Sub->RegisterSmartObject(Comp);

	// Honest diagnostic: "registered" above
	// only means the API call was made, not that the subsystem accepted the
	// component. If the USmartObjectDefinition has no behavior definition (per-slot
	// or default), SmartObjectContainer::AddSmartObject silently rejects it on
	// Validate() without a handle — the caller must call
	// add_smart_object_slot_behavior_definition / add_smart_object_definition_default_behavior
	// first. handle_valid is the ground truth other handlers (claim/use/find) depend on.
	const bool bHandleValid = Comp->GetRegisteredHandle().IsValid();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
	Data->SetStringField(TEXT("actor_name"), Actor->GetName());
	Data->SetStringField(TEXT("definition_path"), Def->GetPathName());
	Data->SetBoolField(TEXT("has_component"), (Comp != nullptr));
	Data->SetBoolField(TEXT("registered"), true);
	Data->SetBoolField(TEXT("handle_valid"), bHandleValid);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// claim_smart_object
//   Returns the claim_handle as the canonical LexToString (and the
//   underlying sub-fields for human inspection / round-trip). The caller
//   is responsible for keeping this string for an eventual release.
// ---------------------------------------------------------------------------
// MCP:claim_smart_object - Claim a free slot on the Smart Object component of an actor in the editor world. Returns the canonical claim_handle (LexToString) for downstream release.
//   @param actor_label - Editor label of the actor carrying the USmartObjectComponent
//   @param definition_path - Optional path to the definition; only used for reporting
//   @returns { actor_label: string, definition_path: string, claim_handle: string, slot_id: string, is_claimed: bool, success: bool }

TSharedPtr<FJsonObject> HandleClaimSmartObject(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ActorLabel, DefinitionPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_label"), ActorLabel)) { return Missing; }
	Params->TryGetStringField(TEXT("definition_path"), DefinitionPath);

	UWorld* EditorWorld = nullptr;
	TSharedPtr<FJsonObject> Err;
	USmartObjectSubsystem* const Sub = SmartObjects_GetSubsystem(EditorWorld, Err);
	if (Sub == nullptr) { return Err; }

	// Carrier actor (by label) -> its registered Smart Object -> the first free slot.
	const USmartObjectComponent* const Comp =
		SmartObjects_FindComponent(EditorWorld, ActorLabel, TEXT("Actor not found in editor world"), Err);
	if (Comp == nullptr) { return Err; }
	const FSmartObjectSlotHandle FreeSlot =
		UEAgentSmartObjectsImpl::FirstFreeSlotOf(*Sub, *Comp, TEXT("All slots are already claimed"), Err);
	if (!FreeSlot.IsValid()) { return Err; }

	const FSmartObjectClaimHandle Claim =
		Sub->MarkSlotAsClaimed(FreeSlot, ESmartObjectClaimPriority::Normal, /*User*/ {});
	const FString ClaimString = LexToString(Claim);
	const bool bValid = Sub->IsClaimedSmartObjectValid(Claim);

	// Sub-fields for human inspection. FSmartObjectHandle stores a FGuid;
	// the public surface is LexToString (friend). The handle's fields
	// (SmartObjectHandle, SlotIndex) are exposed publicly in the struct
	// definition. FSmartObjectUserHandle's ID field is private, so we
	// can only return the canonical claim_handle string.
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_label"), ActorLabel);
	Data->SetStringField(TEXT("definition_path"), DefinitionPath);
	Data->SetStringField(TEXT("claim_handle"), ClaimString);
	Data->SetStringField(TEXT("slot_id"), LexToString(FreeSlot));
	Data->SetBoolField(TEXT("is_claimed"), bValid);
	Data->SetBoolField(TEXT("success"), bValid);
	// The verdict measured above is the verdict EMITTED.
	return mcp_common::MakeOutcomeReply(Data, bValid,
	    bValid ? FString() : mcp_common::MeasuredOutcomeError(TEXT("claim_smart_object"), TEXT("bValid")));
}

// ---------------------------------------------------------------------------
// release_smart_object  — HONEST GATE
//
// FSmartObjectClaimHandle contains a FSmartObjectUserHandle whose ID is
// private to USmartObjectSubsystem (friend-only ctor in 5.7). Without a
// way to reconstruct the user_id from outside, the FSmartObjectClaimHandle
// cannot be rebuilt for MarkSlotAsFree's equality check.
//
// The canonical pattern is for the caller to keep the original claim_handle
// string. Until Epic exposes a public round-trip parser (e.g. Parse from
// LexToString output), headless release is not safely possible.
// ---------------------------------------------------------------------------
// MCP:release_smart_object - Release a Smart Object claim. The only headless-safe path is to pass actor_label so the subsystem can unregister the component (which releases any claim it holds). FSmartObjectUserHandle is private in UE 5.7, so a claim_handle-only path is not possible without a C++ extension.
//   @param actor_label - Optional editor label of the actor whose component to unregister (releases all its claims)
//   @param claim_handle - Optional canonical claim string; documented limitation: round-trip is not public in 5.7
//   @returns { actor_label: string, released: bool } | (error: "release_smart_object: ...")

TSharedPtr<FJsonObject> HandleReleaseSmartObject(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ClaimHandleStr;
	Params->TryGetStringField(TEXT("claim_handle"), ClaimHandleStr);

	UWorld* World = nullptr;
	TSharedPtr<FJsonObject> Err;
	USmartObjectSubsystem* Sub = SmartObjects_GetSubsystem(World, Err);
	if (!Sub) { return Err; }

	// Even though we cannot reconstruct the exact FSmartObjectClaimHandle,
	// we can still unregister the smart-object component, which releases
	// any claim held by it. This is the only headless-safe release path:
	// the caller can pass actor_label to find the component.
	FString ActorLabel;
	Params->TryGetStringField(TEXT("actor_label"), ActorLabel);
	if (!ActorLabel.IsEmpty())
	{
		AActor* Actor = mcp_common::FindActorByLabel(World, ActorLabel);
		if (!Actor)
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("Actor not found: %s"), *ActorLabel));
		}
		USmartObjectComponent* Comp = Actor->FindComponentByClass<USmartObjectComponent>();
		if (!Comp)
		{
			return mcp_common::MakeErrorReply(
				TEXT("Actor has no USmartObjectComponent"));
		}
		Sub->UnregisterSmartObject(Comp);
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("actor_label"), ActorLabel);
		Data->SetBoolField(TEXT("released"), true);
		return mcp_common::MakeOkReply(Data);
	}

	// No actor_label provided: document the limitation.
	return mcp_common::MakeErrorReply(
		TEXT("release_smart_object: FSmartObjectUserHandle is private in UE 5.7; the canonical release path is to pass actor_label so the subsystem can unregister the component (which releases any claim it holds). Pass claim_handle-only if you can supply a C++ extension that exposes USmartObjectSubsystem::ReleaseByHandle."));
}

// ---------------------------------------------------------------------------
// get_smart_object_info
// ---------------------------------------------------------------------------
// MCP:get_smart_object_info - Get slot count, activity tag set, and the number of registered actors for a USmartObjectDefinition.
//   @param definition_path - Path to the USmartObjectDefinition asset
//   @returns { definition_path: string, slot_count: int, activity_tags: [string], registered_actor_count: int }

TSharedPtr<FJsonObject> HandleGetSmartObjectInfo(
	const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Err;
	USmartObjectDefinition* Def = SmartObjects_LoadDefinition(Params, Err);
	if (!Def) { return Err; }

	TSet<FName> TagSet;
	for (const FSmartObjectSlotDefinition& Slot : Def->GetSlots())
	{
		for (const FGameplayTag& T : Slot.ActivityTags)
		{
			TagSet.Add(T.GetTagName());
		}
	}
	TArray<FString> ActivityTags;
	ActivityTags.Reserve(TagSet.Num());
	for (const FName& N : TagSet) { ActivityTags.Add(N.ToString()); }

	int32 RegisteredActorCount = 0;
	UWorld* World = mcp_common::GetEditorWorld();
	if (World)
	{
		USmartObjectSubsystem* Sub = USmartObjectSubsystem::GetCurrent(World);
		if (Sub)
		{
			const FSmartObjectContainer& Container = Sub->GetSmartObjectContainer();
			for (const FSmartObjectCollectionEntry& Entry : Container.GetEntries())
			{
				USmartObjectComponent* C = Container.GetSmartObjectComponent(Entry.GetHandle());
				if (C && C->GetDefinition() == Def)
				{
					++RegisteredActorCount;
				}
			}
		}
	}

	TSharedPtr<FJsonObject> Data = SmartObjects_NewDefinitionData(Def);
	Data->SetNumberField(TEXT("slot_count"), Def->GetSlots().Num());
	Data->SetArrayField(TEXT("activity_tags"), mcp_common::StringsToJson(ActivityTags));
	Data->SetNumberField(TEXT("registered_actor_count"), RegisteredActorCount);
	return mcp_common::MakeOkReply(Data);
}

// ===========================================================================
// SmartObject definition authoring (slot + object level + read-back)
//   All operate on a loaded USmartObjectDefinition asset (no PIE). Slots are
//   addressed by slot_index (see get_smart_object_slots_detail). Mutations go
//   through GetMutableSlot()/the public Set* methods, then save the package.
// ===========================================================================

// MCP:set_smart_object_slot_activity_tags - Replace a slot's activity tags (which activity it supports) on a USmartObjectDefinition, addressed by slot_index.
//   @param definition_path - Path to the USmartObjectDefinition asset
//   @param slot_index - Index of the slot to edit (from get_smart_object_slots_detail)
//   @param activity_tags - Gameplay tag names to set as the slot's activity tags; tags not registered are skipped
//   @returns { definition_path: string, slot_index: int, activity_tags: [string], saved: bool }
TSharedPtr<FJsonObject> HandleSetSmartObjectSlotActivityTags(
	const TSharedPtr<FJsonObject>& Params)
{
	int32 SlotIndex = INDEX_NONE;
	TSharedPtr<FJsonObject> Err;
	USmartObjectDefinition* Def = SmartObjects_LoadDefinitionSlot(Params, SlotIndex, Err);
	if (!Def) { return Err; }
	const TArray<TSharedPtr<FJsonValue>>* TagsArr = nullptr;
	Params->TryGetArrayField(TEXT("activity_tags"), TagsArr);

	const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetSmartObjectSlotActivityTags", "Set Smart Object Slot Activity Tags (MCP)"));
	Def->Modify();
	FSmartObjectSlotDefinition& Slot = Def->GetMutableSlot(SlotIndex);
	Slot.ActivityTags = ParseTagContainer(TagsArr);
	const bool bSaved = SmartObjects_SaveDefinition(Def);

	TSharedPtr<FJsonObject> Data = SmartObjects_NewSlotData(Def, SlotIndex);
	Data->SetArrayField(TEXT("activity_tags"), mcp_common::GameplayTagsToJson(Slot.ActivityTags));
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_smart_object_slot_activity_tags"));
}

// MCP:set_smart_object_slot_user_tag_filter - Replace the user tag filter of the slot at slot_index in a USmartObjectDefinition asset (the gameplay tag query a user must pass to use the slot) with one built from user_tags and match (any, all or none of the tags), in one undo step, then save it. Names that are not registered gameplay tags are skipped silently (the reply lists the tags kept), and an unrecognized match acts as any but is echoed as sent. An empty list still builds a query (filter_is_empty false) that, with any, admits no user, so this cannot clear the filter. A missing definition or an invalid slot_index is an error; a failed save, as during PIE, makes success false. Repeat calls write the same query (idempotent). Object-level filter: set_smart_object_definition_user_tag_filter; slot activity tags: set_smart_object_slot_activity_tags; read slots back: get_smart_object_slots_detail.
//   @param definition_path - Path to the USmartObjectDefinition asset
//   @param slot_index - Zero-based index of the slot to edit (see get_smart_object_slots_detail); out of range is an error
//   @param user_tags - Gameplay tag names the filter query matches against; unregistered names are skipped, and an empty list does not clear the filter
//   @param match - Match mode: "any" (default), "all", or "none"; any other value acts as any
//   @returns { definition_path: string, slot_index: int, user_tags: [string], match: string, filter_is_empty: bool, saved: bool }
TSharedPtr<FJsonObject> HandleSetSmartObjectSlotUserTagFilter(
	const TSharedPtr<FJsonObject>& Params)
{
	int32 SlotIndex = INDEX_NONE;
	TSharedPtr<FJsonObject> Err;
	USmartObjectDefinition* Def = SmartObjects_LoadDefinitionSlot(Params, SlotIndex, Err);
	if (!Def) { return Err; }
	FString Match = TEXT("any");
	Params->TryGetStringField(TEXT("match"), Match);
	const TArray<TSharedPtr<FJsonValue>>* TagsArr = nullptr;
	Params->TryGetArrayField(TEXT("user_tags"), TagsArr);
	const FGameplayTagContainer Tags = ParseTagContainer(TagsArr);

	const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetSmartObjectSlotUserTagFilter", "Set Smart Object Slot User Tag Filter (MCP)"));
	Def->Modify();
	FSmartObjectSlotDefinition& Slot = Def->GetMutableSlot(SlotIndex);
	Slot.UserTagFilter = BuildTagQuery(Tags, Match);
	const bool bSaved = SmartObjects_SaveDefinition(Def);

	TSharedPtr<FJsonObject> Data = SmartObjects_NewSlotData(Def, SlotIndex);
	Data->SetArrayField(TEXT("user_tags"), mcp_common::GameplayTagsToJson(Tags));
	Data->SetStringField(TEXT("match"), Match);
	Data->SetBoolField(TEXT("filter_is_empty"), Slot.UserTagFilter.IsEmpty());
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_smart_object_slot_user_tag_filter"));
}

// MCP:set_smart_object_slot_enabled - Set the authored enabled flag (bEnabled) of the slot at slot_index in a USmartObjectDefinition asset, then save it: the initial state that smart objects built from this definition start with. enabled defaults to true when omitted. An invalid slot_index is an error, and a failed save returns an error with the change kept in memory. Undoable (one editor transaction). Use remove_smart_object_slot to delete the slot instead, and get_smart_object_slots_detail to read slots back.
//   @param definition_path - Path to the USmartObjectDefinition asset
//   @param slot_index - Index of the slot to edit
//   @param enabled - Whether the slot is enabled
//   @returns { definition_path: string, slot_index: int, enabled: bool, saved: bool }
TSharedPtr<FJsonObject> HandleSetSmartObjectSlotEnabled(
	const TSharedPtr<FJsonObject>& Params)
{
	int32 SlotIndex = INDEX_NONE;
	TSharedPtr<FJsonObject> Err;
	USmartObjectDefinition* Def = SmartObjects_LoadDefinitionSlot(Params, SlotIndex, Err);
	if (!Def) { return Err; }
	bool bEnabled = true;
	Params->TryGetBoolField(TEXT("enabled"), bEnabled);

	const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetSmartObjectSlotEnabled", "Set Smart Object Slot Enabled (MCP)"));
	Def->Modify();
	FSmartObjectSlotDefinition& Slot = Def->GetMutableSlot(SlotIndex);
	Slot.bEnabled = bEnabled;
	const bool bSaved = SmartObjects_SaveDefinition(Def);

	TSharedPtr<FJsonObject> Data = SmartObjects_NewSlotData(Def, SlotIndex);
	Data->SetBoolField(TEXT("enabled"), Slot.bEnabled);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_smart_object_slot_enabled"));
}

// MCP:set_smart_object_slot_transform - Set the local offset and/or rotation (degrees) of the slot at slot_index in a USmartObjectDefinition asset, relative to the smart object's owner, then save it. Each of offset and rotation is written only when given with 3 numbers; otherwise it keeps its value. An invalid slot_index is an error, and a failed save returns an error with the change kept in memory. Undoable (one editor transaction). Read slots back with get_smart_object_slots_detail; set_smart_object_slot_enabled and set_smart_object_slot_activity_tags edit other slot fields.
//   @param definition_path - Path to the USmartObjectDefinition asset
//   @param slot_index - Index of the slot to edit
//   @param offset - Optional [x, y, z] local offset
//   @param rotation - Optional [pitch, yaw, roll] local rotation (degrees)
//   @returns { definition_path: string, slot_index: int, offset: [number, number, number], rotation: [number, number, number], saved: bool }
TSharedPtr<FJsonObject> HandleSetSmartObjectSlotTransform(
	const TSharedPtr<FJsonObject>& Params)
{
	int32 SlotIndex = INDEX_NONE;
	TSharedPtr<FJsonObject> Err;
	USmartObjectDefinition* Def = SmartObjects_LoadDefinitionSlot(Params, SlotIndex, Err);
	if (!Def) { return Err; }

	const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetSmartObjectSlotTransform", "Set Smart Object Slot Transform (MCP)"));
	Def->Modify();
	FSmartObjectSlotDefinition& Slot = Def->GetMutableSlot(SlotIndex);
	SmartObjects_ReadOffsetRotation(Params, Slot.Offset, Slot.Rotation);
	const bool bSaved = SmartObjects_SaveDefinition(Def);

	TSharedPtr<FJsonObject> Data = SmartObjects_NewSlotData(Def, SlotIndex);
	Data->SetArrayField(TEXT("offset"), mcp_common::VectorToJsonArray(Slot.Offset));
	Data->SetArrayField(TEXT("rotation"), mcp_common::RotatorToJsonArray(Slot.Rotation));
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_smart_object_slot_transform"));
}

// MCP:set_smart_object_definition_activity_tags - Replace the object-level activity tags of a USmartObjectDefinition asset with activity_tags, optionally set its activity-tag merging policy and user-tag filtering policy, then save it. The whole tag container is written: an omitted or empty list empties it, and names that are not registered gameplay tags are skipped silently (compare activity_tags in the reply); an omitted policy keeps its value. A missing definition or an unknown policy name is an error, but the policies are checked after the tags are written, so that error leaves the new tags in memory, unsaved. Repeat calls write the same state (idempotent); no undo step is recorded, and a failed save, as during PIE, makes success false. Per-slot tags: set_smart_object_slot_activity_tags; object-level user filter: set_smart_object_definition_user_tag_filter; read back: get_smart_object_definition_summary.
//   @param definition_path - Path to the USmartObjectDefinition asset
//   @param activity_tags - Gameplay tag names for the definition's object-level activity tags; the list replaces the current tags whole
//   @param merging_policy - Optional activity-tag merging policy: "Combine" (default) or "Override"; Combine merges slot and object tags, Override lets non-empty slot tags take their place; omitted keeps the current policy
//   @param filtering_policy - Optional user-tag filtering policy: "NoFilter", "Combine", or "Override"; how the object and slot user filters apply to a request; omitted keeps the current policy
//   @returns { definition_path: string, activity_tags: [string], merging_policy: string, filtering_policy: string, saved: bool }
TSharedPtr<FJsonObject> HandleSetSmartObjectDefinitionActivityTags(
	const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Err;
	USmartObjectDefinition* Def = SmartObjects_LoadDefinition(Params, Err);
	if (!Def) { return Err; }
	const TArray<TSharedPtr<FJsonValue>>* TagsArr = nullptr;
	Params->TryGetArrayField(TEXT("activity_tags"), TagsArr);
	Def->SetActivityTags(ParseTagContainer(TagsArr));

	FString MergingStr;
	if (Params->TryGetStringField(TEXT("merging_policy"), MergingStr) && !MergingStr.IsEmpty())
	{
		ESmartObjectTagMergingPolicy MP;
		if (!ParseMergingPolicy(MergingStr, MP))
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("Invalid merging_policy '%s' (expected Combine|Override)"), *MergingStr));
		}
		Def->SetActivityTagsMergingPolicy(MP);
	}
	FString FilteringStr;
	if (Params->TryGetStringField(TEXT("filtering_policy"), FilteringStr) && !FilteringStr.IsEmpty())
	{
		ESmartObjectTagFilteringPolicy FP;
		if (!ParseFilteringPolicy(FilteringStr, FP))
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("Invalid filtering_policy '%s' (expected NoFilter|Combine|Override)"), *FilteringStr));
		}
		Def->SetUserTagsFilteringPolicy(FP);
	}
	const bool bSaved = SmartObjects_SaveDefinition(Def);

	TSharedPtr<FJsonObject> Data = SmartObjects_NewDefinitionData(Def);
	Data->SetArrayField(TEXT("activity_tags"), mcp_common::GameplayTagsToJson(Def->GetActivityTags()));
	Data->SetStringField(TEXT("merging_policy"), MergingPolicyToString(Def->GetActivityTagsMergingPolicy()));
	Data->SetStringField(TEXT("filtering_policy"), FilteringPolicyToString(Def->GetUserTagsFilteringPolicy()));
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_smart_object_definition_activity_tags"));
}

// MCP:set_smart_object_definition_user_tag_filter - Set the object-level user tag filter (a gameplay tag query) of a USmartObjectDefinition, built from a tag list and a match mode.
//   @param definition_path - Path to the USmartObjectDefinition asset
//   @param user_tags - Gameplay tag names the filter query matches against
//   @param match - Match mode: "any" (default), "all", or "none"
//   @returns { definition_path: string, user_tags: [string], match: string, filter_is_empty: bool, saved: bool }
TSharedPtr<FJsonObject> HandleSetSmartObjectDefinitionUserTagFilter(
	const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Err;
	USmartObjectDefinition* Def = SmartObjects_LoadDefinition(Params, Err);
	if (!Def) { return Err; }
	FString Match = TEXT("any");
	Params->TryGetStringField(TEXT("match"), Match);
	const TArray<TSharedPtr<FJsonValue>>* TagsArr = nullptr;
	Params->TryGetArrayField(TEXT("user_tags"), TagsArr);
	const FGameplayTagContainer Tags = ParseTagContainer(TagsArr);

	Def->SetUserTagFilter(BuildTagQuery(Tags, Match));
	const bool bSaved = SmartObjects_SaveDefinition(Def);

	TSharedPtr<FJsonObject> Data = SmartObjects_NewDefinitionData(Def);
	Data->SetArrayField(TEXT("user_tags"), mcp_common::GameplayTagsToJson(Tags));
	Data->SetStringField(TEXT("match"), Match);
	Data->SetBoolField(TEXT("filter_is_empty"), Def->GetUserTagFilter().IsEmpty());
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_smart_object_definition_user_tag_filter"));
}

// MCP:get_smart_object_slots_detail - Read back per-slot detail (name, enabled, offset, rotation, activity tags, behavior/data counts) of a USmartObjectDefinition.
//   @param definition_path - Path to the USmartObjectDefinition asset
//   @returns { definition_path: string, slot_count: int, slots: [{ index: int, name: string, enabled: bool, offset: [number, number, number], rotation: [number, number, number], activity_tags: [string], user_filter_is_empty: bool, behavior_count: int, behavior_classes: [string], data_count: int }] }
TSharedPtr<FJsonObject> HandleGetSmartObjectSlotsDetail(
	const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Err;
	USmartObjectDefinition* Def = SmartObjects_LoadDefinition(Params, Err);
	if (!Def) { return Err; }

	const TConstArrayView<FSmartObjectSlotDefinition> Slots = Def->GetSlots();
	TArray<TSharedPtr<FJsonValue>> SlotsJson;
	for (int32 i = 0; i < Slots.Num(); ++i)
	{
		const FSmartObjectSlotDefinition& S = Slots[i];
		TSharedPtr<FJsonObject> SO = MakeShared<FJsonObject>();
		SO->SetNumberField(TEXT("index"), i);
		SO->SetStringField(TEXT("name"), S.Name.ToString());
		SO->SetBoolField(TEXT("enabled"), S.bEnabled);
		SO->SetArrayField(TEXT("offset"), mcp_common::VectorToJsonArray(S.Offset));
		SO->SetArrayField(TEXT("rotation"), mcp_common::RotatorToJsonArray(S.Rotation));
		SO->SetArrayField(TEXT("activity_tags"), mcp_common::GameplayTagsToJson(S.ActivityTags));
		SO->SetBoolField(TEXT("user_filter_is_empty"), S.UserTagFilter.IsEmpty());
		SO->SetNumberField(TEXT("behavior_count"), S.BehaviorDefinitions.Num());
		{
			TArray<TSharedPtr<FJsonValue>> BehJson;
			for (const TObjectPtr<USmartObjectBehaviorDefinition>& B : S.BehaviorDefinitions)
			{
				if (B) { BehJson.Add(MakeShared<FJsonValueString>(B->GetClass()->GetName())); }
			}
			SO->SetArrayField(TEXT("behavior_classes"), BehJson);
		}
		SO->SetNumberField(TEXT("data_count"), S.DefinitionData.Num());
		SO->SetNumberField(TEXT("entrance_count"), SmartObjects_CountEntrances(S));
		SlotsJson.Add(MakeShared<FJsonValueObject>(SO));
	}

	TSharedPtr<FJsonObject> Data = SmartObjects_NewDefinitionData(Def);
	Data->SetNumberField(TEXT("slot_count"), Slots.Num());
	Data->SetArrayField(TEXT("slots"), SlotsJson);
	return mcp_common::MakeOkReply(Data);
}

// MCP:get_smart_object_definition_summary - Read the object-level state of a USmartObjectDefinition asset without changing it: its activity tags with their merging policy, the user-tag filtering policy and whether the object-level user filter is empty, the slot count, the default behavior count, and bounds around every slot in the object local space (slot offsets plus the default slot size, not world space). It also runs the engine Validate() pass: is_valid, and validation_errors with the messages of every severity. A missing definition_path, or one that does not load, is an error. Per-slot fields: get_smart_object_slots_detail; slot and registered-actor counts: get_smart_object_info; entrances: get_smart_object_slot_entrance_annotations; edit the object level with set_smart_object_definition_activity_tags or set_smart_object_definition_user_tag_filter.
//   @param definition_path - Object path of the USmartObjectDefinition asset, e.g. /Game/AI/SO_Bench.SO_Bench
//   @returns { definition_path: string, slot_count: int, activity_tags: [string], merging_policy: string, filtering_policy: string, user_filter_is_empty: bool, bounds: { min: [number, number, number], max: [number, number, number] }, is_valid: bool, validation_errors: [string], default_behavior_count: int }
TSharedPtr<FJsonObject> HandleGetSmartObjectDefinitionSummary(
	const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Err;
	USmartObjectDefinition* Def = SmartObjects_LoadDefinition(Params, Err);
	if (!Def) { return Err; }

	TArray<TPair<EMessageSeverity::Type, FText>> Errors;
	const bool bValid = Def->Validate(&Errors);

	const FBox Bounds = Def->GetBounds();
	TSharedPtr<FJsonObject> BoundsJson = MakeShared<FJsonObject>();
	{
		TArray<TSharedPtr<FJsonValue>> Min, Max;
		Min.Append(mcp_common::VectorToJsonArray(Bounds.Min));
		Max.Append(mcp_common::VectorToJsonArray(Bounds.Max));
		BoundsJson->SetArrayField(TEXT("min"), Min);
		BoundsJson->SetArrayField(TEXT("max"), Max);
	}

	TArray<TSharedPtr<FJsonValue>> ErrorsJson;
	for (const TPair<EMessageSeverity::Type, FText>& E : Errors)
	{
		ErrorsJson.Add(MakeShared<FJsonValueString>(E.Value.ToString()));
	}

	TSharedPtr<FJsonObject> Data = SmartObjects_NewDefinitionData(Def);
	Data->SetNumberField(TEXT("slot_count"), Def->GetSlots().Num());
	Data->SetArrayField(TEXT("activity_tags"), mcp_common::GameplayTagsToJson(Def->GetActivityTags()));
	Data->SetStringField(TEXT("merging_policy"), MergingPolicyToString(Def->GetActivityTagsMergingPolicy()));
	Data->SetStringField(TEXT("filtering_policy"), FilteringPolicyToString(Def->GetUserTagsFilteringPolicy()));
	Data->SetBoolField(TEXT("user_filter_is_empty"), Def->GetUserTagFilter().IsEmpty());
	Data->SetObjectField(TEXT("bounds"), BoundsJson);
	Data->SetBoolField(TEXT("is_valid"), bValid);
	Data->SetArrayField(TEXT("validation_errors"), ErrorsJson);
	Data->SetNumberField(TEXT("default_behavior_count"), GetDefaultBehaviorCount(Def));
	return mcp_common::MakeOkReply(Data);
}

// MCP:add_smart_object_slot_behavior_definition - Attach a behavior definition (class resolved by name) to a slot of a USmartObjectDefinition. The class must derive from USmartObjectBehaviorDefinition (e.g. GameplayBehaviorSmartObjectBehaviorDefinition). A behavior on every slot (or a default) makes the definition Validate()-valid.
//   @param definition_path - Path to the USmartObjectDefinition asset
//   @param slot_index - Index of the slot to attach the behavior to
//   @param behavior_class - Behavior class: short native name (e.g. "GameplayBehaviorSmartObjectBehaviorDefinition") or full /Script path
//   @returns { definition_path: string, slot_index: int, behavior_class: string, behavior_count: int, saved: bool }
TSharedPtr<FJsonObject> HandleAddSmartObjectSlotBehaviorDefinition(
	const TSharedPtr<FJsonObject>& Params)
{
	int32 SlotIndex = INDEX_NONE;
	TSharedPtr<FJsonObject> Err;
	USmartObjectDefinition* Def = SmartObjects_LoadDefinitionSlot(Params, SlotIndex, Err);
	if (!Def) { return Err; }
	FString BehaviorClassName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("behavior_class"), BehaviorClassName)) { return Missing; }
	USmartObjectBehaviorDefinition* Behavior = SmartObjects_NewBehavior(Def, BehaviorClassName,
		FString::Printf(TEXT("Behavior class not found: %s (is its plugin enabled / module loaded?)"), *BehaviorClassName), Err);
	if (!Behavior) { return Err; }

	FSmartObjectSlotDefinition& Slot = Def->GetMutableSlot(SlotIndex);
	Slot.BehaviorDefinitions.Add(Behavior);
	const bool bSaved = SmartObjects_SaveDefinition(Def);

	TSharedPtr<FJsonObject> Data = SmartObjects_NewSlotData(Def, SlotIndex);
	Data->SetStringField(TEXT("behavior_class"), Behavior->GetClass()->GetName());
	Data->SetNumberField(TEXT("behavior_count"), Slot.BehaviorDefinitions.Num());
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("add_smart_object_slot_behavior_definition"));
}

// ---------------------------------------------------------------------------
// Slot entrance annotations + component definition set/get
// ---------------------------------------------------------------------------

// MCP:add_smart_object_slot_entrance_annotation - Add an entrance (entry/exit point) annotation to a slot of a USmartObjectDefinition, including its navigation/trajectory fields.
//   @param definition_path - Path to the USmartObjectDefinition asset
//   @param slot_index - Index of the slot to add the entrance to
//   @param offset - Optional [x, y, z] local-space offset of the entrance
//   @param rotation - Optional [pitch, yaw, roll] local-space rotation (degrees)
//   @param is_entry - Optional: entrance can be used to enter the slot (default true)
//   @param is_exit - Optional: entrance can be used to exit the slot (default true)
//   @param tags - Optional gameplay tag names identifying the entrance
//   @param trace_ground_location - Optional: adjust ground location with a line trace (default true).
//   @param check_transition_trajectory - Optional: check collisions on the transition from nav location to slot (default false).
//   @param selection_priority - Optional entrance selection priority: Lowest|Lower|Low|BelowNormal|Normal|AboveNormal|High|Higher|Highest (default Normal).
//   @param trajectory_start_height_offset - Optional height offset at the start of the transition check (default 80).
//   @param trajectory_slot_height_offset - Optional height offset at the slot for the transition check (default 40).
//   @param transition_check_radius - Optional radius of the transition trajectory check (default 10).
//   @returns { definition_path: string, slot_index: int, entrance_count: int, data_count: int, saved: bool }
TSharedPtr<FJsonObject> HandleAddSmartObjectSlotEntranceAnnotation(
	const TSharedPtr<FJsonObject>& Params)
{
	int32 SlotIndex = INDEX_NONE;
	TSharedPtr<FJsonObject> Err;
	USmartObjectDefinition* Def = SmartObjects_LoadDefinitionSlot(Params, SlotIndex, Err);
	if (!Def) { return Err; }

	FSmartObjectSlotEntranceAnnotation Entrance;
	SmartObjects_ReadOffsetRotation(Params, Entrance.Offset, Entrance.Rotation);
	bool bEntry = true; Params->TryGetBoolField(TEXT("is_entry"), bEntry); Entrance.bIsEntry = bEntry ? 1 : 0;
	bool bExit = true; Params->TryGetBoolField(TEXT("is_exit"), bExit); Entrance.bIsExit = bExit ? 1 : 0;
	const TArray<TSharedPtr<FJsonValue>>* TagsArr = nullptr;
	if (Params->TryGetArrayField(TEXT("tags"), TagsArr))
	{
		Entrance.Tags = ParseTagContainer(TagsArr);
	}

	// Navigation / trajectory fields (data-only; settable headless).
	bool bTraceGround = false;
	if (Params->TryGetBoolField(TEXT("trace_ground_location"), bTraceGround)) { Entrance.bTraceGroundLocation = bTraceGround ? 1 : 0; }
	bool bCheckTraj = false;
	if (Params->TryGetBoolField(TEXT("check_transition_trajectory"), bCheckTraj)) { Entrance.bCheckTransitionTrajectory = bCheckTraj ? 1 : 0; }
	FString PriorityStr;
	if (Params->TryGetStringField(TEXT("selection_priority"), PriorityStr) && !PriorityStr.IsEmpty())
	{
		ESmartObjectEntrancePriority Priority;
		if (!ParseEntrancePriority(PriorityStr, Priority))
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("Invalid selection_priority '%s' (Lowest|Lower|Low|BelowNormal|Normal|AboveNormal|High|Higher|Highest)"), *PriorityStr));
		}
		Entrance.SelectionPriority = Priority;
	}
	double TrajStart = 0.0;
	if (Params->TryGetNumberField(TEXT("trajectory_start_height_offset"), TrajStart)) { Entrance.TrajectoryStartHeightOffset = static_cast<float>(TrajStart); }
	double TrajSlot = 0.0;
	if (Params->TryGetNumberField(TEXT("trajectory_slot_height_offset"), TrajSlot)) { Entrance.TrajectorySlotHeightOffset = static_cast<float>(TrajSlot); }
	double TransRadius = 0.0;
	if (Params->TryGetNumberField(TEXT("transition_check_radius"), TransRadius)) { Entrance.TransitionCheckRadius = static_cast<float>(TransRadius); }

	FSmartObjectSlotDefinition& Slot = Def->GetMutableSlot(SlotIndex);
	Slot.DefinitionData.Add(FSmartObjectDefinitionDataProxy::Make(Entrance));
	const bool bSaved = SmartObjects_SaveDefinition(Def);

	TSharedPtr<FJsonObject> Data = SmartObjects_NewSlotData(Def, SlotIndex);
	Data->SetNumberField(TEXT("entrance_count"), SmartObjects_CountEntrances(Slot));
	Data->SetNumberField(TEXT("data_count"), Slot.DefinitionData.Num());
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("add_smart_object_slot_entrance_annotation"));
}

// MCP:get_smart_object_slot_entrance_annotations - List a slot's entrance annotations with all their fields (round-trip reader for add_smart_object_slot_entrance_annotation, including the navigation/trajectory fields).
//   @param definition_path - Path to the USmartObjectDefinition asset.
//   @param slot_index - Index of the slot to read.
//   @returns { definition_path: string, slot_index: int, entrance_count: int, entrances: [ { offset: [float], rotation: [float], is_entry: bool, is_exit: bool, tags: [string], trace_ground_location: bool, check_transition_trajectory: bool, selection_priority: string, trajectory_start_height_offset: float, trajectory_slot_height_offset: float, transition_check_radius: float } ] }
TSharedPtr<FJsonObject> HandleGetSmartObjectSlotEntranceAnnotations(
	const TSharedPtr<FJsonObject>& Params)
{
	int32 SlotIndex = INDEX_NONE;
	TSharedPtr<FJsonObject> Err;
	USmartObjectDefinition* Def = SmartObjects_LoadDefinitionSlot(Params, SlotIndex, Err);
	if (!Def) { return Err; }

	const FSmartObjectSlotDefinition& Slot = Def->GetSlots()[SlotIndex];
	TArray<TSharedPtr<FJsonValue>> Entrances;
	for (const FSmartObjectDefinitionDataProxy& Proxy : Slot.DefinitionData)
	{
		const FSmartObjectSlotEntranceAnnotation* E = Proxy.Data.GetPtr<FSmartObjectSlotEntranceAnnotation>();
		if (!E) { continue; }

		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetArrayField(TEXT("offset"), mcp_common::VectorToJsonArray(E->Offset));
		O->SetArrayField(TEXT("rotation"), mcp_common::RotatorToJsonArray(E->Rotation));
		O->SetBoolField(TEXT("is_entry"), E->bIsEntry != 0);
		O->SetBoolField(TEXT("is_exit"), E->bIsExit != 0);
		O->SetArrayField(TEXT("tags"), mcp_common::GameplayTagsToJson(E->Tags));
		O->SetBoolField(TEXT("trace_ground_location"), E->bTraceGroundLocation != 0);
		O->SetBoolField(TEXT("check_transition_trajectory"), E->bCheckTransitionTrajectory != 0);
		O->SetStringField(TEXT("selection_priority"), EntrancePriorityName(E->SelectionPriority));
		O->SetNumberField(TEXT("trajectory_start_height_offset"), E->TrajectoryStartHeightOffset);
		O->SetNumberField(TEXT("trajectory_slot_height_offset"), E->TrajectorySlotHeightOffset);
		O->SetNumberField(TEXT("transition_check_radius"), E->TransitionCheckRadius);
		Entrances.Add(MakeShared<FJsonValueObject>(O));
	}

	TSharedPtr<FJsonObject> Data = SmartObjects_NewSlotData(Def, SlotIndex);
	Data->SetNumberField(TEXT("entrance_count"), Entrances.Num());
	Data->SetArrayField(TEXT("entrances"), Entrances);
	return mcp_common::MakeOkReply(Data);
}

// MCP:set_component_definition - Set (change) the USmartObjectDefinition on the USmartObjectComponent of an actor in the editor world.
//   @param actor_label - Editor label of the actor carrying the USmartObjectComponent
//   @param definition_path - Path to the USmartObjectDefinition asset to assign
//   @returns { actor_label: string, definition_path: string, definition: string, registered: bool, set: bool }
TSharedPtr<FJsonObject> HandleSetComponentDefinition(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ActorLabel;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_label"), ActorLabel)) { return Missing; }
	TSharedPtr<FJsonObject> Err;
	USmartObjectDefinition* Def = SmartObjects_LoadDefinition(Params, Err);
	if (!Def) { return Err; }
	UWorld* World = nullptr;
	if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World)) { return NoWorld; }
	USmartObjectComponent* Comp = SmartObjects_FindComponent(World, ActorLabel, TEXT("Actor not found"), Err);
	if (!Comp) { return Err; }

	Comp->SetDefinition(Def);
	const USmartObjectDefinition* Now = Comp->GetDefinition();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_label"), ActorLabel);
	Data->SetStringField(TEXT("definition_path"), Def->GetPathName());
	Data->SetStringField(TEXT("definition"), Now ? Now->GetPathName() : FString());
	Data->SetBoolField(TEXT("registered"), Comp->GetRegisteredHandle().IsValid());
	Data->SetBoolField(TEXT("set"), Now == Def);
	return mcp_common::MakeOkReply(Data);
}

// MCP:get_smart_object_component_definition - Read the USmartObjectDefinition assigned to an actor's USmartObjectComponent and whether it is registered.
//   @param actor_label - Editor label of the actor carrying the USmartObjectComponent
//   @returns { actor_label: string, definition: string, has_definition: bool, registered: bool }
TSharedPtr<FJsonObject> HandleGetSmartObjectComponentDefinition(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ActorLabel;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_label"), ActorLabel)) { return Missing; }
	UWorld* World = nullptr;
	if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World)) { return NoWorld; }
	TSharedPtr<FJsonObject> Err;
	USmartObjectComponent* Comp = SmartObjects_FindComponent(World, ActorLabel, TEXT("Actor not found"), Err);
	if (!Comp) { return Err; }
	const USmartObjectDefinition* Def = Comp->GetDefinition();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_label"), ActorLabel);
	Data->SetStringField(TEXT("definition"), Def ? Def->GetPathName() : FString());
	Data->SetBoolField(TEXT("has_definition"), Def != nullptr);
	Data->SetBoolField(TEXT("registered"), Comp->GetRegisteredHandle().IsValid());
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// Definition-level default behavior + slot removal (private members)
// ---------------------------------------------------------------------------

// MCP:add_smart_object_definition_default_behavior - Attach a definition-level default behavior (applied to slots without their own) to a USmartObjectDefinition; the private DefaultBehaviorDefinitions array is reached via FArrayProperty reflection.
//   @param definition_path - Path to the USmartObjectDefinition asset
//   @param behavior_class - Behavior class: short native name or /Script path; must derive from USmartObjectBehaviorDefinition
//   @returns { definition_path: string, behavior_class: string, default_behavior_count: int, added: bool, saved: bool }
TSharedPtr<FJsonObject> HandleAddSmartObjectDefinitionDefaultBehavior(
	const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Err;
	USmartObjectDefinition* Def = SmartObjects_LoadDefinition(Params, Err);
	if (!Def) { return Err; }
	FString BehaviorClassName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("behavior_class"), BehaviorClassName)) { return Missing; }
	USmartObjectBehaviorDefinition* Behavior = SmartObjects_NewBehavior(Def, BehaviorClassName,
		FString::Printf(TEXT("Behavior class not found: %s"), *BehaviorClassName), Err);
	if (!Behavior) { return Err; }

	// DefaultBehaviorDefinitions is a private UPROPERTY with no public mutator — reach it
	// via reflection. Its element is a TObjectPtr (trivial destructor), so reinterpreting
	// the array as its real type and adding is safe.
	FArrayProperty* P = CastField<FArrayProperty>(
		USmartObjectDefinition::StaticClass()->FindPropertyByName(TEXT("DefaultBehaviorDefinitions")));
	if (!P)
	{
		return mcp_common::MakeErrorReply(
			TEXT("DefaultBehaviorDefinitions property not found (engine layout changed)"));
	}
	TArray<TObjectPtr<USmartObjectBehaviorDefinition>>* Arr =
		P->ContainerPtrToValuePtr<TArray<TObjectPtr<USmartObjectBehaviorDefinition>>>(Def);
	if (!Arr) { return mcp_common::MakeErrorReply(TEXT("Failed to access DefaultBehaviorDefinitions")); }
	Arr->Add(Behavior);
	const bool bSaved = SmartObjects_SaveDefinition(Def);

	TSharedPtr<FJsonObject> Data = SmartObjects_NewDefinitionData(Def);
	Data->SetStringField(TEXT("behavior_class"), Behavior->GetClass()->GetName());
	Data->SetNumberField(TEXT("default_behavior_count"), Arr->Num());
	Data->SetBoolField(TEXT("added"), true);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("add_smart_object_definition_default_behavior"));
}

// MCP:remove_smart_object_slot - Remove the slot at slot_index from a USmartObjectDefinition asset, together with its behaviors, tags and annotations; later slots shift down one index, so re-read indices with get_smart_object_slots_detail. Saves the asset; a failed save returns an error with the removal kept in memory. An invalid slot_index is an error. Undoable (one editor transaction). To turn a slot off while keeping indices stable use set_smart_object_slot_enabled; add_smart_object_slot appends one.
//   @param definition_path - Path to the USmartObjectDefinition asset
//   @param slot_index - Index of the slot to remove
//   @returns { definition_path: string, slot_index: int, slot_count: int, removed: bool, saved: bool }
TSharedPtr<FJsonObject> HandleRemoveSmartObjectSlot(
	const TSharedPtr<FJsonObject>& Params)
{
	int32 SlotIndex = INDEX_NONE;
	TSharedPtr<FJsonObject> Err;
	USmartObjectDefinition* Def = SmartObjects_LoadDefinitionSlot(Params, SlotIndex, Err);
	if (!Def) { return Err; }

	// Slots is a private UPROPERTY (GetMutableSlots returns a non-resizable TArrayView).
	// Resize it via FScriptArrayHelper, which destructs the removed element through the
	// property's reflection — avoiding instantiating ~FSmartObjectSlotDefinition here.
	FArrayProperty* P = CastField<FArrayProperty>(
		USmartObjectDefinition::StaticClass()->FindPropertyByName(TEXT("Slots")));
	if (!P)
	{
		return mcp_common::MakeErrorReply(TEXT("Slots property not found (engine layout changed)"));
	}
	const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "RemoveSmartObjectSlot", "Remove Smart Object Slot (MCP)"));
	Def->Modify();
	FScriptArrayHelper Helper(P, P->ContainerPtrToValuePtr<void>(Def));
	Helper.RemoveValues(SlotIndex, 1);
	const bool bSaved = SmartObjects_SaveDefinition(Def);

	TSharedPtr<FJsonObject> Data = SmartObjects_NewSlotData(Def, SlotIndex);
	Data->SetNumberField(TEXT("slot_count"), Def->GetSlots().Num());
	Data->SetBoolField(TEXT("removed"), true);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("remove_smart_object_slot"));
}

// ---------------------------------------------------------------------------
// Slot behavior removal (idempotent) + slot runtime tags.
// ---------------------------------------------------------------------------

// MCP:remove_smart_object_slot_behavior_definition - Remove a behavior definition from a slot of a USmartObjectDefinition by its index in the slot's BehaviorDefinitions array. Idempotent: a behavior_index past the end returns already_absent without error.
//   @param definition_path - Path to the USmartObjectDefinition asset.
//   @param slot_index - Index of the slot to edit.
//   @param behavior_index - Index into the slot's BehaviorDefinitions to remove.
//   @returns { definition_path: string, slot_index: int, behavior_index: int, behavior_count: int, removed: bool, already_absent: bool, saved: bool }
TSharedPtr<FJsonObject> HandleRemoveSmartObjectSlotBehaviorDefinition(
	const TSharedPtr<FJsonObject>& Params)
{
	int32 SlotIndex = INDEX_NONE;
	TSharedPtr<FJsonObject> Err;
	USmartObjectDefinition* Def = SmartObjects_LoadDefinitionSlot(Params, SlotIndex, Err);
	if (!Def) { return Err; }
	int32 BehaviorIndex = -1;
	{ double D = -1.0; if (Params->TryGetNumberField(TEXT("behavior_index"), D)) { BehaviorIndex = static_cast<int32>(D); } }

	FSmartObjectSlotDefinition& Slot = Def->GetMutableSlot(SlotIndex);

	// Idempotent: a behavior_index past the end is reported as already_absent
	// (no error, no save) so re-running the command is a no-op.
	if (!Slot.BehaviorDefinitions.IsValidIndex(BehaviorIndex))
	{
		TSharedPtr<FJsonObject> Data = SmartObjects_NewSlotData(Def, SlotIndex);
		Data->SetNumberField(TEXT("behavior_index"), BehaviorIndex);
		Data->SetNumberField(TEXT("behavior_count"), Slot.BehaviorDefinitions.Num());
		Data->SetBoolField(TEXT("removed"), false);
		Data->SetBoolField(TEXT("already_absent"), true);
		Data->SetBoolField(TEXT("saved"), false);
		return mcp_common::MakeOkReply(Data);
	}

	const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "RemoveSmartObjectSlotBehavior", "Remove Smart Object Slot Behavior Definition (MCP)"));
	Def->Modify();
	Slot.BehaviorDefinitions.RemoveAt(BehaviorIndex);
	const bool bSaved = SmartObjects_SaveDefinition(Def);

	TSharedPtr<FJsonObject> Data = SmartObjects_NewSlotData(Def, SlotIndex);
	Data->SetNumberField(TEXT("behavior_index"), BehaviorIndex);
	Data->SetNumberField(TEXT("behavior_count"), Slot.BehaviorDefinitions.Num());
	Data->SetBoolField(TEXT("removed"), true);
	Data->SetBoolField(TEXT("already_absent"), false);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("remove_smart_object_slot_behavior_definition"));
}

// MCP:set_smart_object_slot_runtime_tags - Replace a slot's initial runtime tags (FGameplayTagContainer) on a USmartObjectDefinition, addressed by slot_index. Tags not registered in the project are skipped.
//   @param definition_path - Path to the USmartObjectDefinition asset.
//   @param slot_index - Index of the slot to edit.
//   @param runtime_tags - Gameplay tag names to set as the slot's runtime tags.
//   @returns { definition_path: string, slot_index: int, runtime_tags: [string], saved: bool }
TSharedPtr<FJsonObject> HandleSetSmartObjectSlotRuntimeTags(
	const TSharedPtr<FJsonObject>& Params)
{
	int32 SlotIndex = INDEX_NONE;
	TSharedPtr<FJsonObject> Err;
	USmartObjectDefinition* Def = SmartObjects_LoadDefinitionSlot(Params, SlotIndex, Err);
	if (!Def) { return Err; }
	const TArray<TSharedPtr<FJsonValue>>* TagsArr = nullptr;
	Params->TryGetArrayField(TEXT("runtime_tags"), TagsArr);

	const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetSmartObjectSlotRuntimeTags", "Set Smart Object Slot Runtime Tags (MCP)"));
	Def->Modify();
	FSmartObjectSlotDefinition& Slot = Def->GetMutableSlot(SlotIndex);
	Slot.RuntimeTags = ParseTagContainer(TagsArr);
	const bool bSaved = SmartObjects_SaveDefinition(Def);

	TSharedPtr<FJsonObject> Data = SmartObjects_NewSlotData(Def, SlotIndex);
	Data->SetArrayField(TEXT("runtime_tags"), mcp_common::GameplayTagsToJson(Slot.RuntimeTags));
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_smart_object_slot_runtime_tags"));
}

// ---------------------------------------------------------------------------
// Runtime discover->claim->occupy cycle.
//
// These close the SmartObject lifecycle: find_smart_objects (spatial query of
// the subsystem's space partition) + use_smart_object (claim a free slot then
// mark it occupied, driving a behavior definition).
//
// HONEST HEADLESS GATE: the SmartObject spatial index only populates once the
// subsystem's runtime is initialized (bRuntimeInitialized). That happens on the
// game-world path (OnWorldComponentsUpdated -> InitializeRuntime) which does NOT
// run for the editor world. So FindSmartObjects returns nothing here and
// GetRegisteredHandle() is invalid — exactly like the existing claim path. We
// return honest, non-fabricated results (find -> empty + note; use -> the same
// "handle is invalid" error the claim path already surfaces). Both work fully
// under PIE / BeginPlay where the runtime is initialized.
// ---------------------------------------------------------------------------

// MCP:find_smart_objects - Spatially query the SmartObject subsystem for slots within a box centered on a point (USmartObjectSubsystem::FindSmartObjects). HONEST GATE: the spatial index only populates once the subsystem runtime is initialized (BeginPlay/PIE); in the editor/headless world it is empty, so this returns success with an empty results list and a note rather than fabricating hits.
//   @param center - [x, y, z] world-space center of the search box
//   @param radius - Half-extent (uu) of the AABB search box around center
//   @param include_claimed - Optional: also return already-claimed slots (default false)
//   @param include_disabled - Optional: also return disabled slots (default false)
//   @returns { center: [number, number, number], radius: number, results: [{ object_handle: string, slot_handle: string, actor_label: string }], count: int, note?: string }
TSharedPtr<FJsonObject> HandleFindSmartObjects(
	const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* CenterArr = nullptr;
	if (!Params->TryGetArrayField(TEXT("center"), CenterArr) || !CenterArr || CenterArr->Num() != 3)
	{
		return mcp_common::MakeErrorReply(TEXT("center is required as [x, y, z]"));
	}
	const FVector Center(
		(*CenterArr)[0]->AsNumber(),
		(*CenterArr)[1]->AsNumber(),
		(*CenterArr)[2]->AsNumber());

	double Radius = 0.0;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("radius"), Radius)) { return Missing; }

	bool bIncludeClaimed = false;
	Params->TryGetBoolField(TEXT("include_claimed"), bIncludeClaimed);
	bool bIncludeDisabled = false;
	Params->TryGetBoolField(TEXT("include_disabled"), bIncludeDisabled);

	UWorld* World = nullptr;
	TSharedPtr<FJsonObject> Err;
	USmartObjectSubsystem* Sub = SmartObjects_GetSubsystem(World, Err);
	if (!Sub) { return Err; }

	// Search-friendly filter: default keeps only free/enabled slots; the two
	// flags widen the search when the caller wants claimed/disabled slots too.
	// bShouldEvaluateConditions=false -> a pure spatial lookup (no WorldConditions
	// context to set up); this is a discovery query, not a gated selection.
	FSmartObjectRequest Req;
	Req.QueryBox = FBox::BuildAABB(Center, FVector(Radius));
	Req.Filter.bShouldEvaluateConditions = false;
	Req.Filter.bShouldIncludeClaimedSlots = bIncludeClaimed;
	Req.Filter.bShouldIncludeDisabledSlots = bIncludeDisabled;

	TArray<FSmartObjectRequestResult> Out;
	// 3rd arg (UserData) has no default in the raw overload — pass an empty view.
	const bool bFound = Sub->FindSmartObjects(Req, Out, FConstStructView());

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("center"), mcp_common::VectorToJsonArray(Center));
	Data->SetNumberField(TEXT("radius"), Radius);

	// HONEST GATE: no fabrication. An empty result (index not initialized
	// headless, or genuinely nothing in range) is a truthful success + note.
	if (!bFound || Out.Num() == 0)
	{
		Data->SetArrayField(TEXT("results"), TArray<TSharedPtr<FJsonValue>>());
		Data->SetNumberField(TEXT("count"), 0);
		Data->SetStringField(TEXT("note"),
			TEXT("SmartObject runtime not initialized in editor world (bRuntimeInitialized==false); ")
			TEXT("spatial index empty headless -- requires PIE/BeginPlay"));
		return mcp_common::MakeOkReply(Data);
	}

	TArray<TSharedPtr<FJsonValue>> ResultsJson;
	ResultsJson.Reserve(Out.Num());
	for (const FSmartObjectRequestResult& R : Out)
	{
		TSharedPtr<FJsonObject> RO = MakeShared<FJsonObject>();
		RO->SetStringField(TEXT("object_handle"), LexToString(R.SmartObjectHandle));
		RO->SetStringField(TEXT("slot_handle"), LexToString(R.SlotHandle));
		FString ActorLabel;
		if (USmartObjectComponent* Comp = Sub->GetSmartObjectComponentByRequestResult(R))
		{
			if (AActor* Owner = Comp->GetOwner())
			{
				ActorLabel = Owner->GetActorLabel();
			}
		}
		RO->SetStringField(TEXT("actor_label"), ActorLabel);
		ResultsJson.Add(MakeShared<FJsonValueObject>(RO));
	}
	Data->SetArrayField(TEXT("results"), ResultsJson);
	Data->SetNumberField(TEXT("count"), ResultsJson.Num());
	return mcp_common::MakeOkReply(Data);
}

// MCP:use_smart_object - Claim a free slot on an actor's SmartObject component and immediately mark it occupied (USmartObjectSubsystem::MarkSlotAsOccupied), driving a behavior definition. Single-call claim+occupy keyed by actor_label (an FSmartObjectClaimHandle cannot be rebuilt from a string, so a two-step claim_handle path is impossible). HONEST GATE: the component handle only registers once the subsystem runtime is initialized (PIE/BeginPlay); headless it is invalid and this returns a clean error. Idempotent: an already-occupied slot returns already_occupied without re-occupying.
//   @param actor_label - Editor label of the actor carrying the USmartObjectComponent
//   @param behavior_class - Optional behavior definition class (short native name or /Script path); defaults to USmartObjectBehaviorDefinition
//   @returns { actor_label: string, slot_id: string, behavior_definition: string, is_occupied: bool, already_occupied: bool, success: bool }
TSharedPtr<FJsonObject> HandleUseSmartObject(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ActorLabel;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_label"), ActorLabel)) { return Missing; }

	// Resolve the behavior definition class (defaults to the base class, which
	// is concrete and drives MarkSlotAsOccupied without a specific behavior).
	TSubclassOf<USmartObjectBehaviorDefinition> BehaviorClass = USmartObjectBehaviorDefinition::StaticClass();
	TSharedPtr<FJsonObject> Err;
	FString BehaviorClassName;
	if (Params->TryGetStringField(TEXT("behavior_class"), BehaviorClassName) && !BehaviorClassName.IsEmpty())
	{
		UClass* Resolved = SmartObjects_ResolveBehaviorClass(BehaviorClassName,
			FString::Printf(TEXT("Behavior class not found: %s (is its plugin enabled / module loaded?)"), *BehaviorClassName), Err);
		if (!Resolved) { return Err; }
		BehaviorClass = Resolved;
	}

	UWorld* World = nullptr;
	USmartObjectSubsystem* Sub = SmartObjects_GetSubsystem(World, Err);
	if (!Sub) { return Err; }

	USmartObjectComponent* Comp = SmartObjects_FindComponent(World, ActorLabel, TEXT("Actor not found in editor world"), Err);
	if (!Comp) { return Err; }

	const FSmartObjectSlotHandle FreeSlot =
		UEAgentSmartObjectsImpl::FirstFreeSlotOf(*Sub, *Comp, TEXT("No free slot to occupy (all claimed/occupied)"), Err);
	if (!FreeSlot.IsValid()) { return Err; }

	// Idempotency: if the slot we picked is already occupied, report it without
	// re-occupying. (A fresh free slot cannot already be occupied, but this
	// guards a re-run against a slot that transitioned Occupied out-of-band.)
	if (Sub->GetSlotState(FreeSlot) == ESmartObjectSlotState::Occupied)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("actor_label"), ActorLabel);
		Data->SetStringField(TEXT("slot_id"), LexToString(FreeSlot));
		Data->SetStringField(TEXT("behavior_definition"), FString());
		Data->SetBoolField(TEXT("is_occupied"), true);
		Data->SetBoolField(TEXT("already_occupied"), true);
		Data->SetBoolField(TEXT("success"), true);
		return mcp_common::MakeOkReply(Data);
	}

	const FSmartObjectClaimHandle Claim =
		Sub->MarkSlotAsClaimed(FreeSlot, ESmartObjectClaimPriority::Normal, /*UserData*/ {});

	// Claim+occupy in one call. MarkSlotAsOccupied returns the behavior
	// definition instance it activated (nullptr on failure).
	const USmartObjectBehaviorDefinition* Def = Sub->MarkSlotAsOccupied(Claim, BehaviorClass);
	const bool bOccupied = (Def != nullptr);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_label"), ActorLabel);
	Data->SetStringField(TEXT("slot_id"), LexToString(FreeSlot));
	Data->SetStringField(TEXT("behavior_definition"), Def ? Def->GetPathName() : FString());
	Data->SetBoolField(TEXT("is_occupied"), bOccupied);
	Data->SetBoolField(TEXT("already_occupied"), false);
	Data->SetBoolField(TEXT("success"), bOccupied);
	// The verdict measured above is the verdict EMITTED.
	return mcp_common::MakeOutcomeReply(Data, bOccupied,
	    bOccupied ? FString() : mcp_common::MeasuredOutcomeError(TEXT("use_smart_object"), TEXT("bOccupied")));
}

// ---------------------------------------------------------------------------
// Removal twins — mirror the add_* handlers, inverted.
//   remove_smart_object_definition_default_behavior <-> add_smart_object_definition_default_behavior
//   remove_smart_object_slot_entrance_annotation    <-> add_smart_object_slot_entrance_annotation
// ---------------------------------------------------------------------------

// MCP:remove_smart_object_definition_default_behavior - Remove a definition-level default behavior (by index) from a USmartObjectDefinition; the private DefaultBehaviorDefinitions array is reached via FArrayProperty reflection and resized through FScriptArrayHelper. Idempotent: a behavior_index past the end returns already_absent without error.
//   @param definition_path - Path to the USmartObjectDefinition asset
//   @param behavior_index - Index into DefaultBehaviorDefinitions to remove
//   @returns { definition_path: string, behavior_index: int, default_behavior_count: int, removed: bool, already_absent: bool, saved: bool }
TSharedPtr<FJsonObject> HandleRemoveSmartObjectDefinitionDefaultBehavior(
	const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Err;
	USmartObjectDefinition* Def = SmartObjects_LoadDefinition(Params, Err);
	if (!Def) { return Err; }
	int32 BehaviorIndex = -1;
	{ double D = -1.0; if (Params->TryGetNumberField(TEXT("behavior_index"), D)) { BehaviorIndex = static_cast<int32>(D); } }

	// DefaultBehaviorDefinitions is a private UPROPERTY with no public mutator — reach it
	// via reflection (mirror of HandleAddSmartObjectDefinitionDefaultBehavior). Resize via
	// FScriptArrayHelper so the removed TObjectPtr element is destructed through the
	// property's reflection (mirror of HandleRemoveSmartObjectSlot).
	FArrayProperty* P = CastField<FArrayProperty>(
		USmartObjectDefinition::StaticClass()->FindPropertyByName(TEXT("DefaultBehaviorDefinitions")));
	if (!P)
	{
		return mcp_common::MakeErrorReply(
			TEXT("DefaultBehaviorDefinitions property not found (engine layout changed)"));
	}
	FScriptArrayHelper Helper(P, P->ContainerPtrToValuePtr<void>(Def));

	// Idempotent: a behavior_index past the end is reported as already_absent
	// (no error, no save) so re-running the command is a no-op.
	if (!Helper.IsValidIndex(BehaviorIndex))
	{
		TSharedPtr<FJsonObject> Data = SmartObjects_NewDefinitionData(Def);
		Data->SetNumberField(TEXT("behavior_index"), BehaviorIndex);
		Data->SetNumberField(TEXT("default_behavior_count"), Helper.Num());
		Data->SetBoolField(TEXT("removed"), false);
		Data->SetBoolField(TEXT("already_absent"), true);
		Data->SetBoolField(TEXT("saved"), false);
		return mcp_common::MakeOkReply(Data);
	}

	Helper.RemoveValues(BehaviorIndex, 1);
	const bool bSaved = SmartObjects_SaveDefinition(Def);

	TSharedPtr<FJsonObject> Data = SmartObjects_NewDefinitionData(Def);
	Data->SetNumberField(TEXT("behavior_index"), BehaviorIndex);
	Data->SetNumberField(TEXT("default_behavior_count"), Helper.Num());
	Data->SetBoolField(TEXT("removed"), true);
	Data->SetBoolField(TEXT("already_absent"), false);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("remove_smart_object_definition_default_behavior"));
}

// MCP:remove_smart_object_slot_entrance_annotation - Remove an entrance annotation (by its index among the slot's entrance annotations) from a slot of a USmartObjectDefinition. entrance_index counts only FSmartObjectSlotEntranceAnnotation entries in the slot's DefinitionData (mirror of add_smart_object_slot_entrance_annotation, inverted). Idempotent: an entrance_index past the number of entrances returns already_absent without error.
//   @param definition_path - Path to the USmartObjectDefinition asset
//   @param slot_index - Index of the slot to edit
//   @param entrance_index - Index among the slot's entrance annotations to remove (0-based; entrances only, not all definition data)
//   @returns { definition_path: string, slot_index: int, entrance_index: int, entrance_count: int, data_count: int, removed: bool, already_absent: bool, saved: bool }
TSharedPtr<FJsonObject> HandleRemoveSmartObjectSlotEntranceAnnotation(
	const TSharedPtr<FJsonObject>& Params)
{
	int32 SlotIndex = INDEX_NONE;
	TSharedPtr<FJsonObject> Err;
	USmartObjectDefinition* Def = SmartObjects_LoadDefinitionSlot(Params, SlotIndex, Err);
	if (!Def) { return Err; }
	int32 EntranceIndex = -1;
	{ double D = -1.0; if (Params->TryGetNumberField(TEXT("entrance_index"), D)) { EntranceIndex = static_cast<int32>(D); } }

	FSmartObjectSlotDefinition& Slot = Def->GetMutableSlot(SlotIndex);

	// Map entrance_index (position among entrance annotations only) to the real
	// DefinitionData array index, with the entrance filter the adder / reader twins use.
	int32 TargetDataIndex = INDEX_NONE;
	int32 EntranceCursor = 0;
	for (int32 i = 0; i < Slot.DefinitionData.Num(); ++i)
	{
		if (SmartObjects_IsEntrance(Slot.DefinitionData[i]))
		{
			if (EntranceCursor == EntranceIndex) { TargetDataIndex = i; break; }
			++EntranceCursor;
		}
	}

	// Idempotent: an entrance_index that maps to no entrance (out of range) is
	// reported as already_absent (no error, no save) so a re-run is a no-op.
	if (TargetDataIndex == INDEX_NONE)
	{
		TSharedPtr<FJsonObject> Data = SmartObjects_NewSlotData(Def, SlotIndex);
		Data->SetNumberField(TEXT("entrance_index"), EntranceIndex);
		Data->SetNumberField(TEXT("entrance_count"), SmartObjects_CountEntrances(Slot));
		Data->SetNumberField(TEXT("data_count"), Slot.DefinitionData.Num());
		Data->SetBoolField(TEXT("removed"), false);
		Data->SetBoolField(TEXT("already_absent"), true);
		Data->SetBoolField(TEXT("saved"), false);
		return mcp_common::MakeOkReply(Data);
	}

	// DefinitionData is a public member of the (private-Slots) slot struct, reached
	// through GetMutableSlot — the adder mutates it the same way (Slot.DefinitionData.Add).
	const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "RemoveSmartObjectSlotEntrance", "Remove Smart Object Slot Entrance Annotation (MCP)"));
	Def->Modify();
	Slot.DefinitionData.RemoveAt(TargetDataIndex);
	const bool bSaved = SmartObjects_SaveDefinition(Def);

	TSharedPtr<FJsonObject> Data = SmartObjects_NewSlotData(Def, SlotIndex);
	Data->SetNumberField(TEXT("entrance_index"), EntranceIndex);
	Data->SetNumberField(TEXT("entrance_count"), SmartObjects_CountEntrances(Slot));
	Data->SetNumberField(TEXT("data_count"), Slot.DefinitionData.Num());
	Data->SetBoolField(TEXT("removed"), true);
	Data->SetBoolField(TEXT("already_absent"), false);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("remove_smart_object_slot_entrance_annotation"));
}

const McpCommandFamilies::FRegistrar Registration =
{
	{ TEXT("add_smart_object_definition_default_behavior"), &HandleAddSmartObjectDefinitionDefaultBehavior },
	{ TEXT("add_smart_object_slot"), &HandleAddSmartObjectSlot },
	{ TEXT("add_smart_object_slot_behavior_definition"), &HandleAddSmartObjectSlotBehaviorDefinition },
	{ TEXT("add_smart_object_slot_entrance_annotation"), &HandleAddSmartObjectSlotEntranceAnnotation },
	{ TEXT("claim_smart_object"), &HandleClaimSmartObject },
	{ TEXT("create_smart_object_definition"), &HandleCreateSmartObjectDefinition },
	{ TEXT("find_smart_objects"), &HandleFindSmartObjects },
	{ TEXT("get_smart_object_component_definition"), &HandleGetSmartObjectComponentDefinition },
	{ TEXT("get_smart_object_definition_summary"), &HandleGetSmartObjectDefinitionSummary },
	{ TEXT("get_smart_object_info"), &HandleGetSmartObjectInfo },
	{ TEXT("get_smart_object_slot_entrance_annotations"), &HandleGetSmartObjectSlotEntranceAnnotations },
	{ TEXT("get_smart_object_slots_detail"), &HandleGetSmartObjectSlotsDetail },
	{ TEXT("register_smart_object_actor"), &HandleRegisterSmartObjectActor },
	{ TEXT("release_smart_object"), &HandleReleaseSmartObject },
	{ TEXT("remove_smart_object_definition_default_behavior"), &HandleRemoveSmartObjectDefinitionDefaultBehavior },
	{ TEXT("remove_smart_object_slot"), &HandleRemoveSmartObjectSlot },
	{ TEXT("remove_smart_object_slot_behavior_definition"), &HandleRemoveSmartObjectSlotBehaviorDefinition },
	{ TEXT("remove_smart_object_slot_entrance_annotation"), &HandleRemoveSmartObjectSlotEntranceAnnotation },
	{ TEXT("set_component_definition"), &HandleSetComponentDefinition },
	{ TEXT("set_smart_object_definition_activity_tags"), &HandleSetSmartObjectDefinitionActivityTags },
	{ TEXT("set_smart_object_definition_user_tag_filter"), &HandleSetSmartObjectDefinitionUserTagFilter },
	{ TEXT("set_smart_object_slot_activity_tags"), &HandleSetSmartObjectSlotActivityTags },
	{ TEXT("set_smart_object_slot_enabled"), &HandleSetSmartObjectSlotEnabled },
	{ TEXT("set_smart_object_slot_runtime_tags"), &HandleSetSmartObjectSlotRuntimeTags },
	{ TEXT("set_smart_object_slot_transform"), &HandleSetSmartObjectSlotTransform },
	{ TEXT("set_smart_object_slot_user_tag_filter"), &HandleSetSmartObjectSlotUserTagFilter },
	{ TEXT("use_smart_object"), &HandleUseSmartObject },
};
} // namespace UEAgentSmartObjectsCommands
