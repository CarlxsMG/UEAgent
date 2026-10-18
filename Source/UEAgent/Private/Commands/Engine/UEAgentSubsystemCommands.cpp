#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Subsystems/EngineSubsystem.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Subsystems/WorldSubsystem.h"
#include "Subsystems/LocalPlayerSubsystem.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"   // FProperty walk for inspect_subsystem_instance

/**
 * Live subsystem instance commands: list the local-player subsystems, and inspect the public properties of
 * a live engine, editor, game-instance, world or local-player subsystem. Subsystem CLASSES are listed by
 * list_subsystem_classes / get_subsystem_class_info (UEAgentSubsystemAuthoringCommands).
 */
namespace UEAgentSubsystemCommands
{

// MCP-DOMAIN: Subsystem

// The world a world-hosted lifetime reads: the `world` selector as mcp_common::ResolveWorldSelector reads it, the
// editor world when absent. Null, with OutError naming the worlds present, when that world does not exist.
UWorld* GetWorld(const TSharedPtr<FJsonObject>& Params, FString& OutSelector, FString& OutError)
{
    return mcp_common::ResolveWorldSelector(Params, TEXT("editor"), OutSelector, OutError);
}

// The game instance of the world the `world` selector picks, the Play-in-Editor world when absent: only a game or
// PIE world is given one (UWorld::SetGameInstance, from UGameInstance's world setup), never the editor world, so
// without PIE there is none. Null, with OutError saying what is missing, when the world or its game instance is not
// there; OutWorld is the world that was resolved, or null.
UGameInstance* GetGameInstance(const TSharedPtr<FJsonObject>& Params, UWorld*& OutWorld, FString& OutSelector, FString& OutError)
{
    FString WorldError;
    OutWorld = mcp_common::ResolveWorldSelector(Params, TEXT("pie"), OutSelector, WorldError);
    if (OutWorld == nullptr)
    {
        OutError = FString::Printf(
            TEXT("GameInstance not available: a game instance exists only in a Play-in-Editor world, never in the editor world. %s"),
            *WorldError);
        return nullptr;
    }
    UGameInstance* GameInstance = OutWorld->GetGameInstance();
    if (GameInstance == nullptr)
    {
        OutError = FString::Printf(
            TEXT("GameInstance not available: the %s world '%s' (world='%s') has no game instance; only a Play-in-Editor world has one, so start PIE and leave world unset or pass pie."),
            LexToString(OutWorld->WorldType.GetValue()), *OutWorld->GetName(), *OutSelector);
    }
    return GameInstance;
}

// ---------------------------------------------------------------------------
// Local-player coverage + reflection-based inspect.
//
// All handlers are read-only and headless-safe — they don't touch the
// SubsystemCollection mutation API (which is reserved for engine init /
// PIE startup). The reflection walk in inspect_subsystem_instance filters
// to EditAnywhere or Blueprint-visible UPROPERTYs (the standard "exposed to
// gameplay layer" surface); transient fields are skipped so we never leak
// internal engine state.
// ---------------------------------------------------------------------------

// MCP:list_local_player_subsystems - Enumerate every live ULocalPlayerSubsystem-derived instance across all local players of the game instance of a Play-in-Editor world, the running PIE world by default (world picks another; world_package and world_type say which one was read). Local players exist only while PIE runs: without PIE, or for a world with no game instance such as the editor world, it is an error that says so. Read-only.
//   @param world - Optional world selector: absent or pie (the default, ambiguous: PlayWorld, else the first PIE context), game (alias of pie), server (the PIE world with authority), client, pie:N, or editor (which has no game instance, so it fails). An unrecognised value fails naming the worlds that exist
//   @returns { world: string, world_name: string, world_package: string, world_type: string, net_mode: string, pie_instance: int, local_player_count: int, subsystem_count: int, subsystems: [{ player_index, player_name, subsystem_class, subsystem_name, subsystem_path }] }
TSharedPtr<FJsonObject> HandleListLocalPlayerSubsystems(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* HostWorld = nullptr;
    FString WorldSelector;
    FString HostProblem;
    UGameInstance* GameInstance = GetGameInstance(Params, HostWorld, WorldSelector, HostProblem);
    if (!GameInstance)
    {
        return mcp_common::MakeErrorReply(HostProblem);
    }

    TArray<TSharedPtr<FJsonValue>> Entries;
    const int32 NumPlayers = GameInstance->GetNumLocalPlayers();
    for (int32 Idx = 0; Idx < NumPlayers; ++Idx)
    {
        ULocalPlayer* LP = GameInstance->GetLocalPlayerByIndex(Idx);
        if (!LP) { continue; }

// ULocalPlayer's public SubsystemCollection is FObjectSubsystemCollection<ULocalPlayerSubsystem>;
        // pulling every derived instance means walking GetSubsystemArrayCopy<ULocalPlayerSubsystem>().
        // UE 5.8 signature: template <typename TSubsystemClass> TArray<TSubsystemClass*> GetSubsystemArrayCopy() const
        //   — no output parameter; the templated return type IS the list.
        TArray<ULocalPlayerSubsystem*> Instances = LP->GetSubsystemArrayCopy<ULocalPlayerSubsystem>();

        for (ULocalPlayerSubsystem* Sub : Instances)
        {
            if (!Sub) { continue; }
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetNumberField(TEXT("player_index"), Idx);
            Entry->SetStringField(TEXT("player_name"), LP->GetName());
            Entry->SetStringField(TEXT("subsystem_class"), Sub->GetClass() ? Sub->GetClass()->GetPathName() : TEXT(""));
            Entry->SetStringField(TEXT("subsystem_name"), Sub->GetName());
            Entry->SetStringField(TEXT("subsystem_path"), Sub->GetPathName());
            Entries.Add(MakeShared<FJsonValueObject>(Entry));
        }
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("world"), WorldSelector);
    mcp_common::AddWorldEvidence(ResultObj, HostWorld);
    ResultObj->SetNumberField(TEXT("local_player_count"), NumPlayers);
    ResultObj->SetNumberField(TEXT("subsystem_count"), Entries.Num());
    ResultObj->SetArrayField(TEXT("subsystems"), Entries);
    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:inspect_subsystem_instance - Reads the exposed properties (EditAnywhere or Blueprint-visible, inherited included, Transient skipped) of a live subsystem instance, each with name, C++ type, property class, edit and blueprint_visible flags, and its value exported as text (empty when it cannot be exported). lifetime picks the host: world reads the world the world selector picks, the editor world by default; gameinstance and localplayer read the game instance of that world, the running Play-in-Editor world by default, since the editor world never has one (without PIE they fail saying so). The reply names the world it read (world_package, world_type). A missing host or instance is an error. Read-only. For a quick existence check use get_subsystem_instance; to enumerate use list_subsystem_instances or list_local_player_subsystems.
//   @param subsystem_class - Class name of the subsystem to inspect (short reflected name or /Script path).
//   @param lifetime - Which host resolves the instance: one of engine | gameinstance | world | editor | localplayer.
//   @param player_index - Local player index, used only when lifetime is "localplayer" (default 0).
//   @param world - Optional world selector for the world, gameinstance and localplayer lifetimes, ignored by the others: editor, pie (ambiguous: PlayWorld, else the first PIE context), game (alias of pie), server (the PIE world with authority), client or pie:N. Absent means editor for world and pie for gameinstance and localplayer. An unrecognised value fails naming the worlds that exist
//   @returns { subsystem_class: string, lifetime: string, subsystem_name: string, subsystem_path: string, property_count: int, properties: [{ name, cpp_type, class, edit, blueprint_visible, value }], world?: string, world_name?: string, world_package?: string, world_type?: string, net_mode?: string, pie_instance?: int } -- the world fields only for the world, gameinstance and localplayer lifetimes
TSharedPtr<FJsonObject> HandleInspectSubsystemInstance(const TSharedPtr<FJsonObject>& Params)
{
    FString ClassName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("subsystem_class"), ClassName)) { return Missing; }
    FString Host;
    if (!Params->TryGetStringField(TEXT("lifetime"), Host) || Host.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: lifetime (engine|gameinstance|world|editor|localplayer)"));
    }
    // Only consulted for the "localplayer" host.
    int32 LocalPlayerSlot = 0;
    Params->TryGetNumberField(TEXT("player_index"), LocalPlayerSlot);

    UClass* const WantedClass = FindFirstObject<UClass>(*ClassName);
    if (WantedClass == nullptr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Subsystem class not found: %s"), *ClassName));
    }

    // The live instance comes from the host the caller named. A host that does not exist
    // right now (no engine, no game instance, no such local player...) is its own error.
    // HostWorld stays null for the engine and editor hosts, which live in no world.
    USubsystem* Instance = nullptr;
    UWorld* HostWorld = nullptr;
    FString WorldSelector;
    FString HostProblem;
    const auto HostIs = [&Host](const TCHAR* Candidate)
    {
        return Host.Equals(Candidate, ESearchCase::IgnoreCase);
    };
    if (HostIs(TEXT("engine")))
    {
        if (GEngine == nullptr)
        {
            HostProblem = TEXT("GEngine unavailable");
        }
        else
        {
            Instance = GEngine->GetEngineSubsystemBase(WantedClass);
        }
    }
    else if (HostIs(TEXT("gameinstance")))
    {
        if (UGameInstance* GameInstance = GetGameInstance(Params, HostWorld, WorldSelector, HostProblem))
        {
            Instance = GameInstance->GetSubsystemBase(TSubclassOf<UGameInstanceSubsystem>(WantedClass));
        }
    }
    else if (HostIs(TEXT("world")))
    {
        FString WorldError;
        HostWorld = GetWorld(Params, WorldSelector, WorldError);
        if (HostWorld == nullptr)
        {
            HostProblem = FString::Printf(TEXT("World not available: %s"), *WorldError);
        }
        else
        {
            Instance = HostWorld->GetSubsystemBase(TSubclassOf<UWorldSubsystem>(WantedClass));
        }
    }
    else if (HostIs(TEXT("editor")))
    {
        if (GEditor == nullptr)
        {
            HostProblem = TEXT("GEditor unavailable");
        }
        else
        {
            Instance = GEditor->GetEditorSubsystemBase(WantedClass);
        }
    }
    else if (HostIs(TEXT("localplayer")))
    {
        if (UGameInstance* GameInstance = GetGameInstance(Params, HostWorld, WorldSelector, HostProblem))
        {
            if (ULocalPlayer* Player = GameInstance->GetLocalPlayerByIndex(LocalPlayerSlot))
            {
                Instance = Player->GetSubsystemBase(TSubclassOf<ULocalPlayerSubsystem>(WantedClass));
            }
            else
            {
                HostProblem = FString::Printf(TEXT("No ULocalPlayer at index %d: the game instance of world '%s' has %d local player(s)"),
                    LocalPlayerSlot, *HostWorld->GetName(), GameInstance->GetNumLocalPlayers());
            }
        }
    }
    else
    {
        HostProblem = FString::Printf(TEXT("Unknown lifetime '%s' (engine|gameinstance|world|editor|localplayer)"), *Host);
    }

    if (!HostProblem.IsEmpty())
    {
        return mcp_common::MakeErrorReply(HostProblem);
    }
    if (Instance == nullptr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Subsystem '%s' (%s) not found / not yet initialized"), *ClassName, *Host));
    }

    // The public surface only: EditAnywhere or BlueprintVisible, and never Transient.
    TArray<TSharedPtr<FJsonValue>> Exposed;
    for (TFieldIterator<FProperty> It(Instance->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
    {
        const FProperty* Prop = *It;
        const bool bPublic = Prop != nullptr
            && !Prop->HasAnyPropertyFlags(CPF_Transient)
            && Prop->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible);
        if (!bPublic)
        {
            continue;
        }

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Prop->GetName());
        Entry->SetStringField(TEXT("cpp_type"), Prop->GetCPPType());
        Entry->SetStringField(TEXT("class"), Prop->GetClass()->GetName());
        Entry->SetBoolField(TEXT("edit"), Prop->HasAnyPropertyFlags(CPF_Edit));
        Entry->SetBoolField(TEXT("blueprint_visible"), Prop->HasAnyPropertyFlags(CPF_BlueprintVisible));

        // Best effort, through the property's own text export: empty when the value cannot
        // be stringified (e.g. a raw FObjectPtr without an import-text path).
        FString AsText;
        Prop->ExportTextItem_Direct(AsText, Prop->ContainerPtrToValuePtr<void>(Instance), nullptr, nullptr, PPF_None);
        Entry->SetStringField(TEXT("value"), AsText);
        Exposed.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("subsystem_class"), ClassName);
    Payload->SetStringField(TEXT("lifetime"), Host);
    Payload->SetStringField(TEXT("subsystem_name"), Instance->GetName());
    Payload->SetStringField(TEXT("subsystem_path"), Instance->GetPathName());
    Payload->SetNumberField(TEXT("property_count"), Exposed.Num());
    Payload->SetArrayField(TEXT("properties"), Exposed);
    if (HostWorld != nullptr)
    {
        Payload->SetStringField(TEXT("world"), WorldSelector);
        mcp_common::AddWorldEvidence(Payload, HostWorld);
    }
    return mcp_common::MakeOkReply(Payload);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("inspect_subsystem_instance"), &HandleInspectSubsystemInstance },
    { TEXT("list_local_player_subsystems"), &HandleListLocalPlayerSubsystems },
};
} // namespace UEAgentSubsystemCommands
