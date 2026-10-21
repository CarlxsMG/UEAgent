// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameplayBehaviorConfig.h"
#include "GameplayBehaviorSubsystem.h"
#include "GameplayBehavior.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"   // FMapProperty / FScriptMapHelper: the protected list of running behaviors

/**
 * Gameplay Behavior commands.
 *
 * Wraps the engine's GameplayBehaviors plugin (UGameplayBehaviorConfig, UGameplayBehavior,
 * UGameplayBehaviorSubsystem) so an LLM can author behavior configs and trigger/cancel behaviors on actors.
 *
 * Most authoring in that plugin is Blueprintable — the C++ surface for creating custom UGameplayBehavior
 * subclasses is intentionally narrow. These commands cover the config asset lifecycle + subsystem-level
 * trigger/end, which IS public C++.
 *
 * Triggering a behavior at runtime requires a target actor (Pawn or Character) and a subsystem instance, so
 * in the editor world it works only if the target is a Pawn/Character spawned into the world.
 */
namespace UEAgentGameplayBehaviorCommands
{

// MCP-DOMAIN: GameplayBehavior

namespace
{
	/**
	 * UGameplayBehaviorConfig::BehaviorClass is PROTECTED in 5.7. The
	 * public surface is the virtual `GetBehavior(World)` method which
	 * returns an instance of the behavior. To access the class for
	 * serialization, we use reflection: the property is named
	 * "BehaviorClass" on the UClass, stored as FClassProperty (a
	 * TSubclassOf<UGameplayBehavior>).
	 *
	 * We use ContainerPtrToValuePtr to compute the property's address
	 * within the config object (this is the canonical way to read a
	 * reflected property's value).
	 */
	UClass* GetBehaviorClassViaReflection(UGameplayBehaviorConfig* Config)
	{
		if (!Config) { return nullptr; }
		FProperty* Prop = Config->GetClass()->FindPropertyByName(
			FName(TEXT("BehaviorClass")));
		if (!Prop) { return nullptr; }
		// FClassProperty inherits from FObjectPropertyBase; cast to the
		// base to use the unified GetObjectPropertyValue_InContainer.
		FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop);
		if (!ObjProp) { return nullptr; }
		return Cast<UClass>(ObjProp->GetObjectPropertyValue_InContainer(Config));
	}

	void SetBehaviorClassViaReflection(UGameplayBehaviorConfig* Config, UClass* Cls)
	{
		if (!Config || !Cls) { return; }
		FProperty* Prop = Config->GetClass()->FindPropertyByName(
			FName(TEXT("BehaviorClass")));
		if (!Prop) { return; }
		FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop);
		if (!ObjProp) { return; }
		ObjProp->SetObjectPropertyValue_InContainer(Config, Cls);
	}

	/** The UClass named Name (retried with a "U" prefix when Name has none), or null. */
	UClass* GameplayBehavior_FindClass(const FString& Name)
	{
		if (UClass* Cls = FindObject<UClass>(nullptr, *Name))
		{
			return Cls;
		}
		const FString Prefixed = FString::Printf(TEXT("%s%s"),
			Name.StartsWith(TEXT("U")) ? TEXT("") : TEXT("U"),
			*Name);
		return FindObject<UClass>(nullptr, *Prefixed);
	}

	/**
	 * The Pawn labelled ActorLabel in the editor world, with that world's UGameplayBehaviorSubsystem in OutSubsystem;
	 * on failure returns nullptr and fills OutError (NoSubsystemError, NotPawnError).
	 */
	APawn* GameplayBehavior_FindPawn(const FString& ActorLabel, const TCHAR* NoSubsystemError, const FString& NotPawnError,
		UGameplayBehaviorSubsystem*& OutSubsystem, TSharedPtr<FJsonObject>& OutError)
	{
		UWorld* World = nullptr;
		if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World))
		{
			OutError = NoWorld;
			return nullptr;
		}
		OutSubsystem = UGameplayBehaviorSubsystem::GetCurrent(World);
		if (!OutSubsystem)
		{
			OutError = mcp_common::MakeErrorReply(NoSubsystemError);
			return nullptr;
		}
		AActor* const LabelledActor = mcp_common::FindActorByLabel(World, ActorLabel);
		if (!LabelledActor)
		{
			OutError = mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor not found: %s"), *ActorLabel));
			return nullptr;
		}
		APawn* LabelledPawn = Cast<APawn>(LabelledActor);
		if (!LabelledPawn)
		{
			OutError = mcp_common::MakeErrorReply(NotPawnError);
		}
		return LabelledPawn;
	}

	/**
	 * How many behaviors that are a BehaviorClass Subsystem tracks as running on Avatar, read by reflection because
	 * UGameplayBehaviorSubsystem::AgentGameplayBehaviors is protected. A behavior leaves that list when it ends
	 * (OnBehaviorFinished), so this is what is still running. INDEX_NONE when the property does not have the layout
	 * this reads (TMap<AActor*, FAgentGameplayBehaviors> whose value holds a Behaviors object array).
	 */
	int32 GameplayBehavior_CountRunning(UGameplayBehaviorSubsystem* Subsystem, const AActor* Avatar, const UClass* BehaviorClass)
	{
		const FMapProperty* MapProp = CastField<FMapProperty>(
			UGameplayBehaviorSubsystem::StaticClass()->FindPropertyByName(TEXT("AgentGameplayBehaviors")));
		const FObjectPropertyBase* KeyProp = MapProp ? CastField<FObjectPropertyBase>(MapProp->GetKeyProperty()) : nullptr;
		const FStructProperty* ValueProp = MapProp ? CastField<FStructProperty>(MapProp->GetValueProperty()) : nullptr;
		const FArrayProperty* BehaviorsProp = ValueProp
			? CastField<FArrayProperty>(ValueProp->Struct->FindPropertyByName(TEXT("Behaviors"))) : nullptr;
		const FObjectPropertyBase* BehaviorProp = BehaviorsProp ? CastField<FObjectPropertyBase>(BehaviorsProp->Inner) : nullptr;
		if (!KeyProp || !BehaviorProp)
		{
			return INDEX_NONE;
		}

		FScriptMapHelper Agents(MapProp, MapProp->ContainerPtrToValuePtr<void>(Subsystem));
		for (FScriptMapHelper::FIterator It(Agents); It; ++It)
		{
			if (KeyProp->GetObjectPropertyValue(Agents.GetKeyPtr(It)) != Avatar)
			{
				continue;
			}
			FScriptArrayHelper Behaviors(BehaviorsProp, BehaviorsProp->ContainerPtrToValuePtr<void>(Agents.GetValuePtr(It)));
			int32 Count = 0;
			for (int32 Index = 0; Index < Behaviors.Num(); ++Index)
			{
				const UObject* Behavior = BehaviorProp->GetObjectPropertyValue(Behaviors.GetRawPtr(Index));
				if (Behavior && Behavior->IsA(BehaviorClass))
				{
					++Count;
				}
			}
			return Count;
		}
		return 0;
	}
}

// ---------------------------------------------------------------------------
// create_behavior_config
// ---------------------------------------------------------------------------
// MCP:create_behavior_config - Create a UGameplayBehaviorConfig asset at package_path/config_name, or load the one already there, set its protected BehaviorClass by reflection when behavior_class resolves, then save it. An unresolved behavior_class is ignored without error, so check behavior_class in the reply; created tells new from existing.
//   @param config_name - Asset name of the config, e.g. "BC_Patrol"
//   @param package_path - Destination /Game folder (default /Game/MCP).
//   @param behavior_class - Full path of a loaded UGameplayBehavior subclass, e.g. "/Script/GameplayBehaviorsModule.GameplayBehavior_AnimationBased"; a short name never resolves, and a miss is ignored
//   @returns { config_path: string, config_name: string, behavior_class: string, created: bool, saved: bool }
TSharedPtr<FJsonObject> HandleCreateBehaviorConfig(
    const TSharedPtr<FJsonObject>& Params)
{
	FString ConfigName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("config_name"), ConfigName)) { return Missing; }
	FString PackagePath = mcp_common::DefaultAssetFolder();
	Params->TryGetStringField(TEXT("package_path"), PackagePath);
	FString BehaviorClass;
	Params->TryGetStringField(TEXT("behavior_class"), BehaviorClass);

	FString CreateError;
	bool bCreated = false;
	UGameplayBehaviorConfig* Config = mcp_common::CreateOrLoadAsset<UGameplayBehaviorConfig>(
		PackagePath / ConfigName, CreateError, &bCreated);
	if (!Config)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Failed to create or load UGameplayBehaviorConfig: %s"), *CreateError));
	}

	// Assign the BehaviorClass via reflection (the field is protected in 5.7).
	UClass* Cls = nullptr;
	if (!BehaviorClass.IsEmpty())
	{
		Cls = GameplayBehavior_FindClass(BehaviorClass);
		if (Cls && Cls->IsChildOf(UGameplayBehavior::StaticClass()))
		{
			SetBehaviorClassViaReflection(Config, Cls);
		}
	}

	Config->MarkPackageDirty();
	const bool bSaved = mcp_common::SaveAssetPackage(Config->GetPackage(), Config);

	UClass* ReadBackClass = GetBehaviorClassViaReflection(Config);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("config_path"), Config->GetPathName());
	Data->SetStringField(TEXT("config_name"), Config->GetName());
	Data->SetStringField(TEXT("behavior_class"),
		ReadBackClass ? ReadBackClass->GetName() : TEXT(""));
	Data->SetBoolField(TEXT("created"), bCreated);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("create_behavior_config"));
}

// ---------------------------------------------------------------------------
// set_behavior_class
// ---------------------------------------------------------------------------
// MCP:set_behavior_class - Set the BehaviorClass of a UGameplayBehaviorConfig (via reflection; the field is protected in 5.7).
//   @param config_path - Content path of the UGameplayBehaviorConfig.
//   @param behavior_class - UGameplayBehavior subclass name (with or without "U" prefix).
//   @returns { config_path: string, behavior_class: string, set: bool, saved: bool }
TSharedPtr<FJsonObject> HandleSetBehaviorClass(
    const TSharedPtr<FJsonObject>& Params)
{
	FString ConfigPath, BehaviorClass;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("config_path"), ConfigPath)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("behavior_class"), BehaviorClass)) { return Missing; }

	UGameplayBehaviorConfig* Config = LoadObject<UGameplayBehaviorConfig>(nullptr, *ConfigPath);
	if (!Config)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Cannot load config: %s"), *ConfigPath));
	}

	UClass* Cls = GameplayBehavior_FindClass(BehaviorClass);
	if (!Cls)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Cannot find behavior class: %s"), *BehaviorClass));
	}
	if (!Cls->IsChildOf(UGameplayBehavior::StaticClass()))
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Class %s is not a UGameplayBehavior"), *BehaviorClass));
	}

	// Set via reflection (the field is protected).
	SetBehaviorClassViaReflection(Config, Cls);

	Config->MarkPackageDirty();
	const bool bSaved = mcp_common::SaveAssetPackage(Config->GetPackage(), Config);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("config_path"), Config->GetPathName());
	Data->SetStringField(TEXT("behavior_class"), Cls->GetName());
	Data->SetBoolField(TEXT("set"), true);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_behavior_class"));
}

// ---------------------------------------------------------------------------
// get_behavior_config_info
// ---------------------------------------------------------------------------
// MCP:get_behavior_config_info - Read the protected BehaviorClass of a UGameplayBehaviorConfig asset by reflection: behavior_class (class name), behavior_class_path and has_behavior_class. Read-only; set it with set_behavior_class, or list every config with list_behavior_configs.
//   @param config_path - Path of the UGameplayBehaviorConfig asset, e.g. "/Game/MCP/BC_Patrol"
//   @returns { config_path: string, behavior_class: string, behavior_class_path: string, has_behavior_class: bool }
TSharedPtr<FJsonObject> HandleGetBehaviorConfigInfo(
    const TSharedPtr<FJsonObject>& Params)
{
	FString ConfigPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("config_path"), ConfigPath)) { return Missing; }
	UGameplayBehaviorConfig* Config = LoadObject<UGameplayBehaviorConfig>(nullptr, *ConfigPath);
	if (!Config)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Cannot load config: %s"), *ConfigPath));
	}

	UClass* BehaviorClass = GetBehaviorClassViaReflection(Config);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("config_path"), Config->GetPathName());
	Data->SetStringField(TEXT("behavior_class"),
		BehaviorClass ? BehaviorClass->GetName() : TEXT(""));
	Data->SetStringField(TEXT("behavior_class_path"),
		BehaviorClass ? BehaviorClass->GetPathName() : TEXT(""));
	Data->SetBoolField(TEXT("has_behavior_class"), BehaviorClass != nullptr);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// trigger_behavior_on_actor  — uses static UGameplayBehaviorSubsystem::TriggerBehavior
// ---------------------------------------------------------------------------
// MCP:trigger_behavior_on_actor - Start the behavior of a UGameplayBehaviorConfig on a Pawn found by label in the editor world: the config supplies its UGameplayBehavior, whose Trigger runs on the pawn, and the world UGameplayBehaviorSubsystem tracks it until it ends or end_behavior_on_actor stops it. triggered=false means the behavior refused. Runtime only: nothing is saved or undoable. A config without BehaviorClass (see set_behavior_class), no subsystem in the editor world (see get_behavior_subsystem_info), a missing actor or a non-Pawn is an error.
//   @param config_path - Content path of the UGameplayBehaviorConfig.
//   @param actor_label - Label of the Pawn actor in the editor world to receive the behavior.
//   @returns { config_path: string, actor_label: string, behavior_class: string, triggered: bool }
TSharedPtr<FJsonObject> HandleTriggerBehaviorOnActor(
    const TSharedPtr<FJsonObject>& Params)
{
	FString ConfigPath, ActorLabel;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("config_path"), ConfigPath)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_label"), ActorLabel)) { return Missing; }

	UGameplayBehaviorConfig* Config = LoadObject<UGameplayBehaviorConfig>(nullptr, *ConfigPath);
	if (!Config)
	{
		return mcp_common::MakeErrorReply(FString::Printf(TEXT("Cannot load config: %s"), *ConfigPath));
	}
	UClass* BehaviorClass = GetBehaviorClassViaReflection(Config);
	if (!BehaviorClass)
	{
		return mcp_common::MakeErrorReply(TEXT("Config has no BehaviorClass; call set_behavior_class first"));
	}

	// The recipient is addressed by its editor label and must be a Pawn.
	UGameplayBehaviorSubsystem* Sub = nullptr;
	TSharedPtr<FJsonObject> Err;
	APawn* LabelledPawn = GameplayBehavior_FindPawn(ActorLabel, TEXT("UGameplayBehaviorSubsystem is not available in this world"),
		FString::Printf(TEXT("Actor %s is not a Pawn (GameplayBehaviors require Pawn/Character)"), *ActorLabel), Sub, Err);
	if (!LabelledPawn)
	{
		return Err;
	}

	// UGameplayBehaviorSubsystem::TriggerBehavior is a static function in 5.7
	// that takes the config by const-ref. Returns true on successful trigger.
	const bool bTriggered = UGameplayBehaviorSubsystem::TriggerBehavior(
		*Config, *LabelledPawn, /*SmartObjectOwner*/ nullptr);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("config_path"), ConfigPath);
	Data->SetStringField(TEXT("actor_label"), ActorLabel);
	Data->SetStringField(TEXT("behavior_class"), BehaviorClass->GetName());
	Data->SetBoolField(TEXT("triggered"), bTriggered);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// end_behavior_on_actor  — calls StopBehavior on the subsystem, then reads back what still runs
// ---------------------------------------------------------------------------
// MCP:end_behavior_on_actor - Interrupt the gameplay behaviors that the editor world UGameplayBehaviorSubsystem runs on a Pawn found by its label: every running behavior that is a behavior_class (subclasses included), or every one when behavior_class is omitted, gets EndBehavior with interrupted=true. behavior_class must be the full class path of a UGameplayBehavior subclass; a short name, or a path that does not resolve to one, is an error and nothing stops. ended is read back from the subsystem: true once every matching behavior stopped, while still_running counts the ones that did not and makes success false. With none running it succeeds with ended false and a note, so a repeat call is idempotent. Runtime only: nothing is saved or undoable. A missing actor, a non-Pawn or no subsystem is an error. Start one: trigger_behavior_on_actor; subsystem check: get_behavior_subsystem_info.
//   @param actor_label - Label of the Pawn actor in the editor world.
//   @param behavior_class - Optional full path of the UGameplayBehavior subclass to stop, e.g. "/Script/GameplayBehaviorsModule.GameplayBehavior_AnimationBased"; omitted stops every behavior on the pawn, and a path that does not resolve to such a class is an error
//   @returns { actor_label: string, behavior_class: string, ended: bool, stopped_count: int, still_running: int, note?: string }
TSharedPtr<FJsonObject> HandleEndBehaviorOnActor(
    const TSharedPtr<FJsonObject>& Params)
{
	FString ActorLabel, BehaviorClass;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_label"), ActorLabel)) { return Missing; }
	Params->TryGetStringField(TEXT("behavior_class"), BehaviorClass);

	// The class to stop. StopBehavior ends every tracked behavior that IsA it, so UGameplayBehavior stops them all;
	// a named class that does not resolve is refused rather than widened to that.
	UClass* ClsToStop = UGameplayBehavior::StaticClass();
	if (!BehaviorClass.IsEmpty())
	{
		UClass* C = GameplayBehavior_FindClass(BehaviorClass);
		if (!C || !C->IsChildOf(UGameplayBehavior::StaticClass()))
		{
			return mcp_common::MakeErrorReply(FString::Printf(
				TEXT("behavior_class '%s' does not resolve to a UGameplayBehavior subclass (pass a full class path, e.g. /Script/GameplayBehaviorsModule.GameplayBehavior_AnimationBased); nothing was stopped"),
				*BehaviorClass));
		}
		ClsToStop = C;
	}

	// The recipient is addressed by its editor label and must be a Pawn.
	UGameplayBehaviorSubsystem* Sub = nullptr;
	TSharedPtr<FJsonObject> Err;
	APawn* LabelledPawn = GameplayBehavior_FindPawn(ActorLabel, TEXT("UGameplayBehaviorSubsystem is not available"),
		TEXT("Actor is not a Pawn"), Sub, Err);
	if (!LabelledPawn)
	{
		return Err;
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_label"), ActorLabel);
	Data->SetStringField(TEXT("behavior_class"), ClsToStop->GetName());

	// StopBehavior returns false whatever it does in UE 5.8, so the outcome is read from the subsystem's own list.
	const int32 RunningBefore = GameplayBehavior_CountRunning(Sub, LabelledPawn, ClsToStop);
	if (RunningBefore == 0)
	{
		Data->SetBoolField(TEXT("ended"), false);
		Data->SetNumberField(TEXT("stopped_count"), 0);
		Data->SetNumberField(TEXT("still_running"), 0);
		Data->SetStringField(TEXT("note"), FString::Printf(
			TEXT("No %s behavior is running on %s; nothing was stopped."), *ClsToStop->GetName(), *ActorLabel));
		return mcp_common::MakeOkReply(Data);
	}

	Sub->StopBehavior(*LabelledPawn, ClsToStop);

	if (RunningBefore == INDEX_NONE)
	{
		// This engine's subsystem does not have the layout read above: the stop ran but cannot be verified.
		Data->SetStringField(TEXT("note"),
			TEXT("StopBehavior ran, but this engine version does not expose the running behaviors, so whether any ended is unknown."));
		return mcp_common::MakeOkReply(Data);
	}

	const int32 RunningAfter = GameplayBehavior_CountRunning(Sub, LabelledPawn, ClsToStop);
	const bool bEnded = RunningAfter == 0;
	Data->SetBoolField(TEXT("ended"), bEnded);
	Data->SetNumberField(TEXT("stopped_count"), FMath::Max(RunningBefore - RunningAfter, 0));
	Data->SetNumberField(TEXT("still_running"), RunningAfter);
	return mcp_common::MakeOutcomeReply(Data, bEnded,
		bEnded ? FString() : mcp_common::MeasuredOutcomeError(TEXT("end_behavior_on_actor"), TEXT("ended")));
}

// ---------------------------------------------------------------------------
// get_behavior_subsystem_info
// ---------------------------------------------------------------------------
// MCP:get_behavior_subsystem_info - Report whether the editor world has a UGameplayBehaviorSubsystem (UGameplayBehaviorSubsystem::GetCurrent): world_name, subsystem_available and, when it exists, subsystem_class. Read-only, no parameters; trigger_behavior_on_actor and end_behavior_on_actor fail without it.
//   @returns { world_name: string, subsystem_available: bool, subsystem_class: string }
TSharedPtr<FJsonObject> HandleGetBehaviorSubsystemInfo(
    const TSharedPtr<FJsonObject>& /*Params*/)
{
	UWorld* World = nullptr;
	if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World)) { return NoWorld; }
	UGameplayBehaviorSubsystem* Sub = UGameplayBehaviorSubsystem::GetCurrent(World);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("world_name"), World->GetName());
	Data->SetBoolField(TEXT("subsystem_available"), Sub != nullptr);
	if (Sub)
	{
		Data->SetStringField(TEXT("subsystem_class"), Sub->GetClass()->GetName());
	}
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// Typed delete config + enumeration.
// ---------------------------------------------------------------------------

// MCP:remove_behavior_config - Destroy a UGameplayBehaviorConfig data asset (mirror of create_behavior_config). Typed-delete: it loads the asset and REFUSES to delete it if it is not a UGameplayBehaviorConfig, so it cannot wipe the wrong asset the way the generic delete_asset can; that refusal, and a delete the engine does not carry out, make success false with deleted false. Idempotent on missing assets.
//   @param config_path - Full /Game/... asset path of the config.
//   @returns { config_path, deleted, already_existed, asset_class?, note? }
//   NOTE (impl): delegates the actual delete to UEditorAssetLibrary::DeleteLoadedAsset once the type check passes.

TSharedPtr<FJsonObject> HandleRemoveBehaviorConfig(
    const TSharedPtr<FJsonObject>& Params)
{
    FString ConfigPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("config_path"), ConfigPath)) { return Missing; }
    // UEditorAssetLibrary::DoesAssetExist is not reliable headless for
    // recently-created assets (see headless-asset-gotchas) -- LoadObject
    // directly is the honest existence check, with no registry-lag false
    // negative. LoadObject needs the full "Package.AssetName" object path,
    // not a bare package path, so normalize (callers pass either form --
    // e.g. a bare "/Game/MyGame/MyBehaviorConfig").
    FString ObjectPath = ConfigPath;
    if (!ObjectPath.Contains(TEXT(".")))
    {
        FString ShortName;
        int32 SlashIdx = INDEX_NONE;
        if (ObjectPath.FindLastChar(TEXT('/'), SlashIdx))
        {
            ShortName = ObjectPath.RightChop(SlashIdx + 1);
        }
        ObjectPath = ObjectPath + TEXT(".") + ShortName;
    }
    UObject* Asset = LoadObject<UObject>(nullptr, *ObjectPath);
    if (!Asset)
    {
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetStringField(TEXT("config_path"), ConfigPath);
        Resp->SetBoolField(TEXT("deleted"), false);
        Resp->SetBoolField(TEXT("already_existed"), false);
        Resp->SetStringField(TEXT("note"), TEXT("config_path does not exist; nothing to remove"));
        return mcp_common::MakeOkReply(Resp);
    }

    UGameplayBehaviorConfig* Config = Cast<UGameplayBehaviorConfig>(Asset);
    if (!Config)
    {
        // The typed-delete guard: an asset of another class fails the call and is not deleted.
        const FString Refusal = FString::Printf(TEXT("asset is not a UGameplayBehaviorConfig (got %s) - wrong type, refusing to delete"),
            *Asset->GetClass()->GetName());
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetStringField(TEXT("config_path"), ConfigPath);
        Resp->SetStringField(TEXT("asset_class"), Asset->GetClass()->GetName());
        Resp->SetBoolField(TEXT("deleted"), false);
        Resp->SetStringField(TEXT("note"), Refusal);
        return mcp_common::MakeOutcomeReply(Resp, false,
            FString::Printf(TEXT("remove_behavior_config: %s at %s"), *Refusal, *ConfigPath));
    }

    // Capture behavior_class BEFORE delete (Config pointer is invalidated below).
    UClass* BehaviorClass = GetBehaviorClassViaReflection(Config);
    const FString AssetClassName = Asset->GetClass()->GetName();
    // DeleteLoadedAsset(Asset), not DeleteAsset(path) -- same headless-reliability
    // fix as the existence check above; we already have the loaded object.
    const bool bDeleted = UEditorAssetLibrary::DeleteLoadedAsset(Asset);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("config_path"), ConfigPath);
    Resp->SetStringField(TEXT("asset_class"), AssetClassName);
    if (BehaviorClass)
    {
        Resp->SetStringField(TEXT("behavior_class"), BehaviorClass->GetName());
    }
    Resp->SetBoolField(TEXT("deleted"), bDeleted);
    Resp->SetBoolField(TEXT("already_existed"), true);
    return mcp_common::MakeOutcomeReply(Resp, bDeleted, bDeleted ? FString() : FString::Printf(
        TEXT("remove_behavior_config: the config at %s was not deleted (UEditorAssetLibrary::DeleteLoadedAsset returned false: ")
        TEXT("a Play-in-Editor session running, an asset the Asset Registry does not list, or a file that cannot be deleted; ")
        TEXT("the Output Log names which)"), *ConfigPath));
}

// MCP:list_behavior_configs - List the UGameplayBehaviorConfig assets under package_path, recursively, from the Asset Registry, without changing them: each config path and name with the name of its BehaviorClass (empty when unset), plus total. Each match is loaded to read that protected field, so a big folder costs one load per config. Only assets whose class is exactly UGameplayBehaviorConfig are listed: subclass configs such as GameplayBehaviorConfig_Animation or GameplayBehaviorConfig_BehaviorTree, and Blueprint subclasses, are skipped. There is no paging, and no match is total 0, not an error. One config in detail: get_behavior_config_info; create one: create_behavior_config; set its class: set_behavior_class; delete one: remove_behavior_config.
//   @param package_path - Optional content folder searched recursively (default "/Game"); an empty string also means /Game, so another mount point needs its own path
//   @returns { package_path, total, configs:[{ config_path, config_name, behavior_class }] }

TSharedPtr<FJsonObject> HandleListBehaviorConfigs(
    const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath;
    Params->TryGetStringField(TEXT("package_path"), PackagePath);
    if (PackagePath.IsEmpty()) { PackagePath = TEXT("/Game"); }

    IAssetRegistry& AR = mcp_common::AssetRegistry();

    TArray<FAssetData> Assets;
    AR.GetAssetsByPath(FName(*PackagePath), Assets, /*bRecursive*/ true);

    TArray<TSharedPtr<FJsonValue>> Entries;
    for (const FAssetData& AD : Assets)
    {
        if (AD.AssetClassPath != UGameplayBehaviorConfig::StaticClass()->GetClassPathName())
        {
            continue;
        }
        UGameplayBehaviorConfig* Config = Cast<UGameplayBehaviorConfig>(AD.GetAsset());
        if (!Config) { continue; }

        UClass* BehaviorClass = GetBehaviorClassViaReflection(Config);
        TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
        E->SetStringField(TEXT("config_name"), Config->GetName());
        E->SetStringField(TEXT("config_path"), Config->GetPathName());
        E->SetStringField(TEXT("behavior_class"), BehaviorClass ? BehaviorClass->GetName() : FString());
        Entries.Add(MakeShared<FJsonValueObject>(E));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("package_path"), PackagePath);
    Data->SetNumberField(TEXT("total"), Entries.Num());
    Data->SetArrayField(TEXT("configs"), Entries);
    return mcp_common::MakeOkReply(Data);
}

const McpCommandFamilies::FRegistrar Registration =
{
	{ TEXT("create_behavior_config"), &HandleCreateBehaviorConfig },
	{ TEXT("end_behavior_on_actor"), &HandleEndBehaviorOnActor },
	{ TEXT("get_behavior_config_info"), &HandleGetBehaviorConfigInfo },
	{ TEXT("get_behavior_subsystem_info"), &HandleGetBehaviorSubsystemInfo },
	{ TEXT("list_behavior_configs"), &HandleListBehaviorConfigs },
	{ TEXT("remove_behavior_config"), &HandleRemoveBehaviorConfig },
	{ TEXT("set_behavior_class"), &HandleSetBehaviorClass },
	{ TEXT("trigger_behavior_on_actor"), &HandleTriggerBehaviorOnActor },
};
} // namespace UEAgentGameplayBehaviorCommands
