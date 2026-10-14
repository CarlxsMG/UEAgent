#include "MCPRuntimeEventsTestComponent.h"

void UMCPRuntimeEventsTestComponent::BroadcastTestEvent()
{
    OnTestFired.Broadcast();
}

void UMCPRuntimeEventsTestComponent::BroadcastParamTestEvent(int32 IntValue, const FString& StringValue)
{
    OnParamTestFired.Broadcast(IntValue, StringValue);
}
