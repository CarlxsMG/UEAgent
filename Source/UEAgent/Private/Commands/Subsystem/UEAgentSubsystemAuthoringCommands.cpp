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
}

// ---------------------------------------------------------------------------
// activate_external_subsystem
// ---------------------------------------------------------------------------
// MCP:activate_external_subsystem - Externally activate a Blueprint-authored USubsystem via FSubsystemCollectionBase::ActivateExternalSubsystem. Refuses to toggle native / abstract / base classes.
//   @param class_name - Short class name or /Script/ path of a Blueprint-derived USubsystem
//   @returns { class_name: string, activated: bool }

TSharedPtr<FJsonObject> HandleActivateExternalSubsystem(
	const TSharedPtr<FJsonObject>& Params)
{
	UClass* Cls = nullptr;
	if (TSharedPtr<FJsonObject> Refusal = SubsystemAuthoring_ReadToggleableClass(Params, TEXT("activate"), Cls))
	{
		return Refusal;
	}
	FSubsystemCollectionBase::ActivateExternalSubsystem(Cls);
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("class_name"), Cls->GetName());
	Data->SetBoolField(TEXT("activated"), true);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// deactivate_external_subsystem
// ---------------------------------------------------------------------------
// MCP:deactivate_external_subsystem - Externally deactivate a Blueprint-authored USubsystem via FSubsystemCollectionBase::DeactivateExternalSubsystem. Refuses to toggle native / abstract / base classes.
//   @param class_name - Short class name or /Script/ path of a Blueprint-derived USubsystem
//   @returns { class_name: string, deactivated: bool }

TSharedPtr<FJsonObject> HandleDeactivateExternalSubsystem(
	const TSharedPtr<FJsonObject>& Params)
{
	UClass* Cls = nullptr;
	if (TSharedPtr<FJsonObject> Refusal = SubsystemAuthoring_ReadToggleableClass(Params, TEXT("deactivate"), Cls))
	{
		return Refusal;
	}
	FSubsystemCollectionBase::DeactivateExternalSubsystem(Cls);
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("class_name"), Cls->GetName());
	Data->SetBoolField(TEXT("deactivated"), true);
	return mcp_common::MakeOkReply(Data);
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
