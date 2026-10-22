// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Misc/EngineVersionComparison.h"
#include "Commands/Common/UEAgentGraphUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "MassEntityConfigAsset.h"
#include "MassEntityTraitBase.h"
#include "MassSpawner.h"
#include "MassSpawnerTypes.h"
#include "MassEntitySubsystem.h"   // UMassEntitySubsystem + FMassEntityManager::DebugGetEntityCount (via Mass_LiveEntityCount)
#include "MassEntityManager.h"
#include "MassEntityQuery.h"       // FMassEntityQuery
#include "MassEntityCollection.h"  // FMassArchetypeEntityCollection
#include "MassRequirements.h"      // EMassFragmentPresence / EMassFragmentAccess
#include "MassSignalSubsystem.h"   // UMassSignalSubsystem (world subsystem, signal dispatch)
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

/**
 * Mass Entity commands.
 *
 * Wraps the engine's MassGameplay plugin (UMassEntityConfigAsset, AMassSpawner, UMassEntitySubsystem) so an
 * LLM can author Mass entity configs (assets with a list of traits), spawn them via AMassSpawner actors, and
 * read back, edit and signal the resulting entities.
 */
namespace UEAgentMassCommands
{

// MCP-DOMAIN: Mass

namespace
{
	/** Resolve a UClass by reflected name (strips a U/A/F/E prefix) or /Script path. */
	UClass* ResolveClassByName(const FString& Name)
	{
		return FUEAgentGraphUtils::ResolveClass(Name);
	}

	/**
	 * The live entities of an entity manager. FMassEntityManager::DebugGetEntityCount is the storage size minus the
	 * reserved and free slots, so on a manager that never created an entity (empty storage, one reserved slot) it
	 * answers -1; that is 0 entities, and a -1 baseline would also add one to every spawn/despawn delta.
	 */
	int32 Mass_LiveEntityCount(const FMassEntityManager& Mgr)
	{
		return FMath::Max(0, Mgr.DebugGetEntityCount());
	}

	/** Mass_LiveEntityCount of the subsystem's manager; 0 without a subsystem. */
	int32 Mass_LiveEntityCount(const UMassEntitySubsystem* MassSub)
	{
		return MassSub ? Mass_LiveEntityCount(MassSub->GetEntityManager()) : 0;
	}

	/** Resolve a dotted property path on an object; returns the leaf FProperty + its value ptr. */
	bool ResolvePropertyPath(UObject* Obj, const FString& Path, FProperty*& OutProp, void*& OutValuePtr)
	{
		if (!Obj || Path.IsEmpty()) { return false; }
		TArray<FString> Parts;
		Path.ParseIntoArray(Parts, TEXT("."), /*CullEmpty*/ true);
		if (Parts.Num() == 0) { return false; }
		UStruct* Owner = Obj->GetClass();
		void* Container = Obj;
		for (int32 i = 0; i < Parts.Num(); ++i)
		{
			FProperty* P = Owner ? Owner->FindPropertyByName(FName(*Parts[i])) : nullptr;
			if (!P) { return false; }
			if (i == Parts.Num() - 1)
			{
				OutProp = P;
				OutValuePtr = P->ContainerPtrToValuePtr<void>(Container);
				return true;
			}
			FStructProperty* SP = CastField<FStructProperty>(P);
			if (!SP) { return false; }
			Container = SP->ContainerPtrToValuePtr<void>(Container);
			Owner = SP->Struct;
		}
		return false;
	}

	/**
	 * The editor world's AMassSpawner labelled ActorLabel (OutWorld, when given, receives that world); on failure
	 * returns nullptr and fills OutError.
	 */
	AMassSpawner* Mass_FindSpawner(const FString& ActorLabel, TSharedPtr<FJsonObject>& OutError, UWorld** OutWorld = nullptr)
	{
		UWorld* World = nullptr;
		if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World))
		{
			OutError = NoWorld;
			return nullptr;
		}
		if (OutWorld)
		{
			*OutWorld = World;
		}
		for (TActorIterator<AMassSpawner> It(World); It; ++It)
		{
			if (It->GetActorLabel() == ActorLabel)
			{
				return *It;
			}
		}
		OutError = mcp_common::MakeErrorReply(FString::Printf(TEXT("AMassSpawner actor not found: %s"), *ActorLabel));
		return nullptr;
	}

	/** Mass_FindSpawner for the required actor_label param, read into OutActorLabel. */
	AMassSpawner* Mass_FindSpawnerParam(const TSharedPtr<FJsonObject>& Params, FString& OutActorLabel,
		TSharedPtr<FJsonObject>& OutError, UWorld** OutWorld = nullptr)
	{
		if (!Params->TryGetStringField(TEXT("actor_label"), OutActorLabel) || OutActorLabel.IsEmpty())
		{
			OutError = mcp_common::MakeErrorReply(TEXT("actor_label is required"));
			return nullptr;
		}
		return Mass_FindSpawner(OutActorLabel, OutError, OutWorld);
	}

	/** A reply payload that starts with Spawner's actor_label and actor_name. */
	TSharedPtr<FJsonObject> Mass_NewSpawnerData(const AMassSpawner* Spawner)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("actor_label"), Spawner->GetActorLabel());
		Data->SetStringField(TEXT("actor_name"), Spawner->GetName());
		return Data;
	}

	/** The spawner's EntityTypes (a protected property, read by reflection) as [{ entity_config, proportion }]. */
	TArray<TSharedPtr<FJsonValue>> Mass_SpawnerEntityTypesJson(AMassSpawner* Spawner)
	{
		TArray<TSharedPtr<FJsonValue>> TypesJson;
		if (FArrayProperty* ArrProp = CastField<FArrayProperty>(
				AMassSpawner::StaticClass()->FindPropertyByName(TEXT("EntityTypes"))))
		{
			if (const TArray<FMassSpawnedEntityType>* Types =
					ArrProp->ContainerPtrToValuePtr<TArray<FMassSpawnedEntityType>>(Spawner))
			{
				for (const FMassSpawnedEntityType& T : *Types)
				{
					TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("entity_config"), T.EntityConfig.ToString());
					Entry->SetNumberField(TEXT("proportion"), T.Proportion);
					TypesJson.Add(MakeShared<FJsonValueObject>(Entry));
				}
			}
		}
		return TypesJson;
	}

	/** The UMassEntityConfigAsset at ConfigPath; on failure returns nullptr and fills OutError. */
	UMassEntityConfigAsset* Mass_LoadConfig(const FString& ConfigPath, TSharedPtr<FJsonObject>& OutError)
	{
		UMassEntityConfigAsset* Config = LoadObject<UMassEntityConfigAsset>(nullptr, *ConfigPath);
		if (!Config)
		{
			OutError = mcp_common::MakeErrorReply(FString::Printf(TEXT("Cannot load Mass config: %s"), *ConfigPath));
		}
		return Config;
	}

	/** The UMassEntityTraitBase subclass named TraitClassName; on failure returns nullptr and fills OutError. */
	UClass* Mass_ResolveTraitClass(const FString& TraitClassName, TSharedPtr<FJsonObject>& OutError)
	{
		// Resolve by reflected name (global search; bare FindObject misses loaded classes
		// that need a package context, which reads as "Cannot find trait class").
		UClass* TraitClass = ResolveClassByName(TraitClassName);
		if (!TraitClass)
		{
			OutError = mcp_common::MakeErrorReply(FString::Printf(TEXT("Cannot find trait class: %s"), *TraitClassName));
			return nullptr;
		}
		if (!TraitClass->IsChildOf(UMassEntityTraitBase::StaticClass()))
		{
			OutError = mcp_common::MakeErrorReply(
				FString::Printf(TEXT("Class %s is not a UMassEntityTraitBase"), *TraitClassName));
			return nullptr;
		}
		return TraitClass;
	}

	/** A property of a trait on a Mass entity config, with the value it holds there. */
	struct FMassTraitPropertyTarget
	{
		UMassEntityConfigAsset* Config = nullptr;
		UClass* TraitClass = nullptr;
		UMassEntityTraitBase* Trait = nullptr;
		FProperty* Prop = nullptr;
		void* ValuePtr = nullptr;
	};

	/**
	 * Resolve PropName (a name or dotted path) on the TraitClassName trait of the config at ConfigPath into Out;
	 * returns the error reply when it cannot (TraitMissingHint ends the message for a trait the config lacks).
	 */
	TSharedPtr<FJsonObject> Mass_ResolveTraitProperty(const FString& ConfigPath, const FString& TraitClassName,
		const FString& PropName, const TCHAR* TraitMissingHint, FMassTraitPropertyTarget& Out)
	{
		TSharedPtr<FJsonObject> Error;
		Out.Config = Mass_LoadConfig(ConfigPath, Error);
		if (!Out.Config)
		{
			return Error;
		}
		Out.TraitClass = ResolveClassByName(TraitClassName);
		if (!Out.TraitClass || !Out.TraitClass->IsChildOf(UMassEntityTraitBase::StaticClass()))
		{
			return mcp_common::MakeErrorReply(FString::Printf(TEXT("Not a Mass trait class: %s"), *TraitClassName));
		}
		Out.Trait = Out.Config->GetMutableConfig().FindMutableTrait(Out.TraitClass);
		if (!Out.Trait)
		{
			return mcp_common::MakeErrorReply(FString::Printf(TEXT("Trait %s is not on config %s%s"),
				*Out.TraitClass->GetName(), *ConfigPath, TraitMissingHint));
		}
		if (!ResolvePropertyPath(Out.Trait, PropName, Out.Prop, Out.ValuePtr) || !Out.Prop || !Out.ValuePtr)
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("Property not found on %s: %s"), *Out.TraitClass->GetName(), *PropName));
		}
		return nullptr;
	}
}

// ---------------------------------------------------------------------------
// create_mass_entity_config
// ---------------------------------------------------------------------------
// MCP:create_mass_entity_config - Create a UMassEntityConfigAsset (an empty list of Mass traits) at package_path/config_name and save it; an existing config there is loaded and re-saved instead (created:false). Returns config_path, trait_count and saved. Add traits with add_mass_trait, then link it with set_mass_spawner_entity_types.
//   @param config_name - Name of the UMassEntityConfig asset
//   @param package_path - Target content package path where the asset is created (default /Game/MCP)
//   @returns { config_path, config_name, trait_count, created, saved }
TSharedPtr<FJsonObject> HandleCreateMassEntityConfig(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ConfigName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("config_name"), ConfigName)) { return Missing; }
	FString PackagePath = mcp_common::DefaultAssetFolder();
	Params->TryGetStringField(TEXT("package_path"), PackagePath);

	const FString FullPath = PackagePath / ConfigName;
	FString CreateError;
	bool bCreated = false;
	UMassEntityConfigAsset* Config = mcp_common::CreateOrLoadAsset<UMassEntityConfigAsset>(FullPath, CreateError, &bCreated);
	if (!Config)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Failed to create or load Mass entity config: %s"), *CreateError));
	}
	const bool bSaved = mcp_common::SaveAssetPackage(Config->GetPackage(), Config);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("config_path"), Config->GetPathName());
	Data->SetStringField(TEXT("config_name"), Config->GetName());
	Data->SetNumberField(TEXT("trait_count"), Config->GetConfig().GetTraits().Num());
	Data->SetBoolField(TEXT("created"), bCreated);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("create_mass_entity_config"));
}

// ---------------------------------------------------------------------------
// add_mass_trait
// ---------------------------------------------------------------------------
// MCP:add_mass_trait - Adds a trait (a concrete UMassEntityTraitBase subclass) to a UMassEntityConfigAsset and saves the config. Idempotent: if the config already holds a trait of exactly that class, itself or through its parent config chain, nothing is added or saved and the reply has added=false, already_existed=true and that trait's trait_path. trait_class takes a reflected name with or without its U prefix, or a /Script path; an unknown or non-trait class is an error, but an abstract one is not refused: the engine creates it behind an ensure and warns it is nulled out on save. Edit it with set_properties on its object_path from list_mass_traits; remove_mass_trait is the inverse; add_mass_tag_to_entity tags a live entity instead.
//   @param config_path - Asset path to the UMassEntityConfig
//   @param trait_class - Trait class name to add (e.g. UMassMovementTrait)
//   @returns { config_path, trait_class, trait_path, trait_count, added, already_existed, saved }
TSharedPtr<FJsonObject> HandleAddMassTrait(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ConfigPath, TraitClassName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("config_path"), ConfigPath)) { return Missing; }
	if (!Params->TryGetStringField(TEXT("trait_class"), TraitClassName) || TraitClassName.IsEmpty())
	{
		return mcp_common::MakeErrorReply(TEXT("trait_class is required (e.g. UMassMovementTrait)"));
	}

	TSharedPtr<FJsonObject> Error;
	UMassEntityConfigAsset* Config = Mass_LoadConfig(ConfigPath, Error);
	if (!Config)
	{
		return Error;
	}
	UClass* TraitClass = Mass_ResolveTraitClass(TraitClassName, Error);
	if (!TraitClass)
	{
		return Error;
	}

	// UMassEntityConfigAsset::AddTrait returns the trait of exactly that class when the config already holds one,
	// without adding anything, so the existing trait is looked up first (same exact-class match) and reported as
	// a no-op: nothing is added and nothing is saved.
	if (const UMassEntityTraitBase* Existing = Config->GetConfig().FindTrait(TraitClass, /*bExactMatch=*/true))
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("config_path"), Config->GetPathName());
		Data->SetStringField(TEXT("trait_class"), TraitClass->GetName());
		Data->SetStringField(TEXT("trait_path"), Existing->GetPathName());
		Data->SetNumberField(TEXT("trait_count"), Config->GetConfig().GetTraits().Num());
		Data->SetBoolField(TEXT("added"), false);
		Data->SetBoolField(TEXT("already_existed"), true);
		Data->SetBoolField(TEXT("saved"), false);
		return mcp_common::MakeOkReply(Data);
	}

	const int32 TraitCountBefore = Config->GetConfig().GetTraits().Num();
	UMassEntityTraitBase* NewTrait = Config->AddTrait(TraitClass);
	if (!NewTrait)
	{
		return mcp_common::MakeErrorReply(
			TEXT("AddTrait returned null (trait is abstract or factory failed)"));
	}
	const int32 TraitCount = Config->GetConfig().GetTraits().Num();
	Config->MarkPackageDirty();
	const bool bSaved = mcp_common::SaveAssetPackage(Config->GetPackage(), Config);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("config_path"), Config->GetPathName());
	Data->SetStringField(TEXT("trait_class"), TraitClass->GetName());
	Data->SetStringField(TEXT("trait_path"), NewTrait->GetPathName());
	Data->SetNumberField(TEXT("trait_count"), TraitCount);
	Data->SetBoolField(TEXT("added"), TraitCount > TraitCountBefore);
	Data->SetBoolField(TEXT("already_existed"), false);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("add_mass_trait"));
}

// ---------------------------------------------------------------------------
// remove_mass_trait
// ---------------------------------------------------------------------------
// MCP:remove_mass_trait - Remove one trait from the trait list of a Mass entity config asset (UMassEntityConfigAsset), picked by index or by class, then save the asset; there is no undo step. Inverse of add_mass_trait. It only reaches traits the config holds itself, as list_mass_traits shows them: a trait inherited from the parent config (set_mass_config_parent) stays. Not idempotent: once the trait is gone, a repeat call is an error (no matching class, or index out of range), and so are both or neither selector, a config that does not load and an unknown or non-trait class; nothing changes then. Later traits shift down one index. A failed save (for example while PIE runs) makes success false with the removal only in memory. To untag one live entity instead use remove_mass_tag_from_entity.
//   @param config_path - Asset path of the Mass entity config, e.g. "/Game/Mass/DA_CrowdAgent", as create_mass_entity_config returns it (config_path).
//   @param trait_index - 0-based position in the config's own trait list, as list_mass_traits reports it (traits[] order); give this or trait_class, not both.
//   @param trait_class - Trait class name, with or without its U prefix, or a /Script path, e.g. "MassMovementTrait"; removes the first own trait whose class is that class or a subclass of it. Give this or trait_index, not both.
//   @returns { config_path, removed, removed_trait_class, removed_index, trait_count, saved }
TSharedPtr<FJsonObject> HandleRemoveMassTrait(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ConfigPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("config_path"), ConfigPath)) { return Missing; }

	// Exactly one selector: trait_index (int) XOR trait_class (string).
	const bool bHasIndex = Params->HasTypedField<EJson::Number>(TEXT("trait_index"));
	FString TraitClassName;
	const bool bHasClass = Params->TryGetStringField(TEXT("trait_class"), TraitClassName) && !TraitClassName.IsEmpty();
	if (bHasIndex == bHasClass)
	{
		return mcp_common::MakeErrorReply(
			TEXT("Provide exactly one of trait_index (int) or trait_class (string)"));
	}

	TSharedPtr<FJsonObject> Error;
	UMassEntityConfigAsset* Config = Mass_LoadConfig(ConfigPath, Error);
	if (!Config)
	{
		return Error;
	}

	FMassEntityConfig& EntityConfig = Config->GetMutableConfig();
	TConstArrayView<UMassEntityTraitBase*> Traits = EntityConfig.GetTraits();

	// Resolve the index to remove.
	int32 RemoveIndex = INDEX_NONE;
	if (bHasIndex)
	{
		RemoveIndex = static_cast<int32>(Params->GetNumberField(TEXT("trait_index")));
		if (!Traits.IsValidIndex(RemoveIndex))
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("trait_index %d out of range (config has %d traits)"),
					RemoveIndex, Traits.Num()));
		}
	}
	else
	{
		UClass* TraitClass = Mass_ResolveTraitClass(TraitClassName, Error);
		if (!TraitClass)
		{
			return Error;
		}
		for (int32 i = 0; i < Traits.Num(); ++i)
		{
			if (Traits[i] && Traits[i]->GetClass()->IsChildOf(TraitClass))
			{
				RemoveIndex = i;
				break;
			}
		}
		if (RemoveIndex == INDEX_NONE)
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("No trait matching class %s found on config"), *TraitClassName));
		}
	}

	// Capture the removed trait's class before mutating the array.
	const UMassEntityTraitBase* RemovedTrait = Traits[RemoveIndex];
	const FString RemovedClass = RemovedTrait ? RemovedTrait->GetClass()->GetName() : FString(TEXT("None"));

	// No public RemoveTrait — remove via reflection on the Instanced Traits[] array.
	FArrayProperty* TraitsProp = CastField<FArrayProperty>(
		FMassEntityConfig::StaticStruct()->FindPropertyByName(TEXT("Traits")));
	if (!TraitsProp)
	{
		return mcp_common::MakeErrorReply(
			TEXT("Cannot resolve FMassEntityConfig::Traits property via reflection"));
	}
	void* TraitsContainer = TraitsProp->ContainerPtrToValuePtr<void>(&EntityConfig);
	FScriptArrayHelper Helper(TraitsProp, TraitsContainer);
	Helper.RemoveValues(RemoveIndex, 1);

	Config->MarkPackageDirty();
	const bool bSaved = mcp_common::SaveAssetPackage(Config->GetPackage(), Config);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("config_path"), Config->GetPathName());
	Data->SetBoolField(TEXT("removed"), true);
	Data->SetStringField(TEXT("removed_trait_class"), RemovedClass);
	Data->SetNumberField(TEXT("removed_index"), RemoveIndex);
	Data->SetNumberField(TEXT("trait_count"), EntityConfig.GetTraits().Num());
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("remove_mass_trait"));
}

// ---------------------------------------------------------------------------
// list_mass_traits
// ---------------------------------------------------------------------------
// MCP:list_mass_traits - List the traits a UMassEntityConfigAsset holds itself (not those inherited from its parent config), read-only, in config order. Returns config_path, trait_count and traits [{class, name, object_path}]; add and remove them with add_mass_trait and remove_mass_trait, and read or write a trait's settings with get_properties and set_properties on its object_path.
//   @param config_path - Asset path to the UMassEntityConfig
//   @returns { class, name, config_path, trait_count, traits }
TSharedPtr<FJsonObject> HandleListMassTraits(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ConfigPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("config_path"), ConfigPath)) { return Missing; }
	TSharedPtr<FJsonObject> Error;
	UMassEntityConfigAsset* Config = Mass_LoadConfig(ConfigPath, Error);
	if (!Config)
	{
		return Error;
	}

	TArray<TSharedPtr<FJsonValue>> TraitsJson;
	for (const UMassEntityTraitBase* Trait : Config->GetConfig().GetTraits())
	{
		if (!Trait) { continue; }
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("class"), Trait->GetClass()->GetName());
		Entry->SetStringField(TEXT("name"), Trait->GetName());
		// The path set_properties and get_properties take for the trait's settings.
		Entry->SetStringField(TEXT("object_path"), Trait->GetPathName());
		TraitsJson.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("config_path"), Config->GetPathName());
	Data->SetNumberField(TEXT("trait_count"), TraitsJson.Num());
	Data->SetArrayField(TEXT("traits"), TraitsJson);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// create_mass_spawner_actor  — HONEST GATE
//   AMassSpawner uses a template-based spawning model. Assigning an
//   EntityConfig to a spawner requires either editor UI ("Regenerate
//   Entity Config" CallInEditor button) or internal-template
//   manipulation. The public API surface is DoSpawning() / DoDespawning()
//   with no setter for the entity config. The actor is created but the
//   config-link is not done.
// ---------------------------------------------------------------------------
// MCP:create_mass_spawner_actor - Spawn an AMassSpawner actor into the editor world at location (zero rotation) and label it; the level is not saved. config_path is only echoed, not linked (config_linked:false): link configs with set_mass_spawner_entity_types. Use a unique label, since the spawner tools take the first match.
//   @retired spawn_actor - type="MassSpawner", name, location
//   @param actor_label - Editor label for the new spawner actor
//   @param config_path - Optional entity config asset path (echoed; not linked)
//   @param location - Optional spawn location [X, Y, Z] in world units (cm), default the origin; ignored unless it holds exactly 3 numbers
//   @returns { actor_label, actor_name, config_path, config_linked, note }
TSharedPtr<FJsonObject> HandleCreateMassSpawnerActor(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ActorLabel, ConfigPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_label"), ActorLabel)) { return Missing; }
	Params->TryGetStringField(TEXT("config_path"), ConfigPath);

	UWorld* World = nullptr;
	if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World, TEXT("No editor world available; cannot spawn actor"))) { return NoWorld; }

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
	AMassSpawner* Spawner = World->SpawnActor<AMassSpawner>(
		AMassSpawner::StaticClass(), Location, FRotator::ZeroRotator, SpawnParams);
	if (!Spawner)
	{
		return mcp_common::MakeErrorReply(
			TEXT("Failed to spawn AMassSpawner actor"));
	}
	Spawner->SetActorLabel(ActorLabel);

	TSharedPtr<FJsonObject> Data = Mass_NewSpawnerData(Spawner);
	Data->SetStringField(TEXT("config_path"), ConfigPath);
	Data->SetBoolField(TEXT("config_linked"), false);
	if (!ConfigPath.IsEmpty())
	{
		Data->SetStringField(TEXT("note"),
			TEXT("Spawner created; call set_mass_spawner_entity_types to link this config to the spawner."));
	}
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// spawn_mass_entities  — calls DoSpawning() on the spawner (safe call path + count)
//   DoSpawning() is invoked DIRECTLY from the bridge's FTSTicker drain (top-level
//   FEngineLoop tick, OUTSIDE Mass processing) — NOT via AsyncTask(GameThread),
//   so it does not trip the Mass re-entrancy assert. The spawned count is
//   the global Mass entity-manager delta around the call. Actual spawning needs a
//   spawn-data-generator for locations and may complete asynchronously, so 'spawned'
//   can be 0 even when entity types are configured.
// ---------------------------------------------------------------------------
// MCP:spawn_mass_entities - Run DoSpawning on an editor-world AMassSpawner found by actor_label and report the global Mass entity-count delta -- it spawns entities, not an actor (spawn_actor places the spawner). All configured types spawn at the spawner's own count (set_mass_spawner_count); a count that differs from it is an error and nothing is spawned. spawned can be 0. Inverse: despawn_mass_entities.
//   @param actor_label - Editor label of the AMassSpawner actor
//   @param count - Optional; must equal the spawner's own count (set_mass_spawner_count), which is what DoSpawning spawns. Any other value is an error and nothing is spawned.
//   @returns { actor_label, entity_count_before, entity_count_after, spawned, spawner_count, note }
TSharedPtr<FJsonObject> HandleSpawnMassEntities(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ActorLabel;
	TSharedPtr<FJsonObject> Error;
	UWorld* World = nullptr;
	AMassSpawner* Spawner = Mass_FindSpawnerParam(Params, ActorLabel, Error, &World);
	if (!Spawner)
	{
		return Error;
	}
	// AMassSpawner::DoSpawning() spawns ALL configured entity types at the spawner's own Count; there is no "spawn N"
	// overload on the public API. A `count` other than that one would be ignored, so it is refused instead.
	int32 Count = 0;
	if (Params->TryGetNumberField(TEXT("count"), Count) && Count != Spawner->GetCount())
	{
		return mcp_common::MakeErrorReply(FString::Printf(
			TEXT("count %d is not the spawner's own count (%d), and DoSpawning always spawns the spawner's count. ")
			TEXT("Set it with set_mass_spawner_count first, or leave count out; nothing was spawned."), Count, Spawner->GetCount()));
	}

	// Count via the global Mass entity manager before/after. DoSpawning() runs here on
	// the bridge's FTSTicker (top-level tick, outside Mass) — outside Mass processing, which is the safe path.
	UMassEntitySubsystem* MassSub = World->GetSubsystem<UMassEntitySubsystem>();
	const int32 CountBefore = Mass_LiveEntityCount(MassSub);
	Spawner->DoSpawning();
	const int32 CountAfter = Mass_LiveEntityCount(MassSub);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_label"), ActorLabel);
	Data->SetNumberField(TEXT("entity_count_before"), CountBefore);
	Data->SetNumberField(TEXT("entity_count_after"), CountAfter);
	Data->SetNumberField(TEXT("spawned"), CountAfter - CountBefore);
	Data->SetNumberField(TEXT("spawner_count"), Spawner->GetCount());
	Data->SetStringField(TEXT("note"),
		TEXT("DoSpawning() runs from the bridge FTSTicker (top-level tick, outside Mass processing, not AsyncTask). ")
		TEXT("entity_count is the global Mass entity-manager delta; actual spawning needs a spawn-data-generator for ")
		TEXT("locations and may be asynchronous, so 'spawned' can be 0 here."));
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// despawn_mass_entities  — calls DoDespawning() on the spawner (inverse of
//   spawn_mass_entities; same safe call path + count). Mirrors HandleSpawnMassEntities.
//   DoDespawning() is invoked DIRECTLY from the bridge's FTSTicker drain (top-level
//   FEngineLoop tick, OUTSIDE Mass processing) — NOT via AsyncTask(GameThread) — so it
//   does not trip the checkf(!IsProcessing()) guard inside DestroyEntities.
//   DoDespawning() early-returns when this spawner spawned nothing (AllSpawnedEntities
//   empty) — an honest no-op -> despawned:0, was_empty:true. The despawned count is the
//   GLOBAL Mass entity-manager delta around the call (per-spawner AllSpawnedEntities is
//   protected and not publicly readable), same convention as spawn.
// ---------------------------------------------------------------------------
// MCP:despawn_mass_entities - Trigger entity despawning on a AMassSpawner (DoDespawning from the FTSTicker path, outside Mass processing); inverse of spawn_mass_entities. Reports the global Mass entity-count delta; no-op (was_empty) when this spawner spawned nothing.
//   @param actor_label - Editor label of the AMassSpawner actor
//   @returns { actor_label, entity_count_before, entity_count_after, despawned, was_empty, note }
TSharedPtr<FJsonObject> HandleDespawnMassEntities(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ActorLabel;
	TSharedPtr<FJsonObject> Error;
	UWorld* World = nullptr;
	AMassSpawner* Spawner = Mass_FindSpawnerParam(Params, ActorLabel, Error, &World);
	if (!Spawner)
	{
		return Error;
	}

	// Count via the global Mass entity manager before/after. DoDespawning() runs here on
	// the bridge's FTSTicker (top-level tick, outside Mass) — outside Mass processing, the safe path. Mirror
	// of spawn: it early-returns when this spawner spawned nothing (honest no-op).
	UMassEntitySubsystem* MassSub = World->GetSubsystem<UMassEntitySubsystem>();
	const int32 CountBefore = Mass_LiveEntityCount(MassSub);
	Spawner->DoDespawning();
	const int32 CountAfter = Mass_LiveEntityCount(MassSub);

	const int32 Despawned = CountBefore - CountAfter;
	const bool bWasEmpty = (Despawned == 0);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_label"), ActorLabel);
	Data->SetNumberField(TEXT("entity_count_before"), CountBefore);
	Data->SetNumberField(TEXT("entity_count_after"), CountAfter);
	Data->SetNumberField(TEXT("despawned"), Despawned);
	Data->SetBoolField(TEXT("was_empty"), bWasEmpty);
	Data->SetStringField(TEXT("note"),
		TEXT("DoDespawning() runs from the bridge FTSTicker (top-level tick, outside Mass processing, not AsyncTask). ")
		TEXT("entity_count is the GLOBAL Mass entity-manager delta, not per-spawner (AllSpawnedEntities is protected). ")
		TEXT("DoDespawning early-returns when this spawner spawned nothing, so despawned:0 / was_empty:true is the correct idempotent no-op."));
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_mass_spawner_info
// ---------------------------------------------------------------------------
// MCP:get_mass_spawner_info - Read an AMassSpawner actor of the editor world, found by actor label (first match), read-only. Returns actor_name, count (the spawn count set_mass_spawner_count writes) and entity_types [{entity_config, proportion}] (what set_mass_spawner_entity_types writes) with entity_type_count; the entities it spawned are counted by list_mass_archetypes instead.
//   @param actor_label - Editor label of the AMassSpawner actor
//   @returns { actor_label, actor_name, count, entity_type_count, entity_types: [{ entity_config, proportion }] }
TSharedPtr<FJsonObject> HandleGetMassSpawnerInfo(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ActorLabel;
	TSharedPtr<FJsonObject> Error;
	AMassSpawner* Spawner = Mass_FindSpawnerParam(Params, ActorLabel, Error);
	if (!Spawner)
	{
		return Error;
	}

	TSharedPtr<FJsonObject> Data = Mass_NewSpawnerData(Spawner);
	Data->SetNumberField(TEXT("count"), Spawner->GetCount());
	const TArray<TSharedPtr<FJsonValue>> TypesJson = Mass_SpawnerEntityTypesJson(Spawner);
	Data->SetNumberField(TEXT("entity_type_count"), TypesJson.Num());
	Data->SetArrayField(TEXT("entity_types"), TypesJson);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// Config parent + trait property reflection + spawner entity types/count
// ---------------------------------------------------------------------------

// MCP:set_mass_config_parent - Set the parent config of a Mass entity config asset (UMassEntityConfigAsset), so the child inherits every trait of the parent chain on top of its own, replacing any parent it had, then save the child; there is no undo step. Use it to share a base trait set between configs instead of repeating add_mass_trait on each. Read it back with get_mass_config_parent; the child's own traits stay as list_mass_traits shows them. A repeat call with the same parent sets and saves again (set stays true). There is no way to clear the parent here. A config or parent that does not load, or a config given as its own parent, is an error and nothing changes; longer cycles (A under B under A) are not checked. A failed save (for example while PIE runs) makes success false with the change only in memory.
//   @param config_path - Asset path of the child config to change, e.g. "/Game/Mass/DA_Soldier", as create_mass_entity_config returns it (config_path).
//   @param parent_config_path - Asset path of the config to inherit from, e.g. "/Game/Mass/DA_BaseAgent"; must be a different UMassEntityConfigAsset.
//   @returns { config_path, parent_config_path, parent, set, saved }
TSharedPtr<FJsonObject> HandleSetMassConfigParent(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ConfigPath, ParentPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("config_path"), ConfigPath)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("parent_config_path"), ParentPath)) { return Missing; }
	TSharedPtr<FJsonObject> Error;
	UMassEntityConfigAsset* Config = Mass_LoadConfig(ConfigPath, Error);
	if (!Config)
	{
		return Error;
	}
	UMassEntityConfigAsset* Parent = LoadObject<UMassEntityConfigAsset>(nullptr, *ParentPath);
	if (!Parent)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Cannot load parent Mass config: %s"), *ParentPath));
	}
	if (Parent == Config)
	{
		return mcp_common::MakeErrorReply(TEXT("A config cannot be its own parent"));
	}

	Config->GetMutableConfig().SetParentAsset(*Parent);
	Config->MarkPackageDirty();
	const bool bSaved = mcp_common::SaveAssetPackage(Config->GetPackage(), Config);
	const UMassEntityConfigAsset* SetParent = Config->GetConfig().GetParent();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("config_path"), Config->GetPathName());
	Data->SetStringField(TEXT("parent_config_path"), Parent->GetPathName());
	Data->SetStringField(TEXT("parent"), SetParent ? SetParent->GetPathName() : FString());
	Data->SetBoolField(TEXT("set"), SetParent == Parent);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_mass_config_parent"));
}

// MCP:set_mass_trait_property - Set one property (name or dotted struct path) of a trait on a Mass entity config asset from import text, as one undo step, and save it. set_properties needs the trait's object path; this finds it by trait_class (add it: add_mass_trait). The trait gets PostEditChange as after a Details-panel edit; there is no edit gate, and a value that does not import is refused with nothing changed. A failed save makes success false, with saved=false and save_error. Read: get_mass_trait_property.
//   @retired set_properties - object_path=<the trait's object_path from list_mass_traits>, properties={"<property>":<value>} (a struct as {"Field":value}), save=true
//   @param config_path - Asset path to the UMassEntityConfigAsset
//   @param trait_class - Trait class name; the trait must already be on the config (e.g. MassVelocityRandomizerTrait)
//   @param property_name - Property name or dotted path (e.g. MaxSpeed, or Movement.HighResSpeed)
//   @param value - String value parsed by FProperty::ImportText_Direct
//   @returns { config_path, trait_class, property_name, value, set, saved, save_error? }
TSharedPtr<FJsonObject> HandleSetMassTraitProperty(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ConfigPath, TraitClassName, PropName, Value;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("config_path"), ConfigPath)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("trait_class"), TraitClassName)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("property_name"), PropName)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("value"), Value, mcp_common::EAllowEmpty::Yes)) { return Missing; }
	FMassTraitPropertyTarget Target;
	if (TSharedPtr<FJsonObject> Error = Mass_ResolveTraitProperty(ConfigPath, TraitClassName, PropName,
		TEXT("; add_mass_trait first"), Target))
	{
		return Error;
	}
	// One undo step: the config and its trait are snapshotted before the write, and the trait is told about the
	// change afterwards, as a Details-panel edit would.
	// The value is parsed before anything is recorded: a text the property cannot read whole is refused unchanged.
	void* Parsed = Target.Prop->AllocateAndInitializeValue();
	Target.Prop->CopyCompleteValue(Parsed, Target.ValuePtr);
	FString ImportError;
	if (!mcp_common::ImportPropertyTextStrict(Target.Prop, Value, Parsed, Target.Trait, ImportError))
	{
		Target.Prop->DestroyAndFreeValue(Parsed);
		return mcp_common::MakeErrorReply(FString::Printf(TEXT("Failed to import value '%s' into %s.%s: %s; nothing was changed"),
			*Value, *Target.TraitClass->GetName(), *PropName, *ImportError));
	}
	const FScopedTransaction Transaction(NSLOCTEXT("UEAgentMass", "SetMassTraitProperty", "Set Mass Trait Property"));
	Target.Config->Modify();
	Target.Trait->Modify();
	Target.Prop->CopyCompleteValue(Target.ValuePtr, Parsed);
	Target.Prop->DestroyAndFreeValue(Parsed);
	Target.Trait->PostEditChange();
	FString SaveError;
	const bool bSaved = mcp_common::SaveAssetPackage(Target.Config->GetPackage(), Target.Config, SaveError);

	FString ReadBack;
	Target.Prop->ExportTextItem_Direct(ReadBack, Target.ValuePtr, nullptr, nullptr, PPF_None);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("config_path"), Target.Config->GetPathName());
	Data->SetStringField(TEXT("trait_class"), Target.TraitClass->GetName());
	Data->SetStringField(TEXT("property_name"), PropName);
	Data->SetStringField(TEXT("value"), ReadBack);
	Data->SetBoolField(TEXT("set"), true);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_mass_trait_property"), SaveError);
}

// MCP:get_mass_trait_property - Read a property (name or dotted path) of a trait on a Mass entity config, picked by trait_class, as ExportText plus its C++ type. Use it because a trait is an instanced object inside the config that get_properties cannot address by class. Write: set_mass_trait_property.
//   @retired get_properties - object_path=<the trait's object_path from list_mass_traits>, property_names=["<property>"]
//   @param config_path - Asset path to the UMassEntityConfigAsset
//   @param trait_class - Trait class name; the trait must be on the config
//   @param property_name - Property name or dotted path
//   @returns { config_path, trait_class, property_name, value, type }
TSharedPtr<FJsonObject> HandleGetMassTraitProperty(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ConfigPath, TraitClassName, PropName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("config_path"), ConfigPath)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("trait_class"), TraitClassName)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("property_name"), PropName)) { return Missing; }
	FMassTraitPropertyTarget Target;
	if (TSharedPtr<FJsonObject> Error = Mass_ResolveTraitProperty(ConfigPath, TraitClassName, PropName, TEXT(""), Target))
	{
		return Error;
	}
	FString ValueStr;
	Target.Prop->ExportTextItem_Direct(ValueStr, Target.ValuePtr, nullptr, nullptr, PPF_None);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("config_path"), Target.Config->GetPathName());
	Data->SetStringField(TEXT("trait_class"), Target.TraitClass->GetName());
	Data->SetStringField(TEXT("property_name"), PropName);
	Data->SetStringField(TEXT("value"), ValueStr);
	Data->SetStringField(TEXT("type"), Target.Prop->GetCPPType());
	return mcp_common::MakeOkReply(Data);
}

// MCP:set_mass_spawner_entity_types - Replace the entity types of an AMassSpawner actor in the editor world (its protected EntityTypes list, written by reflection) with the given Mass entity configs, each at proportion 1.0. It replaces, never appends: the old list is cleared first, and an empty array leaves the spawner with no types. Paths that do not load as a UMassEntityConfigAsset are skipped silently, so compare linked with the number you passed. No undo step, and the level is neither marked dirty nor saved (save_level). Place the spawner first with spawn_actor (type "MassSpawner"); set how many it spawns with set_mass_spawner_count; read back with get_mass_spawner_info; spawn with spawn_mass_entities. A missing actor_label or config_paths, or no spawner with that label, is an error and nothing changes. Idempotent: the same list gives the same result.
//   @param actor_label - Editor label of the AMassSpawner actor, e.g. "CrowdSpawner"; matched case-insensitively, first match in the editor world (not PIE).
//   @param config_paths - Array of Mass entity config asset paths, e.g. ["/Game/Mass/DA_CrowdAgent"], as create_mass_entity_config returns them (config_path); [] clears the list.
//   @returns { actor_label, entity_type_count, linked, set }
TSharedPtr<FJsonObject> HandleSetMassSpawnerEntityTypes(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ActorLabel;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_label"), ActorLabel)) { return Missing; }
	const TArray<TSharedPtr<FJsonValue>>* PathsArr = nullptr;
	if (!Params->TryGetArrayField(TEXT("config_paths"), PathsArr) || !PathsArr)
	{
		return mcp_common::MakeErrorReply(TEXT("config_paths is required (array of asset paths)"));
	}
	TSharedPtr<FJsonObject> Error;
	AMassSpawner* Spawner = Mass_FindSpawner(ActorLabel, Error);
	if (!Spawner)
	{
		return Error;
	}

	// AMassSpawner::EntityTypes is a protected UPROPERTY — reach it via reflection,
	// reinterpreting the property memory as the real (included) array type.
	FArrayProperty* ArrProp = CastField<FArrayProperty>(
		AMassSpawner::StaticClass()->FindPropertyByName(TEXT("EntityTypes")));
	if (!ArrProp)
	{
		return mcp_common::MakeErrorReply(TEXT("EntityTypes property not found (engine layout changed)"));
	}
	TArray<FMassSpawnedEntityType>* Types =
		ArrProp->ContainerPtrToValuePtr<TArray<FMassSpawnedEntityType>>(Spawner);
	if (!Types) { return mcp_common::MakeErrorReply(TEXT("Failed to access EntityTypes")); }

	Types->Reset();
	int32 Linked = 0;
	for (const TSharedPtr<FJsonValue>& V : *PathsArr)
	{
		FString P;
		if (V.IsValid() && V->TryGetString(P) && !P.IsEmpty())
		{
			if (UMassEntityConfigAsset* Cfg = LoadObject<UMassEntityConfigAsset>(nullptr, *P))
			{
				FMassSpawnedEntityType T;
				T.EntityConfig = Cfg;
				T.Proportion = 1.0f;
				Types->Add(T);
				++Linked;
			}
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_label"), ActorLabel);
	Data->SetNumberField(TEXT("entity_type_count"), Types->Num());
	Data->SetNumberField(TEXT("linked"), Linked);
	Data->SetBoolField(TEXT("set"), true);
	return mcp_common::MakeOkReply(Data);
}

// MCP:set_mass_spawner_count - Set how many entities an AMassSpawner actor in the editor world spawns (its protected Count property, written by reflection); spawn_mass_entities spawns this count, scaled by the spawner's SpawningCountScale and the scalability spawn density. No undo step, and the level is neither marked dirty nor saved (save_level). Use it before spawn_mass_entities; set the types with set_mass_spawner_entity_types; read the current count with get_mass_spawner_info. Idempotent: the same count again gives the same result. set is true when the count read back equals the requested one. A missing actor_label or count, or no spawner with that label, is an error and nothing changes; the count is not range-checked.
//   @param actor_label - Editor label of the AMassSpawner actor, e.g. "CrowdSpawner"; matched case-insensitively, first match in the editor world (not PIE).
//   @param count - Number of entities to spawn, an integer such as 100 (a fraction is truncated).
//   @returns { actor_label, count, set }
TSharedPtr<FJsonObject> HandleSetMassSpawnerCount(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ActorLabel;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_label"), ActorLabel)) { return Missing; }
	double CountD = 0.0;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("count"), CountD)) { return Missing; }
	TSharedPtr<FJsonObject> Error;
	AMassSpawner* Spawner = Mass_FindSpawner(ActorLabel, Error);
	if (!Spawner)
	{
		return Error;
	}

	FIntProperty* CountProp = CastField<FIntProperty>(
		AMassSpawner::StaticClass()->FindPropertyByName(TEXT("Count")));
	if (!CountProp)
	{
		return mcp_common::MakeErrorReply(TEXT("Count property not found (engine layout changed)"));
	}
	CountProp->SetPropertyValue_InContainer(Spawner, static_cast<int32>(CountD));

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_label"), ActorLabel);
	Data->SetNumberField(TEXT("count"), Spawner->GetCount());
	Data->SetBoolField(TEXT("set"), Spawner->GetCount() == static_cast<int32>(CountD));
	return mcp_common::MakeOkReply(Data);
}

// ===========================================================================
// Full authoring: entity CRUD + runtime edit + introspection + signals
// ===========================================================================

namespace
{
	/** Parse "{Index}:{SerialNumber}" or "Index:SerialNumber" into FMassEntityHandle. */
	bool ParseMassEntityHandle(const FString& S, FMassEntityHandle& Out)
	{
		int32 ColonPos = INDEX_NONE;
		if (!S.FindChar(TEXT(':'), ColonPos) || ColonPos <= 0 || ColonPos >= S.Len() - 1)
		{
			return false;
		}
		const FString IndexStr = S.Left(ColonPos);
		const FString SerialStr = S.Mid(ColonPos + 1);
		if (!IndexStr.IsNumeric() || !SerialStr.IsNumeric()) { return false; }
		Out.Index = FCString::Atoi(*IndexStr);
		Out.SerialNumber = FCString::Atoi(*SerialStr);
		return true;
	}

	/** Best-effort FMassEntitySubsystem fetch from the editor world. */
	UMassEntitySubsystem* GetMassEntitySubsystem()
	{
		const UWorld* W = mcp_common::GetEditorWorld();
		return W ? W->GetSubsystem<UMassEntitySubsystem>() : nullptr;
	}

	/** Resolve a class by reflection (accepts "/Script/Mass.foo" or "UMassFoo" etc.). */
	UScriptStruct* ResolveMassScriptStructByName(const FString& Name)
	{
		if (Name.IsEmpty()) { return nullptr; }
		if (FUEAgentGraphUtils::ResolveClass(Name) != nullptr)
		{
			// A name that resolves to a UClass (e.g. a UMassEntityTraitBase subclass) is never a fragment or tag
			// struct (a UClass is not a UScriptStruct); callers reject the null.
			return nullptr;
		}
		// Direct lookup by name (handles "FMassFoo" without prefix).
		for (TObjectIterator<UScriptStruct> It; It; ++It)
		{
			if (It->GetName() == Name) { return *It; }
		}
		return nullptr;
	}

#if UE_VERSION_OLDER_THAN(5, 8, 0)
	/**
	 * Appends every entity of Archetype to OutEntities, chunk by chunk. 5.7 has no
	 * FMassEntityManager::ForEachArchetypeChunkEntityList; a whole-archetype collection exports the same handles.
	 */
	void Mass_GatherArchetypeEntities(const FMassArchetypeHandle& Archetype, TArray<FMassEntityHandle>& OutEntities)
	{
		FMassArchetypeEntityCollection(Archetype).ExportEntityHandles(OutEntities);
	}
#endif

	/**
	 * Add (bAdd) or remove the tag param on the entity_handle param's entity by migrating its archetype, and describe
	 * the migration in Data; returns the error reply when it cannot (TagRequiredError when tag is missing).
	 */
	TSharedPtr<FJsonObject> Mass_ChangeEntityTag(const TSharedPtr<FJsonObject>& Params, const TCHAR* TagRequiredError,
		bool bAdd, const TSharedPtr<FJsonObject>& Data)
	{
		FString HandleStr;
		FString TagName;
		if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("entity_handle"), HandleStr)) { return Missing; }
		if (!Params->TryGetStringField(TEXT("tag"), TagName) || TagName.IsEmpty())
		{
			return mcp_common::MakeErrorReply(TagRequiredError);
		}
		FMassEntityHandle Handle;
		if (!ParseMassEntityHandle(HandleStr, Handle))
		{
			return mcp_common::MakeErrorReply(FString::Printf(TEXT("Invalid entity_handle: %s"), *HandleStr));
		}
		UScriptStruct* TagStruct = ResolveMassScriptStructByName(TagName);
		if (!TagStruct)
		{
			return mcp_common::MakeErrorReply(FString::Printf(TEXT("tag not found as UScriptStruct: %s"), *TagName));
		}

		UMassEntitySubsystem* MassSub = GetMassEntitySubsystem();
		if (!MassSub)
		{
			return mcp_common::MakeErrorReply(TEXT("UMassEntitySubsystem unavailable"));
		}
		FMassEntityManager& Mgr = MassSub->GetMutableEntityManager();
		if (!Mgr.IsEntityValid(Handle))
		{
			return mcp_common::MakeErrorReply(FString::Printf(TEXT("Entity no longer valid: %s"), *HandleStr));
		}

		const FMassArchetypeHandle OldArch = Mgr.GetArchetypeForEntityUnsafe(Handle);
		// No public GetArchetypeEntities: gather the handles with ForEachArchetypeChunkEntityList.
		TArray<FMassEntityHandle> ArchEntities;
#if UE_VERSION_OLDER_THAN(5, 8, 0)
		Mass_GatherArchetypeEntities(OldArch, ArchEntities);
#else
		FMassEntityManager::ForEachArchetypeChunkEntityList(OldArch,
			[&ArchEntities](TConstArrayView<FMassEntityHandle> ChunkEntities)
			{
				ArchEntities.Append(ChunkEntities.GetData(), ChunkEntities.Num());
			});
#endif
		FMassArchetypeEntityCollection Collection(OldArch, ArchEntities,
			FMassArchetypeEntityCollection::NoDuplicates);
		FMassTagBitSet Tags;
#if UE_VERSION_OLDER_THAN(5, 8, 0)
		Tags.Add(*TagStruct);  // 5.7's Add takes the struct by reference
#else
		Tags.Add(TagStruct);
#endif
		if (bAdd)
		{
			Mgr.BatchChangeTagsForEntities(MakeArrayView(&Collection, 1), Tags, FMassTagBitSet());
		}
		else
		{
			Mgr.BatchChangeTagsForEntities(MakeArrayView(&Collection, 1), FMassTagBitSet(), Tags);
		}

		const FMassArchetypeHandle NewArch = Mgr.GetArchetypeForEntityUnsafe(Handle);
		Data->SetStringField(TEXT("entity_handle"), HandleStr);
		Data->SetStringField(TEXT("old_archetype_id"), FString::Printf(TEXT("arch_%u"), GetTypeHash(OldArch)));
		Data->SetStringField(TEXT("new_archetype_id"), FString::Printf(TEXT("arch_%u"), GetTypeHash(NewArch)));
		Data->SetStringField(TEXT("tag"), TagName);
		Data->SetBoolField(TEXT("changed"), OldArch != NewArch);
		return nullptr;
	}
}

// ---------------------------------------------------------------------------
// delete_mass_entity
// ---------------------------------------------------------------------------
// MCP:delete_mass_entity - Destroy a single Mass entity by its handle (Index:SerialNumber), or with all=true every Mass entity in the editor world (not a PIE world), archetype by archetype; no undo either way. Give exactly one of entity_handle and all. To despawn what one spawner made use despawn_mass_entities.
//   @renamed-from destroy_mass_entity
//   @param entity_handle - "{Index}:{SerialNumber}" string returned by spawn/query. Provide this OR all.
//   @param all - Optional bool, default false: true destroys every entity of the editor world's entity manager instead of one. Provide this OR entity_handle.
//   @returns { entity_handle?: string, destroyed?: bool, already_destroyed?: bool, all?: bool, destroyed_count?: int, before_count?: int, after_count?: int }
TSharedPtr<FJsonObject> HandleDestroyMassEntity(const TSharedPtr<FJsonObject>& Params)
{
	FString HandleStr;
	const bool bHasHandle = Params->TryGetStringField(TEXT("entity_handle"), HandleStr) && !HandleStr.IsEmpty();
	bool bAll = false;
	Params->TryGetBoolField(TEXT("all"), bAll);
	if (bAll && bHasHandle)
	{
		return mcp_common::MakeErrorReply(TEXT("Provide entity_handle OR all=true, not both"));
	}

	if (bAll)
	{
		UMassEntitySubsystem* MassSub = GetMassEntitySubsystem();
		if (!MassSub) { return mcp_common::MakeErrorReply(TEXT("UMassEntitySubsystem unavailable (editor world not ready)")); }
		FMassEntityManager& Mgr = MassSub->GetMutableEntityManager();

		const int32 Before = Mass_LiveEntityCount(Mgr);
		// No public GetArchetypes() -- walk via FMassEntityQuery (empty
		// requirements = match all) and batch-destroy each per-archetype collection.
		FMassEntityQuery Query(Mgr.AsShared());
		TArray<FMassArchetypeEntityCollection> Collections = Query.CreateMatchingEntitiesCollection();
		int32 Destroyed = 0;
		for (const FMassArchetypeEntityCollection& C : Collections)
		{
			Destroyed += Mgr.DebugGetArchetypeEntitiesCount(C.GetArchetype());
			Mgr.BatchDestroyEntityChunks(C);
		}
		const int32 After = Mass_LiveEntityCount(Mgr);

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("all"), true);
		Data->SetNumberField(TEXT("destroyed_count"), Destroyed);
		Data->SetNumberField(TEXT("before_count"), Before);
		Data->SetNumberField(TEXT("after_count"), After);
		return mcp_common::MakeOkReply(Data);
	}

	if (!bHasHandle)
	{
		return mcp_common::MakeErrorReply(TEXT("Provide entity_handle (format: Index:SerialNumber) or all=true"));
	}
	FMassEntityHandle Handle;
	if (!ParseMassEntityHandle(HandleStr, Handle))
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Invalid entity_handle (expected Index:SerialNumber): %s"), *HandleStr));
	}

	UMassEntitySubsystem* MassSub = GetMassEntitySubsystem();
	if (!MassSub) { return mcp_common::MakeErrorReply(TEXT("UMassEntitySubsystem unavailable (editor world not ready)")); }
	FMassEntityManager& Mgr = MassSub->GetMutableEntityManager();

	const bool bAlreadyDead = !Mgr.IsEntityValid(Handle);
	if (bAlreadyDead)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("entity_handle"), HandleStr);
		Data->SetBoolField(TEXT("destroyed"), false);
		Data->SetBoolField(TEXT("already_destroyed"), true);
		return mcp_common::MakeOkReply(Data);
	}
	Mgr.DestroyEntity(Handle);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("entity_handle"), HandleStr);
	Data->SetBoolField(TEXT("destroyed"), true);
	Data->SetBoolField(TEXT("already_destroyed"), false);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_mass_entity_info
// ---------------------------------------------------------------------------
// MCP:get_mass_entity_info - Inspect one Mass entity of the editor world (not PIE) by handle, read-only: valid, built, archetype_id and the fragment types of its archetype. A destroyed or unknown handle returns valid:false rather than an error; a malformed handle is an error.
//   @param entity_handle - Entity handle as "Index:SerialNumber", two integers such as "12:3" (no braces)
//   @returns { entity_handle, valid, built, archetype_id, fragment_count, fragment_types: [string] }
TSharedPtr<FJsonObject> HandleGetMassEntityInfo(const TSharedPtr<FJsonObject>& Params)
{
	FString HandleStr;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("entity_handle"), HandleStr)) { return Missing; }
	FMassEntityHandle Handle;
	if (!ParseMassEntityHandle(HandleStr, Handle))
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Invalid entity_handle: %s"), *HandleStr));
	}

	UMassEntitySubsystem* MassSub = GetMassEntitySubsystem();
	if (!MassSub) { return mcp_common::MakeErrorReply(TEXT("UMassEntitySubsystem unavailable")); }
	FMassEntityManager& Mgr = MassSub->GetMutableEntityManager();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("entity_handle"), HandleStr);
	const bool bValid = Mgr.IsEntityValid(Handle);
	Data->SetBoolField(TEXT("valid"), bValid);
	if (bValid)
	{
		Data->SetBoolField(TEXT("built"), Mgr.IsEntityBuilt(Handle));
		const FMassArchetypeHandle Archetype = Mgr.GetArchetypeForEntityUnsafe(Handle);
		Data->SetStringField(TEXT("archetype_id"), FString::Printf(TEXT("arch_%u"), GetTypeHash(Archetype)));
		TArray<const UScriptStruct*> Fragments;
		Mgr.DebugGetArchetypeFragmentTypes(Archetype, Fragments);
		TArray<TSharedPtr<FJsonValue>> FragJson;
		for (const UScriptStruct* S : Fragments)
		{
			if (S) { FragJson.Add(MakeShared<FJsonValueString>(S->GetName())); }
		}
		Data->SetNumberField(TEXT("fragment_count"), Fragments.Num());
		Data->SetArrayField(TEXT("fragment_types"), FragJson);
	}
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_mass_entity_count
// ---------------------------------------------------------------------------
// MCP:get_mass_entity_count - Count entities matching a fragment/tag requirement. Requires "fragment" (UScriptStruct path or name) and optional "presence" ("All"|"Any"|"None" — default "All"). For tag-only queries use "tag" (UScriptStruct path).
//   @renamed-from query_mass_entities
//   @param fragment - Script struct path or name (e.g. "/Script/MassMovement.MassMovementFragment" or "FMassMovementFragment").
//   @param tag - Optional. Same format as fragment. Use to require an FMassTag marker.
//   @param presence - "All" (default), "Any", or "None".
//   @returns { matching_count: int, fragment, tag, presence }
TSharedPtr<FJsonObject> HandleQueryMassEntities(const TSharedPtr<FJsonObject>& Params)
{
	FString FragmentName;
	Params->TryGetStringField(TEXT("fragment"), FragmentName);
	FString TagName;
	Params->TryGetStringField(TEXT("tag"), TagName);
	if (FragmentName.IsEmpty() && TagName.IsEmpty())
	{
		return mcp_common::MakeErrorReply(TEXT("At least one of 'fragment' or 'tag' is required"));
	}
	FString PresenceStr;
	Params->TryGetStringField(TEXT("presence"), PresenceStr);
	EMassFragmentPresence Presence = EMassFragmentPresence::All;
	if (PresenceStr.Equals(TEXT("Any"), ESearchCase::IgnoreCase)) { Presence = EMassFragmentPresence::Any; }
	else if (PresenceStr.Equals(TEXT("None"), ESearchCase::IgnoreCase)) { Presence = EMassFragmentPresence::None; }

	UMassEntitySubsystem* MassSub = GetMassEntitySubsystem();
	if (!MassSub) { return mcp_common::MakeErrorReply(TEXT("UMassEntitySubsystem unavailable")); }
	FMassEntityManager& Mgr = MassSub->GetMutableEntityManager();

	FMassEntityQuery Query(Mgr.AsShared());
	if (!FragmentName.IsEmpty())
	{
		UScriptStruct* FragStruct = ResolveMassScriptStructByName(FragmentName);
		if (!FragStruct)
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("fragment not found as UScriptStruct: %s"), *FragmentName));
		}
		Query.AddRequirement(FragStruct, EMassFragmentAccess::ReadOnly, Presence);
	}
	if (!TagName.IsEmpty())
	{
		UScriptStruct* TagStruct = ResolveMassScriptStructByName(TagName);
		if (!TagStruct)
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("tag not found as UScriptStruct: %s"), *TagName));
		}
		Query.AddTagRequirement(*TagStruct, Presence);
	}
	Query.CacheArchetypes();
	const int32 Count = Query.GetNumMatchingEntities();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("matching_count"), Count);
	if (!FragmentName.IsEmpty()) { Data->SetStringField(TEXT("fragment"), FragmentName); }
	if (!TagName.IsEmpty())      { Data->SetStringField(TEXT("tag"), TagName); }
	Data->SetStringField(TEXT("presence"), PresenceStr.IsEmpty() ? TEXT("All") : PresenceStr);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// add_mass_tag_to_entity  — HOP REPLAY
// ---------------------------------------------------------------------------
// MCP:add_mass_tag_to_entity - Add an FMassTag (a tag, not a trait or fragment, despite the name) to a Mass entity of the editor world (not PIE) by archetype migration. Caveat: EVERY entity sharing that archetype gets the tag, not only this one. Returns old_archetype_id, new_archetype_id and changed; inverse: remove_mass_tag_from_entity.
//   @param entity_handle - Entity handle as "Index:SerialNumber", two integers such as "12:3" (no braces); a no-longer-valid entity is an error
//   @param tag - Reflected name of an FMassTag struct without the F prefix, as list_mass_tag_types returns it (e.g. "MassOffLODTag"); /Script paths do not resolve
//   @returns { entity_handle, old_archetype_id, new_archetype_id, tag, changed }
TSharedPtr<FJsonObject> HandleAddMassTraitToEntity(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	if (TSharedPtr<FJsonObject> Error = Mass_ChangeEntityTag(Params, TEXT("tag is required (UScriptStruct name)"), /*bAdd*/ true, Data))
	{
		return Error;
	}
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// remove_mass_tag_from_entity
// ---------------------------------------------------------------------------
// MCP:remove_mass_tag_from_entity - Remove an FMassTag from a Mass entity of the editor world (not PIE) by archetype migration; inverse of add_mass_tag_to_entity. Caveat: the tag is removed from EVERY entity sharing that archetype, not only this one. Returns old_archetype_id, new_archetype_id and changed (false when nothing moved).
//   @param entity_handle - Entity handle as "Index:SerialNumber", two integers such as "12:3" (no braces); a no-longer-valid entity is an error
//   @param tag - Reflected name of the FMassTag struct to remove, without the F prefix, as list_mass_tag_types returns it (e.g. "MassOffLODTag"); /Script paths do not resolve
//   @returns { entity_handle, old_archetype_id, new_archetype_id, tag, changed }
TSharedPtr<FJsonObject> HandleRemoveMassTraitFromEntity(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	if (TSharedPtr<FJsonObject> Error = Mass_ChangeEntityTag(Params, TEXT("tag is required"), /*bAdd*/ false, Data))
	{
		return Error;
	}
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// list_mass_tag_types
// ---------------------------------------------------------------------------
// MCP:list_mass_tag_types - Lists the Mass tag types loaded in this editor: every struct derived from FMassTag, the zero-size markers an entity carries or not (e.g. MassOffLODTag), each with its reflected name and path, in no particular order. Abstract tag structs are left out unless include_abstract is true; nothing else is filtered. Read-only reflection: changes nothing. The name is what add_mass_tag_to_entity and remove_mass_tag_from_entity take as tag. Tags are not traits: the traits an entity config holds are listed with list_mass_traits and added with add_mass_trait; live entities and their archetypes come from list_mass_archetypes and get_mass_entity_info.
//   @param include_abstract - Optional bool, default false; true also lists tag structs marked Abstract (bases that are not meant to be set on entities).
//   @returns { count, traits: [{ name, path }] } -- traits holds the tag types (name without the F prefix, e.g. "MassOffLODTag"; path is the /Script path)
TSharedPtr<FJsonObject> HandleListMassTraitTypes(const TSharedPtr<FJsonObject>& Params)
{
	bool bIncludeAbstract = false;
	Params->TryGetBoolField(TEXT("include_abstract"), bIncludeAbstract);

	TArray<TSharedPtr<FJsonValue>> Out;
	int32 Count = 0;
	for (TObjectIterator<UScriptStruct> It; It; ++It)
	{
		UScriptStruct* S = *It;
		if (!S) { continue; }
		// FMassTag is a zero-size marker base struct; concrete tags derive from it.
		if (!S->IsChildOf(FMassTag::StaticStruct())) { continue; }
		if (S->HasMetaData(TEXT("Abstract")) && !bIncludeAbstract) { continue; }
		if (S == FMassTag::StaticStruct()) { continue; } // skip the base
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), S->GetName());
		Entry->SetStringField(TEXT("path"), S->GetPathName());
		Out.Add(MakeShared<FJsonValueObject>(Entry));
		++Count;
	}
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("count"), Count);
	Data->SetArrayField (TEXT("traits"), Out);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// fire_mass_signal
// ---------------------------------------------------------------------------
// MCP:fire_mass_signal - Raise a named Mass signal through the UMassSignalSubsystem of the editor world (not a PIE world) for one entity, or for every live entity when entity_handle is left out; the signal processors subscribed to that name react the next time Mass processing runs in that world, which is the only side effect (nothing is saved and there is no undo step). Use it to trigger signal-driven logic (for example StateTree re-evaluation) without waiting for gameplay. The signal name is NOT validated: a name no processor listens to is accepted and does nothing, and dispatched_count still counts the entities signalled. An entity_handle that does not parse or names no live entity (check it with get_mass_entity_info) is an error and nothing is signalled; so is a world without the Mass signal or entity subsystem. Count the live entities with list_mass_archetypes or get_mass_entity_count.
//   @param signal - Signal name, as the receiving processors subscribe to it, e.g. "StateTreeActivate" (case-insensitive FName); any name is accepted.
//   @param entity_handle - Optional handle "Index:SerialNumber" of one live entity, e.g. "12:3" (Index above 0, SerialNumber not 0). Leave it out to signal every live entity of the editor world.
//   @returns { signal, dispatched_count: int, broadcast: bool }
TSharedPtr<FJsonObject> HandleFireMassSignal(const TSharedPtr<FJsonObject>& Params)
{
	FString SignalName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("signal"), SignalName)) { return Missing; }

	if (!GEditor) { return mcp_common::MakeErrorReply(TEXT("No editor world available")); }
	UWorld* W = mcp_common::GetEditorWorld();
	UMassSignalSubsystem* SigSub = W ? W->GetSubsystem<UMassSignalSubsystem>() : nullptr;
	if (!SigSub) { return mcp_common::MakeErrorReply(TEXT("UMassSignalSubsystem unavailable")); }

	int32 Dispatched = 0;
	bool bBroadcast = false;
	FString HandleStr;
	if (Params->TryGetStringField(TEXT("entity_handle"), HandleStr) && !HandleStr.IsEmpty())
	{
		FMassEntityHandle Handle;
		if (!ParseMassEntityHandle(HandleStr, Handle))
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("Invalid entity_handle: %s"), *HandleStr));
		}
		// UMassSignalSubsystem::SignalEntity check()s that the handle is set: a zero index or serial number closed the
		// editor. A handle of no live entity is refused too, as the other entity commands refuse it.
		UMassEntitySubsystem* EntitySub = W->GetSubsystem<UMassEntitySubsystem>();
		if (Handle.Index <= 0 || Handle.SerialNumber == 0 || !EntitySub || !EntitySub->GetEntityManager().IsEntityValid(Handle))
		{
			return mcp_common::MakeErrorReply(FString::Printf(
				TEXT("entity_handle %s names no live Mass entity (get_mass_entity_count gives the live ones); nothing was signalled"), *HandleStr));
		}
		SigSub->SignalEntity(FName(*SignalName), Handle);
		Dispatched = 1;
	}
	else
	{
		// Broadcast to every live entity.
		UMassEntitySubsystem* MassSub = W->GetSubsystem<UMassEntitySubsystem>();
		if (!MassSub) { return mcp_common::MakeErrorReply(TEXT("UMassEntitySubsystem unavailable for broadcast")); }
		FMassEntityManager& Mgr = MassSub->GetMutableEntityManager();
		// Walk via FMassEntityQuery (empty requirements = match all)
		// -> per-archetype collections. For each archetype gather handles via the
		// static FMassEntityManager::ForEachArchetypeChunkEntityList helper.
		FMassEntityQuery Query(Mgr.AsShared());
		TArray<FMassArchetypeEntityCollection> Collections = Query.CreateMatchingEntitiesCollection();
		for (const FMassArchetypeEntityCollection& C : Collections)
		{
			const FMassArchetypeHandle A = C.GetArchetype();
			if (Mgr.DebugGetArchetypeEntitiesCount(A) <= 0) { continue; }
			TArray<FMassEntityHandle> ArchEntities;
#if UE_VERSION_OLDER_THAN(5, 8, 0)
			Mass_GatherArchetypeEntities(A, ArchEntities);
#else
			FMassEntityManager::ForEachArchetypeChunkEntityList(A,
				[&ArchEntities](TConstArrayView<FMassEntityHandle> ChunkEntities)
				{
					ArchEntities.Append(ChunkEntities.GetData(), ChunkEntities.Num());
				});
#endif
			if (ArchEntities.Num() == 0) { continue; }
			SigSub->SignalEntities(FName(*SignalName), ArchEntities);
			Dispatched += ArchEntities.Num();
		}
		bBroadcast = true;
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("signal"), SignalName);
	Data->SetNumberField(TEXT("dispatched_count"), Dispatched);
	Data->SetBoolField(TEXT("broadcast"), bBroadcast);
	return mcp_common::MakeOkReply(Data);
}

// ===========================================================================
// Read-side counterpart of the config setter: get_mass_config_parent (the
// spawner setters are read back by get_mass_spawner_info).
// ===========================================================================

// MCP:get_mass_config_parent - Read the parent config path of a UMassEntityConfigAsset (inverse of set_mass_config_parent).
//   @param config_path - Asset path of the UMassEntityConfigAsset
//   @returns { config_path, parent_config_path, has_parent, config_class }
TSharedPtr<FJsonObject> HandleGetMassConfigParent(const TSharedPtr<FJsonObject>& Params)
{
	FString ConfigPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("config_path"), ConfigPath)) { return Missing; }
	TSharedPtr<FJsonObject> Error;
	UMassEntityConfigAsset* Config = Mass_LoadConfig(ConfigPath, Error);
	if (!Config)
	{
		return Error;
	}
	const UMassEntityConfigAsset* Parent = Config->GetConfig().GetParent();
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("config_path"), Config->GetPathName());
	Data->SetStringField(TEXT("parent_config_path"), Parent ? Parent->GetPathName() : FString());
	Data->SetBoolField(TEXT("has_parent"), Parent != nullptr);
	Data->SetStringField(TEXT("config_class"), Config->GetClass()->GetName());
	return mcp_common::MakeOkReply(Data);
}

// MCP:list_mass_archetypes - Census the Mass entities of the editor world (not a PIE world), read-only: every archetype with its live-entity and fragment counts. Returns archetype_count, total_entities (the sum over the archetypes), total (the entity manager's own count) and archetypes [{archetype_id, entity_count, fragment_count}]; get_mass_entity_info names the fragments of one entity, get_mass_entity_count counts by fragment or tag.
//   @returns { archetype_count: int, total_entities: int, total: int, archetypes: [{ archetype_id: string, entity_count: int, fragment_count: int }] }
TSharedPtr<FJsonObject> HandleListMassArchetypes(const TSharedPtr<FJsonObject>& Params)
{
	UMassEntitySubsystem* MassSub = GetMassEntitySubsystem();
	if (!MassSub)
	{
		return mcp_common::MakeErrorReply(TEXT("UMassEntitySubsystem unavailable (editor world not ready)"));
	}
	const FMassEntityManager& Mgr = MassSub->GetEntityManager();
	const int32 ArchetypeCount = Mgr.DebugGetArchetypesCount();

	TArray<TSharedPtr<FJsonValue>> Archetypes;
	int32 TotalEntities = 0;
Mgr.ForEachArchetype(0, ArchetypeCount, [&](const FMassEntityManager& IterMgr, const FMassArchetypeHandle& Arch)
    {
        const int32 EntityCount = IterMgr.DebugGetArchetypeEntitiesCount(Arch);

        TArray<const UScriptStruct*> Fragments;
        IterMgr.DebugGetArchetypeFragmentTypes(Arch, Fragments);

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("archetype_id"), FString::Printf(TEXT("arch_%u"), GetTypeHash(Arch)));
        Entry->SetNumberField(TEXT("entity_count"), EntityCount);
        Entry->SetNumberField(TEXT("fragment_count"), Fragments.Num());
        Archetypes.Add(MakeShared<FJsonValueObject>(Entry));
        TotalEntities += EntityCount;
    });

	TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
	Resp->SetNumberField(TEXT("archetype_count"), ArchetypeCount);
	Resp->SetNumberField(TEXT("total_entities"), TotalEntities);
	Resp->SetNumberField(TEXT("total"), Mass_LiveEntityCount(Mgr));
	Resp->SetArrayField(TEXT("archetypes"), Archetypes);
	return mcp_common::MakeOkReply(Resp);
}

const McpCommandFamilies::FRegistrar Registration =
{
	{ TEXT("add_mass_trait"), &HandleAddMassTrait },
	{ TEXT("add_mass_tag_to_entity"), &HandleAddMassTraitToEntity },
	{ TEXT("create_mass_entity_config"), &HandleCreateMassEntityConfig },
	{ TEXT("create_mass_spawner_actor"), &HandleCreateMassSpawnerActor },
	{ TEXT("despawn_mass_entities"), &HandleDespawnMassEntities },
	{ TEXT("delete_mass_entity"), &HandleDestroyMassEntity },
	{ TEXT("fire_mass_signal"), &HandleFireMassSignal },
	{ TEXT("get_mass_config_parent"), &HandleGetMassConfigParent },
	{ TEXT("get_mass_entity_info"), &HandleGetMassEntityInfo },
	{ TEXT("get_mass_spawner_info"), &HandleGetMassSpawnerInfo },
	{ TEXT("get_mass_trait_property"), &HandleGetMassTraitProperty },
	{ TEXT("list_mass_archetypes"), &HandleListMassArchetypes },
	{ TEXT("list_mass_tag_types"), &HandleListMassTraitTypes },
	{ TEXT("list_mass_traits"), &HandleListMassTraits },
	{ TEXT("get_mass_entity_count"), &HandleQueryMassEntities },
	{ TEXT("remove_mass_trait"), &HandleRemoveMassTrait },
	{ TEXT("remove_mass_tag_from_entity"), &HandleRemoveMassTraitFromEntity },
	{ TEXT("set_mass_config_parent"), &HandleSetMassConfigParent },
	{ TEXT("set_mass_spawner_count"), &HandleSetMassSpawnerCount },
	{ TEXT("set_mass_spawner_entity_types"), &HandleSetMassSpawnerEntityTypes },
	{ TEXT("set_mass_trait_property"), &HandleSetMassTraitProperty },
	{ TEXT("spawn_mass_entities"), &HandleSpawnMassEntities },
};
} // namespace UEAgentMassCommands
