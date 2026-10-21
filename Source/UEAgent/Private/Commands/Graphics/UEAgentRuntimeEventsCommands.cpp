// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Graphics/UEAgentDelegateObserver.h"
#include "Commands/Common/McpCommonUtils.h"
#include "UObject/UnrealType.h"
#include "UObject/ScriptDelegates.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Class.h"
#include "UObject/PropertyPortFlags.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Editor.h"
#include "Engine/Blueprint.h"

/**
 * Runtime observation commands: subscribe to any dynamic multicast delegate (0..N parameters) on a live
 * object, poll the events it fires (non-destructive cursor read, including each fire's parameter payload),
 * and unsubscribe. See UEAgentDelegateObserver.h and the per-signature dynamic UFunction generator in this
 * file for how an arbitrary signature is observed safely.
 *
 * The family also introspects the events a level actor's component exposes.
 */
namespace UEAgentRuntimeEventsCommands
{

// MCP-DOMAIN: Reflection

// The delegate thunk below records each fire through this; it is defined after the subscription registry.
void RecordDelegateEvent(int64 SubscriptionId, const TSharedPtr<FJsonObject>& Payload);

namespace
{
    // One fired-delegate marker, including the delegate's own parameters at the time it
    // fired (Payload is an empty JSON object, not null, for a zero-parameter delegate).
    struct FMcpDelegateEvent
    {
        int64 SequenceId = 0;
        double Timestamp = 0.0;
        TSharedPtr<FJsonObject> Payload;
    };

    struct FMcpDelegateSubscription
    {
        FString ObjectPath;
        FString DelegateName;
        TWeakObjectPtr<UObject> TargetObject;
        FMulticastDelegateProperty* Property = nullptr; // FField, never GC'd while the owning UClass is loaded
        void* PropertyValuePtr = nullptr;
        FScriptDelegate BoundDelegate; // kept for the symmetric RemoveDelegate call
        TWeakObjectPtr<UUEAgentDelegateObserver> Observer;
        TArray<FMcpDelegateEvent> Events; // bounded ring buffer, non-destructive cursor read idiom
        int64 LastEventSequenceId = 0;
    };

    FCriticalSection GSubscriptionsMutex;
    TMap<int64, FMcpDelegateSubscription> GSubscriptions;
    TMap<FString, int64> GKeyToSubscriptionId; // "ObjectPath::DelegateName" -> subscription_id (idempotency key)
    int64 GNextSubscriptionId = 0;
    constexpr int32 MaxEventsPerSubscription = 512;

    FString MakeSubscriptionKey(const FString& ObjectPath, const FString& DelegateName)
    {
        return ObjectPath + TEXT("::") + DelegateName;
    }

    // Per-signature dynamic UFunction cache: one UFunction per unique delegate
    // SignatureFunction, shared by every subscription to that same signature. Game-thread
    // only (mutates UUEAgentDelegateObserver::StaticClass()'s Children/FuncMap).
    TMap<UFunction*, UFunction*> GDynamicObserverFunctions;
    int32 GNextDynamicFunctionSuffix = 0;

    // Native thunk shared by EVERY dynamically-generated per-signature UFunction (see
    // GetOrCreateDynamicObserverFunction). Safe generically because each dynamic UFunction
    // is a byte-exact clone of the broadcaster's own SignatureFunction (StaticDuplicateObject
    // preserves the FProperty children/offsets), so Stack.Locals is guaranteed to be laid out
    // exactly like Stack.Node's own FProperty children -- reading them back can never read
    // through the wrong property's layout. Stack.Code is null (no bytecode was ever attached
    // to this function), so P_FINISH is a no-op and there's nothing to step through; we read
    // the parameter frame directly, mirroring how HandleCallFunction already reads
    // out-params in UEAgentReflectionCommands.cpp.
    DEFINE_FUNCTION(McpDynamicDelegateThunk)
    {
        P_FINISH;

        UUEAgentDelegateObserver* Observer = CastChecked<UUEAgentDelegateObserver>(Context);
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        for (TFieldIterator<FProperty> It(Stack.Node); It; ++It)
        {
            FProperty* Prop = *It;
            if (!Prop || !Prop->HasAnyPropertyFlags(CPF_Parm) || Prop->HasAnyPropertyFlags(CPF_ReturnParm))
            {
                continue;
            }
            const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Stack.Locals);
            FString ValueStr;
            Prop->ExportText_Direct(ValueStr, ValuePtr, /*Delta*/ nullptr, /*Parent*/ nullptr, PPF_None, /*ExportRootScope*/ nullptr);
            Payload->SetStringField(Prop->GetName(), ValueStr);
        }

        RecordDelegateEvent(Observer->SubscriptionId, Payload);
    }

    // Returns the shared dynamic UFunction observing `Signature`, creating + caching it the
    // first time this exact signature is seen. Precedent: BlueprintCompilationManager.cpp's
    // CreateDelegateProxyFunctions duplicates a delegate SignatureFunction into a plain
    // UFunction the same way (StaticDuplicateObject + strip FUNC_Delegate|FUNC_MulticastDelegate).
    UFunction* GetOrCreateDynamicObserverFunction(UFunction* Signature)
    {
        if (UFunction** Cached = GDynamicObserverFunctions.Find(Signature))
        {
            return *Cached;
        }

        UClass* ObserverClass = UUEAgentDelegateObserver::StaticClass();
        const FName UniqueName(*FString::Printf(TEXT("McpDyn_%s_%d"), *Signature->GetName(), ++GNextDynamicFunctionSuffix));

        // RF_AllFlags matches the exact precedent (BlueprintCompilationManager.cpp's
        // CreateDelegateProxyFunctions) for this same SignatureFunction->UFunction duplication.
        UObject* Dup = StaticDuplicateObject(Signature, ObserverClass, UniqueName, RF_AllFlags, UFunction::StaticClass());
        UFunction* NewFn = CastChecked<UFunction>(Dup);

        // StaticDuplicateObject always serializes with PPF_Duplicate (DuplicateDataReader.cpp),
        // which makes UStruct::Serialize skip Link() (Class.cpp) -- every duplicated FProperty's
        // Offset_Internal is left at 0 (Property.cpp), so with 2+ parameters they all collide at
        // offset 0. Relink explicitly so each property's offset is recomputed correctly.
        NewFn->StaticLink(/*bRelinkExistingProperties*/ true);

        NewFn->FunctionFlags &= ~(FUNC_Delegate | FUNC_MulticastDelegate);
        NewFn->FunctionFlags |= FUNC_Native;
        NewFn->SetNativeFunc(&McpDynamicDelegateThunk);

        NewFn->Next = ObserverClass->Children;
        ObserverClass->Children = NewFn;
        ObserverClass->AddFunctionToFunctionMap(NewFn, UniqueName);

        GDynamicObserverFunctions.Add(Signature, NewFn);
        return NewFn;
    }
}

// Called by the per-signature dynamic delegate thunk (McpDynamicDelegateThunk, file-local
// to UEAgentRuntimeEventsCommands.cpp). Not part of the MCP command surface; internal
// wiring between the observer UObject and this family's subscription registry (GSubscriptions).
// Payload is the fired delegate's parameters serialized to JSON (empty object for a zero-parameter
// delegate).
void RecordDelegateEvent(int64 SubscriptionId, const TSharedPtr<FJsonObject>& Payload)
{
    FScopeLock Lock(&GSubscriptionsMutex);
    FMcpDelegateSubscription* Sub = GSubscriptions.Find(SubscriptionId);
    if (!Sub)
    {
        return; // stale callback firing after unsubscribe_delegate already tore it down; ignore
    }
    FMcpDelegateEvent Event;
    Event.SequenceId = ++Sub->LastEventSequenceId;
    Event.Timestamp = FPlatformTime::Seconds();
    Event.Payload = Payload.IsValid() ? Payload : MakeShared<FJsonObject>();
    Sub->Events.Add(Event);
    if (Sub->Events.Num() > MaxEventsPerSubscription)
    {
        Sub->Events.RemoveAt(0, Sub->Events.Num() - MaxEventsPerSubscription, EAllowShrinking::No);
    }
}

// Resolves "ObjectPath" the same way UEAgentReflectionCommands::ResolveObjectByName
// does (asset path / live object GetPathName() / bare actor name / dotted
// "Actor.Component" path) -- a small intentional duplication rather than promoting a
// shared utility across two independently-owned files (rule of three).
UObject* ResolveObjectForEvents(const FString& ObjectPath)
{
    UObject* Found = mcp_common::ResolveObjectByPath(ObjectPath);
    if (Found)
    {
        return Found;
    }

    TArray<FString> Segments;
    ObjectPath.ParseIntoArray(Segments, TEXT("."), /*InCullEmpty=*/true);
    if (Segments.Num() == 0)
    {
        return nullptr;
    }

    UObject* Resolved = mcp_common::FindActorByObjectName(mcp_common::GetEditorWorld(), Segments[0]);
    for (int32 i = 1; Resolved && i < Segments.Num(); ++i)
    {
        AActor* Actor = Cast<AActor>(Resolved);
        UObject* Next = nullptr;
        if (Actor)
        {
            TArray<UActorComponent*> Components;
            Actor->GetComponents(Components);
            for (UActorComponent* Comp : Components)
            {
                if (Comp && Comp->GetName() == Segments[i])
                {
                    Next = Comp;
                    break;
                }
            }
        }
        Resolved = Next;
    }
    return Resolved;
}

// MCP:subscribe_to_delegate - Bind a generic observer to any dynamic multicast delegate (0..N parameters) on a live object and start recording every time it fires, including its parameters. Idempotent per (object_path, delegate_name): re-subscribing returns the existing subscription_id.
//   @param object_path - Path or name of the object owning the delegate (asset path, live GetPathName(), bare actor name, or dotted "Actor.Component")
//   @param delegate_name - Name of the FMulticastDelegateProperty (BlueprintAssignable/BlueprintCallable dynamic multicast delegate) on that object's class
//   @returns { subscription_id: int, signature: string, already_existed: bool }

TSharedPtr<FJsonObject> HandleSubscribeToDelegate(const TSharedPtr<FJsonObject>& Params)
{
    FString ObjectPath, DelegateName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("object_path"), ObjectPath)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("delegate_name"), DelegateName)) { return Missing; }

    UObject* Object = ResolveObjectForEvents(ObjectPath);
    if (!Object)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Object not found: %s"), *ObjectPath));
    }

    FMulticastDelegateProperty* Property = FindFProperty<FMulticastDelegateProperty>(Object->GetClass(), FName(*DelegateName));
    if (!Property)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Delegate not found: %s"), *DelegateName));
    }

    // A fixed parameterless observer function
    // would be undefined behavior against a parameterized delegate (ProcessEvent does no
    // signature validation, confirmed against ScriptDelegates.h/ScriptCore.h). Instead of
    // refusing non-zero-parameter delegates, bind to a per-signature dynamic UFunction that is
    // a byte-exact clone of the broadcaster's OWN SignatureFunction (see
    // GetOrCreateDynamicObserverFunction) -- there is no signature to mismatch.
    UFunction* Signature = Property->SignatureFunction;
    if (!Signature)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Delegate '%s' has no SignatureFunction, cannot bind generically"), *DelegateName));
    }

    FScopeLock Lock(&GSubscriptionsMutex);
    const FString Key = MakeSubscriptionKey(ObjectPath, DelegateName);
    if (const int64* ExistingId = GKeyToSubscriptionId.Find(Key))
    {
        FMcpDelegateSubscription* Existing = GSubscriptions.Find(*ExistingId);
        if (Existing && Existing->TargetObject.IsValid())
        {
            TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
            Out->SetNumberField(TEXT("subscription_id"), static_cast<double>(*ExistingId));
            Out->SetStringField(TEXT("signature"), Signature ? Signature->GetName() : FString());
            Out->SetBoolField(TEXT("already_existed"), true);
            return mcp_common::MakeOkReply(Out);
        }
        // Stale entry (target object was destroyed since) -- clean up and re-subscribe below.
        if (Existing) { GSubscriptions.Remove(*ExistingId); }
        GKeyToSubscriptionId.Remove(Key);
    }

    UUEAgentDelegateObserver* Observer = NewObject<UUEAgentDelegateObserver>(GetTransientPackage());
    Observer->AddToRoot(); // no Outer keeps this alive; root it explicitly for the subscription's lifetime
    const int64 SubscriptionId = ++GNextSubscriptionId;
    Observer->SubscriptionId = SubscriptionId;

    void* PropertyValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
    UFunction* DynamicObserverFunction = GetOrCreateDynamicObserverFunction(Signature);
    FScriptDelegate Delegate;
    Delegate.BindUFunction(Observer, DynamicObserverFunction->GetFName());

    FMcpDelegateSubscription Sub;
    Sub.ObjectPath = ObjectPath;
    Sub.DelegateName = DelegateName;
    Sub.TargetObject = Object;
    Sub.Property = Property;
    Sub.PropertyValuePtr = PropertyValuePtr;
    Sub.BoundDelegate = Delegate; // copy retained for the symmetric RemoveDelegate later
    Sub.Observer = Observer;

    Property->AddDelegate(MoveTemp(Delegate), Object, PropertyValuePtr);
    GSubscriptions.Add(SubscriptionId, MoveTemp(Sub));
    GKeyToSubscriptionId.Add(Key, SubscriptionId);

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetNumberField(TEXT("subscription_id"), static_cast<double>(SubscriptionId));
    Out->SetStringField(TEXT("signature"), Signature ? Signature->GetName() : FString());
    Out->SetBoolField(TEXT("already_existed"), false);
    return mcp_common::MakeOkReply(Out);
}

// MCP:poll_delegate_events - Return the fires a subscribe_to_delegate subscription has recorded after cursor, oldest first, each with its sequence_id, a timestamp in seconds (FPlatformTime::Seconds) and the delegate's parameters as payload. It does not wait for new fires and does not consume what it returns, so the same cursor gives the same events again; pass next_cursor back to read only newer ones. Only the latest 512 fires per subscription are kept. subscription_active false means the target object is gone: recorded fires still come back, but no new ones will arrive. An unknown subscription_id is an error. unsubscribe_delegate ends the subscription; poll_gameplay_tag_events is the same reader for register_gameplay_tag_event.
//   @param subscription_id - The subscription_id that subscribe_to_delegate returned, e.g. 3.
//   @param cursor - Optional sequence_id: only fires with a higher one are returned; pass the previous reply's next_cursor. Default 0 returns every fire still kept.
//   @returns { events: [{sequence_id, ts, payload}], next_cursor: int, subscription_active: bool }

TSharedPtr<FJsonObject> HandlePollDelegateEvents(const TSharedPtr<FJsonObject>& Params)
{
    double SubIdRaw = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("subscription_id"), SubIdRaw)) { return Missing; }
    const int64 SubscriptionId = static_cast<int64>(SubIdRaw);

    int64 Cursor = 0;
    {
        double Tmp = 0.0;
        if (Params->TryGetNumberField(TEXT("cursor"), Tmp) && Tmp > 0.0) { Cursor = static_cast<int64>(Tmp); }
    }

    FScopeLock Lock(&GSubscriptionsMutex);
    FMcpDelegateSubscription* Sub = GSubscriptions.Find(SubscriptionId);
    if (!Sub)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Unknown subscription_id: %lld"), SubscriptionId));
    }

    TArray<TSharedPtr<FJsonValue>> EventsArray;
    int64 NextCursor = Cursor;
    for (const FMcpDelegateEvent& Event : Sub->Events)
    {
        if (Event.SequenceId <= Cursor) { continue; }
        TSharedPtr<FJsonObject> EventObj = MakeShared<FJsonObject>();
        EventObj->SetNumberField(TEXT("sequence_id"), static_cast<double>(Event.SequenceId));
        EventObj->SetNumberField(TEXT("ts"), Event.Timestamp);
        EventObj->SetObjectField(TEXT("payload"), Event.Payload.IsValid() ? Event.Payload : MakeShared<FJsonObject>());
        EventsArray.Add(MakeShared<FJsonValueObject>(EventObj));
        NextCursor = Event.SequenceId;
    }

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetArrayField(TEXT("events"), EventsArray);
    Out->SetNumberField(TEXT("next_cursor"), static_cast<double>(NextCursor));
    // Honest signal: the target object may have been destroyed since subscribing; the
    // subscription keeps replaying whatever it already captured, but will never fire again.
    Out->SetBoolField(TEXT("subscription_active"), Sub->TargetObject.IsValid());
    return mcp_common::MakeOkReply(Out);
}

// MCP:unsubscribe_delegate - Remove a subscription: unbinds the observer from the delegate and frees it. Idempotent (removing an unknown/already-removed subscription_id is a no-op success, not an error).
//   @param subscription_id - The id returned by subscribe_to_delegate
//   @returns { removed: bool }

TSharedPtr<FJsonObject> HandleUnsubscribeDelegate(const TSharedPtr<FJsonObject>& Params)
{
    double SubIdRaw = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("subscription_id"), SubIdRaw)) { return Missing; }
    const int64 SubscriptionId = static_cast<int64>(SubIdRaw);

    FScopeLock Lock(&GSubscriptionsMutex);
    FMcpDelegateSubscription* Sub = GSubscriptions.Find(SubscriptionId);
    if (!Sub)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetBoolField(TEXT("removed"), false);
        return mcp_common::MakeOkReply(Out);
    }

    if (Sub->TargetObject.IsValid() && Sub->Property)
    {
        Sub->Property->RemoveDelegate(Sub->BoundDelegate, Sub->TargetObject.Get(), Sub->PropertyValuePtr);
    }
    if (Sub->Observer.IsValid())
    {
        Sub->Observer->RemoveFromRoot();
    }

    GKeyToSubscriptionId.Remove(MakeSubscriptionKey(Sub->ObjectPath, Sub->DelegateName));
    GSubscriptions.Remove(SubscriptionId);

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetBoolField(TEXT("removed"), true);
    return mcp_common::MakeOkReply(Out);
}

// get_component_events' reflection of a component class's multicast delegates, plus its actor/component lookup.
namespace UEAgentRuntimeEventsImpl
{
    /**
     * One multicast delegate property of a component, read from reflection: its name, the class that declares it,
     * its signature function and parameters, its Blueprint flags and whether anything is bound to it on Component
     * right now. The delegate name is what subscribe_to_delegate takes.
     */
    TSharedPtr<FJsonObject> DescribeMulticastDelegate(const FMulticastDelegateProperty* Delegate, UActorComponent* Component)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Delegate->GetName());
        const UClass* Owner = Delegate->GetOwnerClass();
        Entry->SetStringField(TEXT("owner_class"), Owner ? Owner->GetName() : FString());
        Entry->SetBoolField(TEXT("is_inherited"), Owner != Component->GetClass());
        Entry->SetBoolField(TEXT("blueprint_assignable"), Delegate->HasAnyPropertyFlags(CPF_BlueprintAssignable));
        Entry->SetBoolField(TEXT("blueprint_callable"), Delegate->HasAnyPropertyFlags(CPF_BlueprintCallable));

        // The signature function is named <Name>__DelegateSignature; the reply drops the suffix.
        const UFunction* Signature = Delegate->SignatureFunction;
        FString SignatureName = Signature ? Signature->GetName() : FString();
        SignatureName.RemoveFromEnd(TEXT("__DelegateSignature"));
        Entry->SetStringField(TEXT("signature"), SignatureName);

        TArray<TSharedPtr<FJsonValue>> Parameters;
        if (Signature)
        {
            for (TFieldIterator<FProperty> It(Signature); It; ++It)
            {
                const FProperty* Param = *It;
                if (!Param->HasAnyPropertyFlags(CPF_Parm) || Param->HasAnyPropertyFlags(CPF_ReturnParm))
                {
                    continue;
                }
                TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
                ParamObj->SetStringField(TEXT("name"), Param->GetName());
                ParamObj->SetStringField(TEXT("type"), Param->GetCPPType());
                ParamObj->SetBoolField(TEXT("is_out"),
                    Param->HasAnyPropertyFlags(CPF_OutParm) && !Param->HasAnyPropertyFlags(CPF_ConstParm));
                Parameters.Add(MakeShared<FJsonValueObject>(ParamObj));
            }
        }
        Entry->SetArrayField(TEXT("parameters"), Parameters);
        Entry->SetNumberField(TEXT("parameter_count"), Parameters.Num());

        const FMulticastScriptDelegate* Bound = Delegate->GetMulticastDelegate(Delegate->ContainerPtrToValuePtr<void>(Component));
        Entry->SetBoolField(TEXT("is_bound"), Bound != nullptr && Bound->IsBound());
        return Entry;
    }

    /**
     * actor_name -> actor (object name, engine's first world context) -> component_name ->
     * component (object name, among that actor's components). Null on success, else the error
     * reply; the world is checked before the actor, the actor before the component.
     */
    TSharedPtr<FJsonObject> FindNamedComponent(const FString& OwnerName, const FString& ComponentName, UActorComponent*& OutComponent)
    {
        UWorld* World = mcp_common::GetFirstContextWorld();
        if (!World)
        {
            return mcp_common::MakeErrorReply(TEXT("No world found"));
        }

        AActor* Owner = mcp_common::FindActorByObjectName(World, OwnerName);
        if (!Owner)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor not found: %s"), *OwnerName));
        }

        for (UActorComponent* Candidate : Owner->GetComponents())
        {
            if (Candidate && Candidate->GetName() == ComponentName)
            {
                OutComponent = Candidate;
                return nullptr;
            }
        }
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Component not found: %s"), *ComponentName));
    }

    const TCHAR* CreationMethodLabel(EComponentCreationMethod Method)
    {
        switch (Method)
        {
        case EComponentCreationMethod::Native:                   return TEXT("Native");
        case EComponentCreationMethod::SimpleConstructionScript: return TEXT("SimpleConstructionScript");
        case EComponentCreationMethod::UserConstructionScript:   return TEXT("UserConstructionScript");
        case EComponentCreationMethod::Instance:                 return TEXT("Instance");
        default:                                                 return TEXT("Unknown");
        }
    }

    /** The component/class facts get_component_events appends to its payload. */
    void WriteComponentFacts(UActorComponent* Component, const TSharedPtr<FJsonObject>& Out)
    {
        UClass* ComponentClass = Component->GetClass();
        Out->SetStringField(TEXT("component_class"), ComponentClass->GetName());
        Out->SetStringField(TEXT("component_category"), ComponentClass->GetMetaData(TEXT("Category")));
        Out->SetStringField(TEXT("component_tooltip"), ComponentClass->GetMetaData(TEXT("Tooltip")));
        Out->SetStringField(TEXT("component_display_name"), ComponentClass->GetMetaData(TEXT("DisplayName")));
        Out->SetBoolField(TEXT("component_is_active"), Component->IsActive());
        Out->SetBoolField(TEXT("component_is_registered"), Component->IsRegistered());
        const AActor* Owner = Component->GetOwner();
        Out->SetStringField(TEXT("component_owner"), Owner ? Owner->GetName() : FString(TEXT("None")));
        Out->SetStringField(TEXT("component_creation_method"), CreationMethodLabel(Component->CreationMethod));
    }
}

// Component event introspection.

// MCP:get_component_events - List the events (dynamic multicast delegate properties, such as OnComponentHit or OnComponentBeginOverlap) that one component of an actor in the editor world exposes, read from its class by reflection, inherited ones included: per event its name, declaring class, whether it is inherited, its signature and parameters (name, C++ type, is_out), whether Blueprints can bind it (blueprint_assignable) and whether anything is bound to it on this component now. Also facts about the component: class with its category, tooltip and display-name metadata, active and registered state, owner and creation method. Read-only. A name it lists is what subscribe_to_delegate takes to record fires, and what add_component_bound_event binds in a Blueprint; get_class_properties lists every property of a class. An actor or component not found is an error.
//   @param actor_name - Object name (not the label) of the actor in the editor world, compared without case, e.g. 'StaticMeshActor_0'.
//   @param component_name - Object name of one of that actor's components, compared without case, e.g. 'StaticMeshComponent0'; get_actor_components lists them.
//   @returns { actor_name: string, component_name: string, component_class: string, component_category: string, component_tooltip: string, component_display_name: string, events: [{name, owner_class, is_inherited, blueprint_assignable, blueprint_callable, signature, parameters: [{name, type, is_out}], parameter_count, is_bound}], events_count: int, declared_events_count: int, component_is_active: bool, component_is_registered: bool, component_owner: string, component_creation_method: string }

TSharedPtr<FJsonObject> HandleGetComponentEvents(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentRuntimeEventsImpl;

    FString OwnerName, TargetComponent;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), OwnerName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("component_name"), TargetComponent, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UActorComponent* Component = nullptr;
    if (TSharedPtr<FJsonObject> Error = Impl::FindNamedComponent(OwnerName, TargetComponent, Component)) { return Error; }

    // Every multicast delegate property the component's class has, its own and inherited ones, in reflection
    // order (the class's own first, then each super class's) -- read from the class, never a fixed list.
    TArray<TSharedPtr<FJsonValue>> Listed;
    int32 DeclaredCount = 0;
    for (TFieldIterator<FMulticastDelegateProperty> It(Component->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
    {
        TSharedPtr<FJsonObject> Entry = Impl::DescribeMulticastDelegate(*It, Component);
        DeclaredCount += Entry->GetBoolField(TEXT("is_inherited")) ? 0 : 1;
        Listed.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actor_name"), OwnerName);
    Payload->SetStringField(TEXT("component_name"), TargetComponent);
    Payload->SetNumberField(TEXT("events_count"), Listed.Num());
    Payload->SetNumberField(TEXT("declared_events_count"), DeclaredCount);
    Payload->SetArrayField(TEXT("events"), Listed);
    Impl::WriteComponentFacts(Component, Payload);
    return Payload;
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("get_component_events"), &HandleGetComponentEvents },
    { TEXT("poll_delegate_events"), &HandlePollDelegateEvents },
    { TEXT("subscribe_to_delegate"), &HandleSubscribeToDelegate },
    { TEXT("unsubscribe_delegate"), &HandleUnsubscribeDelegate },
};
} // namespace UEAgentRuntimeEventsCommands
