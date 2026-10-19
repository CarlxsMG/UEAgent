#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Common/UEAgentGraphUtils.h"

#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Engine/Level.h"                 // ULevel must be complete -- GetCurrentLevel() feeds StaticFindObjectFast's Outer
#include "EngineUtils.h"
#include "GameFramework/GameMode.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/HUD.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/SpectatorPawn.h"
#include "GameFramework/WorldSettings.h"
// MCP:place_player_start needs the COMPLETE USceneComponent type: the OFPA-safe
// write path Modify()s the actor AND its RootComponent (see the handler).
#include "Components/SceneComponent.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Factories/BlueprintFactory.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Class.h"

/**
 * Game Framework commands.
 *
 * Wraps AGameModeBase / AGameStateBase / APlayerState / APlayerController / AHUD / UGameInstance authoring
 * for an LLM, plus PlayerStart placement. The GameMode class slots (DefaultPawnClass, HUDClass,
 * PlayerStateClass, GameStateClass, SpectatorClass, PlayerControllerClass) are public EditAnywhere
 * BlueprintReadWrite UPROPERTYs of AGameModeBase: the slot setters write them on the CDO of an AGameModeBase
 * subclass, so they become defaults the next time the engine spawns a GameMode of that class.
 */
namespace UEAgentGameFrameworkCommands
{

// MCP-DOMAIN: GameFramework

namespace
{
	UClass* ResolveClassByName(const FString& Name)
	{
		return FUEAgentGraphUtils::ResolveClass(Name);
	}

	/**
	 * The AGameModeBase subclass a game-mode command targets: `game_mode_class_path` (long /Script/... path)
	 * first, then `game_mode_class` (short name). AGameMode (multiplayer match state) is a subclass of
	 * AGameModeBase, so both are accepted. Returns the error reply to send back, or nullptr once OutClass
	 * holds the class and OutChosenName its path.
	 */
	TSharedPtr<FJsonObject> ResolveGameModeClass(const TSharedPtr<FJsonObject>& Params, UClass*& OutClass, FString& OutChosenName)
	{
		FString Path, Short;
		Params->TryGetStringField(TEXT("game_mode_class_path"), Path);
		Params->TryGetStringField(TEXT("game_mode_class"), Short);
		OutClass = nullptr;
		if (!Path.IsEmpty())
		{
			OutClass = ResolveClassByName(Path);
		}
		if (!OutClass && !Short.IsEmpty())
		{
			OutClass = ResolveClassByName(Short);
		}
		if (!OutClass || !OutClass->IsChildOf(AGameModeBase::StaticClass()))
		{
			return mcp_common::MakeErrorReply(
				TEXT("game_mode_class or game_mode_class_path is required and must derive from AGameModeBase"));
		}
		OutChosenName = OutClass->GetPathName();
		return nullptr;
	}

	/** Find AWorldSettings in the editor world. */
	AWorldSettings* GetWorldSettings()
	{
		UWorld* World = mcp_common::GetEditorWorld();
		return World ? World->GetWorldSettings() : nullptr;
	}

	/**
	 * Whether the package holding CDO can be saved: true for a Blueprint class, whose CDO lives in its
	 * Blueprint package; false for a native class, whose CDO lives in a compiled-in script package.
	 */
	bool IsCDOPackageSavable(const UObject* CDO)
	{
		return CDO && !CDO->GetOutermost()->HasAnyPackageFlags(PKG_CompiledIn);
	}

	/** The TSubclassOf<...> UPROPERTY PropName of CDO, or null when CDO has no such object property. */
	FObjectPropertyBase* FindClassPropertyOnCDO(const UObject* CDO, FName PropName)
	{
		return CDO ? CastField<FObjectPropertyBase>(CDO->GetClass()->FindPropertyByName(PropName)) : nullptr;
	}

	/**
	 * Set a TSubclassOf<...> UPROPERTY on a CDO by FName.
	 * Used for DefaultPawnClass / HUDClass / PlayerStateClass / GameStateClass /
	 * SpectatorClass / PlayerControllerClass on AGameModeBase. A CDO in a savable package (a Blueprint
	 * GameMode) is Modify()'d before the write and its package marked dirty after it, so save_asset or the
	 * editor's save prompt persists the value; a native class's CDO is written in memory only, since its
	 * script package is never saved.
	 */
	bool SetClassPropertyOnCDO(UObject* CDO, FName PropName, UClass* NewValue)
	{
		FObjectPropertyBase* ObjProp = FindClassPropertyOnCDO(CDO, PropName);
		if (!ObjProp) { return false; }
		const bool bSavable = IsCDOPackageSavable(CDO);
		if (bSavable)
		{
			CDO->Modify();
		}
		ObjProp->SetObjectPropertyValue_InContainer(CDO, NewValue);
		if (bSavable)
		{
			CDO->MarkPackageDirty();
		}
		return true;
	}

	/** The class held by the TSubclassOf<...> UPROPERTY PropName of CDO; null when unset. */
	const UObject* ReadClassPropertyOnCDO(const UObject* CDO, FName PropName)
	{
		const FObjectPropertyBase* ObjProp = FindClassPropertyOnCDO(CDO, PropName);
		return ObjProp ? ObjProp->GetObjectPropertyValue_InContainer(CDO) : nullptr;
	}

	/** One class slot of AGameModeBase that set_game_mode_classes writes: its param, reply field, member and base. */
	struct FGameModeSlot
	{
		const TCHAR* Param;
		const TCHAR* Field;
		FName Member;
		UClass* Base;
	};

	/** The six class slots of AGameModeBase, in the order set_game_mode_classes reads and writes them. */
	TArray<FGameModeSlot> GameModeSlots()
	{
		return {
			{ TEXT("pawn_class"),              TEXT("default_pawn_class"),      GET_MEMBER_NAME_CHECKED(AGameModeBase, DefaultPawnClass),      APawn::StaticClass() },
			{ TEXT("hud_class"),               TEXT("hud_class"),               GET_MEMBER_NAME_CHECKED(AGameModeBase, HUDClass),              AHUD::StaticClass() },
			{ TEXT("player_state_class"),      TEXT("player_state_class"),      GET_MEMBER_NAME_CHECKED(AGameModeBase, PlayerStateClass),      APlayerState::StaticClass() },
			{ TEXT("game_state_class"),        TEXT("game_state_class"),        GET_MEMBER_NAME_CHECKED(AGameModeBase, GameStateClass),        AGameStateBase::StaticClass() },
			{ TEXT("spectator_class"),         TEXT("spectator_class"),         GET_MEMBER_NAME_CHECKED(AGameModeBase, SpectatorClass),        ASpectatorPawn::StaticClass() },
			{ TEXT("player_controller_class"), TEXT("player_controller_class"), GET_MEMBER_NAME_CHECKED(AGameModeBase, PlayerControllerClass), APlayerController::StaticClass() },
		};
	}
}

// ---------------------------------------------------------------------------
// set_game_mode_override  (AWorldSettings::DefaultGameMode)
// ---------------------------------------------------------------------------
// MCP:set_game_mode_override - Set -- or, with game_mode_class "" or "None", clear -- the GameMode override in the World Settings of the current editor world (AWorldSettings::DefaultGameMode), the GameMode this level uses instead of the project default; a cleared override falls back to the engine's URL/INI DefaultGameMode on the next world load. game_mode_class can be a native class name, a /Script/ path or a Blueprint GameMode asset path, and must derive from AGameModeBase (AGameMode too; is_multiplayer says which). The World Settings actor is modified as part of the call's undo step and the level is marked dirty but not saved: persist it with save_level. Repeating the call leaves the same state (idempotent). An unknown or non-GameMode class, or no editor world, is an error. Read it back with get_game_mode_info; the GameMode's own class slots are set with set_game_mode_classes, and a GameMode Blueprint is made with create_blueprint (parent_class "GameModeBase").
//   @param game_mode_class - AGameModeBase-derivative class: a native short name (e.g. "GameModeBase"), a /Script/ path, or a Blueprint GameMode asset path (e.g. "/Game/Core/BP_MyGameMode"). An empty string or "None" clears the override.
//   @returns { world_settings_path: string, world_name: string, default_game_mode: string, previous_default_game_mode: string, reset: bool, is_multiplayer?: bool } -- default_game_mode is "" and reset true when the override was cleared; previous_default_game_mode is the path of the override before the call ("" when there was none); is_multiplayer only when a class was set.

TSharedPtr<FJsonObject> HandleSetGameModeOverride(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ClassName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("game_mode_class"), ClassName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
	ClassName.TrimStartAndEndInline();
	const bool bClear = ClassName.IsEmpty() || ClassName.Equals(TEXT("None"), ESearchCase::IgnoreCase);
	UClass* Cls = nullptr;
	if (!bClear)
	{
		Cls = ResolveClassByName(ClassName);
		if (!Cls || !Cls->IsChildOf(AGameModeBase::StaticClass()))
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("GameMode class not found: %s"), *ClassName));
		}
	}
	AWorldSettings* WS = GetWorldSettings();
	if (!WS)
	{
		return mcp_common::MakeErrorReply(
			TEXT("No WorldSettings in current editor world"));
	}
	const FString Previous = WS->DefaultGameMode ? WS->DefaultGameMode->GetPathName() : FString();
	WS->Modify();
	WS->DefaultGameMode = Cls;
	WS->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("world_settings_path"), WS->GetPathName());
	Data->SetStringField(TEXT("world_name"), WS->GetWorld() ? WS->GetWorld()->GetName() : FString());
	Data->SetStringField(TEXT("default_game_mode"), Cls ? Cls->GetPathName() : FString());
	Data->SetStringField(TEXT("previous_default_game_mode"), Previous);
	Data->SetBoolField(TEXT("reset"), bClear);
	if (Cls)
	{
		Data->SetBoolField(TEXT("is_multiplayer"), Cls->IsChildOf(AGameMode::StaticClass()));
	}
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_game_mode_classes  (AGameModeBase CDO class slots)
// ---------------------------------------------------------------------------
// MCP:set_game_mode_classes - Write one or more class slots -- DefaultPawnClass, HUDClass, PlayerStateClass, GameStateClass, SpectatorClass, PlayerControllerClass -- on the class default object (CDO) of an AGameModeBase subclass, picked by game_mode_class_path or else game_mode_class (one is required). Pass at least one slot param; each must resolve to a class deriving from that slot's base, and every one is checked before anything is written, so a bad one writes nothing. A slot that already holds the class is left alone (idempotent). GameModes spawned later (the next PIE) use the new classes. A Blueprint GameMode's CDO is modified and its package marked dirty but not saved: persist it with save_asset (persistable true). A native GameMode class has no savable package, so its values last only until the editor closes (persistable false). Each slot written is read back from the CDO. Read every slot with get_game_mode_info; set_game_mode_override picks the GameMode of the level.
//   @param game_mode_class - Optional AGameModeBase class name (short or /Script/ path)
//   @param game_mode_class_path - Optional long /Script/Engine.GameModeBase path (takes precedence)
//   @param pawn_class - Optional APawn-derivative class for DefaultPawnClass
//   @param hud_class - Optional AHUD-derivative class for HUDClass
//   @param player_state_class - Optional APlayerState-derivative class for PlayerStateClass
//   @param game_state_class - Optional AGameStateBase-derivative class for GameStateClass
//   @param spectator_class - Optional ASpectatorPawn-derivative class for SpectatorClass
//   @param player_controller_class - Optional APlayerController-derivative class for PlayerControllerClass
//   @returns { game_mode_class: string, persistable: bool, default_pawn_class?: string, hud_class?: string, player_state_class?: string, game_state_class?: string, spectator_class?: string, player_controller_class?: string, already_set: bool, slots_written: [string] } -- one read-back field per slot that was passed; already_set is true when every requested slot already held its class (nothing written), and slots_written names the reply fields of the slots that changed.

TSharedPtr<FJsonObject> HandleSetGameModeClasses(
	const TSharedPtr<FJsonObject>& Params)
{
	UClass* Cls = nullptr;
	FString Chosen;
	if (TSharedPtr<FJsonObject> Invalid = ResolveGameModeClass(Params, Cls, Chosen))
	{
		return Invalid;
	}
	AGameModeBase* CDO = Cast<AGameModeBase>(Cls->GetDefaultObject());
	if (!CDO)
	{
		return mcp_common::MakeErrorReply(
			TEXT("Could not fetch CDO for AGameModeBase subclass"));
	}

	// Every slot param is read and checked first, so a bad one leaves the CDO untouched.
	struct FSlotWrite
	{
		FGameModeSlot Slot;
		UClass* Value = nullptr;
	};
	TArray<FSlotWrite> Writes;
	for (const FGameModeSlot& Slot : GameModeSlots())
	{
		FString ValueName;
		if (!Params->TryGetStringField(Slot.Param, ValueName) || ValueName.IsEmpty())
		{
			continue;
		}
		UClass* Value = ResolveClassByName(ValueName);
		if (!Value || !Value->IsChildOf(Slot.Base))
		{
			return mcp_common::MakeErrorReply(FString::Printf(
				TEXT("%s: class not found or not a %s: %s; nothing was written"), Slot.Param, *Slot.Base->GetName(), *ValueName));
		}
		Writes.Add({ Slot, Value });
	}
	if (Writes.Num() == 0)
	{
		return mcp_common::MakeErrorReply(
			TEXT("Pass at least one slot: pawn_class, hud_class, player_state_class, game_state_class, spectator_class or player_controller_class"));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("game_mode_class"), Chosen);
	TArray<TSharedPtr<FJsonValue>> Written;
	for (const FSlotWrite& Write : Writes)
	{
		// Idempotency: a slot that already holds this class is not written again.
		if (ReadClassPropertyOnCDO(CDO, Write.Slot.Member) != Write.Value)
		{
			if (!SetClassPropertyOnCDO(CDO, Write.Slot.Member, Write.Value))
			{
				return mcp_common::MakeErrorReply(FString::Printf(
					TEXT("Failed to set %s (CDO write)"), *Write.Slot.Member.ToString()));
			}
			Written.Add(MakeShared<FJsonValueString>(Write.Slot.Field));
		}
		const UObject* ReadBack = ReadClassPropertyOnCDO(CDO, Write.Slot.Member);
		Data->SetStringField(Write.Slot.Field, ReadBack ? ReadBack->GetPathName() : FString());
	}
	UE_LOG(LogTemp, Verbose, TEXT("[set_game_mode_classes] gm=%s slots=%d written=%d"), *Chosen, Writes.Num(), Written.Num());
	Data->SetBoolField(TEXT("already_set"), Written.Num() == 0);
	Data->SetArrayField(TEXT("slots_written"), Written);
	Data->SetBoolField(TEXT("persistable"), IsCDOPackageSavable(CDO));
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_game_mode_info
// ---------------------------------------------------------------------------
// MCP:get_game_mode_info - Read the AGameModeBase CDO's class slots (DefaultPawnClass, HUDClass, PlayerStateClass, GameStateClass, SpectatorClass, PlayerControllerClass) and the world settings' DefaultGameMode override.
//   @param game_mode_class - Optional AGameModeBase class name (short or /Script/ path)
//   @param game_mode_class_path - Optional long /Script/Engine.GameModeBase path (takes precedence)
//   @returns { game_mode_class: string, parent_class: string, is_multiplayer: bool, default_pawn_class: string, hud_class: string, player_state_class: string, game_state_class: string, spectator_class: string, player_controller_class: string, world_settings_default_game_mode?: string }

TSharedPtr<FJsonObject> HandleGetGameModeInfo(
	const TSharedPtr<FJsonObject>& Params)
{
	UClass* Cls = nullptr;
	FString Chosen;
	if (TSharedPtr<FJsonObject> Invalid = ResolveGameModeClass(Params, Cls, Chosen))
	{
		return Invalid;
	}
	const AGameModeBase* CDO = Cast<AGameModeBase>(Cls->GetDefaultObject());
	if (!CDO)
	{
		return mcp_common::MakeErrorReply(
			TEXT("Could not fetch CDO for AGameModeBase subclass"));
	}

	auto ClassNameOrEmpty = [](const UClass* C) -> FString
	{
		return C ? C->GetPathName() : FString();
	};

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("game_mode_class"), Chosen);
	Data->SetStringField(TEXT("parent_class"), Cls->GetSuperClass()
		? Cls->GetSuperClass()->GetPathName() : FString());
	Data->SetBoolField(TEXT("is_multiplayer"), Cls->IsChildOf(AGameMode::StaticClass()));
	Data->SetStringField(TEXT("default_pawn_class"), ClassNameOrEmpty(CDO->DefaultPawnClass));
	Data->SetStringField(TEXT("hud_class"),         ClassNameOrEmpty(CDO->HUDClass));
	Data->SetStringField(TEXT("player_state_class"),ClassNameOrEmpty(CDO->PlayerStateClass));
	Data->SetStringField(TEXT("game_state_class"),  ClassNameOrEmpty(CDO->GameStateClass));
	Data->SetStringField(TEXT("spectator_class"),   ClassNameOrEmpty(CDO->SpectatorClass));
	Data->SetStringField(TEXT("player_controller_class"),
		ClassNameOrEmpty(CDO->PlayerControllerClass));

	AWorldSettings* WS = GetWorldSettings();
	if (WS)
	{
		Data->SetStringField(TEXT("world_settings_default_game_mode"),
			ClassNameOrEmpty(WS->DefaultGameMode));
	}
	return mcp_common::MakeOkReply(Data);
}

// ============================================================================
// PlayerStart placement with its tag.
// ============================================================================

// MCP:place_player_start - Spawn an APlayerStart actor in the editor world at the given location with optional rotation + player-start tag. Location AND rotation are RE-READ from the actor after writing them, so a mismatch is visible instead of echoed back.
//   @param location - [X, Y, Z] world location for the spawn point.
//   @param rotation - Optional world rotation. TWO forms: the unambiguous OBJECT form {"roll":R,"pitch":P,"yaw":Y} (RECOMMENDED) or the ambiguous ARRAY form [Pitch, Yaw, Roll] -- note the array order, index 2 is ROLL, not yaw. Default identity.
//   @param player_start_tag - Optional FName for the PlayerStart.Tag UPROPERTY (e.g. "Red", "Spectator", "DM"). Used by AGameModeBase to filter start points by team.
//   @param name - Optional label for the new actor. Defaults to "PlayerStart_N".
//   @returns { actor_name, location: [x,y,z] (RE-READ from the actor), rotation: {roll,pitch,yaw} (RE-READ from the actor, axes NAMED), is_spatially_loaded: bool, location_matches_request: bool, rotation_matches_request: bool, player_start_tag, spawned: bool, already_existed: bool, requested: { location, rotation, player_start_tag, name } }
//
// WHY THIS HANDLER RE-READS.
// An echo of the `rotation` ARRAY agrees with the request by construction, so it can never
// expose a mismatch: rotation=[0,0,66.8] meant as "Yaw 66.8" is Roll=66.8 -- and a
// PlayerStart with Roll makes the player spawn TILTED. So the reported rotation is
// Actor->GetActorRotation() emitted with the axes NAMED (AddRotatorField), and the requested
// value goes under "requested".
// The write path also matters on a World Partition (OFPA) map: SetActorTransform loses
// the change silently, and the transaction only captures the external actor package when
// BOTH the actor and its RootComponent are Modify()'d first. That is why this uses
// Modify() + SetActorLocation/SetActorRotation and never SetActorTransform.
TSharedPtr<FJsonObject> HandlePlacePlayerStart(
    const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World)) { return NoWorld; }

    FVector Location = mcp_common::ReadJsonVector(Params, TEXT("location"));
    FRotator Rotation = Params->HasField(TEXT("rotation"))
        ? mcp_common::ReadJsonRotator(Params, TEXT("rotation"))
        : FRotator::ZeroRotator;

    FString NameStr;
    Params->TryGetStringField(TEXT("name"), NameStr);
    FString TagStr;
    Params->TryGetStringField(TEXT("player_start_tag"), TagStr);

    // Idempotent by name — if an actor with this label already exists at the
    // requested location, reuse it (and just update its tag/rotation).
    APlayerStart* PS = nullptr;
    if (!NameStr.IsEmpty())
    {
        for (TActorIterator<APlayerStart> It(World); It; ++It)
        {
            APlayerStart* Existing = *It;
            if (Existing && (Existing->GetName() == NameStr || Existing->GetActorLabel() == NameStr))
            {
                PS = Existing;
                break;
            }
        }
    }

    const bool bAlreadyExisted = (PS != nullptr);
    if (!PS)
    {
        FActorSpawnParameters SpawnParams;
        SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        if (!NameStr.IsEmpty())
        {
            // The reuse scan above (TActorIterator) only sees LIVE, registered actors. The engine
            // asks a different question inside SpawnActor: StaticFindObjectFast(nullptr,
            // LevelToSpawnIn, Name) -- any class, and it does NOT exclude garbage
            // (UObjectGlobals.cpp) -- and on a hit it does UE_LOG(LogSpawn, Fatal, ...) at
            // LevelActor.cpp, which KILLS THE PROCESS. A deleted actor keeps its name alive:
            // the delete is transacted, so the undo buffer holds the corpse.
            // Measured as a real crash on the sibling path (spawn_actor):
            //   Saved/Crashes/UECC-Windows-6A213E624F9B48DDA7537FA87332A631_0001.
            // Here we do NOT refuse -- place_player_start is idempotent-by-name and the caller-visible
            // identity is the LABEL (set below, and the reuse scan matches on it). So if the
            // internal name is taken by a corpse we let the engine pick a unique one and still
            // apply the requested label: the operation keeps working.
            const FName DesiredName(*NameStr);
            if (StaticFindObjectFast(nullptr, World->GetCurrentLevel(), DesiredName) == nullptr)
            {
                SpawnParams.Name = DesiredName;
            }
        }
        // Belt and braces: if the check above ever misses a case, make the engine RETURN NULL and
        // log an Error -- never kill the process. The default NameMode is Required_Fatal.
        SpawnParams.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Required_ErrorAndReturnNull;
        PS = World->SpawnActor<APlayerStart>(APlayerStart::StaticClass(), Location, Rotation, SpawnParams);
        if (!PS)
        {
            return mcp_common::MakeErrorReply(
                TEXT("Failed to spawn APlayerStart"));
        }
        if (!NameStr.IsEmpty())
        {
            PS->SetActorLabel(NameStr);
        }
    }

    // OFPA-safe write path. Modify() on the actor AND on its RootComponent
    // before touching the transform (the external actor package is only captured
    // when both are dirtied), then SetActorLocation/SetActorRotation -- NEVER
    // SetActorTransform, which loses OFPA changes silently. Applied to the
    // freshly-spawned actor too: SpawnActor already placed it, so these are
    // no-ops there, but running the same path in both branches means there is
    // exactly ONE way place_player_start writes a transform.
    PS->Modify();
    if (USceneComponent* Root = PS->GetRootComponent())
    {
        Root->Modify();
    }
    PS->SetActorLocation(Location);
    PS->SetActorRotation(Rotation);

    if (!TagStr.IsEmpty())
    {
        PS->PlayerStartTag = FName(*TagStr);
    }

    PS->MarkPackageDirty();

    // Everything below is a RE-READ of the actor, never a copy of the input.
    const FVector  ActualLocation = PS->GetActorLocation();
    const FRotator ActualRotation = PS->GetActorRotation();
    // 0.1 uu on position and 0.01 deg on angle: enough to absorb the float/double
    // round-trip through FTransform, far below anything that matters for a spawn
    // point, and small enough that a Yaw landing in Roll can never slip through.
    const bool bLocationOk = ActualLocation.Equals(Location, 0.1);
    const bool bRotationOk = ActualRotation.Equals(Rotation, 0.01f);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("actor_name"), PS->GetName());
    {
        Data->SetArrayField(TEXT("location"), mcp_common::VectorToJsonArray(ActualLocation));
    }
    // Named axes, not a 3-number array: the array form is what let a Yaw request
    // come back looking confirmed while the actor sat at Roll.
    mcp_common::AddRotatorField(Data, TEXT("rotation"), ActualRotation);
    Data->SetBoolField(TEXT("location_matches_request"), bLocationOk);
    Data->SetBoolField(TEXT("rotation_matches_request"), bRotationOk);
    // World Partition: a PlayerStart you cannot stream in is a PlayerStart that
    // does not exist at spawn time, so report what the actor actually says.
    Data->SetBoolField(TEXT("is_spatially_loaded"), PS->GetIsSpatiallyLoaded());
    Data->SetStringField(TEXT("player_start_tag"), PS->PlayerStartTag.ToString());
    Data->SetBoolField(TEXT("spawned"), !bAlreadyExisted);
    Data->SetBoolField(TEXT("already_existed"), bAlreadyExisted);

    mcp_common::AddRequestedEcho(Data, Params,
        { TEXT("location"), TEXT("rotation"), TEXT("player_start_tag"), TEXT("name") });

    FString Error;
    if (!bRotationOk)
    {
        Error = FString::Printf(
            TEXT("Rotation read back from the actor (roll=%.3f pitch=%.3f yaw=%.3f) does not match the requested (roll=%.3f pitch=%.3f yaw=%.3f). If you sent the ARRAY form, remember index 2 is ROLL: send {\"roll\":..,\"pitch\":..,\"yaw\":..} instead."),
            ActualRotation.Roll, ActualRotation.Pitch, ActualRotation.Yaw,
            Rotation.Roll, Rotation.Pitch, Rotation.Yaw);
    }
    else if (!bLocationOk)
    {
        Error = FString::Printf(
            TEXT("Location read back from the actor (%s) does not match the requested (%s)."),
            *ActualLocation.ToString(), *Location.ToString());
    }
    return mcp_common::MakeOutcomeReply(Data, bLocationOk && bRotationOk, Error);
}

// MCP:register_spawn_point - Mark an existing actor as a registered spawn point for the project (sets PlayerStartTag + ensures it's a APlayerStart). Useful for tagging generic actors as respawn anchors in lieu of placing APlayerStarts.
//   @param actor_name - Name or label of the actor to mark as a spawn point.
//   @param player_start_tag - Optional FName tag (e.g. "Red"/"Blue" for team filtering; "Respawn" for general spawn anchors).
//   @returns { actor_name, was_player_start, promoted_to_player_start: bool, player_start_tag }
TSharedPtr<FJsonObject> HandleRegisterSpawnPoint(
    const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World)) { return NoWorld; }
    FString ActorName, TagStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName)) { return Missing; }
    Params->TryGetStringField(TEXT("player_start_tag"), TagStr);

    AActor* Actor = mcp_common::FindActorByName(World, ActorName);
    if (!Actor)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Actor '%s' not found in editor world"), *ActorName));
    }

    bool bPromoted = false;
    APlayerStart* PS = Cast<APlayerStart>(Actor);
    if (!PS)
    {
        // Don't promote in place (UE doesn't support changing a runtime class).
        // Honest error: actor must already be a PlayerStart (use place_player_start
        // to spawn a fresh one).
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Actor '%s' is not a APlayerStart. Use place_player_start to spawn one."), *ActorName));
    }

    if (!TagStr.IsEmpty())
    {
        PS->PlayerStartTag = FName(*TagStr);
    }
    PS->MarkPackageDirty();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("actor_name"), PS->GetName());
    Data->SetBoolField(TEXT("was_player_start"), true);
    Data->SetBoolField(TEXT("promoted_to_player_start"), bPromoted);
    Data->SetStringField(TEXT("player_start_tag"), PS->PlayerStartTag.ToString());
    return mcp_common::MakeOkReply(Data);
}

const McpCommandFamilies::FRegistrar Registration =
{
	{ TEXT("get_game_mode_info"), &HandleGetGameModeInfo },
	{ TEXT("place_player_start"), &HandlePlacePlayerStart },
	{ TEXT("register_spawn_point"), &HandleRegisterSpawnPoint },
	{ TEXT("set_game_mode_classes"), &HandleSetGameModeClasses },
	{ TEXT("set_game_mode_override"), &HandleSetGameModeOverride },
};
} // namespace UEAgentGameFrameworkCommands
