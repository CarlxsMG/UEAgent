// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Misc/EngineVersionComparison.h" // UE_VERSION_OLDER_THAN: the UE 5.7 paths
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Kismet/GameplayStatics.h"

// Gameplay Ability System (GameplayAbilities plugin, Runtime). The ability classes sit in
// the plugin's Abilities/ include folder, the component and effect types at its root.
#include "AbilitySystemComponent.h"
#if UE_VERSION_OLDER_THAN(5, 8, 0)
#include "AbilitySystemBlueprintLibrary.h" // SendGameplayEventToActor -- 5.8's AbilitySystemComponent.h includes it, 5.7's does not
#endif
#include "AbilitySystemInterface.h"
#include "Abilities/GameplayAbility.h"
#include "Abilities/Tasks/AbilityTask.h"
#include "GameplayTasksComponent.h"
#include "GameplayEffect.h"
#include "AttributeSet.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/SavePackage.h"
#include "Engine/Blueprint.h"                // UBlueprint::GeneratedClass (blueprint-authored GE resolution)
#include "Engine/DataTable.h"                // UDataTable (FAttributeMetaData DataTable)
#include "UObject/StructOnScope.h"           // FStructOnScope for FAttributeMetaData row memory
#include "UObject/Package.h"                 // CreatePackage
#include "UObject/UObjectGlobals.h"          // StaticLoadObject / LoadClass
#include "EditorAssetLibrary.h"              // UEditorAssetLibrary::DoesAssetExist / CreateAsset
#include "DataTableEditorUtils.h"            // FDataTableEditorUtils broadcast pre/post change
#include "JsonObjectConverter.h"             // FJsonObjectConverter::JsonValueToUProperty / UStructToJsonObject
#include "Misc/Paths.h"                      // FPaths::GetPath / GetBaseFilename for data table path split

/**
 * MCP commands for the Gameplay Ability System.
 *
 * Covers live ability system components on editor-world actors (abilities, applied effects, attribute values,
 * ability tasks, gameplay events and tag listeners) plus a little asset authoring: GameplayEffect assets and
 * the attribute metadata DataTable of an AttributeSet class.
 */
namespace UEAgentGameplayAbilitiesCommands
{

// MCP-DOMAIN: GameplayAbilities

namespace
{
    // register/poll/unregister_gameplay_tag_event registry — same shape as
    // UEAgentRuntimeEventsCommands' subscribe_to_delegate/poll_delegate_events/
    // unsubscribe_delegate (mutex-guarded TMap<int64, Subscription> + a content-keyed
    // map for idempotent re-register + a capped FIFO ring buffer of fires per subscription,
    // drained via a non-destructive cursor read).

    struct FGameplayTagEventFire
    {
        int64 SequenceId = 0;
        double Timestamp = 0.0;
        int32 NewCount = 0;
    };

    struct FGameplayTagEventSubscription
    {
        FString ActorName;
        TWeakObjectPtr<UAbilitySystemComponent> ASC;
        FGameplayTag Tag;
        EGameplayTagEventType::Type EventType = EGameplayTagEventType::NewOrRemoved;
        FDelegateHandle Handle;
        TArray<FGameplayTagEventFire> Events;
        int64 LastEventSequenceId = 0;
    };

    FCriticalSection GTagEventMutex;
    TMap<int64, FGameplayTagEventSubscription> GTagEventSubscriptions;
    TMap<FString, int64> GTagEventKeyToId;   // "ActorName::Tag::EventType" -> subscription_id
    int64 GNextTagEventSubscriptionId = 0;
    constexpr int32 MaxGameplayTagEventsPerSubscription = 512;

    FString MakeGameplayTagEventKey(const FString& ActorName, const FGameplayTag& Tag, EGameplayTagEventType::Type EventType)
    {
        return FString::Printf(TEXT("%s::%s::%d"), *ActorName, *Tag.ToString(), static_cast<int32>(EventType));
    }

    const TCHAR* GameplayTagEventTypeToString(EGameplayTagEventType::Type EventType)
    {
        return EventType == EGameplayTagEventType::AnyCountChange ? TEXT("AnyCountChange") : TEXT("NewOrRemoved");
    }
}

// ---------------------------------------------------------------------------
// Shared lookups for the actor-scoped ability commands.
//
// Every command below addresses an actor of the editor world by its object name and
// works on the ability system that actor exposes. The pieces they share (finding the
// actor, finding its component, finding an ability spec / active effect / attribute
// by name, and the stock replies) live here.
// ---------------------------------------------------------------------------
namespace UEAgentGameplayAbilitiesImpl
{

    // An actor that implements IAbilitySystemInterface decides on its own which component
    // it exposes (its answer is final, even a null one). Any other actor is searched for a
    // UAbilitySystemComponent among its components.
    UAbilitySystemComponent* FindAbilitySystem(AActor* Actor)
    {
        if (Actor == nullptr)
        {
            return nullptr;
        }
        if (const IAbilitySystemInterface* Provider = Cast<IAbilitySystemInterface>(Actor))
        {
            return Provider->GetAbilitySystemComponent();
        }
        return Actor->FindComponentByClass<UAbilitySystemComponent>();
    }

    // The actor a request names together with its ability system component. When either
    // one cannot be found, Error carries the reply the handler must return unchanged.
    struct FAbilityOwner
    {
        AActor* Actor = nullptr;
        UAbilitySystemComponent* Asc = nullptr;
        TSharedPtr<FJsonObject> Error;
        // Set by ReadTagEventTarget: "object_name" or "label", the way the actor was found.
        const TCHAR* ResolvedBy = TEXT("object_name");
    };

    FAbilityOwner ResolveAbilityOwner(const FString& ActorName)
    {
        FAbilityOwner Owner;
        Owner.Actor = mcp_common::FindActorByObjectName(mcp_common::GetEditorWorld(), ActorName);
        if (Owner.Actor == nullptr)
        {
            Owner.Error = mcp_common::MakeErrorReply(TEXT("Actor not found: ") + ActorName);
            return Owner;
        }

        Owner.Asc = FindAbilitySystem(Owner.Actor);
        if (Owner.Asc == nullptr)
        {
            Owner.Error = mcp_common::MakeErrorReply(TEXT("Actor does not have an AbilitySystemComponent"));
        }
        return Owner;
    }

    // ResolveAbilityOwner for the actor named by the required actor_name field.
    FAbilityOwner ReadAbilityOwner(const TSharedPtr<FJsonObject>& Params, FString& OutActorName)
    {
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), OutActorName, mcp_common::EAllowEmpty::Yes))
        {
            FAbilityOwner Owner;
            Owner.Error = Missing;
            return Owner;
        }
        return ResolveAbilityOwner(OutActorName);
    }

    // ReadAbilityOwner for a command that also names an ability: actor_name and ability_name are both
    // required, and both are read before the actor is looked up.
    FAbilityOwner ReadAbilityOwnerAndAbility(const TSharedPtr<FJsonObject>& Params, FString& OutActorName, FString& OutAbilityName)
    {
        FAbilityOwner Owner;
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), OutActorName, mcp_common::EAllowEmpty::Yes))
        {
            Owner.Error = Missing;
            return Owner;
        }
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ability_name"), OutAbilityName, mcp_common::EAllowEmpty::Yes))
        {
            Owner.Error = Missing;
            return Owner;
        }
        return ResolveAbilityOwner(OutActorName);
    }

    // The target of a gameplay-tag event command, checked in this order: the required actor_name, that
    // editor-world actor (its object name, else an editor label only one actor has), its ability system, and
    // the required event_tag, which must be registered. Owner.Error carries the reply when a check fails.
    FAbilityOwner ReadTagEventTarget(const TSharedPtr<FJsonObject>& Params, FString& OutActorName, FGameplayTag& OutEventTag)
    {
        FAbilityOwner Owner;
        if (!Params->TryGetStringField(TEXT("actor_name"), OutActorName) || OutActorName.IsEmpty())
        {
            Owner.Error = mcp_common::MakeErrorReply(TEXT("Missing 'actor_name'"));
            return Owner;
        }
        UWorld* const EditorWorld = mcp_common::GetEditorWorld();
        Owner.Actor = mcp_common::FindActorByObjectName(EditorWorld, OutActorName);
        if (!Owner.Actor)
        {
            // A label is taken only when exactly one actor carries it: two actors with that label would make
            // the target a guess.
            int32 LabelMatches = 0;
            Owner.Actor = mcp_common::FindActorByUniqueLabel(EditorWorld, OutActorName, &LabelMatches);
            Owner.ResolvedBy = TEXT("label");
            if (!Owner.Actor)
            {
                Owner.Error = mcp_common::MakeErrorReply(LabelMatches > 1
                    ? FString::Printf(TEXT("Actor label '%s' is shared by %d actors; pass the object name instead"), *OutActorName, LabelMatches)
                    : FString::Printf(TEXT("Actor '%s' not found (neither an object name nor an editor label)"), *OutActorName));
                return Owner;
            }
        }
        Owner.Asc = FindAbilitySystem(Owner.Actor);
        if (!Owner.Asc)
        {
            Owner.Error = mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Actor '%s' has no UAbilitySystemComponent"), *OutActorName));
            return Owner;
        }
        FString TagStr;
        if (!Params->TryGetStringField(TEXT("event_tag"), TagStr) || TagStr.IsEmpty())
        {
            Owner.Error = mcp_common::MakeErrorReply(TEXT("Missing 'event_tag'"));
            return Owner;
        }
        OutEventTag = FGameplayTag::RequestGameplayTag(FName(*TagStr), /*ErrorIfNotFound*/ false);
        if (!OutEventTag.IsValid())
        {
            Owner.Error = mcp_common::MakeErrorReply(FString::Printf(TEXT("'%s' is not a registered GameplayTag"), *TagStr));
        }
        return Owner;
    }

    TSharedPtr<FJsonObject> AbilityNotFound(const FString& AbilityName)
    {
        return mcp_common::MakeErrorReply(TEXT("Ability not found: ") + AbilityName);
    }

    TSharedPtr<FJsonObject> EffectNotFound(const FString& EffectName)
    {
        return mcp_common::MakeErrorReply(TEXT("Effect not found: ") + EffectName);
    }

    // A reply that starts with the actor_name the request named.
    TSharedPtr<FJsonObject> NewActorReply(const FString& ActorName)
    {
        TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
        Reply->SetStringField(TEXT("actor_name"), ActorName);
        return Reply;
    }

    // NewActorReply, then Value under Field: the ability, effect or tag the request named.
    TSharedPtr<FJsonObject> NewActorReply(const FString& ActorName, const TCHAR* Field, const FString& Value)
    {
        TSharedPtr<FJsonObject> Reply = NewActorReply(ActorName);
        Reply->SetStringField(Field, Value);
        return Reply;
    }

    // First activatable spec whose ability object is called AbilityName (case-insensitive).
    // Ability and effect definitions are class default objects ("Default__GA_Jump_C"), a name no
    // caller writes. A name matches the object name, the class name with or without "_C", the
    // class path, or the asset ("/Game/X/GA_Jump" or "/Game/X/GA_Jump.GA_Jump"); case-insensitive.
    bool DefinitionNamed(const UObject* Definition, const FString& Name)
    {
        if (Definition == nullptr || Name.IsEmpty())
        {
            return false;
        }
        const UClass* Class = Definition->GetClass();
        FString ClassName = Class->GetName();
        if (Definition->GetName() == Name || ClassName == Name || Class->GetPathName() == Name)
        {
            return true;
        }
        ClassName.RemoveFromEnd(TEXT("_C"));
        const FString Package = Class->GetOutermost()->GetName();
        return ClassName == Name || Package == Name || (Package + TEXT(".") + ClassName) == Name;
    }

    const FGameplayAbilitySpec* FindSpecNamed(const UAbilitySystemComponent* Asc, const FString& AbilityName)
    {
        return Asc->GetActivatableAbilities().FindByPredicate(
            [&AbilityName](const FGameplayAbilitySpec& Spec)
            {
                return DefinitionNamed(Spec.Ability, AbilityName);
            });
    }

    // Same search, but hands back a COPY of the spec handle. Activating, cancelling or
    // clearing an ability can reshuffle the spec array, so callers that mutate the
    // component must not hold a reference into it.
    TOptional<FGameplayAbilitySpecHandle> FindSpecHandleNamed(const UAbilitySystemComponent* Asc, const FString& AbilityName)
    {
        if (const FGameplayAbilitySpec* Spec = FindSpecNamed(Asc, AbilityName))
        {
            return Spec->Handle;
        }
        return {};
    }

    // ReadAbilityOwnerAndAbility, then the FindSpecHandleNamed copy of that ability's spec handle; when the
    // actor has no such ability, Owner.Error is the AbilityNotFound reply.
    FAbilityOwner ReadAbilitySpecHandle(const TSharedPtr<FJsonObject>& Params, FString& OutActorName, FString& OutAbilityName,
                                        FGameplayAbilitySpecHandle& OutSpecHandle)
    {
        FAbilityOwner Owner = ReadAbilityOwnerAndAbility(Params, OutActorName, OutAbilityName);
        if (Owner.Error.IsValid())
        {
            return Owner;
        }
        const TOptional<FGameplayAbilitySpecHandle> SpecHandle = FindSpecHandleNamed(Owner.Asc, OutAbilityName);
        if (!SpecHandle.IsSet())
        {
            Owner.Error = AbilityNotFound(OutAbilityName);
            return Owner;
        }
        OutSpecHandle = SpecHandle.GetValue();
        return Owner;
    }

    // Calls Visitor for every active effect that still has a definition, in the order the
    // component reports them (an empty query matches every effect). Returning false from
    // the visitor ends the walk.
    void VisitDefinedEffects(
        const UAbilitySystemComponent* Asc,
        TFunctionRef<bool(const FActiveGameplayEffectHandle&, const FActiveGameplayEffect&)> Visitor)
    {
        const FGameplayEffectQuery MatchAll;
        for (const FActiveGameplayEffectHandle& EffectHandle : Asc->GetActiveEffects(MatchAll))
        {
            const FActiveGameplayEffect* Effect = Asc->GetActiveGameplayEffect(EffectHandle);
            if (Effect != nullptr && Effect->Spec.Def != nullptr && !Visitor(EffectHandle, *Effect))
            {
                return;
            }
        }
    }

    // First active effect whose definition is called EffectName (case-insensitive). The
    // effect's handle is written to OutHandle when one is asked for.
    const FActiveGameplayEffect* FindActiveEffectNamed(
        const UAbilitySystemComponent* Asc, const FString& EffectName, FActiveGameplayEffectHandle* OutHandle = nullptr)
    {
        const FActiveGameplayEffect* Match = nullptr;
        VisitDefinedEffects(Asc, [&](const FActiveGameplayEffectHandle& EffectHandle, const FActiveGameplayEffect& Effect)
        {
            if (!DefinitionNamed(Effect.Spec.Def, EffectName))
            {
                return true;
            }
            Match = &Effect;
            if (OutHandle != nullptr)
            {
                *OutHandle = EffectHandle;
            }
            return false;
        });
        return Match;
    }

    // Resolves an attribute registered on the component either by its bare name ("Health")
    // or by "SetClass.Attribute" ("MyAttributeSet.Health"), ignoring case. Both spellings are
    // tried on one attribute before the next one is looked at, so registration order decides
    // between competing matches. Returns an invalid attribute when nothing matches.
    FGameplayAttribute FindRegisteredAttribute(UAbilitySystemComponent* Asc, const FString& Wanted)
    {
        if (Asc == nullptr || Wanted.IsEmpty())
        {
            return FGameplayAttribute();
        }

        TArray<FGameplayAttribute> Registered;
        Asc->GetAllAttributes(Registered);

        const FGameplayAttribute* Hit = Registered.FindByPredicate([&Wanted](const FGameplayAttribute& Candidate)
        {
            if (!Candidate.IsValid())
            {
                return false;
            }
            const FString BareName = Candidate.GetName();
            if (BareName.Equals(Wanted, ESearchCase::IgnoreCase))
            {
                return true;
            }
            const UClass* SetClass = Candidate.GetAttributeSetClass();
            return SetClass != nullptr
                && (SetClass->GetName() + TEXT(".") + BareName).Equals(Wanted, ESearchCase::IgnoreCase);
        });
        return (Hit != nullptr) ? *Hit : FGameplayAttribute();
    }

    // Writes attribute_name, attribute_set (when the owning set class is known), value
    // (current value, modifiers included), base_value and found into Into.
    void WriteAttributeReading(FJsonObject& Into, const UAbilitySystemComponent* Asc, const FGameplayAttribute& Attribute)
    {
        Into.SetStringField(TEXT("attribute_name"), Attribute.GetName());
        if (const UClass* SetClass = Attribute.GetAttributeSetClass())
        {
            Into.SetStringField(TEXT("attribute_set"), SetClass->GetName());
        }

        bool bFound = false;
        const float CurrentValue = Asc->GetGameplayAttributeValue(Attribute, bFound);
        Into.SetNumberField(TEXT("value"), CurrentValue);
        Into.SetNumberField(TEXT("base_value"), Asc->GetNumericAttributeBase(Attribute));
        Into.SetBoolField(TEXT("found"), bFound);
    }
}

// --- Reading an actor's ability system component -------------------------------

// MCP:get_ability_system_component - Read a short census of an editor-world actor's ability system component: component_name, active_abilities_count (every granted spec, running or not) and active_effects_count. Call it first to confirm an actor has a component before grant_ability, apply_gameplay_effect or send_gameplay_event_to_actor; for the entries behind the counts use get_active_abilities, get_active_effects or get_attributes. The component is the actor's IAbilitySystemInterface answer, else its first UAbilitySystemComponent. Read-only and idempotent; it reads the editor world, not a PIE session. A missing actor or component is an error, so has_ability_system is never false.
//   @param actor_name - Object name (not the label) of the editor-world actor, e.g. "BP_Hero_C_1": the whole name, case-insensitive, no wildcards. get_actors_in_level lists names; find_actors_by_name maps a label to its name
//   @returns { actor_name, has_ability_system, component_name, active_abilities_count, active_effects_count }
TSharedPtr<FJsonObject> HandleGetAbilitySystemComponent(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentGameplayAbilitiesImpl;

    FString ActorName;
    const Impl::FAbilityOwner Owner = Impl::ReadAbilityOwner(Params, ActorName);
    if (Owner.Error.IsValid())
    {
        return Owner.Error;
    }

    // A short census of the component: its object name, how many abilities it could
    // activate and how many gameplay effects are applied to it right now.
    TSharedPtr<FJsonObject> Reply = Impl::NewActorReply(ActorName);
    Reply->SetBoolField(TEXT("has_ability_system"), true);
    Reply->SetStringField(TEXT("component_name"), Owner.Asc->GetName());
    Reply->SetNumberField(TEXT("active_abilities_count"), Owner.Asc->GetActivatableAbilities().Num());
    Reply->SetNumberField(TEXT("active_effects_count"), Owner.Asc->GetNumActiveGameplayEffects());
    return Reply;
}

// MCP:get_active_abilities - List every ability granted to an editor-world actor ability system component, running or not (all activatable specs, despite the tool name): ability_name (the class default object name), ability_class, level and is_active, plus count. Read-only; get_ability_info reads one.
//   @param actor_name - Object name (not the label) of the editor-world actor, e.g. "BP_Hero_C_1", matched case-insensitively; find_actors_by_name maps labels to names
//   @returns { ability_name, ability_class, level, is_active, actor_name, abilities, count }
TSharedPtr<FJsonObject> HandleGetActiveAbilities(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentGameplayAbilitiesImpl;

    FString ActorName;
    const Impl::FAbilityOwner Owner = Impl::ReadAbilityOwner(Params, ActorName);
    if (Owner.Error.IsValid())
    {
        return Owner.Error;
    }

    // One entry per activatable spec. A spec whose ability object is gone is left out
    // instead of being reported with empty fields.
    TArray<TSharedPtr<FJsonValue>> Entries;
    for (const FGameplayAbilitySpec& Spec : Owner.Asc->GetActivatableAbilities())
    {
        if (Spec.Ability == nullptr)
        {
            continue;
        }
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("ability_name"), Spec.Ability->GetName());
        Entry->SetStringField(TEXT("ability_class"), Spec.Ability->GetClass()->GetName());
        Entry->SetNumberField(TEXT("level"), Spec.Level);
        Entry->SetBoolField(TEXT("is_active"), Spec.IsActive());
        Entries.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> Reply = Impl::NewActorReply(ActorName);
    Reply->SetArrayField(TEXT("abilities"), Entries);
    Reply->SetNumberField(TEXT("count"), Entries.Num());
    return Reply;
}

// MCP:get_active_effects - List the gameplay effects currently active on an editor-world actor ability system component: effect_name (the definition object name), effect_class and level for each, plus count. Read-only; effects whose definition is gone are skipped. get_gameplay_effect_info reads one.
//   @param actor_name - Object name (not the label) of the editor-world actor, e.g. "BP_Hero_C_1", matched case-insensitively; find_actors_by_name maps labels to names
//   @returns { effect_name, effect_class, level, actor_name, effects, count }
TSharedPtr<FJsonObject> HandleGetActiveEffects(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentGameplayAbilitiesImpl;

    FString ActorName;
    const Impl::FAbilityOwner Owner = Impl::ReadAbilityOwner(Params, ActorName);
    if (Owner.Error.IsValid())
    {
        return Owner.Error;
    }

    TArray<TSharedPtr<FJsonValue>> Entries;
    Impl::VisitDefinedEffects(Owner.Asc, [&Entries](const FActiveGameplayEffectHandle&, const FActiveGameplayEffect& Effect)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("effect_name"), Effect.Spec.Def->GetName());
        Entry->SetStringField(TEXT("effect_class"), Effect.Spec.Def->GetClass()->GetName());
        Entry->SetNumberField(TEXT("level"), Effect.Spec.GetLevel());
        Entries.Add(MakeShared<FJsonValueObject>(Entry));
        return true;
    });

    TSharedPtr<FJsonObject> Reply = Impl::NewActorReply(ActorName);
    Reply->SetArrayField(TEXT("effects"), Entries);
    Reply->SetNumberField(TEXT("count"), Entries.Num());
    return Reply;
}

// --- Abilities: grant, activate, cancel, inspect, remove -------------------------

// MCP:activate_ability - Try to activate one granted ability on an editor-world actor ability system component with TryActivateAbility; activated is the engine verdict, false when the activation checks of the ability refuse it. Use grant_ability first, and cancel_ability to stop it.
//   @param actor_name - Object name (not the label) of the editor-world actor, e.g. "BP_Hero_C_1", matched case-insensitively; find_actors_by_name maps labels to names
//   @param ability_name - Ability to match among the granted specs: the ability_name from get_active_abilities, the class name with or without _C ("GA_Jump"), the asset path ("/Game/Abilities/GA_Jump") or the class path; case-insensitive, first match wins
//   @returns { actor_name, ability_name, activated }
TSharedPtr<FJsonObject> HandleActivateAbility(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentGameplayAbilitiesImpl;

    FString ActorName, AbilityName;
    FGameplayAbilitySpecHandle SpecHandle;
    const Impl::FAbilityOwner Owner = Impl::ReadAbilitySpecHandle(Params, ActorName, AbilityName, SpecHandle);
    if (Owner.Error.IsValid())
    {
        return Owner.Error;
    }

    // The engine's own verdict: false when the ability's activation checks refuse it.
    const bool bActivated = Owner.Asc->TryActivateAbility(SpecHandle);

    TSharedPtr<FJsonObject> Reply = Impl::NewActorReply(ActorName, TEXT("ability_name"), AbilityName);
    Reply->SetBoolField(TEXT("activated"), bActivated);
    return Reply;
}

// MCP:cancel_ability - Stop a running ability on an editor-world actor's ability system component (CancelAbilityHandle) while keeping it granted, reading its spec before and after: was_active, still_active and cancelled. Start it again with activate_ability; to ungrant it use remove_ability instead; get_active_abilities shows which granted abilities are running (is_active). Idempotent: cancelling one that is not running changes nothing and still succeeds with cancelled=false. success is false, with an error, when it is still active afterwards (the ability may not allow cancelling). Runtime state only: no undo step, nothing saved. An unknown actor, a missing component or no matching granted ability is an error.
//   @param actor_name - Object name (not the label) of the editor-world actor, e.g. "BP_Hero_C_1": the whole name, case-insensitive, no wildcards. get_actors_in_level lists names; find_actors_by_name maps a label to its name
//   @param ability_name - Granted ability to cancel: the ability_name get_active_abilities returns, the class name with or without _C ("GA_Jump"), the asset path ("/Game/Abilities/GA_Jump") or the class path; case-insensitive, first matching spec wins
//   @returns { actor_name, ability_name, was_active, still_active, cancelled } -- read back from the spec: cancelled means was_active and not still_active
TSharedPtr<FJsonObject> HandleCancelAbility(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentGameplayAbilitiesImpl;

    FString ActorName, AbilityName;
    FGameplayAbilitySpecHandle SpecHandle;
    const Impl::FAbilityOwner Owner = Impl::ReadAbilitySpecHandle(Params, ActorName, AbilityName, SpecHandle);
    if (Owner.Error.IsValid())
    {
        return Owner.Error;
    }

    // CancelAbilityHandle returns nothing: the spec is read before and after instead.
    auto IsActiveNow = [&Owner, &SpecHandle]()
    {
        const FGameplayAbilitySpec* Spec = Owner.Asc->FindAbilitySpecFromHandle(SpecHandle);
        return Spec != nullptr && Spec->IsActive();
    };
    const bool bWasActive = IsActiveNow();
    Owner.Asc->CancelAbilityHandle(SpecHandle);
    const bool bStillActive = IsActiveNow();

    TSharedPtr<FJsonObject> Reply = Impl::NewActorReply(ActorName, TEXT("ability_name"), AbilityName);
    Reply->SetBoolField(TEXT("was_active"), bWasActive);
    Reply->SetBoolField(TEXT("still_active"), bStillActive);
    Reply->SetBoolField(TEXT("cancelled"), bWasActive && !bStillActive);
    Reply->SetBoolField(TEXT("success"), !bStillActive);
    if (bStillActive)
    {
        Reply->SetStringField(TEXT("error"), TEXT("cancel_ability: the ability is still active after the cancel (it may not allow cancelling)."));
    }
    return Reply;
}

// MCP:get_ability_info - Read one ability granted to an editor-world actor ability system component: ability_class, level, is_active and input_id of the first matching spec. Read-only; an error when no granted spec matches ability_name. Use get_active_abilities to list them all.
//   @param actor_name - Object name (not the label) of the editor-world actor, e.g. "BP_Hero_C_1", matched case-insensitively; find_actors_by_name maps labels to names
//   @param ability_name - Ability to match among the granted specs: the ability_name from get_active_abilities, the class name with or without _C ("GA_Jump"), the asset path ("/Game/Abilities/GA_Jump") or the class path; case-insensitive, first match wins
//   @returns { actor_name, ability_name, ability_class, level, is_active, input_id }
TSharedPtr<FJsonObject> HandleGetAbilityInfo(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentGameplayAbilitiesImpl;

    FString ActorName, AbilityName;
    const Impl::FAbilityOwner Owner = Impl::ReadAbilityOwnerAndAbility(Params, ActorName, AbilityName);
    if (Owner.Error.IsValid())
    {
        return Owner.Error;
    }

    // Read-only, so a pointer into the spec array is safe for the rest of this call.
    const FGameplayAbilitySpec* Spec = Impl::FindSpecNamed(Owner.Asc, AbilityName);
    if (Spec == nullptr)
    {
        return Impl::AbilityNotFound(AbilityName);
    }

    TSharedPtr<FJsonObject> Reply = Impl::NewActorReply(ActorName, TEXT("ability_name"), AbilityName);
    Reply->SetStringField(TEXT("ability_class"), Spec->Ability->GetClass()->GetName());
    Reply->SetNumberField(TEXT("level"), Spec->Level);
    Reply->SetBoolField(TEXT("is_active"), Spec->IsActive());
    Reply->SetNumberField(TEXT("input_id"), Spec->InputID);
    return Reply;
}

// MCP:grant_ability - Grant an ability class to an editor-world actor ability system component with GiveAbility at a whole-number level; granted tells whether a valid spec handle came back. Runtime state only (not saved with the level), and granting the same class again adds another spec.
//   @param actor_name - Object name (not the label) of the editor-world actor, e.g. "BP_Hero_C_1", matched case-insensitively; find_actors_by_name maps labels to names
//   @param ability_class - Class path of a UGameplayAbility subclass: "/Game/Abilities/GA_Jump.GA_Jump_C" for a Blueprint, "/Script/MyGame.MyAbility" for C++
//   @param level - Spec level, a whole number; default 1
//   @returns { actor_name, ability_class, level, granted }
TSharedPtr<FJsonObject> HandleGrantAbility(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentGameplayAbilitiesImpl;

    FString ActorName, AbilityClassPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ability_class"), AbilityClassPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    const Impl::FAbilityOwner Owner = Impl::ResolveAbilityOwner(ActorName);
    if (Owner.Error.IsValid())
    {
        return Owner.Error;
    }

    // LoadClass only accepts classes deriving from UGameplayAbility.
    UClass* AbilityClass = LoadClass<UGameplayAbility>(nullptr, *AbilityClassPath);
    if (AbilityClass == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to load ability class: ") + AbilityClassPath);
    }

    // Spec levels are whole numbers; the optional "level" is read as an int32.
    int32 GrantLevel = 1;
    Params->TryGetNumberField(TEXT("level"), GrantLevel);

    const FGameplayAbilitySpecHandle GrantedHandle = Owner.Asc->GiveAbility(FGameplayAbilitySpec(AbilityClass, GrantLevel));

    TSharedPtr<FJsonObject> Reply = Impl::NewActorReply(ActorName, TEXT("ability_class"), AbilityClassPath);
    Reply->SetNumberField(TEXT("level"), GrantLevel);
    Reply->SetBoolField(TEXT("granted"), GrantedHandle.IsValid());
    return Reply;
}

// MCP:remove_ability - Ungrant one ability from an editor-world actor's ability system component with ClearAbility, the inverse of grant_ability. To only stop a running ability and keep it granted, use cancel_ability instead; get_active_abilities lists what is granted and confirms the result. A running ability is only marked and leaves when it ends (pending_removal=true, still a success); removed=true means the spec is gone now. Only the first matching spec goes, so a class granted twice needs two calls; once none is left a call answers Ability not found. success is false with an error if the spec is still granted afterwards. Runtime state only: no undo step, nothing saved. An unknown actor or missing component is an error and changes nothing.
//   @param actor_name - Object name (not the label) of the editor-world actor, e.g. "BP_Hero_C_1": the whole name, case-insensitive, no wildcards. get_actors_in_level lists names; find_actors_by_name maps a label to its name
//   @param ability_name - Granted ability to remove: the ability_name get_active_abilities returns, the class name with or without _C ("GA_Jump"), the asset path ("/Game/Abilities/GA_Jump") or the class path; case-insensitive, first matching spec wins
//   @returns { actor_name, ability_name, removed, pending_removal } -- read back: removed means the spec is gone; an active ability is only marked, and leaves when it ends (pending_removal)
TSharedPtr<FJsonObject> HandleRemoveAbility(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentGameplayAbilitiesImpl;

    FString ActorName, AbilityName;
    FGameplayAbilitySpecHandle SpecHandle;
    const Impl::FAbilityOwner Owner = Impl::ReadAbilitySpecHandle(Params, ActorName, AbilityName, SpecHandle);
    if (Owner.Error.IsValid())
    {
        return Owner.Error;
    }

    // ClearAbility removes the spec from the very array the handle was found in; the
    // handle passed here is our own copy, not a reference into that array.
    Owner.Asc->ClearAbility(SpecHandle);
    const FGameplayAbilitySpec* Left = Owner.Asc->FindAbilitySpecFromHandle(SpecHandle);
    const bool bRemoved = Left == nullptr;
    const bool bPending = Left != nullptr && Left->PendingRemove;

    TSharedPtr<FJsonObject> Reply = Impl::NewActorReply(ActorName, TEXT("ability_name"), AbilityName);
    Reply->SetBoolField(TEXT("removed"), bRemoved);
    Reply->SetBoolField(TEXT("pending_removal"), bPending);
    Reply->SetBoolField(TEXT("success"), bRemoved || bPending);
    if (!bRemoved && !bPending)
    {
        Reply->SetStringField(TEXT("error"), TEXT("remove_ability: the ability spec is still granted after ClearAbility."));
    }
    return Reply;
}

// --- Gameplay effects applied to a component -------------------------------------

// MCP:apply_gameplay_effect - Apply a GameplayEffect class to an editor-world actor's ability system component: an outgoing spec at the given level, with a fresh context from that component, applied to self. Duration and infinite effects then show in get_active_effects and come off with remove_gameplay_effect; an instant effect changes base values at once and leaves nothing active. Check the outcome with get_attributes or get_attribute_value; to set one value directly use set_attribute_base_value. applied is the engine's verdict (WasSuccessfullyApplied), true for an instant effect that executed; active says whether an active effect now stands for it, false for an instant one. When the engine refuses the effect (application requirements, an immunity, its chance to apply) applied is false and the call fails, with nothing changed. Not idempotent: each call adds another application or stack. Runtime state only: no undo step, nothing saved. execute_batch of apply_gameplay_effect calls applies a list.
//   @param actor_name - Object name (not the label) of the editor-world actor, e.g. "BP_Hero_C_1": the whole name, case-insensitive, no wildcards. get_actors_in_level lists names; find_actors_by_name maps a label to its name
//   @param effect_class - Class path of a UGameplayEffect subclass, loaded with LoadClass: a Blueprint GE needs its _C class path, e.g. "/Game/Effects/GE_Burn.GE_Burn_C"; a C++ one "/Script/MyGame.GE_Burn". create_gameplay_effect_blueprint makes one; an unloadable path is an error
//   @param level - Effect level that scalable magnitudes are read at, a number, fractions allowed (e.g. 2.5); default 1.0
//   @returns { actor_name, effect_class, level, applied, active } -- applied: the engine applied or executed the effect; active: an active effect now tracks it (duration or infinite effects)
TSharedPtr<FJsonObject> HandleApplyGameplayEffect(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentGameplayAbilitiesImpl;

    FString ActorName, EffectClassPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("effect_class"), EffectClassPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    const Impl::FAbilityOwner Owner = Impl::ResolveAbilityOwner(ActorName);
    if (Owner.Error.IsValid())
    {
        return Owner.Error;
    }

    const TSubclassOf<UGameplayEffect> EffectClass = LoadClass<UGameplayEffect>(nullptr, *EffectClassPath);
    if (!EffectClass)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to load effect class: ") + EffectClassPath);
    }

    // Effect levels are fractional, so the optional "level" is read as a float.
    float EffectLevel = 1.0f;
    Params->TryGetNumberField(TEXT("level"), EffectLevel);

    // Outgoing spec built with a fresh context from this same component, then applied to it.
    const FGameplayEffectSpecHandle OutgoingSpec = Owner.Asc->MakeOutgoingSpec(EffectClass, EffectLevel, Owner.Asc->MakeEffectContext());
    if (!OutgoingSpec.IsValid())
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to create effect spec"));
    }
    const FActiveGameplayEffectHandle AppliedHandle = Owner.Asc->ApplyGameplayEffectSpecToSelf(*OutgoingSpec.Data.Get());

    // WasSuccessfullyApplied is the engine's verdict for every duration policy. IsValid() is not: an Instant
    // effect that executed comes back as a handle that tracks no active effect, so IsValid() is false for it.
    const bool bApplied = AppliedHandle.WasSuccessfullyApplied();
    const bool bActive = AppliedHandle.IsValid() && Owner.Asc->GetActiveGameplayEffect(AppliedHandle) != nullptr;

    TSharedPtr<FJsonObject> Reply = Impl::NewActorReply(ActorName, TEXT("effect_class"), EffectClassPath);
    Reply->SetNumberField(TEXT("level"), EffectLevel);
    Reply->SetBoolField(TEXT("applied"), bApplied);
    Reply->SetBoolField(TEXT("active"), bActive);
    return mcp_common::MakeOutcomeReply(Reply, bApplied, bApplied ? FString()
        : FString(TEXT("apply_gameplay_effect: the engine did not apply the effect (its application requirements, an immunity or its chance to apply refused it); nothing changed.")));
}

// MCP:remove_gameplay_effect - Remove one active gameplay effect, with all its stacks, from an editor-world actor's ability system component (RemoveActiveGameplayEffect): the first whose definition matches effect_name. The inverse of apply_gameplay_effect; get_active_effects lists what is active and confirms the result. To change the effect asset itself use remove_gameplay_effect_modifier or remove_gameplay_effect_component instead. Instant effects never stay active, so they cannot be removed here (restore a value with set_attribute_base_value). When several active effects share a definition only the first goes, so call again for each; with none left a call answers Effect not found. removed is the engine answer confirmed by the effect no longer being active; success is false with an error otherwise. Runtime state only: no undo step, nothing saved.
//   @param actor_name - Object name (not the label) of the editor-world actor, e.g. "BP_Hero_C_1": the whole name, case-insensitive, no wildcards. get_actors_in_level lists names; find_actors_by_name maps a label to its name
//   @param effect_name - Active effect to remove, matched by its definition: the effect_name get_active_effects returns, the class name with or without _C ("GE_Burn"), the asset path ("/Game/Effects/GE_Burn") or the class path; case-insensitive, first active match wins
//   @returns { actor_name, effect_name, removed } -- removed is what the engine reports, confirmed by the effect not being active afterwards
TSharedPtr<FJsonObject> HandleRemoveGameplayEffect(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentGameplayAbilitiesImpl;

    FString ActorName, EffectName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("effect_name"), EffectName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    const Impl::FAbilityOwner Owner = Impl::ResolveAbilityOwner(ActorName);
    if (Owner.Error.IsValid())
    {
        return Owner.Error;
    }

    FActiveGameplayEffectHandle Doomed;
    if (Impl::FindActiveEffectNamed(Owner.Asc, EffectName, &Doomed) == nullptr)
    {
        return Impl::EffectNotFound(EffectName);
    }

    // All stacks go (the engine default of StacksToRemove = -1).
    const bool bEngineSaysRemoved = Owner.Asc->RemoveActiveGameplayEffect(Doomed);
    const bool bRemoved = bEngineSaysRemoved && Owner.Asc->GetActiveGameplayEffect(Doomed) == nullptr;

    TSharedPtr<FJsonObject> Reply = Impl::NewActorReply(ActorName, TEXT("effect_name"), EffectName);
    Reply->SetBoolField(TEXT("removed"), bRemoved);
    Reply->SetBoolField(TEXT("success"), bRemoved);
    if (!bRemoved)
    {
        Reply->SetStringField(TEXT("error"), TEXT("remove_gameplay_effect: the effect is still active after RemoveActiveGameplayEffect."));
    }
    return Reply;
}

// MCP:get_gameplay_effect_info - Read the first active gameplay effect on an editor-world actor ability system component whose definition matches effect_name: effect_class and level. Read-only; an error when none matches. For an effect asset that is not applied, use get_gameplay_effect_modifiers instead.
//   @param actor_name - Object name (not the label) of the editor-world actor, e.g. "BP_Hero_C_1", matched case-insensitively; find_actors_by_name maps labels to names
//   @param effect_name - Active effect to match by its definition: the effect_name from get_active_effects, the class name with or without _C ("GE_Burn"), the asset path ("/Game/Effects/GE_Burn") or the class path; case-insensitive, first match wins
//   @returns { actor_name, effect_name, effect_class, level }
TSharedPtr<FJsonObject> HandleGetGameplayEffectInfo(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentGameplayAbilitiesImpl;

    FString ActorName, EffectName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("effect_name"), EffectName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    const Impl::FAbilityOwner Owner = Impl::ResolveAbilityOwner(ActorName);
    if (Owner.Error.IsValid())
    {
        return Owner.Error;
    }

    const FActiveGameplayEffect* Effect = Impl::FindActiveEffectNamed(Owner.Asc, EffectName);
    if (Effect == nullptr)
    {
        return Impl::EffectNotFound(EffectName);
    }

    TSharedPtr<FJsonObject> Reply = Impl::NewActorReply(ActorName, TEXT("effect_name"), EffectName);
    Reply->SetStringField(TEXT("effect_class"), Effect->Spec.Def->GetClass()->GetName());
    Reply->SetNumberField(TEXT("level"), Effect->Spec.GetLevel());
    return Reply;
}

// --- Attribute values (current and base) -----------------------------------------

// MCP:get_attributes - List every attribute of the attribute sets registered on the ability system component of an editor-world actor (its IAbilitySystemInterface answer, else its first UAbilitySystemComponent), each with attribute_name, attribute_set, value (current, modifiers included), base_value and found, plus count. Read-only. A missing actor or component is an error; a component with no registered set gives count 0. Use get_attribute_value for one attribute, set_attribute_base_value to change one.
//   @param actor_name - Object name (not the label) of the editor-world actor, e.g. "BP_Hero_C_1", matched case-insensitively; find_actors_by_name maps labels to names
//   @returns { actor_name: string, attributes: [{ attribute_name: string, attribute_set?: string, value: number, base_value: number, found: bool }], count: int }
TSharedPtr<FJsonObject> HandleGetAttributes(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentGameplayAbilitiesImpl;

    FString ActorName;
    const Impl::FAbilityOwner Owner = Impl::ReadAbilityOwner(Params, ActorName);
    if (Owner.Error.IsValid())
    {
        return Owner.Error;
    }

    // Every attribute contributed by the attribute sets registered on the component.
    TArray<FGameplayAttribute> Registered;
    Owner.Asc->GetAllAttributes(Registered);

    TArray<TSharedPtr<FJsonValue>> Readings;
    for (const FGameplayAttribute& Attribute : Registered)
    {
        if (!Attribute.IsValid())
        {
            continue;
        }
        TSharedPtr<FJsonObject> Reading = MakeShared<FJsonObject>();
        Impl::WriteAttributeReading(*Reading, Owner.Asc, Attribute);
        Readings.Add(MakeShared<FJsonValueObject>(Reading));
    }

    TSharedPtr<FJsonObject> Reply = Impl::NewActorReply(ActorName);
    Reply->SetArrayField(TEXT("attributes"), Readings);
    Reply->SetNumberField(TEXT("count"), Readings.Num());
    return Reply;
}

// MCP:get_attribute_value - Read one attribute from the attribute sets registered on an editor-world actor ability system component: value (current, modifiers included), base_value and attribute_set. Read-only; an unknown attribute is not an error but found=false. get_attributes lists every attribute.
//   @param actor_name - Object name (not the label) of the editor-world actor, e.g. "BP_Hero_C_1", matched case-insensitively; find_actors_by_name maps labels to names
//   @param attribute_name - Attribute name ("Health") or "SetClass.Attribute" ("MyAttributeSet.Health"), case-insensitive; the first registered attribute that matches wins
//   @returns { actor_name, attribute_name, found, value, base_value, attribute_set }
TSharedPtr<FJsonObject> HandleGetAttributeValue(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentGameplayAbilitiesImpl;

    FString ActorName, AttributeName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("attribute_name"), AttributeName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    const Impl::FAbilityOwner Owner = Impl::ResolveAbilityOwner(ActorName);
    if (Owner.Error.IsValid())
    {
        return Owner.Error;
    }

    TSharedPtr<FJsonObject> Reply = Impl::NewActorReply(ActorName);

    const FGameplayAttribute Attribute = Impl::FindRegisteredAttribute(Owner.Asc, AttributeName);
    if (Attribute.IsValid())
    {
        Impl::WriteAttributeReading(*Reply, Owner.Asc, Attribute);
        return Reply;
    }

    // No registered attribute set declares this name (a component without any attribute
    // set has no attributes at all). That is an answer, not a failure: found=false.
    Reply->SetStringField(TEXT("attribute_name"), AttributeName);
    Reply->SetBoolField(TEXT("found"), false);
    return Reply;
}

// MCP:set_attribute_base_value - Set the base value of one attribute on an editor-world actor ability system component (SetNumericAttributeBase); active modifiers keep applying on top. Returns the read-back base_value and current value; set=false, not an error, when no registered attribute set has that attribute.
//   @param actor_name - Object name (not the label) of the editor-world actor, e.g. "BP_Hero_C_1", matched case-insensitively; find_actors_by_name maps labels to names
//   @param attribute_name - Attribute name ("Health") or "SetClass.Attribute" ("MyAttributeSet.Health"), case-insensitive; the first registered attribute that matches wins
//   @param value - The new base value for the attribute
//   @returns { actor_name, attribute_name, value, set, base_value }
TSharedPtr<FJsonObject> HandleSetAttributeBaseValue(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentGameplayAbilitiesImpl;

    FString ActorName, AttributeName;
    float RequestedBase = 0.0f;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("attribute_name"), AttributeName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("value"), RequestedBase)) { return Missing; }

    const Impl::FAbilityOwner Owner = Impl::ResolveAbilityOwner(ActorName);
    if (Owner.Error.IsValid())
    {
        return Owner.Error;
    }

    TSharedPtr<FJsonObject> Reply = Impl::NewActorReply(ActorName);

    const FGameplayAttribute Attribute = Impl::FindRegisteredAttribute(Owner.Asc, AttributeName);
    if (!Attribute.IsValid())
    {
        // Nothing to write to: say so (set=false) and echo the requested value, rather than
        // pretending the write happened.
        Reply->SetStringField(TEXT("attribute_name"), AttributeName);
        Reply->SetNumberField(TEXT("value"), RequestedBase);
        Reply->SetBoolField(TEXT("set"), false);
        return Reply;
    }

    // Only the permanent base value changes. Active modifiers stay in place and now act on
    // the new base, which is why the current value is read back separately below.
    Owner.Asc->SetNumericAttributeBase(Attribute, RequestedBase);

    bool bFoundAfterWrite = false;
    const float CurrentAfterWrite = Owner.Asc->GetGameplayAttributeValue(Attribute, bFoundAfterWrite);
    const float BaseAfterWrite = Owner.Asc->GetNumericAttributeBase(Attribute);

    Reply->SetStringField(TEXT("attribute_name"), Attribute.GetName());
    Reply->SetNumberField(TEXT("value"), CurrentAfterWrite);
    Reply->SetNumberField(TEXT("base_value"), BaseAfterWrite);
    Reply->SetBoolField(TEXT("set"), true);
    return Reply;
}

// ---------------------------------------------------------------------------
// Ability task listing + GameplayEffect asset authoring.
// ---------------------------------------------------------------------------

namespace
{
    // Resolve a UGameplayEffect by /Game path or class name. Returns nullptr on
    // failure (path empty, nothing resolves, not a UGameplayEffect).
    //
    // GEs authored through this plugin are BLUEPRINTS (create_gameplay_effect_blueprint
    // -> /Game/.../GE_X, a UBlueprint whose GeneratedClass CDO is the UGameplayEffect);
    // the GAS-authoring MUTATORS resolve them via the blueprint-CDO path
    // (LoadGameplayEffectCDO in UEAgentGASAuthoringCommands.cpp). The GE READERS
    // here must resolve the SAME shapes, so resolution is additive:
    //   1) a NATIVE UGameplayEffect asset (LoadObject<UGameplayEffect>, +".basename"),
    //   2) ELSE a UBlueprint whose GeneratedClass CDO is a UGameplayEffect.
    // A native asset resolves via (1) first; a blueprint-authored GE resolves via (2).
    // If NEITHER resolves
    // we return nullptr (the caller emits an honest "not found").
    UGameplayEffect* LoadGameplayEffectAsset(const FString& PathOrName)
    {
        if (PathOrName.IsEmpty())
        {
            return nullptr;
        }
        // (1) Native UGameplayEffect asset, by full path then by ".basename" object name.
        if (UGameplayEffect* Effect = LoadObject<UGameplayEffect>(nullptr, *PathOrName))
        {
            UE_LOG(LogTemp, Verbose, TEXT("[UEAgent] LoadGameplayEffectAsset: resolved native UGameplayEffect '%s'"), *PathOrName);
            return Effect;
        }
        if (UGameplayEffect* Effect = LoadObject<UGameplayEffect>(nullptr, *(PathOrName + TEXT(".") + FPaths::GetBaseFilename(PathOrName))))
        {
            UE_LOG(LogTemp, Verbose, TEXT("[UEAgent] LoadGameplayEffectAsset: resolved native UGameplayEffect '%s' (.basename)"), *PathOrName);
            return Effect;
        }
        // (2) Blueprint-authored GE: load the UBlueprint and read its GeneratedClass CDO,
        // mirroring LoadGameplayEffectCDO (the writers' resolution path).
        if (UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *PathOrName))
        {
            if (Blueprint->GeneratedClass)
            {
                if (UGameplayEffect* GECDO = Cast<UGameplayEffect>(Blueprint->GeneratedClass->GetDefaultObject()))
                {
                    UE_LOG(LogTemp, Verbose, TEXT("[UEAgent] LoadGameplayEffectAsset: resolved blueprint-authored GE CDO '%s'"), *PathOrName);
                    return GECDO;
                }
            }
        }
        UE_LOG(LogTemp, Verbose, TEXT("[UEAgent] LoadGameplayEffectAsset: nothing resolved for '%s'"), *PathOrName);
        return nullptr;
    }

    // Instant, HasDuration or Infinite.
    const TCHAR* GameplayAbilities_DurationPolicyName(EGameplayEffectDurationType Policy)
    {
        return Policy == EGameplayEffectDurationType::Instant ? TEXT("Instant") :
            Policy == EGameplayEffectDurationType::HasDuration ? TEXT("HasDuration") : TEXT("Infinite");
    }

    // The duration type a duration_policy string names (Instant, HasDuration or Infinite, in any case); false when it names none.
    bool GameplayAbilities_ParseDurationPolicy(const FString& Name, EGameplayEffectDurationType& OutPolicy)
    {
        for (const EGameplayEffectDurationType Policy : { EGameplayEffectDurationType::Instant,
                 EGameplayEffectDurationType::HasDuration, EGameplayEffectDurationType::Infinite })
        {
            if (Name.Equals(GameplayAbilities_DurationPolicyName(Policy), ESearchCase::IgnoreCase))
            {
                OutPolicy = Policy;
                return true;
            }
        }
        return false;
    }

}

// Gameplay tasks are listed from the actor's GameplayTasksComponents. GameplayEffect assets can be created,
// have their modifiers read back, or be applied in batches. Engine entry points:
//   UGameplayTasksComponent::GetKnownTaskIterator              GameplayTasksComponent.h
//   UGameplayEffect, EGameplayEffectDurationType               GameplayEffect.h
//   UAbilitySystemComponent::ApplyGameplayEffectSpecToSelf     AbilitySystemComponent.h

// MCP:get_ability_tasks - List the gameplay tasks the GameplayTasksComponents of an editor-world actor (its ability system component counts) know about, without changing anything: the tasks activated and not yet ended (the component's KnownTasks), plus any queued waiting for task resources. Each has task_name, task_class, task_state (Uninitialized, AwaitingActivation, Active, Paused or Finished), is_ability_task and owner_name (the object that started it, e.g. an ability instance); counts split ability tasks from other gameplay tasks. A task that was created but never activated is not known to the component and is not listed. An actor that is not found, or has no ability system component, is an error.
//   @param actor_name - Object name (not the label) of the editor-world actor, e.g. "BP_Hero_C_1", matched case-insensitively; find_actors_by_name maps labels to names
//   @returns { actor_name, task_count, ability_task_count, generic_task_count, tasks: [{ task_name, task_class, task_state, is_ability_task, owner_name }] }
TSharedPtr<FJsonObject> HandleGetAbilityTasks(const TSharedPtr<FJsonObject>& Params)
{
    UE_LOG(LogTemp, Verbose, TEXT("[UEAgent] get_ability_tasks: enter"));
    namespace Impl = UEAgentGameplayAbilitiesImpl;

    // The component itself is not read below, but an actor without one is still refused.
    FString ActorName;
    const Impl::FAbilityOwner Owner = Impl::ReadAbilityOwner(Params, ActorName);
    if (Owner.Error.IsValid())
    {
        return Owner.Error;
    }
    AActor* Actor = Owner.Actor;

    // Every engine task factory (UGameplayTask::NewTask, UAbilityTask::NewAbilityTask, UAITask) outers its task
    // to the transient package, so a task is found through the component that runs it, not through its outer:
    // the component's KnownTasks (added on activation, removed when the task ends), and its priority queue,
    // which also holds tasks still waiting for their resources. A task in both is listed once.
    TArray<TSharedPtr<FJsonValue>> TasksJson;
    int32 AbilityTaskCount = 0;
    int32 GenericTaskCount = 0;
    TSet<const UGameplayTask*> Listed;

    const auto AddTask = [&](const UGameplayTask* Task)
    {
        if (!Task || Listed.Contains(Task))
        {
            return;
        }
        Listed.Add(Task);
        const bool bIsAbilityTask = Task->IsA<UAbilityTask>();
        IGameplayTaskOwnerInterface* const OwnerInterface = Task->GetTaskOwner();
        const UObject* TaskOwner = OwnerInterface ? Cast<UObject>(OwnerInterface) : nullptr;
        TSharedPtr<FJsonObject> TaskObj = MakeShared<FJsonObject>();
        TaskObj->SetStringField(TEXT("task_name"), Task->GetName());
        TaskObj->SetStringField(TEXT("task_class"), Task->GetClass()->GetName());
        TaskObj->SetStringField(TEXT("task_state"), Task->GetTaskStateName());
        TaskObj->SetBoolField(TEXT("is_ability_task"), bIsAbilityTask);
        TaskObj->SetStringField(TEXT("owner_name"), TaskOwner ? TaskOwner->GetName() : FString());
        ++(bIsAbilityTask ? AbilityTaskCount : GenericTaskCount);
        TasksJson.Add(MakeShared<FJsonValueObject>(TaskObj));
    };

    TArray<UGameplayTasksComponent*> GTComps;
    Actor->GetComponents<UGameplayTasksComponent>(GTComps);
    for (const UGameplayTasksComponent* GTComp : GTComps)
    {
        for (auto It = GTComp->GetKnownTaskIterator(); It; ++It)
        {
            AddTask(*It);
        }
        for (auto It = GTComp->GetPriorityQueueIterator(); It; ++It)
        {
            AddTask(*It);
        }
    }

    TSharedPtr<FJsonObject> ResultObj = Impl::NewActorReply(ActorName);
    ResultObj->SetNumberField(TEXT("task_count"), TasksJson.Num());
    ResultObj->SetNumberField(TEXT("ability_task_count"), AbilityTaskCount);
    ResultObj->SetNumberField(TEXT("generic_task_count"), GenericTaskCount);
    ResultObj->SetArrayField(TEXT("tasks"), TasksJson);
    UE_LOG(LogTemp, Verbose, TEXT("[UEAgent] get_ability_tasks: %d tasks on %s (ability_task=%d, generic=%d)"),
        TasksJson.Num(), *ActorName, AbilityTaskCount, GenericTaskCount);
    return ResultObj;
}

// MCP:create_gameplay_effect_asset - Create and save a native UGameplayEffect asset (despite the name, not a container and not a Blueprint) at package_path/name. An existing asset there is returned untouched (already_existed=true). apply_gameplay_effect needs a class, so use create_gameplay_effect_blueprint for an effect it can apply.
//   @param name - Asset name without a path, e.g. "GE_Damage"
//   @param package_path - Target content package path where the asset is created (default /Game/GameplayEffects)
//   @param duration_policy - Duration type, case-insensitive: Instant, HasDuration or Infinite (default Instant)
//   @param duration_magnitude - Duration in seconds, applied when policy is HasDuration (default 0.0)
//   @returns { name, asset_path, already_existed, duration_policy, duration_magnitude, created }
TSharedPtr<FJsonObject> HandleCreateGameplayEffectContainer(const TSharedPtr<FJsonObject>& Params)
{
    UE_LOG(LogTemp, Verbose, TEXT("[UEAgent] create_gameplay_effect_asset: enter"));

    FString Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString PackagePath = TEXT("/Game/GameplayEffects");
    Params->TryGetStringField(TEXT("package_path"), PackagePath);
    PackagePath.RemoveFromEnd(TEXT("/"));

    FString DurationPolicyStr = TEXT("Instant");
    Params->TryGetStringField(TEXT("duration_policy"), DurationPolicyStr);

    float DurationMagnitude = 0.0f;
    Params->TryGetNumberField(TEXT("duration_magnitude"), DurationMagnitude);

    EGameplayEffectDurationType DurationPolicy = EGameplayEffectDurationType::Instant;
    if (!GameplayAbilities_ParseDurationPolicy(DurationPolicyStr, DurationPolicy))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Unknown duration_policy: %s (expected Instant|HasDuration|Infinite)"), *DurationPolicyStr));
    }

    // Idempotent: if asset already exists at the resolved path, return it. The asset's own package is
    // package_path/name; asset_path is the object path inside it.
    const FString PackageName = FString::Printf(TEXT("%s/%s"), *PackagePath, *Name);
    const FString AssetPath = FString::Printf(TEXT("%s.%s"), *PackageName, *Name);
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(AssetPath)) { return Invalid; }
    if (UGameplayEffect* Existing = LoadGameplayEffectAsset(AssetPath))
    {
        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
        ResultObj->SetStringField(TEXT("name"), Name);
        ResultObj->SetStringField(TEXT("asset_path"), AssetPath);
        ResultObj->SetBoolField(TEXT("already_existed"), true);
        ResultObj->SetStringField(TEXT("duration_policy"), GameplayAbilities_DurationPolicyName(DurationPolicy));
        float DurationMag = 0.f;
        if (Existing->DurationMagnitude.GetStaticMagnitudeIfPossible(1.f, DurationMag))
        {
            ResultObj->SetNumberField(TEXT("duration_magnitude"), DurationMag);
        }
        return ResultObj;
    }

    // Create the package + asset. UGameplayEffect inherits UObject (not a Blueprint) so
    // NewObject + Initialize is sufficient; no factory required.
    FString PackageError;
    UPackage* Package = mcp_common::CreateAssetPackage(PackageName, Name, UGameplayEffect::StaticClass(), PackageError);
    if (!Package)
    {
        return mcp_common::MakeErrorReply(PackageError);
    }
    Package->FullyLoad();

    UGameplayEffect* NewEffect = NewObject<UGameplayEffect>(
        Package, UGameplayEffect::StaticClass(), FName(*Name), RF_Public | RF_Standalone);
    if (!NewEffect)
    {
        return mcp_common::MakeErrorReply(TEXT("NewObject<UGameplayEffect> returned null"));
    }

    NewEffect->DurationPolicy = DurationPolicy;
    if (DurationPolicy == EGameplayEffectDurationType::HasDuration)
    {
        // FGameplayEffectModifierMagnitude DurationMagnitude (FScalableFloat). Set as a
        // constant magnitude at level 1; game-side can override via SetByCaller at runtime.
        NewEffect->DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(DurationMagnitude));
    }

    // Mark + notify + save.
    FAssetRegistryModule::AssetCreated(NewEffect);
    FString SaveError;
    if (!mcp_common::SaveAssetPackage(Package, NewEffect, SaveError))
    {
        return mcp_common::MakeErrorReply(SaveError);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("name"), Name);
    ResultObj->SetStringField(TEXT("asset_path"), AssetPath);
    ResultObj->SetBoolField(TEXT("created"), true);
    ResultObj->SetBoolField(TEXT("already_existed"), false);
    ResultObj->SetStringField(TEXT("duration_policy"), DurationPolicyStr);
    ResultObj->SetNumberField(TEXT("duration_magnitude"), DurationMagnitude);
    UE_LOG(LogTemp, Verbose, TEXT("[UEAgent] create_gameplay_effect_asset: created %s (policy=%s)"),
        *AssetPath, *DurationPolicyStr);
    return ResultObj;
}

// MCP:apply_multiple_gameplay_effects - Apply several GameplayEffect classes at one level to an editor-world actor ability system component; each entry is loaded and applied separately, so a bad path does not stop the rest. Returns requested, applied_count, failed_count and the per-entry applied and failed lists.
//   @retired execute_batch - one apply_gameplay_effect per effect
//   @param actor_name - Object name (not the label) of the editor-world actor, e.g. "BP_Hero_C_1", matched case-insensitively; find_actors_by_name maps labels to names
//   @param effect_classes - JSON array of UGameplayEffect class path strings, e.g. ["/Game/Effects/GE_Burn.GE_Burn_C"]; non-string entries go to failed
//   @param level - GameplayEffect/ability level applied to all effects (default 1.0)
//   @returns { error, effect_class, applied, active_handle, actor_name, requested, applied_count, failed_count, failed }
TSharedPtr<FJsonObject> HandleApplyMultipleGameplayEffects(const TSharedPtr<FJsonObject>& Params)
{
    UE_LOG(LogTemp, Verbose, TEXT("[UEAgent] apply_multiple_gameplay_effects: enter"));
    namespace Impl = UEAgentGameplayAbilitiesImpl;

    FString ActorName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    const TArray<TSharedPtr<FJsonValue>>* EffectClassesArrayPtr = nullptr;
    if (!Params->TryGetArrayField(TEXT("effect_classes"), EffectClassesArrayPtr) || !EffectClassesArrayPtr)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Missing required parameter: effect_classes (array of UGameplayEffect class paths)"));
    }

    const Impl::FAbilityOwner Owner = Impl::ResolveAbilityOwner(ActorName);
    if (Owner.Error.IsValid())
    {
        return Owner.Error;
    }
    UAbilitySystemComponent* ASC = Owner.Asc;

    float DefaultLevel = 1.0f;
    Params->TryGetNumberField(TEXT("level"), DefaultLevel);

    TArray<TSharedPtr<FJsonValue>> AppliedJson;
    TArray<TSharedPtr<FJsonValue>> FailedJson;
    int32 AppliedCount = 0;
    int32 FailedCount = 0;

    for (const TSharedPtr<FJsonValue>& Entry : *EffectClassesArrayPtr)
    {
        if (!Entry.IsValid() || Entry->Type != EJson::String)
        {
            TSharedPtr<FJsonObject> FailObj = MakeShared<FJsonObject>();
            FailObj->SetStringField(TEXT("error"), TEXT("Non-string entry in effect_classes"));
            FailedJson.Add(MakeShared<FJsonValueObject>(FailObj));
            ++FailedCount;
            continue;
        }
        const FString EffectClassPath = Entry->AsString();
        UClass* EffectClass = LoadClass<UGameplayEffect>(nullptr, *EffectClassPath);
        if (!EffectClass)
        {
            TSharedPtr<FJsonObject> FailObj = MakeShared<FJsonObject>();
            FailObj->SetStringField(TEXT("effect_class"), EffectClassPath);
            FailObj->SetStringField(TEXT("error"), TEXT("Class not found"));
            FailedJson.Add(MakeShared<FJsonValueObject>(FailObj));
            ++FailedCount;
            continue;
        }
        FGameplayEffectContextHandle Ctx = ASC->MakeEffectContext();
        FGameplayEffectSpecHandle Spec = ASC->MakeOutgoingSpec(EffectClass, DefaultLevel, Ctx);
        if (!Spec.IsValid())
        {
            TSharedPtr<FJsonObject> FailObj = MakeShared<FJsonObject>();
            FailObj->SetStringField(TEXT("effect_class"), EffectClassPath);
            FailObj->SetStringField(TEXT("error"), TEXT("MakeOutgoingSpec returned invalid handle"));
            FailedJson.Add(MakeShared<FJsonValueObject>(FailObj));
            ++FailedCount;
            continue;
        }
        const FActiveGameplayEffectHandle Active = ASC->ApplyGameplayEffectSpecToSelf(*Spec.Data.Get());
        TSharedPtr<FJsonObject> ApplyObj = MakeShared<FJsonObject>();
        ApplyObj->SetStringField(TEXT("effect_class"), EffectClassPath);
        ApplyObj->SetBoolField(TEXT("applied"), Active.IsValid());
        if (Active.IsValid())
        {
            ApplyObj->SetStringField(TEXT("active_handle"), Active.ToString());
            ++AppliedCount;
        }
        else
        {
            ++FailedCount;
        }
        AppliedJson.Add(MakeShared<FJsonValueObject>(ApplyObj));
    }

    TSharedPtr<FJsonObject> ResultObj = Impl::NewActorReply(ActorName);
    ResultObj->SetNumberField(TEXT("requested"), EffectClassesArrayPtr->Num());
    ResultObj->SetNumberField(TEXT("applied_count"), AppliedCount);
    ResultObj->SetNumberField(TEXT("failed_count"), FailedCount);
    ResultObj->SetArrayField(TEXT("applied"), AppliedJson);
    ResultObj->SetArrayField(TEXT("failed"), FailedJson);
    UE_LOG(LogTemp, Verbose, TEXT("[UEAgent] apply_multiple_gameplay_effects: %d/%d applied on %s"),
        AppliedCount, EffectClassesArrayPtr->Num(), *ActorName);
    return ResultObj;
}

// MCP:get_gameplay_effect_modifiers - Read the attribute modifiers authored on a GameplayEffect asset (a native UGameplayEffect asset, or a Blueprint GE's class default object): for each, attribute, attribute_set, modifier_op (Additive, Multiplicitive, Division or Override) and magnitude_value_at_level_1; plus duration_policy (Instant, HasDuration or Infinite) and modifier_count. Use it to inspect an effect before apply_gameplay_effect; for an effect already active on an actor use get_gameplay_effect_info, for the asset's components get_gameplay_effect_components, and change modifiers with add_gameplay_effect_modifier, set_gameplay_effect_modifier or remove_gameplay_effect_modifier. Read-only and idempotent. magnitude_type always says ScalableFloat whatever the real type; magnitude_value_at_level_1 is present only for a ScalableFloat magnitude, so its absence means attribute-based, SetByCaller or custom. An unknown path is an error.
//   @param effect_path - Asset path of the effect, not its _C class path: a Blueprint GE "/Game/Effects/GE_Burn" or a native UGameplayEffect asset "/Game/Effects/GE_Native" (".Name" suffix optional). find_assets_by_name finds it; create_gameplay_effect_blueprint or create_gameplay_effect_asset makes one
//   @returns { attribute, attribute_set, modifier_op, magnitude_type, magnitude_value_at_level_1, effect_path, duration_policy, modifier_count, modifiers }
TSharedPtr<FJsonObject> HandleGetGameplayEffectModifiers(const TSharedPtr<FJsonObject>& Params)
{
    UE_LOG(LogTemp, Verbose, TEXT("[UEAgent] get_gameplay_effect_modifiers: enter"));

    FString EffectPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("effect_path"), EffectPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UGameplayEffect* Effect = LoadGameplayEffectAsset(EffectPath);
    if (!Effect)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("UGameplayEffect asset not found: %s"), *EffectPath));
    }

    // Modifiers are UGameplayEffect::Modifiers (TArray<FGameplayModifierInfo>).
    // Each FGameplayModifierInfo carries an FGameplayAttribute (Attribute),
    // EGameplayModOp (ModifierOp), and an FGameplayEffectModifierMagnitude (ModifierMagnitude).
    TArray<TSharedPtr<FJsonValue>> ModifiersJson;
    for (const FGameplayModifierInfo& ModInfo : Effect->Modifiers)
    {
        TSharedPtr<FJsonObject> ModObj = MakeShared<FJsonObject>();
        if (ModInfo.Attribute.IsValid())
        {
            ModObj->SetStringField(TEXT("attribute"), ModInfo.Attribute.GetName());
            if (UClass* AttrSet = ModInfo.Attribute.GetAttributeSetClass())
            {
                ModObj->SetStringField(TEXT("attribute_set"), AttrSet->GetName());
            }
        }
        const FString OpStr =
            ModInfo.ModifierOp == EGameplayModOp::Additive ? TEXT("Additive") :
            ModInfo.ModifierOp == EGameplayModOp::Multiplicitive ? TEXT("Multiplicitive") :
            ModInfo.ModifierOp == EGameplayModOp::Division ? TEXT("Division") :
            ModInfo.ModifierOp == EGameplayModOp::Override ? TEXT("Override") : TEXT("Unknown");
        ModObj->SetStringField(TEXT("modifier_op"), OpStr);
        ModObj->SetStringField(TEXT("magnitude_type"), TEXT("ScalableFloat"));
        float ModMag = 0.f;
        if (ModInfo.ModifierMagnitude.GetStaticMagnitudeIfPossible(1.f, ModMag))
        {
            ModObj->SetNumberField(TEXT("magnitude_value_at_level_1"), ModMag);
        }
        ModifiersJson.Add(MakeShared<FJsonValueObject>(ModObj));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("effect_path"), EffectPath);
    ResultObj->SetStringField(TEXT("duration_policy"), GameplayAbilities_DurationPolicyName(Effect->DurationPolicy));
    ResultObj->SetNumberField(TEXT("modifier_count"), ModifiersJson.Num());
    ResultObj->SetArrayField(TEXT("modifiers"), ModifiersJson);
    return ResultObj;
}

// ---------------------------------------------------------------------------
// Attribute metadata authoring on the AttributeSet CDO.
//
// Authors a DataTable<FAttributeMetaData> that UAttributeSet::InitFromMetaDataTable
// expects at character init time. The AttributeSet CDO itself is NOT modified
// directly — only the DataTable is created/updated (by design; the
// runtime must call CDO->InitFromMetaDataTable(DataTable) at spawn time).
//
// Engine APIs used:
//   FAttributeMetaData                      GameplayAbilities/Public/AttributeSet.h
//   UAttributeSet::InitFromMetaDataTable   GameplayAbilities/Public/AttributeSet.h
//   UDataTable::AddRow(FName, const uint8*, const UScriptStruct*)  Engine/DataTable.h
//   FJsonObjectConverter::JsonValueToUProperty / UStructToJsonObject  JsonUtilities
//   UEditorAssetLibrary::DoesAssetExist / CreateAsset   EditorScriptingUtilities
// ---------------------------------------------------------------------------

namespace
{
    // Reads the required attribute_set_class_path and resolves it -- a /Script path, a /Game path or a bare
    // name -- to a UAttributeSet subclass. Null with OutError when it is missing or names no such class.
    UClass* GameplayAbilities_ReadAttributeSetClass(const TSharedPtr<FJsonObject>& Params, FString& OutError)
    {
        FString ClassPath;
        if (!Params->TryGetStringField(TEXT("attribute_set_class_path"), ClassPath))
        {
            OutError = TEXT("Missing required parameter: attribute_set_class_path");
            return nullptr;
        }
        UClass* Cls = nullptr;
        if (!ClassPath.IsEmpty())
        {
            Cls = LoadClass<UAttributeSet>(nullptr, *ClassPath);
            if (!Cls)
            {
                Cls = FindFirstObject<UClass>(*ClassPath);
            }
        }
        if (!Cls || !Cls->IsChildOf(UAttributeSet::StaticClass()))
        {
            OutError = FString::Printf(TEXT("UAttributeSet class not found or not a UAttributeSet: %s"), *ClassPath);
            return nullptr;
        }
        return Cls;
    }

    // Load a UDataTable from a /Game path, or nullptr on failure.
    UDataTable* GameplayAbilities_LoadDataTableAt(const FString& TablePath)
    {
        return Cast<UDataTable>(StaticLoadObject(UDataTable::StaticClass(), nullptr, *TablePath));
    }

    // Create a fresh /Game DataTable<FAttributeMetaData> at DataTablePath. Returns
    // the new asset or nullptr on failure, with OutError set when the name is taken
    // (mirrors HandleCreateDataTable's flow).
    UDataTable* CreateAttributeMetaDataTable(const FString& DataTablePath, FString& OutError)
    {
        const FString PackagePath = FPaths::GetPath(DataTablePath);
        const FString AssetName = FPaths::GetBaseFilename(DataTablePath);
        if (PackagePath.IsEmpty() || AssetName.IsEmpty())
        {
            return nullptr;
        }
        OutError = mcp_common::AssetNameConflict(DataTablePath, AssetName, UDataTable::StaticClass());
        if (!OutError.IsEmpty())
        {
            return nullptr;
        }

        UPackage* Package = CreatePackage(*DataTablePath);
        if (!Package)
        {
            return nullptr;
        }
        Package->FullyLoad();

        UDataTable* NewTable = NewObject<UDataTable>(
            Package, UDataTable::StaticClass(), *AssetName, RF_Public | RF_Standalone);
        if (!NewTable)
        {
            return nullptr;
        }
        NewTable->RowStruct = FAttributeMetaData::StaticStruct();
        FAssetRegistryModule::AssetCreated(NewTable);
        return NewTable;
    }
}

// MCP:set_attribute_meta_on_set_class - Author the DataTable<FAttributeMetaData> that gives a UAttributeSet class its starting/min/max values, then validate it by calling InitFromMetaDataTable on that class's CDO. WHAT IT DOES NOT DO: it never writes the CDO -- only the DataTable is authored, and the running game is still expected to call CDO->InitFromMetaDataTable(DataTable) itself at character init (typically from an ASC's InitStats path). Use set_attribute_base_value to change a LIVE actor's attribute instead.
//   @param attribute_set_class_path - Full UAttributeSet class path (e.g. "/Script/GameplayAbilities.AbilitySystemTestAttributeSet" or "/Game/MyAS")
//   @param data_table_path - Target asset path (default "/Game/UEAgent/AttributeMeta/<ClassName>_Meta")
//   @param metadata_rows - Array of {attribute_name, base_value, min_value, max_value, can_stack, derived_info}
//   @returns { attribute_set_class, data_table_path, row_count, rows_imported, init_from_meta_succeeded, saved, problems }
TSharedPtr<FJsonObject> HandleSetAttributeMetaOnSetClass(const TSharedPtr<FJsonObject>& Params)
{
    UE_LOG(LogTemp, Verbose, TEXT("[UEAgent] set_attribute_meta_on_set_class: enter"));

    FString Error;
    UClass* AttributeSetClass = GameplayAbilities_ReadAttributeSetClass(Params, Error);
    if (!AttributeSetClass)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    // Default DataTable path: /Game/UEAgent/AttributeMeta/<ClassName>_Meta.
    FString DataTablePath;
    if (!Params->TryGetStringField(TEXT("data_table_path"), DataTablePath) || DataTablePath.IsEmpty())
    {
        DataTablePath = FString::Printf(TEXT("/Game/UEAgent/AttributeMeta/%s_Meta"), *AttributeSetClass->GetName());
    }
    // Normalize "/Game/.../Name" -> "/Game/.../Name.Name" only if it does not already
    // contain the object-name suffix; we use the package path throughout.
    FString PackagePath;
    FString AssetBaseName;
    {
        const FString Base = FPaths::GetBaseFilename(DataTablePath);
        const FString Path = FPaths::GetPath(DataTablePath);
        if (Base.IsEmpty() || Path.IsEmpty())
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Invalid data_table_path: %s"), *DataTablePath));
        }
        PackagePath = Path;
        AssetBaseName = Base;
    }

    const TArray<TSharedPtr<FJsonValue>>* RowsArr = nullptr;
    if (!Params->TryGetArrayField(TEXT("metadata_rows"), RowsArr) || !RowsArr || RowsArr->Num() == 0)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Missing required parameter: metadata_rows (non-empty array)"));
    }

    // Resolve or create the DataTable. Re-running the command is idempotent: the
    // existing DataTable is reused with its existing rows; per-attribute_name upserts
    // add/update without duplicating.
    const FString DataTableObjectPath = FString::Printf(TEXT("%s/%s.%s"), *PackagePath, *AssetBaseName, *AssetBaseName);
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(DataTableObjectPath)) { return Invalid; }
    UDataTable* DataTable = GameplayAbilities_LoadDataTableAt(DataTableObjectPath);
    bool bCreated = false;
    if (!DataTable)
    {
        FString CreateError;
        DataTable = CreateAttributeMetaDataTable(FString::Printf(TEXT("%s/%s"), *PackagePath, *AssetBaseName), CreateError);
        if (!DataTable)
        {
            return mcp_common::MakeErrorReply(CreateError.IsEmpty()
                ? FString::Printf(TEXT("Failed to create DataTable at %s/%s"), *PackagePath, *AssetBaseName)
                : CreateError);
        }
        bCreated = true;
    }
    if (DataTable->RowStruct != FAttributeMetaData::StaticStruct())
    {
        // Honest refuse: do NOT silently clobber a DataTable that was authored for a
        // different row struct.
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("DataTable %s/%s has row struct '%s' but expected FAttributeMetaData"),
            *PackagePath, *AssetBaseName,
            DataTable->RowStruct ? *DataTable->RowStruct->GetName() : TEXT("None")));
    }

    DataTable->Modify();
    FDataTableEditorUtils::BroadcastPreChange(DataTable, FDataTableEditorUtils::EDataTableChangeInfo::RowData);

    // Upsert each requested row by attribute_name. We use FStructOnScope + JsonValueToUProperty
    // per row (the same approach WriteDataTableRow in DataAssetCommands.cpp uses), so the
    // row seed is copied from the current row when updating (preserves columns not in the
    // payload) and a fresh default row is allocated when adding.
    UScriptStruct* RowStruct = FAttributeMetaData::StaticStruct();
    int32 RowsImported = 0;
    TArray<FString> Problems;
    for (const TSharedPtr<FJsonValue>& RowVal : *RowsArr)
    {
        if (!RowVal.IsValid() || RowVal->Type != EJson::Object)
        {
            Problems.Add(FString(TEXT("Non-object entry in metadata_rows")));
            continue;
        }
        const TSharedPtr<FJsonObject>* RowObj = nullptr;
        if (!RowVal->TryGetObject(RowObj) || !RowObj || !(*RowObj).IsValid())
        {
            Problems.Add(FString(TEXT("Invalid row object in metadata_rows")));
            continue;
        }

        FString AttributeName;
        if (!(*RowObj)->TryGetStringField(TEXT("attribute_name"), AttributeName) || AttributeName.IsEmpty())
        {
            Problems.Add(FString(TEXT("Row missing required field: attribute_name")));
            continue;
        }
        const FName RowFName(*AttributeName);
        const bool bExisted = (DataTable->FindRowUnchecked(RowFName) != nullptr);

        FStructOnScope RowScope(RowStruct);
        uint8* RowMemory = RowScope.GetStructMemory();
        if (!RowMemory)
        {
            Problems.Add(FString::Printf(TEXT("Failed to allocate row memory for '%s'"), *AttributeName));
            continue;
        }

        // If updating, seed the new row memory from the current row so columns not
        // present in the JSON are preserved (idempotent partial updates).
        if (bExisted)
        {
            const uint8* Current = DataTable->FindRowUnchecked(RowFName);
            if (Current)
            {
                RowStruct->CopyScriptStruct(RowMemory, Current);
            }
        }

        // Import every JSON field into the row memory via reflection.
        bool bAnyFailure = false;
        for (const auto& FieldPair : (*RowObj)->Values)
        {
            const FString FieldName = *FieldPair.Key;
            // attribute_name is the row key, not a struct column.
            if (FieldName.Equals(TEXT("attribute_name"), ESearchCase::IgnoreCase))
            {
                continue;
            }
            // Map the documented snake_case column names to the row struct's UPROPERTY names.
            // FAttributeMetaData is PascalCase and two columns don't follow a mechanical
            // snake->Pascal rule (bCanStack, DerivedAttributeInfo), so use an explicit alias
            // map; falls back to the raw name for any other row struct.
            FString StructFieldName = FieldName;
            {
                static const TMap<FString, FString> MetaColumnAliases = {
                    { TEXT("base_value"),   TEXT("BaseValue") },
                    { TEXT("min_value"),    TEXT("MinValue") },
                    { TEXT("max_value"),    TEXT("MaxValue") },
                    { TEXT("can_stack"),    TEXT("bCanStack") },
                    { TEXT("derived_info"), TEXT("DerivedAttributeInfo") },
                };
                if (const FString* Alias = MetaColumnAliases.Find(FieldName.ToLower()))
                {
                    StructFieldName = *Alias;
                }
            }
            FProperty* Property = RowStruct->FindPropertyByName(FName(*StructFieldName));
            if (!Property)
            {
                Problems.Add(FString::Printf(TEXT("Row '%s' unknown column '%s'"), *AttributeName, *FieldName));
                continue;
            }
            void* ValuePtr = Property->ContainerPtrToValuePtr<void>(RowMemory);
            FText FailReason;
            const bool bOk = FJsonObjectConverter::JsonValueToUProperty(
                FieldPair.Value, Property, ValuePtr, /*CheckFlags*/ 0, /*SkipFlags*/ 0, /*bStrictMode*/ false, &FailReason);
            if (!bOk)
            {
                bAnyFailure = true;
                const FString Reason = FailReason.IsEmpty() ? FString() : (TEXT(": ") + FailReason.ToString());
                Problems.Add(FString::Printf(TEXT("Row '%s' column '%s' failed%s"), *AttributeName, *FieldName, *Reason));
            }
        }

        if (bAnyFailure)
        {
            continue;
        }

        // UDataTable::AddRow removes the existing row (if any) and copies in the new memory.
        DataTable->AddRow(RowFName, RowMemory, RowStruct);
        ++RowsImported;
    }

    FDataTableEditorUtils::BroadcastPostChange(DataTable, FDataTableEditorUtils::EDataTableChangeInfo::RowData);
    DataTable->MarkPackageDirty();

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(DataTable->GetOutermost(), DataTable, SaveError);

    // Validate by calling InitFromMetaDataTable on the CDO.
    // UAttributeSet::InitFromMetaDataTable is virtual; the base implementation walks
    // every FProperty of every UScriptStruct in this AttributeSet and looks up rows by
    // Attribute name. Per the engine header, some custom AttributeSets that override
    // the function may choose to do nothing; we report honestly (the call returns void).
    bool bInitSucceeded = true;
    if (UAttributeSet* CDO = Cast<UAttributeSet>(AttributeSetClass->GetDefaultObject()))
    {
        CDO->InitFromMetaDataTable(DataTable);
    }
    else
    {
        bInitSucceeded = false;
        Problems.Add(FString(TEXT("AttributeSet CDO is not a UAttributeSet instance")));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("attribute_set_class"), AttributeSetClass->GetPathName());
    ResultObj->SetStringField(TEXT("attribute_set_class_name"), AttributeSetClass->GetName());
    ResultObj->SetStringField(TEXT("data_table_path"), DataTable->GetPathName());
    ResultObj->SetNumberField(TEXT("row_count"), DataTable->GetRowMap().Num());
    ResultObj->SetNumberField(TEXT("rows_imported"), RowsImported);
    ResultObj->SetStringField(TEXT("row_struct"), RowStruct->GetName());
    ResultObj->SetBoolField(TEXT("created"), bCreated);
    ResultObj->SetBoolField(TEXT("init_from_meta_succeeded"), bInitSucceeded);
    ResultObj->SetBoolField(TEXT("saved"), bSaved);
    ResultObj->SetArrayField(TEXT("problems"), mcp_common::StringsToJson(Problems));

    UE_LOG(LogTemp, Verbose, TEXT("[UEAgent] set_attribute_meta_on_set_class: %s -> %s, rows_imported=%d, saved=%s"),
        *AttributeSetClass->GetName(), *DataTable->GetPathName(), RowsImported, bSaved ? TEXT("true") : TEXT("false"));
    return ResultObj;
}

// MCP:get_attribute_meta_on_set_class - Read back the FAttributeMetaData DataTable an AttributeSet class is initialized from (read-side of set_attribute_meta_on_set_class): row struct name, row count, and the serialized rows. This reports the AUTHORED defaults on disk, not the live values of any actor -- for those use get_attribute_value / get_attributes.
//   @param attribute_set_class_path - Full UAttributeSet class path
//   @param data_table_path - Optional override; default is /Game/UEAgent/AttributeMeta/<ClassName>_Meta
//   @returns { attribute_set_class, data_table_path, row_struct, row_count, rows }
TSharedPtr<FJsonObject> HandleGetAttributeMetaOnSetClass(const TSharedPtr<FJsonObject>& Params)
{
    UE_LOG(LogTemp, Verbose, TEXT("[UEAgent] get_attribute_meta_on_set_class: enter"));

    FString Error;
    UClass* AttributeSetClass = GameplayAbilities_ReadAttributeSetClass(Params, Error);
    if (!AttributeSetClass)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    FString DataTablePath;
    Params->TryGetStringField(TEXT("data_table_path"), DataTablePath);
    if (DataTablePath.IsEmpty())
    {
        DataTablePath = FString::Printf(TEXT("/Game/UEAgent/AttributeMeta/%s_Meta.%s_Meta"),
            *AttributeSetClass->GetName(), *AttributeSetClass->GetName());
    }
    // The default joins a folder to the class name, and the load creates a package of the path it cannot find.
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(DataTablePath)) { return Invalid; }

    UDataTable* DataTable = GameplayAbilities_LoadDataTableAt(DataTablePath);
    if (!DataTable)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("DataTable not found: %s"), *DataTablePath));
    }

    if (!DataTable->RowStruct)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("DataTable %s has no row struct"), *DataTablePath));
    }

    // Use reflection per row: for FAttributeMetaData rows, copy into a local struct
    // and read the well-known columns; for any other row struct, walk the fields via
    // FField and emit generic strings.
    TArray<TSharedPtr<FJsonValue>> RowsJson;
    for (const auto& RowPair : DataTable->GetRowMap())
    {
        const FName RowKey = RowPair.Key;
        const uint8* RowMemory = RowPair.Value;
        if (!RowMemory)
        {
            continue;
        }

        TSharedPtr<FJsonObject> RowObj = MakeShared<FJsonObject>();
        RowObj->SetStringField(TEXT("attribute_name"), RowKey.ToString());

        // Serialize the struct into a JSON object. We copy the row memory into a local
        // FAttributeMetaData (the row memory IS the RowStruct-typed memory from
        // GetRowMap(); the memcpy is well-defined when RowStruct matches, and
        // downstream field reads go through FField reflection so an unrelated struct
        // would just return zeros rather than crashing).
        if (DataTable->RowStruct == FAttributeMetaData::StaticStruct())
        {
            FAttributeMetaData LocalMeta;
            FMemory::Memcpy(&LocalMeta, RowMemory, sizeof(FAttributeMetaData));
            RowObj->SetNumberField(TEXT("base_value"), LocalMeta.BaseValue);
            RowObj->SetNumberField(TEXT("min_value"), LocalMeta.MinValue);
            RowObj->SetNumberField(TEXT("max_value"), LocalMeta.MaxValue);
            RowObj->SetBoolField(TEXT("can_stack"), LocalMeta.bCanStack);
            RowObj->SetStringField(TEXT("derived_info"), LocalMeta.DerivedAttributeInfo);
        }
        else
        {
            // Generic exporter via FProperty when the row struct is something else.
            for (TFieldIterator<FProperty> It(DataTable->RowStruct); It; ++It)
            {
                FProperty* Property = *It;
                if (!Property)
                {
                    continue;
                }
                const FString FieldName = Property->GetName();
                const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(RowMemory);
                FString ValueStr;
                Property->ExportTextItem_Direct(ValueStr, ValuePtr, nullptr, nullptr, PPF_None);
                RowObj->SetStringField(FieldName, ValueStr);
            }
        }

        RowsJson.Add(MakeShared<FJsonValueObject>(RowObj));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("attribute_set_class"), AttributeSetClass->GetPathName());
    ResultObj->SetStringField(TEXT("data_table_path"), DataTable->GetPathName());
    ResultObj->SetStringField(TEXT("row_struct"), DataTable->RowStruct->GetName());
    ResultObj->SetNumberField(TEXT("row_count"), RowsJson.Num());
    ResultObj->SetArrayField(TEXT("rows"), RowsJson);
    UE_LOG(LogTemp, Verbose, TEXT("[UEAgent] get_attribute_meta_on_set_class: %s -> %d rows"),
        *DataTable->GetPathName(), RowsJson.Num());
    return ResultObj;
}

// MCP:send_gameplay_event_to_actor - Send a gameplay event to an editor-world actor's ability system component (UAbilitySystemBlueprintLibrary::SendGameplayEventToActor): it tries to activate every granted ability whose trigger is a GameplayEvent on event_tag or a parent tag, and wakes listeners such as Wait Gameplay Event tasks. Use it for event-driven abilities; to start an ability directly use activate_ability, to change attributes apply_gameplay_effect, and to watch tag count changes register_gameplay_tag_event. Not idempotent: each call fires the event again. Nothing is saved and there is no undo step. sent is measured: a probe listening on the component for exactly event_tag saw the event arrive (the call fails when it did not); asc_resolved says the engine's send path found that same component. Neither says which abilities reacted: check with get_active_abilities (is_active) or get_ability_tasks. An unknown actor, a label several actors share, a missing component or an unregistered tag is an error and sends nothing.
//   @param actor_name - Object name of the editor-world actor, e.g. "BP_Hero_C_1" (whole name, case-insensitive, no wildcards), or else an editor label only one actor has; resolved_by in the reply says which matched. get_actors_in_level lists names (required)
//   @param event_tag - Full name of a registered gameplay tag, e.g. "Gameplay.Event.Hit"; find_gameplay_tags lists them, create_gameplay_tag adds one, and an unregistered tag is an error (required)
//   @param payload_magnitude - Optional number placed in the payload's EventMagnitude, e.g. 25.0; omitted leaves 0. The payload carries only EventTag and EventMagnitude (no instigator, target or object)
//   @returns { actor_name: string, event_tag: string, resolved_by: string ("object_name" | "label"), actor_object_name: string, sent: bool, asc_resolved: bool }
TSharedPtr<FJsonObject> HandleSendGameplayEventToActor(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    FGameplayTag EventTag;
    const UEAgentGameplayAbilitiesImpl::FAbilityOwner Target = UEAgentGameplayAbilitiesImpl::ReadTagEventTarget(Params, ActorName, EventTag);
    if (Target.Error.IsValid())
    {
        return Target.Error;
    }

    FGameplayEventData Payload;
    double Mag = 0.0;
    if (Params->TryGetNumberField(TEXT("payload_magnitude"), Mag))
    {
        Payload.EventMagnitude = static_cast<float>(Mag);
    }
    Payload.EventTag = EventTag;

    // The component the engine's send path resolves for the actor (UAbilitySystemGlobals' lookup); asc_resolved
    // says it is the component this call listens on.
    UAbilitySystemComponent* const SendTarget = UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(Target.Actor);
    const bool bAscResolved = SendTarget != nullptr && SendTarget == Target.Asc;

    // A probe on the component's exact-tag event callbacks for the duration of the send: HandleGameplayEvent
    // broadcasts them when the component receives the event, so the probe firing is the measured delivery.
    // The callbacks may add map entries, so the entry is looked up again before the probe is taken off, and an
    // entry this call created is removed again so the component is left as it was.
    bool bDelivered = false;
    const bool bHadCallbacks = Target.Asc->GenericGameplayEventCallbacks.Contains(EventTag);
    const FDelegateHandle Probe = Target.Asc->GenericGameplayEventCallbacks.FindOrAdd(EventTag).AddLambda(
        [&bDelivered](const FGameplayEventData*) { bDelivered = true; });

    UAbilitySystemBlueprintLibrary::SendGameplayEventToActor(Target.Actor, EventTag, Payload);

    if (FGameplayEventMulticastDelegate* Callbacks = Target.Asc->GenericGameplayEventCallbacks.Find(EventTag))
    {
        Callbacks->Remove(Probe);
        if (!bHadCallbacks && !Callbacks->IsBound())
        {
            Target.Asc->GenericGameplayEventCallbacks.Remove(EventTag);
        }
    }

    TSharedPtr<FJsonObject> Result = UEAgentGameplayAbilitiesImpl::NewActorReply(ActorName, TEXT("event_tag"), EventTag.ToString());
    Result->SetStringField(TEXT("resolved_by"), Target.ResolvedBy);
    Result->SetStringField(TEXT("actor_object_name"), Target.Actor->GetName());
    Result->SetBoolField(TEXT("asc_resolved"), bAscResolved);
    Result->SetBoolField(TEXT("sent"), bDelivered);
    return mcp_common::MakeOutcomeReply(Result, bDelivered, bDelivered ? FString()
        : FString(TEXT("send_gameplay_event_to_actor: the event did not reach the actor's ability system component (asc_resolved says whether the engine's send path found the same component)")));
}

// --- Gameplay tag listeners: register / poll / unregister ------------------------

// Target of the bound tag-count delegate: records one fire in the subscription's ring
// buffer. A fire that arrives after the subscription was torn down is ignored.
void RecordGameplayTagEventFire(int64 SubscriptionId, int32 NewCount)
{
    FScopeLock Lock(&GTagEventMutex);
    FGameplayTagEventSubscription* Sub = GTagEventSubscriptions.Find(SubscriptionId);
    if (!Sub)
    {
        return; // stale callback firing after unregister_gameplay_tag_event already tore it down
    }

    FGameplayTagEventFire Fire;
    Fire.SequenceId = ++Sub->LastEventSequenceId;
    Fire.Timestamp = FPlatformTime::Seconds();
    Fire.NewCount = NewCount;
    Sub->Events.Add(Fire);
    if (Sub->Events.Num() > MaxGameplayTagEventsPerSubscription)
    {
        Sub->Events.RemoveAt(0, Sub->Events.Num() - MaxGameplayTagEventsPerSubscription, EAllowShrinking::No);
    }
}

// MCP:register_gameplay_tag_event - Registers a REAL listener on a target actor's UAbilitySystemComponent for a gameplay tag's count changes (UAbilitySystemComponent::RegisterGameplayTagEvent, AbilitySystemComponent.h), binding an actual callback (.AddLambda) and keeping its FDelegateHandle so unregister_gameplay_tag_event can honestly undo it later. Idempotent: re-registering the same (actor, tag, event_type) returns the existing subscription_id instead of stacking a duplicate listener. Poll fires with poll_gameplay_tag_events.
//   @param actor_name - Object name of the target editor-world actor, or an editor label only one actor has (required)
//   @param event_tag - FGameplayTag name (e.g. "Gameplay.Event.Hit") (required)
//   @param event_type - "NewOrRemoved" | "AnyCountChange" (default "NewOrRemoved")
//   @returns { actor_name: string, event_tag: string, event_type: string, subscription_id: number, registered: bool, already_existed: bool }
TSharedPtr<FJsonObject> HandleRegisterGameplayTagEvent(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    FGameplayTag EventTag;
    const UEAgentGameplayAbilitiesImpl::FAbilityOwner Target = UEAgentGameplayAbilitiesImpl::ReadTagEventTarget(Params, ActorName, EventTag);
    if (Target.Error.IsValid())
    {
        return Target.Error;
    }
    UAbilitySystemComponent* ASC = Target.Asc;

    FString TypeStr = TEXT("NewOrRemoved");
    Params->TryGetStringField(TEXT("event_type"), TypeStr);
    EGameplayTagEventType::Type EventType = EGameplayTagEventType::NewOrRemoved;
    if (TypeStr.Equals(TEXT("AnyCountChange"), ESearchCase::IgnoreCase)) EventType = EGameplayTagEventType::AnyCountChange;

    // Keyed by the actor's object name, so the same actor named by its label or by its object name is one subscription.
    const FString Key = MakeGameplayTagEventKey(Target.Actor->GetName(), EventTag, EventType);

    FScopeLock Lock(&GTagEventMutex);

    if (const int64* ExistingId = GTagEventKeyToId.Find(Key))
    {
        TSharedPtr<FJsonObject> ExistResult = UEAgentGameplayAbilitiesImpl::NewActorReply(ActorName, TEXT("event_tag"), EventTag.ToString());
        ExistResult->SetStringField(TEXT("event_type"), GameplayTagEventTypeToString(EventType));
        ExistResult->SetNumberField(TEXT("subscription_id"), static_cast<double>(*ExistingId));
        ExistResult->SetBoolField(TEXT("registered"), true);
        ExistResult->SetBoolField(TEXT("already_existed"), true);
        return mcp_common::MakeOkReply(ExistResult);
    }

    const int64 SubscriptionId = ++GNextTagEventSubscriptionId;

    // RegisterGameplayTagEvent only FindOrAdd's the map entry and returns a reference to the
    // multicast delegate; nothing fires until .AddLambda binds a real callback here, and Add()
    // itself never invokes it synchronously (unlike RegisterAndCallGameplayTagEvent), so it's
    // safe to do the bind + registry insert under a single lock with no re-entrancy risk.
    FOnGameplayEffectTagCountChanged& Delegate = ASC->RegisterGameplayTagEvent(EventTag, EventType);
    FDelegateHandle Handle = Delegate.AddLambda([SubscriptionId](const FGameplayTag /*FiredTag*/, int32 NewCount)
    {
        RecordGameplayTagEventFire(SubscriptionId, NewCount);
    });

    FGameplayTagEventSubscription& Sub = GTagEventSubscriptions.Add(SubscriptionId);
    Sub.ActorName = Target.Actor->GetName(); // what unregister rebuilds the key from
    Sub.ASC = ASC;
    Sub.Tag = EventTag;
    Sub.EventType = EventType;
    Sub.Handle = Handle;
    GTagEventKeyToId.Add(Key, SubscriptionId);

    TSharedPtr<FJsonObject> Result = UEAgentGameplayAbilitiesImpl::NewActorReply(ActorName, TEXT("event_tag"), EventTag.ToString());
    Result->SetStringField(TEXT("event_type"), GameplayTagEventTypeToString(EventType));
    Result->SetNumberField(TEXT("subscription_id"), static_cast<double>(SubscriptionId));
    Result->SetBoolField(TEXT("registered"), true);
    Result->SetBoolField(TEXT("already_existed"), false);
    return mcp_common::MakeOkReply(Result);
}

// ---------------------------------------------------------------------------
// poll_gameplay_tag_events
// ---------------------------------------------------------------------------
// MCP:poll_gameplay_tag_events - Reads fires recorded for a register_gameplay_tag_event subscription since a cursor (non-destructive cursor read, same idiom as poll_delegate_events). Never destructive: call again with next_cursor to keep draining without missing or repeating a fire.
//   @param subscription_id - subscription_id returned by register_gameplay_tag_event (required)
//   @param cursor - Optional; only fires with sequence_id > cursor are returned. Defaults to 0 (everything retained).
//   @returns { subscription_id: number, subscription_exists: bool, events: [{sequence_id: number, timestamp: number, new_count: number}], next_cursor: number }
TSharedPtr<FJsonObject> HandlePollGameplayTagEvents(const TSharedPtr<FJsonObject>& Params)
{
    double SubscriptionIdRaw = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("subscription_id"), SubscriptionIdRaw)) { return Missing; }
    const int64 SubscriptionId = static_cast<int64>(SubscriptionIdRaw);

    double CursorRaw = 0.0;
    Params->TryGetNumberField(TEXT("cursor"), CursorRaw);
    const int64 Cursor = static_cast<int64>(CursorRaw);

    FScopeLock Lock(&GTagEventMutex);
    FGameplayTagEventSubscription* Sub = GTagEventSubscriptions.Find(SubscriptionId);

    TArray<TSharedPtr<FJsonValue>> EventsArr;
    int64 NextCursor = Cursor;
    if (Sub)
    {
        for (const FGameplayTagEventFire& Fire : Sub->Events)
        {
            if (Fire.SequenceId <= Cursor)
            {
                continue;
            }
            TSharedPtr<FJsonObject> FireObj = MakeShared<FJsonObject>();
            FireObj->SetNumberField(TEXT("sequence_id"), static_cast<double>(Fire.SequenceId));
            FireObj->SetNumberField(TEXT("timestamp"), Fire.Timestamp);
            FireObj->SetNumberField(TEXT("new_count"), Fire.NewCount);
            EventsArr.Add(MakeShared<FJsonValueObject>(FireObj));
            NextCursor = FMath::Max(NextCursor, Fire.SequenceId);
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetNumberField(TEXT("subscription_id"), static_cast<double>(SubscriptionId));
    Result->SetBoolField(TEXT("subscription_exists"), Sub != nullptr);
    Result->SetArrayField(TEXT("events"), EventsArr);
    Result->SetNumberField(TEXT("next_cursor"), static_cast<double>(NextCursor));
    return mcp_common::MakeOkReply(Result);
}

// ---------------------------------------------------------------------------
// unregister_gameplay_tag_event
// ---------------------------------------------------------------------------
// MCP:unregister_gameplay_tag_event - Honestly unbinds a register_gameplay_tag_event subscription (UAbilitySystemComponent::UnregisterGameplayTagEvent, AbilitySystemComponent.h) using the real FDelegateHandle captured at register time, and removes it from the registry. Idempotent: an unknown/already-removed subscription_id is a success with removed:false, not an error.
//   @param subscription_id - subscription_id returned by register_gameplay_tag_event (required)
//   @returns { subscription_id: number, removed: bool, was_registered: bool, engine_unregister_ok: bool }
TSharedPtr<FJsonObject> HandleUnregisterGameplayTagEvent(const TSharedPtr<FJsonObject>& Params)
{
    double SubscriptionIdRaw = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("subscription_id"), SubscriptionIdRaw)) { return Missing; }
    const int64 SubscriptionId = static_cast<int64>(SubscriptionIdRaw);

    FScopeLock Lock(&GTagEventMutex);
    FGameplayTagEventSubscription* Sub = GTagEventSubscriptions.Find(SubscriptionId);
    if (!Sub)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetNumberField(TEXT("subscription_id"), static_cast<double>(SubscriptionId));
        Result->SetBoolField(TEXT("removed"), false);
        Result->SetBoolField(TEXT("was_registered"), false);
        Result->SetBoolField(TEXT("engine_unregister_ok"), false);
        return mcp_common::MakeOkReply(Result);
    }

    // If the ASC/actor is already gone there's nothing left to unbind on the engine side, but
    // our own registry entry is still stale and must be cleaned up regardless (matches the
    // TargetObject.IsValid() guard in UEAgentRuntimeEventsCommands' unsubscribe_delegate).
    bool bEngineUnregisterOk = false;
    if (UAbilitySystemComponent* ASC = Sub->ASC.Get())
    {
        bEngineUnregisterOk = ASC->UnregisterGameplayTagEvent(Sub->Handle, Sub->Tag, Sub->EventType);
    }

    const FString Key = MakeGameplayTagEventKey(Sub->ActorName, Sub->Tag, Sub->EventType);
    GTagEventKeyToId.Remove(Key);
    GTagEventSubscriptions.Remove(SubscriptionId);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetNumberField(TEXT("subscription_id"), static_cast<double>(SubscriptionId));
    Result->SetBoolField(TEXT("removed"), true);
    Result->SetBoolField(TEXT("was_registered"), true);
    Result->SetBoolField(TEXT("engine_unregister_ok"), bEngineUnregisterOk);
    return mcp_common::MakeOkReply(Result);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("activate_ability"), &HandleActivateAbility },
    { TEXT("apply_gameplay_effect"), &HandleApplyGameplayEffect },
    { TEXT("apply_multiple_gameplay_effects"), &HandleApplyMultipleGameplayEffects },
    { TEXT("cancel_ability"), &HandleCancelAbility },
    { TEXT("create_gameplay_effect_asset"), &HandleCreateGameplayEffectContainer },
    { TEXT("get_ability_info"), &HandleGetAbilityInfo },
    { TEXT("get_ability_system_component"), &HandleGetAbilitySystemComponent },
    { TEXT("get_ability_tasks"), &HandleGetAbilityTasks },
    { TEXT("get_active_abilities"), &HandleGetActiveAbilities },
    { TEXT("get_active_effects"), &HandleGetActiveEffects },
    { TEXT("get_attribute_meta_on_set_class"), &HandleGetAttributeMetaOnSetClass },
    { TEXT("get_attribute_value"), &HandleGetAttributeValue },
    { TEXT("get_attributes"), &HandleGetAttributes },
    { TEXT("get_gameplay_effect_info"), &HandleGetGameplayEffectInfo },
    { TEXT("get_gameplay_effect_modifiers"), &HandleGetGameplayEffectModifiers },
    { TEXT("grant_ability"), &HandleGrantAbility },
    { TEXT("poll_gameplay_tag_events"), &HandlePollGameplayTagEvents },
    { TEXT("register_gameplay_tag_event"), &HandleRegisterGameplayTagEvent },
    { TEXT("remove_ability"), &HandleRemoveAbility },
    { TEXT("remove_gameplay_effect"), &HandleRemoveGameplayEffect },
    { TEXT("send_gameplay_event_to_actor"), &HandleSendGameplayEventToActor },
    { TEXT("set_attribute_base_value"), &HandleSetAttributeBaseValue },
    { TEXT("set_attribute_meta_on_set_class"), &HandleSetAttributeMetaOnSetClass },
    { TEXT("unregister_gameplay_tag_event"), &HandleUnregisterGameplayTagEvent },
};
} // namespace UEAgentGameplayAbilitiesCommands
