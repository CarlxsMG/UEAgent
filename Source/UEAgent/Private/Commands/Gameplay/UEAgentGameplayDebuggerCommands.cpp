// Copyright (c) 2026 CarlxsMG.

#include "Commands/Gameplay/UEAgentGameplayDebuggerCommands.h"
#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
// MCP-DOMAIN: GameplayDebugger

#if WITH_GAMEPLAY_DEBUGGER

#include "GameplayDebugger.h"
#include "GameplayDebuggerCategory.h"
#include "GameFramework/Actor.h"
#include "UObject/UnrealType.h"
#include "UObject/Class.h"
#include "Engine/Engine.h"

namespace
{
    // One row of the data-driven spec the LLM supplies: which property to show, on which actor
    // class (empty = any), with what label/color. Plain struct -- no reflection needed, this never
    // crosses the MCP wire as a UPROPERTY, only as parsed JSON.
    struct FMcpDebugPropertySpec
    {
        FString ActorClass;
        FString PropertyName;
        FString Color = TEXT("white");
        FString Label;
    };

    FString GetPropertyValueAsString(FProperty* Property, void* ValuePtr)
    {
        if (!Property || !ValuePtr) { return TEXT("null"); }
        FString OutputString;
        Property->ExportTextItem_Direct(OutputString, ValuePtr, nullptr, nullptr, PPF_None);
        return OutputString;
    }
}

/**
 * Single generic FGameplayDebuggerCategory backing EVERY category MCP registers. Each instance
 * knows which named category it is via GetCategoryName() (assigned by the manager during
 * registration) and looks up its own spec list from the shared static registry -- this is what
 * makes it "one class, many categories" instead of a bespoke subclass per category.
 */
class FGameplayDebuggerCategory_MCPGeneric : public FGameplayDebuggerCategory
{
public:
    FGameplayDebuggerCategory_MCPGeneric()
    {
        bShowOnlyWithDebugActor = true;
    }

    virtual void CollectData(APlayerController* OwnerPC, AActor* DebugActor) override
    {
        if (!DebugActor) { return; }
        const TArray<FMcpDebugPropertySpec>* Specs = SpecsRegistry.Find(GetCategoryName());
        if (!Specs) { return; }

        for (const FMcpDebugPropertySpec& Spec : *Specs)
        {
            if (!Spec.ActorClass.IsEmpty())
            {
                UClass* FilterClass = FindFirstObject<UClass>(*Spec.ActorClass);
                if (FilterClass && !DebugActor->IsA(FilterClass)) { continue; }
            }

            FProperty* Prop = DebugActor->GetClass()->FindPropertyByName(*Spec.PropertyName);
            const FString ValueStr = Prop
                ? GetPropertyValueAsString(Prop, Prop->ContainerPtrToValuePtr<void>(DebugActor))
                : TEXT("<property not found>");
            const FString Label = Spec.Label.IsEmpty() ? Spec.PropertyName : Spec.Label;
            AddTextLine(FString::Printf(TEXT("{%s}%s: %s"), *Spec.Color, *Label, *ValueStr));
        }
    }

    static TSharedRef<FGameplayDebuggerCategory> MakeInstance()
    {
        return MakeShared<FGameplayDebuggerCategory_MCPGeneric>();
    }

    // Shared across every instance (one per player-controller/replicator) -- game-thread only,
    // same as the rest of the bridge (mutated by HandleRegisterGameplayDebuggerCategory via FTSTicker,
    // read by CollectData which the debugger also ticks on the game thread).
    static TMap<FName, TArray<FMcpDebugPropertySpec>> SpecsRegistry;
    static TSet<FName> RegisteredCategoryNames;
};

TMap<FName, TArray<FMcpDebugPropertySpec>> FGameplayDebuggerCategory_MCPGeneric::SpecsRegistry;
TSet<FName> FGameplayDebuggerCategory_MCPGeneric::RegisteredCategoryNames;

#endif // WITH_GAMEPLAY_DEBUGGER

/**
 * GameplayDebugger commands.
 *
 * FGameplayDebuggerCategory (UE 5.8) has no BlueprintCallable or reflected surface, and the real registration
 * API (IGameplayDebugger::RegisterCategory) is C++-only. Rather than a bespoke subclass per custom category,
 * which would need a plugin rebuild for every new category, every MCP-registered category is backed by the
 * single data-driven FGameplayDebuggerCategory_MCPGeneric above, configured purely by the specs the LLM
 * supplies at runtime.
 *
 * GameplayDebugger is compiled only when WITH_GAMEPLAY_DEBUGGER is 1 (Editor/Development, never
 * Shipping/Test); where it is 0, every command returns a clean error, never a false success. Registration
 * only takes effect visually once a HUD/player controller with an active AGameplayDebuggerCategoryReplicator
 * exists (PIE/game world); registering in a bare editor world still succeeds (the category and its specs are
 * recorded), but there is nothing to draw them yet.
 */
namespace UEAgentGameplayDebuggerCommands
{

void UnregisterAllCategories()
{
#if WITH_GAMEPLAY_DEBUGGER
    if (IGameplayDebugger::IsAvailable())
    {
        IGameplayDebugger& Debugger = IGameplayDebugger::Get();
        for (const FName& Name : FGameplayDebuggerCategory_MCPGeneric::RegisteredCategoryNames)
        {
            Debugger.UnregisterCategory(Name);
        }
    }
    FGameplayDebuggerCategory_MCPGeneric::RegisteredCategoryNames.Empty();
    FGameplayDebuggerCategory_MCPGeneric::SpecsRegistry.Empty();
#endif
}

// MCP:register_gameplay_debugger_category - Registers (or reconfigures, idempotently) a named GameplayDebugger category backed by a single generic data-driven FGameplayDebuggerCategory (GameplayDebugger.h IGameplayDebugger::RegisterCategory). FGameplayDebuggerCategory has 0 BlueprintCallable surface, so this is the only path to author a custom debugger category from MCP. Toggle visibility in-game with the console command "showdebug <category_name>".
//   @param category_name - Unique name for this debugger category (required)
//   @param specs [array] - [{actor_class?: string, property_name: string, color?: string ("white"|"red"|"green"|"yellow"|"cyan"...), label?: string}] -- which properties to print for the current debug-actor, and how
//   @returns { category_name: string, spec_count: int, registered: bool, already_existed: bool }
TSharedPtr<FJsonObject> HandleRegisterGameplayDebuggerCategory(const TSharedPtr<FJsonObject>& Params)
{
#if !WITH_GAMEPLAY_DEBUGGER
    return mcp_common::MakeErrorReply(
        TEXT("GameplayDebugger is not compiled into this build (WITH_GAMEPLAY_DEBUGGER=0; only available in Editor/Development configs)"));
#else
    FString CategoryName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("category_name"), CategoryName)) { return Missing; }

    const TArray<TSharedPtr<FJsonValue>>* SpecsArray = nullptr;
    if (!Params->TryGetArrayField(TEXT("specs"), SpecsArray) || !SpecsArray || SpecsArray->Num() == 0)
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: specs (non-empty array)"));

    TArray<FMcpDebugPropertySpec> ParsedSpecs;
    ParsedSpecs.Reserve(SpecsArray->Num());
    for (const TSharedPtr<FJsonValue>& Entry : *SpecsArray)
    {
        const TSharedPtr<FJsonObject>* EntryObj = nullptr;
        if (!Entry->TryGetObject(EntryObj) || !EntryObj || !(*EntryObj))
            return mcp_common::MakeErrorReply(TEXT("Each entry in 'specs' must be an object"));

        FMcpDebugPropertySpec Spec;
        if (!(*EntryObj)->TryGetStringField(TEXT("property_name"), Spec.PropertyName) || Spec.PropertyName.IsEmpty())
            return mcp_common::MakeErrorReply(TEXT("Each spec entry requires a non-empty 'property_name'"));
        (*EntryObj)->TryGetStringField(TEXT("actor_class"), Spec.ActorClass);
        (*EntryObj)->TryGetStringField(TEXT("color"), Spec.Color);
        (*EntryObj)->TryGetStringField(TEXT("label"), Spec.Label);
        ParsedSpecs.Add(MoveTemp(Spec));
    }

    const FName CategoryFName(*CategoryName);
    FGameplayDebuggerCategory_MCPGeneric::SpecsRegistry.Add(CategoryFName, ParsedSpecs);

    const bool bAlreadyExisted = FGameplayDebuggerCategory_MCPGeneric::RegisteredCategoryNames.Contains(CategoryFName);
    if (!bAlreadyExisted)
    {
        IGameplayDebugger::Get().RegisterCategory(
            CategoryFName,
            IGameplayDebugger::FOnGetCategory::CreateStatic(&FGameplayDebuggerCategory_MCPGeneric::MakeInstance),
            EGameplayDebuggerCategoryState::EnabledInGameAndSimulate);
        IGameplayDebugger::Get().NotifyCategoriesChanged();
        FGameplayDebuggerCategory_MCPGeneric::RegisteredCategoryNames.Add(CategoryFName);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("category_name"), CategoryName);
    Result->SetNumberField(TEXT("spec_count"), ParsedSpecs.Num());
    Result->SetBoolField(TEXT("registered"), true);
    Result->SetBoolField(TEXT("already_existed"), bAlreadyExisted);
    return mcp_common::MakeOkReply(Result);
#endif
}

// MCP:unregister_gameplay_debugger_category - Unregisters a named GameplayDebugger category created by register_gameplay_debugger_category (GameplayDebugger.h IGameplayDebugger::UnregisterCategory, the exact inverse of RegisterCategory). Also drops the category's stored specs. Idempotent: unregistering a category that register_gameplay_debugger_category did not create is an honest no-op reporting already_absent:true.
//   @param category_name - Name of the debugger category to unregister (required)
//   @returns { category_name: string, unregistered: bool, already_absent: bool }
TSharedPtr<FJsonObject> HandleUnregisterGameplayDebuggerCategory(const TSharedPtr<FJsonObject>& Params)
{
#if !WITH_GAMEPLAY_DEBUGGER
    return mcp_common::MakeErrorReply(
        TEXT("GameplayDebugger is not compiled into this build (WITH_GAMEPLAY_DEBUGGER=0; only available in Editor/Development configs)"));
#else
    FString CategoryName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("category_name"), CategoryName)) { return Missing; }

    const FName CategoryFName(*CategoryName);
    const bool bWasRegistered = FGameplayDebuggerCategory_MCPGeneric::RegisteredCategoryNames.Contains(CategoryFName);
    if (bWasRegistered)
    {
        IGameplayDebugger::Get().UnregisterCategory(CategoryFName);
        IGameplayDebugger::Get().NotifyCategoriesChanged();
        FGameplayDebuggerCategory_MCPGeneric::RegisteredCategoryNames.Remove(CategoryFName);
        FGameplayDebuggerCategory_MCPGeneric::SpecsRegistry.Remove(CategoryFName);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("category_name"), CategoryName);
    Result->SetBoolField(TEXT("unregistered"), bWasRegistered);
    Result->SetBoolField(TEXT("already_absent"), !bWasRegistered);
    return mcp_common::MakeOkReply(Result);
#endif
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("register_gameplay_debugger_category"), &HandleRegisterGameplayDebuggerCategory },
    { TEXT("unregister_gameplay_debugger_category"), &HandleUnregisterGameplayDebuggerCategory },
};
} // namespace UEAgentGameplayDebuggerCommands
