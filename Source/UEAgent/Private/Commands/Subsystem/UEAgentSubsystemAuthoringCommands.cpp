// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Common/UEAgentGraphUtils.h"
#include "Commands/Asset/AssetCommandHelpers.h"
#include "Commands/Subsystem/SubsystemLifetime.h"

#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Subsystems/EngineSubsystem.h"
#include "EditorSubsystem.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Subsystems/Subsystem.h"
#include "Subsystems/SubsystemCollection.h"
#include "Subsystems/WorldSubsystem.h"
#include "Subsystems/LocalPlayerSubsystem.h"
#include "EditorAssetLibrary.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Class.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Factories/BlueprintFactory.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

/**
 * Subsystem authoring commands.
 *
 * Complements UEAgentSubsystemCommands (introspection only) with the live-instance surface: list the
 * live subsystem instances (or ask whether one class has any), and activate / deactivate external
 * subsystems (FSubsystemCollectionBase::ActivateExternalSubsystem / DeactivateExternalSubsystem).
 * Subsystem classes are listed by list_derived_classes, described by get_class_hierarchy and created by
 * create_blueprint.
 *
 * Lifetime categories (UEAgentSubsystemLifetime) are detected by walking the parent chain (no reflection
 * of the Within specifier needed):
 *  - Engine       (UEngineSubsystem)
 *  - Editor       (UEditorSubsystem)
 *  - GameInstance (UGameInstanceSubsystem)
 *  - World        (UWorldSubsystem)
 *  - LocalPlayer  (ULocalPlayerSubsystem)
 */
namespace UEAgentSubsystemAuthoringCommands
{

// MCP-DOMAIN: SubsystemAuthoring

namespace
{
	UClass* ResolveSubsystemClass(const FString& ClassName)
	{
		return FUEAgentGraphUtils::ResolveClass(ClassName);
	}

	/**
	 * Engine-provided subsystem BASE classes. Externally activating/deactivating
	 * one of these forces every collection of that family to instantiate/tear
	 * down a default — including engine-critical singletons like the
	 * WorldPartitionSubsystem, which trips an engine invariant and crashes the
	 * editor. Only concrete leaf subsystem classes are safe to toggle.
	 */
	bool IsUnsafeExternalSubsystemClass(const UClass* Cls)
	{
		if (!Cls) { return true; }
		if (Cls->HasAnyClassFlags(CLASS_Abstract)) { return true; }
		static const TCHAR* kBases[] = {
			TEXT("Subsystem"), TEXT("DynamicSubsystem"), TEXT("EngineSubsystem"),
			TEXT("GameInstanceSubsystem"), TEXT("LocalPlayerSubsystem"),
			TEXT("WorldSubsystem"), TEXT("TickableWorldSubsystem"),
			TEXT("EditorSubsystem"),
		};
		const FString Name = Cls->GetName();
		for (const TCHAR* Base : kBases)
		{
			if (Name == Base) { return true; }
		}
		// Native engine subsystems are auto-managed by their collection; only
		// allow user (Blueprint-generated) leaf classes to be toggled externally.
		return Cls->IsNative();
	}

	/**
	 * class_name of activate/deactivate_external_subsystem: a USubsystem class that is safe to toggle externally
	 * (Verb names the operation in the refusal). Returns the error reply to send back, or nullptr once OutClass
	 * holds the class.
	 */
	TSharedPtr<FJsonObject> SubsystemAuthoring_ReadToggleableClass(const TSharedPtr<FJsonObject>& Params, const TCHAR* Verb, UClass*& OutClass)
	{
		FString ClassName;
		if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("class_name"), ClassName)) { return Missing; }
		OutClass = ResolveSubsystemClass(ClassName);
		if (!OutClass || !OutClass->IsChildOf(USubsystem::StaticClass()))
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("Subsystem class not found: %s"), *ClassName));
		}
		if (IsUnsafeExternalSubsystemClass(OutClass))
		{
			return mcp_common::MakeErrorReply(FString::Printf(
				TEXT("Refusing to externally %s '%s': only concrete, non-abstract, "
					"non-native (Blueprint-authored) subsystem leaf classes are safe to "
					"toggle. Engine subsystem base/native classes are auto-managed and "
					"tearing them down crashes the editor."), Verb, *OutClass->GetName()));
		}
		return nullptr;
	}

	/**
	 * Whether Sub is still held by the subsystem collection it was created in: the collection of its outer (the
	 * engine, the editor, a game instance, a world or a local player) returns it for its class. A deinitialized
	 * instance is out of its collection but lives on until garbage collection, so being a live object proves nothing.
	 * OutMeasurable is false for a subsystem of another family, which this cannot tell.
	 */
	bool SubsystemAuthoring_IsHeldByCollection(USubsystem* Sub, bool& OutMeasurable)
	{
		OutMeasurable = true;
		UClass* Cls = Sub->GetClass();
		UObject* Outer = Sub->GetOuter();
		if (Cls->IsChildOf(UWorldSubsystem::StaticClass()))
		{
			const UWorld* World = Cast<UWorld>(Outer);
			return World && World->GetSubsystemBase(Cls) == Sub;
		}
		if (Cls->IsChildOf(UGameInstanceSubsystem::StaticClass()))
		{
			const UGameInstance* GameInstance = Cast<UGameInstance>(Outer);
			return GameInstance && GameInstance->GetSubsystemBase(Cls) == Sub;
		}
		if (Cls->IsChildOf(ULocalPlayerSubsystem::StaticClass()))
		{
			const ULocalPlayer* LocalPlayer = Cast<ULocalPlayer>(Outer);
			return LocalPlayer && LocalPlayer->GetSubsystemBase(Cls) == Sub;
		}
		if (Cls->IsChildOf(UEditorSubsystem::StaticClass()))
		{
			return GEditor && GEditor->GetEditorSubsystemBase(Cls) == Sub;
		}
		if (Cls->IsChildOf(UEngineSubsystem::StaticClass()))
		{
			return GEngine && GEngine->GetEngineSubsystemBase(Cls) == Sub;
		}
		OutMeasurable = false;
		return false;
	}

	/** How many instances of Cls (or a subclass) are held by a subsystem collection; OutMeasurable as above. */
	int32 SubsystemAuthoring_CountHeldInstances(const UClass* Cls, bool& OutMeasurable)
	{
		OutMeasurable = true;
		int32 Held = 0;
		for (TObjectIterator<USubsystem> It; It; ++It)
		{
			USubsystem* Sub = *It;
			if (!IsValid(Sub) || Sub->HasAnyFlags(RF_ClassDefaultObject) || !Sub->GetClass()->IsChildOf(Cls))
			{
				continue;
			}
			bool bMeasurable = true;
			Held += SubsystemAuthoring_IsHeldByCollection(Sub, bMeasurable) ? 1 : 0;
			OutMeasurable &= bMeasurable;
		}
		return Held;
	}
}

// ---------------------------------------------------------------------------
// activate_external_subsystem
// ---------------------------------------------------------------------------
// MCP:activate_external_subsystem - Externally activate a Blueprint-authored USubsystem via FSubsystemCollectionBase::ActivateExternalSubsystem: every live collection of its family (engine, editor, game instance, world or local player) creates and initializes an instance, unless the class's ShouldCreateSubsystem declines. Refuses to toggle native / abstract / base classes. The engine call reports nothing, so the instances the collections hold are counted before and after it: activated is true when at least one is held afterwards, already_active when one was held before (a repeat call adds nothing), and success is false when none is held. Inverse: deactivate_external_subsystem.
//   @param class_name - Short class name or /Script/ path of a Blueprint-derived USubsystem
//   @returns { class_name: string, lifetime: string, instances_before: int, instances_after: int, activated: bool, already_active: bool } -- for a subsystem of another family than those five, activated is null with a note and no counts.

TSharedPtr<FJsonObject> HandleActivateExternalSubsystem(
	const TSharedPtr<FJsonObject>& Params)
{
	UClass* Cls = nullptr;
	if (TSharedPtr<FJsonObject> Refusal = SubsystemAuthoring_ReadToggleableClass(Params, TEXT("activate"), Cls))
	{
		return Refusal;
	}
	// The engine call reports nothing: the instances its collections hold are counted before and after it.
	bool bMeasurableBefore = true;
	bool bMeasurableAfter = true;
	const int32 HeldBefore = SubsystemAuthoring_CountHeldInstances(Cls, bMeasurableBefore);
	FSubsystemCollectionBase::ActivateExternalSubsystem(Cls);
	const int32 HeldAfter = SubsystemAuthoring_CountHeldInstances(Cls, bMeasurableAfter);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("class_name"), Cls->GetName());
	Data->SetStringField(TEXT("lifetime"), UEAgentSubsystemLifetime::DetectLifetimeCategory(Cls));
	if (!bMeasurableBefore || !bMeasurableAfter)
	{
		// A subsystem family whose collections cannot be asked: the call ran, its effect is not known.
		Data->SetField(TEXT("activated"), MakeShared<FJsonValueNull>());
		Data->SetStringField(TEXT("note"), TEXT("This subsystem's family (not Engine, Editor, GameInstance, World or LocalPlayer) cannot be read back, so whether it was activated is unknown."));
		return mcp_common::MakeOkReply(Data);
	}
	Data->SetNumberField(TEXT("instances_before"), HeldBefore);
	Data->SetNumberField(TEXT("instances_after"), HeldAfter);
	Data->SetBoolField(TEXT("activated"), HeldAfter > 0);
	Data->SetBoolField(TEXT("already_active"), HeldBefore > 0);
	return mcp_common::MakeOutcomeReply(Data, HeldAfter > 0, HeldAfter > 0 ? FString()
		: FString::Printf(TEXT("activate_external_subsystem: no collection holds an instance of %s after the call (its ShouldCreateSubsystem may decline every outer)."), *Cls->GetName()));
}

// ---------------------------------------------------------------------------
// deactivate_external_subsystem
// ---------------------------------------------------------------------------
// MCP:deactivate_external_subsystem - Tear down a Blueprint-authored subsystem (a non-native, non-abstract USubsystem class) in every live subsystem collection of its family (engine, editor, game instance, world or local player), through the engine's external-subsystem switch: its Deinitialize runs and its runtime state is lost. Runtime only: nothing is saved and there is no undo, and a world or game instance created later builds it again. The inverse is activate_external_subsystem; check what is live with list_subsystem_instances (with class_name it answers active true or false), and find candidate classes with list_derived_classes. Native engine subsystems, the subsystem base classes and abstract classes are refused, since tearing them down can crash the editor; a class that is not found or is not a subsystem is an error. Idempotent: a repeat call, or one for a subsystem that is not active, changes nothing and answers deactivated false with a note. The engine call reports nothing, so the instances the collections hold are counted before and after it: deactivated is true when some were held and none are left, and success is false if any is still held.
//   @param class_name - Blueprint subsystem class, by its Blueprint asset path (e.g. "/Game/Systems/BP_ScoreSubsystem"), generated class path, or loaded generated class name (e.g. "BP_ScoreSubsystem_C"); a native /Script/ class resolves but is refused.
//   @returns { class_name: string, lifetime: string, instances_before: int, instances_after: int, deactivated: bool, note? } -- for a subsystem of another family than those five, deactivated is null with a note and no counts.

TSharedPtr<FJsonObject> HandleDeactivateExternalSubsystem(
	const TSharedPtr<FJsonObject>& Params)
{
	UClass* Cls = nullptr;
	if (TSharedPtr<FJsonObject> Refusal = SubsystemAuthoring_ReadToggleableClass(Params, TEXT("deactivate"), Cls))
	{
		return Refusal;
	}
	// The engine call reports nothing: the instances its collections hold are counted before and after it.
	bool bMeasurableBefore = true;
	bool bMeasurableAfter = true;
	const int32 HeldBefore = SubsystemAuthoring_CountHeldInstances(Cls, bMeasurableBefore);
	FSubsystemCollectionBase::DeactivateExternalSubsystem(Cls);
	const int32 HeldAfter = SubsystemAuthoring_CountHeldInstances(Cls, bMeasurableAfter);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("class_name"), Cls->GetName());
	Data->SetStringField(TEXT("lifetime"), UEAgentSubsystemLifetime::DetectLifetimeCategory(Cls));
	if (!bMeasurableBefore || !bMeasurableAfter)
	{
		// A subsystem family whose collections cannot be asked: the call ran, its effect is not known.
		Data->SetField(TEXT("deactivated"), MakeShared<FJsonValueNull>());
		Data->SetStringField(TEXT("note"), TEXT("This subsystem's family (not Engine, Editor, GameInstance, World or LocalPlayer) cannot be read back, so whether it was torn down is unknown."));
		return mcp_common::MakeOkReply(Data);
	}
	Data->SetNumberField(TEXT("instances_before"), HeldBefore);
	Data->SetNumberField(TEXT("instances_after"), HeldAfter);
	Data->SetBoolField(TEXT("deactivated"), HeldBefore > 0 && HeldAfter == 0);
	if (HeldBefore == 0)
	{
		Data->SetStringField(TEXT("note"), TEXT("No collection held an instance of this class, so there was nothing to tear down (idempotent no-op)."));
		return mcp_common::MakeOkReply(Data);
	}
	return mcp_common::MakeOutcomeReply(Data, HeldAfter == 0, HeldAfter == 0 ? FString()
		: FString::Printf(TEXT("deactivate_external_subsystem: %d instance(s) of %s are still held by their collections after the call."), HeldAfter, *Cls->GetName()));
}

// ---------------------------------------------------------------------------
// list_subsystem_instances
// ---------------------------------------------------------------------------
// MCP:list_subsystem_instances - Enumerate the USubsystem instances live right now, with each one's class path and lifetime (class default objects are skipped); with class_name it answers the yes/no question for ONE class instead -- active is whether that class (or a subclass) has any live instance, across every lifetime. A LocalPlayer subsystem only exists while a PIE session has a local player, so a headless editor lists Engine / Editor / GameInstance / World ones; list_local_player_subsystems reads a PIE player's.
//   @param lifetime - Optional filter: "Engine"|"Editor"|"GameInstance"|"World"|"LocalPlayer" (case-insensitive)
//   @param class_name - Optional USubsystem class (short name or /Script/ path): keep only the instances of that class or a subclass. A name that does not resolve to a USubsystem class is an error.
//   @returns { total: int, instance_count: int, active: bool, lifetime_filter?, class_name?, lifetime?, instances:[{ class_name, class_path, lifetime, instance_name, instance_path }] } -- total and instance_count are the same number and active is total > 0; class_name and lifetime (the class's own lifetime category) appear when class_name was given.

TSharedPtr<FJsonObject> HandleListSubsystemInstances(
	const TSharedPtr<FJsonObject>& Params)
{
	FString LifetimeFilter;
	Params->TryGetStringField(TEXT("lifetime"), LifetimeFilter);
	FString ClassName;
	const bool bClassFilter = Params->TryGetStringField(TEXT("class_name"), ClassName) && !ClassName.IsEmpty();
	UClass* FilterClass = nullptr;
	if (bClassFilter)
	{
		FilterClass = FUEAgentGraphUtils::ResolveClass(ClassName);
		if (!FilterClass || !FilterClass->IsChildOf(USubsystem::StaticClass()))
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("Subsystem class not found: %s"), *ClassName));
		}
	}

	TArray<TSharedPtr<FJsonValue>> Entries;

	// Walk every live USubsystem instance. We use TObjectIterator<USubsystem> instead
	// of the per-collection `GetSubsystem<T>()` accessors because UWorld hides its
	// subsystem collection entirely, and the class-based variants only return the
	// typed base. Iterating live instances is O(N_subsystems) and avoids per-class
	// discovery cost.
	for (TObjectIterator<USubsystem> It; It; ++It)
	{
		USubsystem* Sub = *It;
		if (!Sub || !Sub->IsValidLowLevel()) { continue; }
		UClass* SubCls = Sub->GetClass();
		if (!SubCls) { continue; }
		if (Sub->HasAnyFlags(RF_ClassDefaultObject)) { continue; }
		if (FilterClass && !SubCls->IsChildOf(FilterClass)) { continue; }

		const FString Lifetime = UEAgentSubsystemLifetime::DetectLifetimeCategory(SubCls);
		if (!LifetimeFilter.IsEmpty() && !Lifetime.Equals(LifetimeFilter, ESearchCase::IgnoreCase)) { continue; }

		TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetStringField(TEXT("class_name"), SubCls->GetName());
		E->SetStringField(TEXT("class_path"), SubCls->GetPathName());
		E->SetStringField(TEXT("lifetime"), Lifetime);
		E->SetStringField(TEXT("instance_name"), Sub->GetName());
		E->SetStringField(TEXT("instance_path"), Sub->GetPathName());
		Entries.Add(MakeShared<FJsonValueObject>(E));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("total"), Entries.Num());
	Data->SetNumberField(TEXT("instance_count"), Entries.Num());
	Data->SetBoolField(TEXT("active"), Entries.Num() > 0);
	if (!LifetimeFilter.IsEmpty())
	{
		Data->SetStringField(TEXT("lifetime_filter"), LifetimeFilter);
	}
	if (FilterClass)
	{
		Data->SetStringField(TEXT("class_name"), FilterClass->GetName());
		Data->SetStringField(TEXT("lifetime"), UEAgentSubsystemLifetime::DetectLifetimeCategory(FilterClass));
	}
	Data->SetArrayField(TEXT("instances"), Entries);
	return mcp_common::MakeOkReply(Data);
}

const McpCommandFamilies::FRegistrar Registration =
{
	{ TEXT("activate_external_subsystem"), &HandleActivateExternalSubsystem },
	{ TEXT("deactivate_external_subsystem"), &HandleDeactivateExternalSubsystem },
	{ TEXT("list_subsystem_instances"), &HandleListSubsystemInstances },
};
} // namespace UEAgentSubsystemAuthoringCommands
