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

// MCP:poll_delegate_events - Read the events a subscription has recorded so far, including each fire's parameters (non-destructive cursor read).
//   @param subscription_id - The id returned by subscribe_to_delegate
//   @param cursor - Optional int64; only events with a higher sequence_id are returned (default 0 = everything currently retained)
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

// Shared by get_component_events and add_component_event: the fixed catalogue of component
// events both commands talk about, plus the actor/component lookup they share.
namespace UEAgentRuntimeEventsImpl
{
    /** Which components get_component_events lists an event for. */
    enum class EEventListing : uint8
    {
        AnyComponent,
        SceneComponentsOnly,
        StaticMeshComponentsOnly,
        NotListed // only accepted (and described) by add_component_event
    };

    /** One event name add_component_event accepts. Category doubles as the listed "type". */
    struct FKnownComponentEvent
    {
        const TCHAR* Name;
        const TCHAR* Category;
        const TCHAR* Description;
        const TCHAR* Signature;
        const TCHAR* DelegateProperty; // multicast delegate looked up on the class, or nullptr
        EEventListing Listing;
    };

    // Order matters twice: it is the order get_component_events lists the entries in, and the
    // order of the "Valid events" list in add_component_event's rejection message.
    const FKnownComponentEvent KnownComponentEvents[] =
    {
        { TEXT("BeginPlay"), TEXT("Lifecycle"), TEXT("Called when the component begins play"),
          TEXT("UFUNCTION(BlueprintImplementableEvent) void BeginPlay()"),
          nullptr, EEventListing::AnyComponent },
        { TEXT("EndPlay"), TEXT("Lifecycle"), TEXT("Called when the component ends play"),
          TEXT("UFUNCTION(BlueprintImplementableEvent) void EndPlay(EEndPlayReason::Type EndPlayReason)"),
          nullptr, EEventListing::AnyComponent },
        { TEXT("Tick"), TEXT("Update"), TEXT("Called every frame"),
          TEXT("UFUNCTION(BlueprintImplementableEvent) void Tick(float DeltaTime)"),
          nullptr, EEventListing::AnyComponent },
        { TEXT("Activate"), TEXT("Activation"), TEXT("Called when the component is activated"),
          TEXT("UFUNCTION(BlueprintImplementableEvent) void Activate()"),
          TEXT("OnComponentActivated"), EEventListing::AnyComponent },
        { TEXT("Deactivate"), TEXT("Activation"), TEXT("Called when the component is deactivated"),
          TEXT("UFUNCTION(BlueprintImplementableEvent) void Deactivate()"),
          TEXT("OnComponentDeactivated"), EEventListing::AnyComponent },
        { TEXT("Register"), TEXT("Registration"), TEXT("Called when the component is registered"),
          TEXT("UFUNCTION(BlueprintImplementableEvent) void Register()"),
          nullptr, EEventListing::AnyComponent },
        { TEXT("Unregister"), TEXT("Registration"), TEXT("Called when the component is unregistered"),
          TEXT("UFUNCTION(BlueprintImplementableEvent) void Unregister()"),
          nullptr, EEventListing::AnyComponent },
        { TEXT("TransformUpdated"), TEXT("Transform"), TEXT("Called when the component's transform is updated"),
          TEXT("UFUNCTION(BlueprintImplementableEvent) void TransformUpdated()"),
          nullptr, EEventListing::SceneComponentsOnly },
        { TEXT("OnClicked"), TEXT("Input"), TEXT("Called when the component is clicked"),
          TEXT("UFUNCTION(BlueprintImplementableEvent) void OnClicked(UPrimitiveComponent* ClickedComp, FKey ButtonPressed)"),
          TEXT("OnClicked"), EEventListing::StaticMeshComponentsOnly },
        { TEXT("OnBeginCursorOver"), TEXT("Input"), TEXT("Called when the cursor begins hovering over the component"),
          TEXT("UFUNCTION(BlueprintImplementableEvent) void OnBeginCursorOver(UPrimitiveComponent* TouchedComponent)"),
          TEXT("OnBeginCursorOver"), EEventListing::NotListed },
        { TEXT("OnEndCursorOver"), TEXT("Input"), TEXT("Called when the cursor stops hovering over the component"),
          TEXT("UFUNCTION(BlueprintImplementableEvent) void OnEndCursorOver(UPrimitiveComponent* TouchedComponent)"),
          TEXT("OnEndCursorOver"), EEventListing::NotListed },
        { TEXT("OnInputTouchBegin"), TEXT("Input"), TEXT("Called when touch input begins on the component"),
          TEXT("UFUNCTION(BlueprintImplementableEvent) void OnInputTouchBegin(ETouchIndex::Type FingerIndex, UPrimitiveComponent* TouchedComponent)"),
          TEXT("OnInputTouchBegin"), EEventListing::NotListed },
        { TEXT("OnInputTouchEnd"), TEXT("Input"), TEXT("Called when touch input ends on the component"),
          TEXT("UFUNCTION(BlueprintImplementableEvent) void OnInputTouchEnd(ETouchIndex::Type FingerIndex, UPrimitiveComponent* TouchedComponent)"),
          TEXT("OnInputTouchEnd"), EEventListing::NotListed },
        { TEXT("OnInputTouchEnter"), TEXT("Input"), TEXT("Called when touch input enters the component"),
          TEXT("UFUNCTION(BlueprintImplementableEvent) void OnInputTouchEnter(ETouchIndex::Type FingerIndex, UPrimitiveComponent* TouchedComponent)"),
          TEXT("OnInputTouchEnter"), EEventListing::NotListed },
        { TEXT("OnInputTouchLeave"), TEXT("Input"), TEXT("Called when touch input leaves the component"),
          TEXT("UFUNCTION(BlueprintImplementableEvent) void OnInputTouchLeave(ETouchIndex::Type FingerIndex, UPrimitiveComponent* TouchedComponent)"),
          TEXT("OnInputTouchLeave"), EEventListing::NotListed },
    };

    /** Catalogue entry for a requested name. FString equality, so the match ignores case. */
    const FKnownComponentEvent* FindKnownEvent(const FString& RequestedName)
    {
        for (const FKnownComponentEvent& Known : KnownComponentEvents)
        {
            if (RequestedName.Equals(Known.Name, ESearchCase::IgnoreCase))
            {
                return &Known;
            }
        }
        return nullptr;
    }

    /** "BeginPlay, EndPlay, ..." in catalogue order. */
    FString KnownEventNameList()
    {
        FString Joined;
        for (const FKnownComponentEvent& Known : KnownComponentEvents)
        {
            if (!Joined.IsEmpty())
            {
                Joined += TEXT(", ");
            }
            Joined += Known.Name;
        }
        return Joined;
    }

    /** Whether get_component_events shows this entry for a component of the given kind. */
    bool IsListedFor(const FKnownComponentEvent& Known, bool bSceneComponent, bool bStaticMeshComponent)
    {
        switch (Known.Listing)
        {
        case EEventListing::AnyComponent:             return true;
        case EEventListing::SceneComponentsOnly:      return bSceneComponent;
        case EEventListing::StaticMeshComponentsOnly: return bStaticMeshComponent;
        default:                                      return false;
        }
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
        default:                                                 return TEXT("Unknown");
        }
    }

    /** The component/class facts both commands append to their payload. */
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

// MCP:get_component_events - Retrieve lifecycle, activation, registration, transform, and input events available on a component.
//   @param actor_name - Name of the actor containing the component
//   @param component_name - Name of the component to get events from
//   @returns { actor_name: string, component_name: string, component_class: string, component_category: string, component_tooltip: string, component_display_name: string, events: [{name, type, description, category, is_overrideable, is_blueprint_event, function_signature}], events_count: int, component_is_active: bool, component_is_registered: bool, component_owner: string, component_creation_method: string }

TSharedPtr<FJsonObject> HandleGetComponentEvents(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentRuntimeEventsImpl;

    FString OwnerName, TargetComponent;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), OwnerName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("component_name"), TargetComponent, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UActorComponent* Component = nullptr;
    if (TSharedPtr<FJsonObject> Error = Impl::FindNamedComponent(OwnerName, TargetComponent, Component)) { return Error; }

    // The catalogue entries that apply to this kind of component, in catalogue order. Every
    // listed entry is reported as an overridable Blueprint event.
    const bool bSceneComponent = Component->IsA<USceneComponent>();
    const bool bStaticMeshComponent = Component->IsA<UStaticMeshComponent>();
    TArray<TSharedPtr<FJsonValue>> Listed;
    for (const Impl::FKnownComponentEvent& Known : Impl::KnownComponentEvents)
    {
        if (!Impl::IsListedFor(Known, bSceneComponent, bStaticMeshComponent))
        {
            continue;
        }
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Known.Name);
        Entry->SetStringField(TEXT("type"), Known.Category);
        Entry->SetStringField(TEXT("category"), Known.Category);
        Entry->SetStringField(TEXT("description"), Known.Description);
        Entry->SetStringField(TEXT("function_signature"), Known.Signature);
        Entry->SetBoolField(TEXT("is_overrideable"), true);
        Entry->SetBoolField(TEXT("is_blueprint_event"), true);
        Listed.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actor_name"), OwnerName);
    Payload->SetStringField(TEXT("component_name"), TargetComponent);
    Payload->SetNumberField(TEXT("events_count"), Listed.Num());
    Payload->SetArrayField(TEXT("events"), Listed);
    Impl::WriteComponentFacts(Component, Payload);
    return Payload;
}

// MCP:add_component_event - Checks an event of a level actor's component; it never creates a binding (success is always false). A binding made on a live level-actor component would not persist: a handler belongs in a Blueprint's event graph. When the actor comes from a Blueprint and the event is a bindable delegate, the reply carries suggested_call: the add_component_bound_event call that authors it, with its parameters filled in.
//   @param actor_name - Name of the actor containing the component
//   @param component_name - Name of the component
//   @param event_name - Name of the event to add (must be one of: BeginPlay, EndPlay, Tick, Activate, Deactivate, Register, Unregister, TransformUpdated, OnClicked, OnBeginCursorOver, OnEndCursorOver, OnInputTouchBegin, OnInputTouchEnd, OnInputTouchEnter, OnInputTouchLeave)
//   @returns { actor_name: string, component_name: string, event_name: string, event_category: string, event_description: string, event_function_signature: string, success: bool, binding_created: bool, is_bindable_delegate: bool, delegate_property_name: string, bound_handler_count: int, message: string, error: string, component_class: string, component_category: string, component_tooltip: string, component_display_name: string, component_is_active: bool, component_is_registered: bool, component_owner: string, component_creation_method: string, suggested_call?: { tool: string, params: { blueprint_name, component_name, delegate_name } } }

TSharedPtr<FJsonObject> HandleAddComponentEvent(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentRuntimeEventsImpl;

    FString OwnerName, TargetComponent, RequestedEvent;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), OwnerName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("component_name"), TargetComponent, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("event_name"), RequestedEvent, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    // The component is resolved before the event name is checked, so a bad target is
    // reported ahead of a bad event.
    UActorComponent* Component = nullptr;
    if (TSharedPtr<FJsonObject> Error = Impl::FindNamedComponent(OwnerName, TargetComponent, Component)) { return Error; }

    const Impl::FKnownComponentEvent* Known = Impl::FindKnownEvent(RequestedEvent);
    if (!Known)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Invalid event name: %s. Valid events: %s"),
            *RequestedEvent, *Impl::KnownEventNameList()));
    }

    // Nothing is ever bound here. The command only reports whether the event corresponds to a
    // BlueprintAssignable multicast delegate that this component's class really declares, and
    // how many handlers that delegate holds right now. Override-style events (BeginPlay, Tick,
    // Register...) have no delegate and are reported as not bindable.
    const FString DelegatePropertyName = Known->DelegateProperty ? Known->DelegateProperty : TEXT("");
    bool bBindable = false;
    int32 BoundHandlers = 0;
    if (!DelegatePropertyName.IsEmpty())
    {
        FProperty* Declared = Component->GetClass()->FindPropertyByName(FName(*DelegatePropertyName));
        if (FMulticastDelegateProperty* DelegateProp = CastField<FMulticastDelegateProperty>(Declared))
        {
            bBindable = true;
            const FMulticastScriptDelegate* Invocations =
                DelegateProp->GetMulticastDelegate(DelegateProp->ContainerPtrToValuePtr<void>(Component));
            if (Invocations)
            {
                BoundHandlers = Invocations->GetAllObjects().Num();
            }
        }
    }

    const FString Explanation = bBindable
        ? FString::Printf(
            TEXT("Event '%s' maps to bindable multicast delegate '%s' on %s (currently %d bound handler(s)). No new binding was created: ")
            TEXT("authoring a persistent event handler requires a Blueprint event graph, not a transient level-actor component."),
            *RequestedEvent, *DelegatePropertyName, *Component->GetClass()->GetName(), BoundHandlers)
        : FString::Printf(
            TEXT("Event '%s' is an overridable function (category '%s'), not a bindable delegate, so it cannot be added to a live ")
            TEXT("level-actor component. Override it in a Blueprint/C++ class instead."),
            *RequestedEvent, Known->Category);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actor_name"), OwnerName);
    Payload->SetStringField(TEXT("component_name"), TargetComponent);
    Payload->SetStringField(TEXT("event_name"), RequestedEvent);
    Payload->SetStringField(TEXT("event_category"), Known->Category);
    Payload->SetStringField(TEXT("event_description"), Known->Description);
    Payload->SetStringField(TEXT("event_function_signature"), Known->Signature);
    // Always false: this command never creates a binding (see Explanation).
    Payload->SetBoolField(TEXT("success"), false);
    Payload->SetBoolField(TEXT("binding_created"), false);
    Payload->SetBoolField(TEXT("is_bindable_delegate"), bBindable);
    Payload->SetStringField(TEXT("delegate_property_name"), DelegatePropertyName);
    Payload->SetNumberField(TEXT("bound_handler_count"), BoundHandlers);
    Payload->SetStringField(TEXT("message"), Explanation);
    Payload->SetStringField(TEXT("error"), Explanation);
    Impl::WriteComponentFacts(Component, Payload);

    // The authoring path, ready to call, when there is one: a delegate of a component whose owner
    // is a Blueprint actor.
    const AActor* Owner = Component->GetOwner();
    const UBlueprint* OwnerBlueprint = Owner != nullptr ? Cast<UBlueprint>(Owner->GetClass()->ClassGeneratedBy) : nullptr;
    if (bBindable && OwnerBlueprint != nullptr)
    {
        TSharedPtr<FJsonObject> CallParams = MakeShared<FJsonObject>();
        CallParams->SetStringField(TEXT("blueprint_name"), OwnerBlueprint->GetPathName());
        CallParams->SetStringField(TEXT("component_name"), Component->GetFName().ToString());
        CallParams->SetStringField(TEXT("delegate_name"), DelegatePropertyName);
        TSharedPtr<FJsonObject> Suggested = MakeShared<FJsonObject>();
        Suggested->SetStringField(TEXT("tool"), TEXT("add_component_bound_event"));
        Suggested->SetObjectField(TEXT("params"), CallParams);
        Payload->SetObjectField(TEXT("suggested_call"), Suggested);
    }
    return Payload;
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_component_event"), &HandleAddComponentEvent },
    { TEXT("get_component_events"), &HandleGetComponentEvents },
    { TEXT("poll_delegate_events"), &HandlePollDelegateEvents },
    { TEXT("subscribe_to_delegate"), &HandleSubscribeToDelegate },
    { TEXT("unsubscribe_delegate"), &HandleUnsubscribeDelegate },
};
} // namespace UEAgentRuntimeEventsCommands
