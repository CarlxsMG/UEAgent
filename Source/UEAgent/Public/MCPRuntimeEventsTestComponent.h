#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "MCPRuntimeEventsTestComponent.generated.h"

// Test support: no zero-parameter DECLARE_DYNAMIC_MULTICAST_DELEGATE existed anywhere
// in the plugin to exercise subscribe_to_delegate/poll_delegate_events/
// unsubscribe_delegate end-to-end (every other multicast delegate in the codebase
// carries parameters). This component is reachable through the EXISTING generic
// add_component_to_actor/call_function/remove_component_from_actor commands -- no new
// MCP command surface for it.
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FMcpZeroParamTestDelegate);

// subscribe_to_delegate observes ANY delegate signature (not only 0-param ones) through a
// per-signature dynamic UFunction generator (see GetOrCreateDynamicObserverFunction,
// UEAgentRuntimeEventsCommands.cpp). This 2-param delegate exercises that path
// end-to-end (payload capture of a non-trivial signature: int32 + FString).
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FMcpParamTestDelegate, int32, IntValue, FString, StringValue);

UCLASS(ClassGroup = (MCP), meta = (BlueprintSpawnableComponent))
class UEAGENT_API UMCPRuntimeEventsTestComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UPROPERTY(BlueprintAssignable, Category = "MCP|Test")
    FMcpZeroParamTestDelegate OnTestFired;

    UPROPERTY(BlueprintAssignable, Category = "MCP|Test")
    FMcpParamTestDelegate OnParamTestFired;

    UFUNCTION(BlueprintCallable, Category = "MCP|Test")
    void BroadcastTestEvent();

    UFUNCTION(BlueprintCallable, Category = "MCP|Test")
    void BroadcastParamTestEvent(int32 IntValue, const FString& StringValue);
};
