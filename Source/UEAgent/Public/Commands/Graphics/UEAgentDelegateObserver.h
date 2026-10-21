// Copyright (c) 2026 CarlxsMG.

#pragma once

#include "CoreMinimal.h"
#include "UEAgentDelegateObserver.generated.h"

/**
 * INTEROP Cluster C (subscribe_to_delegate) — a generic, reflectively-bindable observer for
 * ANY dynamic multicast delegate (0..N parameters). BindUFunction/ProcessEvent do no
 * compile-time (or runtime) signature validation (confirmed against ScriptDelegates.h/
 * ScriptCore.h): a mismatched observer function would read the broadcaster's parameter
 * buffer through the wrong FProperty layout, which is undefined behavior. Safety instead
 * comes from never having a fixed-signature observer function at all: each unique delegate
 * signature gets its own UFunction, byte-exact-duplicated from the broadcaster's own
 * SignatureFunction (StaticDuplicateObject), cached on this class's shared UClass and reused
 * by every subscription to that same signature. See McpDynamicDelegateThunk and
 * GetOrCreateDynamicObserverFunction in UEAgentRuntimeEventsCommands.cpp.
 */
UCLASS()
class UEAGENT_API UUEAgentDelegateObserver : public UObject
{
    GENERATED_BODY()

public:
    // Which subscription (UEAgentRuntimeEventsCommands' registry) this instance
    // observes. Set right after NewObject, before binding.
    int64 SubscriptionId = 0;
};
