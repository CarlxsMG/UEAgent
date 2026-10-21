// Copyright (c) 2026 CarlxsMG.

#pragma once

#include "CoreMinimal.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Common/UEAgentGraphUtils.h"

// Node and pin lookups of the Blueprint graph and AnimGraph commands. The ones that can fail answer with
// the error reply naming what was not found, or null.
namespace GraphNodePinLookup
{
    // The pin PinName of Node in PreferredDirection, else in any direction; EGPD_MAX looks in any direction.
    inline UEdGraphPin* FindPinPreferring(UEdGraphNode* Node, const FString& PinName, EEdGraphPinDirection PreferredDirection)
    {
        UEdGraphPin* Pin = FUEAgentGraphUtils::FindPinByName(Node, PinName, PreferredDirection);
        if (!Pin && PreferredDirection != EGPD_MAX)
        {
            Pin = FUEAgentGraphUtils::FindPinByName(Node, PinName);
        }
        return Pin;
    }

    // The node whose NodeGuid is NodeId in Graph.
    inline TSharedPtr<FJsonObject> FindNode(UEdGraph* Graph, const FString& NodeId, UEdGraphNode*& OutNode)
    {
        OutNode = FUEAgentGraphUtils::FindNodeByGuid(Graph, NodeId);
        if (!OutNode)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Node not found: %s"), *NodeId));
        }
        return nullptr;
    }

    // The pin PinName of Node (see FindPinPreferring).
    inline TSharedPtr<FJsonObject> FindPin(UEdGraphNode* Node, const FString& PinName, EEdGraphPinDirection PreferredDirection, UEdGraphPin*& OutPin)
    {
        OutPin = FindPinPreferring(Node, PinName, PreferredDirection);
        if (!OutPin)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Pin not found: %s"), *PinName));
        }
        return nullptr;
    }

    // The two pins a connection names: an output pin of the source node and an input pin of the target
    // node are preferred, any pin with the name is the fallback. Both nodes are checked before the pins.
    inline TSharedPtr<FJsonObject> FindConnectionPins(UEdGraph* Graph, const FString& SourceNodeId, const FString& SourcePinName,
                                                      const FString& TargetNodeId, const FString& TargetPinName,
                                                      UEdGraphPin*& OutSourcePin, UEdGraphPin*& OutTargetPin)
    {
        UEdGraphNode* SourceNode = FUEAgentGraphUtils::FindNodeByGuid(Graph, SourceNodeId);
        UEdGraphNode* TargetNode = FUEAgentGraphUtils::FindNodeByGuid(Graph, TargetNodeId);
        if (!SourceNode)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Source node not found: %s"), *SourceNodeId));
        }
        if (!TargetNode)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Target node not found: %s"), *TargetNodeId));
        }

        OutSourcePin = FindPinPreferring(SourceNode, SourcePinName, EGPD_Output);
        OutTargetPin = FindPinPreferring(TargetNode, TargetPinName, EGPD_Input);
        if (!OutSourcePin)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Source pin not found: %s"), *SourcePinName));
        }
        if (!OutTargetPin)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Target pin not found: %s"), *TargetPinName));
        }
        return nullptr;
    }
}
