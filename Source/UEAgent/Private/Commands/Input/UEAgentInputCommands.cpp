#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Containers/Ticker.h" // trigger_input_action hold_seconds: FTSTicker sustain across PIE ticks
#include "GameFramework/PlayerController.h"
#include "GameFramework/InputSettings.h"
#include "GameFramework/PlayerInput.h"
// The Enhanced Input headers are only pulled in when that module is part of the build. Every
// command that needs them falls back to an "Enhanced Input module not available" error
// when WITH_ENHANCED_INPUT is 0.
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputTriggers.h"
#include "InputModifiers.h"
#include "EnhancedActionKeyMapping.h"
#include "EnhancedInputSubsystems.h"
#include "EnhancedPlayerInput.h"
#include "UserSettings/EnhancedInputUserSettings.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h" // get_input_action_state: enumerate loaded UInputMappingContexts
#include "UObject/UnrealType.h"
// ForceFeedbackEffect data asset.
#include "GameFramework/ForceFeedbackEffect.h"
#include "UObject/Package.h"

/**
 * Commands of the Input domain: the legacy UInputSettings key tables and the live UPlayerInput, Enhanced
 * Input assets (UInputAction, UInputMappingContext, triggers, modifiers), key-binding audits, user key
 * remapping, runtime input injection in PIE and force feedback effects.
 */
namespace UEAgentInputCommands
{

// MCP-DOMAIN: Input

// File-local helpers of the Input commands.
namespace UEAgentInputCommandsImpl
{
    /** One legacy UInputSettings action mapping, as get_input_settings / get_action_mappings report it. */
    TSharedPtr<FJsonValue> LegacyActionRow(const FInputActionKeyMapping& Mapping)
    {
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("action_name"), Mapping.ActionName.ToString());
        Row->SetStringField(TEXT("key"), Mapping.Key.GetDisplayName().ToString());
        Row->SetBoolField(TEXT("shift"), Mapping.bShift);
        Row->SetBoolField(TEXT("ctrl"), Mapping.bCtrl);
        Row->SetBoolField(TEXT("alt"), Mapping.bAlt);
        Row->SetBoolField(TEXT("cmd"), Mapping.bCmd);
        return MakeShared<FJsonValueObject>(Row);
    }

    /** One legacy UInputSettings axis mapping, as get_input_settings / get_axis_mappings report it. */
    TSharedPtr<FJsonValue> LegacyAxisRow(const FInputAxisKeyMapping& Mapping)
    {
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("axis_name"), Mapping.AxisName.ToString());
        Row->SetStringField(TEXT("key"), Mapping.Key.GetDisplayName().ToString());
        Row->SetNumberField(TEXT("scale"), Mapping.Scale);
        return MakeShared<FJsonValueObject>(Row);
    }

    TArray<TSharedPtr<FJsonValue>> LegacyActionRows(const UInputSettings& Settings)
    {
        TArray<TSharedPtr<FJsonValue>> Rows;
        for (const FInputActionKeyMapping& Mapping : Settings.GetActionMappings())
        {
            Rows.Add(LegacyActionRow(Mapping));
        }
        return Rows;
    }

    TArray<TSharedPtr<FJsonValue>> LegacyAxisRows(const UInputSettings& Settings)
    {
        TArray<TSharedPtr<FJsonValue>> Rows;
        for (const FInputAxisKeyMapping& Mapping : Settings.GetAxisMappings())
        {
            Rows.Add(LegacyAxisRow(Mapping));
        }
        return Rows;
    }

    /** Every registered asset of exactly AssetClass (class-path filter, all content paths). */
    TArray<FAssetData> ListRegisteredAssets(const UClass* AssetClass)
    {
        IAssetRegistry& Registry = mcp_common::AssetRegistry();
        FARFilter ByClass;
        ByClass.ClassPaths.Add(AssetClass->GetClassPathName());
        ByClass.bRecursivePaths = true;
        TArray<FAssetData> Found;
        Registry.GetAssets(ByClass, Found);
        return Found;
    }

    /**
     * The first registered asset of AssetClass whose short asset name or full object path equals
     * NameOrPath (FString equality, so case-insensitive), loaded. Null when nothing matches.
     */
    UObject* FindRegisteredAsset(const UClass* AssetClass, const FString& NameOrPath)
    {
        for (const FAssetData& Entry : ListRegisteredAssets(AssetClass))
        {
            if (Entry.AssetName.ToString() == NameOrPath || Entry.GetObjectPathString() == NameOrPath)
            {
                return Entry.GetAsset();
            }
        }
        return nullptr;
    }

    /**
     * Object path of a new input asset: "<folder>/<AssetName>", the folder read from "package_path" (alias
     * "path"), "/Game/Input" when neither is given, without a trailing '/'.
     */
    FString NewInputAssetPath(const TSharedPtr<FJsonObject>& Params, const FString& AssetName)
    {
        FString Folder;
        if (!Params->TryGetStringField(TEXT("package_path"), Folder))
        {
            Params->TryGetStringField(TEXT("path"), Folder);
        }
        if (Folder.IsEmpty())
        {
            Folder = TEXT("/Game/Input");
        }
        while (Folder.EndsWith(TEXT("/")))
        {
            Folder.LeftChopInline(1);
        }
        return Folder + TEXT("/") + AssetName;
    }

    /**
     * The controller of World that a player index addresses: 0 is the world's first player controller, any
     * other index the controller whose local player has that controller id. Null when none matches.
     */
    APlayerController* FindLocalController(UWorld* World, int32 PlayerIndex)
    {
        if (PlayerIndex == 0)
        {
            return World->GetFirstPlayerController();
        }
        for (FConstPlayerControllerIterator Walker = World->GetPlayerControllerIterator(); Walker; ++Walker)
        {
            APlayerController* Candidate = Walker->Get();
            const ULocalPlayer* Local = Candidate ? Candidate->GetLocalPlayer() : nullptr;
            if (Local != nullptr && Local->GetControllerId() == PlayerIndex)
            {
                return Candidate;
            }
        }
        return nullptr;
    }

    /**
     * The world a runtime input command acts on: the "world" selector ('pie' when absent or 'game') as
     * mcp_common::ResolveWorldSelector resolves it, else the active world when it is a Game world (a cooked
     * or -game process). OutSelector receives the selector the reply reports, lower-cased.
     */
    UWorld* ResolveRuntimeWorld(const TSharedPtr<FJsonObject>& Params, FString& OutSelector)
    {
        FString UnusedError;
        UWorld* World = mcp_common::ResolveWorldSelector(Params, TEXT("pie"), OutSelector, UnusedError);
        if (!World)
        {
            World = mcp_common::ActiveGameWorld();
        }
        return World;
    }

    /** Reads the required "ctx_path" and "mapping_index" that address one mapping of a context; the error reply when either is missing. */
    TSharedPtr<FJsonObject> ReadMappingAddress(const TSharedPtr<FJsonObject>& Params, FString& OutContextPath, int32& OutMappingIndex)
    {
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ctx_path"), OutContextPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("mapping_index"), OutMappingIndex)) { return Missing; }
        return nullptr;
    }

    /** Bool or Boolean, Axis1D, Axis2D or Axis3D (any case) into OutType; false, OutType untouched, for any other name. */
    bool TryParseValueType(const FString& Name, EInputActionValueType& OutType)
    {
        if (Name.Equals(TEXT("Boolean"), ESearchCase::IgnoreCase) || Name.Equals(TEXT("Bool"), ESearchCase::IgnoreCase))
        {
            OutType = EInputActionValueType::Boolean;
            return true;
        }
        if (Name.Equals(TEXT("Axis1D"), ESearchCase::IgnoreCase))
        {
            OutType = EInputActionValueType::Axis1D;
            return true;
        }
        if (Name.Equals(TEXT("Axis2D"), ESearchCase::IgnoreCase))
        {
            OutType = EInputActionValueType::Axis2D;
            return true;
        }
        if (Name.Equals(TEXT("Axis3D"), ESearchCase::IgnoreCase))
        {
            OutType = EInputActionValueType::Axis3D;
            return true;
        }
        return false;
    }

    /** Reported name of a value type; Boolean is reported as BooleanLabel. */
    const TCHAR* ValueTypeLabel(EInputActionValueType Type, const TCHAR* BooleanLabel)
    {
        switch (Type)
        {
        case EInputActionValueType::Axis1D: return TEXT("Axis1D");
        case EInputActionValueType::Axis2D: return TEXT("Axis2D");
        case EInputActionValueType::Axis3D: return TEXT("Axis3D");
        default:                            return BooleanLabel;
        }
    }

    const TCHAR* AccumulationLabel(EInputActionAccumulationBehavior Behavior)
    {
        return Behavior == EInputActionAccumulationBehavior::Cumulative ? TEXT("Cumulative") : TEXT("TakeHighestAbsoluteValue");
    }

    /** StaticLoadObject of a full object path, cast to the expected asset type. */
    template <typename AssetType>
    AssetType* LoadAssetAt(const FString& ObjectPath)
    {
        return Cast<AssetType>(StaticLoadObject(AssetType::StaticClass(), nullptr, *ObjectPath));
    }

    /** The FKey KeyName names; the error reply when it is not a valid key. */
    TSharedPtr<FJsonObject> ParseKey(const FString& KeyName, FKey& OutKey)
    {
        OutKey = FKey(*KeyName);
        if (!OutKey.IsValid())
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Invalid key: %s"), *KeyName));
        }
        return nullptr;
    }

    /**
     * Saves the package of Asset and answers with Reply: success only when the package reached disk. A
     * failed save leaves the edit in memory only, and the reply says so (saved false, save_error).
     */
    TSharedPtr<FJsonObject> SaveAndReply(UObject* Asset, const TSharedPtr<FJsonObject>& Reply, const TCHAR* Command)
    {
        FString SaveError;
        const bool bSaved = mcp_common::SaveAssetPackage(Asset->GetPackage(), Asset, SaveError);
        return mcp_common::MakeSavedReply(Reply, bSaved, Command, SaveError);
    }

    /** Index of the first mapping of Action to Key in Context, or INDEX_NONE. */
    int32 FindMappingIndex(const UInputMappingContext* Context, const UInputAction* Action, const FKey& Key)
    {
        if (!Context)
        {
            return INDEX_NONE;
        }
        const TArray<FEnhancedActionKeyMapping>& Mappings = Context->GetMappings();
        for (int32 i = 0; i < Mappings.Num(); ++i)
        {
            if (Mappings[i].Action == Action && Mappings[i].Key == Key)
            {
                return i;
            }
        }
        return INDEX_NONE;
    }

    /**
     * The reply of map_input_action and add_mapping_to_context: binds Key to Action in Context and saves the
     * context, unless the context already maps that key to that action -- UInputMappingContext::MapKey appends
     * a mapping on every call, duplicates included. Fills Reply with index, mapping_count, added and
     * already_mapped; success needs the mapping in the context and, when one was added, the context on disk.
     */
    TSharedPtr<FJsonObject> MapKeyOnce(UInputMappingContext* Context, const UInputAction* Action, const FKey& Key,
        const TSharedPtr<FJsonObject>& Reply, const TCHAR* Command)
    {
        const int32 Existing = FindMappingIndex(Context, Action, Key);
        if (Existing != INDEX_NONE)
        {
            Reply->SetNumberField(TEXT("index"), Existing);
            Reply->SetNumberField(TEXT("mapping_count"), Context->GetMappings().Num());
            Reply->SetBoolField(TEXT("added"), false);
            Reply->SetBoolField(TEXT("already_mapped"), true);
            Reply->SetBoolField(TEXT("saved"), false);
            return mcp_common::MakeOkReply(Reply);
        }

        Context->Modify();
        Context->MapKey(Action, Key);
        const int32 Index = FindMappingIndex(Context, Action, Key);
        Reply->SetNumberField(TEXT("index"), Index);
        Reply->SetNumberField(TEXT("mapping_count"), Context->GetMappings().Num());
        Reply->SetBoolField(TEXT("added"), Index != INDEX_NONE);
        Reply->SetBoolField(TEXT("already_mapped"), false);
        if (Index == INDEX_NONE)
        {
            Reply->SetBoolField(TEXT("saved"), false);
            return mcp_common::MakeOutcomeReply(Reply, false, FString::Printf(
                TEXT("%s: MapKey left no mapping of that key to that action in the context; nothing was saved."), Command));
        }
        return SaveAndReply(Context, Reply, Command);
    }

    /**
     * A new AssetType named AssetName, in a package of its own at AssetPath so it can be saved to disk; the
     * error reply when an asset is already there or the package or the object cannot be created. TypeName
     * names the type in those replies.
     */
    template <typename AssetType>
    TSharedPtr<FJsonObject> NewInputAsset(const FString& AssetPath, const FString& AssetName, const TCHAR* TypeName,
        UPackage*& OutPackage, AssetType*& OutAsset)
    {
        if (UEditorAssetLibrary::DoesAssetExist(AssetPath))
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("%s already exists: %s"), TypeName, *AssetPath));
        }
        FString PackageError;
        OutPackage = mcp_common::CreateAssetPackage(*AssetPath, FName(*AssetName), AssetType::StaticClass(), PackageError);
        if (!OutPackage)
        {
            return mcp_common::MakeErrorReply(PackageError);
        }
        OutAsset = NewObject<AssetType>(OutPackage, AssetType::StaticClass(), FName(*AssetName),
            RF_Public | RF_Standalone | RF_Transactional);
        if (OutAsset == nullptr)
        {
            return mcp_common::MakeErrorReply(FString(TEXT("Failed to create ")) + TypeName);
        }
        return nullptr;
    }

    /**
     * The Enhanced Input user settings of PC's local player, which hold its runtime key remaps; the error reply
     * for the first missing link of PC -> local player -> subsystem -> settings.
     */
    TSharedPtr<FJsonObject> ResolveUserSettings(APlayerController* PC, UEnhancedInputUserSettings*& OutSettings)
    {
        if (!PC)
        {
            return mcp_common::MakeErrorReply(TEXT("No PlayerController resolved"));
        }
        ULocalPlayer* LP = PC->GetLocalPlayer();
        if (!LP)
        {
            return mcp_common::MakeErrorReply(TEXT("PlayerController has no ULocalPlayer"));
        }
        UEnhancedInputLocalPlayerSubsystem* Subsystem = LP->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>();
        if (!Subsystem)
        {
            return mcp_common::MakeErrorReply(TEXT("No UEnhancedInputLocalPlayerSubsystem on this LocalPlayer"));
        }
        OutSettings = Subsystem->GetUserSettings();
        if (!OutSettings)
        {
            return mcp_common::MakeErrorReply(TEXT("No UEnhancedInputUserSettings on the local player subsystem"));
        }
        return nullptr;
    }

    /** The "slot" param ("First" when absent) into OutSlotName, as a key slot; First for a name the enum does not know. */
    EPlayerMappableKeySlot ReadMappableKeySlot(const TSharedPtr<FJsonObject>& Params, FString& OutSlotName)
    {
        OutSlotName = TEXT("First");
        Params->TryGetStringField(TEXT("slot"), OutSlotName);
        static const UEnum* SlotEnum = StaticEnum<EPlayerMappableKeySlot>();
        if (SlotEnum)
        {
            const int64 V = SlotEnum->GetValueByNameString(OutSlotName);
            if (V != INDEX_NONE)
            {
                return static_cast<EPlayerMappableKeySlot>(V);
            }
        }
        return EPlayerMappableKeySlot::First;
    }
}

// The controller addressed by the optional "player_index" param: 0 is the first player
// controller of the engine's first world, any other value a local player's controller id.
APlayerController* ResolvePlayerController(const TSharedPtr<FJsonObject>& Params)
{
    int32 RequestedIndex = 0;
    Params->TryGetNumberField(TEXT("player_index"), RequestedIndex);

    UWorld* FirstWorld = mcp_common::GetFirstContextWorld();
    if (FirstWorld == nullptr)
    {
        return nullptr;
    }
    return UEAgentInputCommandsImpl::FindLocalController(FirstWorld, RequestedIndex);
}

// Legacy input: the UInputSettings action/axis tables and a player's UPlayerInput.

// MCP:get_input_settings - Read the project's LEGACY input mappings from UInputSettings: named action mappings (key + shift/ctrl/alt/cmd modifiers) and axis mappings (key + scale), in one call. Legacy only -- it does NOT see Enhanced Input mapping contexts, so on an Enhanced Input project it can return almost nothing while the game is fully bound. For the complete picture across both systems use list_all_input_mappings; for one Enhanced Input context use get_input_mapping_context.
//   @returns { action_name, key, shift, ctrl, alt, cmd, action_mappings, axis_name, scale, axis_mappings }
TSharedPtr<FJsonObject> HandleGetInputSettings(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentInputCommandsImpl;

    const UInputSettings* Legacy = UInputSettings::GetInputSettings();
    if (Legacy == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to get Input Settings"));
    }

    // Both legacy tables at once; Enhanced Input contexts are not part of this answer.
    TSharedPtr<FJsonObject> Tables = MakeShared<FJsonObject>();
    Tables->SetArrayField(TEXT("action_mappings"), Impl::LegacyActionRows(*Legacy));
    Tables->SetArrayField(TEXT("axis_mappings"), Impl::LegacyAxisRows(*Legacy));
    return mcp_common::MakeOkReply(Tables);
}

// MCP:get_player_input - Read what a player is pressing RIGHT NOW: resolves the PlayerController for a local player index and returns it plus the currently pressed keys. Runtime state, so it needs a live PIE/game session -- an empty pressed_keys outside PIE means nothing. For which keys are BOUND (a static question) use list_all_input_mappings instead; to make an action fire use trigger_input_action.
//   @param player_index - Player controller index to query (default 0).
//   @returns { player_controller, pressed_keys }
TSharedPtr<FJsonObject> HandleGetPlayerInput(const TSharedPtr<FJsonObject>& Params)
{
    APlayerController* Controller = ResolvePlayerController(Params);
    if (Controller == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("Player Controller not found"));
    }
    if (Controller->PlayerInput == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("PlayerInput not found"));
    }

    TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
    State->SetStringField(TEXT("player_controller"), Controller->GetName());
    // UPlayerInput keeps its key-state map protected; IsPressed is the public question, asked
    // for every key the engine defines.
    TArray<FKey> AllKeys;
    EKeys::GetAllKeys(AllKeys);
    TArray<TSharedPtr<FJsonValue>> Pressed;
    for (const FKey& Key : AllKeys)
    {
        if (Controller->PlayerInput->IsPressed(Key))
        {
            Pressed.Add(MakeShared<FJsonValueString>(Key.GetFName().ToString()));
        }
    }
    State->SetArrayField(TEXT("pressed_keys"), Pressed);
    return mcp_common::MakeOkReply(State);
}

// MCP:get_action_mappings - List the project's LEGACY action key mappings only (action_name + key + shift/ctrl/alt/cmd), with a count. Narrower than get_input_settings, which returns actions and axes together, and blind to Enhanced Input mapping contexts -- an empty list does not mean the game has no buttons bound. Use list_all_input_mappings when the question is "is this key already taken".
//   @returns { action_name, key, shift, ctrl, alt, cmd, action_mappings, count }
TSharedPtr<FJsonObject> HandleGetActionMappings(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentInputCommandsImpl;

    const UInputSettings* Legacy = UInputSettings::GetInputSettings();
    if (Legacy == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to get Input Settings"));
    }

    const TArray<TSharedPtr<FJsonValue>> Rows = Impl::LegacyActionRows(*Legacy);
    TSharedPtr<FJsonObject> Table = MakeShared<FJsonObject>();
    Table->SetNumberField(TEXT("count"), Rows.Num());
    Table->SetArrayField(TEXT("action_mappings"), Rows);
    return mcp_common::MakeOkReply(Table);
}

// MCP:get_axis_mappings - List the project's LEGACY axis key mappings only (axis_name + key + scale), with a count -- the analog/movement half of the legacy input system. Blind to Enhanced Input mapping contexts, so an empty list is not proof that movement is unbound; get_input_settings returns actions and axes together, and list_all_input_mappings covers both systems.
//   @returns { axis_name, key, scale, axis_mappings, count }
TSharedPtr<FJsonObject> HandleGetAxisMappings(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentInputCommandsImpl;

    const UInputSettings* Legacy = UInputSettings::GetInputSettings();
    if (Legacy == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to get Input Settings"));
    }

    const TArray<TSharedPtr<FJsonValue>> Rows = Impl::LegacyAxisRows(*Legacy);
    TSharedPtr<FJsonObject> Table = MakeShared<FJsonObject>();
    Table->SetNumberField(TEXT("count"), Rows.Num());
    Table->SetArrayField(TEXT("axis_mappings"), Rows);
    return mcp_common::MakeOkReply(Table);
}

// NOTE: HandleGetInputAction is defined further below, after the Enhanced Input
// authoring helper namespace, so it can reuse ResolveInputAction and
// SerializeTriggerProps (see "Enhanced Input authoring").

// Enhanced Input assets: read an action or a context, create either, bind a key.

// MCP:get_input_mapping_context - Read ONE Enhanced Input mapping context asset (IMC): every action-to-key mapping it declares. An IMC asset stores no priority -- that is given when the context is added to a player (AddMappingContext), so it is not reported here. The per-asset reader, and the reliable one -- the Python property InputMappingContext.mappings is deprecated and hands back an EMPTY list without failing, so a scan built on it reports a fully-bound context as empty. Use list_all_input_mappings for the project-wide "which keys are taken / which conflict" question.
//   @param context_path - Full path to the InputMappingContext asset (e.g., /Game/Input/IMC_Default).
//   @returns { context_path, context_name, mappings: [{ action_name, key (display name), key_name (the FKey name map_input_action takes) }], mapping_count }
TSharedPtr<FJsonObject> HandleGetInputMappingContext(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentInputCommandsImpl;

    FString RequestedContext;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("context_path"), RequestedContext, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    // A full object path loads directly; otherwise the first registered context whose short
    // name or object path matches is used.
    UInputMappingContext* Imc = Impl::LoadAssetAt<UInputMappingContext>(RequestedContext);
    if (Imc == nullptr)
    {
        Imc = Cast<UInputMappingContext>(Impl::FindRegisteredAsset(UInputMappingContext::StaticClass(), RequestedContext));
    }
    if (Imc == nullptr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("InputMappingContext not found: %s"), *RequestedContext));
    }

    // One row per mapping that has an action.
    TArray<TSharedPtr<FJsonValue>> Rows;
    for (const FEnhancedActionKeyMapping& Mapping : Imc->GetMappings())
    {
        if (!Mapping.Action)
        {
            continue;
        }
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("action_name"), Mapping.Action->GetName());
        Row->SetStringField(TEXT("key"), Mapping.Key.GetDisplayName().ToString());
        Row->SetStringField(TEXT("key_name"), Mapping.Key.GetFName().ToString());
        Rows.Add(MakeShared<FJsonValueObject>(Row));
    }

    TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
    Summary->SetStringField(TEXT("context_path"), RequestedContext);
    Summary->SetStringField(TEXT("context_name"), Imc->GetName());
    Summary->SetNumberField(TEXT("mapping_count"), Rows.Num());
    Summary->SetArrayField(TEXT("mappings"), Rows);
    return mcp_common::MakeOkReply(Summary);
}

// MCP:create_input_action - Create an Enhanced Input InputAction asset (IA_): the verb half of a keybinding -- what the player does, independent of which key does it. Pick value_type up front (Bool for a button, Axis1D for a trigger/scroll, Axis2D for a stick or WASD, Axis3D), then bind it to a key inside a mapping context with map_input_action or add_mapping_to_context. Changing the type later is set_input_action_value_type; behaviour flags are set_input_action_flags. The asset is saved, and the call succeeds only when it reaches disk.
//   @param action_name - Name of the new InputAction.
//   @param package_path - Target content package path where the asset is created (default /Game/Input).
//   @param path - Fallback for package_path when package_path is absent.
//   @param value_type - Value type Bool (default), Axis1D, Axis2D, or Axis3D.
//   @returns { action_name, action_path, value_type, saved, save_error? }
TSharedPtr<FJsonObject> HandleCreateInputAction(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentInputCommandsImpl;

    FString NewName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("action_name"), NewName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    const FString AssetPath = Impl::NewInputAssetPath(Params, NewName);
    UPackage* OwningPackage = nullptr;
    UInputAction* Action = nullptr;
    if (TSharedPtr<FJsonObject> Failure = Impl::NewInputAsset<UInputAction>(AssetPath, NewName, TEXT("InputAction"), OwningPackage, Action))
    {
        return Failure;
    }

    FString RequestedType;
    Params->TryGetStringField(TEXT("value_type"), RequestedType);
    // A value_type that TryParseValueType does not know, or none, leaves the action Boolean.
    EInputActionValueType ValueType = EInputActionValueType::Boolean;
    Impl::TryParseValueType(RequestedType, ValueType);
    Action->ValueType = ValueType;

    FAssetRegistryModule::AssetCreated(Action);

    TSharedPtr<FJsonObject> Created = MakeShared<FJsonObject>();
    Created->SetStringField(TEXT("action_name"), NewName);
    Created->SetStringField(TEXT("action_path"), AssetPath);
    Created->SetStringField(TEXT("value_type"), Impl::ValueTypeLabel(Action->ValueType, TEXT("Bool")));
    return Impl::SaveAndReply(Action, Created, TEXT("create_input_action"));
}

// MCP:create_input_mapping_context - Create an Enhanced Input mapping context asset (IMC_): the container that binds keys to InputActions and can be pushed and popped as a set. Prefer a context per MODE (driving, swimming, building, menu) over one key per verb -- pushing a context re-purposes the existing keys instead of consuming another global one. Empty on creation: fill it with map_input_action / add_mapping_to_context, and check the keys are free first with find_free_input_key. The asset is saved, and the call succeeds only when it reaches disk.
//   @param context_name - Name of the new InputMappingContext.
//   @param package_path - Target content package path where the asset is created (default /Game/Input).
//   @param path - Fallback for package_path when package_path is absent.
//   @returns { context_name, context_path, saved, save_error? }
TSharedPtr<FJsonObject> HandleCreateInputMappingContext(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentInputCommandsImpl;

    FString NewName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("context_name"), NewName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    const FString AssetPath = Impl::NewInputAssetPath(Params, NewName);

    // The context starts empty; mappings are added by map_input_action / add_mapping_to_context.
    UPackage* OwningPackage = nullptr;
    UInputMappingContext* Context = nullptr;
    if (TSharedPtr<FJsonObject> Failure = Impl::NewInputAsset<UInputMappingContext>(AssetPath, NewName, TEXT("InputMappingContext"), OwningPackage, Context))
    {
        return Failure;
    }

    FAssetRegistryModule::AssetCreated(Context);

    TSharedPtr<FJsonObject> Created = MakeShared<FJsonObject>();
    Created->SetStringField(TEXT("context_name"), NewName);
    Created->SetStringField(TEXT("context_path"), AssetPath);
    return Impl::SaveAndReply(Context, Created, TEXT("create_input_mapping_context"));
}

// MCP:map_input_action - Bind a key to an InputAction in an Enhanced Input mapping context, both by full asset path (add_mapping_to_context takes short names), and save the context. A context that already maps that key to that action is left as it is (added false, already_mapped true, not saved); a new mapping succeeds only when the context reaches disk. Other actions on the same key are not checked: use find_free_input_key or list_all_input_mappings first.
//   @param context_path - Full path to the InputMappingContext asset.
//   @param action_path - Full path to the InputAction asset.
//   @param key_name - Key name to map (e.g., "SpaceBar", "LeftMouseButton").
//   @param key - Alias of key_name, read only when key_name is absent (same FKey names).
//   @param input_action_path - Fallback for action_path when action_path is absent.
//   @returns { context_path, action_path, key_name, index, mapping_count, added, already_mapped, saved, save_error? }
TSharedPtr<FJsonObject> HandleMapInputAction(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentInputCommandsImpl;

    FString ContextAsset;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("context_path"), ContextAsset, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    // action_path, else the alias input_action_path.
    FString ActionAsset;
    if (!Params->TryGetStringField(TEXT("action_path"), ActionAsset))
    {
        Params->TryGetStringField(TEXT("input_action_path"), ActionAsset);
    }
    if (ActionAsset.IsEmpty())
    {
        return mcp_common::MissingParameterReply(TEXT("action_path"));
    }
    // key_name, else key.
    FString KeyText;
    if (!Params->TryGetStringField(TEXT("key_name"), KeyText))
    {
        Params->TryGetStringField(TEXT("key"), KeyText);
    }
    if (KeyText.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: key (or key_name)"));
    }

    // Both assets are addressed by full object path here (add_mapping_to_context also takes
    // short names); the key must name a real FKey.
    UInputMappingContext* Imc = Impl::LoadAssetAt<UInputMappingContext>(ContextAsset);
    if (Imc == nullptr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("InputMappingContext not found: %s"), *ContextAsset));
    }
    UInputAction* Action = Impl::LoadAssetAt<UInputAction>(ActionAsset);
    if (Action == nullptr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("InputAction not found: %s"), *ActionAsset));
    }
    FKey Key;
    if (TSharedPtr<FJsonObject> Failure = Impl::ParseKey(KeyText, Key))
    {
        return Failure;
    }

    // The mapping array itself is protected, so the binding goes through the public MapKey API.
    TSharedPtr<FJsonObject> Mapped = MakeShared<FJsonObject>();
    Mapped->SetStringField(TEXT("context_path"), ContextAsset);
    Mapped->SetStringField(TEXT("action_path"), ActionAsset);
    Mapped->SetStringField(TEXT("key_name"), KeyText);
    return Impl::MapKeyOnce(Imc, Action, Key, Mapped, TEXT("map_input_action"));
}

// ============================================================================
// Enhanced Input authoring
// ============================================================================
namespace
{
    /** Resolve a UInputAction by full object path or by short asset name (scans the asset registry). */
    UInputAction* ResolveInputAction(const FString& PathOrName)
    {
        if (PathOrName.IsEmpty())
        {
            return nullptr;
        }

        // A full object/package path loads directly; a short asset name (or an object path the
        // loader missed) is looked up in the asset registry instead.
        if (UInputAction* Direct = UEAgentInputCommandsImpl::LoadAssetAt<UInputAction>(PathOrName))
        {
            return Direct;
        }
        return Cast<UInputAction>(UEAgentInputCommandsImpl::FindRegisteredAsset(UInputAction::StaticClass(), PathOrName));
    }

    /** Resolve a UInputMappingContext by full object path or by short asset name (scans the asset registry). */
    UInputMappingContext* ResolveMappingContext(const FString& PathOrName)
    {
        if (PathOrName.IsEmpty())
        {
            return nullptr;
        }

        if (UInputMappingContext* Direct = UEAgentInputCommandsImpl::LoadAssetAt<UInputMappingContext>(PathOrName))
        {
            return Direct;
        }
        return Cast<UInputMappingContext>(UEAgentInputCommandsImpl::FindRegisteredAsset(UInputMappingContext::StaticClass(), PathOrName));
    }

    /** Resolve a trigger type string to the concrete UInputTrigger subclass. */
    UClass* ResolveTriggerClass(const FString& Type)
    {
        if (Type.Equals(TEXT("Down"), ESearchCase::IgnoreCase))        { return UInputTriggerDown::StaticClass(); }
        if (Type.Equals(TEXT("Pressed"), ESearchCase::IgnoreCase))     { return UInputTriggerPressed::StaticClass(); }
        if (Type.Equals(TEXT("Released"), ESearchCase::IgnoreCase))    { return UInputTriggerReleased::StaticClass(); }
        if (Type.Equals(TEXT("Hold"), ESearchCase::IgnoreCase))        { return UInputTriggerHold::StaticClass(); }
        if (Type.Equals(TEXT("Tap"), ESearchCase::IgnoreCase))         { return UInputTriggerTap::StaticClass(); }
        if (Type.Equals(TEXT("Pulse"), ESearchCase::IgnoreCase))       { return UInputTriggerPulse::StaticClass(); }
        if (Type.Equals(TEXT("ChordAction"), ESearchCase::IgnoreCase)) { return UInputTriggerChordAction::StaticClass(); }
        return nullptr;
    }

    /** Resolve a modifier type string to the concrete UInputModifier subclass. */
    UClass* ResolveModifierClass(const FString& Type)
    {
        if (Type.Equals(TEXT("Negate"), ESearchCase::IgnoreCase))   { return UInputModifierNegate::StaticClass(); }
        if (Type.Equals(TEXT("Swizzle"), ESearchCase::IgnoreCase))  { return UInputModifierSwizzleAxis::StaticClass(); }
        if (Type.Equals(TEXT("DeadZone"), ESearchCase::IgnoreCase)) { return UInputModifierDeadZone::StaticClass(); }
        if (Type.Equals(TEXT("Scalar"), ESearchCase::IgnoreCase))   { return UInputModifierScalar::StaticClass(); }
        return nullptr;
    }

    /**
     * Serialize the editable behavior UPROPERTYs of a trigger instance into a JSON object,
     * by reflection over the concrete subclass. Only properties that actually exist on the
     * instance's class are emitted, so a Hold reports HoldTimeThreshold while a Tap does not.
     */
    void SerializeTriggerProps(const UInputTrigger* Trigger, const TSharedPtr<FJsonObject>& Out)
    {
        if (!Trigger || !Out.IsValid())
        {
            return;
        }
        const UClass* TriggerClass = Trigger->GetClass();

        // Float properties shared/per-subclass: base ActuationThreshold, Hold HoldTimeThreshold, Tap TapReleaseTimeThreshold.
        static const TCHAR* FloatProps[] = { TEXT("ActuationThreshold"), TEXT("HoldTimeThreshold"), TEXT("TapReleaseTimeThreshold") };
        for (const TCHAR* PropName : FloatProps)
        {
            if (FFloatProperty* Prop = FindFProperty<FFloatProperty>(TriggerClass, PropName))
            {
                Out->SetNumberField(PropName, Prop->GetPropertyValue_InContainer(Trigger));
            }
        }

        // Bool properties: TimedBase bAffectedByTimeDilation, Hold bIsOneShot.
        static const TCHAR* BoolProps[] = { TEXT("bAffectedByTimeDilation"), TEXT("bIsOneShot") };
        for (const TCHAR* PropName : BoolProps)
        {
            if (FBoolProperty* Prop = FindFProperty<FBoolProperty>(TriggerClass, PropName))
            {
                Out->SetBoolField(PropName, Prop->GetPropertyValue_InContainer(Trigger));
            }
        }
    }
}

namespace UEAgentInputCommandsImpl
{
    /** Reads "action_path", or its alias "action_name", into OutRef; the error reply when neither is given. */
    TSharedPtr<FJsonObject> ReadActionPathOrName(const TSharedPtr<FJsonObject>& Params, FString& OutRef)
    {
        if (!Params->TryGetStringField(TEXT("action_path"), OutRef))
        {
            Params->TryGetStringField(TEXT("action_name"), OutRef);
        }
        if (OutRef.IsEmpty())
        {
            return mcp_common::MakeErrorReply(TEXT("Missing required parameter: action_path (or action_name)"));
        }
        return nullptr;
    }

    /** Reads "action_name", or its alias "action_path", into OutRef; the error reply when neither is given. */
    TSharedPtr<FJsonObject> ReadActionNameOrPath(const TSharedPtr<FJsonObject>& Params, FString& OutRef)
    {
        if (!Params->TryGetStringField(TEXT("action_name"), OutRef))
        {
            Params->TryGetStringField(TEXT("action_path"), OutRef);
        }
        if (OutRef.IsEmpty())
        {
            return mcp_common::MakeErrorReply(TEXT("Missing required parameter: action_name (or action_path)"));
        }
        return nullptr;
    }

    /** The InputAction ActionRef names (full object path or short asset name); the error reply when none is found. */
    TSharedPtr<FJsonObject> FindInputAction(const FString& ActionRef, UInputAction*& OutAction)
    {
        OutAction = ResolveInputAction(ActionRef);
        if (!OutAction)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("InputAction not found: %s"), *ActionRef));
        }
        return nullptr;
    }

    /** The InputMappingContext ContextRef names (full object path or short asset name); the error reply when none is found. */
    TSharedPtr<FJsonObject> FindMappingContext(const FString& ContextRef, UInputMappingContext*& OutContext)
    {
        OutContext = ResolveMappingContext(ContextRef);
        if (!OutContext)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("InputMappingContext not found: %s"), *ContextRef));
        }
        return nullptr;
    }

    /**
     * The InputAction a runtime command names in "action" (aliases "action_path", "action_name"); the error reply
     * when none of them is given or nothing is found.
     */
    TSharedPtr<FJsonObject> ResolveRuntimeAction(const TSharedPtr<FJsonObject>& Params, UInputAction*& OutAction)
    {
        FString ActionRef;
        if (!Params->TryGetStringField(TEXT("action"), ActionRef) || ActionRef.IsEmpty())
        {
            // Accept action_path / action_name as convenience aliases.
            if (!Params->TryGetStringField(TEXT("action_path"), ActionRef))
            {
                Params->TryGetStringField(TEXT("action_name"), ActionRef);
            }
        }
        if (ActionRef.IsEmpty())
        {
            return mcp_common::MissingParameterReply(TEXT("action"));
        }
        return FindInputAction(ActionRef, OutAction);
    }

    /**
     * The context, action and key a mapping command addresses, resolved in that order; the error reply for the
     * first one that does not resolve.
     */
    TSharedPtr<FJsonObject> ResolveMappingTarget(const FString& ContextRef, const FString& ActionRef, const FString& KeyName,
        UInputMappingContext*& OutContext, UInputAction*& OutAction, FKey& OutKey)
    {
        if (TSharedPtr<FJsonObject> Failure = FindMappingContext(ContextRef, OutContext))
        {
            return Failure;
        }
        if (TSharedPtr<FJsonObject> Failure = FindInputAction(ActionRef, OutAction))
        {
            return Failure;
        }
        return ParseKey(KeyName, OutKey);
    }

    /** Index of the Action+Key mapping in Context, mapping the key first when it is not there; INDEX_NONE when that fails. */
    int32 FindOrAddMappingIndex(UInputMappingContext* Context, const UInputAction* Action, const FKey& Key)
    {
        int32 Index = FindMappingIndex(Context, Action, Key);
        if (Index == INDEX_NONE)
        {
            Context->Modify();
            Context->MapKey(Action, Key);
            Index = FindMappingIndex(Context, Action, Key);
        }
        return Index;
    }
}

// MCP:get_input_action - Read one Enhanced Input InputAction asset in full: its value type (Bool / Axis1D / Axis2D / Axis3D), its accumulation behaviour, all the consume/pause/reserve flags, and the triggers and modifiers attached to the ACTION itself. The read-back oracle for create_input_action, set_input_action_value_type and set_input_action_flags, and the place to check b_consume_input -- an action left consuming input can swallow a key for the whole game. Triggers listed here are the action's own; the ones on a specific key binding live in the mapping context (list_input_triggers).
//   @param action_path - Full path to the InputAction asset (e.g., /Game/Input/IA_Jump).
//   @param action_name - Fallback for action_path when action_path is absent (short asset name).
//   @returns { action_path, action_name, value_type, accumulation_behavior, b_consume_input, b_trigger_when_paused, b_consumes_action_and_axis_mappings, b_reserve_all_mappings, trigger_events_that_consume_legacy_keys, action_description, b_tap, triggers, modifiers }
TSharedPtr<FJsonObject> HandleGetInputAction(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentInputCommandsImpl;

    FString ActionRef;
    if (TSharedPtr<FJsonObject> Missing = Impl::ReadActionPathOrName(Params, ActionRef))
    {
        return Missing;
    }
    UInputAction* InputAction = nullptr;
    if (TSharedPtr<FJsonObject> Failure = Impl::FindInputAction(ActionRef, InputAction))
    {
        return Failure;
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("action_path"), InputAction->GetPathName());
    ResultObj->SetStringField(TEXT("action_name"), InputAction->GetName());
    ResultObj->SetStringField(TEXT("value_type"), Impl::ValueTypeLabel(InputAction->ValueType, TEXT("Bool")));
    ResultObj->SetStringField(TEXT("accumulation_behavior"), Impl::AccumulationLabel(InputAction->AccumulationBehavior));

    ResultObj->SetBoolField(TEXT("b_consume_input"), InputAction->bConsumeInput);
    ResultObj->SetBoolField(TEXT("b_trigger_when_paused"), InputAction->bTriggerWhenPaused);
    ResultObj->SetBoolField(TEXT("b_consumes_action_and_axis_mappings"), InputAction->bConsumesActionAndAxisMappings);
    ResultObj->SetBoolField(TEXT("b_reserve_all_mappings"), InputAction->bReserveAllMappings);
    ResultObj->SetNumberField(TEXT("trigger_events_that_consume_legacy_keys"), InputAction->TriggerEventsThatConsumeLegacyKeys);
    ResultObj->SetStringField(TEXT("action_description"), InputAction->ActionDescription.ToString());
    ResultObj->SetBoolField(TEXT("b_tap"), InputAction->Triggers.ContainsByPredicate([](const UInputTrigger* Trigger) {
        return Trigger && Trigger->GetClass()->GetName() == TEXT("InputTriggerTap");
    }));

    // Triggers on the action itself (asset-level), with their reflected behavior props.
    TArray<TSharedPtr<FJsonValue>> TriggersArray;
    for (const TObjectPtr<UInputTrigger>& Trigger : InputAction->Triggers)
    {
        if (!Trigger)
        {
            continue;
        }
        TSharedPtr<FJsonObject> TriggerObj = MakeShared<FJsonObject>();
        TriggerObj->SetStringField(TEXT("class"), Trigger->GetClass()->GetName());
        SerializeTriggerProps(Trigger, TriggerObj);
        TriggersArray.Add(MakeShared<FJsonValueObject>(TriggerObj));
    }
    ResultObj->SetArrayField(TEXT("triggers"), TriggersArray);

    // Modifiers on the action itself (asset-level).
    TArray<TSharedPtr<FJsonValue>> ModifiersArray;
    for (const TObjectPtr<UInputModifier>& Modifier : InputAction->Modifiers)
    {
        if (!Modifier)
        {
            continue;
        }
        TSharedPtr<FJsonObject> ModifierObj = MakeShared<FJsonObject>();
        ModifierObj->SetStringField(TEXT("class"), Modifier->GetClass()->GetName());
        ModifiersArray.Add(MakeShared<FJsonValueObject>(ModifierObj));
    }
    ResultObj->SetArrayField(TEXT("modifiers"), ModifiersArray);

    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:set_input_action_value_type - Change an InputAction's value type after the fact (button vs axis: Bool / Axis1D / Axis2D / Axis3D). Not cosmetic -- the type decides what shape of value the bound handlers receive and what trigger_input_action must inject, so switching it can break existing graph pins. Re-read with get_input_action and re-check the handlers. The action is saved, and the call succeeds only when it reaches disk.
//   @param action_path - Asset path to the input action
//   @param value_type - One of "Bool", "Float", "Int", "Vector2D", "Vector", "Rotator", "String", etc.
//   @param action_name - Fallback for action_path when action_path is absent.
//   @returns { action_name, action_path, value_type, value_type_set, saved, save_error? }
TSharedPtr<FJsonObject> HandleSetInputActionValueType(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentInputCommandsImpl;

    FString ActionRef;
    if (TSharedPtr<FJsonObject> Missing = Impl::ReadActionPathOrName(Params, ActionRef))
    {
        return Missing;
    }

    FString ValueTypeStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("value_type"), ValueTypeStr)) { return Missing; }

    UInputAction* Action = nullptr;
    if (TSharedPtr<FJsonObject> Failure = Impl::FindInputAction(ActionRef, Action))
    {
        return Failure;
    }

    EInputActionValueType NewType = EInputActionValueType::Boolean;
    if (!Impl::TryParseValueType(ValueTypeStr, NewType))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Invalid value_type: %s (expected Boolean|Axis1D|Axis2D|Axis3D)"), *ValueTypeStr));
    }

    Action->Modify();
    Action->ValueType = NewType;

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("action_name"), Action->GetName());
    ResultObj->SetStringField(TEXT("action_path"), Action->GetPathName());
    // Read back to confirm the change actually took.
    ResultObj->SetStringField(TEXT("value_type"), Impl::ValueTypeLabel(Action->ValueType, TEXT("Boolean")));
    ResultObj->SetBoolField(TEXT("value_type_set"), Action->ValueType == NewType);
    return Impl::SaveAndReply(Action, ResultObj, TEXT("set_input_action_value_type"));
}

// MCP:set_input_action_flags - Set an InputAction's behaviour flags: consume_input, trigger_when_paused, consumes_action_and_axis_mappings, reserve_all_mappings, the legacy-key consume bitmask, accumulation behaviour and the editor description. Handle consume_input with care -- it is the flag that decides whether OTHER actions on the same key still fire, and turning it on can swallow the game's interaction key entirely, a failure that shows up in an unrelated system days later. Only the fields you pass change, and a call that passes none is an error; the action is saved, and the call succeeds only when it reaches disk. Verify with get_input_action.
//   @param action_name - Input action name
//   @param action_path - Asset path of the Input Action
//   @param consume_input - When set, updates the action's bConsumeInput flag.
//   @param trigger_when_paused - When set, updates the action's bTriggerWhenPaused flag.
//   @param consumes_action_and_axis_mappings - When set, updates bConsumesActionAndAxisMappings.
//   @param reserve_all_mappings - When set, updates bReserveAllMappings.
//   @param trigger_events_that_consume_legacy_keys - When set (int32 ETriggerEvent bitmask), updates TriggerEventsThatConsumeLegacyKeys.
//   @param accumulation_behavior - When set, one of "TakeHighestAbsoluteValue" or "Cumulative".
//   @param action_description - When set, updates the action's ActionDescription (FText) shown in the editor.
//   @returns { action_name, action_path, b_consume_input, b_trigger_when_paused, b_consumes_action_and_axis_mappings, b_reserve_all_mappings, trigger_events_that_consume_legacy_keys, accumulation_behavior, action_description, flags_set, saved, save_error? }
TSharedPtr<FJsonObject> HandleSetInputActionFlags(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentInputCommandsImpl;

    FString ActionRef;
    if (TSharedPtr<FJsonObject> Missing = Impl::ReadActionNameOrPath(Params, ActionRef))
    {
        return Missing;
    }
    UInputAction* Action = nullptr;
    if (TSharedPtr<FJsonObject> Failure = Impl::FindInputAction(ActionRef, Action))
    {
        return Failure;
    }

    Action->Modify();

    bool bAnyProvided = false;

    bool bConsumeInput = false;
    if (Params->TryGetBoolField(TEXT("consume_input"), bConsumeInput))
    {
        Action->bConsumeInput = bConsumeInput;
        bAnyProvided = true;
    }

    bool bTriggerWhenPaused = false;
    if (Params->TryGetBoolField(TEXT("trigger_when_paused"), bTriggerWhenPaused))
    {
        Action->bTriggerWhenPaused = bTriggerWhenPaused;
        bAnyProvided = true;
    }

    bool bConsumesActionAndAxis = false;
    if (Params->TryGetBoolField(TEXT("consumes_action_and_axis_mappings"), bConsumesActionAndAxis))
    {
        Action->bConsumesActionAndAxisMappings = bConsumesActionAndAxis;
        bAnyProvided = true;
    }

    bool bReserveAll = false;
    if (Params->TryGetBoolField(TEXT("reserve_all_mappings"), bReserveAll))
    {
        Action->bReserveAllMappings = bReserveAll;
        bAnyProvided = true;
    }

    // int32 bitmask over ETriggerEvent.
    int32 LegacyKeysMask = 0;
    if (Params->TryGetNumberField(TEXT("trigger_events_that_consume_legacy_keys"), LegacyKeysMask))
    {
        Action->TriggerEventsThatConsumeLegacyKeys = LegacyKeysMask;
        bAnyProvided = true;
    }

    FString AccumulationStr;
    if (Params->TryGetStringField(TEXT("accumulation_behavior"), AccumulationStr) && !AccumulationStr.IsEmpty())
    {
        if (AccumulationStr.Equals(TEXT("TakeHighestAbsoluteValue"), ESearchCase::IgnoreCase))
        {
            Action->AccumulationBehavior = EInputActionAccumulationBehavior::TakeHighestAbsoluteValue;
        }
        else if (AccumulationStr.Equals(TEXT("Cumulative"), ESearchCase::IgnoreCase))
        {
            Action->AccumulationBehavior = EInputActionAccumulationBehavior::Cumulative;
        }
        else
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Invalid accumulation_behavior: %s (expected TakeHighestAbsoluteValue|Cumulative)"), *AccumulationStr));
        }
        bAnyProvided = true;
    }

    FString ActionDescription;
    if (Params->TryGetStringField(TEXT("action_description"), ActionDescription))
    {
        Action->ActionDescription = FText::FromString(ActionDescription);
        bAnyProvided = true;
    }

    if (!bAnyProvided)
    {
        return mcp_common::MakeErrorReply(TEXT("No flag provided (consume_input|trigger_when_paused|consumes_action_and_axis_mappings|")
            TEXT("reserve_all_mappings|trigger_events_that_consume_legacy_keys|accumulation_behavior|action_description): nothing was changed"));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("action_name"), Action->GetName());
    ResultObj->SetStringField(TEXT("action_path"), Action->GetPathName());
    ResultObj->SetBoolField(TEXT("b_consume_input"), Action->bConsumeInput);
    ResultObj->SetBoolField(TEXT("b_trigger_when_paused"), Action->bTriggerWhenPaused);
    ResultObj->SetBoolField(TEXT("b_consumes_action_and_axis_mappings"), Action->bConsumesActionAndAxisMappings);
    ResultObj->SetBoolField(TEXT("b_reserve_all_mappings"), Action->bReserveAllMappings);
    ResultObj->SetNumberField(TEXT("trigger_events_that_consume_legacy_keys"), Action->TriggerEventsThatConsumeLegacyKeys);
    ResultObj->SetStringField(TEXT("accumulation_behavior"), Impl::AccumulationLabel(Action->AccumulationBehavior));
    ResultObj->SetStringField(TEXT("action_description"), Action->ActionDescription.ToString());
    ResultObj->SetBoolField(TEXT("flags_set"), bAnyProvided);
    return Impl::SaveAndReply(Action, ResultObj, TEXT("set_input_action_flags"));
}

// MCP:set_input_action_trigger_behavior - Tune HOW an input trigger fires on an InputAction: actuation threshold, hold time and one-shot for Hold triggers, tap release window for Tap, time-dilation sensitivity for timed ones -- i.e. how long "hold E to interact" must be held. Address the trigger either by index or by trigger_class (first of that type). Only the fields you pass change; the action is saved, and the call succeeds only when it reaches disk. Add the trigger first with add_input_trigger and re-read with get_input_action.
//   @param action_path - Asset path of the Input Action (or short asset name).
//   @param action_name - Fallback for action_path when action_path is absent.
//   @param trigger_index - Index into the action's Triggers array to edit. Mutually exclusive with trigger_class.
//   @param trigger_class - Select the first trigger of this type (Down|Pressed|Released|Hold|Tap|Pulse|ChordAction) instead of an index.
//   @param actuation_threshold - When set, the base UInputTrigger::ActuationThreshold (float).
//   @param hold_time_threshold - When set, UInputTriggerHold::HoldTimeThreshold (float). Hold triggers only.
//   @param is_one_shot - When set, UInputTriggerHold::bIsOneShot (bool). Hold triggers only.
//   @param tap_release_time - When set, UInputTriggerTap::TapReleaseTimeThreshold (float). Tap triggers only.
//   @param affected_by_time_dilation - When set, UInputTriggerTimedBase::bAffectedByTimeDilation (bool). Timed triggers only.
//   @returns { action_path, action_name, trigger_index, trigger_class, properties_set, trigger, saved, save_error? }
TSharedPtr<FJsonObject> HandleSetInputActionTriggerBehavior(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentInputCommandsImpl;

    FString ActionRef;
    if (TSharedPtr<FJsonObject> Missing = Impl::ReadActionPathOrName(Params, ActionRef))
    {
        return Missing;
    }
    UInputAction* Action = nullptr;
    if (TSharedPtr<FJsonObject> Failure = Impl::FindInputAction(ActionRef, Action))
    {
        return Failure;
    }

    // Resolve the target trigger: by explicit index, or the first instance of a given class.
    int32 TriggerIndex = INDEX_NONE;
    const bool bHasIndex = Params->TryGetNumberField(TEXT("trigger_index"), TriggerIndex);

    FString TriggerClassStr;
    const bool bHasClass = Params->TryGetStringField(TEXT("trigger_class"), TriggerClassStr) && !TriggerClassStr.IsEmpty();

    if (!bHasIndex && !bHasClass)
    {
        return mcp_common::MakeErrorReply(TEXT("Missing target selector: provide trigger_index or trigger_class"));
    }

    if (bHasClass)
    {
        UClass* WantedClass = ResolveTriggerClass(TriggerClassStr);
        if (!WantedClass)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Invalid trigger_class: %s (expected Down|Pressed|Released|Hold|Tap|Pulse|ChordAction)"), *TriggerClassStr));
        }
        TriggerIndex = Action->Triggers.IndexOfByPredicate([WantedClass](const TObjectPtr<UInputTrigger>& T)
        {
            return T && T->GetClass() == WantedClass;
        });
        if (TriggerIndex == INDEX_NONE)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("No trigger of class %s found on action %s"), *WantedClass->GetName(), *Action->GetName()));
        }
    }

    if (!Action->Triggers.IsValidIndex(TriggerIndex))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("trigger_index %d out of range (action has %d triggers)"), TriggerIndex, Action->Triggers.Num()));
    }

    UInputTrigger* Trigger = Action->Triggers[TriggerIndex];
    if (!Trigger)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Trigger at index %d is null"), TriggerIndex));
    }
    const UClass* TriggerClass = Trigger->GetClass();

    // Apply each provided property by reflection. A property that does not exist on this
    // concrete trigger subclass is an honest error (e.g. hold_time_threshold on a Tap).
    Action->Modify();
    bool bAnyProvided = false;
    FString PropError;  // set to the offending UPROPERTY name if it is absent on this class

    // Write a float UPROPERTY by name if the param was supplied; record an error if absent.
    auto ApplyFloat = [&](const TCHAR* ParamKey, const TCHAR* PropName)
    {
        if (!PropError.IsEmpty())
        {
            return;
        }
        double Value = 0.0;
        if (Params->TryGetNumberField(ParamKey, Value))
        {
            if (FFloatProperty* Prop = FindFProperty<FFloatProperty>(TriggerClass, PropName))
            {
                Prop->SetPropertyValue_InContainer(Trigger, static_cast<float>(Value));
                bAnyProvided = true;
            }
            else
            {
                PropError = PropName;
            }
        }
    };

    // Write a bool UPROPERTY by name if the param was supplied; record an error if absent.
    auto ApplyBool = [&](const TCHAR* ParamKey, const TCHAR* PropName)
    {
        if (!PropError.IsEmpty())
        {
            return;
        }
        bool Value = false;
        if (Params->TryGetBoolField(ParamKey, Value))
        {
            if (FBoolProperty* Prop = FindFProperty<FBoolProperty>(TriggerClass, PropName))
            {
                Prop->SetPropertyValue_InContainer(Trigger, Value);
                bAnyProvided = true;
            }
            else
            {
                PropError = PropName;
            }
        }
    };

    ApplyFloat(TEXT("actuation_threshold"), TEXT("ActuationThreshold"));
    ApplyFloat(TEXT("hold_time_threshold"), TEXT("HoldTimeThreshold"));
    ApplyFloat(TEXT("tap_release_time"),    TEXT("TapReleaseTimeThreshold"));
    ApplyBool(TEXT("is_one_shot"),               TEXT("bIsOneShot"));
    ApplyBool(TEXT("affected_by_time_dilation"), TEXT("bAffectedByTimeDilation"));

    if (!PropError.IsEmpty())
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Property %s does not exist on trigger class %s"), *PropError, *TriggerClass->GetName()));
    }

    if (!bAnyProvided)
    {
        return mcp_common::MakeErrorReply(TEXT("No trigger property provided (actuation_threshold|hold_time_threshold|is_one_shot|tap_release_time|affected_by_time_dilation)"));
    }

    // Echo back the resulting trigger state (round-trip verification).
    TSharedPtr<FJsonObject> TriggerObj = MakeShared<FJsonObject>();
    TriggerObj->SetStringField(TEXT("class"), TriggerClass->GetName());
    SerializeTriggerProps(Trigger, TriggerObj);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("action_path"), Action->GetPathName());
    ResultObj->SetStringField(TEXT("action_name"), Action->GetName());
    ResultObj->SetNumberField(TEXT("trigger_index"), TriggerIndex);
    ResultObj->SetStringField(TEXT("trigger_class"), TriggerClass->GetName());
    ResultObj->SetBoolField(TEXT("properties_set"), bAnyProvided);
    ResultObj->SetObjectField(TEXT("trigger"), TriggerObj);
    return Impl::SaveAndReply(Action, ResultObj, TEXT("set_input_action_trigger_behavior"));
}

// MCP:add_mapping_to_context - Bind a key to an InputAction in a saved Enhanced Input mapping context, addressing both by SHORT ASSET NAME (IMC_Foo / IA_Bar) instead of full paths -- otherwise the same operation as map_input_action, which takes paths. Returns the mapping index, which is what add_input_trigger / add_input_modifier / list_input_triggers address. A context that already maps that key to that action keeps its mapping (added false, already_mapped true, not saved); a new mapping succeeds only when the context reaches disk. Other actions on the same key are not checked: use find_free_input_key first.
//   @param context_name - Short asset name of the InputMappingContext (e.g. IMC_Example).
//   @param action_name - Short asset name of the InputAction (e.g. IA_Example).
//   @param key - Key name to map (e.g. "SpaceBar", "W", "Gamepad_FaceButton_Bottom").
//   @param context_path - Asset path of the Input Mapping Context
//   @param action_path - Asset path of the Input Action
//   @param key_name - Input key name (e.g. SpaceBar, Gamepad_FaceButton_Bottom)
//   @returns { context_name, context_path, action_name, key, index, mapping_count, added, already_mapped, saved, save_error? }
TSharedPtr<FJsonObject> HandleAddMappingToContext(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentInputCommandsImpl;

    FString ContextRef;
    if (!Params->TryGetStringField(TEXT("context_name"), ContextRef))
    {
        Params->TryGetStringField(TEXT("context_path"), ContextRef);
    }
    if (ContextRef.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: context_name (or context_path)"));
    }

    FString ActionRef;
    if (TSharedPtr<FJsonObject> Missing = Impl::ReadActionNameOrPath(Params, ActionRef))
    {
        return Missing;
    }

    FString KeyName;
    if (!Params->TryGetStringField(TEXT("key"), KeyName))
    {
        Params->TryGetStringField(TEXT("key_name"), KeyName);
    }
    if (KeyName.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: key (or key_name)"));
    }

    UInputMappingContext* Context = nullptr;
    UInputAction* Action = nullptr;
    FKey Key;
    if (TSharedPtr<FJsonObject> Failure = Impl::ResolveMappingTarget(ContextRef, ActionRef, KeyName, Context, Action, Key))
    {
        return Failure;
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("context_name"), Context->GetName());
    ResultObj->SetStringField(TEXT("context_path"), Context->GetPathName());
    ResultObj->SetStringField(TEXT("action_name"), Action->GetName());
    ResultObj->SetStringField(TEXT("key"), KeyName);
    return Impl::MapKeyOnce(Context, Action, Key, ResultObj, TEXT("add_mapping_to_context"));
}

// MCP:add_input_trigger - Decide WHEN a key fires its action: append a trigger (Down / Pressed / Released / Hold / Tap / Pulse / ChordAction) so a key can mean tap-to-do-X and hold-to-do-Y instead of consuming two keys. Targets either one key mapping inside a mapping context (pass context + key) or, with no context and no key, the InputAction asset's own Triggers array -- the difference decides whether the rule applies to that binding or to the action everywhere. The edited asset is saved, and the call succeeds only when it reaches disk. Tune it afterwards with set_input_action_trigger_behavior; list_input_trigger_types enumerates the classes.
//   @param context_name - Short asset name of the InputMappingContext.
//   @param action_name - Short asset name of the InputAction.
//   @param key - Key name of the mapping the trigger is attached to.
//   @param trigger_type - One of "Down", "Pressed", "Released", "Hold", "Tap", "Pulse", "ChordAction".
//   @param context_path - Asset path of the Input Mapping Context. When omitted (with no key), the trigger is appended to the InputAction asset's own Triggers array instead of a context mapping.
//   @param action_path - Asset path of the Input Action
//   @param key_name - Input key name (e.g. SpaceBar, Gamepad_FaceButton_Bottom)
//   @returns Context target: { context_name, action_name, key, trigger_type, trigger_class, index, trigger_count, saved, save_error? }; Action target: { action_name, action_path, trigger_type, trigger_class, index, trigger_count, saved, save_error? }
TSharedPtr<FJsonObject> HandleAddInputTrigger(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentInputCommandsImpl;

    FString ContextRef;
    if (!Params->TryGetStringField(TEXT("context_name"), ContextRef))
    {
        Params->TryGetStringField(TEXT("context_path"), ContextRef);
    }
    FString ActionRef;
    if (!Params->TryGetStringField(TEXT("action_name"), ActionRef))
    {
        Params->TryGetStringField(TEXT("action_path"), ActionRef);
    }
    FString KeyName;
    if (!Params->TryGetStringField(TEXT("key"), KeyName))
    {
        Params->TryGetStringField(TEXT("key_name"), KeyName);
    }
    FString TriggerType;
    Params->TryGetStringField(TEXT("trigger_type"), TriggerType);

    // action_path and trigger_type are always required. The mapping context is optional:
    // present -> the trigger is appended to the matching context mapping;
    // absent  -> the trigger is appended to the InputAction asset's own Triggers array.
    if (ActionRef.IsEmpty() || TriggerType.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameters: action_name, trigger_type"));
    }

    UClass* TriggerClass = ResolveTriggerClass(TriggerType);
    if (!TriggerClass)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Invalid trigger_type: %s (expected Down|Pressed|Released|Hold|Tap|Pulse|ChordAction)"), *TriggerType));
    }

    UInputAction* Action = nullptr;
    if (TSharedPtr<FJsonObject> Failure = Impl::FindInputAction(ActionRef, Action))
    {
        return Failure;
    }

    // ---- Action-asset target (no context provided) --------------------------
    if (ContextRef.IsEmpty())
    {
        Action->Modify();
        UInputTrigger* NewTrigger = NewObject<UInputTrigger>(Action, TriggerClass, NAME_None, RF_Transactional);
        if (!NewTrigger)
        {
            return mcp_common::MakeErrorReply(TEXT("Failed to construct trigger object"));
        }
        const int32 ActionIndex = Action->Triggers.Add(NewTrigger);

        TSharedPtr<FJsonObject> ActionResult = MakeShared<FJsonObject>();
        ActionResult->SetStringField(TEXT("action_name"), Action->GetName());
        ActionResult->SetStringField(TEXT("action_path"), Action->GetPathName());
        ActionResult->SetStringField(TEXT("trigger_type"), TriggerType);
        ActionResult->SetStringField(TEXT("trigger_class"), TriggerClass->GetName());
        ActionResult->SetNumberField(TEXT("index"), ActionIndex);
        ActionResult->SetNumberField(TEXT("trigger_count"), Action->Triggers.Num());
        return Impl::SaveAndReply(Action, ActionResult, TEXT("add_input_trigger"));
    }

    // ---- Context-mapping target ---------------------------------------------
    if (KeyName.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter for context target: key (or key_name)"));
    }

    UInputMappingContext* Context = nullptr;
    if (TSharedPtr<FJsonObject> Failure = Impl::FindMappingContext(ContextRef, Context))
    {
        return Failure;
    }
    FKey Key;
    if (TSharedPtr<FJsonObject> Failure = Impl::ParseKey(KeyName, Key))
    {
        return Failure;
    }

    // A mapping that does not exist yet is created, so the trigger has somewhere to live.
    const int32 Index = Impl::FindOrAddMappingIndex(Context, Action, Key);
    if (Index == INDEX_NONE)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to locate or create the requested mapping"));
    }

    Context->Modify();
    UInputTrigger* NewTrigger = NewObject<UInputTrigger>(Context, TriggerClass, NAME_None, RF_Transactional);
    if (!NewTrigger)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to construct trigger object"));
    }
    FEnhancedActionKeyMapping& Mapping = Context->GetMapping(Index);
    Mapping.Triggers.Add(NewTrigger);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("context_name"), Context->GetName());
    ResultObj->SetStringField(TEXT("action_name"), Action->GetName());
    ResultObj->SetStringField(TEXT("key"), KeyName);
    ResultObj->SetStringField(TEXT("trigger_type"), TriggerType);
    ResultObj->SetStringField(TEXT("trigger_class"), TriggerClass->GetName());
    ResultObj->SetNumberField(TEXT("index"), Index);
    ResultObj->SetNumberField(TEXT("trigger_count"), Context->GetMapping(Index).Triggers.Num());
    return Impl::SaveAndReply(Context, ResultObj, TEXT("add_input_trigger"));
}

// MCP:add_input_modifier - Reshape the VALUE a key sends before the action sees it: append a modifier (Negate to flip an axis, Swizzle to remap X/Y/Z, DeadZone for stick drift, Scalar for sensitivity) to a key mapping in a mapping context. This is how one Axis2D action serves WASD -- S is W negated, not a second action. Modifiers change the value, triggers change when it fires (add_input_trigger); list_input_modifier_types enumerates the classes, remove_input_modifier undoes it by index. The context is saved, and the call succeeds only when it reaches disk.
//   @param context_name - Short asset name of the InputMappingContext.
//   @param action_name - Short asset name of the InputAction.
//   @param key - Key name of the mapping the modifier is attached to.
//   @param modifier_type - One of "Negate", "Swizzle", "DeadZone", "Scalar".
//   @param context_path - Asset path of the Input Mapping Context
//   @param action_path - Asset path of the Input Action
//   @param key_name - Input key name (e.g. SpaceBar, Gamepad_FaceButton_Bottom)
//   @returns { context_name, action_name, key, modifier_type, modifier_class, index, modifier_count, saved, save_error? }
TSharedPtr<FJsonObject> HandleAddInputModifier(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentInputCommandsImpl;

    FString ContextRef;
    if (!Params->TryGetStringField(TEXT("context_name"), ContextRef))
    {
        Params->TryGetStringField(TEXT("context_path"), ContextRef);
    }
    FString ActionRef;
    if (!Params->TryGetStringField(TEXT("action_name"), ActionRef))
    {
        Params->TryGetStringField(TEXT("action_path"), ActionRef);
    }
    FString KeyName;
    if (!Params->TryGetStringField(TEXT("key"), KeyName))
    {
        Params->TryGetStringField(TEXT("key_name"), KeyName);
    }
    FString ModifierType;
    Params->TryGetStringField(TEXT("modifier_type"), ModifierType);

    if (ContextRef.IsEmpty() || ActionRef.IsEmpty() || KeyName.IsEmpty() || ModifierType.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameters: context_name, action_name, key, modifier_type"));
    }

    UClass* ModifierClass = ResolveModifierClass(ModifierType);
    if (!ModifierClass)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Invalid modifier_type: %s (expected Negate|Swizzle|DeadZone|Scalar)"), *ModifierType));
    }

    UInputMappingContext* Context = nullptr;
    UInputAction* Action = nullptr;
    FKey Key;
    if (TSharedPtr<FJsonObject> Failure = Impl::ResolveMappingTarget(ContextRef, ActionRef, KeyName, Context, Action, Key))
    {
        return Failure;
    }

    const int32 Index = Impl::FindOrAddMappingIndex(Context, Action, Key);
    if (Index == INDEX_NONE)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to locate or create the requested mapping"));
    }

    Context->Modify();
    UInputModifier* NewModifier = NewObject<UInputModifier>(Context, ModifierClass, NAME_None, RF_Transactional);
    if (!NewModifier)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to construct modifier object"));
    }
    FEnhancedActionKeyMapping& Mapping = Context->GetMapping(Index);
    Mapping.Modifiers.Add(NewModifier);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("context_name"), Context->GetName());
    ResultObj->SetStringField(TEXT("action_name"), Action->GetName());
    ResultObj->SetStringField(TEXT("key"), KeyName);
    ResultObj->SetStringField(TEXT("modifier_type"), ModifierType);
    ResultObj->SetStringField(TEXT("modifier_class"), ModifierClass->GetName());
    ResultObj->SetNumberField(TEXT("index"), Index);
    ResultObj->SetNumberField(TEXT("modifier_count"), Context->GetMapping(Index).Modifiers.Num());
    return Impl::SaveAndReply(Context, ResultObj, TEXT("add_input_modifier"));
}

// MCP:remove_mapping_from_context - Unbind a key from an InputAction in a saved Enhanced Input mapping context -- how you free a key that list_all_input_mappings flagged as conflicting, or retire a verb. Identified by context + action + key, so only the first mapping of that action to that key goes; the assets themselves survive. The engine moves the context's last mapping into the freed slot, so mapping indexes (add_input_trigger, list_input_triggers) can change. The context is saved, and a removal succeeds only when it reaches disk; a mapping that is not there succeeds with removed false and already_absent true, and saves nothing.
//   @param context_name - Short asset name of the InputMappingContext.
//   @param action_name - Short asset name of the InputAction.
//   @param key - Key name of the mapping to remove.
//   @param context_path - Asset path of the Input Mapping Context
//   @param action_path - Asset path of the Input Action
//   @param key_name - Input key name (e.g. SpaceBar, Gamepad_FaceButton_Bottom)
//   @returns { context_name, action_name, key, removed, already_absent, mapping_count, saved, save_error? }
TSharedPtr<FJsonObject> HandleRemoveMappingFromContext(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentInputCommandsImpl;

    FString ContextRef;
    if (!Params->TryGetStringField(TEXT("context_name"), ContextRef))
    {
        Params->TryGetStringField(TEXT("context_path"), ContextRef);
    }
    FString ActionRef;
    if (!Params->TryGetStringField(TEXT("action_name"), ActionRef))
    {
        Params->TryGetStringField(TEXT("action_path"), ActionRef);
    }
    FString KeyName;
    if (!Params->TryGetStringField(TEXT("key"), KeyName))
    {
        Params->TryGetStringField(TEXT("key_name"), KeyName);
    }

    if (ContextRef.IsEmpty() || ActionRef.IsEmpty() || KeyName.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameters: context_name, action_name, key"));
    }

    UInputMappingContext* Context = nullptr;
    UInputAction* Action = nullptr;
    FKey Key;
    if (TSharedPtr<FJsonObject> Failure = Impl::ResolveMappingTarget(ContextRef, ActionRef, KeyName, Context, Action, Key))
    {
        return Failure;
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("context_name"), Context->GetName());
    ResultObj->SetStringField(TEXT("action_name"), Action->GetName());
    ResultObj->SetStringField(TEXT("key"), KeyName);

    // Idempotency: a mapping that is not there is already the state asked for, so nothing changes
    // and nothing is saved.
    const int32 CountBefore = Context->GetMappings().Num();
    if (Impl::FindMappingIndex(Context, Action, Key) == INDEX_NONE)
    {
        ResultObj->SetBoolField(TEXT("removed"), false);
        ResultObj->SetBoolField(TEXT("already_absent"), true);
        ResultObj->SetNumberField(TEXT("mapping_count"), CountBefore);
        ResultObj->SetBoolField(TEXT("saved"), false);
        return mcp_common::MakeOkReply(ResultObj);
    }

    Context->Modify();
    Context->UnmapKey(Action, Key);

    const int32 CountAfter = Context->GetMappings().Num();
    const bool bRemoved = CountAfter < CountBefore;
    ResultObj->SetBoolField(TEXT("removed"), bRemoved);
    ResultObj->SetBoolField(TEXT("already_absent"), false);
    ResultObj->SetNumberField(TEXT("mapping_count"), CountAfter);
    if (!bRemoved)
    {
        ResultObj->SetBoolField(TEXT("saved"), false);
        return mcp_common::MakeOutcomeReply(ResultObj, false,
            TEXT("remove_mapping_from_context: UnmapKey left the mapping in the context; nothing was saved."));
    }
    return Impl::SaveAndReply(Context, ResultObj, TEXT("remove_mapping_from_context"));
}

// ===========================================================================
// Input reflection + triggers discovery (all headless-friendly).
// ===========================================================================

namespace
{
    /** Enumerate concrete (non-abstract, non-deprecated) UClass subclasses of BaseClass. */
    template <typename BaseT>
    void EnumerateSubclasses(TArray<TSharedPtr<FJsonValue>>& OutArr)
    {
        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Cls = *It;
            if (!Cls || !Cls->IsChildOf(BaseT::StaticClass())) { continue; }
            if (Cls->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)) { continue; }
            TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
            O->SetStringField(TEXT("class_name"), Cls->GetName());
            O->SetStringField(TEXT("display_name"), Cls->GetDisplayNameText().ToString());
            OutArr.Add(MakeShared<FJsonValueObject>(O));
        }
    }
}

// MCP:discover_input_types - One-call inventory of what Enhanced Input offers in this build: every value type plus every concrete modifier and trigger subclass, including any the project or a plugin added. Run it before add_input_trigger / add_input_modifier so you pass a class that actually exists instead of guessing a name. Superset of list_input_modifier_types and list_input_trigger_types, which return the same lists one at a time. Read-only.
//   @returns { modifier_count, trigger_count, modifier_types: [...], trigger_types: [...] }
TSharedPtr<FJsonObject> HandleDiscoverInputTypes(const TSharedPtr<FJsonObject>& Params)
{
    TArray<TSharedPtr<FJsonValue>> Modifiers, Triggers;
    EnumerateSubclasses<UInputModifier>(Modifiers);
    EnumerateSubclasses<UInputTrigger>(Triggers);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetNumberField(TEXT("modifier_count"), Modifiers.Num());
    Data->SetNumberField(TEXT("trigger_count"), Triggers.Num());
    Data->SetArrayField(TEXT("modifier_types"), Modifiers);
    Data->SetArrayField(TEXT("trigger_types"), Triggers);
    return mcp_common::MakeOkReply(Data);
}

// MCP:list_input_modifier_types - List the concrete input modifier classes available in this build (Negate, Swizzle, DeadZone, Scalar and any project/plugin ones) -- the valid values for add_input_modifier. Read-only. Use discover_input_types to get modifiers, triggers and value types in a single call.
//   @returns { count, types: [...] }
TSharedPtr<FJsonObject> HandleListInputModifierTypes(const TSharedPtr<FJsonObject>& Params)
{
    TArray<TSharedPtr<FJsonValue>> Out;
    EnumerateSubclasses<UInputModifier>(Out);
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetNumberField(TEXT("count"), Out.Num());
    Data->SetArrayField(TEXT("types"), Out);
    return mcp_common::MakeOkReply(Data);
}

// MCP:list_input_trigger_types - List the concrete input trigger classes available in this build (Down, Pressed, Released, Hold, Tap, Pulse, ChordAction and any project/plugin ones) -- the valid values for add_input_trigger. Read-only. Use discover_input_types to get triggers, modifiers and value types in a single call.
//   @returns { count, types: [...] }
TSharedPtr<FJsonObject> HandleListInputTriggerTypes(const TSharedPtr<FJsonObject>& Params)
{
    TArray<TSharedPtr<FJsonValue>> Out;
    EnumerateSubclasses<UInputTrigger>(Out);
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetNumberField(TEXT("count"), Out.Num());
    Data->SetArrayField(TEXT("types"), Out);
    return mcp_common::MakeOkReply(Data);
}

// MCP:get_available_keys - List the input key NAMES the engine knows (keyboard, mouse, gamepad, touch), with an optional substring filter -- how you find the exact spelling a binding call needs ("SpaceBar", "Gamepad_FaceButton_Bottom", "LeftMouseButton") instead of guessing. "Available" here means the engine defines it, NOT that it is unbound: for keys that are actually free use find_free_input_key, and for what is already taken list_all_input_mappings.
//   @param filter - Optional case-insensitive substring to filter key names
//   @returns { count, keys: [...] }
TSharedPtr<FJsonObject> HandleGetAvailableKeys(const TSharedPtr<FJsonObject>& Params)
{
    FString Filter;
    Params->TryGetStringField(TEXT("filter"), Filter);

    TArray<TSharedPtr<FJsonValue>> Out;
    TArray<FKey> AllKeys;
    EKeys::GetAllKeys(AllKeys);
    for (const FKey& Key : AllKeys)
    {
        const FString Name = Key.GetDisplayName().ToString();
        if (!Filter.IsEmpty() && !Name.Contains(Filter, ESearchCase::IgnoreCase)) { continue; }
        TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("name"), Name);
        O->SetStringField(TEXT("display_name"), Name);
        O->SetBoolField(TEXT("is_valid"), Key.IsValid());
        Out.Add(MakeShared<FJsonValueObject>(O));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetNumberField(TEXT("count"), Out.Num());
    Data->SetArrayField(TEXT("keys"), Out);
    return mcp_common::MakeOkReply(Data);
}

// MCP:list_input_triggers - List the triggers attached to ONE key mapping inside an Enhanced Input mapping context, addressed by mapping_index, with each trigger's class and index. The read-back for add_input_trigger and the way to find the trigger_index that remove_input_trigger needs. Scoped to the binding: triggers living on the InputAction asset itself show up in get_input_action instead.
//   @param ctx_path - Asset path of the UInputMappingContext
//   @param mapping_index - 0-based index of the FEnhancedActionKeyMapping within Mappings[]
//   @returns { count, triggers: [{index, class_name, display_name}] }
TSharedPtr<FJsonObject> HandleListInputTriggers(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentInputCommandsImpl;

    FString CtxPath;
    int32 MappingIdx = 0;
    if (TSharedPtr<FJsonObject> Missing = Impl::ReadMappingAddress(Params, CtxPath, MappingIdx))
    {
        return Missing;
    }

    UInputMappingContext* Ctx = Impl::LoadAssetAt<UInputMappingContext>(CtxPath);
    if (!Ctx)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("InputMappingContext not found: %s"), *CtxPath));
    }
    const TArray<FEnhancedActionKeyMapping>& MappingsRef = Ctx->GetMappings();
    if (!MappingsRef.IsValidIndex(MappingIdx))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("mapping_index %d out of range (count=%d)"), MappingIdx, MappingsRef.Num()));
    }

    const FEnhancedActionKeyMapping& Mapping = MappingsRef[MappingIdx];
    TArray<TSharedPtr<FJsonValue>> TriggerJson;
    int32 Idx = 0;
    for (UInputTrigger* Trig : Mapping.Triggers)
    {
        if (!Trig) { ++Idx; continue; }
        TSharedPtr<FJsonObject> T = MakeShared<FJsonObject>();
        T->SetNumberField(TEXT("index"), Idx);
        T->SetStringField(TEXT("class_name"), Trig->GetClass()->GetName());
        T->SetStringField(TEXT("display_name"), Trig->GetClass()->GetDisplayNameText().ToString());
        TriggerJson.Add(MakeShared<FJsonValueObject>(T));
        ++Idx;
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetNumberField(TEXT("count"), TriggerJson.Num());
    Data->SetArrayField(TEXT("triggers"), TriggerJson);
    return mcp_common::MakeOkReply(Data);
}

// MCP:remove_input_trigger - Delete one trigger from a key mapping in an Enhanced Input mapping context, by mapping_index + trigger_index -- e.g. dropping a Hold so the key fires immediately again -- and save the context; the call succeeds only when the context reaches disk. Get the indices from list_input_triggers; they shift after a removal, so work from the highest down.
//   @param ctx_path - Asset path of the UInputMappingContext
//   @param mapping_index - 0-based index of the FEnhancedActionKeyMapping
//   @param trigger_index - 0-based index of the trigger within Mapping.Triggers
//   @returns { applied, removed_class_name, remaining_count, saved, save_error? }
TSharedPtr<FJsonObject> HandleRemoveInputTrigger(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentInputCommandsImpl;

    FString CtxPath;
    int32 MappingIdx = 0;
    if (TSharedPtr<FJsonObject> Missing = Impl::ReadMappingAddress(Params, CtxPath, MappingIdx))
    {
        return Missing;
    }
    int32 TriggerIdx = 0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("trigger_index"), TriggerIdx)) { return Missing; }

    UInputMappingContext* Ctx = Impl::LoadAssetAt<UInputMappingContext>(CtxPath);
    if (!Ctx) { return mcp_common::MakeErrorReply(FString::Printf(TEXT("InputMappingContext not found: %s"), *CtxPath)); }
    if (!Ctx->GetMappings().IsValidIndex(MappingIdx))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("mapping_index %d out of range"), MappingIdx));
    }
    FEnhancedActionKeyMapping& Mapping = Ctx->GetMapping(MappingIdx);
    if (!Mapping.Triggers.IsValidIndex(TriggerIdx))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("trigger_index %d out of range (count=%d)"), TriggerIdx, Mapping.Triggers.Num()));
    }
    FString RemovedClass = Mapping.Triggers[TriggerIdx] ? Mapping.Triggers[TriggerIdx]->GetClass()->GetName() : TEXT("");
    const int32 CountBefore = Mapping.Triggers.Num();
    Ctx->Modify();
    Mapping.Triggers.RemoveAt(TriggerIdx);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("applied"), Mapping.Triggers.Num() == CountBefore - 1);
    Data->SetStringField(TEXT("removed_class_name"), RemovedClass);
    Data->SetNumberField(TEXT("remaining_count"), Mapping.Triggers.Num());
    return Impl::SaveAndReply(Ctx, Data, TEXT("remove_input_trigger"));
}

// MCP:remove_input_modifier - Delete one modifier from a key mapping in an Enhanced Input mapping context, by mapping_index + modifier_index -- e.g. removing a Negate that flipped an axis the wrong way -- and save the context; the call succeeds only when the context reaches disk. Indices shift after a removal, so work from the highest down. add_input_modifier is the inverse.
//   @param ctx_path - Asset path of the UInputMappingContext
//   @param mapping_index - 0-based index of the FEnhancedActionKeyMapping
//   @param modifier_index - 0-based index of the modifier within Mapping.Modifiers
//   @returns { applied, removed_class_name, remaining_count, saved, save_error? }
TSharedPtr<FJsonObject> HandleRemoveInputModifier(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentInputCommandsImpl;

    FString CtxPath;
    int32 MappingIdx = 0;
    if (TSharedPtr<FJsonObject> Missing = Impl::ReadMappingAddress(Params, CtxPath, MappingIdx))
    {
        return Missing;
    }
    int32 ModifierIdx = 0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("modifier_index"), ModifierIdx)) { return Missing; }

    UInputMappingContext* Ctx = Impl::LoadAssetAt<UInputMappingContext>(CtxPath);
    if (!Ctx) { return mcp_common::MakeErrorReply(FString::Printf(TEXT("InputMappingContext not found: %s"), *CtxPath)); }
    if (!Ctx->GetMappings().IsValidIndex(MappingIdx))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("mapping_index %d out of range"), MappingIdx));
    }
    FEnhancedActionKeyMapping& Mapping = Ctx->GetMapping(MappingIdx);
    if (!Mapping.Modifiers.IsValidIndex(ModifierIdx))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("modifier_index %d out of range (count=%d)"), ModifierIdx, Mapping.Modifiers.Num()));
    }
    FString RemovedClass = Mapping.Modifiers[ModifierIdx] ? Mapping.Modifiers[ModifierIdx]->GetClass()->GetName() : TEXT("");
    const int32 CountBefore = Mapping.Modifiers.Num();
    Ctx->Modify();
    Mapping.Modifiers.RemoveAt(ModifierIdx);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("applied"), Mapping.Modifiers.Num() == CountBefore - 1);
    Data->SetStringField(TEXT("removed_class_name"), RemovedClass);
    Data->SetNumberField(TEXT("remaining_count"), Mapping.Modifiers.Num());
    return Impl::SaveAndReply(Ctx, Data, TEXT("remove_input_modifier"));
}

// Enhanced Input user settings (runtime key remap)

// MCP:register_input_key_mapping - Remaps a player input mapping to a new key at runtime via UEnhancedInputUserSettings::MapPlayerKey (EnhancedInputUserSettings.h). Wraps FMapPlayerKeyArgs with the supplied mapping name + new key + slot. The call fails when the engine refuses the remap, with its reasons in failure_tags.
//   @param player_controller_name - Optional editor-world APlayerController actor name (defaults to first local player)
//   @param player_index - Optional local player index used to resolve the PlayerController (default 0).
//   @param mapping_name - FName of the source mapping inside an already-mapped IMC (required)
//   @param new_key - FKey name (e.g. "Gamepad_FaceButton_Bottom") (required)
//   @param slot - "First" | "Second" | "Third" | "Fourth" (default "First"); EPlayerMappableKeySlot enum
//   @returns { applied: bool, mapping_name: string, new_key: string, slot: string, failure_tags: [string] }
TSharedPtr<FJsonObject> HandleRegisterInputKeyMapping(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentInputCommandsImpl;

    UEnhancedInputUserSettings* UserSettings = nullptr;
    if (TSharedPtr<FJsonObject> Failure = Impl::ResolveUserSettings(ResolvePlayerController(Params), UserSettings))
    {
        return Failure;
    }

    FString MappingNameStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("mapping_name"), MappingNameStr)) { return Missing; }

    FString NewKeyStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("new_key"), NewKeyStr)) { return Missing; }
    const FKey NewKey(*NewKeyStr);
    if (!NewKey.IsValid())
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("'new_key' '%s' is not a valid FKey"), *NewKeyStr));

    FString SlotStr;
    const EPlayerMappableKeySlot Slot = Impl::ReadMappableKeySlot(Params, SlotStr);

    FMapPlayerKeyArgs Args;
    Args.MappingName = FName(*MappingNameStr);
    Args.Slot = Slot;
    Args.NewKey = NewKey;

    FGameplayTagContainer FailureReason;
    UserSettings->MapPlayerKey(Args, FailureReason);

    const bool bApplied = FailureReason.IsEmpty();
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("applied"), bApplied);
    Result->SetStringField(TEXT("mapping_name"), MappingNameStr);
    Result->SetStringField(TEXT("new_key"), NewKeyStr);
    Result->SetStringField(TEXT("slot"), SlotStr);
    Result->SetArrayField(TEXT("failure_tags"), mcp_common::GameplayTagsToJson(FailureReason));
    return mcp_common::MakeOutcomeReply(Result, bApplied, bApplied ? FString() : FString::Printf(
        TEXT("register_input_key_mapping: the engine refused the remap (%s)."), *FailureReason.ToStringSimple()));
}

// MCP:unregister_input_key_mapping - Resets a player input mapping back to its default key at runtime via UEnhancedInputUserSettings::UnMapPlayerKey (EnhancedInputUserSettings.h). Inverse of register_input_key_mapping; locates the mapping by name + slot and clears the player override (does not need new_key, does not delete the row). The call fails when the engine refuses it, with its reasons in failure_tags.
//   @param player_controller_name - Optional editor-world APlayerController actor name (defaults to first local player)
//   @param player_index - Optional local player index used to resolve the PlayerController (default 0).
//   @param mapping_name - FName of the source mapping inside an already-mapped IMC (required)
//   @param slot - "First" | "Second" | "Third" | "Fourth" (default "First"); EPlayerMappableKeySlot enum
//   @returns { applied: bool, mapping_name: string, slot: string, failure_tags: [string] }
TSharedPtr<FJsonObject> HandleUnregisterInputKeyMapping(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentInputCommandsImpl;

    UEnhancedInputUserSettings* UserSettings = nullptr;
    if (TSharedPtr<FJsonObject> Failure = Impl::ResolveUserSettings(ResolvePlayerController(Params), UserSettings))
    {
        return Failure;
    }

    FString MappingNameStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("mapping_name"), MappingNameStr)) { return Missing; }

    FString SlotStr;
    const EPlayerMappableKeySlot Slot = Impl::ReadMappableKeySlot(Params, SlotStr);

    FMapPlayerKeyArgs Args;
    Args.MappingName = FName(*MappingNameStr);
    Args.Slot = Slot;

    FGameplayTagContainer FailureReason;
    UserSettings->UnMapPlayerKey(Args, FailureReason);

    const bool bApplied = FailureReason.IsEmpty();
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("applied"), bApplied);
    Result->SetStringField(TEXT("mapping_name"), MappingNameStr);
    Result->SetStringField(TEXT("slot"), SlotStr);
    Result->SetArrayField(TEXT("failure_tags"), mcp_common::GameplayTagsToJson(FailureReason));
    return mcp_common::MakeOutcomeReply(Result, bApplied, bApplied ? FString() : FString::Printf(
        TEXT("unregister_input_key_mapping: the engine refused to reset the mapping (%s)."), *FailureReason.ToStringSimple()));
}

// MCP:trigger_input_action - Inject a value for an Enhanced Input action into a local player's UEnhancedInputLocalPlayerSubsystem in a running PIE or game session, so the real bound handlers run with the action's own modifiers and triggers (headless key injection never reaches Enhanced Input). dispatched true does not prove a handler fired: compare get_input_action_state before and after.
//   @param action - Full object path of a UInputAction, or its short asset name (required).
//   @param value_json - The value to inject, coerced to the action's ValueType: bool (Boolean), number (Axis1D), or a [x,y]/[x,y,z] array or {x,y,z} object (Axis2D/Axis3D). If omitted, Boolean actions default to true (pressed) and axis actions default to zero.
//   @param player_index - Local player index used to resolve the PlayerController (default 0).
//   @param hold_seconds - Optional float. When > 0, the value is re-injected every tick for this many seconds (so a movement AXIS actually moves the pawn), then a final neutral/zero value is injected to release. Fire-and-forget: the command returns immediately and the hold runs across the next hold_seconds of PIE. Omit (or 0) for a single one-shot sample.
//   @param world - Optional world selector: absent/'pie' (default: PlayWorld, else the first PIE context) | 'server' (the PIE world with authority) | 'client' | 'pie:N' | 'game' (alias of 'pie'). With 2+ players the default may drive the OTHER player's pawn.
//   @param action_path - Alias of action, read when action is absent or empty: InputAction object path or short asset name.
//   @param action_name - Alias of action, read only when action is absent or empty and action_path is absent: InputAction object path or short asset name.
//   @returns { dispatched: bool, note: string, action_path: string, value_type: string, injected_value: [x,y,z], player_index: number, hold_seconds: number, world: string, world_name: string|null, world_package: string|null, world_type: string|null, net_mode: string|null, pie_instance: number }
TSharedPtr<FJsonObject> HandleTriggerInputAction(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentInputCommandsImpl;

    UInputAction* Action = nullptr;
    if (TSharedPtr<FJsonObject> Failure = Impl::ResolveRuntimeAction(Params, Action))
    {
        return Failure;
    }

    // Resolve the local player in the LIVE PIE/Game world (Enhanced Input injection
    // only reaches real bound handlers on a playing session). Prefer a PIE/Game
    // world context; fall back to the shared ResolvePlayerController helper.
    int32 PlayerIndex = 0;
    Params->TryGetNumberField(TEXT("player_index"), PlayerIndex);

    // CANONICAL WORLD RESOLUTION. Input is injected into a LOCAL player's Enhanced Input
    // subsystem, so in a 2-player PIE session "the first context" may well drive the OTHER
    // player's pawn -- which reads exactly like an input binding that does not work.
    // Default: 'pie'; `world` accepts 'server'/'client'/'pie:N'.
    FString WorldSelector;
    UWorld* ResolvedWorld = Impl::ResolveRuntimeWorld(Params, WorldSelector);
    APlayerController* PC = ResolvedWorld ? Impl::FindLocalController(ResolvedWorld, PlayerIndex) : nullptr;
    if (!PC)
    {
        PC = ResolvePlayerController(Params);
    }
    if (!PC)
    {
        return mcp_common::MakeErrorReply(TEXT("trigger_input_action: no local PlayerController found (start PIE / a game session first)"));
    }

    ULocalPlayer* LP = PC->GetLocalPlayer();
    if (!LP)
    {
        return mcp_common::MakeErrorReply(TEXT("trigger_input_action: PlayerController has no ULocalPlayer"));
    }

    UEnhancedInputLocalPlayerSubsystem* Subsystem = LP->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>();
    if (!Subsystem)
    {
        return mcp_common::MakeErrorReply(TEXT("trigger_input_action: no UEnhancedInputLocalPlayerSubsystem on this LocalPlayer (project may not use Enhanced Input at runtime)"));
    }

    // Build an FInputActionValue matching the action's ValueType from value_json.
    const EInputActionValueType ValueType = Action->ValueType;
    FVector RawVec = FVector::ZeroVector;
    bool bValueProvided = false;

    bool bBoolVal = false;
    double NumVal = 0.0;
    const TArray<TSharedPtr<FJsonValue>>* ArrVal = nullptr;
    const TSharedPtr<FJsonObject>* ObjVal = nullptr;

    if (Params->TryGetBoolField(TEXT("value_json"), bBoolVal))
    {
        RawVec.X = bBoolVal ? 1.0 : 0.0;
        bValueProvided = true;
    }
    else if (Params->TryGetNumberField(TEXT("value_json"), NumVal))
    {
        RawVec.X = NumVal;
        bValueProvided = true;
    }
    else if (Params->TryGetArrayField(TEXT("value_json"), ArrVal) && ArrVal)
    {
        if (ArrVal->Num() > 0) { RawVec.X = (*ArrVal)[0]->AsNumber(); }
        if (ArrVal->Num() > 1) { RawVec.Y = (*ArrVal)[1]->AsNumber(); }
        if (ArrVal->Num() > 2) { RawVec.Z = (*ArrVal)[2]->AsNumber(); }
        bValueProvided = true;
    }
    else if (Params->TryGetObjectField(TEXT("value_json"), ObjVal) && ObjVal && ObjVal->IsValid())
    {
        (*ObjVal)->TryGetNumberField(TEXT("x"), RawVec.X);
        (*ObjVal)->TryGetNumberField(TEXT("y"), RawVec.Y);
        (*ObjVal)->TryGetNumberField(TEXT("z"), RawVec.Z);
        bValueProvided = true;
    }

    if (!bValueProvided && ValueType == EInputActionValueType::Boolean)
    {
        // Sensible default for a button: a press (value 1.0).
        RawVec.X = 1.0;
    }

    // FInputActionValue(EInputActionValueType, FVector) zeroes unused components to
    // match the type, so a bool/Axis1D action ignores Y/Z automatically.
    const FInputActionValue InjectedValue(ValueType, RawVec);

    // Empty modifier/trigger arrays: the action's own asset-level modifiers and
    // triggers are still applied by the injection pipeline.
    const TArray<UInputModifier*> NoModifiers;
    const TArray<UInputTrigger*> NoTriggers;
    Subsystem->InjectInputForAction(Action, InjectedValue, NoModifiers, NoTriggers);

    // Optional sustained hold: a single injection is one value sample, so a movement axis won't
    // move the pawn. When hold_seconds > 0, re-inject the same value every tick against the LIVE
    // PIE subsystem, then inject a final neutral (zero) value to release. Fire-and-forget: guarded
    // by weak pointers each tick so a mid-hold PIE stop just unregisters the ticker safely.
    double HoldSeconds = 0.0;
    Params->TryGetNumberField(TEXT("hold_seconds"), HoldSeconds);
    if (HoldSeconds > 0.0)
    {
        TWeakObjectPtr<ULocalPlayer> WeakLP(LP);
        TWeakObjectPtr<UInputAction> WeakAction(Action);
        const FInputActionValue HeldValue = InjectedValue;
        const EInputActionValueType HeldType = ValueType;
        const double TotalHold = HoldSeconds;
        TSharedRef<double> Elapsed = MakeShared<double>(0.0);
        FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
            [WeakLP, WeakAction, HeldValue, HeldType, TotalHold, Elapsed](float DeltaTime) -> bool
            {
                ULocalPlayer* LPNow = WeakLP.Get();
                UInputAction* ActionNow = WeakAction.Get();
                if (!LPNow || !ActionNow || !LPNow->GetWorld())
                {
                    return false; // PIE gone -> unregister
                }
                UEnhancedInputLocalPlayerSubsystem* SubNow = LPNow->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>();
                if (!SubNow)
                {
                    return false;
                }
                *Elapsed += DeltaTime;
                if (*Elapsed >= TotalHold)
                {
                    // Final neutral sample = release.
                    SubNow->InjectInputForAction(ActionNow, FInputActionValue(HeldType, FVector::ZeroVector),
                        TArray<UInputModifier*>(), TArray<UInputTrigger*>());
                    return false; // done -> unregister
                }
                SubNow->InjectInputForAction(ActionNow, HeldValue, TArray<UInputModifier*>(), TArray<UInputTrigger*>());
                return true; // keep ticking
            }), 0.0f);
    }

    TArray<TSharedPtr<FJsonValue>> ValueArr = mcp_common::VectorToJsonArray(RawVec);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("dispatched"), true);
    Result->SetStringField(TEXT("action_path"), Action->GetPathName());
    Result->SetStringField(TEXT("value_type"), Impl::ValueTypeLabel(ValueType, TEXT("Boolean")));
    Result->SetArrayField(TEXT("injected_value"), ValueArr);
    Result->SetNumberField(TEXT("player_index"), PlayerIndex);
    Result->SetNumberField(TEXT("hold_seconds"), HoldSeconds);
    // WHICH world's local player received the injection. Driving the wrong client is
    // indistinguishable from an input binding that never fires.
    Result->SetStringField(TEXT("world"), WorldSelector);
    mcp_common::AddWorldEvidence(Result, PC ? PC->GetWorld() : ResolvedWorld);
    Result->SetStringField(TEXT("note"),
        HoldSeconds > 0.0
        ? TEXT("Injected via UEnhancedInputLocalPlayerSubsystem::InjectInputForAction, then SUSTAINED: the value is re-injected every tick for hold_seconds against the live PIE subsystem, then a zero value is injected to release. Fire-and-forget -- the command returned immediately and the hold runs across the next hold_seconds of PIE (a mid-hold PIE stop unregisters the ticker safely).")
        : TEXT("Injected via UEnhancedInputLocalPlayerSubsystem::InjectInputForAction. This runs the action's modifiers/triggers and fires the real bound handlers on the next input processing tick. Injection only affects an active PIE/game session; a single call delivers one value sample (for a movement axis or a Hold/press-and-release, pass hold_seconds to sustain the value across ticks, then it auto-releases)."));
    return mcp_common::MakeOkReply(Result);
}

// get_input_action_state
// ---------------------------------------------------------------------------- //
// MCP:get_input_action_state - Read the live Enhanced Input state of one InputAction for a local player in a running PIE or game session: is_tracked, trigger_events, value, last_triggered_world_time and the mapping contexts that carry it, applied or not. It is the oracle for whether an input fired, since inject_key_input and trigger_input_action succeed either way: read it before and after, and an advancing last_triggered_world_time is the proof. is_mapped_in_applied_context false means no applied context maps it (removed on mounting, in a vehicle or a menu); is_tracked false means the engine is not evaluating it, typically an unpossessed pawn. In co-op, world picks which local player is read.
//   @param action - InputAction to read: full object path or short asset name. Aliases: action_path, action_name.
//   @param player_index - Local player index (default 0).
//   @param world - World selector: 'pie' (default) | 'server' | 'client' | 'pie:N' | 'game'.
//   @param action_path - Alias of action, read when action is absent or empty: InputAction object path or short asset name.
//   @param action_name - Alias of action, read only when action is absent or empty and action_path is absent: InputAction object path or short asset name.
//   @returns { action_path: string, action_name: string, value_type: string, is_enhanced_input: bool, is_tracked: bool, is_mapped_in_applied_context: bool, trigger_event: number, trigger_events: array, value: array, elapsed_seconds: number, triggered_seconds: number, last_triggered_world_time: number, world_time_seconds: number, contexts_mapping_action: array, contexts_mapping_action_count: number, applied_context_count: number, player_index: number, player_controller: string, player_input_class: string, world: string, note: string }

TSharedPtr<FJsonObject> HandleGetInputActionState(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentInputCommandsImpl;

    UInputAction* Action = nullptr;
    if (TSharedPtr<FJsonObject> Failure = Impl::ResolveRuntimeAction(Params, Action))
    {
        return Failure;
    }

    // Same canonical world resolution as trigger_input_action: reading the WRONG local
    // player's Enhanced Input in a 2-client session looks exactly like a dead binding.
    int32 PlayerIndex = 0;
    Params->TryGetNumberField(TEXT("player_index"), PlayerIndex);

    FString WorldSelector;
    UWorld* ResolvedWorld = Impl::ResolveRuntimeWorld(Params, WorldSelector);
    APlayerController* PC = ResolvedWorld ? Impl::FindLocalController(ResolvedWorld, PlayerIndex) : nullptr;
    if (!PC)
    {
        PC = ResolvePlayerController(Params);
    }
    if (!PC)
    {
        return mcp_common::MakeErrorReply(TEXT("get_input_action_state: no local PlayerController found (start PIE / a game session first)"));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("action_path"), Action->GetPathName());
    Result->SetStringField(TEXT("action_name"), Action->GetName());
    Result->SetStringField(TEXT("value_type"), Impl::ValueTypeLabel(Action->ValueType, TEXT("Boolean")));
    Result->SetNumberField(TEXT("player_index"), PlayerIndex);
    Result->SetStringField(TEXT("player_controller"), PC->GetName());
    Result->SetNumberField(TEXT("world_time_seconds"), PC->GetWorld() ? PC->GetWorld()->GetTimeSeconds() : 0.0);

    UEnhancedPlayerInput* EPI = Cast<UEnhancedPlayerInput>(PC->PlayerInput);
    Result->SetStringField(TEXT("player_input_class"), PC->PlayerInput ? PC->PlayerInput->GetClass()->GetName() : TEXT("None"));
    Result->SetBoolField(TEXT("is_enhanced_input"), EPI != nullptr);
    if (!EPI)
    {
        // Not an error: a project (or a pawn) can legitimately be on legacy input, and
        // saying so is the measurement.
        Result->SetBoolField(TEXT("is_tracked"), false);
        Result->SetBoolField(TEXT("is_mapped_in_applied_context"), false);
        Result->SetStringField(TEXT("note"), TEXT("This PlayerController's PlayerInput is NOT a UEnhancedPlayerInput, so no Enhanced Input action state exists to read."));
        Result->SetStringField(TEXT("world"), WorldSelector);
        mcp_common::AddWorldEvidence(Result, PC->GetWorld() ? PC->GetWorld() : ResolvedWorld);
        return mcp_common::MakeOkReply(Result);
    }

    // Which contexts could deliver THIS action, and which of them are pushed right now.
    // NOTE: UEnhancedPlayerInput::GetAppliedInputContextData() is PROTECTED (C2248 from outside the class),
    // so the public route is the subsystem's HasMappingContext() asked per candidate context.
    // That is also the more useful answer than a bare list of applied contexts: it separates
    // "nothing maps this action" from "something maps it but that context is not pushed".
    TArray<TSharedPtr<FJsonValue>> ContextArr;
    bool bMappedInApplied = false;
    int32 AppliedCount = 0;
    UEnhancedInputLocalPlayerSubsystem* Subsystem = nullptr;
    if (ULocalPlayer* LP = PC->GetLocalPlayer())
    {
        Subsystem = LP->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>();
    }
    for (TObjectIterator<UInputMappingContext> It; It; ++It)
    {
        UInputMappingContext* Ctx = *It;
        if (!Ctx || Ctx->HasAnyFlags(RF_ClassDefaultObject)) { continue; }

        int32 MappingsForThisAction = 0;
        for (const FEnhancedActionKeyMapping& Mapping : Ctx->GetMappings())
        {
            if (Mapping.Action == Action) { ++MappingsForThisAction; }
        }
        if (MappingsForThisAction == 0) { continue; }

        int32 FoundPriority = 0;
        const bool bApplied = Subsystem ? Subsystem->HasMappingContext(Ctx, FoundPriority) : false;
        if (bApplied) { bMappedInApplied = true; ++AppliedCount; }

        TSharedPtr<FJsonObject> CtxObj = MakeShared<FJsonObject>();
        CtxObj->SetStringField(TEXT("context_path"), Ctx->GetPathName());
        CtxObj->SetStringField(TEXT("context_name"), Ctx->GetName());
        CtxObj->SetNumberField(TEXT("mappings_for_this_action"), MappingsForThisAction);
        CtxObj->SetBoolField(TEXT("is_applied"), bApplied);
        CtxObj->SetNumberField(TEXT("applied_priority"), bApplied ? FoundPriority : -1);
        ContextArr.Add(MakeShared<FJsonValueObject>(CtxObj));
    }
    Result->SetArrayField(TEXT("contexts_mapping_action"), ContextArr);
    Result->SetNumberField(TEXT("contexts_mapping_action_count"), ContextArr.Num());
    Result->SetNumberField(TEXT("applied_context_count"), AppliedCount);
    Result->SetBoolField(TEXT("is_mapped_in_applied_context"), bMappedInApplied);

    // The action instance itself. The engine only keeps one for actions it is actually
    // evaluating, so a null here IS a measurement: nothing is driving this action.
    const FInputActionInstance* Inst = EPI->FindActionInstanceData(Action);
    Result->SetBoolField(TEXT("is_tracked"), Inst != nullptr);
    if (Inst)
    {
        const ETriggerEvent Ev = Inst->GetTriggerEvent();
        Result->SetNumberField(TEXT("trigger_event"), static_cast<int32>(Ev));

        // ETriggerEvent is a BITMASK (None=0, Triggered=1<<0, Started=1<<1, ...), so more
        // than one can be set in the same frame. Reporting a single name would lose that.
        TArray<TSharedPtr<FJsonValue>> EvArr;
        auto AddIfSet = [&EvArr, Ev](ETriggerEvent Flag, const TCHAR* Name)
        {
            if ((static_cast<uint8>(Ev) & static_cast<uint8>(Flag)) != 0)
            {
                EvArr.Add(MakeShared<FJsonValueString>(Name));
            }
        };
        AddIfSet(ETriggerEvent::Triggered, TEXT("Triggered"));
        AddIfSet(ETriggerEvent::Started,   TEXT("Started"));
        AddIfSet(ETriggerEvent::Ongoing,   TEXT("Ongoing"));
        AddIfSet(ETriggerEvent::Canceled,  TEXT("Canceled"));
        AddIfSet(ETriggerEvent::Completed, TEXT("Completed"));
        if (EvArr.Num() == 0)
        {
            EvArr.Add(MakeShared<FJsonValueString>(TEXT("None")));
        }
        Result->SetArrayField(TEXT("trigger_events"), EvArr);

        const FVector V = Inst->GetValue().Get<FVector>();
        Result->SetArrayField(TEXT("value"), mcp_common::VectorToJsonArray(V));

        Result->SetNumberField(TEXT("elapsed_seconds"), Inst->GetElapsedTime());
        Result->SetNumberField(TEXT("triggered_seconds"), Inst->GetTriggeredTime());
        Result->SetNumberField(TEXT("last_triggered_world_time"), Inst->GetLastTriggeredWorldTime());
    }

    Result->SetStringField(TEXT("world"), WorldSelector);
    mcp_common::AddWorldEvidence(Result, PC->GetWorld() ? PC->GetWorld() : ResolvedWorld);
    Result->SetStringField(TEXT("note"),
        TEXT("Snapshot of ONE tick. To prove an input fired, read last_triggered_world_time (or triggered_seconds) BEFORE the injection and again AFTER: an advance is the proof, and no advance with is_mapped_in_applied_context:true means the key reached Enhanced Input but no trigger fired. is_tracked:false plus is_mapped_in_applied_context:false means the action is not reachable at all right now -- the usual cause is that the pawn carrying the handler is not possessed."));
    return mcp_common::MakeOkReply(Result);
}

// create_force_feedback_effect
// ---------------------------------------------------------------------------- //
// MCP:create_force_feedback_effect - Create a UForceFeedbackEffect data asset (NewObject + save).  Seeds the effect with one default FForceFeedbackChannelDetails so it is a meaningful, non-empty asset. Idempotent: if the asset already exists it is loaded and returned.
//   @param name - Asset name (e.g. FFE_Rumble)
//   @param package_path - Target /Game package path (optional, default "/Game/Gameplay")
//   @returns { name: string, asset_path: string, asset_class: string, channel_detail_count: number, is_force_feedback_effect: bool, already_existed: bool, created: bool }

TSharedPtr<FJsonObject> HandleCreateForceFeedbackEffect(const TSharedPtr<FJsonObject>& Params)
{
    FString Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString RequestedFolder;
    Params->TryGetStringField(TEXT("package_path"), RequestedFolder);
    const FString PackagePath = mcp_common::NormalizeFolder(RequestedFolder, TEXT("/Game/Gameplay"));
    const FString FullPath = FString::Printf(TEXT("%s/%s"), *PackagePath, *Name);
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(FullPath)) { return Invalid; }

    bool bAlreadyExisted = false;
    UForceFeedbackEffect* Effect = nullptr;

    if (UEditorAssetLibrary::DoesAssetExist(FullPath))
    {
        Effect = LoadObject<UForceFeedbackEffect>(nullptr, *FullPath);
        if (!Effect)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Asset exists but failed to load as ForceFeedbackEffect: %s"), *FullPath));
        }
        bAlreadyExisted = true;
    }
    else
    {
        FString PackageError;
        UPackage* Package = mcp_common::CreateAssetPackage(*FullPath, *Name, UForceFeedbackEffect::StaticClass(), PackageError);
        if (!Package)
        {
            return mcp_common::MakeErrorReply(PackageError);
        }

        // NewObject via StaticClass() (UForceFeedbackEffect is MinimalAPI; only the
        // generated StaticClass() is exported — that is all we need).
        Effect = NewObject<UForceFeedbackEffect>(Package, UForceFeedbackEffect::StaticClass(), *Name,
                                                 RF_Public | RF_Standalone | RF_Transactional);
        if (!Effect)
        {
            return mcp_common::MakeErrorReply(TEXT("NewObject<UForceFeedbackEffect> returned null"));
        }

        // Seed a single default channel detail so the effect is non-empty/meaningful.
        // FForceFeedbackChannelDetails default ctor enables all four affects flags.
        Effect->ChannelDetails.Add(FForceFeedbackChannelDetails());

        FAssetRegistryModule::AssetCreated(Effect);
        Effect->MarkPackageDirty();

        FString SaveError;
        if (!mcp_common::SaveAssetPackage(Package, Effect, SaveError))
        {
            return mcp_common::MakeErrorReply(SaveError);
        }
    }

    const bool bIsForceFeedbackEffect = Effect->IsA(UForceFeedbackEffect::StaticClass());

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("name"), Name);
    Result->SetStringField(TEXT("asset_path"), FullPath);
    Result->SetStringField(TEXT("asset_class"), Effect->GetClass()->GetName());
    Result->SetNumberField(TEXT("channel_detail_count"), Effect->ChannelDetails.Num());
    Result->SetBoolField(TEXT("is_force_feedback_effect"), bIsForceFeedbackEffect);
    Result->SetBoolField(TEXT("already_existed"), bAlreadyExisted);
    Result->SetBoolField(TEXT("created"), true);
    return mcp_common::MakeOkReply(Result);
}

// ============================================================================
// Input binding audit — list_all_input_mappings / find_free_input_key
// ============================================================================
namespace
{
    // One resolved key binding: which action/axis the key drives and where it
    // comes from (an Enhanced Input context asset, or a legacy UInputSettings
    // action/axis mapping).
    struct FKeyBindingRec_InputCmds
    {
        FString Name;         // action name / axis name
        FString Source;       // "enhanced_input_context" | "legacy_action" | "legacy_axis"
        FString ContextPath;  // IMC asset path (enhanced only), else empty
    };

    // What BuildInputBindingMap_InputCmds gathers.
    struct FInputBindingCensus_InputCmds
    {
        TMap<FKey, TArray<FKeyBindingRec_InputCmds>> KeyToBindings;
        TArray<TSharedPtr<FJsonValue>> Contexts;
        int32 ContextCount = 0;
        int32 LegacyActionCount = 0;
        int32 LegacyAxisCount = 0;
    };

    // Aggregate every key->binding across ALL UInputMappingContext assets (via the
    // AssetRegistry, iterating ->GetMappings() — the reliable C++ path used by
    // HandleGetInputMappingContext) plus the legacy UInputSettings action/axis
    // mappings. Fills the per-context summary and the legacy counts. Keyed by FKey
    // (GetTypeHash(FKey) is defined in InputCoreTypes).
    FInputBindingCensus_InputCmds BuildInputBindingMap_InputCmds()
    {
        FInputBindingCensus_InputCmds Census;
        // Every registered mapping-context asset, loaded one by one.
        for (const FAssetData& AssetData : UEAgentInputCommandsImpl::ListRegisteredAssets(UInputMappingContext::StaticClass()))
        {
            UInputMappingContext* IMC = Cast<UInputMappingContext>(AssetData.GetAsset());
            if (!IMC)
            {
                continue;
            }
            const TArray<FEnhancedActionKeyMapping>& Mappings = IMC->GetMappings();

            TSharedPtr<FJsonObject> CtxObj = MakeShared<FJsonObject>();
            CtxObj->SetStringField(TEXT("context_path"), IMC->GetPathName());
            CtxObj->SetStringField(TEXT("context_name"), IMC->GetName());
            CtxObj->SetNumberField(TEXT("mapping_count"), Mappings.Num());
            Census.Contexts.Add(MakeShared<FJsonValueObject>(CtxObj));
            ++Census.ContextCount;

            for (const FEnhancedActionKeyMapping& M : Mappings)
            {
                if (!M.Key.IsValid())
                {
                    continue;
                }
                FKeyBindingRec_InputCmds Rec;
                Rec.Name = M.Action ? M.Action->GetName() : TEXT("<null action>");
                Rec.Source = TEXT("enhanced_input_context");
                Rec.ContextPath = IMC->GetPathName();
                Census.KeyToBindings.FindOrAdd(M.Key).Add(Rec);
            }
        }

        // Legacy UInputSettings mappings (always available, no Enhanced Input needed).
        if (UInputSettings* InputSettings = UInputSettings::GetInputSettings())
        {
            for (const FInputActionKeyMapping& M : InputSettings->GetActionMappings())
            {
                ++Census.LegacyActionCount;
                if (!M.Key.IsValid())
                {
                    continue;
                }
                FKeyBindingRec_InputCmds Rec;
                Rec.Name = M.ActionName.ToString();
                Rec.Source = TEXT("legacy_action");
                Census.KeyToBindings.FindOrAdd(M.Key).Add(Rec);
            }
            for (const FInputAxisKeyMapping& M : InputSettings->GetAxisMappings())
            {
                ++Census.LegacyAxisCount;
                if (!M.Key.IsValid())
                {
                    continue;
                }
                FKeyBindingRec_InputCmds Rec;
                Rec.Name = FString::Printf(TEXT("%s (scale %.2f)"), *M.AxisName.ToString(), M.Scale);
                Rec.Source = TEXT("legacy_axis");
                Census.KeyToBindings.FindOrAdd(M.Key).Add(Rec);
            }
        }
        return Census;
    }
}

// MCP:list_all_input_mappings - Which keys are already TAKEN, and which ones CONFLICT: aggregates every key-to-action binding across ALL Enhanced Input mapping context assets plus the legacy InputSettings action/axis mappings, and flags any key bound to more than one distinct action. Run it BEFORE claiming a new key -- a keybinding collision does not fail to compile, it surfaces weeks later inside another system. This is the aggregate you can trust: the Python route (InputMappingContext.mappings) is deprecated and returns an EMPTY list, which reads as "every key is free" even when every letter is taken. Use find_free_input_key for the complement (what is still unbound), get_input_mapping_context for one context.
//   @param filter - Optional case-insensitive substring on the key name to narrow the returned key_bindings.
//   @param conflicts_only - When true, return only keys flagged as conflicts (more than one distinct action on the same key). Default false.
//   @returns { contexts:[{context_path,context_name,mapping_count}], context_count, key_bindings:[{key,display_name,bindings:[{action,source,context_path}],binding_count,distinct_action_count,conflict}], bound_key_count, conflicts:[...], conflict_count, legacy_action_mapping_count, legacy_axis_mapping_count }
TSharedPtr<FJsonObject> HandleListAllInputMappings(const TSharedPtr<FJsonObject>& Params)
{
    FInputBindingCensus_InputCmds Census = BuildInputBindingMap_InputCmds();

    FString Filter;
    Params->TryGetStringField(TEXT("filter"), Filter);
    bool bConflictsOnly = false;
    Params->TryGetBoolField(TEXT("conflicts_only"), bConflictsOnly);

    // Deterministic output order: sort keys lexically by their FName.
    Census.KeyToBindings.KeySort([](const FKey& A, const FKey& B)
    {
        return A.GetFName().Compare(B.GetFName()) < 0;
    });

    TArray<TSharedPtr<FJsonValue>> KeyBindingsJson;
    TArray<TSharedPtr<FJsonValue>> ConflictsJson;
    int32 ConflictCount = 0;

    for (const TPair<FKey, TArray<FKeyBindingRec_InputCmds>>& Pair : Census.KeyToBindings)
    {
        const FString KeyName = Pair.Key.GetFName().ToString();
        if (!Filter.IsEmpty() && !KeyName.Contains(Filter, ESearchCase::IgnoreCase))
        {
            continue;
        }

        TSet<FString> DistinctActions;
        TArray<TSharedPtr<FJsonValue>> BindsJson;
        for (const FKeyBindingRec_InputCmds& Rec : Pair.Value)
        {
            DistinctActions.Add(Rec.Name);
            TSharedPtr<FJsonObject> B = MakeShared<FJsonObject>();
            B->SetStringField(TEXT("action"), Rec.Name);
            B->SetStringField(TEXT("source"), Rec.Source);
            if (!Rec.ContextPath.IsEmpty())
            {
                B->SetStringField(TEXT("context_path"), Rec.ContextPath);
            }
            BindsJson.Add(MakeShared<FJsonValueObject>(B));
        }

        const bool bConflict = DistinctActions.Num() > 1;
        if (bConflict)
        {
            ++ConflictCount;
        }
        if (bConflictsOnly && !bConflict)
        {
            continue;
        }

        TSharedPtr<FJsonObject> KObj = MakeShared<FJsonObject>();
        KObj->SetStringField(TEXT("key"), KeyName);
        KObj->SetStringField(TEXT("display_name"), Pair.Key.GetDisplayName().ToString());
        KObj->SetArrayField(TEXT("bindings"), BindsJson);
        KObj->SetNumberField(TEXT("binding_count"), BindsJson.Num());
        KObj->SetNumberField(TEXT("distinct_action_count"), DistinctActions.Num());
        KObj->SetBoolField(TEXT("conflict"), bConflict);
        KeyBindingsJson.Add(MakeShared<FJsonValueObject>(KObj));
        if (bConflict)
        {
            ConflictsJson.Add(MakeShared<FJsonValueObject>(KObj));
        }
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("contexts"), Census.Contexts);
    Data->SetNumberField(TEXT("context_count"), Census.ContextCount);
    Data->SetArrayField(TEXT("key_bindings"), KeyBindingsJson);
    Data->SetNumberField(TEXT("bound_key_count"), Census.KeyToBindings.Num());
    Data->SetArrayField(TEXT("conflicts"), ConflictsJson);
    Data->SetNumberField(TEXT("conflict_count"), ConflictCount);
    Data->SetNumberField(TEXT("legacy_action_mapping_count"), Census.LegacyActionCount);
    Data->SetNumberField(TEXT("legacy_axis_mapping_count"), Census.LegacyAxisCount);
    return mcp_common::MakeOkReply(Data);
}

// MCP:find_free_input_key - Which keys are still FREE: lists every input key not bound by any Enhanced Input mapping context or legacy mapping, filterable by category (keyboard / gamepad / mouse / touch) and substring. The complement of list_all_input_mappings and the tool to run before binding a new keyboard shortcut, since a collision compiles silently and only shows up in play. Trustworthy where the Python route is not: InputMappingContext.mappings is deprecated and returns empty, which makes every taken key look available.
//   @param filter - Optional case-insensitive substring matched against the key name or display name.
//   @param category - Optional key category filter: one of 'keyboard', 'gamepad', 'mouse', 'touch'. Absent = all categories.
//   @param bindable_only - When true (default), only keys where FKey::IsBindableInBlueprints() is true are considered (skips AnyKey/gestures/etc.).
//   @returns { free_keys:[{key,display_name,is_gamepad,is_mouse,is_analog}], free_key_count, bound_key_count, bound_keys:[...] }
TSharedPtr<FJsonObject> HandleFindFreeInputKey(const TSharedPtr<FJsonObject>& Params)
{
    const FInputBindingCensus_InputCmds Census = BuildInputBindingMap_InputCmds();

    FString Filter;
    Params->TryGetStringField(TEXT("filter"), Filter);
    FString Category;
    Params->TryGetStringField(TEXT("category"), Category);
    bool bBindableOnly = true;
    Params->TryGetBoolField(TEXT("bindable_only"), bBindableOnly);

    TArray<FKey> AllKeys;
    EKeys::GetAllKeys(AllKeys);

    TArray<TSharedPtr<FJsonValue>> FreeKeys;
    TArray<TSharedPtr<FJsonValue>> BoundKeysJson;
    int32 FreeCount = 0;

    for (const FKey& Key : AllKeys)
    {
        if (!Key.IsValid())
        {
            continue;
        }
        if (bBindableOnly && !Key.IsBindableInBlueprints())
        {
            continue;
        }

        const FString Name = Key.GetFName().ToString();
        const FString Display = Key.GetDisplayName().ToString();
        if (!Filter.IsEmpty()
            && !Name.Contains(Filter, ESearchCase::IgnoreCase)
            && !Display.Contains(Filter, ESearchCase::IgnoreCase))
        {
            continue;
        }

        if (!Category.IsEmpty())
        {
            const bool bGamepad = Key.IsGamepadKey();
            const bool bMouse = Key.IsMouseButton();
            const bool bTouch = Key.IsTouch();
            const bool bKeyboard = !bGamepad && !bMouse && !bTouch;
            if (Category.Equals(TEXT("gamepad"), ESearchCase::IgnoreCase) && !bGamepad) { continue; }
            if (Category.Equals(TEXT("mouse"), ESearchCase::IgnoreCase) && !bMouse) { continue; }
            if (Category.Equals(TEXT("touch"), ESearchCase::IgnoreCase) && !bTouch) { continue; }
            if (Category.Equals(TEXT("keyboard"), ESearchCase::IgnoreCase) && !bKeyboard) { continue; }
        }

        TSharedPtr<FJsonObject> KObj = MakeShared<FJsonObject>();
        KObj->SetStringField(TEXT("key"), Name);
        KObj->SetStringField(TEXT("display_name"), Display);
        KObj->SetBoolField(TEXT("is_gamepad"), Key.IsGamepadKey());
        KObj->SetBoolField(TEXT("is_mouse"), Key.IsMouseButton());
        KObj->SetBoolField(TEXT("is_analog"), Key.IsAnalog());

        if (Census.KeyToBindings.Contains(Key))
        {
            BoundKeysJson.Add(MakeShared<FJsonValueObject>(KObj));
        }
        else
        {
            FreeKeys.Add(MakeShared<FJsonValueObject>(KObj));
            ++FreeCount;
        }
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("free_keys"), FreeKeys);
    Data->SetNumberField(TEXT("free_key_count"), FreeCount);
    Data->SetNumberField(TEXT("bound_key_count"), Census.KeyToBindings.Num());
    Data->SetArrayField(TEXT("bound_keys"), BoundKeysJson);
    return mcp_common::MakeOkReply(Data);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_input_modifier"), &HandleAddInputModifier },
    { TEXT("add_input_trigger"), &HandleAddInputTrigger },
    { TEXT("add_mapping_to_context"), &HandleAddMappingToContext },
    { TEXT("create_force_feedback_effect"), &HandleCreateForceFeedbackEffect },
    { TEXT("create_input_action"), &HandleCreateInputAction },
    { TEXT("create_input_mapping_context"), &HandleCreateInputMappingContext },
    { TEXT("discover_input_types"), &HandleDiscoverInputTypes },
    { TEXT("find_free_input_key"), &HandleFindFreeInputKey },
    { TEXT("get_action_mappings"), &HandleGetActionMappings },
    { TEXT("get_available_keys"), &HandleGetAvailableKeys },
    { TEXT("get_axis_mappings"), &HandleGetAxisMappings },
    { TEXT("get_input_action"), &HandleGetInputAction },
    { TEXT("get_input_action_state"), &HandleGetInputActionState },
    { TEXT("get_input_mapping_context"), &HandleGetInputMappingContext },
    { TEXT("get_input_settings"), &HandleGetInputSettings },
    { TEXT("get_player_input"), &HandleGetPlayerInput },
    { TEXT("list_all_input_mappings"), &HandleListAllInputMappings },
    { TEXT("list_input_modifier_types"), &HandleListInputModifierTypes },
    { TEXT("list_input_trigger_types"), &HandleListInputTriggerTypes },
    { TEXT("list_input_triggers"), &HandleListInputTriggers },
    { TEXT("map_input_action"), &HandleMapInputAction },
    { TEXT("register_input_key_mapping"), &HandleRegisterInputKeyMapping },
    { TEXT("remove_input_modifier"), &HandleRemoveInputModifier },
    { TEXT("remove_input_trigger"), &HandleRemoveInputTrigger },
    { TEXT("remove_mapping_from_context"), &HandleRemoveMappingFromContext },
    { TEXT("set_input_action_flags"), &HandleSetInputActionFlags },
    { TEXT("set_input_action_trigger_behavior"), &HandleSetInputActionTriggerBehavior },
    { TEXT("set_input_action_value_type"), &HandleSetInputActionValueType },
    { TEXT("trigger_input_action"), &HandleTriggerInputAction },
    { TEXT("unregister_input_key_mapping"), &HandleUnregisterInputKeyMapping },
};
} // namespace UEAgentInputCommands
