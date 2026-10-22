// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Common/UEAgentGraphUtils.h"
#include "Commands/Blueprint/GraphNodePinLookup.h"

#include "Editor.h"
#include "JsonObjectConverter.h"
#include "ScopedTransaction.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphUtilities.h"                 // FEdGraphUtilities (UnrealEd): Export/Import nodes as text

#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimInstance.h"
#include "AnimationGraph.h"                    // UAnimationGraph
#include "AnimGraphNode_Base.h"                // UAnimGraphNode_Base::GetFNodeProperty/GetFNodeType

#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"

#include "UObject/UnrealType.h"
#include "UObject/Package.h"

#define LOCTEXT_NAMESPACE "UEAgentAnimGraph"

/**
 * Generic AnimGraph node editing commands.
 *
 * Sibling of UEAgentAnimBlueprintCommands, which owns state-machine and scaffold authoring. add_graph_node and
 * friends only place K2 nodes on the event/function ubergraph -- they cannot place or wire UAnimGraphNode_*
 * nodes inside an AnimGraph (or a state's inner graph). These commands work on the AnimGraph the way the
 * editor's copy/paste, drag and detail panel do, but fully headless:
 *   - copying goes through FEdGraphUtilities::Export/ImportNodesFromText, which preserves each node's
 *     internal FAnimNode settings and the links within the copied set;
 *   - wiring uses the graph's OWN schema: UAnimationGraphSchema understands FPoseLink, the K2 schema used by
 *     connect_pins does not;
 *   - placing a node allocates its default pins BEFORE PostPlacedNewNode, the crash-safe order.
 *
 * Graph resolution goes through FUEAgentGraphUtils::FindGraphByNameDeep, which reaches AnimGraphs and nested
 * state subgraphs (e.g. "MainSM > Idle"), defaulting to the top-level "AnimGraph" when graph_name is omitted.
 */
namespace UEAgentAnimGraphCommands
{

// MCP-DOMAIN: Animation

namespace
{
    /** Load a UAnimBlueprint from an object path, a package path, or a bare asset name. */
    UAnimBlueprint* LoadAnimBP(const FString& PathOrName)
    {
        UAnimBlueprint* Asset = mcp_common::LoadAssetOrSoftPath<UAnimBlueprint>(PathOrName);
        if (!Asset && !PathOrName.IsEmpty())
        {
            // Bare-name fallback: the generic Blueprint finder (by short name) may resolve it.
            Asset = Cast<UAnimBlueprint>(mcp_common::ResolveBlueprint(PathOrName));
        }
        return Asset;
    }

    /**
     * Resolve the target graph inside an AnimBlueprint. An empty (or "AnimGraph") graph_name
     * defaults to the canonical top-level UAnimationGraph named "AnimGraph". Any other name
     * (including nested "MainSM > Idle") delegates to FindGraphByNameDeep, which
     * already reaches AnimGraphs + nested state subgraphs.
     */
    UEdGraph* ResolveAnimGraph(UAnimBlueprint* AnimBP, const FString& GraphName)
    {
        if (!AnimBP)
        {
            return nullptr;
        }
        if (GraphName.IsEmpty() || GraphName.Equals(TEXT("AnimGraph"), ESearchCase::IgnoreCase))
        {
            TArray<UEdGraph*> AllGraphs;
            AnimBP->GetAllGraphs(AllGraphs);
            UAnimationGraph* Fallback = nullptr;
            for (UEdGraph* G : AllGraphs)
            {
                if (UAnimationGraph* AG = Cast<UAnimationGraph>(G))
                {
                    if (AG->GetFName() == FName(TEXT("AnimGraph")))
                    {
                        return AG;
                    }
                    if (!Fallback) { Fallback = AG; }
                }
            }
            if (Fallback) { return Fallback; }
        }
        return FUEAgentGraphUtils::FindGraphByNameDeep(AnimBP, GraphName);
    }

    // The AnimBlueprint AnimBPPath names and its graph_name graph (see ResolveAnimGraph); the error reply of
    // the first that is not found, or null.
    TSharedPtr<FJsonObject> AnimGraph_ResolveTarget(const TSharedPtr<FJsonObject>& Params, const FString& AnimBPPath,
                                                    UAnimBlueprint*& OutAnimBP, UEdGraph*& OutGraph)
    {
        OutAnimBP = LoadAnimBP(AnimBPPath);
        if (!OutAnimBP)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Anim blueprint not found: %s"), *AnimBPPath));
        }
        FString GraphName;
        Params->TryGetStringField(TEXT("graph_name"), GraphName);
        OutGraph = ResolveAnimGraph(OutAnimBP, GraphName);
        if (!OutGraph)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Graph not found: '%s'"), *GraphName));
        }
        return nullptr;
    }

    // Marks AnimBP structurally modified, refreshes Graph and saves AnimBP; whether the save reached disk.
    bool AnimGraph_MarkModifiedAndSave(UAnimBlueprint* AnimBP, UEdGraph* Graph)
    {
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
        Graph->NotifyGraphChanged();
        return mcp_common::SaveAssetPackage(AnimBP->GetOutermost(), AnimBP);
    }
}

// ===========================================================================
// add_anim_graph_node
// ===========================================================================
// MCP:add_anim_graph_node - Place a UAnimGraphNode_* (by class name) into an AnimGraph or a state's inner graph. Saves the ABP but does NOT recompile it -- run compile_anim_blueprint after your edits for runtime effect.
//   @retired add_graph_node - asset_path=<anim blueprint>, graph_name="AnimGraph" (or "<StateMachine> > <State>"), node_type=<AnimGraphNode class, e.g. AnimGraphNode_Slot>, position_x=<x>, position_y=<y>; the Animation Blueprint is not saved (save_asset)
//   @param anim_blueprint_path - Path or name of the UAnimBlueprint.
//   @param node_class - AnimGraphNode class: short name (e.g. "AnimGraphNode_Slot", "AnimGraphNode_PoseSearchHistoryCollector") or full path.
//   @param graph_name - Target graph; omit/"AnimGraph" for the top-level AnimGraph, or a nested name like "MainSM > Idle".
//   @param x - Node X position (optional, default 0).
//   @param y - Node Y position (optional, default 0).
//   @returns { node_id, node_class, graph_name, pins[], saved } (NodeToJson of the created node)
TSharedPtr<FJsonObject> HandleAddAnimGraphNode(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimBPPath, NodeClassName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("anim_blueprint_path"), AnimBPPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("node_class"), NodeClassName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UAnimBlueprint* AnimBP = nullptr;
    UEdGraph* Graph = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimGraph_ResolveTarget(Params, AnimBPPath, AnimBP, Graph)) { return Error; }

    UClass* NodeClass = FUEAgentGraphUtils::ResolveClass(NodeClassName);
    if (!NodeClass)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Could not resolve node class: %s"), *NodeClassName));
    if (!NodeClass->IsChildOf(UEdGraphNode::StaticClass()))
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Class '%s' is not a UEdGraphNode (expected a UAnimGraphNode_* subclass)"), *NodeClass->GetName()));
    if (NodeClass->HasAnyClassFlags(CLASS_Abstract))
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Class '%s' is abstract"), *NodeClass->GetName()));

    double X = 0.0, Y = 0.0;
    Params->TryGetNumberField(TEXT("x"), X);
    Params->TryGetNumberField(TEXT("y"), Y);

    FScopedTransaction Transaction(LOCTEXT("AddAnimGraphNode", "MCP: Add AnimGraph Node"));
    AnimBP->Modify();
    Graph->Modify();

    // Manual creation with the crash-safe ordering: several
    // AnimGraphNode PostPlacedNewNode overrides dereference pins that only exist after
    // AllocateDefaultPins, so FGraphNodeCreator::Finalize()'s PostPlaced-then-Allocate order
    // would null-deref. Allocate pins FIRST, then PostPlacedNewNode.
    UEdGraphNode* NewNode = NewObject<UEdGraphNode>(Graph, NodeClass, NAME_None, RF_Transactional);
    if (!NewNode)
        return mcp_common::MakeErrorReply(TEXT("Failed to construct node object"));
    NewNode->NodePosX = static_cast<int32>(X);
    NewNode->NodePosY = static_cast<int32>(Y);
    Graph->AddNode(NewNode, /*bUserAction=*/false, /*bSelectNewNode=*/false);
    NewNode->CreateNewGuid();
    NewNode->AllocateDefaultPins();
    NewNode->PostPlacedNewNode();

    const bool bSaved = AnimGraph_MarkModifiedAndSave(AnimBP, Graph);

    TSharedPtr<FJsonObject> Result = FUEAgentGraphUtils::NodeToJson(NewNode, /*bIncludePins=*/true);
    Result->SetStringField(TEXT("anim_blueprint_path"), AnimBP->GetPathName());
    Result->SetStringField(TEXT("graph_name"), Graph->GetName());
    Result->SetStringField(TEXT("node_class"), NodeClass->GetName());
    Result->SetBoolField(TEXT("saved"), bSaved);
    return Result;
}

// ===========================================================================
// copy_anim_graph_nodes
// ===========================================================================
// MCP:copy_anim_graph_nodes - Copy a set of AnimGraph nodes (with internal FAnimNode settings + intra-selection wiring) between graphs. Equivalent to the editor's copy/paste: preserves each node's runtime struct properties and any links WITHIN the copied set (links to nodes outside the selection are dropped). Saves the destination ABP but does NOT recompile it -- run compile_anim_blueprint after your edits for runtime effect.
//   @param src_anim_blueprint_path - Source UAnimBlueprint path/name.
//   @param node_ids - Array of source NodeGuid strings (from get_graph_nodes) to copy.
//   @param dst_anim_blueprint_path - Destination UAnimBlueprint path/name.
//   @param src_graph - Source graph name (optional, default the top-level AnimGraph).
//   @param dst_graph - Destination graph name (optional, default the top-level AnimGraph).
//   @param offset_x - X offset added to every pasted node (optional, default 0).
//   @param offset_y - Y offset added to every pasted node (optional, default 0).
//   @returns { copied_count, id_map:{old->new}, imported_nodes[], missing_node_ids[], saved }
TSharedPtr<FJsonObject> HandleCopyAnimGraphNodes(const TSharedPtr<FJsonObject>& Params)
{
    FString SrcPath, DstPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("src_anim_blueprint_path"), SrcPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("dst_anim_blueprint_path"), DstPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    const TArray<TSharedPtr<FJsonValue>>* NodeIdsJson = nullptr;
    if (!Params->TryGetArrayField(TEXT("node_ids"), NodeIdsJson) || NodeIdsJson->Num() == 0)
        return mcp_common::MakeErrorReply(TEXT("Missing/empty required parameter: node_ids (array of NodeGuid strings)"));

    UAnimBlueprint* SrcBP = LoadAnimBP(SrcPath);
    if (!SrcBP)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Source anim blueprint not found: %s"), *SrcPath));
    UAnimBlueprint* DstBP = LoadAnimBP(DstPath);
    if (!DstBP)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Destination anim blueprint not found: %s"), *DstPath));

    FString SrcGraphName, DstGraphName;
    Params->TryGetStringField(TEXT("src_graph"), SrcGraphName);
    Params->TryGetStringField(TEXT("dst_graph"), DstGraphName);
    UEdGraph* SrcGraph = ResolveAnimGraph(SrcBP, SrcGraphName);
    if (!SrcGraph)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Source graph not found: '%s'"), *SrcGraphName));
    UEdGraph* DstGraph = ResolveAnimGraph(DstBP, DstGraphName);
    if (!DstGraph)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Destination graph not found: '%s'"), *DstGraphName));

    double OffsetX = 0.0, OffsetY = 0.0;
    Params->TryGetNumberField(TEXT("offset_x"), OffsetX);
    Params->TryGetNumberField(TEXT("offset_y"), OffsetY);

    // Collect the source nodes.
    TSet<UObject*> NodesToExport;
    TArray<TSharedPtr<FJsonValue>> MissingIds;
    for (const TSharedPtr<FJsonValue>& V : *NodeIdsJson)
    {
        const FString Id = V.IsValid() ? V->AsString() : FString();
        UEdGraphNode* N = FUEAgentGraphUtils::FindNodeByGuid(SrcGraph, Id);
        if (N) { NodesToExport.Add(N); }
        else   { MissingIds.Add(MakeShared<FJsonValueString>(Id)); }
    }
    if (NodesToExport.Num() == 0)
        return mcp_common::MakeErrorReply(TEXT("None of the given node_ids resolved in the source graph"));

    // Serialize the selection to clipboard text, then re-hydrate it in the destination graph.
    FString ExportText;
    FEdGraphUtilities::ExportNodesToText(NodesToExport, ExportText);
    if (!FEdGraphUtilities::CanImportNodesFromText(DstGraph, ExportText))
        return mcp_common::MakeErrorReply(
            TEXT("Destination graph cannot accept these nodes (schema mismatch -- e.g. AnimGraph node into a non-AnimGraph)"));

    FScopedTransaction Transaction(LOCTEXT("CopyAnimGraphNodes", "MCP: Copy AnimGraph Nodes"));
    DstBP->Modify();
    DstGraph->Modify();

    TSet<UEdGraphNode*> Imported;
    FEdGraphUtilities::ImportNodesFromText(DstGraph, ExportText, Imported);

    // ImportNodesFromText preserves the SOURCE NodeGuid on each hydrated node (it does not
    // reassign — see EdGraphUtilities.cpp). So the current GUID == old id; read
    // it, then assign a fresh one so the pasted node is uniquely addressable. That pairing is
    // the old->new map the caller uses to wire boundary pins with connect_anim_graph_pins.
    TSharedPtr<FJsonObject> IdMap = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> ImportedInfo;
    for (UEdGraphNode* N : Imported)
    {
        if (!N) { continue; }
        const FString OldId = N->NodeGuid.ToString();
        N->CreateNewGuid();
        N->NodePosX += static_cast<int32>(OffsetX);
        N->NodePosY += static_cast<int32>(OffsetY);
        N->ReconstructNode();
        IdMap->SetStringField(OldId, N->NodeGuid.ToString());
        ImportedInfo.Add(MakeShared<FJsonValueObject>(FUEAgentGraphUtils::NodeToJson(N, /*bIncludePins=*/false)));
    }

    const bool bSaved = AnimGraph_MarkModifiedAndSave(DstBP, DstGraph);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetNumberField(TEXT("copied_count"), Imported.Num());
    Result->SetObjectField(TEXT("id_map"), IdMap);
    Result->SetArrayField(TEXT("imported_nodes"), ImportedInfo);
    Result->SetArrayField(TEXT("missing_node_ids"), MissingIds);
    Result->SetStringField(TEXT("src_graph"), SrcGraph->GetName());
    Result->SetStringField(TEXT("dst_graph"), DstGraph->GetName());
    Result->SetStringField(TEXT("dst_anim_blueprint_path"), DstBP->GetPathName());
    Result->SetBoolField(TEXT("saved"), bSaved);
    return Result; // raw payload (matches K2 siblings connect_pins/add_graph_node; uniform across the 7)
}

// ===========================================================================
// connect_anim_graph_pins
// ===========================================================================
// MCP:connect_anim_graph_pins - Wire two AnimGraph pins (pose links AND data pins) using the graph's OWN schema. USE THIS INSTEAD OF connect_pins inside an AnimGraph: the generic connect_pins uses the K2 schema, which does not understand FPoseLink/FComponentSpacePoseLink pose pins, whereas this goes through UAnimationGraphSchema::TryCreateConnection so pose links connect correctly. Saves the ABP but does NOT recompile it -- run compile_anim_blueprint after your edits for runtime effect.
//   @retired connect_pins - asset_path=<anim blueprint>, graph_name="AnimGraph", source_node_id=<NodeGuid>, source_pin=<output pin>, target_node_id=<NodeGuid>, target_pin=<input pin> (the graph's own schema checks pose links); not saved
//   @param anim_blueprint_path - Path/name of the UAnimBlueprint.
//   @param source_node_id - NodeGuid of the source node.
//   @param source_pin - Output pin name on the source node.
//   @param target_node_id - NodeGuid of the target node.
//   @param target_pin - Input pin name on the target node.
//   @param graph_name - Target graph (optional, default the top-level AnimGraph).
//   @returns { connected, source_node_id, target_node_id, saved }
TSharedPtr<FJsonObject> HandleConnectAnimGraphPins(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimBPPath, SourceNodeId, SourcePin, TargetNodeId, TargetPin;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("anim_blueprint_path"), AnimBPPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("source_node_id"), SourceNodeId, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("source_pin"), SourcePin, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("target_node_id"), TargetNodeId, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("target_pin"), TargetPin, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UAnimBlueprint* AnimBP = nullptr;
    UEdGraph* Graph = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimGraph_ResolveTarget(Params, AnimBPPath, AnimBP, Graph)) { return Error; }

    UEdGraphPin* PinA = nullptr;
    UEdGraphPin* PinB = nullptr;
    if (TSharedPtr<FJsonObject> Error = GraphNodePinLookup::FindConnectionPins(Graph, SourceNodeId, SourcePin, TargetNodeId, TargetPin, PinA, PinB)) { return Error; }

    const UEdGraphSchema* Schema = Graph->GetSchema();
    if (!Schema)
        return mcp_common::MakeErrorReply(TEXT("Graph has no schema"));

    const FPinConnectionResponse CanConnect = Schema->CanCreateConnection(PinA, PinB);
    if (CanConnect.Response == CONNECT_RESPONSE_DISALLOW)
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Cannot connect pins: %s"), *CanConnect.Message.ToString()));

    FScopedTransaction Transaction(LOCTEXT("ConnectAnimGraphPins", "MCP: Connect AnimGraph Pins"));
    AnimBP->Modify();
    Graph->Modify();
    const bool bConnected = Schema->TryCreateConnection(PinA, PinB);
    if (!bConnected)
        return mcp_common::MakeErrorReply(TEXT("TryCreateConnection failed"));

    const bool bSaved = AnimGraph_MarkModifiedAndSave(AnimBP, Graph);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("connected"), true);
    Result->SetStringField(TEXT("source_node_id"), SourceNodeId);
    Result->SetStringField(TEXT("target_node_id"), TargetNodeId);
    Result->SetStringField(TEXT("graph_name"), Graph->GetName());
    Result->SetBoolField(TEXT("saved"), bSaved);
    return Result; // raw payload (matches K2 siblings connect_pins/add_graph_node; uniform across the 7)
}

// ===========================================================================
// disconnect_anim_graph_pins
// ===========================================================================
// MCP:disconnect_anim_graph_pins - Break AnimGraph pin links: a specific source->target link, or ALL links on the source pin. Use the specific form to insert a node into an existing chain (break A->B, then wire A->N and N->B). Saves the ABP but does NOT recompile it -- run compile_anim_blueprint after your edits for runtime effect.
//   @retired disconnect_pin - asset_path=<anim blueprint>, graph_name="AnimGraph", node_id=<source_node_id>, pin_name=<source_pin>; add linked_node_id=<target_node_id>, linked_pin=<target_pin> to break only that link; not saved
//   @param anim_blueprint_path - Path/name of the UAnimBlueprint.
//   @param source_node_id - NodeGuid of the source node.
//   @param source_pin - Pin name on the source node.
//   @param target_node_id - NodeGuid of the target node (optional; if omitted, breaks ALL links on source_pin).
//   @param target_pin - Pin name on the target node (optional; required if target_node_id is given).
//   @param graph_name - Target graph (optional, default the top-level AnimGraph).
//   @returns { disconnected, broke_all, source_node_id, source_pin, saved }
TSharedPtr<FJsonObject> HandleDisconnectAnimGraphPins(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimBPPath, SourceNodeId, SourcePin;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("anim_blueprint_path"), AnimBPPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("source_node_id"), SourceNodeId, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("source_pin"), SourcePin, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UAnimBlueprint* AnimBP = nullptr;
    UEdGraph* Graph = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimGraph_ResolveTarget(Params, AnimBPPath, AnimBP, Graph)) { return Error; }

    UEdGraphNode* SourceNode = FUEAgentGraphUtils::FindNodeByGuid(Graph, SourceNodeId);
    if (!SourceNode)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Source node not found: %s"), *SourceNodeId));
    UEdGraphPin* PinA = FUEAgentGraphUtils::FindPinByName(SourceNode, SourcePin);
    if (!PinA)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Source pin not found: %s"), *SourcePin));

    // Both target params are OPTIONAL: read them WITHOUT a hard "missing required" guard so the
    // wrapper codegen (which marks any `!TryGet*Field(...) return error` param as required) keeps
    // them optional -- the default mode "break ALL links on source_pin" must stay reachable.
    FString TargetNodeId, TargetPin;
    const bool bHasTarget = Params->TryGetStringField(TEXT("target_node_id"), TargetNodeId);
    Params->TryGetStringField(TEXT("target_pin"), TargetPin);

    FScopedTransaction Transaction(LOCTEXT("DisconnectAnimGraphPins", "MCP: Disconnect AnimGraph Pins"));
    AnimBP->Modify();
    Graph->Modify();

    bool bBrokeAll = false;
    if (bHasTarget)
    {
        if (TargetPin.IsEmpty())
            return mcp_common::MakeErrorReply(TEXT("target_pin is required when target_node_id is given"));
        UEdGraphNode* TargetNode = FUEAgentGraphUtils::FindNodeByGuid(Graph, TargetNodeId);
        if (!TargetNode)
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Target node not found: %s"), *TargetNodeId));
        UEdGraphPin* PinB = FUEAgentGraphUtils::FindPinByName(TargetNode, TargetPin);
        if (!PinB)
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Target pin not found: %s"), *TargetPin));
        PinA->BreakLinkTo(PinB); // BreakLinkTo removes the link on both pins
    }
    else
    {
        PinA->BreakAllPinLinks();
        bBrokeAll = true;
    }

    const bool bSaved = AnimGraph_MarkModifiedAndSave(AnimBP, Graph);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("disconnected"), true);
    Result->SetBoolField(TEXT("broke_all"), bBrokeAll);
    Result->SetStringField(TEXT("source_node_id"), SourceNodeId);
    Result->SetStringField(TEXT("source_pin"), SourcePin);
    Result->SetBoolField(TEXT("saved"), bSaved);
    return Result; // raw payload (matches K2 siblings connect_pins/add_graph_node; uniform across the 7)
}

// ===========================================================================
// read — get_anim_graph_node_properties
// ===========================================================================
// MCP:get_anim_graph_node_properties - Read the internal FAnimNode struct of one AnimGraphNode, found by NodeGuid (node_id) in the AnimGraph or a nested graph_name, as typed JSON: the runtime settings (Pose History PoseCount/CollectorName, Slot SlotName, blend params), not the pin surface get_graph_nodes reports. Use it instead of get_properties, which cannot address a graph node by GUID; a non-anim node returns its EditAnywhere properties. Read-only; write with set_anim_graph_node_property.
//   @retired get_properties - object_path=<the node's object_path from get_graph_nodes with graph_name="AnimGraph">, property_names=["Node"]
//   @param anim_blueprint_path - Path/name of the UAnimBlueprint.
//   @param node_id - NodeGuid of the target AnimGraphNode.
//   @param graph_name - Target graph (optional, default the top-level AnimGraph).
//   @returns { node_id, node_class, node_struct, node_properties:{...} }
TSharedPtr<FJsonObject> HandleGetAnimGraphNodeProperties(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimBPPath, NodeId;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("anim_blueprint_path"), AnimBPPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("node_id"), NodeId, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UAnimBlueprint* AnimBP = nullptr;
    UEdGraph* Graph = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimGraph_ResolveTarget(Params, AnimBPPath, AnimBP, Graph)) { return Error; }

    UEdGraphNode* Node = nullptr;
    if (TSharedPtr<FJsonObject> Error = GraphNodePinLookup::FindNode(Graph, NodeId, Node)) { return Error; }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("node_id"), NodeId);
    Result->SetStringField(TEXT("node_class"), Node->GetClass()->GetName());

    TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
    UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(Node);
    if (AnimNode)
    {
        FStructProperty* NodeProp = AnimNode->GetFNodeProperty();
        UScriptStruct* NodeStruct = AnimNode->GetFNodeType();
        if (NodeProp && NodeStruct)
        {
            const void* NodeMem = NodeProp->ContainerPtrToValuePtr<void>(AnimNode);
            Result->SetStringField(TEXT("node_struct"), NodeStruct->GetName());
            for (TFieldIterator<FProperty> It(NodeStruct); It; ++It)
            {
                FProperty* P = *It;
                const void* ValuePtr = P->ContainerPtrToValuePtr<void>(NodeMem);
                Props->SetField(P->GetName(), mcp_common::PropertyValueToJson(P, ValuePtr));
            }
        }
    }
    else
    {
        // Not an AnimGraphNode (e.g. a K2 node in a transition rule graph): fall back to the
        // node object's own EditAnywhere UPROPERTYs.
        Result->SetStringField(TEXT("node_struct"), TEXT(""));
        for (TFieldIterator<FProperty> It(Node->GetClass()); It; ++It)
        {
            FProperty* P = *It;
            if (!P->HasAnyPropertyFlags(CPF_Edit)) { continue; }
            const void* ValuePtr = P->ContainerPtrToValuePtr<void>(Node);
            Props->SetField(P->GetName(), mcp_common::PropertyValueToJson(P, ValuePtr));
        }
    }
    Result->SetObjectField(TEXT("node_properties"), Props);
    return Result; // raw payload (matches K2 siblings connect_pins/add_graph_node; uniform across the 7)
}

// ===========================================================================
// write — set_anim_graph_node_property
// ===========================================================================
// MCP:set_anim_graph_node_property - Write one field of an AnimGraphNode's internal FAnimNode struct (runtime settings, not pins): node by NodeGuid, field by name or dotted path. set_properties cannot address a graph node by GUID; this also takes a dotted path, rebuilds the node's pins, is undoable and saves the ABP, but does NOT recompile -- run compile_anim_blueprint. Read: get_anim_graph_node_properties.
//   @retired set_properties - object_path=<the node's object_path from get_graph_nodes with graph_name="AnimGraph">, properties={"Node":{"<field>":<value>}}; the node is reconstructed and its Blueprint marked modified (compile_blueprint)
//   @param anim_blueprint_path - Path/name of the UAnimBlueprint.
//   @param node_id - NodeGuid of the target AnimGraphNode.
//   @param property - Property path inside the FAnimNode (e.g. "PoseCount", "SlotName"). A leading "Node." is tolerated. Nested structs use dots ("A.B").
//   @param value - Text value (UE ImportText form: number, "EnumName", "(X=1,Y=2)"...). Use for scalars/enums/simple structs.
//   @param value_json [object] - JSON value applied via JsonValueToUProperty. Use for complex/nested types. Takes precedence over value.
//   @param graph_name - Target graph (optional, default the top-level AnimGraph).
//   @returns { node_id, property, applied, new_value, saved }
TSharedPtr<FJsonObject> HandleSetAnimGraphNodeProperty(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimBPPath, NodeId, PropertyPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("anim_blueprint_path"), AnimBPPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("node_id"), NodeId, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("property"), PropertyPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    // Detect value_json WITHOUT HasField: the codegen's HASFIELD_RE would otherwise type it as a
    // string, but it must be an object/array. Values.Contains keeps it annotation-typed ([object]).
    const bool bHasJson = Params->Values.Contains(TEXT("value_json"));
    FString ValueStr;
    const bool bHasStr = Params->TryGetStringField(TEXT("value"), ValueStr);
    if (!bHasJson && !bHasStr)
        return mcp_common::MakeErrorReply(TEXT("Provide 'value' (text) or 'value_json'"));

    UAnimBlueprint* AnimBP = nullptr;
    UEdGraph* Graph = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimGraph_ResolveTarget(Params, AnimBPPath, AnimBP, Graph)) { return Error; }

    UEdGraphNode* Node = nullptr;
    if (TSharedPtr<FJsonObject> Error = GraphNodePinLookup::FindNode(Graph, NodeId, Node)) { return Error; }
    UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(Node);
    if (!AnimNode)
        return mcp_common::MakeErrorReply(TEXT("Node is not a UAnimGraphNode_* (no FAnimNode struct to write)"));

    FStructProperty* NodeProp = AnimNode->GetFNodeProperty();
    UScriptStruct* NodeStruct = AnimNode->GetFNodeType();
    if (!NodeProp || !NodeStruct)
        return mcp_common::MakeErrorReply(TEXT("AnimGraphNode has no resolvable FAnimNode struct"));

    // Strip a leading "Node." (a common path form) and split by '.'.
    FString Path = PropertyPath;
    if (Path.StartsWith(TEXT("Node.")))
    {
        Path = Path.RightChop(5);
    }
    TArray<FString> Segments;
    Path.ParseIntoArray(Segments, TEXT("."), /*InCullEmpty=*/true);
    if (Segments.Num() == 0)
        return mcp_common::MakeErrorReply(TEXT("Empty property path"));

    // Navigate to the leaf property + its container memory.
    UStruct* CurStruct = NodeStruct;
    void* CurMem = NodeProp->ContainerPtrToValuePtr<void>(AnimNode);
    for (int32 i = 0; i < Segments.Num() - 1; ++i)
    {
        FStructProperty* SP = CastField<FStructProperty>(CurStruct->FindPropertyByName(FName(*Segments[i])));
        if (!SP)
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Path segment '%s' is not a struct property on '%s'"), *Segments[i], *CurStruct->GetName()));
        CurMem = SP->ContainerPtrToValuePtr<void>(CurMem);
        CurStruct = SP->Struct;
    }
    const FString LeafName = Segments.Last();
    FProperty* LeafProp = CurStruct->FindPropertyByName(FName(*LeafName));
    if (!LeafProp)
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Property '%s' not found on struct '%s'"), *LeafName, *CurStruct->GetName()));
    void* LeafPtr = LeafProp->ContainerPtrToValuePtr<void>(CurMem);

    FScopedTransaction Transaction(LOCTEXT("SetAnimGraphNodeProperty", "MCP: Set AnimGraph Node Property"));
    AnimBP->Modify();
    Graph->Modify();
    AnimNode->Modify();

    if (bHasJson)
    {
        const TSharedPtr<FJsonValue> JsonVal = Params->TryGetField(TEXT("value_json"));
        FText FailReason;
        if (!FJsonObjectConverter::JsonValueToUProperty(JsonVal, LeafProp, LeafPtr, 0, 0, false, &FailReason))
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("value_json did not apply to '%s': %s"), *LeafName, *FailReason.ToString()));
    }
    else
    {
        FString ParseError;
        if (!mcp_common::ImportPropertyTextStrict(LeafProp, ValueStr, LeafPtr, AnimNode, ParseError))
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Could not parse value for property '%s': %s"), *LeafName, *ParseError));
    }

    // Property changes can add/remove exposed pins (e.g. Pose History trajectory) -- reconstruct.
    AnimNode->ReconstructNode();
    const bool bSaved = AnimGraph_MarkModifiedAndSave(AnimBP, Graph);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("node_id"), NodeId);
    Result->SetStringField(TEXT("property"), PropertyPath);
    Result->SetBoolField(TEXT("applied"), true);
    Result->SetField(TEXT("new_value"), mcp_common::PropertyValueToJson(LeafProp, LeafPtr));
    Result->SetBoolField(TEXT("saved"), bSaved);
    return Result; // raw payload (matches K2 siblings connect_pins/add_graph_node; uniform across the 7)
}

// ===========================================================================
// set_skeletal_mesh_anim_class
// ===========================================================================
// MCP:set_skeletal_mesh_anim_class - Repoint a Blueprint's skeletal mesh component AnimClass to a (fork) AnimBlueprint. Also handles the INHERITED native Character Mesh, which is not in the SCS and which the generic component-property setters cannot reach: it writes the CDO component archetype so spawned instances inherit the new AnimClass. Also sets AnimationMode=AnimationBlueprint.
//   @param blueprint_name - Character/Actor Blueprint (name or path), e.g. "BP_MyCharacter".
//   @param anim_class - The AnimBlueprint to use: an ABP asset path (resolved to its generated class) or a "..._C" class path/name.
//   @param component_name - Skeletal mesh component to target (optional). Omit to target the Character's inherited Mesh.
//   @returns { blueprint, component, anim_class, previous_anim_class, applied, wrote_cdo_archetype, saved }
TSharedPtr<FJsonObject> HandleSetSkeletalMeshAnimClass(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, AnimClassStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("anim_class"), AnimClassStr, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UBlueprint* BP = mcp_common::ResolveBlueprint(BlueprintName);
    if (!BP || !BP->GeneratedClass)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Blueprint not found (or not compiled): %s"), *BlueprintName));

    // Resolve the anim class: an ABP asset -> its generated class, or a class path/name directly.
    UClass* AnimCls = nullptr;
    if (UObject* Obj = mcp_common::ResolveObjectByPath(AnimClassStr))
    {
        if (UAnimBlueprint* ABP = Cast<UAnimBlueprint>(Obj))
        {
            AnimCls = ABP->GeneratedClass;
        }
    }
    if (!AnimCls)
    {
        AnimCls = FUEAgentGraphUtils::ResolveClass(AnimClassStr);
    }
    if (!AnimCls || !AnimCls->IsChildOf(UAnimInstance::StaticClass()))
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("anim_class '%s' did not resolve to a UAnimInstance subclass"), *AnimClassStr));

    FString ComponentName;
    Params->TryGetStringField(TEXT("component_name"), ComponentName);

    // Find the target skeletal mesh component. Prefer an SCS-added one when a name is given;
    // otherwise (or as fallback) the CDO's inherited Mesh (Character) / first skeletal mesh.
    USkeletalMeshComponent* MeshComp = nullptr;
    bool bWroteCDOArchetype = false;

    if (!ComponentName.IsEmpty() && BP->SimpleConstructionScript)
    {
        for (USCS_Node* N : BP->SimpleConstructionScript->GetAllNodes())
        {
            if (!N) { continue; }
            USkeletalMeshComponent* SMC = Cast<USkeletalMeshComponent>(N->ComponentTemplate);
            if (SMC && N->GetVariableName().ToString().Equals(ComponentName, ESearchCase::IgnoreCase))
            {
                MeshComp = SMC;
                break;
            }
        }
    }

    if (!MeshComp)
    {
        UObject* CDO = BP->GeneratedClass->GetDefaultObject();
        if (AActor* ActorCDO = Cast<AActor>(CDO))
        {
            if (!ComponentName.IsEmpty())
            {
                TArray<USkeletalMeshComponent*> Comps;
                ActorCDO->GetComponents<USkeletalMeshComponent>(Comps);
                for (USkeletalMeshComponent* C : Comps)
                {
                    if (C && (C->GetName().Equals(ComponentName, ESearchCase::IgnoreCase) ||
                              C->GetFName().ToString().Equals(ComponentName, ESearchCase::IgnoreCase)))
                    {
                        MeshComp = C;
                        break;
                    }
                }
            }
            if (!MeshComp)
            {
                if (ACharacter* Char = Cast<ACharacter>(ActorCDO))
                {
                    MeshComp = Char->GetMesh();
                }
                else
                {
                    TArray<USkeletalMeshComponent*> Comps;
                    ActorCDO->GetComponents<USkeletalMeshComponent>(Comps);
                    if (Comps.Num() > 0) { MeshComp = Comps[0]; }
                }
            }
            bWroteCDOArchetype = (MeshComp != nullptr);
        }
    }

    if (!MeshComp)
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Could not find a skeletal mesh component%s on '%s'"),
            ComponentName.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" named '%s'"), *ComponentName), *BlueprintName));

    const FString PrevAnimClass = MeshComp->AnimClass ? MeshComp->AnimClass->GetPathName() : FString(TEXT(""));

    FScopedTransaction Transaction(LOCTEXT("SetSkeletalMeshAnimClass", "MCP: Set Skeletal Mesh Anim Class"));
    BP->Modify();
    MeshComp->Modify();

    // Set via reflection so class + enum marshalling is consistent (and access-agnostic).
    FString Err;
    if (!mcp_common::SetObjectProperty(MeshComp, TEXT("AnimClass"),
            MakeShared<FJsonValueString>(AnimCls->GetPathName()), Err))
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Failed to set AnimClass: %s"), *Err));
    // AnimationMode must be AnimationBlueprint for AnimClass to drive the mesh.
    mcp_common::SetObjectProperty(MeshComp, TEXT("AnimationMode"),
        MakeShared<FJsonValueString>(TEXT("AnimationBlueprint")), Err);

    FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
    if (bWroteCDOArchetype)
    {
        BP->GeneratedClass->GetDefaultObject()->MarkPackageDirty();
    }
    const bool bSaved = mcp_common::SaveAssetPackage(BP->GetOutermost(), BP);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("blueprint"), BP->GetPathName());
    Result->SetStringField(TEXT("component"), MeshComp->GetName());
    Result->SetStringField(TEXT("anim_class"), AnimCls->GetPathName());
    Result->SetStringField(TEXT("previous_anim_class"), PrevAnimClass);
    Result->SetBoolField(TEXT("applied"), MeshComp->AnimClass == AnimCls);
    Result->SetBoolField(TEXT("wrote_cdo_archetype"), bWroteCDOArchetype);
    Result->SetBoolField(TEXT("saved"), bSaved);
    return Result; // raw payload (matches K2 siblings connect_pins/add_graph_node; uniform across the 7)
}

// ===========================================================================
// set/clear_anim_graph_node_pin_binding
// ===========================================================================
namespace
{
    /** Read the ABP path with the primary key and a couple of aliases. */
    FString ReadAnimBPPathAliased(const TSharedPtr<FJsonObject>& Params)
    {
        FString Path;
        if (Params->TryGetStringField(TEXT("anim_blueprint_path"), Path) && !Path.IsEmpty()) { return Path; }
        if (Params->TryGetStringField(TEXT("anim_blueprint"), Path) && !Path.IsEmpty()) { return Path; }
        Params->TryGetStringField(TEXT("blueprint_name"), Path);
        return Path;
    }

    /**
     * Reach the AnimGraph node's per-pin property-binding map. The map is not a direct
     * member of UAnimGraphNode_Base -- it lives on the node's private `Binding` subobject
     * (UAnimGraphNodeBinding_Base::PropertyBindings, a TMap<FName, FAnimGraphNodePropertyBinding>).
     * The concrete binding class' header is Private-only, but the field is a reflected UPROPERTY and the
     * value struct (FAnimGraphNodePropertyBinding) is PUBLIC, so we fetch the subobject via reflection
     * (the node's `Binding` FObjectProperty) and reinterpret the reflected map member as its real C++
     * type (layout-identical; it IS that member). Returns nullptr (with OutError) if unavailable.
     */
    TMap<FName, FAnimGraphNodePropertyBinding>* ResolvePropertyBindingMap(UAnimGraphNode_Base* AnimNode, UObject*& OutBindingObj, FString& OutError)
    {
        OutBindingObj = nullptr;
        FObjectPropertyBase* BindingObjProp = CastField<FObjectPropertyBase>(
            UAnimGraphNode_Base::StaticClass()->FindPropertyByName(TEXT("Binding")));
        if (!BindingObjProp)
        {
            OutError = TEXT("UAnimGraphNode_Base has no reflected 'Binding' property (engine version mismatch)");
            return nullptr;
        }
        UObject* BindingObj = BindingObjProp->GetObjectPropertyValue_InContainer(AnimNode);
        if (!BindingObj)
        {
            OutError = TEXT("This AnimGraphNode has no bindings subobject (node may not support property bindings)");
            return nullptr;
        }
        FMapProperty* MapProp = FindFProperty<FMapProperty>(BindingObj->GetClass(), TEXT("PropertyBindings"));
        if (!MapProp)
        {
            OutError = FString::Printf(TEXT("The node's binding class '%s' does not expose a PropertyBindings map (custom binding type)"),
                *BindingObj->GetClass()->GetName());
            return nullptr;
        }
        OutBindingObj = BindingObj;
        return MapProp->ContainerPtrToValuePtr<TMap<FName, FAnimGraphNodePropertyBinding>>(BindingObj);
    }

    /** Serialize an FAnimGraphNodePropertyBinding to JSON for read-back. */
    TSharedPtr<FJsonObject> PinBindingToJson(const FAnimGraphNodePropertyBinding& B)
    {
        TSharedPtr<FJsonObject> J = MakeShared<FJsonObject>();
        J->SetStringField(TEXT("property_name"), B.PropertyName.ToString());
        J->SetArrayField(TEXT("property_path"), mcp_common::StringsToJson(B.PropertyPath));
        J->SetStringField(TEXT("path_as_text"), B.PathAsText.ToString());
        J->SetBoolField(TEXT("is_bound"), B.bIsBound);
        J->SetBoolField(TEXT("is_promotion"), B.bIsPromotion);
        J->SetNumberField(TEXT("binding_type"), static_cast<int32>(B.Type)); // 0=None,1=Property,2=Function
        J->SetStringField(TEXT("pin_category"), B.PinType.PinCategory.ToString());
        return J;
    }
}

// MCP:set_anim_graph_node_pin_binding - Bind an AnimGraph node input pin to a variable or property path in the node property-binding map (the Details-panel bind chip), e.g. "TransformTrajectory" to "Trajectory"; set_properties on the node's object_path edits the FAnimNode struct (Node) instead. Returns applied and the stored binding; saves the AnimBP without recompiling (run compile_anim_blueprint).
//   @param anim_blueprint_path - Path or name of the UAnimBlueprint (aliases: anim_blueprint, blueprint_name).
//   @param node_id - NodeGuid of the target UAnimGraphNode_* (from get_graph_nodes).
//   @param pin_name - The bound property/pin name (e.g. "TransformTrajectory").
//   @param binding_path - The variable/property path to bind the pin to (e.g. "Trajectory", or nested "MyStruct.Member"). This tool does not check that the path resolves.
//   @param graph_name - Target graph (optional; deep-resolved; default the top-level AnimGraph).
//   @param anim_blueprint - Alias of anim_blueprint_path, used only when anim_blueprint_path is absent or empty.
//   @param blueprint_name - Alias of anim_blueprint_path, used only when anim_blueprint_path and anim_blueprint are both absent or empty.
//   @returns { node_id, pin_name, binding_path, applied, binding:{property_name, property_path[], path_as_text, is_bound, is_promotion, binding_type, pin_category}, saved }
TSharedPtr<FJsonObject> HandleSetAnimGraphNodePinBinding(const TSharedPtr<FJsonObject>& Params)
{
    const FString AnimBPPath = ReadAnimBPPathAliased(Params);
    if (AnimBPPath.IsEmpty())
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: anim_blueprint_path (or anim_blueprint / blueprint_name)"));
    FString NodeId, PinName, BindingPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("node_id"), NodeId, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("pin_name"), PinName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("binding_path"), BindingPath)) { return Missing; }

    UAnimBlueprint* AnimBP = nullptr;
    UEdGraph* Graph = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimGraph_ResolveTarget(Params, AnimBPPath, AnimBP, Graph)) { return Error; }

    UEdGraphNode* Node = nullptr;
    if (TSharedPtr<FJsonObject> Error = GraphNodePinLookup::FindNode(Graph, NodeId, Node)) { return Error; }
    UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(Node);
    if (!AnimNode)
        return mcp_common::MakeErrorReply(TEXT("Node is not a UAnimGraphNode_* (no pin bindings to write)"));

    UObject* BindingObj = nullptr;
    FString ResolveErr;
    TMap<FName, FAnimGraphNodePropertyBinding>* BindMap = ResolvePropertyBindingMap(AnimNode, BindingObj, ResolveErr);
    if (!BindMap)
        return mcp_common::MakeErrorReply(ResolveErr);

    const FName PinFName(*PinName);

    FScopedTransaction Transaction(LOCTEXT("SetAnimGraphNodePinBinding", "MCP: Set AnimGraph Node Pin Binding"));
    AnimBP->Modify();
    Graph->Modify();
    AnimNode->Modify();
    BindingObj->Modify();

    // Build the binding entry. The path is a chain of property names (dot-separated for nested access).
    // PinType/PromotedPinType/bIsPromotion are recomputed automatically when the node reconstructs
    // (UAnimGraphNodeBinding_Base::OnReconstructNode -> RecalculateBindingType resolves PropertyPath
    // against the skeleton class), so we only need to seed the identifying fields here.
    FAnimGraphNodePropertyBinding NewBinding;
    NewBinding.PropertyName = PinFName;
    TArray<FString> PathParts;
    BindingPath.ParseIntoArray(PathParts, TEXT("."), /*InCullEmpty=*/true);
    if (PathParts.Num() == 0) { PathParts.Add(BindingPath); }
    NewBinding.PropertyPath = PathParts;
    NewBinding.PathAsText = FText::FromString(BindingPath);
    NewBinding.bIsBound = true;
    NewBinding.Type = EAnimGraphNodePropertyBindingType::Property;
    // Seed PinType from the bound pin (a valid binding's source type matches the destination pin);
    // ReconstructNode refines it against the resolved leaf property.
    if (UEdGraphPin* BoundPin = GraphNodePinLookup::FindPinPreferring(Node, PinName, EGPD_Input))
    {
        NewBinding.PinType = BoundPin->PinType;
        NewBinding.PromotedPinType = BoundPin->PinType;
    }
    BindMap->Add(PinFName, NewBinding);

    // Reconstruct so the node picks up the binding (and RecalculateBindingType fixes up the type).
    AnimNode->ReconstructNode();
    const bool bSaved = AnimGraph_MarkModifiedAndSave(AnimBP, Graph);

    // Read the resulting state back (re-fetch the map -- the subobject is stable across ReconstructNode).
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("node_id"), NodeId);
    Result->SetStringField(TEXT("pin_name"), PinName);
    Result->SetStringField(TEXT("binding_path"), BindingPath);
    UObject* ReadBindingObj = nullptr;
    FString ReadErr;
    if (TMap<FName, FAnimGraphNodePropertyBinding>* ReadMap = ResolvePropertyBindingMap(AnimNode, ReadBindingObj, ReadErr))
    {
        if (const FAnimGraphNodePropertyBinding* Stored = ReadMap->Find(PinFName))
        {
            Result->SetBoolField(TEXT("applied"), true);
            Result->SetObjectField(TEXT("binding"), PinBindingToJson(*Stored));
        }
        else
        {
            Result->SetBoolField(TEXT("applied"), false);
        }
    }
    Result->SetBoolField(TEXT("saved"), bSaved);
    return Result; // raw payload (matches the other AnimGraph mutators)
}

// MCP:clear_anim_graph_node_pin_binding - Remove the property binding from one input pin of an AnimGraph node (inverse of set_anim_graph_node_pin_binding), dropping a stale binding without recreating the node. Returns was_bound, is_bound and cleared; an unbound pin gives cleared=false, not an error. Undoable; saves the AnimBP without recompiling (run compile_anim_blueprint).
//   @param anim_blueprint_path - Path or name of the UAnimBlueprint (aliases: anim_blueprint, blueprint_name).
//   @param node_id - NodeGuid of the target UAnimGraphNode_* (from get_graph_nodes).
//   @param pin_name - The bound property/pin name whose binding to clear (e.g. "TransformTrajectory").
//   @param graph_name - Target graph (optional; deep-resolved; default the top-level AnimGraph).
//   @param anim_blueprint - Alias of anim_blueprint_path, used only when anim_blueprint_path is absent or empty.
//   @param blueprint_name - Alias of anim_blueprint_path, used only when anim_blueprint_path and anim_blueprint are both absent or empty.
//   @returns { node_id, pin_name, was_bound, is_bound, cleared, saved }
TSharedPtr<FJsonObject> HandleClearAnimGraphNodePinBinding(const TSharedPtr<FJsonObject>& Params)
{
    const FString AnimBPPath = ReadAnimBPPathAliased(Params);
    if (AnimBPPath.IsEmpty())
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: anim_blueprint_path (or anim_blueprint / blueprint_name)"));
    FString NodeId, PinName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("node_id"), NodeId, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("pin_name"), PinName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UAnimBlueprint* AnimBP = nullptr;
    UEdGraph* Graph = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimGraph_ResolveTarget(Params, AnimBPPath, AnimBP, Graph)) { return Error; }

    UEdGraphNode* Node = nullptr;
    if (TSharedPtr<FJsonObject> Error = GraphNodePinLookup::FindNode(Graph, NodeId, Node)) { return Error; }
    UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(Node);
    if (!AnimNode)
        return mcp_common::MakeErrorReply(TEXT("Node is not a UAnimGraphNode_* (no pin bindings to clear)"));

    const FName PinFName(*PinName);
    const bool bWasBound = AnimNode->HasBinding(PinFName);

    FScopedTransaction Transaction(LOCTEXT("ClearAnimGraphNodePinBinding", "MCP: Clear AnimGraph Node Pin Binding"));
    AnimBP->Modify();
    Graph->Modify();
    AnimNode->Modify();
    // Modify the binding subobject via reflection (GetMutableBinding() returns the forward-declared
    // UAnimGraphNodeBinding*, whose definition is not available in this TU for a direct ->Modify()).
    if (FObjectPropertyBase* BindingObjProp = CastField<FObjectPropertyBase>(
            UAnimGraphNode_Base::StaticClass()->FindPropertyByName(TEXT("Binding"))))
    {
        if (UObject* BindingObj = BindingObjProp->GetObjectPropertyValue_InContainer(AnimNode)) { BindingObj->Modify(); }
    }

    // Public, supported removal (handles array-index binding names too).
    AnimNode->RemoveBindings(PinFName);

    AnimNode->ReconstructNode();
    const bool bSaved = AnimGraph_MarkModifiedAndSave(AnimBP, Graph);

    const bool bStillBound = AnimNode->HasBinding(PinFName);
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("node_id"), NodeId);
    Result->SetStringField(TEXT("pin_name"), PinName);
    Result->SetBoolField(TEXT("was_bound"), bWasBound);
    Result->SetBoolField(TEXT("is_bound"), bStillBound);
    Result->SetBoolField(TEXT("cleared"), bWasBound && !bStillBound);
    Result->SetBoolField(TEXT("saved"), bSaved);
    return Result; // raw payload (matches the other AnimGraph mutators)
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_anim_graph_node"), &HandleAddAnimGraphNode },
    { TEXT("clear_anim_graph_node_pin_binding"), &HandleClearAnimGraphNodePinBinding },
    { TEXT("connect_anim_graph_pins"), &HandleConnectAnimGraphPins },
    { TEXT("copy_anim_graph_nodes"), &HandleCopyAnimGraphNodes },
    { TEXT("disconnect_anim_graph_pins"), &HandleDisconnectAnimGraphPins },
    { TEXT("get_anim_graph_node_properties"), &HandleGetAnimGraphNodeProperties },
    { TEXT("set_anim_graph_node_pin_binding"), &HandleSetAnimGraphNodePinBinding },
    { TEXT("set_anim_graph_node_property"), &HandleSetAnimGraphNodeProperty },
    { TEXT("set_skeletal_mesh_anim_class"), &HandleSetSkeletalMeshAnimClass },
};
} // namespace UEAgentAnimGraphCommands

#undef LOCTEXT_NAMESPACE
