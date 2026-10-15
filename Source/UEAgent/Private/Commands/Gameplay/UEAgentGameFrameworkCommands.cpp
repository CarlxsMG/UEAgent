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

	/** What a create_*_class_bp command asks for: the asset name, its folder, and the resolved parent class. */
	struct FGameFrameworkBpRequest
	{
		FString AssetName;
		FString PackagePath;
		UClass* ParentClass = nullptr;
	};

	/**
	 * asset_name, parent_class and package_path (default: the plugin's asset folder) of a create_*_class_bp
	 * command, with the parent class resolved. Returns the error reply to send back, or nullptr once Out is filled.
	 */
	TSharedPtr<FJsonObject> GameFramework_ReadBpRequest(const TSharedPtr<FJsonObject>& Params, FGameFrameworkBpRequest& Out)
	{
		FString ParentClassName;
		if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_name"), Out.AssetName)) { return Missing; }
		if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("parent_class"), ParentClassName)) { return Missing; }
		Params->TryGetStringField(TEXT("package_path"), Out.PackagePath);
		if (Out.PackagePath.IsEmpty())
		{
			Out.PackagePath = mcp_common::DefaultAssetFolder();
		}

		Out.ParentClass = ResolveClassByName(ParentClassName);
		if (!Out.ParentClass)
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("Parent class not found: %s"), *ParentClassName));
		}
		return nullptr;
	}

	/**
	 * Why the Blueprint BP already at PackagePath/AssetName cannot be returned for a request for a subclass of
	 * ParentClass, or empty when it can: a re-run returns that Blueprint only when its parent is ParentClass or
	 * derives from it. Anything else is another asset, which the caller leaves untouched.
	 */
	FString ExistingBlueprintParentMismatch(const UBlueprint* BP, const UClass* ParentClass,
		const FString& PackagePath, const FString& AssetName)
	{
		if (BP->ParentClass && BP->ParentClass->IsChildOf(ParentClass))
		{
			return FString();
		}
		return FString::Printf(
			TEXT("A Blueprint already at %s/%s has parent %s, which does not derive from parent_class %s; ")
			TEXT("nothing was created or changed. Pick another asset_name, or delete that asset first."),
			*PackagePath, *AssetName,
			BP->ParentClass ? *BP->ParentClass->GetName() : TEXT("none"), *ParentClass->GetName());
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

	/** The path of the class held by the TSubclassOf<...> UPROPERTY PropName of CDO, read back; empty when unset. */
	FString ReadClassPropertyOnCDO(const UObject* CDO, FName PropName)
	{
		const FObjectPropertyBase* ObjProp = FindClassPropertyOnCDO(CDO, PropName);
		const UObject* Value = ObjProp ? ObjProp->GetObjectPropertyValue_InContainer(CDO) : nullptr;
		return Value ? Value->GetPathName() : FString();
	}

	/** The reply of a GameMode class-slot setter: game_mode_class, the slot read back under SlotField, persistable. */
	TSharedPtr<FJsonObject> GameModeSlotReply(const FString& GameModeClass, const UObject* CDO, FName PropName, const TCHAR* SlotField)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("game_mode_class"), GameModeClass);
		Data->SetStringField(SlotField, ReadClassPropertyOnCDO(CDO, PropName));
		Data->SetBoolField(TEXT("persistable"), IsCDOPackageSavable(CDO));
		return Data;
	}
}

// ---------------------------------------------------------------------------
// create_game_mode_class_bp
// ---------------------------------------------------------------------------
// MCP:create_game_mode_class_bp - Create an AGameModeBase-derivative Blueprint asset at package_path/asset_name, registered and marked dirty but not saved (persist it with save_asset). Idempotent on re-run: a Blueprint already at that path whose parent derives from parent_class is returned with already_exists=true; one with another parent is an error and is left untouched. parent_class and is_multiplayer are read off the Blueprint.
//   @param asset_name - Name for the new Blueprint asset
//   @param parent_class - Parent class name (must derive from AGameModeBase; AGameMode also accepted for multiplayer)
//   @param package_path - Target content folder; defaults to /Game/MCP
//   @returns { asset_name: string, asset_path: string, package_path: string, package_class: string, parent_class: string, is_multiplayer: bool, already_exists: bool }

TSharedPtr<FJsonObject> HandleCreateGameModeClassBp(
	const TSharedPtr<FJsonObject>& Params)
{
	FGameFrameworkBpRequest Request;
	if (TSharedPtr<FJsonObject> Invalid = GameFramework_ReadBpRequest(Params, Request))
	{
		return Invalid;
	}
	if (!Request.ParentClass->IsChildOf(AGameModeBase::StaticClass()))
	{
		return mcp_common::MakeErrorReply(
			TEXT("parent_class must be an AGameModeBase derivative (or AGameMode)"));
	}

	FString PackageError;
	UPackage* Package = mcp_common::CreateAssetPackage(Request.PackagePath / Request.AssetName, Request.AssetName, UBlueprint::StaticClass(), PackageError);
	if (!Package)
	{
	    return mcp_common::MakeErrorReply(PackageError);
	}
	Package->FullyLoad();

	// FKismetEditorUtilities::CreateBlueprint ASSERTS (Kismet2.cpp) if a
	// UBlueprint of this name still lives in the package — which happens on a
	// re-run because in-editor delete_asset does not reliably unload a loaded
	// Blueprint. Return the existing asset idempotently instead of crashing.
	UBlueprint* BP = FindObject<UBlueprint>(Package, *Request.AssetName);
	const bool bAlreadyExists = (BP != nullptr);
	if (BP)
	{
		const FString Mismatch = ExistingBlueprintParentMismatch(BP, Request.ParentClass, Request.PackagePath, Request.AssetName);
		if (!Mismatch.IsEmpty())
		{
			return mcp_common::MakeErrorReply(Mismatch);
		}
	}
	else
	{
		BP = Cast<UBlueprint>(
			FKismetEditorUtilities::CreateBlueprint(
				Request.ParentClass, Package, FName(*Request.AssetName), BPTYPE_Normal,
				FName(TEXT("CreateGameModeBP"))));
		if (!BP)
		{
			return mcp_common::MakeErrorReply(
				TEXT("Failed to create GameMode Blueprint"));
		}
		FAssetRegistryModule::AssetCreated(BP);
		BP->MarkPackageDirty();
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_name"), BP->GetName());
	Data->SetStringField(TEXT("asset_path"), BP->GetPathName());
	Data->SetStringField(TEXT("package_path"), Request.PackagePath);
	Data->SetStringField(TEXT("package_class"),
		BP->GeneratedClass ? BP->GeneratedClass->GetPathName() : FString());
	// Read off the asset, so a Blueprint that already existed reports the parent it really has.
	const UClass* ActualParent = BP->ParentClass;
	Data->SetStringField(TEXT("parent_class"), ActualParent ? ActualParent->GetName() : FString());
	Data->SetBoolField(TEXT("is_multiplayer"), ActualParent && ActualParent->IsChildOf(AGameMode::StaticClass()));
	Data->SetBoolField(TEXT("already_exists"), bAlreadyExists);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_game_mode_override  (AWorldSettings::DefaultGameMode)
// ---------------------------------------------------------------------------
// MCP:set_game_mode_override - Set the AWorldSettings::DefaultGameMode override on the current editor world.
//   @param game_mode_class - AGameModeBase-derivative class name (short or /Script/ path)
//   @returns { world_settings_path: string, default_game_mode: string, is_multiplayer: bool }

TSharedPtr<FJsonObject> HandleSetGameModeOverride(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ClassName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("game_mode_class"), ClassName)) { return Missing; }
	UClass* Cls = ResolveClassByName(ClassName);
	if (!Cls || !Cls->IsChildOf(AGameModeBase::StaticClass()))
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("GameMode class not found: %s"), *ClassName));
	}
	AWorldSettings* WS = GetWorldSettings();
	if (!WS)
	{
		return mcp_common::MakeErrorReply(
			TEXT("No WorldSettings in current editor world"));
	}
	WS->Modify();
	WS->DefaultGameMode = Cls;
	WS->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("world_settings_path"), WS->GetPathName());
	Data->SetStringField(TEXT("default_game_mode"), Cls->GetPathName());
	Data->SetBoolField(TEXT("is_multiplayer"), Cls->IsChildOf(AGameMode::StaticClass()));
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_game_mode_default_pawn  (AGameModeBase CDO :: DefaultPawnClass)
// ---------------------------------------------------------------------------
// MCP:set_game_mode_default_pawn - Write DefaultPawnClass on the class default object (CDO) of an AGameModeBase subclass, picked by game_mode_class_path or else game_mode_class (one is required), to pawn_class, which must derive from APawn; a class that does not resolve or derive is an error. GameModes spawned later (the next PIE) use it. A Blueprint GameMode's CDO is modified and its package marked dirty but not saved: persist it with save_asset (persistable true). A native GameMode class has no savable package, so its value lasts only until the editor closes (persistable false). default_pawn_class is read back from the CDO. Read the slots with get_game_mode_info; set_game_mode_override picks the GameMode of the level.
//   @param game_mode_class - Optional AGameModeBase class name (short or /Script/ path)
//   @param game_mode_class_path - Optional long /Script/Engine.GameModeBase path (takes precedence)
//   @param pawn_class - APawn-derivative class name
//   @returns { game_mode_class: string, default_pawn_class: string, persistable: bool }

TSharedPtr<FJsonObject> HandleSetGameModeDefaultPawn(
	const TSharedPtr<FJsonObject>& Params)
{
	UClass* Cls = nullptr;
	FString Chosen;
	if (TSharedPtr<FJsonObject> Invalid = ResolveGameModeClass(Params, Cls, Chosen))
	{
		return Invalid;
	}
	FString PawnClassName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("pawn_class"), PawnClassName)) { return Missing; }
	UClass* PawnCls = ResolveClassByName(PawnClassName);
	if (!PawnCls || !PawnCls->IsChildOf(APawn::StaticClass()))
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Pawn class not found or not an APawn: %s"), *PawnClassName));
	}
	UObject* CDO = Cls->GetDefaultObject();
	const FName Slot = GET_MEMBER_NAME_CHECKED(AGameModeBase, DefaultPawnClass);
	if (!SetClassPropertyOnCDO(CDO, Slot, PawnCls))
	{
		return mcp_common::MakeErrorReply(
			TEXT("Failed to set DefaultPawnClass (CDO write)"));
	}
	return mcp_common::MakeOkReply(GameModeSlotReply(Chosen, CDO, Slot, TEXT("default_pawn_class")));
}

// ---------------------------------------------------------------------------
// set_game_mode_hud_class  (AGameModeBase CDO :: HUDClass)
// ---------------------------------------------------------------------------
// MCP:set_game_mode_hud_class - Set the AGameModeBase::HUDClass UPROPERTY on the GameMode's CDO. A Blueprint GameMode's package is marked dirty but not saved (persist it with save_asset); a native class's value lasts only until the editor closes (persistable false). hud_class is read back from the CDO.
//   @param game_mode_class - Optional AGameModeBase class name (short or /Script/ path)
//   @param game_mode_class_path - Optional long /Script/Engine.GameModeBase path (takes precedence)
//   @param hud_class - AHUD-derivative class name
//   @returns { game_mode_class: string, hud_class: string, persistable: bool }

TSharedPtr<FJsonObject> HandleSetGameModeHudClass(
	const TSharedPtr<FJsonObject>& Params)
{
	UClass* Cls = nullptr;
	FString Chosen;
	if (TSharedPtr<FJsonObject> Invalid = ResolveGameModeClass(Params, Cls, Chosen))
	{
		return Invalid;
	}
	FString HudClassName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("hud_class"), HudClassName)) { return Missing; }
	UClass* HudCls = ResolveClassByName(HudClassName);
	if (!HudCls || !HudCls->IsChildOf(AHUD::StaticClass()))
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("HUD class not found or not an AHUD: %s"), *HudClassName));
	}
	UObject* CDO = Cls->GetDefaultObject();
	const FName Slot = GET_MEMBER_NAME_CHECKED(AGameModeBase, HUDClass);
	if (!SetClassPropertyOnCDO(CDO, Slot, HudCls))
	{
		return mcp_common::MakeErrorReply(
			TEXT("Failed to set HUDClass (CDO write)"));
	}
	return mcp_common::MakeOkReply(GameModeSlotReply(Chosen, CDO, Slot, TEXT("hud_class")));
}

// ---------------------------------------------------------------------------
// set_game_mode_player_state_class
// ---------------------------------------------------------------------------
// MCP:set_game_mode_player_state_class - Set the AGameModeBase::PlayerStateClass UPROPERTY on the GameMode's CDO. A Blueprint GameMode's package is marked dirty but not saved (persist it with save_asset); a native class's value lasts only until the editor closes (persistable false). player_state_class is read back from the CDO.
//   @param game_mode_class - Optional AGameModeBase class name (short or /Script/ path)
//   @param game_mode_class_path - Optional long /Script/Engine.GameModeBase path (takes precedence)
//   @param player_state_class - APlayerState-derivative class name
//   @returns { game_mode_class: string, player_state_class: string, persistable: bool }

TSharedPtr<FJsonObject> HandleSetGameModePlayerStateClass(
	const TSharedPtr<FJsonObject>& Params)
{
	UClass* Cls = nullptr;
	FString Chosen;
	if (TSharedPtr<FJsonObject> Invalid = ResolveGameModeClass(Params, Cls, Chosen))
	{
		return Invalid;
	}
	FString PsClassName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("player_state_class"), PsClassName)) { return Missing; }
	UClass* PsCls = ResolveClassByName(PsClassName);
	if (!PsCls || !PsCls->IsChildOf(APlayerState::StaticClass()))
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("PlayerState class not found or not an APlayerState: %s"), *PsClassName));
	}
	UObject* CDO = Cls->GetDefaultObject();
	const FName Slot = GET_MEMBER_NAME_CHECKED(AGameModeBase, PlayerStateClass);
	if (!SetClassPropertyOnCDO(CDO, Slot, PsCls))
	{
		return mcp_common::MakeErrorReply(
			TEXT("Failed to set PlayerStateClass (CDO write)"));
	}
	return mcp_common::MakeOkReply(GameModeSlotReply(Chosen, CDO, Slot, TEXT("player_state_class")));
}

// ---------------------------------------------------------------------------
// set_game_mode_game_state_class  (AGameModeBase CDO :: GameStateClass)
// ---------------------------------------------------------------------------
// MCP:set_game_mode_game_state_class - Set the AGameModeBase::GameStateClass UPROPERTY on the GameMode's CDO. A Blueprint GameMode's package is marked dirty but not saved (persist it with save_asset); a native class's value lasts only until the editor closes (persistable false). game_state_class is read back from the CDO.
//   @param game_mode_class - Optional AGameModeBase class name (short or /Script/ path)
//   @param game_mode_class_path - Optional long /Script/Engine.GameModeBase path (takes precedence)
//   @param game_state_class - AGameStateBase-derivative class name (NoClear: empty/None rejected)
//   @returns { game_mode_class: string, game_state_class: string, already_set: bool, persistable: bool }

TSharedPtr<FJsonObject> HandleSetGameModeGameStateClass(
	const TSharedPtr<FJsonObject>& Params)
{
	UClass* Cls = nullptr;
	FString Chosen;
	if (TSharedPtr<FJsonObject> Invalid = ResolveGameModeClass(Params, Cls, Chosen))
	{
		return Invalid;
	}
	FString GsClassName;
	// NoClear UPROPERTY: reject an empty/None value class rather than clearing to null.
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("game_state_class"), GsClassName)) { return Missing; }
	UClass* GsCls = ResolveClassByName(GsClassName);
	if (!GsCls || !GsCls->IsChildOf(AGameStateBase::StaticClass()))
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("GameState class not found or not an AGameStateBase: %s"), *GsClassName));
	}
	AGameModeBase* CDO = Cast<AGameModeBase>(Cls->GetDefaultObject());
	if (!CDO)
	{
		return mcp_common::MakeErrorReply(
			TEXT("Could not fetch CDO for AGameModeBase subclass"));
	}
	// Idempotency: if the CDO already holds this class, report already_set and skip the write.
	const bool bAlreadySet = (CDO->GameStateClass == GsCls);
	UE_LOG(LogTemp, Verbose,
		TEXT("[set_game_mode_game_state_class] gm=%s prev=%s new=%s already_set=%d"),
		*Chosen,
		CDO->GameStateClass ? *CDO->GameStateClass->GetPathName() : TEXT("<null>"),
		*GsCls->GetPathName(), bAlreadySet ? 1 : 0);
	const FName Slot = GET_MEMBER_NAME_CHECKED(AGameModeBase, GameStateClass);
	if (!bAlreadySet)
	{
		if (!SetClassPropertyOnCDO(CDO, Slot, GsCls))
		{
			return mcp_common::MakeErrorReply(
				TEXT("Failed to set GameStateClass (CDO write)"));
		}
	}
	TSharedPtr<FJsonObject> Data = GameModeSlotReply(Chosen, CDO, Slot, TEXT("game_state_class"));
	Data->SetBoolField(TEXT("already_set"), bAlreadySet);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_game_mode_spectator_class  (AGameModeBase CDO :: SpectatorClass)
// ---------------------------------------------------------------------------
// MCP:set_game_mode_spectator_class - Set the AGameModeBase::SpectatorClass UPROPERTY on the GameMode's CDO. A Blueprint GameMode's package is marked dirty but not saved (persist it with save_asset); a native class's value lasts only until the editor closes (persistable false). spectator_class is read back from the CDO.
//   @param game_mode_class - Optional AGameModeBase class name (short or /Script/ path)
//   @param game_mode_class_path - Optional long /Script/Engine.GameModeBase path (takes precedence)
//   @param spectator_class - ASpectatorPawn-derivative class name (NoClear: empty/None rejected)
//   @returns { game_mode_class: string, spectator_class: string, already_set: bool, persistable: bool }

TSharedPtr<FJsonObject> HandleSetGameModeSpectatorClass(
	const TSharedPtr<FJsonObject>& Params)
{
	UClass* Cls = nullptr;
	FString Chosen;
	if (TSharedPtr<FJsonObject> Invalid = ResolveGameModeClass(Params, Cls, Chosen))
	{
		return Invalid;
	}
	FString SpClassName;
	// NoClear UPROPERTY: reject an empty/None value class rather than clearing to null.
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("spectator_class"), SpClassName)) { return Missing; }
	UClass* SpCls = ResolveClassByName(SpClassName);
	if (!SpCls || !SpCls->IsChildOf(ASpectatorPawn::StaticClass()))
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Spectator class not found or not an ASpectatorPawn: %s"), *SpClassName));
	}
	AGameModeBase* CDO = Cast<AGameModeBase>(Cls->GetDefaultObject());
	if (!CDO)
	{
		return mcp_common::MakeErrorReply(
			TEXT("Could not fetch CDO for AGameModeBase subclass"));
	}
	// Idempotency: if the CDO already holds this class, report already_set and skip the write.
	const bool bAlreadySet = (CDO->SpectatorClass == SpCls);
	UE_LOG(LogTemp, Verbose,
		TEXT("[set_game_mode_spectator_class] gm=%s prev=%s new=%s already_set=%d"),
		*Chosen,
		CDO->SpectatorClass ? *CDO->SpectatorClass->GetPathName() : TEXT("<null>"),
		*SpCls->GetPathName(), bAlreadySet ? 1 : 0);
	const FName Slot = GET_MEMBER_NAME_CHECKED(AGameModeBase, SpectatorClass);
	if (!bAlreadySet)
	{
		if (!SetClassPropertyOnCDO(CDO, Slot, SpCls))
		{
			return mcp_common::MakeErrorReply(
				TEXT("Failed to set SpectatorClass (CDO write)"));
		}
	}
	TSharedPtr<FJsonObject> Data = GameModeSlotReply(Chosen, CDO, Slot, TEXT("spectator_class"));
	Data->SetBoolField(TEXT("already_set"), bAlreadySet);
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

// ---------------------------------------------------------------------------
// Clear override + world settings introspection.
// ---------------------------------------------------------------------------

// MCP:reset_game_mode_override - Inverse of set_game_mode_override: clears the per-world GameMode override (AWorldSettings::DefaultGameMode) back to null, so the world falls back to the engine's URL/INI DefaultGameMode. Takes no parameters and acts on the current editor world.
//   @returns { reset, world_name, previous_default_game_mode }

TSharedPtr<FJsonObject> HandleResetGameModeOverride(
    const TSharedPtr<FJsonObject>& Params)
{
    AWorldSettings* WS = GetWorldSettings();
    if (!WS)
    {
        return mcp_common::MakeErrorReply(TEXT("No editor world available"));
    }
    FString PrevClass;
    if (WS->DefaultGameMode)
    {
        PrevClass = WS->DefaultGameMode->GetName();
    }
    WS->Modify();
    WS->DefaultGameMode = nullptr;
    WS->MarkPackageDirty();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("reset"), true);
    Data->SetStringField(TEXT("world_name"), WS->GetWorld() ? WS->GetWorld()->GetName() : TEXT(""));
    Data->SetStringField(TEXT("previous_default_game_mode"), PrevClass);
    Data->SetStringField(TEXT("note"),
        TEXT("WorldSettings.DefaultGameMode cleared; the engine URL/INI DefaultGameMode will apply on next world load"));
    return mcp_common::MakeOkReply(Data);
}

// ============================================================================
// Framework scaffolds: Blueprint subclasses of the framework classes besides the
// GameMode (AGameStateBase / APlayerController / APlayerState / UGameInstance /
// AHUD), and PlayerStart placement with its tag.
// ============================================================================

namespace
{
    /**
     * The Blueprint a create_*_class_bp request names, of a RequiredBase subclass: the one already in the
     * package on a re-run, otherwise a new one, registered and marked dirty. Null, with OutError, when the
     * parent class does not derive from RequiredBase or the package cannot hold the asset.
     */
    UBlueprint* CreateClassBlueprint(const FGameFrameworkBpRequest& Request, UClass* RequiredBase,
        bool& bOutAlreadyExisted, FString& OutError)
    {
        bOutAlreadyExisted = false;
        const FString& AssetName = Request.AssetName;
        const FString& PackagePath = Request.PackagePath;
        UClass* ParentClass = Request.ParentClass;
        if (!ParentClass) { OutError = TEXT("parent_class is required"); return nullptr; }
        if (!ParentClass->IsChildOf(RequiredBase))
        {
            OutError = FString::Printf(
                TEXT("parent_class must be a %s derivative"), *RequiredBase->GetName());
            return nullptr;
        }

        OutError = mcp_common::AssetNameConflict(PackagePath / AssetName, AssetName, UBlueprint::StaticClass());
        if (!OutError.IsEmpty()) { return nullptr; }
        UPackage* Package = CreatePackage(*(PackagePath / AssetName));
        if (!Package) { OutError = TEXT("Failed to create package"); return nullptr; }
        Package->FullyLoad();

        // Idempotency: FKismetEditorUtilities::CreateBlueprint assert()-crashes if
        // the BP already exists in the package. Detect first, same pattern as
        // create_game_mode_class_bp.
        UBlueprint* BP = FindObject<UBlueprint>(Package, *AssetName);
        if (BP)
        {
            OutError = ExistingBlueprintParentMismatch(BP, ParentClass, PackagePath, AssetName);
            if (!OutError.IsEmpty())
            {
                return nullptr;
            }
            bOutAlreadyExisted = true;
            return BP;
        }

        BP = Cast<UBlueprint>(
            FKismetEditorUtilities::CreateBlueprint(
                ParentClass, Package, FName(*AssetName), BPTYPE_Normal,
                FName(*FString::Printf(TEXT("Create%s"), *RequiredBase->GetName()))));
        if (!BP) { OutError = TEXT("Failed to create Blueprint"); return nullptr; }
        FAssetRegistryModule::AssetCreated(BP);
        BP->MarkPackageDirty();
        return BP;
    }

    // Helper for create_*_class_bp responses. parent_class is the Blueprint's own parent, read off the asset,
    // so a Blueprint that already existed reports the parent it really has.
    TSharedPtr<FJsonObject> ClassBpPayload(UBlueprint* BP, bool bAlreadyExisted)
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("asset_name"), BP->GetName());
        Data->SetStringField(TEXT("asset_path"), BP->GetPathName());
        Data->SetStringField(TEXT("package_class"),
            BP->GeneratedClass ? BP->GeneratedClass->GetPathName() : FString());
        Data->SetStringField(TEXT("parent_class"), BP->ParentClass ? BP->ParentClass->GetName() : FString());
        Data->SetBoolField(TEXT("already_exists"), bAlreadyExisted);
        return Data;
    }
}

// MCP:create_game_state_class_bp - Create a Blueprint subclass of AGameStateBase (or AGameState) at package_path/asset_name with FKismetEditorUtilities::CreateBlueprint, registered and marked dirty but NOT saved -- persist it with save_asset. Idempotent on re-run: a Blueprint already at that path whose parent derives from parent_class is returned as is with already_exists=true; one with another parent is an error and is left untouched. parent_class is read off the Blueprint. A parent_class that does not resolve or derive from AGameStateBase, or a name taken by another asset class, is an error. Refused during PIE unless force. Siblings: create_game_mode_class_bp, create_player_state_class_bp.
//   @param asset_name - Name for the new Blueprint asset.
//   @param parent_class - Parent class name; must derive from AGameStateBase.
//   @param package_path - Target content folder; defaults to /Game/MCP.
//   @returns { asset_name, asset_path, package_class, parent_class, already_exists }
TSharedPtr<FJsonObject> HandleCreateGameStateClassBp(
    const TSharedPtr<FJsonObject>& Params)
{
    FGameFrameworkBpRequest Request;
    if (TSharedPtr<FJsonObject> Invalid = GameFramework_ReadBpRequest(Params, Request))
    {
        return Invalid;
    }
    FString Err;
    bool bAlreadyExisted = false;
    UBlueprint* BP = CreateClassBlueprint(Request, AGameStateBase::StaticClass(), bAlreadyExisted, Err);
    if (!BP) { return mcp_common::MakeErrorReply(Err); }

    TSharedPtr<FJsonObject> Data = ClassBpPayload(BP, bAlreadyExisted);
    return mcp_common::MakeOkReply(Data);
}

// MCP:create_player_controller_class_bp - Create a Blueprint subclass of APlayerController, registered and marked dirty but not saved (persist it with save_asset). Idempotent on re-run: a Blueprint already at that path whose parent derives from parent_class is returned with already_exists=true; one with another parent is an error and is left untouched. parent_class is read off the Blueprint.
//   @param asset_name - Name for the new Blueprint asset.
//   @param parent_class - Parent class name; must derive from APlayerController.
//   @param package_path - Target content folder; defaults to /Game/MCP.
//   @returns { asset_name, asset_path, package_class, parent_class, already_exists }
TSharedPtr<FJsonObject> HandleCreatePlayerControllerClassBp(
    const TSharedPtr<FJsonObject>& Params)
{
    FGameFrameworkBpRequest Request;
    if (TSharedPtr<FJsonObject> Invalid = GameFramework_ReadBpRequest(Params, Request))
    {
        return Invalid;
    }
    FString Err;
    bool bAlreadyExisted = false;
    UBlueprint* BP = CreateClassBlueprint(Request, APlayerController::StaticClass(), bAlreadyExisted, Err);
    if (!BP) { return mcp_common::MakeErrorReply(Err); }

    TSharedPtr<FJsonObject> Data = ClassBpPayload(BP, bAlreadyExisted);
    return mcp_common::MakeOkReply(Data);
}

// MCP:create_player_state_class_bp - Create a Blueprint subclass of APlayerState, registered and marked dirty but not saved (persist it with save_asset). Idempotent on re-run: a Blueprint already at that path whose parent derives from parent_class is returned with already_exists=true; one with another parent is an error and is left untouched. parent_class is read off the Blueprint.
//   @param asset_name - Name for the new Blueprint asset.
//   @param parent_class - Parent class name; must derive from APlayerState.
//   @param package_path - Target content folder; defaults to /Game/MCP.
//   @returns { asset_name, asset_path, package_class, parent_class, already_exists }
TSharedPtr<FJsonObject> HandleCreatePlayerStateClassBp(
    const TSharedPtr<FJsonObject>& Params)
{
    FGameFrameworkBpRequest Request;
    if (TSharedPtr<FJsonObject> Invalid = GameFramework_ReadBpRequest(Params, Request))
    {
        return Invalid;
    }
    FString Err;
    bool bAlreadyExisted = false;
    UBlueprint* BP = CreateClassBlueprint(Request, APlayerState::StaticClass(), bAlreadyExisted, Err);
    if (!BP) { return mcp_common::MakeErrorReply(Err); }

    TSharedPtr<FJsonObject> Data = ClassBpPayload(BP, bAlreadyExisted);
    return mcp_common::MakeOkReply(Data);
}

// MCP:create_game_instance_class_bp - Create a Blueprint subclass of UGameInstance, registered and marked dirty but not saved (persist it with save_asset). Idempotent on re-run: a Blueprint already at that path whose parent derives from parent_class is returned with already_exists=true; one with another parent is an error and is left untouched. parent_class is read off the Blueprint.
//   @param asset_name - Name for the new Blueprint asset.
//   @param parent_class - Parent class name; must derive from UGameInstance.
//   @param package_path - Target content folder; defaults to /Game/MCP.
//   @returns { asset_name, asset_path, package_class, parent_class, already_exists }
TSharedPtr<FJsonObject> HandleCreateGameInstanceClassBp(
    const TSharedPtr<FJsonObject>& Params)
{
    FGameFrameworkBpRequest Request;
    if (TSharedPtr<FJsonObject> Invalid = GameFramework_ReadBpRequest(Params, Request))
    {
        return Invalid;
    }
    FString Err;
    bool bAlreadyExisted = false;
    UBlueprint* BP = CreateClassBlueprint(Request, UGameInstance::StaticClass(), bAlreadyExisted, Err);
    if (!BP) { return mcp_common::MakeErrorReply(Err); }

    TSharedPtr<FJsonObject> Data = ClassBpPayload(BP, bAlreadyExisted);
    return mcp_common::MakeOkReply(Data);
}

// MCP:create_hud_class_bp - Create a Blueprint subclass of AHUD, registered and marked dirty but not saved (persist it with save_asset). Idempotent on re-run: a Blueprint already at that path whose parent derives from parent_class is returned with already_exists=true; one with another parent is an error and is left untouched. parent_class is read off the Blueprint.
//   @param asset_name - Name for the new Blueprint asset.
//   @param parent_class - Parent class name; must derive from AHUD.
//   @param package_path - Target content folder; defaults to /Game/MCP.
//   @returns { asset_name, asset_path, package_class, parent_class, already_exists }
TSharedPtr<FJsonObject> HandleCreateHudClassBp(
    const TSharedPtr<FJsonObject>& Params)
{
    FGameFrameworkBpRequest Request;
    if (TSharedPtr<FJsonObject> Invalid = GameFramework_ReadBpRequest(Params, Request))
    {
        return Invalid;
    }
    FString Err;
    bool bAlreadyExisted = false;
    UBlueprint* BP = CreateClassBlueprint(Request, AHUD::StaticClass(), bAlreadyExisted, Err);
    if (!BP) { return mcp_common::MakeErrorReply(Err); }

    TSharedPtr<FJsonObject> Data = ClassBpPayload(BP, bAlreadyExisted);
    return mcp_common::MakeOkReply(Data);
}

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
	{ TEXT("create_game_instance_class_bp"), &HandleCreateGameInstanceClassBp },
	{ TEXT("create_game_mode_class_bp"), &HandleCreateGameModeClassBp },
	{ TEXT("create_game_state_class_bp"), &HandleCreateGameStateClassBp },
	{ TEXT("create_hud_class_bp"), &HandleCreateHudClassBp },
	{ TEXT("create_player_controller_class_bp"), &HandleCreatePlayerControllerClassBp },
	{ TEXT("create_player_state_class_bp"), &HandleCreatePlayerStateClassBp },
	{ TEXT("get_game_mode_info"), &HandleGetGameModeInfo },
	{ TEXT("place_player_start"), &HandlePlacePlayerStart },
	{ TEXT("register_spawn_point"), &HandleRegisterSpawnPoint },
	{ TEXT("reset_game_mode_override"), &HandleResetGameModeOverride },
	{ TEXT("set_game_mode_default_pawn"), &HandleSetGameModeDefaultPawn },
	{ TEXT("set_game_mode_game_state_class"), &HandleSetGameModeGameStateClass },
	{ TEXT("set_game_mode_hud_class"), &HandleSetGameModeHudClass },
	{ TEXT("set_game_mode_override"), &HandleSetGameModeOverride },
	{ TEXT("set_game_mode_player_state_class"), &HandleSetGameModePlayerStateClass },
	{ TEXT("set_game_mode_spectator_class"), &HandleSetGameModeSpectatorClass },
};
} // namespace UEAgentGameFrameworkCommands
