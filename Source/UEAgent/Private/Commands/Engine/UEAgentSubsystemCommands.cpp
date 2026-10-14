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

UWorld* GetWorld(const TSharedPtr<FJsonObject>& Params)
{
    // The editor's world when there is one; otherwise (no editor, or no editor world yet)
    // the engine's first world context.
    if (UWorld* EditorWorld = mcp_common::GetEditorWorld())
    {
        return EditorWorld;
    }
    return mcp_common::GetFirstContextWorld();
}

UGameInstance* GetGameInstance(const TSharedPtr<FJsonObject>& Params)
{
    // The game instance of whichever world GetWorld picked; none without a world.
    const UWorld* HostWorld = GetWorld(Params);
    return HostWorld != nullptr ? HostWorld->GetGameInstance() : nullptr;
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

// MCP:list_local_player_subsystems - Enumerate every live ULocalPlayerSubsystem-derived instance across all local players in the editor world's GameInstance.
TSharedPtr<FJsonObject> HandleListLocalPlayerSubsystems(const TSharedPtr<FJsonObject>& Params)
{
    UGameInstance* GameInstance = GetGameInstance(Params);
    if (!GameInstance)
    {
        return mcp_common::MakeErrorReply(TEXT("GameInstance not available"));
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
    ResultObj->SetNumberField(TEXT("local_player_count"), NumPlayers);
    ResultObj->SetNumberField(TEXT("subsystem_count"), Entries.Num());
    ResultObj->SetArrayField(TEXT("subsystems"), Entries);
    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:inspect_subsystem_instance - Reflection walk of public UPROPERTYs on a live subsystem instance. Resolves the instance via the lifetime hint (engine/gameinstance/world/editor/localplayer) + subsystem class.
//   @param subsystem_class - Class name of the subsystem to inspect (short reflected name or /Script path).
//   @param lifetime - Which host resolves the instance: one of engine | gameinstance | world | editor | localplayer.
//   @param player_index - Local player index, used only when lifetime is "localplayer" (default 0).
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
    USubsystem* Instance = nullptr;
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
        UGameInstance* GameInstance = GetGameInstance(Params);
        if (GameInstance == nullptr)
        {
            HostProblem = TEXT("GameInstance not available");
        }
        else
        {
            Instance = GameInstance->GetSubsystemBase(TSubclassOf<UGameInstanceSubsystem>(WantedClass));
        }
    }
    else if (HostIs(TEXT("world")))
    {
        UWorld* HostWorld = GetWorld(Params);
        if (HostWorld == nullptr)
        {
            HostProblem = TEXT("World not available");
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
        UGameInstance* GameInstance = GetGameInstance(Params);
        ULocalPlayer* Player = (GameInstance != nullptr) ? GameInstance->GetLocalPlayerByIndex(LocalPlayerSlot) : nullptr;
        if (GameInstance == nullptr)
        {
            HostProblem = TEXT("GameInstance not available");
        }
        else if (Player == nullptr)
        {
            HostProblem = FString::Printf(TEXT("No ULocalPlayer at index %d"), LocalPlayerSlot);
        }
        else
        {
            Instance = Player->GetSubsystemBase(TSubclassOf<ULocalPlayerSubsystem>(WantedClass));
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
    return mcp_common::MakeOkReply(Payload);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("inspect_subsystem_instance"), &HandleInspectSubsystemInstance },
    { TEXT("list_local_player_subsystems"), &HandleListLocalPlayerSubsystems },
};
} // namespace UEAgentSubsystemCommands
