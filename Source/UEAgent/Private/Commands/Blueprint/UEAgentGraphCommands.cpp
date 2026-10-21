// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Mcp/McpToolCatalog.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Misc/EngineVersionComparison.h" // UE_VERSION_OLDER_THAN: the UE 5.7 paths
#include "Commands/Common/UEAgentGraphUtils.h"
#include "Commands/Blueprint/GraphNodePinLookup.h"

#include "Engine/Blueprint.h"
#include "UObject/UnrealType.h" // FDefaultConstructedPropertyElement: temporary buffer for the set_blueprint_variable_default round-trip
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"

#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_SpawnActorFromClass.h"
#include "K2Node_Self.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_BreakStruct.h"
// Additional spawnable K2 node types (module BlueprintGraph)
#include "K2Node_GetArrayItem.h"
#include "K2Node_Select.h"
#include "K2Node_MakeArray.h"
#include "K2Node_MakeSet.h"
#include "K2Node_MakeMap.h"
#include "K2Node_SwitchInteger.h"
#include "K2Node_SwitchString.h"
#include "K2Node_SwitchName.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_EnumLiteral.h"
#include "K2Node_FormatText.h"
// Macro-instance + delegate-bind node types (module BlueprintGraph)
#include "K2Node_MacroInstance.h"
#include "K2Node_Composite.h"
#include "K2Node_AddDelegate.h"
// Parent: super-call + multicast broadcast node types (module BlueprintGraph)
#include "K2Node_CallParentFunction.h"
#include "K2Node_CallDelegate.h"
// create_widget node_type: UK2Node_CreateWidget (UMGEditor) is MinimalAPI with a PRIVATE header, so
// it is resolved by /Script path and driven through its PUBLIC base UK2Node_ConstructObjectFromClass
// (BlueprintGraph/Classes -- includable; GetClassPin/ReconstructNode exposed). No private include, no
// Build.cs dep. UUserWidget is used only to validate the requested widget class.
#include "K2Node_ConstructObjectFromClass.h"
#include "Blueprint/UserWidget.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "UObject/CoreRedirects.h"        // rename_function_parameter -- FCoreRedirects::AddRedirectList, which makes call sites re-map themselves
#include "Misc/FileHelper.h"              // Persist the +PropertyRedirects (GConfig does NOT work: the engine deletes the section when it reads it)
#include "Misc/Paths.h"                   // FPaths::ProjectConfigDir
#include "K2Node_EditablePinBase.h"
#include "K2Node_Tunnel.h"                // macro signatures: the entry and exit tunnels
#include "K2Node_Timeline.h"
#include "K2Node_ComponentBoundEvent.h"
// list_widget_bound_events: the binding nodes do NOT share a base. UK2Node_AddDelegate derives from
// UK2Node_BaseMCDelegate; the *BoundEvent ones from UK2Node_Event. They have to be walked separately or
// the reader misses exactly the ones add_component_bound_event creates.
// WARNING: there is a fourth, UK2Node_GeneratedBoundEvent, which is NOT walked: no tool in this
// plugin produces it and it has no component field, so it could not tell "what" it is bound to.
// It is declared here so that its absence is a decision and not an oversight.
#include "K2Node_ActorBoundEvent.h"
#include "K2Node_InputAction.h"
#include "K2Node_EnhancedInputAction.h"   // UK2Node_EnhancedInputAction (InputBlueprintNodes module)
#include "InputAction.h"                   // UInputAction (EnhancedInput module)
#include "K2Node_Knot.h"
#include "EdGraphNode_Comment.h"
#include "Engine/TimelineTemplate.h"
#include "Curves/CurveFloat.h"            // Blueprint timeline tracks: float and event curves
#include "Curves/CurveVector.h"
#include "Curves/CurveLinearColor.h"
#include "Curves/RichCurve.h"
#include "Components/PrimitiveComponent.h"

#include "Editor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
// rename_blueprint_variable: the engine does NOT check that the new name is free (it writes VarName and moves
// on), so it is checked here against the whole class chain; and the referencers have to be loaded
// first, because their reference fixup only sees what is already in memory.
#include "Kismet2/Kismet2NameValidators.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetStringLibrary.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "ScopedTransaction.h"
// build_blueprint_graph's rollback is GEditor->UndoTransaction, i.e. the editor's GLOBAL
// undo -- it applies whatever sits on top of the stack, not necessarily ours. To refuse undoing
// someone else's work we have to READ the stack first: UTransactor (the base type of
// GEditor->Trans, with GetQueueLength/GetUndoCount/GetTransaction) and FTransaction (whose
// GetTitle/GetContext identify the top entry) both live in Editor/Transactor.h.
#include "Editor/Transactor.h"

// Blueprint replication & RPC authoring
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "UObject/CoreNetTypes.h"
#include "UObject/ObjectMacros.h"
// Struct-pin default canonicalization (GameplayTag/value_json).
#include "GameplayTagContainer.h"          // FGameplayTag(Container)::RequestGameplayTag
#include "JsonObjectConverter.h"           // FJsonObjectConverter::JsonObjectToUStruct
#include "UObject/StructOnScope.h"         // FStructOnScope
// get/set_property_access_path: UK2Node_PropertyAccess is MinimalAPI with a PRIVATE header, so
// its Path/TextPath UPROPERTYs are reached by reflection (no header include). Its ReconstructNode
// re-resolves the path against the PropertyAccessEditor modular feature -- guarded before calling.
#include "Features/IModularFeatures.h"     // IModularFeatures::IsModularFeatureAvailable (reconstruct guard)
#include "UObject/TextProperty.h"          // FTextProperty (read UK2Node_PropertyAccess::TextPath)

#define LOCTEXT_NAMESPACE "UEAgentGraph"

/**
 * Blueprint Event-Graph editing commands (the "logic control" surface): creates UK2Node instances in a
 * Blueprint's UEdGraph, wires pins, sets pin defaults and inspects the graph, plus the members the graph
 * works with (variables, function parameters, interfaces, event dispatchers, replication/RPC flags).
 * Mutating commands normally run inside an undo transaction and mark the Blueprint modified
 * (structurally when its shape changes).
 */
namespace UEAgentGraphCommands
{

// MCP-DOMAIN: Blueprint

// A reply builder and an SCS lookup reused by several handlers below. The wording of the reply is
// part of the tool contract (callers match on it), so it lives in exactly one place here.
namespace UEAgentGraphCommandsImpl
{

TSharedPtr<FJsonObject> ComponentNotFound(const FString& RequestedComponent)
{
    return mcp_common::MakeErrorReply(FString::Printf(TEXT("Component not found: %s"), *RequestedComponent));
}

// First node of the construction script whose variable name matches (case-insensitive, like
// FString ==). The caller guarantees Scs is valid.
USCS_Node* FindNodeByVariableName(USimpleConstructionScript* Scs, const FString& VariableName)
{
    for (USCS_Node* Candidate : Scs->GetAllNodes())
    {
        if (Candidate && Candidate->GetVariableName().ToString().Equals(VariableName, ESearchCase::IgnoreCase))
        {
            return Candidate;
        }
    }
    return nullptr;
}
}

namespace
{
    UBlueprint* FindBlueprintFlexible(const FString& Name)
    {
        UBlueprint* BP = mcp_common::ResolveBlueprint(Name);
        if (!BP)
        {
            // Allow a full object path too (e.g. /Game/MyDir/BP_Foo.BP_Foo).
            BP = LoadObject<UBlueprint>(nullptr, *Name);
        }
        return BP;
    }

    // Resolve an EXTERNAL class from a Blueprint/widget short name, a full asset path, a full class
    // path (/Game/.../WBP.WBP_C), a native /Script path, or the generated "..._C" class name. Used by
    // add_object_bound_event (dispatcher owner) and the create_widget node_type (widget class).
    // Guaranteed reliable for full paths and loaded "_C" class names; a bare short name resolves when
    // the asset is under /Game/Blueprints or already loaded (callers should prefer a full path).
    UClass* ResolveTargetClassFlexible(const FString& Identifier)
    {
        if (Identifier.IsEmpty())
        {
            return nullptr;
        }
        // (a) Blueprint by short name or asset path -> its (skeleton) generated class.
        if (UBlueprint* BP = FindBlueprintFlexible(Identifier))
        {
            if (BP->GeneratedClass)          { return BP->GeneratedClass; }
            if (BP->SkeletonGeneratedClass)  { return BP->SkeletonGeneratedClass; }
        }
        // (b) A generated "..._C" name -> strip and retry as a Blueprint asset.
        if (Identifier.EndsWith(TEXT("_C")))
        {
            const FString Stripped = Identifier.LeftChop(2);
            if (UBlueprint* BP = FindBlueprintFlexible(Stripped))
            {
                if (BP->GeneratedClass)         { return BP->GeneratedClass; }
                if (BP->SkeletonGeneratedClass) { return BP->SkeletonGeneratedClass; }
            }
        }
        // (c) Direct class resolution: native /Script path, /Game/.../BP.BP_C, or a loaded bare "_C" name.
        if (UClass* C = FUEAgentGraphUtils::ResolveClass(Identifier))
        {
            return C;
        }
        // (d) Retry with a "_C" suffix for a path/name that names the Blueprint but not its class.
        if (!Identifier.EndsWith(TEXT("_C")))
        {
            if (UClass* C = FUEAgentGraphUtils::ResolveClass(Identifier + TEXT("_C")))
            {
                return C;
            }
        }
        return nullptr;
    }

    // Find a multicast delegate (event dispatcher / BlueprintAssignable) property on Class, matching
    // the requested name tolerantly: exact FName, then spaces-stripped FName, then a case-insensitive
    // sweep over each delegate's variable name AND its display name (spaces stripped). OutFound gets the
    // real property name. The tolerance lets callers pass the editor's display-name-ish form
    // ("On Item Selected") for a property whose FName has no spaces.
    FMulticastDelegateProperty* FindMulticastDelegatePropertyTolerant(UClass* Class, const FString& Name, FString& OutFound)
    {
        if (!Class)
        {
            return nullptr;
        }
        const FString Wanted = Name.Replace(TEXT(" "), TEXT(""));
        if (FMulticastDelegateProperty* P = FindFProperty<FMulticastDelegateProperty>(Class, FName(*Name)))
        {
            OutFound = P->GetName();
            return P;
        }
        if (FMulticastDelegateProperty* P = FindFProperty<FMulticastDelegateProperty>(Class, FName(*Wanted)))
        {
            OutFound = P->GetName();
            return P;
        }
        for (TFieldIterator<FMulticastDelegateProperty> It(Class); It; ++It)
        {
            FMulticastDelegateProperty* P = *It;
            const FString PropName = P->GetName();
            const FString DisplayName = P->GetDisplayNameText().ToString().Replace(TEXT(" "), TEXT(""));
            if (PropName.Equals(Wanted, ESearchCase::IgnoreCase)
                || DisplayName.Equals(Wanted, ESearchCase::IgnoreCase)
                || PropName.Equals(Name, ESearchCase::IgnoreCase))
            {
                OutFound = PropName;
                return P;
            }
        }
        return nullptr;
    }

    FVector2D ReadPosition(const TSharedPtr<FJsonObject>& Params)
    {
        if (Params->HasField(TEXT("node_position")))
        {
            return mcp_common::ReadJsonVector2D(Params, TEXT("node_position"));
        }
        FVector2D Pos(0.0f, 0.0f);
        double X = 0.0, Y = 0.0;
        if (Params->TryGetNumberField(TEXT("position_x"), X)) { Pos.X = (float)X; }
        if (Params->TryGetNumberField(TEXT("position_y"), Y)) { Pos.Y = (float)Y; }
        return Pos;
    }

    // Read an optional string array param (e.g. cases / arguments).
    TArray<FString> ReadStringArray(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field)
    {
        TArray<FString> Out;
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (Params->TryGetArrayField(Field, Arr) && Arr)
        {
            for (const TSharedPtr<FJsonValue>& V : *Arr)
            {
                FString S;
                if (V.IsValid() && V->TryGetString(S))
                {
                    Out.Add(S);
                }
            }
        }
        return Out;
    }

    // The search list is extracted so the ERROR MESSAGE can name it.
    // "Function not found" is the most frequent error of the whole Blueprint surface, and
    // without the classes searched and the closest names it leaves nothing to act on.
    TArray<UClass*> BuildFunctionSearchClasses(const FString& ClassId, UBlueprint* BP)
    {
        TArray<UClass*> SearchClasses;
        if (!ClassId.IsEmpty())
        {
            if (UClass* C = FUEAgentGraphUtils::ResolveClass(ClassId))
            {
                SearchClasses.Add(C);
            }
        }
        if (BP && BP->GeneratedClass)
        {
            SearchClasses.Add(BP->GeneratedClass);
        }
        SearchClasses.Add(UKismetSystemLibrary::StaticClass());
        SearchClasses.Add(UGameplayStatics::StaticClass());
        SearchClasses.Add(UKismetMathLibrary::StaticClass());
        return SearchClasses;
    }

    // "similar" candidates within the SAME classes that were searched. No edit
    // distance: containment in either direction is enough, which catches the two usual
    // misses -- the engine's K2_ prefix
    // (GetPawn -> K2_GetPawn) and the partial name (SetActorLocation ->
    // K2_SetActorLocationAndRotation).
    FString SuggestSimilarFunctions(const FString& FuncName, const TArray<UClass*>& SearchClasses)
    {
        const FString Needle = FuncName.ToLower();
        TArray<FString> Hits;
        for (UClass* C : SearchClasses)
        {
            if (!C) { continue; }
            for (TFieldIterator<UFunction> It(C, EFieldIteratorFlags::IncludeSuper); It; ++It)
            {
                const FString Name = It->GetName();
                const FString Lower = Name.ToLower();
                if (Lower.Contains(Needle) || Needle.Contains(Lower))
                {
                    Hits.AddUnique(FString::Printf(TEXT("%s::%s"), *C->GetName(), *Name));
                    if (Hits.Num() >= 8) { break; }
                }
            }
            if (Hits.Num() >= 8) { break; }
        }
        return Hits.Num() > 0 ? FString::Join(Hits, TEXT(", ")) : FString();
    }

    UFunction* ResolveFunction(const FString& FuncName, const FString& ClassId, UBlueprint* BP)
    {
        TArray<UClass*> SearchClasses = BuildFunctionSearchClasses(ClassId, BP);

        const FName FuncFName(*FuncName);
        for (UClass* C : SearchClasses)
        {
            if (C)
            {
                if (UFunction* Found = C->FindFunctionByName(FuncFName))
                {
                    return Found;
                }
            }
        }
        return nullptr;
    }

    // get_graph_nodes support: serialize a graph's nodes, tagging any node that owns
    // a nested graph (collapsed composite / macro instance) with its "subgraph" name,
    // and -- when bExpand -- inlining the nodes of collapsed composite subgraphs
    // recursively. The Visited set guards against cycles / double-visits.
    // bCompact threads token-lean serialization through the recursion;
    // ExcludeClasses skips noise node classes (matched on GetClass()->GetName(),
    // the SAME token NodeToJson reports in "class"); MaxNodes (>0) caps the number
    // of nodes actually emitted and sets bTruncated so a huge EventGraph always fits
    // the budget. TotalCount counts every non-excluded node (past the cap too) so the
    // caller learns how many exist; ExcludedCount counts nodes dropped by ExcludeClasses.
    void AppendGraphNodes(UEdGraph* Graph, TArray<TSharedPtr<FJsonValue>>& OutNodes,
                          bool bExpand, TSet<UEdGraph*>& Visited,
                          bool bCompact, const TSet<FString>& ExcludeClasses,
                          int32 MaxNodes, bool& bTruncated, int32& TotalCount, int32& ExcludedCount)
    {
        if (!Graph || Visited.Contains(Graph))
        {
            return;
        }
        Visited.Add(Graph);

        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node)
            {
                continue;
            }
            // Skip excluded/noise node classes before doing any work.
            if (ExcludeClasses.Contains(Node->GetClass()->GetName()))
            {
                ++ExcludedCount;
                continue;
            }
            ++TotalCount;
            // Bounded size lever -- keep counting for total_node_count but stop
            // appending (and mark truncated) once the cap is reached.
            if (MaxNodes > 0 && OutNodes.Num() >= MaxNodes)
            {
                bTruncated = true;
                continue;
            }
            TSharedPtr<FJsonObject> NodeObj = FUEAgentGraphUtils::NodeToJson(Node, true, bCompact);
            // Advertise the drill-in name for collapsed composites / macro instances,
            // so a caller can pass it straight back as graph_name to read the inside.
            if (UEdGraph* Sub = FUEAgentGraphUtils::GetNodeSubGraph(Node))
            {
                NodeObj->SetStringField(TEXT("subgraph"), Sub->GetName());
            }
            if (bExpand)
            {
                // When flattening, tag which graph each node came from.
                NodeObj->SetStringField(TEXT("graph"), Graph->GetName());
            }
            OutNodes.Add(MakeShared<FJsonValueObject>(NodeObj));
        }

        if (bExpand)
        {
            // Follow collapsed composite bound graphs only (not referenced macro
            // libraries, which would drag whole engine macros inline).
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (UK2Node_Composite* Composite = Cast<UK2Node_Composite>(Node))
                {
                    AppendGraphNodes(Composite->BoundGraph, OutNodes, bExpand, Visited,
                                     bCompact, ExcludeClasses, MaxNodes, bTruncated, TotalCount, ExcludedCount);
                }
            }
        }
    }
}

// Graph lookups, node-creation steps and reply openers the handlers below share. A lookup answers with
// the error reply naming what it could not find, or null.
namespace UEAgentGraphCommandsImpl
{
TSharedPtr<FJsonObject> FindBlueprint(const FString& BlueprintName, UBlueprint*& OutBlueprint)
{
    OutBlueprint = FindBlueprintFlexible(BlueprintName);
    if (!OutBlueprint)
    {
        return mcp_common::BlueprintNotFoundReply(BlueprintName);
    }
    return nullptr;
}

// The graph named by the optional graph_name param, the event graph when it is absent.
TSharedPtr<FJsonObject> FindTargetGraph(const TSharedPtr<FJsonObject>& Params, UBlueprint* Blueprint, UEdGraph*& OutGraph)
{
    FString GraphName;
    Params->TryGetStringField(TEXT("graph_name"), GraphName);
    OutGraph = FUEAgentGraphUtils::FindGraphByNameDeep(Blueprint, GraphName);
    if (!OutGraph)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Graph not found: %s"), *GraphName));
    }
    return nullptr;
}

// The Blueprint named BlueprintName and its graph_name graph (see FindTargetGraph).
TSharedPtr<FJsonObject> ResolveBlueprintGraph(const TSharedPtr<FJsonObject>& Params, const FString& BlueprintName,
                                              UBlueprint*& OutBlueprint, UEdGraph*& OutGraph)
{
    if (TSharedPtr<FJsonObject> Error = FindBlueprint(BlueprintName, OutBlueprint))
    {
        return Error;
    }
    return FindTargetGraph(Params, OutBlueprint, OutGraph);
}

// What a connect_pins or validate_pin_connection request names, and what it resolves to.
struct FPinPair
{
    FString BlueprintName;
    FString SourceNodeId;
    FString TargetNodeId;
    UBlueprint* Blueprint = nullptr;
    UEdGraph* Graph = nullptr;
    UEdGraphPin* SourcePin = nullptr;
    UEdGraphPin* TargetPin = nullptr;
};

// Reads a connect_pins or validate_pin_connection request and resolves its Blueprint, graph and pins.
TSharedPtr<FJsonObject> ReadPinPair(const TSharedPtr<FJsonObject>& Params, FPinPair& Out)
{
    FString SourcePinName, TargetPinName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), Out.BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("source_node_id"), Out.SourceNodeId, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("source_pin"), SourcePinName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("target_node_id"), Out.TargetNodeId, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("target_pin"), TargetPinName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    // Not ResolveBlueprintGraph: the tool catalog follows Params two calls deep and has to see graph_name.
    if (TSharedPtr<FJsonObject> Error = FindBlueprint(Out.BlueprintName, Out.Blueprint)) { return Error; }
    if (TSharedPtr<FJsonObject> Error = FindTargetGraph(Params, Out.Blueprint, Out.Graph)) { return Error; }
    return GraphNodePinLookup::FindConnectionPins(Out.Graph, Out.SourceNodeId, SourcePinName, Out.TargetNodeId, TargetPinName,
                                                  Out.SourcePin, Out.TargetPin);
}

// The reply of a command that made Node on Graph: the node with its pins, then blueprint_name and graph_name.
TSharedPtr<FJsonObject> NodeReply(UEdGraphNode* Node, const FString& BlueprintName, const UEdGraph* Graph)
{
    TSharedPtr<FJsonObject> Reply = FUEAgentGraphUtils::NodeToJson(Node, true);
    Reply->SetStringField(TEXT("blueprint_name"), BlueprintName);
    Reply->SetStringField(TEXT("graph_name"), Graph->GetName());
    return Reply;
}

// A reply that starts with blueprint_name and Field, which names what the command acted on.
TSharedPtr<FJsonObject> BlueprintReply(const FString& BlueprintName, const TCHAR* Field, const FString& Value)
{
    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("blueprint_name"), BlueprintName);
    Reply->SetStringField(Field, Value);
    return Reply;
}

// The last step of the node commands that compile: marks Blueprint structurally modified, refreshes
// Graph and compiles Blueprint.
void MarkModifiedAndCompile(UBlueprint* Blueprint, UEdGraph* Graph)
{
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    Graph->NotifyGraphChanged();
    FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None);
}

// Places Node on Graph at Position, allocating its pins BEFORE PostPlacedNewNode: the reverse of
// FGraphNodeCreator::Finalize, for nodes whose PostPlacedNewNode reads one of their pins.
void PlaceNodePinsFirst(UEdGraph* Graph, UEdGraphNode* Node, const FVector2D& Position)
{
    Node->NodePosX = Position.X;
    Node->NodePosY = Position.Y;
    Graph->AddNode(Node, /*bFromUI=*/false);
    Node->CreateNewGuid();
    Node->AllocateDefaultPins();
    Node->PostPlacedNewNode();
}

// A TNode created on Graph at Position by FGraphNodeCreator. Configure runs before Finalize, whose
// AllocateDefaultPins reads what it sets.
template <typename TNode, typename TConfigure>
TNode* SpawnNode(UEdGraph* Graph, const FVector2D& Position, TConfigure Configure)
{
    FGraphNodeCreator<TNode> Creator(*Graph);
    TNode* Node = Creator.CreateNode();
    Configure(Node);
    Node->NodePosX = Position.X;
    Node->NodePosY = Position.Y;
    Creator.Finalize();
    return Node;
}

template <typename TNode>
TNode* SpawnNode(UEdGraph* Graph, const FVector2D& Position)
{
    return SpawnNode<TNode>(Graph, Position, [](TNode*) {});
}

// A MakeArray / MakeSet / MakeMap node with NumInputs input pins. A new one has one, and AddInputPin
// is only safe after Finalize (it reads the Blueprint).
template <typename TNode>
TNode* SpawnMakeContainerNode(UEdGraph* Graph, const FVector2D& Position, int32 NumInputs)
{
    TNode* Node = SpawnNode<TNode>(Graph, Position);
    for (int32 i = 1; i < NumInputs; ++i)
    {
        Node->AddInputPin();
    }
    return Node;
}

// The engine's StandardMacros library (EditorKismetResources only holds a redirector to it).
UBlueprint* LoadStandardMacros()
{
    return LoadObject<UBlueprint>(nullptr, TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"));
}

// The macro graph named GraphName in Library's MacroGraphs; null when Library is null or has none.
UEdGraph* FindMacroGraph(const UBlueprint* Library, const FString& GraphName)
{
    if (Library)
    {
        for (UEdGraph* MacroGraph : Library->MacroGraphs)
        {
            if (MacroGraph && MacroGraph->GetName() == GraphName)
            {
                return MacroGraph;
            }
        }
    }
    return nullptr;
}

// The StandardMacros graph an add_graph_node alias node_type stands for; null for any other node_type.
const TCHAR* FindStandardMacroAlias(const FString& NodeType)
{
    struct FAliasEntry
    {
        const TCHAR* Alias;
        const TCHAR* MacroGraphName;
    };
    static const FAliasEntry Entries[] =
    {
        { TEXT("foreach"),            TEXT("ForEachLoop") },
        { TEXT("foreach_with_break"), TEXT("ForEachLoopWithBreak") },
        { TEXT("do_once"),            TEXT("DoOnce") },
        { TEXT("gate"),               TEXT("Gate") },
        { TEXT("flipflop"),           TEXT("FlipFlop") },
        { TEXT("for_loop"),           TEXT("ForLoop") },
        { TEXT("while_loop"),         TEXT("WhileLoop") },
        { TEXT("is_valid"),           TEXT("IsValid") },
    };
    for (const FAliasEntry& Entry : Entries)
    {
        if (NodeType == Entry.Alias)
        {
            return Entry.MacroGraphName;
        }
    }
    return nullptr;
}
}

// Class and macro-graph resolution for add_graph_node's call_function and macro_instance node types.
namespace
{
    // Resolve one of the standard Blueprint function libraries (all in the Engine
    // module) to its UClass. Accepts the bare class name with or without the 'U'
    // prefix; the four canonical names are matched directly to their StaticClass
    // (robust, no name-lookup traps), with a generic /Script/Engine.<Name> and
    // TryFindTypeSlow fallback for any other engine library.
    UClass* ResolveFunctionLibraryClass(const FString& LibraryName)
    {
        FString Name = LibraryName;
        Name.TrimStartAndEndInline();
        if (Name.StartsWith(TEXT("U")) && Name.Len() > 1 && FChar::IsUpper(Name[1]))
        {
            // Only strip a leading 'U' if it looks like a UObject class prefix.
            Name.RightChopInline(1, EAllowShrinking::No);
        }

        if (Name.Equals(TEXT("KismetMathLibrary"), ESearchCase::IgnoreCase))   { return UKismetMathLibrary::StaticClass(); }
        if (Name.Equals(TEXT("KismetSystemLibrary"), ESearchCase::IgnoreCase)) { return UKismetSystemLibrary::StaticClass(); }
        if (Name.Equals(TEXT("GameplayStatics"), ESearchCase::IgnoreCase))     { return UGameplayStatics::StaticClass(); }
        if (Name.Equals(TEXT("KismetStringLibrary"), ESearchCase::IgnoreCase)) { return UKismetStringLibrary::StaticClass(); }

        // Generic fallback: a fully-qualified path, or a slow short-name search.
        if (UClass* ByPath = LoadObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/Engine.%s"), *Name)))
        {
            return ByPath;
        }
        return UClass::TryFindTypeSlow<UClass>(Name);
    }

    // Resolve a USER BlueprintFunctionLibrary (or any Blueprint) asset to its generated UClass,
    // so a /Game/... BFL's static functions can be called without the '_C' hack. Accepts the asset path
    // ("/Game/Dir/BFL_X" or "/Game/Dir/BFL_X.BFL_X"), the generated-class path ("...BFL_X_C"), or any
    // class object path. Returns null when nothing loads.
    UClass* ResolveUserFunctionLibraryClass(const FString& Identifier)
    {
        FString Id = Identifier;
        Id.TrimStartAndEndInline();
        if (Id.IsEmpty())
        {
            return nullptr;
        }

        // Direct: an explicit generated-class path (or any class object path) loads straight to a UClass.
        if (UClass* Direct = LoadObject<UClass>(nullptr, *Id))
        {
            return Direct;
        }

        // Asset path -> UBlueprint -> GeneratedClass. Covers "/Game/Dir/BFL_X" and "/Game/Dir/BFL_X.BFL_X".
        if (UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *Id))
        {
            return BP->GeneratedClass;
        }

        // Bare package path with no object part: synthesize "<path>.<asset>_C" then "<path>.<asset>".
        if (Id.StartsWith(TEXT("/")) && !Id.Contains(TEXT(".")))
        {
            FString AssetName = Id;
            int32 SlashIdx = INDEX_NONE;
            if (Id.FindLastChar(TEXT('/'), SlashIdx))
            {
                AssetName = Id.RightChop(SlashIdx + 1);
            }
            if (UClass* ByGen = LoadObject<UClass>(nullptr, *FString::Printf(TEXT("%s.%s_C"), *Id, *AssetName)))
            {
                return ByGen;
            }
            if (UBlueprint* BP2 = LoadObject<UBlueprint>(nullptr, *FString::Printf(TEXT("%s.%s"), *Id, *AssetName)))
            {
                return BP2->GeneratedClass;
            }
        }
        return nullptr;
    }

    // The class a call_function node names (target_class or function_library): a class name or path, a
    // user BlueprintFunctionLibrary asset path, or an engine library by name with or without its U prefix.
    UClass* ResolveCallTargetClass(const FString& ClassId)
    {
        if (UClass* Resolved = FUEAgentGraphUtils::ResolveClass(ClassId))
        {
            return Resolved;
        }
        if (ClassId.StartsWith(TEXT("/")))
        {
            if (UClass* UserLibrary = ResolveUserFunctionLibraryClass(ClassId))
            {
                return UserLibrary;
            }
        }
        return ResolveFunctionLibraryClass(ClassId);
    }

    // Resolve a macro UEdGraph from MacroParam + optional MacroGraphParam: "<asset-path>:<MacroGraphName>"
    // (single colon separates the macro library asset from the macro graph inside it), OR a library asset
    // path in MacroParam with the graph name in MacroGraphParam (add_graph_node's macro_library +
    // macro_graph_name). A bare graph name falls back to the TARGET Blueprint's own MacroGraphs and then
    // the engine StandardMacros. OutError carries a human-readable reason.
    UEdGraph* ResolveMacroInstanceGraph(UBlueprint* TargetBlueprint, const FString& MacroParam,
                                        const FString& MacroGraphParam, FString& OutError)
    {
        FString AssetPath;
        FString GraphName;

        int32 ColonIdx = INDEX_NONE;
        // A ':' at index >1 disambiguates a "<path>:<Graph>" spec from a drive-less path. A leading "/Game.."
        // has no ':'; a "/Script/..:Graph" is unusual but the >1 guard avoids splitting on a stray early ':'.
        if (MacroParam.FindLastChar(TEXT(':'), ColonIdx) && ColonIdx > 1)
        {
            AssetPath = MacroParam.Left(ColonIdx);
            GraphName = MacroParam.RightChop(ColonIdx + 1);
        }
        else if (!MacroGraphParam.IsEmpty())
        {
            // MacroParam is a library asset path; the graph name comes from MacroGraphParam.
            AssetPath = MacroParam;
            GraphName = MacroGraphParam;
        }
        else
        {
            // A bare name with no ":<Graph>" suffix and no MacroGraphParam is the GRAPH NAME, resolved against
            // the Blueprint's own MacroGraphs then engine StandardMacros ("ForEachLoop", "IsValid", "Do Once").
            AssetPath.Empty();
            GraphName = MacroParam;
        }
        AssetPath.TrimStartAndEndInline();
        GraphName.TrimStartAndEndInline();

        if (GraphName.IsEmpty())
        {
            OutError = TEXT("macro graph name is empty (pass macro_graph_name, or '<library-path>:<GraphName>')");
            return nullptr;
        }

        // 1) Explicit macro-library asset path -> search its MacroGraphs.
        if (!AssetPath.IsEmpty())
        {
            UBlueprint* MacroLib = FindBlueprintFlexible(AssetPath);
            if (!MacroLib)
            {
                OutError = FString::Printf(TEXT("Macro library Blueprint not found: %s"), *AssetPath);
                return nullptr;
            }
            if (UEdGraph* G = UEAgentGraphCommandsImpl::FindMacroGraph(MacroLib, GraphName)) { return G; }
            OutError = FString::Printf(TEXT("Macro graph '%s' not found in library '%s'"), *GraphName, *AssetPath);
            return nullptr;
        }

        // 2) No library path: search the target Blueprint's own MacroGraphs, then StandardMacros.
        if (UEdGraph* G = UEAgentGraphCommandsImpl::FindMacroGraph(TargetBlueprint, GraphName)) { return G; }
        if (UEdGraph* G = UEAgentGraphCommandsImpl::FindMacroGraph(UEAgentGraphCommandsImpl::LoadStandardMacros(), GraphName)) { return G; }
        OutError = FString::Printf(
            TEXT("Macro graph '%s' not found in the target Blueprint's MacroGraphs or in StandardMacros (pass a library path)"),
            *GraphName);
        return nullptr;
    }
}

// MCP:add_graph_node - Create ONE node in a Blueprint's event/function graph (node_type selects the kind) and return it WITH its freshly allocated pins, so connect_pins / set_pin_default_value can wire it immediately without a follow-up get_graph_nodes; with compile=true it also compiles the Blueprint afterwards. Use build_blueprint_graph instead when several nodes must be created AND wired in a single undo transaction, and get_function_pins to learn a call_function's pin names BEFORE creating anything. node_type='event' creates a REAL override (the declaring interface/parent class is resolved automatically and bOverrideFunction is set; target_class is ignored here; an existing override is returned with already_exists:true) and fails loudly for a name that is not an overridable event -- an interface method WITH a return value is not an event, use add_blueprint_function with override=true. node_type='custom_event' is idempotent by name (an existing custom event of that name in the graph comes back with already_existed:true, since a second one breaks the compile); a custom event cannot be given parameters, so anything that needs arguments (an RPC in particular) must be a function. node_type='get_component_by_class' makes a GetComponentByClass node BORN TYPED to target_class, so its ReturnValue connects without a cast -- inside build_blueprint_graph a generic one would be connected before its class pin default is applied and fail.
//   @param blueprint_name - Blueprint name (resolved under /Game/Blueprints) or full asset path. Pass the FULL path when the short name is ambiguous: a bare name that matches two assets resolves to one of them silently.
//   @param node_type - One of event, custom_event, call_function, print_string, variable_get, variable_set, branch, sequence, self, cast, spawn_actor (alias spawn_actor_from_class), create_widget, make_struct, break_struct, get_array_item, select, make_array, make_set, make_map, switch_int, switch_string, switch_name, switch_enum, enum_literal, format_text, foreach, foreach_with_break, do_once, gate, flipflop, for_loop, while_loop, is_valid, add_delegate, macro_instance, get_component_by_class
//   @param event_name - Event name; required for event and custom_event node types (e.g. "ReceiveActorBeginOverlap", "ReceiveBeginPlay" for event)
//   @param function_name - Function to call; required for call_function node type
//   @param target_class - Class owning the function or cast/spawn target class; required for cast, create_widget and get_component_by_class. For call_function it is searched STRICTLY: a function that is not on that class is an error, never a silent fallback to another library; it also resolves a user BlueprintFunctionLibrary by asset path ("/Game/Dir/BFL_X"). For get_component_by_class it is the component class to fetch (a UActorComponent subclass). For variable_get/variable_set it names the class that owns the variable, producing a non-self member-access node (with a 'target' pin) instead of a self-context node. For create_widget it names the widget class to construct (e.g. WBP_Foo_C, a full asset path, or the widget Blueprint short name); the node exposes that widget's Expose-On-Spawn pins.
//   @param function_library - Optional, call_function only: the library class owning function_name, used instead of target_class -- an engine library by name ("KismetMathLibrary", "KismetSystemLibrary", "GameplayStatics", "KismetStringLibrary", with or without the U prefix, or any /Script/Engine class) or a user BlueprintFunctionLibrary asset path ("/Game/Dir/BFL_X", "/Game/Dir/BFL_X.BFL_X" or the "..._C" class). Searched strictly, like target_class.
//   @param variable_name - Variable to get/set; required for variable_get and variable_set
//   @param scope - Optional scope hint for variable_get/variable_set: 'auto' (default; a function-local variable declared in the target graph wins, else a self/member variable), 'local' (function-local variable ONLY), or 'member' (member variable only -- self, or on target_class when supplied). Ignored by other node types.
//   @param struct_type - Struct type; required for make_struct and break_struct nodes: a UScriptStruct path (/Game/.../S_Foo.S_Foo), bare name, or engine alias (Vector/Rotator/Transform...)
//   @param enum_path - Enum path or name; required for switch_enum and enum_literal nodes. Optional for select: the enum then drives the Index pin and one option pin is generated per enum entry.
//   @param index_type - Optional alias of enum_path for node_type=select.
//   @param option_count - Optional, select only: total number of option pins to end up with (default the built-in 2; ignored when an enum drives the index).
//   @param num_inputs - Optional input-pin count for make_array/make_set/make_map (added after spawn)
//   @param cases - Optional string array of case pins for switch_int/switch_string/switch_name
//   @param arguments - Optional string array of argument names for format_text
//   @param dispatcher - Event-dispatcher (multicast delegate) name; required for add_delegate node type. Must already exist in the COMPILED class (add_event_dispatcher + compile_blueprint), and it binds on self only -- for a dispatcher on another class use add_object_bound_event.
//   @param macro_graph_name - Macro graph name; required for node_type=macro_instance. Resolved against this Blueprint's own MacroGraphs first, then the engine StandardMacros; "<library-path>:<MacroGraphName>" (e.g. "/Game/Lib/ML_X.ML_X:MyMacro") resolves it in that macro library instead.
//   @param macro_library - Optional, macro_instance only: asset path of a macro LIBRARY Blueprint to resolve macro_graph_name in -- the only way to reach a third-party or project macro library.
//   @param macro_source - Set to "standard_macros" to skip this Blueprint's own MacroGraphs and resolve macro_graph_name only in the engine StandardMacros library. Only read for node_type=macro_instance.
//   @param compile - Optional bool, default false: compile the Blueprint after placing the node. Ignored inside build_blueprint_graph, which never compiles its steps.
//   @param graph_name - Target graph name; defaults to the Blueprint's event graph when omitted
//   @param position_x - Node X position on the graph canvas (ignored when node_position is set)
//   @param position_y - Node Y position on the graph canvas (ignored when node_position is set)
//   @param node_position - Node canvas position as [x, y]; takes precedence over position_x/position_y
//   @returns { node_id, class, title, pos_x, pos_y, comment, pins:[{name, direction:'input'|'output', type, linked_to, default}], blueprint_name, graph_name, compiled, event_name?, node_title?, already_existed?, is_override?, already_exists?, cast_to?, function_library?, function?, struct_type?, index_pin?, return_value_pin?, option_pins?, option_count?, index_enum?, input_pins?, output_pins?, component_class?, return_pin_type? } -- the created node WITH its freshly-allocated pins. The pin names are already here, so you can wire the node via connect_pins/set_pin_default_value WITHOUT a follow-up get_graph_nodes. The optional fields belong to their node_type: event_name/node_title/already_existed for custom_event; event_name/is_override/already_exists for event (is_override:true is what tells a live override from a dead look-alike); cast_to for cast; function_library/function for call_function; struct_type for make_struct/break_struct; index_pin/return_value_pin/option_pins/option_count/index_enum for select; input_pins/output_pins (pin names by direction) for macro_instance; component_class/return_pin_type for get_component_by_class (return_pin_type is the oracle: if it does not name the requested class, the pin stayed generic). Read "class" as a sanity check: a node_type that was expected to produce an async/latent node and comes back as "K2Node_CallFunction" is a mute no-op that still compiles 0/0.
TSharedPtr<FJsonObject> HandleAddGraphNode(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString NodeType;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("node_type"), NodeType, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    NodeType = NodeType.ToLower();

    UBlueprint* Blueprint = nullptr;
    UEdGraph* Graph = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::ResolveBlueprintGraph(Params, BlueprintName, Blueprint, Graph)) { return Error; }

    const FVector2D Position = ReadPosition(Params);

    // Optional compile once the node is placed. build_blueprint_graph strips it from its steps.
    bool bCompile = false;
    Params->TryGetBoolField(TEXT("compile"), bCompile);

    // A custom event compiles to a function, so a second one with the same name yields "more than one
    // function with the same name" and breaks the Blueprint: an existing one of that name is returned
    // as it is, before anything is modified.
    if (NodeType == TEXT("custom_event"))
    {
        FString ExistingEventName;
        if (Params->TryGetStringField(TEXT("event_name"), ExistingEventName))
        {
            const FName EventFName(*ExistingEventName);
            for (UEdGraphNode* ExistingNode : Graph->Nodes)
            {
                UK2Node_CustomEvent* ExistingEvent = Cast<UK2Node_CustomEvent>(ExistingNode);
                if (ExistingEvent && ExistingEvent->CustomFunctionName == EventFName)
                {
                    TSharedPtr<FJsonObject> Existing = UEAgentGraphCommandsImpl::NodeReply(ExistingEvent, BlueprintName, Graph);
                    Existing->SetStringField(TEXT("event_name"), ExistingEvent->CustomFunctionName.ToString());
                    Existing->SetStringField(TEXT("node_title"), ExistingEvent->GetNodeTitle(ENodeTitleType::ListView).ToString());
                    Existing->SetBoolField(TEXT("already_existed"), true);
                    Existing->SetBoolField(TEXT("compiled"), false);
                    return Existing;
                }
            }
        }
    }

    FScopedTransaction Transaction(LOCTEXT("AddGraphNode", "MCP: Add Graph Node"));
    Blueprint->Modify();
    Graph->Modify();

    UEdGraphNode* NewNode = nullptr;
    FString ErrorMessage;
    // Fields one node type adds to the reply, merged into it at the end.
    TSharedPtr<FJsonObject> Extra = MakeShared<FJsonObject>();

    // Each node type reads only the params it needs, into a bool tested afterwards. A read negated
    // inside the if would make the catalog publish that param as required for EVERY node type, so
    // the server would refuse a call that omits, say, enum_path for an event node.
    if (NodeType == TEXT("event"))
    {
        FString EventName;
        const bool bHasEventName = Params->TryGetStringField(TEXT("event_name"), EventName);
        if (!bHasEventName)
        {
            return mcp_common::MakeErrorReply(TEXT("event node requires 'event_name'"));
        }
        // SpawnEventNode returns the Blueprint's existing override when there is one; the events that
        // existed before tell which.
        TArray<UK2Node_Event*> EventsBefore;
        FBlueprintEditorUtils::GetAllNodesOfClass<UK2Node_Event>(Blueprint, EventsBefore);
        UK2Node_Event* EventNode = mcp_common::SpawnEventNode(Graph, EventName, Position);
        NewNode = EventNode;
        if (!EventNode)
        {
            ErrorMessage = FString::Printf(TEXT("Could not create event '%s' (is it a valid overridable event?)"), *EventName);
        }
        else
        {
            Extra->SetStringField(TEXT("event_name"), EventName);
            Extra->SetBoolField(TEXT("is_override"), EventNode->bOverrideFunction != 0);
            if (EventsBefore.Contains(EventNode))
            {
                Extra->SetBoolField(TEXT("already_exists"), true);
            }
        }
    }
    else if (NodeType == TEXT("custom_event"))
    {
        FString EventName;
        const bool bHasEventName = Params->TryGetStringField(TEXT("event_name"), EventName);
        if (!bHasEventName)
        {
            return mcp_common::MakeErrorReply(TEXT("custom_event node requires 'event_name'"));
        }
        UK2Node_CustomEvent* CustomEventNode = UEAgentGraphCommandsImpl::SpawnNode<UK2Node_CustomEvent>(Graph, Position,
            [&EventName](UK2Node_CustomEvent* CustomEvent) { CustomEvent->CustomFunctionName = FName(*EventName); });
        Extra->SetStringField(TEXT("event_name"), CustomEventNode->CustomFunctionName.ToString());
        Extra->SetStringField(TEXT("node_title"), CustomEventNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
        NewNode = CustomEventNode;
    }
    else if (NodeType == TEXT("call_function") || NodeType == TEXT("print_string"))
    {
        FString FunctionName;
        FString TargetClass;
        FString FunctionLibrary;
        if (NodeType == TEXT("print_string"))
        {
            FunctionName = TEXT("PrintString");
            TargetClass = TEXT("KismetSystemLibrary");
        }
        else
        {
            const bool bHasFunctionName = Params->TryGetStringField(TEXT("function_name"), FunctionName);
            if (!bHasFunctionName)
            {
                return mcp_common::MakeErrorReply(TEXT("call_function node requires 'function_name'"));
            }
            Params->TryGetStringField(TEXT("target_class"), TargetClass);
            Params->TryGetStringField(TEXT("function_library"), FunctionLibrary);
        }
        // A named class is searched strictly: a function that is not on it is an error, never a silent
        // fallback to another library that happens to have a function of that name.
        const bool bLibraryNamed = !FunctionLibrary.IsEmpty();
        const FString ClassId = bLibraryNamed ? FunctionLibrary : TargetClass;
        UFunction* Function = nullptr;
        if (!ClassId.IsEmpty())
        {
            UClass* OwnerClass = ResolveCallTargetClass(ClassId);
            if (!OwnerClass)
            {
                return mcp_common::MakeErrorReply(FString::Printf(TEXT("Could not resolve %s: %s"),
                    bLibraryNamed ? TEXT("function_library") : TEXT("target_class"), *ClassId));
            }
            Function = OwnerClass->FindFunctionByName(FName(*FunctionName));
            if (!Function)
            {
                const FString Similar = SuggestSimilarFunctions(FunctionName, { OwnerClass });
                return mcp_common::MakeErrorReply(FString::Printf(TEXT("Function '%s' not found on '%s'%s"),
                    *FunctionName, *OwnerClass->GetName(),
                    Similar.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (similar: %s)"), *Similar)));
            }
        }
        else
        {
            Function = ResolveFunction(FunctionName, FString(), Blueprint);
            if (!Function)
            {
                return mcp_common::MakeErrorReply(FString::Printf(TEXT("Function not found: %s"), *FunctionName));
            }
        }
        NewNode = mcp_common::CreateFunctionCallNode(Graph, Function, Position);
        if (!NewNode) { ErrorMessage = TEXT("Failed to create call-function node"); }
        UClass* FunctionOwner = Function->GetOwnerClass();
        Extra->SetStringField(TEXT("function_library"), FunctionOwner ? FunctionOwner->GetName() : FString());
        Extra->SetStringField(TEXT("function"), Function->GetName());
    }
    else if (NodeType == TEXT("variable_get") || NodeType == TEXT("variable_set"))
    {
        FString VarName;
        const bool bHasVarName = Params->TryGetStringField(TEXT("variable_name"), VarName);
        if (!bHasVarName)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("%s node requires 'variable_name'"), *NodeType));
        }

        // Optional non-self member access: target_class names the class that owns the variable
        // (emits a member-access node with a "target" pin instead of a self-context node). Only
        // resolved when present + non-empty; present-but-unresolvable is a hard error.
        UClass* VarTargetClass = nullptr;
        FString VarTargetClassName;
        if (Params->TryGetStringField(TEXT("target_class"), VarTargetClassName) && !VarTargetClassName.IsEmpty())
        {
            VarTargetClass = FUEAgentGraphUtils::ResolveClass(VarTargetClassName);
            if (!VarTargetClass)
            {
                return mcp_common::MakeErrorReply(FString::Printf(TEXT("Could not resolve target_class: %s"), *VarTargetClassName));
            }
        }

        // Optional scope hint: 'auto' (default; a function-local of this graph wins, else member),
        // 'local' (function-local only), or 'member' (member variable only). Threaded verbatim
        // from build_blueprint_graph, which forwards each node's params to add_graph_node.
        FString VarScope;
        Params->TryGetStringField(TEXT("scope"), VarScope);

        FString VarError;
        if (NodeType == TEXT("variable_get"))
        {
            NewNode = mcp_common::SpawnVariableGetNode(Graph, Blueprint, VarName, Position, VarTargetClass, VarScope, &VarError);
        }
        else
        {
            NewNode = mcp_common::SpawnVariableSetNode(Graph, Blueprint, VarName, Position, VarTargetClass, VarScope, &VarError);
        }
        if (!NewNode)
        {
            ErrorMessage = VarError.IsEmpty() ? FString::Printf(TEXT("Variable not found on blueprint: %s"), *VarName) : VarError;
        }
    }
    else if (NodeType == TEXT("branch") || NodeType == TEXT("if_then_else"))
    {
        NewNode = UEAgentGraphCommandsImpl::SpawnNode<UK2Node_IfThenElse>(Graph, Position);
    }
    else if (NodeType == TEXT("sequence"))
    {
        NewNode = UEAgentGraphCommandsImpl::SpawnNode<UK2Node_ExecutionSequence>(Graph, Position);
    }
    else if (NodeType == TEXT("self"))
    {
        NewNode = mcp_common::SpawnSelfNode(Graph, Position);
    }
    else if (NodeType == TEXT("cast") || NodeType == TEXT("dynamic_cast"))
    {
        FString TargetClassName;
        const bool bHasTargetClass = Params->TryGetStringField(TEXT("target_class"), TargetClassName);
        if (!bHasTargetClass)
        {
            return mcp_common::MakeErrorReply(TEXT("cast node requires 'target_class'"));
        }
        UClass* TargetClass = FUEAgentGraphUtils::ResolveClass(TargetClassName);
        if (!TargetClass)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Could not resolve target_class: %s"), *TargetClassName));
        }
        NewNode = UEAgentGraphCommandsImpl::SpawnNode<UK2Node_DynamicCast>(Graph, Position, [TargetClass](UK2Node_DynamicCast* CastNode)
        {
            CastNode->TargetType = TargetClass;
            CastNode->SetPurity(false);
        });
        Extra->SetStringField(TEXT("cast_to"), TargetClass->GetName());
    }
    else if (NodeType == TEXT("spawn_actor") || NodeType == TEXT("spawn_actor_from_class"))
    {
        FString TargetClassName;
        Params->TryGetStringField(TEXT("target_class"), TargetClassName);
        // NOTE: do NOT use FGraphNodeCreator here. Its Finalize() runs PostPlacedNewNode()
        // BEFORE AllocateDefaultPins() (EdGraph.h), but UK2Node_SpawnActorFromClass::
        // PostPlacedNewNode() dereferences the ScaleMethod pin -- which only exists after
        // AllocateDefaultPins() -- so Finalize() null-derefs and CRASHES the editor. Create
        // manually with pins allocated FIRST, then run PostPlacedNewNode().
        UK2Node_SpawnActorFromClass* Node = NewObject<UK2Node_SpawnActorFromClass>(Graph);
        UEAgentGraphCommandsImpl::PlaceNodePinsFirst(Graph, Node, Position);
        if (!TargetClassName.IsEmpty())
        {
            if (UClass* TargetClass = FUEAgentGraphUtils::ResolveClass(TargetClassName))
            {
                if (UEdGraphPin* ClassPin = Node->GetClassPin())
                {
                    ClassPin->DefaultObject = TargetClass;
                    Node->ReconstructNode();
                }
            }
        }
        NewNode = Node;
    }
    else if (NodeType == TEXT("make_struct") || NodeType == TEXT("break_struct"))
    {
        FString StructTypeName;
        const bool bHasStructType = Params->TryGetStringField(TEXT("struct_type"), StructTypeName);
        if (!bHasStructType)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("%s node requires 'struct_type'"), *NodeType));
        }
        UScriptStruct* Struct = FUEAgentGraphUtils::ResolveScriptStruct(StructTypeName);
        if (!Struct)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Could not resolve struct_type: %s"), *StructTypeName));
        }
        // StructType MUST be set before Finalize, whose AllocateDefaultPins emits the per-member pins
        // from it; the reconstruct then refreshes them from the struct.
        const auto SetStructType = [Struct](auto* StructNode) { StructNode->StructType = Struct; };
        if (NodeType == TEXT("make_struct"))
        {
            NewNode = UEAgentGraphCommandsImpl::SpawnNode<UK2Node_MakeStruct>(Graph, Position, SetStructType);
        }
        else
        {
            NewNode = UEAgentGraphCommandsImpl::SpawnNode<UK2Node_BreakStruct>(Graph, Position, SetStructType);
        }
        NewNode->ReconstructNode();
        Extra->SetStringField(TEXT("struct_type"), Struct->GetPathName());
    }
    // --- Additional spawnable K2 node types ----------------------------------
    else if (NodeType == TEXT("get_array_item"))
    {
        // Zero-config: pins start wildcard and resolve when the Array pin is linked.
        NewNode = UEAgentGraphCommandsImpl::SpawnNode<UK2Node_GetArrayItem>(Graph, Position);
    }
    else if (NodeType == TEXT("select"))
    {
        // A wildcard Select with the built-in 2 options, grown to option_count, or driven by an enum
        // (enum_path, alias index_type) that makes the Index pin that enum with one option per entry.
        FString IndexEnumPath;
        const bool bHasEnumPath = Params->TryGetStringField(TEXT("enum_path"), IndexEnumPath) && !IndexEnumPath.IsEmpty();
        if (!bHasEnumPath)
        {
            Params->TryGetStringField(TEXT("index_type"), IndexEnumPath);
        }
        UEnum* IndexEnum = nullptr;
        if (!IndexEnumPath.IsEmpty())
        {
            IndexEnum = FUEAgentGraphUtils::ResolveEnum(IndexEnumPath);
            if (!IndexEnum)
            {
                return mcp_common::MakeErrorReply(FString::Printf(TEXT("Could not resolve enum for the index pin: %s"), *IndexEnumPath));
            }
        }
        int32 OptionCount = 0;
        double OptionCountD = 0.0;
        const bool bHasOptionCount = Params->TryGetNumberField(TEXT("option_count"), OptionCountD);
        if (bHasOptionCount)
        {
            OptionCount = (int32)OptionCountD;
            if (OptionCountD != (double)OptionCount || OptionCount < 2 || OptionCount > 128)
            {
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("option_count must be a whole number from 2 to 128 (a Select starts with 2 option pins); got %g"), OptionCountD));
            }
        }

        UK2Node_Select* SelectNode = UEAgentGraphCommandsImpl::SpawnNode<UK2Node_Select>(Graph, Position);
        if (IndexEnum)
        {
            SelectNode->SetEnum(IndexEnum, /*bForceRegenerate=*/true);
        }
        else if (OptionCount > 0)
        {
            // AddInputPin is the AddPin interface; it grows one option pin at a time.
            TArray<UEdGraphPin*> GrowingPins;
            SelectNode->GetOptionPins(GrowingPins);
            int32 Guard = 0;
            while (GrowingPins.Num() < OptionCount && SelectNode->CanAddPin() && Guard++ < 128)
            {
                SelectNode->AddInputPin();
                GrowingPins.Reset();
                SelectNode->GetOptionPins(GrowingPins);
            }
        }
        if (UEdGraphPin* IndexPin = SelectNode->GetIndexPin())
        {
            Extra->SetStringField(TEXT("index_pin"), IndexPin->PinName.ToString());
        }
        if (UEdGraphPin* ReturnPin = SelectNode->GetReturnValuePin())
        {
            Extra->SetStringField(TEXT("return_value_pin"), ReturnPin->PinName.ToString());
        }
        TArray<UEdGraphPin*> OptionPins;
        SelectNode->GetOptionPins(OptionPins);
        TArray<TSharedPtr<FJsonValue>> OptionNames;
        for (UEdGraphPin* OptionPin : OptionPins)
        {
            if (OptionPin) { OptionNames.Add(MakeShared<FJsonValueString>(OptionPin->PinName.ToString())); }
        }
        Extra->SetArrayField(TEXT("option_pins"), OptionNames);
        Extra->SetNumberField(TEXT("option_count"), OptionPins.Num());
        if (IndexEnum)
        {
            Extra->SetStringField(TEXT("index_enum"), IndexEnum->GetPathName());
        }
        NewNode = SelectNode;
    }
    else if (NodeType == TEXT("make_array") || NodeType == TEXT("make_set") || NodeType == TEXT("make_map"))
    {
        // All derive UK2Node_MakeContainer (abstract).
        int32 NumInputs = 0;
        {
            double NumInputsD = 0.0;
            if (Params->TryGetNumberField(TEXT("num_inputs"), NumInputsD)) { NumInputs = (int32)NumInputsD; }
        }
        if (NodeType == TEXT("make_array"))
        {
            NewNode = UEAgentGraphCommandsImpl::SpawnMakeContainerNode<UK2Node_MakeArray>(Graph, Position, NumInputs);
        }
        else if (NodeType == TEXT("make_set"))
        {
            NewNode = UEAgentGraphCommandsImpl::SpawnMakeContainerNode<UK2Node_MakeSet>(Graph, Position, NumInputs);
        }
        else // make_map
        {
            NewNode = UEAgentGraphCommandsImpl::SpawnMakeContainerNode<UK2Node_MakeMap>(Graph, Position, NumInputs);
        }
    }
    else if (NodeType == TEXT("switch_int") || NodeType == TEXT("switch_string") || NodeType == TEXT("switch_name"))
    {
        // Base UK2Node_Switch is abstract; AddPinToSwitchNode() is exported on the
        // concrete subclasses and safe before or after Finalize().
        const TArray<FString> Cases = ReadStringArray(Params, TEXT("cases"));
        const auto AddCasePinNames = [&Cases](auto* NameSwitch)
        {
            for (const FString& C : Cases) { NameSwitch->PinNames.Add(FName(*C)); }
        };
        if (NodeType == TEXT("switch_int"))
        {
            UK2Node_SwitchInteger* N = UEAgentGraphCommandsImpl::SpawnNode<UK2Node_SwitchInteger>(Graph, Position);
            for (int32 i = 0; i < Cases.Num(); ++i) { N->AddPinToSwitchNode(); }
            NewNode = N;
        }
        else if (NodeType == TEXT("switch_string"))
        {
            NewNode = UEAgentGraphCommandsImpl::SpawnNode<UK2Node_SwitchString>(Graph, Position, AddCasePinNames);
        }
        else // switch_name
        {
            NewNode = UEAgentGraphCommandsImpl::SpawnNode<UK2Node_SwitchName>(Graph, Position, AddCasePinNames);
        }
    }
    else if (NodeType == TEXT("switch_enum") || NodeType == TEXT("enum_literal"))
    {
        FString EnumPath;
        const bool bHasEnumPath = Params->TryGetStringField(TEXT("enum_path"), EnumPath);
        if (!bHasEnumPath)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("%s node requires 'enum_path'"), *NodeType));
        }
        UEnum* Enum = FUEAgentGraphUtils::ResolveEnum(EnumPath);
        if (!Enum)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Could not resolve enum_path: %s"), *EnumPath));
        }
        if (NodeType == TEXT("switch_enum"))
        {
            // The cases derive from the enum.
            NewNode = UEAgentGraphCommandsImpl::SpawnNode<UK2Node_SwitchEnum>(Graph, Position,
                [Enum](UK2Node_SwitchEnum* EnumSwitch) { EnumSwitch->SetEnum(Enum); });
        }
        else
        {
            NewNode = UEAgentGraphCommandsImpl::SpawnNode<UK2Node_EnumLiteral>(Graph, Position,
                [Enum](UK2Node_EnumLiteral* EnumLiteral) { EnumLiteral->Enum = Enum; });
        }
    }
    else if (NodeType == TEXT("format_text"))
    {
        // Zero-config spawn = node with just the Format pin (valid). Argument pins
        // can only be added AFTER the node is in the graph (post-Finalize).
        const TArray<FString> Arguments = ReadStringArray(Params, TEXT("arguments"));
        UK2Node_FormatText* Node = UEAgentGraphCommandsImpl::SpawnNode<UK2Node_FormatText>(Graph, Position);
        for (int32 i = 0; i < Arguments.Num(); ++i)
        {
            Node->AddArgumentPin();
            Node->SetArgumentName(i, FName(*Arguments[i]));
        }
        NewNode = Node;
    }
    // --- Macro-instance node types from StandardMacros -----------------------
    else if (const TCHAR* StandardMacroName = UEAgentGraphCommandsImpl::FindStandardMacroAlias(NodeType))
    {
        const FString MacroGraphName = StandardMacroName;
        UBlueprint* MacroLib = UEAgentGraphCommandsImpl::LoadStandardMacros();
        if (!MacroLib)
        {
            return mcp_common::MakeErrorReply(
                TEXT("Could not load StandardMacros library (/Engine/EditorBlueprintResources/StandardMacros.StandardMacros)"));
        }
        UEdGraph* MacroGraph = UEAgentGraphCommandsImpl::FindMacroGraph(MacroLib, MacroGraphName);
        if (!MacroGraph)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Macro graph '%s' not found in StandardMacros"), *MacroGraphName));
        }
        UE_LOG(LogTemp, Verbose, TEXT("[MCP] add_graph_node: macro_instance '%s' -> '%s'"), *NodeType, *MacroGraphName);
        NewNode = UEAgentGraphCommandsImpl::SpawnNode<UK2Node_MacroInstance>(Graph, Position,
            [MacroGraph](UK2Node_MacroInstance* MacroNode) { MacroNode->SetMacroGraph(MacroGraph); });
    }
    // --- Bind to an event dispatcher (multicast delegate) --------------------
    else if (NodeType == TEXT("add_delegate"))
    {
        FString DispatcherName;
        const bool bHasDispatcher = Params->TryGetStringField(TEXT("dispatcher"), DispatcherName);
        if (!bHasDispatcher)
        {
            return mcp_common::MakeErrorReply(TEXT("add_delegate node requires 'dispatcher'"));
        }
        // The dispatcher must be a compiled FMulticastDelegateProperty (add_event_dispatcher + compile).
        UClass* OwnerClass = Blueprint->SkeletonGeneratedClass;
        FMulticastDelegateProperty* DProp = OwnerClass
            ? FindFProperty<FMulticastDelegateProperty>(OwnerClass, FName(*DispatcherName)) : nullptr;
        if (!DProp && Blueprint->GeneratedClass)
        {
            OwnerClass = Blueprint->GeneratedClass;
            DProp = FindFProperty<FMulticastDelegateProperty>(OwnerClass, FName(*DispatcherName));
        }
        if (!DProp)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Dispatcher '%s' not found or blueprint not compiled"), *DispatcherName));
        }
        UE_LOG(LogTemp, Verbose, TEXT("[MCP] add_graph_node: add_delegate -> '%s'"), *DispatcherName);
        NewNode = UEAgentGraphCommandsImpl::SpawnNode<UK2Node_AddDelegate>(Graph, Position,
            [DProp, OwnerClass](UK2Node_AddDelegate* BindNode) { BindNode->SetFromProperty(DProp, /*bSelfContext=*/true, OwnerClass); });
    }
    // --- create_widget: UK2Node_CreateWidget (UMGEditor). Unlike UWidgetBlueprintLibrary::Create,
    // this node EXPOSES the widget's Expose-On-Spawn pins (e.g. "Categories") which the caller must set
    // BEFORE Event Construct. The node class is MinimalAPI with a PRIVATE header, so it is resolved by
    // /Script path and constructed by class, then driven through its PUBLIC base
    // UK2Node_ConstructObjectFromClass (GetClassPin + ReconstructNode) -- no private include, no Build.cs dep.
    else if (NodeType == TEXT("create_widget"))
    {
        FString TargetClassName;
        const bool bHasTargetClass = Params->TryGetStringField(TEXT("target_class"), TargetClassName);
        if (!bHasTargetClass || TargetClassName.IsEmpty())
        {
            return mcp_common::MakeErrorReply(TEXT("create_widget node requires 'target_class' (the widget class, e.g. WBP_Foo_C or a full asset path)"));
        }
        UClass* WidgetClass = ResolveTargetClassFlexible(TargetClassName);
        if (!WidgetClass)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("create_widget: widget class not found: %s (try a full asset path or the generated ..._C class)"), *TargetClassName));
        }
        if (!WidgetClass->IsChildOf(UUserWidget::StaticClass()))
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("create_widget: '%s' is not a UUserWidget subclass"), *WidgetClass->GetName()));
        }
        // Resolve the node class by /Script path (private header avoided). ResolveClass loads the native
        // UClass for /Script/UMGEditor.K2Node_CreateWidget when the editor UMGEditor module is present.
        UClass* NodeClass = FUEAgentGraphUtils::ResolveClass(TEXT("/Script/UMGEditor.K2Node_CreateWidget"));
        if (!NodeClass)
        {
            return mcp_common::MakeErrorReply(TEXT("create_widget: UK2Node_CreateWidget class unavailable (UMGEditor not loaded)"));
        }
        // Manual creation with pins allocated FIRST, then PostPlacedNewNode (same crash-safe order as
        // spawn_actor). UK2Node_ConstructObjectFromClass::PostPlacedNewNode is a no-op until a class is
        // set on the Class pin, so calling it before setting the class is safe.
        UK2Node_ConstructObjectFromClass* Node = Cast<UK2Node_ConstructObjectFromClass>(
            NewObject<UEdGraphNode>(Graph, NodeClass));
        if (!Node)
        {
            return mcp_common::MakeErrorReply(TEXT("create_widget: failed to construct UK2Node_CreateWidget node"));
        }
        UEAgentGraphCommandsImpl::PlaceNodePinsFirst(Graph, Node, Position);
        if (UEdGraphPin* ClassPin = Node->GetClassPin())
        {
            ClassPin->DefaultObject = WidgetClass;
            Node->ReconstructNode(); // rebuilds pins incl. the widget's Expose-On-Spawn inputs
        }
        NewNode = Node;
    }
    // --- Generic macro-instance node. Resolves against the Blueprint's OWN
    // MacroGraphs first (so create_blueprint_macro's output is actually reachable --
    // the 8 aliases above only ever look in StandardMacros), falling back to
    // StandardMacros if no match there (so e.g. "ForEachLoop" still works without
    // requiring macro_source="standard_macros"). ---
    else if (NodeType == TEXT("macro_instance"))
    {
        FString MacroGraphName;
        const bool bHasMacroGraphName = Params->TryGetStringField(TEXT("macro_graph_name"), MacroGraphName);
        if (!bHasMacroGraphName || MacroGraphName.IsEmpty())
        {
            return mcp_common::MakeErrorReply(TEXT("macro_instance node requires 'macro_graph_name'"));
        }
        FString MacroSource;
        Params->TryGetStringField(TEXT("macro_source"), MacroSource);
        FString MacroLibrary;
        Params->TryGetStringField(TEXT("macro_library"), MacroLibrary);

        UEdGraph* MacroGraph = nullptr;
        int32 LibrarySeparator = INDEX_NONE;
        const bool bLibrarySpec = MacroGraphName.FindLastChar(TEXT(':'), LibrarySeparator) && LibrarySeparator > 1;
        if (!MacroLibrary.IsEmpty() || bLibrarySpec)
        {
            // A macro LIBRARY asset: macro_library + macro_graph_name, or "<library-path>:<MacroGraphName>".
            FString ResolveError;
            MacroGraph = MacroLibrary.IsEmpty()
                ? ResolveMacroInstanceGraph(Blueprint, MacroGraphName, FString(), ResolveError)
                : ResolveMacroInstanceGraph(Blueprint, MacroLibrary, MacroGraphName, ResolveError);
            if (!MacroGraph)
            {
                return mcp_common::MakeErrorReply(ResolveError.IsEmpty() ? FString(TEXT("Could not resolve macro graph")) : ResolveError);
            }
        }
        else
        {
            if (!MacroSource.Equals(TEXT("standard_macros"), ESearchCase::IgnoreCase))
            {
                MacroGraph = UEAgentGraphCommandsImpl::FindMacroGraph(Blueprint, MacroGraphName);
            }
            if (!MacroGraph)
            {
                MacroGraph = UEAgentGraphCommandsImpl::FindMacroGraph(UEAgentGraphCommandsImpl::LoadStandardMacros(), MacroGraphName);
            }
            if (!MacroGraph)
            {
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("Macro graph '%s' not found in this Blueprint's MacroGraphs or in StandardMacros (for a macro library pass macro_library)"), *MacroGraphName));
            }
        }

        UK2Node_MacroInstance* MacroInstance = UEAgentGraphCommandsImpl::SpawnNode<UK2Node_MacroInstance>(Graph, Position,
            [MacroGraph](UK2Node_MacroInstance* MacroNode) { MacroNode->SetMacroGraph(MacroGraph); });
        // The pin names split by direction.
        TArray<TSharedPtr<FJsonValue>> InputPins;
        TArray<TSharedPtr<FJsonValue>> OutputPins;
        for (UEdGraphPin* Pin : MacroInstance->Pins)
        {
            if (!Pin) { continue; }
            const FString PinName = Pin->PinName.ToString();
            if (Pin->Direction == EGPD_Input)       { InputPins.Add(MakeShared<FJsonValueString>(PinName)); }
            else if (Pin->Direction == EGPD_Output) { OutputPins.Add(MakeShared<FJsonValueString>(PinName)); }
        }
        Extra->SetArrayField(TEXT("input_pins"), InputPins);
        Extra->SetArrayField(TEXT("output_pins"), OutputPins);
        NewNode = MacroInstance;
    }
    // --- GetComponentByClass born typed: the ComponentClass pin is set before the node is
    // reconstructed, so its DeterminesOutputType ReturnValue comes out as that class. ---
    else if (NodeType == TEXT("get_component_by_class"))
    {
        FString ComponentClassName;
        const bool bHasComponentClass = Params->TryGetStringField(TEXT("target_class"), ComponentClassName);
        if (!bHasComponentClass || ComponentClassName.IsEmpty())
        {
            return mcp_common::MakeErrorReply(TEXT("get_component_by_class node requires 'target_class' (the component class to fetch)"));
        }
        UClass* ComponentClass = FUEAgentGraphUtils::ResolveClass(ComponentClassName);
        if (!ComponentClass || !ComponentClass->IsChildOf(UActorComponent::StaticClass()))
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("target_class '%s' is not a valid UActorComponent subclass"), *ComponentClassName));
        }
        UFunction* GetComponentFunction = AActor::StaticClass()->FindFunctionByName(FName(TEXT("GetComponentByClass")));
        if (!GetComponentFunction)
        {
            return mcp_common::MakeErrorReply(TEXT("AActor::GetComponentByClass not found (engine)"));
        }
        UK2Node_CallFunction* CallNode = mcp_common::CreateFunctionCallNode(Graph, GetComponentFunction, Position);
        if (!CallNode)
        {
            return mcp_common::MakeErrorReply(TEXT("Failed to create GetComponentByClass node"));
        }
        if (UEdGraphPin* ClassPin = CallNode->FindPin(TEXT("ComponentClass"), EGPD_Input))
        {
            ClassPin->DefaultObject = ComponentClass;
            CallNode->ReconstructNode();
        }
        FString ReturnPinType;
        if (UEdGraphPin* ReturnPin = CallNode->GetReturnValuePin())
        {
            ReturnPinType = ReturnPin->PinType.PinSubCategoryObject.IsValid()
                ? ReturnPin->PinType.PinSubCategoryObject->GetName()
                : ReturnPin->PinType.PinCategory.ToString();
        }
        Extra->SetStringField(TEXT("component_class"), ComponentClass->GetPathName());
        Extra->SetStringField(TEXT("return_pin_type"), ReturnPinType);
        NewNode = CallNode;
    }
    else
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Unsupported node_type '%s'. Supported: event, custom_event, call_function, print_string, variable_get, variable_set, branch, sequence, self, cast, spawn_actor, create_widget, make_struct, break_struct, get_array_item, select, make_array, make_set, make_map, switch_int, switch_string, switch_name, switch_enum, enum_literal, format_text, foreach, foreach_with_break, do_once, gate, flipflop, for_loop, while_loop, is_valid, add_delegate, macro_instance, get_component_by_class"),
            *NodeType));
    }

    if (!NewNode)
    {
        return mcp_common::MakeErrorReply(ErrorMessage.IsEmpty() ? TEXT("Failed to create node") : ErrorMessage);
    }

    // Guarantee an addressable identity. A node made without FGraphNodeCreator can keep an
    // all-zero NodeGuid. Without a real GUID the returned node_id is 000...0, so a later
    // connect_pins / delete_node (which resolve nodes by NodeGuid) cannot reference
    // this node. Assign one now so the returned node_id -- and get_graph_nodes -- report
    // a usable, unique GUID. No-op when the node already has a valid GUID.
    if (!NewNode->NodeGuid.IsValid())
    {
        NewNode->CreateNewGuid();
    }

    if (bCompile)
    {
        UEAgentGraphCommandsImpl::MarkModifiedAndCompile(Blueprint, Graph);
    }
    else
    {
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        Graph->NotifyGraphChanged();
    }

    // An existing event override may live on another page of the Blueprint: report the graph it is in.
    const UEdGraph* NodeGraph = NewNode->GetGraph() ? NewNode->GetGraph() : Graph;
    TSharedPtr<FJsonObject> Result = UEAgentGraphCommandsImpl::NodeReply(NewNode, BlueprintName, NodeGraph);
    for (const auto& Field : Extra->Values)
    {
        Result->SetField(Field.Key, Field.Value);
    }
    Result->SetBoolField(TEXT("compiled"), bCompile);
    return Result;
}

// MCP:connect_pins - Wire one node's OUTPUT pin to another node's INPUT pin inside one Blueprint graph, type-checked by the K2 schema (an incompatible pair is returned as an error, never a silent no-op). Nodes are addressed by NodeGuid, pins by name. Either node that still carries a WILDCARD pin is reconstructed afterwards, so array / ForEach / Select pins really re-type instead of compiling into "The type of Target Array is undetermined" (which compile_blueprint reports as compiled:true). Use validate_pin_connection to test a pair without mutating, and build_blueprint_graph to create+wire in one transaction.
//   @param blueprint_name - Blueprint name or path.
//   @param source_node_id - NodeGuid of the source node (from add_graph_node / get_graph_nodes).
//   @param source_pin - Output pin name on the source node (e.g. "then", "ReturnValue"). An output pin of that name is preferred; any pin with the name is accepted as a fallback.
//   @param target_node_id - NodeGuid of the target node.
//   @param target_pin - Input pin name on the target node (e.g. "execute", "Condition").
//   @param graph_name - Target graph name; defaults to the Blueprint's event graph when omitted. BOTH nodes must live in this same graph.
//   @returns { connected, source_node_id, target_node_id } -- connected is always true when the call succeeds; the failure path is an error response, so there is no false to check.
TSharedPtr<FJsonObject> HandleConnectPins(const TSharedPtr<FJsonObject>& Params)
{
    UEAgentGraphCommandsImpl::FPinPair Pair;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::ReadPinPair(Params, Pair)) { return Error; }

    const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
    const FPinConnectionResponse CanConnect = Schema->CanCreateConnection(Pair.SourcePin, Pair.TargetPin);
    if (CanConnect.Response == CONNECT_RESPONSE_DISALLOW)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Cannot connect pins: %s"), *CanConnect.Message.ToString()));
    }

    FScopedTransaction Transaction(LOCTEXT("ConnectPins", "MCP: Connect Pins"));
    Pair.Blueprint->Modify();
    Pair.Graph->Modify();

    // Capture owning NODES before connecting: ReconstructNode() below reallocates pins, so the two pins
    // become dangling afterwards -- the node objects survive, the pins do not.
    UEdGraphNode* NodeA = Pair.SourcePin->GetOwningNode();
    UEdGraphNode* NodeB = Pair.TargetPin->GetOwningNode();

    const bool bConnected = Schema->TryCreateConnection(Pair.SourcePin, Pair.TargetPin);
    if (!bConnected)
    {
        return mcp_common::MakeErrorReply(TEXT("TryCreateConnection failed"));
    }
    // WARNING: the two pins may be invalidated by ReconstructNode() below -- do NOT use them past here.

    // Wildcard-resolving nodes (UK2Node_GetArrayItem, ForEach macros / UK2Node_MacroInstance, and
    // UI-placed UK2Node_CallArrayFunction / UK2Node_Select) only re-type their wildcard pins to the
    // connected concrete type on a node RECONSTRUCT. TryCreateConnection fires PinConnectionListChanged
    // but through this path it does not reliably stick, leaving the pin PC_Wildcard -> compiles to
    // "The type of Target Array is undetermined" and blocks PIE (compile_blueprint still reports
    // compiled:true -- validate with compile_blueprint(return_messages=true)). ReconstructNode() re-derives the pins with
    // the link already in place (RewireOldPinsToNewPins restores the connection + pin defaults by name),
    // which is exactly the reopen-fixes-it behavior. Mirror the interactive editor, but ONLY for a node
    // that still carries a wildcard pin, so ordinary connects skip the reconstruct.
    auto ReconstructIfHasWildcardPin = [](UEdGraphNode* Node)
    {
        if (!Node) { return; }
        for (const UEdGraphPin* P : Node->Pins)
        {
            if (P && P->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
            {
                Node->ReconstructNode();
                return;
            }
        }
    };
    ReconstructIfHasWildcardPin(NodeA);
    if (NodeB != NodeA) { ReconstructIfHasWildcardPin(NodeB); }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Pair.Blueprint);
    Pair.Graph->NotifyGraphChanged();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("connected"), true);
    Result->SetStringField(TEXT("source_node_id"), Pair.SourceNodeId);
    Result->SetStringField(TEXT("target_node_id"), Pair.TargetNodeId);
    return Result;
}

// MCP:disconnect_pin - Break ALL links on one pin of one node (there is no way to drop a single link of a multi-linked pin). Main use: clearing links that point at nodes which no longer exist after a failed or refused build_blueprint_graph rollback -- get_graph_nodes with compact=false shows those as linked_to entries with dead GUIDs. To remove the whole node instead, use delete_node.
//   @param blueprint_name - Blueprint asset name or path
//   @param node_id - NodeGuid of the node owning the pin (from add_graph_node / get_graph_nodes)
//   @param pin_name - Pin name, in either direction
//   @param graph_name - Target graph name; defaults to the Blueprint's event graph when omitted
//   @returns { disconnected, node_id, pin_name }
TSharedPtr<FJsonObject> HandleDisconnectPin(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, NodeId, PinName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("node_id"), NodeId, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("pin_name"), PinName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UBlueprint* Blueprint = nullptr;
    UEdGraph* Graph = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::ResolveBlueprintGraph(Params, BlueprintName, Blueprint, Graph)) { return Error; }

    UEdGraphNode* Node = nullptr;
    if (TSharedPtr<FJsonObject> Error = GraphNodePinLookup::FindNode(Graph, NodeId, Node)) { return Error; }
    UEdGraphPin* Pin = nullptr;
    if (TSharedPtr<FJsonObject> Error = GraphNodePinLookup::FindPin(Node, PinName, EGPD_MAX, Pin)) { return Error; }

    FScopedTransaction Transaction(LOCTEXT("DisconnectPin", "MCP: Disconnect Pin"));
    Blueprint->Modify();
    Graph->Modify();
    Pin->BreakAllPinLinks();
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    Graph->NotifyGraphChanged();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("disconnected"), true);
    Result->SetStringField(TEXT("node_id"), NodeId);
    Result->SetStringField(TEXT("pin_name"), PinName);
    return Result;
}

// MCP:set_pin_default_value - Write the literal default of an UNCONNECTED input pin and return the value read BACK off the pin. Struct pins are canonicalized: a bare GameplayTag name becomes (TagName="X") and an UNREGISTERED tag is rejected up front instead of silently landing as an empty tag; value_json nativizes any struct (and is the only way to put SEVERAL tags in a GameplayTagContainer). Array/set/map pins are refused (use a MakeArray/MakeSet/MakeMap node) and so are pins currently split into sub-pins (recombine_blueprint_pin first). A plain string is fine for an FText pin. TRAP: on a FormatText "Format" pin the literal IS written but the {argument} pins are NOT created, leaving a valid, useless node -- build the string with Concat_StrStr / Conv_StringToText instead.
//   @param blueprint_name - Blueprint asset name or path
//   @param node_id - NodeGuid of the node owning the pin
//   @param pin_name - Pin name; an INPUT pin of that name is preferred, any pin with the name is the fallback
//   @param value - Literal default in the pin's exported-text form; empty string allowed (clears it). For a struct pin, "()" clears and a leading "(" is passed through untouched.
//   @param value_json - Optional JSON object nativized into the struct's export-text form, for STRUCT pins only. For an FGameplayTagContainer it must carry a "GameplayTags" array (["A","B"] or [{"TagName":"A"}]) and every tag is validated; takes precedence over 'value'.
//   @param graph_name - Target graph name; defaults to the Blueprint's event graph when omitted
//   @returns { node_id, pin_name, default_value } -- default_value is re-read from the pin AFTER the write, so it is the oracle for what actually landed (a rejected value shows up as the old one).
TSharedPtr<FJsonObject> HandleSetPinDefaultValue(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, NodeId, PinName, Value;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("node_id"), NodeId, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("pin_name"), PinName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    Params->TryGetStringField(TEXT("value"), Value); // empty allowed

    UBlueprint* Blueprint = nullptr;
    UEdGraph* Graph = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::ResolveBlueprintGraph(Params, BlueprintName, Blueprint, Graph)) { return Error; }

    UEdGraphNode* Node = nullptr;
    if (TSharedPtr<FJsonObject> Error = GraphNodePinLookup::FindNode(Graph, NodeId, Node)) { return Error; }
    UEdGraphPin* Pin = nullptr;
    if (TSharedPtr<FJsonObject> Error = GraphNodePinLookup::FindPin(Node, PinName, EGPD_Input, Pin)) { return Error; }

    // For a struct-category pin, K2 stores the default as the struct's EXPORTED-TEXT
    // form (e.g. (TagName="X") for an FGameplayTag). A bare token can already resolve on
    // import, but canonicalizing it makes it round-trip unambiguously AND lets us reject an
    // unregistered tag up front. An optional value_json object nativizes ANY struct into its
    // export-text form. All of this is gated strictly behind PC_Struct so every scalar/enum/
    // object pin takes the plain raw-string code path.
    FString FinalValue = Value;
    if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
    {
        if (Pin->PinType.IsContainer())
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Pin '%s' is a container (array/set/map); use a MakeArray/MakeSet/MakeMap node, not a pin default"), *PinName));
        }
        if (Pin->SubPins.Num() > 0)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Pin '%s' is split into sub-pins; recombine it (recombine_blueprint_pin) or set each sub-pin individually"), *PinName));
        }

        UScriptStruct* SS = Cast<UScriptStruct>(Pin->PinType.PinSubCategoryObject.Get());
        const TSharedPtr<FJsonObject>* ValueObj = nullptr;
        const bool bHasJson = Params->TryGetObjectField(TEXT("value_json"), ValueObj) && ValueObj && (*ValueObj).IsValid();
        const bool bAlreadyStructText = Value.StartsWith(TEXT("(")); // pre-serialized / '()' clear passes through

        // FGameplayTagContainer literal from value_json. The container has custom struct
        // (de)serialization that FJsonObjectConverter does NOT round-trip reliably, so build a real
        // container from an explicit "GameplayTags" list, validate every tag, then ExportText it into
        // the canonical pin-default form. Accepts both {"GameplayTags":["A","B"]} and the wrapper
        // {"GameplayTags":[{"TagName":"A"},...]}. This is checked BEFORE the generic value_json path.
        if (bHasJson && SS == FGameplayTagContainer::StaticStruct())
        {
            const TArray<TSharedPtr<FJsonValue>>* TagArr = nullptr;
            if (!(*ValueObj)->TryGetArrayField(TEXT("GameplayTags"), TagArr) || !TagArr)
            {
                return mcp_common::MakeErrorReply(
                    TEXT("value_json for a GameplayTagContainer must contain a \"GameplayTags\" array"));
            }

            FGameplayTagContainer Container;
            for (const TSharedPtr<FJsonValue>& Elem : *TagArr)
            {
                if (!Elem.IsValid()) { continue; }

                FString TagStr;
                if (!Elem->TryGetString(TagStr))
                {
                    // Wrapper form: an object with a "TagName" string field.
                    const TSharedPtr<FJsonObject>* ElemObj = nullptr;
                    if (Elem->TryGetObject(ElemObj) && ElemObj && (*ElemObj).IsValid())
                    {
                        (*ElemObj)->TryGetStringField(TEXT("TagName"), TagStr);
                    }
                }
                if (TagStr.IsEmpty()) { continue; }

                const FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*TagStr), /*ErrorIfNotFound=*/false);
                if (!Tag.IsValid())
                {
                    return mcp_common::MakeErrorReply(FString::Printf(
                        TEXT("Unknown GameplayTag '%s' (register it first, e.g. via create_gameplay_tag)"), *TagStr));
                }
                Container.AddTag(Tag);
            }

            FGameplayTagContainer::StaticStruct()->ExportText(FinalValue, &Container, &Container, nullptr, PPF_None, nullptr);
        }
        else if (bHasJson && SS)
        {
            FStructOnScope Tmp(SS);
            if (!FJsonObjectConverter::JsonObjectToUStruct((*ValueObj).ToSharedRef(), SS, Tmp.GetStructMemory(), 0, 0))
            {
                return mcp_common::MakeErrorReply(FString::Printf(TEXT("Could not nativize value_json into struct %s"), *SS->GetName()));
            }
            SS->ExportText(FinalValue, Tmp.GetStructMemory(), Tmp.GetStructMemory(), nullptr, PPF_None, nullptr);
        }
        else if (SS == FGameplayTag::StaticStruct() && !bAlreadyStructText)
        {
            if (Value.IsEmpty())
            {
                FinalValue = TEXT("()"); // clear the tag
            }
            else
            {
                const FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*Value), /*ErrorIfNotFound=*/false);
                if (!Tag.IsValid())
                {
                    return mcp_common::MakeErrorReply(FString::Printf(
                        TEXT("Unknown GameplayTag '%s' (register it first, e.g. via create_gameplay_tag)"), *Value));
                }
                FinalValue = FString::Printf(TEXT("(TagName=\"%s\")"), *Tag.GetTagName().ToString());
            }
        }
        else if (SS == FGameplayTagContainer::StaticStruct() && !bAlreadyStructText && !Value.IsEmpty())
        {
            const FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*Value), /*ErrorIfNotFound=*/false);
            if (!Tag.IsValid())
            {
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("Unknown GameplayTag '%s' (for multiple tags use value_json)"), *Value));
            }
            FinalValue = FString::Printf(TEXT("(GameplayTags=((TagName=\"%s\")))"), *Tag.GetTagName().ToString());
        }
    }

    FScopedTransaction Transaction(LOCTEXT("SetPinDefault", "MCP: Set Pin Default"));
    Blueprint->Modify();
    Node->Modify();
    const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
    Schema->TrySetDefaultValue(*Pin, FinalValue);
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("node_id"), NodeId);
    Result->SetStringField(TEXT("pin_name"), PinName);
    Result->SetStringField(TEXT("default_value"), Pin->GetDefaultAsString());
    return Result;
}

// MCP:delete_node - Delete ONE node from a graph by its NodeGuid, breaking its links; refused for nodes the editor protects (FunctionEntry, Result, tunnels). This is the cleanup for orphan nodes left behind when a build_blueprint_graph batch fails and its rollback is refused, and it is reversible (the .uasset on disk is untouched until save_asset). It does NOT compile -- follow with compile_blueprint with return_messages=true.
//   @param blueprint_name - Blueprint asset name or path
//   @param node_id - NodeGuid of the node to delete
//   @param graph_name - Target graph name; defaults to the Blueprint's event graph when omitted
//   @returns { deleted, node_id }
TSharedPtr<FJsonObject> HandleDeleteNode(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, NodeId;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("node_id"), NodeId, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UBlueprint* Blueprint = nullptr;
    UEdGraph* Graph = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::ResolveBlueprintGraph(Params, BlueprintName, Blueprint, Graph)) { return Error; }

    UEdGraphNode* Node = nullptr;
    if (TSharedPtr<FJsonObject> Error = GraphNodePinLookup::FindNode(Graph, NodeId, Node)) { return Error; }
    if (!Node->CanUserDeleteNode())
        return mcp_common::MakeErrorReply(TEXT("Node cannot be deleted"));

    FScopedTransaction Transaction(LOCTEXT("DeleteNode", "MCP: Delete Node"));
    Blueprint->Modify();
    Graph->Modify();
    FBlueprintEditorUtils::RemoveNode(Blueprint, Node, true);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    Graph->NotifyGraphChanged();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("deleted"), true);
    Result->SetStringField(TEXT("node_id"), NodeId);
    return Result;
}

// MCP:move_node - Move a node to a new [x, y] canvas position. Purely cosmetic layout: no pin, link, default or type changes, and no compile. Returns the position actually applied.
//   @param blueprint_name - Blueprint asset name or path
//   @param node_id - NodeGuid of the node to move
//   @param graph_name - Target graph name; defaults to the Blueprint's event graph when omitted
//   @param position_x - Node X position on the graph canvas (ignored when node_position is set)
//   @param position_y - Node Y position on the graph canvas (ignored when node_position is set)
//   @param node_position - Node canvas position as [x, y]; takes precedence over position_x/position_y
//   @returns { node_id, pos_x, pos_y }
TSharedPtr<FJsonObject> HandleMoveNode(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, NodeId;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("node_id"), NodeId, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UBlueprint* Blueprint = nullptr;
    UEdGraph* Graph = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::ResolveBlueprintGraph(Params, BlueprintName, Blueprint, Graph)) { return Error; }

    UEdGraphNode* Node = nullptr;
    if (TSharedPtr<FJsonObject> Error = GraphNodePinLookup::FindNode(Graph, NodeId, Node)) { return Error; }

    const FVector2D Position = ReadPosition(Params);
    FScopedTransaction Transaction(LOCTEXT("MoveNode", "MCP: Move Node"));
    Node->Modify();
    Node->NodePosX = Position.X;
    Node->NodePosY = Position.Y;
    Graph->NotifyGraphChanged();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("node_id"), NodeId);
    Result->SetNumberField(TEXT("pos_x"), Node->NodePosX);
    Result->SetNumberField(TEXT("pos_y"), Node->NodePosY);
    return Result;
}

// MCP:add_comment_node - Add a resizable comment box (UEdGraphNode_Comment) with text to a graph, in one undo step. The text is written after the engine places the node (its PostPlacedNewNode resets the box to "Comment"), and the returned "comment" field is the box's text read back; without text the box keeps the engine's "Comment". It has NO pins, so a pin_defaults entry aimed at it breaks a whole build_blueprint_graph batch. Read back: get_graph_nodes (compact=false: compact skips comment boxes); remove: delete_node.
//   @param blueprint_name - Blueprint asset name or path
//   @param text - Comment text shown in the box; omitted or empty leaves the engine default "Comment"
//   @param graph_name - Target graph name; defaults to the Blueprint's event graph when omitted
//   @param width - Comment box width in pixels; defaults to 400
//   @param height - Comment box height in pixels; defaults to 200
//   @param position_x - Node X position on the graph canvas (ignored when node_position is set)
//   @param position_y - Node Y position on the graph canvas (ignored when node_position is set)
//   @param node_position - Node canvas position as [x, y]; takes precedence over position_x/position_y
//   @returns { node_id, class, title, pos_x, pos_y, comment } -- no pins array; comment is the text the box holds, read back after placement.
TSharedPtr<FJsonObject> HandleAddCommentNode(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, Text;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    Params->TryGetStringField(TEXT("text"), Text);

    UBlueprint* Blueprint = nullptr;
    UEdGraph* Graph = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::ResolveBlueprintGraph(Params, BlueprintName, Blueprint, Graph)) { return Error; }

    const FVector2D Position = ReadPosition(Params);
    double Width = 400.0, Height = 200.0;
    Params->TryGetNumberField(TEXT("width"), Width);
    Params->TryGetNumberField(TEXT("height"), Height);

    FScopedTransaction Transaction(LOCTEXT("AddComment", "MCP: Add Comment Node"));
    Blueprint->Modify();
    Graph->Modify();

    UEdGraphNode_Comment* Node = UEAgentGraphCommandsImpl::SpawnNode<UEdGraphNode_Comment>(Graph, Position,
        [Width, Height](UEdGraphNode_Comment* CommentNode)
        {
            CommentNode->NodeWidth = (int32)Width;
            CommentNode->NodeHeight = (int32)Height;
        });
    // PostPlacedNewNode (run by Finalize) writes the default "Comment" into NodeComment, so the text goes in after.
    if (!Text.IsEmpty())
    {
        Node->NodeComment = Text;
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    Graph->NotifyGraphChanged();

    TSharedPtr<FJsonObject> Result = FUEAgentGraphUtils::NodeToJson(Node, false);
    return Result;
}

// MCP:get_graph_nodes - Read one Blueprint graph: every node with its node_id (NodeGuid), class, title, position, comment and pins (name, direction, type, linked_to, default) -- the read that feeds connect_pins / set_pin_default_value / delete_node, and the way to see what a graph actually DOES. TRAP: compact=true drops per-pin type metadata AND omits every pin that is unlinked and default-less (plus knots and comment boxes), so a pin missing from a compact listing is NOT proof the pin does not exist -- re-read with compact=false before concluding anything about pins. Also: a bare blueprint_name that matches two assets picks one silently (pass the full path), and reading a graph under a read-only vendor root leaves that package DIRTY in memory (reload_asset_package with on_dirty="discard" cleans it).
//   @param blueprint_name - Blueprint asset name or path. Prefer the FULL path: a short name with more than one match resolves silently to one of them, and stale backup copies share the original's name.
//   @param graph_name - Target graph name; defaults to the Blueprint's event graph when omitted. Also resolves a collapsed composite subgraph by its title (e.g. 'In Game HUD') and accepts a path form ('EventGraph > In Game HUD').
//   @param expand_composites - When true, inline the nodes of collapsed composite subgraphs recursively; each node gains a "graph" tag naming the graph it came from. Macro-library graphs are never inlined. (Optional, default false)
//   @param compact - Token-lean listing: drops pin types, positions and comments, and skips pins that are unlinked AND have no default. Also excludes K2Node_Knot + EdGraphNode_Comment unless exclude_classes is given. Good for reading topology, WRONG for checking whether a pin exists. (Optional, default false)
//   @param exclude_classes - Array of leaf node class names to skip, matched against each node's "class" field (e.g. ["K2Node_Knot"]). Pass [] to include everything even in compact mode. Absent + compact=true means knots and comments are excluded; absent + compact=false excludes nothing.
//   @param max_nodes - Cap on the number of nodes emitted (<=0 or absent = unlimited); the response still counts every non-excluded node in total_node_count and sets truncated=true.
//   @param node_id - Optional NodeGuid (from add_graph_node / get_graph_nodes): return ONLY that node of the graph, always at full fidelity (compact, exclude_classes, expand_composites and max_nodes are ignored) -- every pin with name, direction, type, linked_to and default, at a fraction of the tokens of a whole graph. A node_id the graph does not contain is an error.
//   @returns { blueprint_name, graph_name, nodes:[{node_id, class, title, pos_x, pos_y, comment, pins:[...], subgraph?}], node_count, compact, total_node_count, excluded_count, truncated } -- node_count is what was EMITTED, total_node_count what exists; a node owning a nested graph (composite / macro instance) carries "subgraph" with the name to pass back as graph_name. With node_id, nodes holds that one node (nodes[0]) and node_count is 1.
TSharedPtr<FJsonObject> HandleGetGraphNodes(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UBlueprint* Blueprint = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::FindBlueprint(BlueprintName, Blueprint)) { return Error; }

    FString GraphName;
    Params->TryGetStringField(TEXT("graph_name"), GraphName);
    UEdGraph* Graph = FUEAgentGraphUtils::FindGraphByNameDeep(Blueprint, GraphName);
    if (!Graph)
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Graph not found: %s. Tip: for a collapsed composite pass its title as graph_name "
                 "(e.g. 'In Game HUD'), a path like 'EventGraph > In Game HUD', or set expand_composites=true."),
            *GraphName));

    // One known node: its full-fidelity record, nothing else.
    FString NodeId;
    const bool bOneNode = Params->TryGetStringField(TEXT("node_id"), NodeId) && !NodeId.IsEmpty();
    if (bOneNode)
    {
        UEdGraphNode* Node = nullptr;
        if (TSharedPtr<FJsonObject> Error = GraphNodePinLookup::FindNode(Graph, NodeId, Node)) { return Error; }
        TArray<TSharedPtr<FJsonValue>> OneNode;
        OneNode.Add(MakeShared<FJsonValueObject>(FUEAgentGraphUtils::NodeToJson(Node, true)));
        TSharedPtr<FJsonObject> Single = UEAgentGraphCommandsImpl::BlueprintReply(BlueprintName, TEXT("graph_name"), Graph->GetName());
        Single->SetArrayField(TEXT("nodes"), OneNode);
        Single->SetNumberField(TEXT("node_count"), 1);
        Single->SetBoolField(TEXT("compact"), false);
        Single->SetNumberField(TEXT("total_node_count"), 1);
        Single->SetNumberField(TEXT("excluded_count"), 0);
        Single->SetBoolField(TEXT("truncated"), false);
        return Single;
    }

    bool bExpand = false;
    Params->TryGetBoolField(TEXT("expand_composites"), bExpand);

    // Compact mode drops per-pin type metadata + positions/comments and skips
    // unlinked-defaultless pins, to keep large graphs inside the token budget.
    bool bCompact = false;
    Params->TryGetBoolField(TEXT("compact"), bCompact);

    // exclude_classes filters out node classes by exact leaf class name (the
    // token reported in each node's "class" field). CRITICAL: only default to skipping
    // {K2Node_Knot, EdGraphNode_Comment} when the field is ABSENT *and* compact is true;
    // in non-compact mode with the field absent, exclude NOTHING so the full-fidelity
    // read stays complete. Explicit exclude_classes overrides
    // in both modes (pass [] to include everything).
    TSet<FString> ExcludeClasses;
    const TArray<TSharedPtr<FJsonValue>>* ExcludeArr = nullptr;
    if (Params->TryGetArrayField(TEXT("exclude_classes"), ExcludeArr) && ExcludeArr)
    {
        for (const TSharedPtr<FJsonValue>& V : *ExcludeArr)
        {
            FString S;
            if (V.IsValid() && V->TryGetString(S) && !S.IsEmpty())
            {
                ExcludeClasses.Add(S);
            }
        }
    }
    else if (bCompact)
    {
        ExcludeClasses.Add(TEXT("K2Node_Knot"));
        ExcludeClasses.Add(TEXT("EdGraphNode_Comment"));
    }

    // Optional max_nodes cap (<=0 or absent => unlimited).
    int32 MaxNodes = 0;
    Params->TryGetNumberField(TEXT("max_nodes"), MaxNodes);

    TArray<TSharedPtr<FJsonValue>> Nodes;
    TSet<UEdGraph*> Visited;
    bool bTruncated = false;
    int32 TotalCount = 0;
    int32 ExcludedCount = 0;
    AppendGraphNodes(Graph, Nodes, bExpand, Visited, bCompact, ExcludeClasses,
                     MaxNodes, bTruncated, TotalCount, ExcludedCount);

    TSharedPtr<FJsonObject> Result = UEAgentGraphCommandsImpl::BlueprintReply(BlueprintName, TEXT("graph_name"), Graph->GetName());
    Result->SetArrayField(TEXT("nodes"), Nodes);
    Result->SetNumberField(TEXT("node_count"), Nodes.Num());
    // Diagnostics -- compact echo, total (pre-cap) non-excluded node
    // count, how many nodes the exclude filter dropped, and whether max_nodes truncated.
    Result->SetBoolField(TEXT("compact"), bCompact);
    Result->SetNumberField(TEXT("total_node_count"), TotalCount);
    Result->SetNumberField(TEXT("excluded_count"), ExcludedCount);
    Result->SetBoolField(TEXT("truncated"), bTruncated);
    return Result;
}

// MCP:add_blueprint_variable - Add a MEMBER variable to a Blueprint (add_function_local_variable is the function-scoped twin), then read/write it with add_graph_node node_type=variable_get/variable_set. TRAP 1: the variable is born NOT editable per instance -- placing the actor and writing that variable fails with "cannot be edited on instances" -- and get_blueprint_variables reports instance_editable:true anyway; fix it with set_blueprint_variable_metadata(instance_editable=true). TRAP 2: on a class that ALREADY has actors placed in a level, those instances keep the value of the first compile, so a default set afterwards never reaches them -- verify on the instances, not on the CDO.
//   @param blueprint_name - Blueprint name or path.
//   @param variable_name - New variable name (fails if the name already exists on this Blueprint).
//   @param variable_type - Type string: bool, int, int64, float, string, name, text, byte, or the name/path of a struct, enum, class or object (e.g. Vector, /Game/Dir/S_Foo.S_Foo, /Script/Engine.Actor).
//   @param is_array - When true, make the variable an array container; defaults to false. This is the ONLY array authoring path -- add_function_parameter has no equivalent.
//   @param default_value - Initial default value for the new variable, in exported-text form; optional
//   @returns { blueprint_name, variable_name, variable_type, is_array } -- an echo of the request, NOT a verification: read the variable back with get_blueprint_variables (or the CDO) before relying on it.
TSharedPtr<FJsonObject> HandleAddBlueprintVariable(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, VarName, VarType;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("variable_name"), VarName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("variable_type"), VarType, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UBlueprint* Blueprint = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::FindBlueprint(BlueprintName, Blueprint)) { return Error; }

    FEdGraphPinType PinType;
    if (!FUEAgentGraphUtils::ResolvePinType(VarType, PinType))
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Could not resolve variable_type: %s"), *VarType));

    bool bIsArray = false;
    Params->TryGetBoolField(TEXT("is_array"), bIsArray);
    if (bIsArray)
    {
        PinType.ContainerType = EPinContainerType::Array;
    }

    FString DefaultValue;
    Params->TryGetStringField(TEXT("default_value"), DefaultValue);

    FScopedTransaction Transaction(LOCTEXT("AddBlueprintVariable", "MCP: Add Blueprint Variable"));
    Blueprint->Modify();
    const bool bAdded = FBlueprintEditorUtils::AddMemberVariable(Blueprint, FName(*VarName), PinType, DefaultValue);
    if (!bAdded)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("AddMemberVariable failed for '%s' (name may already exist)"), *VarName));
    }
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    TSharedPtr<FJsonObject> Result = UEAgentGraphCommandsImpl::BlueprintReply(BlueprintName, TEXT("variable_name"), VarName);
    Result->SetStringField(TEXT("variable_type"), VarType);
    Result->SetBoolField(TEXT("is_array"), bIsArray);
    return Result;
}

// MCP:set_blueprint_variable_default - Set the default value of a variable DECLARED on this Blueprint: writes the live CDO and mirrors the result back into the serialized variable description so it survives save/reload. LIMIT (not a lie, a hard boundary): it CANNOT touch an INHERITED variable -- it answers "Variable 'X' is not declared on this blueprint" -- so the "child that only overrides a parent default" pattern needs the CDO route (execute_python + set_editor_property + compile_blueprint + save). And on a class with actors already placed in a level, the placed instances keep their serialized value: verify on the instances, never on the CDO.
//   @param blueprint_name - Blueprint asset name or path
//   @param variable_name - Name of a variable declared on THIS Blueprint (inherited ones are refused)
//   @param value - New default in exported-text form, parsed into the variable's type; empty allowed. A value the type cannot parse is an error AND the previous value is restored (the engine's struct import zeroes the target before parsing, so a failed parse wipes the CDO).
//   @returns { blueprint_name, variable_name, default_value, roundtrip_verified } -- default_value is re-exported from the CDO after the write, so it is the real stored value. WARNING: roundtrip_verified:false means the exported text does NOT re-import into the type (e.g. FGameplayTagContainer): the CDO holds the right value and the compiler's property copy carries it, but the serialized description was deliberately left empty because feeding an unparseable string back through a Full compile ZEROES the default. Verify that variable on the CDO after compiling, and prefer wiring it in the graph if it must survive a fresh regeneration.
TSharedPtr<FJsonObject> HandleSetBpVariableDefault(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, VarName, Value;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("variable_name"), VarName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    Params->TryGetStringField(TEXT("value"), Value); // empty default is valid

    UBlueprint* Blueprint = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::FindBlueprint(BlueprintName, Blueprint)) { return Error; }

    const FName VName(*VarName);
    const int32 VarIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VName);
    if (VarIndex == INDEX_NONE)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Variable '%s' is not declared on this blueprint"), *VarName));

    UClass* GeneratedClass = Blueprint->GeneratedClass;
    UObject* CDO = GeneratedClass ? GeneratedClass->GetDefaultObject(false) : nullptr;
    if (!CDO)
        return mcp_common::MakeErrorReply(TEXT("Blueprint not compiled yet; compile it before setting defaults"));

    // Use the live CDO's class (may be a REINST_ class), matching the engine's own code.
    FProperty* Property = FindFProperty<FProperty>(CDO->GetClass(), VName);
    if (!Property)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Property '%s' not found on CDO"), *VarName));

    FScopedTransaction Transaction(LOCTEXT("SetVarDefault", "MCP: Set Variable Default"));
    Blueprint->Modify();
    CDO->Modify();

    // A failed parse is DESTRUCTIVE unless it is undone here.
    // For a generic FStructProperty (FGameplayTagContainer among them) PropertyValueFromString runs
    // StructProperty->InitializeValue(DirectValue) BEFORE ImportText_Direct
    // (BlueprintEditorUtils.cpp), so a parse that fails leaves the CDO WIPED, while
    // NewVariables[].DefaultValue still holds the OLD string -- and the next Full compile would
    // re-apply that old value onto the emptied CDO (an error, an empty CDO and a stale value after
    // compiling: three readings that contradict each other). So the previous value is restored.
    uint8* const ValuePtr = Property->ContainerPtrToValuePtr<uint8>(CDO);
    FDefaultConstructedPropertyElement Backup(Property);
    Property->CopyCompleteValue(Backup.GetObjAddress(), ValuePtr);

    if (!FBlueprintEditorUtils::PropertyValueFromString(Property, Value, reinterpret_cast<uint8*>(CDO)))
    {
        // Undo the partial write before reporting: an error must not change state.
        Property->CopyCompleteValue(ValuePtr, Backup.GetObjAddress());
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Could not parse '%s' into the variable's type (%s); the previous value was restored"), *Value, *Property->GetClass()->GetName()));
    }

    // Mirror the live value back into the serialized description so it survives save/reload.
    FBPVariableDescription& VarDesc = Blueprint->NewVariables[VarIndex];
    VarDesc.DefaultValue.Empty();
    FBlueprintEditorUtils::PropertyValueToString(Property, reinterpret_cast<const uint8*>(CDO), VarDesc.DefaultValue);

    // The mirrored string has to be RE-IMPORTABLE, or it destroys the very value it was meant to
    // preserve. A Full compile feeds NewVariables[].DefaultValue back through
    // SetPropertyDefaultValue and then clears it (KismetCompiler.cpp); that path hits the
    // same InitializeValue-then-ImportText above, so a text form the type cannot parse ZEROES a
    // default that was already correct. The round-trip is verified here, while both sides are at
    // hand, instead of surfacing a compile later as "the tool did not write".
    bool bRoundTrips = false;
    {
        FDefaultConstructedPropertyElement Probe(Property);
        uint8* const ProbePtr = static_cast<uint8*>(Probe.GetObjAddress());
        if (FBlueprintEditorUtils::PropertyValueFromString_Direct(Property, VarDesc.DefaultValue, ProbePtr, CDO))
        {
            bRoundTrips = Property->Identical(ProbePtr, ValuePtr, PPF_None);
        }
    }

    // What the caller gets back is the value really stored on the CDO, whatever we do with the mailbox.
    FString EffectiveValue;
    FBlueprintEditorUtils::PropertyValueToString(Property, reinterpret_cast<const uint8*>(CDO), EffectiveValue);

    if (!bRoundTrips)
    {
        // Leave the mailbox EMPTY on purpose. The CDO already holds the right value and the compiler's
        // old-CDO -> new-CDO property copy carries it across; an unparseable string here would not.
        VarDesc.DefaultValue.Empty();
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    TSharedPtr<FJsonObject> Result = UEAgentGraphCommandsImpl::BlueprintReply(BlueprintName, TEXT("variable_name"), VarName);
    Result->SetStringField(TEXT("default_value"), EffectiveValue);
    Result->SetBoolField(TEXT("roundtrip_verified"), bRoundTrips);
    return Result;
}

// MCP:set_blueprint_variable_metadata - Set the editor-facing checkboxes and text of an existing Blueprint variable: instance_editable (per-instance editability), save_game (CPF_SaveGame -- whether the variable goes into a .sav), expose_on_spawn (adds the variable as a pin on SpawnActor), category and tooltip. Only the fields present are touched; passing none is an error. This is the fix for a variable created by add_blueprint_variable, which is born NOT instance-editable while the readers report it as editable. Variable must be declared on THIS Blueprint; on a library variable the answer is a child class of your own, not this call. WARNING: this call does NOT compile: it structurally-modifies the Blueprint, which regenerates the SKELETON only, so a save_game just written is authored but NOT yet on the generated class the save archive reads -- the response says so in save_game_needs_compile, and the loop that finishes the job is compile_blueprint -> save_asset -> get_blueprint_variable_detailed_info and check save_game_generated.
//   @param blueprint_name - Blueprint name or path.
//   @param variable_name - Name of a variable declared on THIS Blueprint (inherited ones are refused).
//   @param instance_editable - true = editable on each placed actor. Verify by WRITING the placed instance and reading it back; the reporting field is unreliable for every variable.
//   @param save_game - true = tick "SaveGame" (CPF_SaveGame), so a SaveGame archive (ArIsSaveGame) serializes this variable; false unticks it. The only headless way to set this flag -- set_properties, Python, set_blueprint_variable_replication and BlueprintEditorLibrary all refuse it. Needs a compile to reach the generated class.
//   @param expose_on_spawn - true = the variable shows up as an input pin on SpawnActorFromClass. Verify on the NODE's pins, not on get_blueprint_variables' expose_on_spawn, which reports false for variables that ARE exposed.
//   @param category - Category to group the variable under in the editor
//   @param tooltip - Tooltip text shown for the variable
//   @returns { blueprint_name, variable_name, updated, save_game?, save_game_generated?, save_game_needs_compile? } -- updated:true only means at least one field was applied, not that it persisted. The three save_game_* fields appear only when save_game was passed, and are READ BACK after the write: save_game off the variable descriptor, save_game_generated off the FProperty of the generated class.
//   WARNING: WHY save_game LIVES HERE and not in set_blueprint_variable_replication or a tool of its own: it is a
//     checkbox of the variable Details panel exactly like instance_editable (which is also nothing but
//     property flags -- CPF_Edit / CPF_DisableEditOnInstance behind SetBlueprintOnlyEditableFlag), and
//     this command is in the PIE-unsafe table for the reason SetVariableSaveGameFlag needs (Reinstances,
//     evidence MarkBlueprintAsStructurallyModified). SaveGame is not replication, and
//     set_blueprint_variable_replication COMPILES, which would make a checkbox tick cost a full compile.
TSharedPtr<FJsonObject> HandleSetBpVariableMetadata(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, VarName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("variable_name"), VarName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UBlueprint* Blueprint = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::FindBlueprint(BlueprintName, Blueprint)) { return Error; }

    const FName VName(*VarName);
    if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VName) == INDEX_NONE)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Variable '%s' is not declared on this blueprint"), *VarName));

    FScopedTransaction Transaction(LOCTEXT("SetVarMeta", "MCP: Set Variable Metadata"));
    Blueprint->Modify();

    bool bAny = false;
    bool bInstanceEditable = false;
    if (Params->TryGetBoolField(TEXT("instance_editable"), bInstanceEditable))
    {
        // Inverted semantics: bBlueprintOnly == true means NOT instance-editable.
        FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(Blueprint, VName, !bInstanceEditable);
        bAny = true;
    }
    // The "SaveGame" checkbox. Same call the editor's own Details panel makes
    // (FBlueprintVarActionDetails::OnSaveGameChanged -> SetVariableSaveGameFlag,
    // BlueprintDetailsCustomization.cpp), which is `NewVariables[i].PropertyFlags |= or &=
    // ~CPF_SaveGame` followed by MarkBlueprintAsStructurallyModified (BlueprintEditorUtils.cpp).
    // Nothing else here reaches the flag: set_properties refuses it for not being CPF_Edit, Python
    // for CPF_NativeAccessSpecifierProtected, and neither BlueprintEditorLibrary nor
    // set_blueprint_variable_replication expose it.
    bool bSaveGame = false;
    const bool bSaveGameRequested = Params->TryGetBoolField(TEXT("save_game"), bSaveGame);
    if (bSaveGameRequested)
    {
        FBlueprintEditorUtils::SetVariableSaveGameFlag(Blueprint, VName, bSaveGame);
        bAny = true;
    }
    bool bExpose = false;
    if (Params->TryGetBoolField(TEXT("expose_on_spawn"), bExpose))
    {
        FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, VName, nullptr,
            FBlueprintMetadata::MD_ExposeOnSpawn, bExpose ? TEXT("true") : TEXT("false"));
        bAny = true;
    }
    FString Category;
    if (Params->TryGetStringField(TEXT("category"), Category))
    {
        FBlueprintEditorUtils::SetBlueprintVariableCategory(Blueprint, VName, nullptr, FText::FromString(Category));
        bAny = true;
    }
    FString Tooltip;
    if (Params->TryGetStringField(TEXT("tooltip"), Tooltip))
    {
        FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, VName, nullptr,
            FBlueprintMetadata::MD_Tooltip, Tooltip);
        bAny = true;
    }

    if (!bAny)
        return mcp_common::MakeErrorReply(TEXT("No metadata fields provided (instance_editable, save_game, expose_on_spawn, category, tooltip)"));

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    TSharedPtr<FJsonObject> Result = UEAgentGraphCommandsImpl::BlueprintReply(BlueprintName, TEXT("variable_name"), VarName);
    Result->SetBoolField(TEXT("updated"), true);
    // `updated:true` measures nothing on its own, so when save_game was asked for, read the flag
    // back off BOTH places it lives -- and expect them to differ. MarkBlueprintAsStructurallyModified
    // recompiles the SKELETON only (BlueprintEditorUtils.cpp, RegenerateSkeletonOnly); the flag
    // reaches the generated class -- the one the SaveGame archive actually asks -- when a real compile
    // copies the descriptor over it (KismetCompiler.cpp). So save_game_needs_compile:true right
    // after this call is the NORMAL, honest answer, not a failure: it says "compile_blueprint next".
    if (bSaveGameRequested)
    {
        bool bAuthored = false;
        if (const uint64* FlagsPtr = FBlueprintEditorUtils::GetBlueprintVariablePropertyFlags(Blueprint, VName))
        {
            bAuthored = (*FlagsPtr & CPF_SaveGame) != 0;
        }
        bool bOnGenerated = false;
        if (UClass* GenClass = Blueprint->GeneratedClass)
        {
            if (const FProperty* GenProp = FindFProperty<FProperty>(GenClass, VName))
            {
                bOnGenerated = GenProp->HasAnyPropertyFlags(CPF_SaveGame);
            }
        }
        Result->SetBoolField(TEXT("save_game"), bAuthored);
        Result->SetBoolField(TEXT("save_game_generated"), bOnGenerated);
        Result->SetBoolField(TEXT("save_game_needs_compile"), bAuthored != bOnGenerated);
    }
    return Result;
}

// MCP:delete_blueprint_variable - Remove a member variable DECLARED on this Blueprint (inherited/native variables are refused) and report whether it is really gone by re-looking it up. Nodes that still read/write it survive as broken references, so run compile_blueprint with return_messages=true afterwards. For an event dispatcher use remove_event_dispatcher instead -- this call would drop the delegate variable and strand its signature graph.
//   @param blueprint_name - Blueprint asset name or path
//   @param variable_name - Name of a variable declared on THIS Blueprint
//   @returns { blueprint_name, variable_name, deleted } -- deleted is re-checked after the removal, so false means it is still there.
TSharedPtr<FJsonObject> HandleDeleteBpVariable(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, VarName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("variable_name"), VarName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UBlueprint* Blueprint = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::FindBlueprint(BlueprintName, Blueprint)) { return Error; }

    const FName VName(*VarName);
    if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VName) == INDEX_NONE)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Variable '%s' is not declared on this blueprint (cannot delete inherited/native variables)"), *VarName));

    FScopedTransaction Transaction(LOCTEXT("DeleteVar", "MCP: Delete Variable"));
    Blueprint->Modify();
    FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, VName);
    const bool bRemoved = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VName) == INDEX_NONE;

    TSharedPtr<FJsonObject> Result = UEAgentGraphCommandsImpl::BlueprintReply(BlueprintName, TEXT("variable_name"), VarName);
    Result->SetBoolField(TEXT("deleted"), bRemoved);
    return Result;
}

namespace UEAgentGraphCommandsImpl
{
// Loads the packages the Asset Registry lists as referencers of Blueprint's package and returns the
// names of those that hold a Blueprint: the engine's rename fixups only reach Blueprints in memory.
TArray<TSharedPtr<FJsonValue>> LoadReferencerBlueprints(UBlueprint* Blueprint)
{
    TArray<TSharedPtr<FJsonValue>> Loaded;
    TArray<FName> Referencers;
    mcp_common::AssetRegistry().GetReferencers(Blueprint->GetOutermost()->GetFName(), Referencers);
    for (const FName& PackageName : Referencers)
    {
        if (UPackage* Pkg = LoadPackage(nullptr, *PackageName.ToString(), LOAD_None))
        {
            TArray<UObject*> Objects;
#if UE_VERSION_OLDER_THAN(5, 8, 0)
            GetObjectsWithOuter(Pkg, Objects, /*bIncludeNestedObjects*/ false); // UE 5.7: a bool, no EGetObjectsFlags
#else
            GetObjectsWithOuter(Pkg, Objects, EGetObjectsFlags::None);
#endif
            for (UObject* Obj : Objects)
            {
                if (Cast<UBlueprint>(Obj))
                {
                    Loaded.Add(MakeShared<FJsonValueString>(PackageName.ToString()));
                    break;
                }
            }
        }
    }
    return Loaded;
}
}

// MCP:rename_blueprint_variable - Rename a member variable DECLARED on this Blueprint, KEEPING everything the delete+re-add workaround throws away: the VarGuid, the type, the default value, the category, the tooltip and the replication flags -- and, crucially, RE-WIRING every Get/Set node that reads it, in this Blueprint and in every dependent one. This is the only correct way to rename an authored variable in place (e.g. to translate its name). WARNING: it refuses a variable that has a RepNotify function: the engine's own rename opens a MODAL dialog for that case (FSuppressableWarningDialog, which does NOT honour unattended mode) and would hang the editor waiting for a click -- clear the RepNotify with set_blueprint_variable_replication, rename, then set it back. WARNING: dependent Blueprints are only repaired if they are LOADED: this command loads the ones the Asset Registry reports as referencers first, and reports them in `referencers_loaded`, but an asset that references it in a way the registry does not see keeps the old name silently. It does NOT create CoreRedirects, so a saved game, a DataTable cell or a Python set_editor_property that names the variable as text is NOT fixed -- grep for those. Run compile_blueprint afterwards and save_asset on this Blueprint AND on every referencer.
//   @param blueprint_name - Blueprint asset name or path owning the variable.
//   @param old_name - Current name of a variable declared on THIS Blueprint; inherited/native ones are refused.
//   @param new_name - New name. Must be a legal identifier and free across the WHOLE class chain (the engine does not check this and would happily create a duplicate). Case-only changes are allowed.
//   @param load_referencers - Optional bool (default true). Load the Blueprints the Asset Registry lists as referencers BEFORE renaming, so the engine's reference fixup reaches them; it only ever sees what is already in memory.
//   @returns { blueprint_name, old_name, new_name, renamed (re-read: true only if new_name exists and old_name is gone), referencers_loaded: [string], repnotify_cleared: bool, warning? }
TSharedPtr<FJsonObject> HandleRenameBpVariable(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, OldName, NewName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("old_name"), OldName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("new_name"), NewName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    OldName.TrimStartAndEndInline();
    NewName.TrimStartAndEndInline();
    if (NewName.IsEmpty())
        return mcp_common::MakeErrorReply(TEXT("Parameter 'new_name' cannot be empty"));

    UBlueprint* Blueprint = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::FindBlueprint(BlueprintName, Blueprint)) { return Error; }

    const FName OldVName(*OldName);
    const FName NewVName(*NewName);
    const int32 VarIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, OldVName);
    if (VarIndex == INDEX_NONE)
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Variable '%s' is not declared on this blueprint (inherited/native variables cannot be renamed here)"), *OldName));

    if (OldVName.IsEqual(NewVName, ENameCase::CaseSensitive))
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("old_name and new_name are identical ('%s'); nothing to do"), *OldName));

    // The engine does NOT check that the new name is free: it writes VarName and moves on, leaving two
    // FBPVariableDescription with the same name. It is validated here, against the WHOLE class chain
    // (FKismetNameValidator also looks at the parent, the components and the functions): a new
    // variable that collides with an inherited one compiles and then behaves the opposite of how it reads.
    FKismetNameValidator Validator(Blueprint, OldVName);
    const EValidatorResult NameCheck = Validator.IsValid(NewVName, /*bOriginalNumericalNothing=*/false);
    if (NameCheck != EValidatorResult::Ok)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("new_name '%s' is not usable here: %s"),
            *NewName, *FKismetNameValidator::GetErrorString(NewName, NameCheck)));
    }

    // REPNOTIFY: RenameMemberVariable calls VerifyUserWantsRepNotifyVariableNameChanged
    // (BlueprintEditorUtils.cpp), which ends in FSuppressableWarningDialog::ShowModal().
    // That dialog does NOT check GIsRunningUnattendedScript -- only whether it is suppressed in the ini --
    // so over MCP it would hang the GAME THREAD waiting for a click nobody will give, and the client
    // would see a timeout indistinguishable from a dead editor. And if someone confirmed it, the engine
    // UNBINDS the RepNotify (RepNotifyFunc = NAME_None) without warning: the variable would keep
    // replicating but its OnRep would stop being called. It is refused, and the error says how to do it.
    const FName RepNotifyFunc = Blueprint->NewVariables[VarIndex].RepNotifyFunc;
    if (!RepNotifyFunc.IsNone())
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Variable '%s' has a RepNotify function ('%s'). Renaming it would open a modal dialog "
                 "that hangs the editor headless, and confirming it would silently unbind the OnRep. "
                 "Clear it first (set_blueprint_variable_replication mode='replicated'), rename, then set the "
                 "RepNotify back."),
            *OldName, *RepNotifyFunc.ToString()));
    }

    // Dependents are only repaired if they are LOADED: ReplaceVariableReferences ->
    // FindDependentBlueprints uses GetObjectsOfClass, which iterates MEMORY, not the Asset Registry.
    // Without this, a Blueprint that uses the variable and is not open keeps the old name, silently
    // and with renamed:true. The referencers the registry knows about are loaded first.
    bool bLoadReferencers = true;
    Params->TryGetBoolField(TEXT("load_referencers"), bLoadReferencers);
    TArray<TSharedPtr<FJsonValue>> LoadedJson;
    if (bLoadReferencers)
    {
        LoadedJson = UEAgentGraphCommandsImpl::LoadReferencerBlueprints(Blueprint);
    }

    FBlueprintEditorUtils::RenameMemberVariable(Blueprint, OldVName, NewVName);

    // POST-CHECK: RenameMemberVariable is void and has TWO paths that silently do nothing
    // (empty/same name, variable not found). The only honest oracle is re-reading both names.
    const bool bNewExists = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, NewVName) != INDEX_NONE;
    const bool bOldGone   = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, OldVName) == INDEX_NONE;
    const bool bRenamed   = bNewExists && bOldGone;
    if (!bRenamed)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Rename did not take effect: '%s' %s and '%s' %s. Nothing was changed."),
            *NewName, bNewExists ? TEXT("exists") : TEXT("is missing"),
            *OldName, bOldGone ? TEXT("is gone") : TEXT("is still there")));
    }

    // EXPLICIT marking, and it is not redundant, for two reasons. (1) Contract: it leaves the asset
    // marked even if the engine's internal path changes. (2) Above all, it is what puts this tool in
    // McpPieSafety: codegen_pie_safety.py looks for the markers in the HANDLER BODY, not in what the
    // handler calls -- and RenameMemberVariable REINSTANCES internally (via
    // MarkBlueprintAsStructurallyModified -> CompileSynchronously). Without it the tool would stay
    // OUT of the table of commands that are unsafe with a live PIE, which is exactly the case that
    // kills the editor: the reinstancer retains a world and CheckForWorldGCLeaks is Fatal.
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    TSharedPtr<FJsonObject> Result = UEAgentGraphCommandsImpl::BlueprintReply(BlueprintName, TEXT("old_name"), OldName);
    Result->SetStringField(TEXT("new_name"), NewName);
    Result->SetBoolField(TEXT("renamed"), bRenamed);
    Result->SetArrayField(TEXT("referencers_loaded"), LoadedJson);
    Result->SetBoolField(TEXT("repnotify_cleared"), false);
    Result->SetStringField(TEXT("warning"),
        TEXT("No CoreRedirect is created: anything naming this variable as TEXT (a saved game, a "
             "DataTable cell, set_editor_property from Python, a doc) still says the old name. "
             "Compile and save this Blueprint AND every referencer."));
    return Result;
}

// MCP:rename_function_parameter - Rename a PARAMETER of a Blueprint function graph -- the parameter counterpart of rename_blueprint_variable, and the only headless way to do it: add_function_parameter only ADDS, there is no remove_function_parameter, so "delete and re-add" is not even available. It renames the pin on the Entry node and on EVERY Result node of the graph, and registers a CoreRedirect so that call sites re-map themselves. That redirect is not a nicety: it is what stops this from being a PHANTOM PIN FACTORY. With the redirect, the call site's pin takes the new name AND KEEPS ITS LITERAL VALUE; renamed WITHOUT one, the old pin stays alive and ORPHANED on the caller. WARNING: that orphan is silent: it survives compiles that report num_errors:0 -- so `compile_blueprint` is NOT the oracle here. The oracle is get_graph_nodes(compact=false) on the callers, checking that NO pin still carries the old name. The redirect matches on `Class.Function.Param`, so it reaches callers that merely CALL the function (K2Node_Message included) without implementing its interface, and dependents that are not even loaded -- both of which rename_blueprint_variable cannot reach. It does NOT rename anything that names the parameter as TEXT (a DataTable cell, a saved game, Python). Run compile_blueprint afterwards and save_asset on this Blueprint AND on every caller. AND THE COMPILE IS NOT THE END OF IT: what reaches the .uasset is UserDefinedPins, not the EdGraphPins you read back -- a rename checked only on the latter can report renamed:true, compile at num_errors:0, be saved and COMMITTED, and still come back as the OLD name the next time the editor starts. So it also reports entry_user_pins_after/result_user_pins_after (the serialized side, legitimately EMPTY on an interface-override graph) and refuses to claim success while any UserDefinedPin still carries the old name. The end-to-end oracle is still disk: save_asset -> reload_asset_package -> get_function_pins.
//   @param blueprint_name - Blueprint asset name or path owning the function graph.
//   @param function_name - Name of the function graph (an interface's own function graph and an interface-override graph both work).
//   @param old_name - Current parameter name, matched on the Entry node and on every Result node. A name present on only one of them is fine.
//   @param new_name - New parameter name. Must be free on the terminal nodes; a collision is refused up front via the engine's own dry-run, before anything is touched.
//   @param write_redirect - Optional bool (default true). Also PERSIST the redirect as a +PropertyRedirects line in Config/DefaultEngine.ini, so it survives a restart and reaches the other machine. With false the redirect is registered in memory only and the rename EVAPORATES for anyone who opens the project later.
//   @param load_referencers - Optional bool (default true). Load the Blueprints the Asset Registry lists as referencers BEFORE renaming, so their nodes reconstruct in this session instead of the next one.
//   @returns { blueprint_name, function_name, old_name, new_name, renamed: bool, terminals_renamed: [string], entry_pins_after: [string], result_pins_after: [string], entry_user_pins_after: [string], result_user_pins_after: [string], redirect_registered: string, redirect_persisted_to: string|null, referencers_loaded: [string], must_compile_and_save: [string], warning: string } -- the *_user_pins_after pair is the SERIALIZED side (UserDefinedPins): empty on an interface-override graph is normal, but the old name appearing there is the rename that will revert on restart.
TSharedPtr<FJsonObject> HandleRenameFunctionParameter(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, FunctionName, OldName, NewName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("function_name"), FunctionName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("old_name"), OldName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("new_name"), NewName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    OldName.TrimStartAndEndInline();
    NewName.TrimStartAndEndInline();
    if (NewName.IsEmpty())
        return mcp_common::MakeErrorReply(TEXT("Parameter 'new_name' cannot be empty"));

    const FName OldPName(*OldName);
    const FName NewPName(*NewName);
    if (OldPName.IsEqual(NewPName, ENameCase::CaseSensitive))
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("old_name and new_name are identical ('%s'); nothing to do"), *OldName));

    UBlueprint* Blueprint = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::FindBlueprint(BlueprintName, Blueprint)) { return Error; }

    UEdGraph* Graph = FUEAgentGraphUtils::FindGraphByNameDeep(Blueprint, FunctionName);
    if (!Graph)
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Function graph '%s' not found on '%s'"), *FunctionName, *BlueprintName));

    // The graph's TERMINATORS are where the parameters live: the Entry carries the inputs and
    // EACH Result carries the outputs. Both kinds are collected and all of them are renamed, the same
    // way the editor itself does it (BlueprintDetailsCustomization.cpp iterates NodesToRename
    // without distinguishing direction) -- which is why this tool has NO is_output parameter: the engine
    // has no such notion, and a name present on only one of the two returns NoSuchPin on the other,
    // which is benign.
    TArray<UK2Node_EditablePinBase*> Terminals;
    {
        TArray<UK2Node_FunctionEntry*> EntryNodes;
        Graph->GetNodesOfClass<UK2Node_FunctionEntry>(EntryNodes);
        for (UK2Node_FunctionEntry* N : EntryNodes) { if (N) { Terminals.Add(N); } }

        TArray<UK2Node_FunctionResult*> ResultNodes;
        Graph->GetNodesOfClass<UK2Node_FunctionResult>(ResultNodes);
        for (UK2Node_FunctionResult* N : ResultNodes) { if (N) { Terminals.Add(N); } }
    }
    if (Terminals.Num() == 0)
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Graph '%s' has no Entry/Result terminal node: it is not a function graph "
                 "(a custom event is not one either, and cannot carry renameable parameters)."), *FunctionName));

    // DRY-RUN FIRST, and it is free: RenameUserDefinedPin(Old, New, bTest=true) (K2Node.h) does
    // exactly the same checks without touching anything. It runs over ALL terminators
    // before mutating any, so the graph is never left half-done -- a collision on the second Result
    // with the first one already renamed would be exactly the inconsistent state this tool exists to
    // avoid.
    int32 WouldRename = 0;
    for (UK2Node_EditablePinBase* Node : Terminals)
    {
        const ERenamePinResult Test = Node->RenameUserDefinedPin(OldPName, NewPName, /*bTest=*/true);
        if (Test == ERenamePinResult_NameCollision)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("new_name '%s' collides with an existing pin on '%s' of graph '%s'. Nothing was changed."),
                *NewName, *Node->GetName(), *FunctionName));
        }
        if (Test == ERenamePinResult_Success) { ++WouldRename; }
    }
    if (WouldRename == 0)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("No parameter named '%s' on any terminal node of graph '%s'. Read the current ones "
                 "with get_function_pins."), *OldName, *FunctionName));
    }

    // Load the referencers BEFORE renaming: their nodes are reconstructed in THIS session instead
    // of the next one. Unlike rename_blueprint_variable, this is NOT what makes the callers correct
    // (the CoreRedirect takes care of that, and it also reaches what is not loaded); here it only
    // brings the work forward so the caller can verify and save right away.
    bool bLoadReferencers = true;
    Params->TryGetBoolField(TEXT("load_referencers"), bLoadReferencers);
    TArray<TSharedPtr<FJsonValue>> LoadedJson;
    if (bLoadReferencers)
    {
        LoadedJson = UEAgentGraphCommandsImpl::LoadReferencerBlueprints(Blueprint);
    }

    // THE REDIRECT GOES BEFORE THE RENAME. Marking the Blueprint as modified reconstructs the
    // calling nodes, and ShouldRedirectParam (K2Node.cpp) is consulted DURING that rewire: if
    // the redirect is not in place yet, the caller's old pin is orphaned and there is nothing left
    // to re-map. Key = "<Class>.<Function>.<Param>", which is one of the keys built by
    // UK2Node_CallFunction::GetRedirectPinNames (K2Node_CallFunction.cpp).
    const FString ClassName = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetName()
                                                        : (Blueprint->GetName() + TEXT("_C"));
    const FString RedirectOld = FString::Printf(TEXT("%s.%s.%s"), *ClassName, *FunctionName, *OldName);
    {
        const FCoreRedirect Redirect(ECoreRedirectFlags::Type_Property, RedirectOld, NewName);
        FCoreRedirects::AddRedirectList(MakeArrayView(&Redirect, 1), TEXT("rename_function_parameter"));
    }

    // Persistence. GConfig is NOT used: the engine's parser DELETES the [CoreRedirects] section from
    // the in-memory config as soon as it reads it (CoreRedirects.cpp RemoveSectionFromBranch), so
    // a read-modify-write through GConfig would see an empty section and wipe the previous lines.
    // Text is appended to the file instead.
    bool bWriteRedirect = true;
    Params->TryGetBoolField(TEXT("write_redirect"), bWriteRedirect);
    FString PersistedTo;
    if (bWriteRedirect)
    {
        const FString IniPath = FPaths::ProjectConfigDir() / TEXT("DefaultEngine.ini");
        const FString Line = FString::Printf(TEXT("+PropertyRedirects=(OldName=\"%s\",NewName=\"%s\")"),
                                             *RedirectOld, *NewName);
        FString Existing;
        FFileHelper::LoadFileToString(Existing, *IniPath);
        if (!Existing.Contains(Line))
        {
            FString ToAppend;
            if (!Existing.Contains(TEXT("[CoreRedirects]")))
            {
                ToAppend += TEXT("\r\n[CoreRedirects]\r\n");
            }
            ToAppend += Line + TEXT("\r\n");
            // WARNING: THE ENCODING IS MANDATORY, not a detail. SaveStringToFile defaults to
            // EEncodingOptions::AutoDetect, and with a single non-ASCII character in the file (this ini
            // has some) it rewrites the WHOLE file as UTF-16LE with a BOM: the file doubles in size in a
            // single call, git shows a full diff, and anything that reads the ini as UTF-8 (grep
            // included) stops seeing it.
            if (FFileHelper::SaveStringToFile(Existing + ToAppend, *IniPath,
                                              FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
            {
                PersistedTo = TEXT("Config/DefaultEngine.ini");
            }
        }
        else
        {
            PersistedTo = TEXT("Config/DefaultEngine.ini");
        }
    }

    // REAL PASS.
    TArray<TSharedPtr<FJsonValue>> TerminalsRenamed;
    for (UK2Node_EditablePinBase* Node : Terminals)
    {
        if (Node->RenameUserDefinedPin(OldPName, NewPName, /*bTest=*/false) == ERenamePinResult_Success)
        {
            TerminalsRenamed.Add(MakeShared<FJsonValueString>(Node->GetName()));
        }
    }

    // Explicit marking: it regenerates the class and reconstructs the callers (with the redirect
    // already in place), and it is also what puts this tool in McpPieSafety -- codegen_pie_safety.py
    // looks for the marker in the BODY of the handler, not in what the handler calls.
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    // RESYNC OF THE ARRAY THAT GETS SERIALIZED -- and it goes AFTER the marking, on purpose.
    //
    // `Node->Pins` are EdGraphPins: the TRANSIENT side, which is rebuilt. What travels to the .uasset
    // is `UserDefinedPins` (TArray<TSharedPtr<FUserPinInfo>>), and on LOAD the pins are rebuilt
    // from there. If the two drift apart, the rename looks perfect in memory and reverts to the old
    // name on the next editor start -- without a single error along the way.
    //
    // WARNING: the whole chain can come out GREEN -- entry_pins_after shows the new name,
    // compile_blueprint gives num_errors:0, get_function_pins on the class exposes the new name and
    // the save CHANGES the file hash -- while the disk never gets the new name: on the next editor
    // start the parameters are back to the old names and their callers do not compile.
    // Repro in 3 calls: rename -> save_asset -> reload_asset_package -> get_function_pins.
    for (UK2Node_EditablePinBase* Node : Terminals)
    {
        for (const TSharedPtr<FUserPinInfo>& Info : Node->UserDefinedPins)
        {
            if (Info.IsValid() && Info->PinName == OldPName)
            {
                Node->Modify();
                Info->PinName = NewPName;
            }
        }
    }

    // POST-CHECK: both sets are checked, because they are two different questions: `Node->Pins`
    // alone is the side that is NOT saved, and a rename can leave the FUserPinInfo unsynchronized.
    //
    // The verdict is that NO UserDefinedPin still carries the old name. The NEW name is not
    // required to appear there: in an interface OVERRIDE graph the parameters come from the
    // UFunction and `UserDefinedPins` is EMPTY, so requiring it would refuse the normal case.
    TArray<TSharedPtr<FJsonValue>> EntryAfter, ResultAfter, EntryUserAfter, ResultUserAfter;
    bool bNewPresent = false, bOldGone = true;   // transient side  (Pins)
    bool bOldPersisted = false;                  // serialized side (UserDefinedPins)
    for (UK2Node_EditablePinBase* Node : Terminals)
    {
        const bool bIsEntry = Node->IsA<UK2Node_FunctionEntry>();
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin) { continue; }
            const FName PinName = Pin->PinName;
            (bIsEntry ? EntryAfter : ResultAfter).Add(MakeShared<FJsonValueString>(PinName.ToString()));
            if (PinName == NewPName) { bNewPresent = true; }
            if (PinName == OldPName) { bOldGone = false; }
        }
        for (const TSharedPtr<FUserPinInfo>& Info : Node->UserDefinedPins)
        {
            if (!Info.IsValid()) { continue; }
            const FName PinName = Info->PinName;
            (bIsEntry ? EntryUserAfter : ResultUserAfter).Add(MakeShared<FJsonValueString>(PinName.ToString()));
            if (PinName == OldPName) { bOldPersisted = true; }
        }
    }
    const bool bRenamed = bNewPresent && bOldGone && !bOldPersisted;
    if (!bRenamed)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Rename did not take effect. In-memory pins (Pins): '%s' %s and '%s' %s. "
                 "SERIALIZED side (UserDefinedPins -- what actually reaches the .uasset): '%s' %s. "
                 "%sThe CoreRedirect WAS registered%s, so re-run this command or undo it by hand."),
            *NewName, bNewPresent ? TEXT("is present") : TEXT("is missing"),
            *OldName, bOldGone ? TEXT("is gone") : TEXT("is still there"),
            *OldName, bOldPersisted ? TEXT("is STILL THERE") : TEXT("is gone"),
            bOldPersisted ? TEXT("That is the dangerous one: the rename would have looked green and "
                                 "reverted on the next editor restart. ") : TEXT(""),
            PersistedTo.IsEmpty() ? TEXT("") : TEXT(" and persisted to Config/DefaultEngine.ini")));
    }

    TArray<TSharedPtr<FJsonValue>> MustSave;
    MustSave.Add(MakeShared<FJsonValueString>(Blueprint->GetOutermost()->GetName()));
    for (const TSharedPtr<FJsonValue>& V : LoadedJson) { MustSave.Add(V); }

    TSharedPtr<FJsonObject> Result = UEAgentGraphCommandsImpl::BlueprintReply(BlueprintName, TEXT("function_name"), FunctionName);
    Result->SetStringField(TEXT("old_name"), OldName);
    Result->SetStringField(TEXT("new_name"), NewName);
    Result->SetBoolField(TEXT("renamed"), bRenamed);
    Result->SetArrayField(TEXT("terminals_renamed"), TerminalsRenamed);
    Result->SetArrayField(TEXT("entry_pins_after"), EntryAfter);
    Result->SetArrayField(TEXT("result_pins_after"), ResultAfter);
    // The SERIALIZED side. Empty on an interface override (the params come from the UFunction) and
    // populated on an interface's own graph or on a normal function's: it is where you see whether
    // the rename will survive closing the editor.
    Result->SetArrayField(TEXT("entry_user_pins_after"), EntryUserAfter);
    Result->SetArrayField(TEXT("result_user_pins_after"), ResultUserAfter);
    Result->SetStringField(TEXT("redirect_registered"), FString::Printf(TEXT("%s -> %s"), *RedirectOld, *NewName));
    if (PersistedTo.IsEmpty()) { Result->SetField(TEXT("redirect_persisted_to"), MakeShared<FJsonValueNull>()); }
    else                       { Result->SetStringField(TEXT("redirect_persisted_to"), PersistedTo); }
    Result->SetArrayField(TEXT("referencers_loaded"), LoadedJson);
    Result->SetArrayField(TEXT("must_compile_and_save"), MustSave);
    Result->SetStringField(TEXT("warning"),
        TEXT("compile_blueprint is NOT the oracle here: a phantom pin left on a caller can survive "
             "two compiles with num_errors:0. Verify with get_graph_nodes(compact=false) on "
             "each caller that no pin still carries the old name. Nothing that names the parameter as "
             "TEXT (DataTable cell, saved game, Python) is fixed. AND compiling green does not mean it "
             "reached DISK: close the loop with save_asset -> reload_asset_package -> get_function_pins, "
             "because a rename that only touched the EdGraphPins reverts on the next editor start."));
    return Result;
}

namespace UEAgentGraphCommandsImpl
{
// The interface class InterfacePath names: the generated class of the interface Blueprint at that
// path, else the class ResolveClass finds for it. Null when neither resolves.
UClass* ResolveInterfaceClass(const FString& InterfacePath)
{
    UClass* InterfaceClass = nullptr;
    if (UBlueprint* InterfaceBP = LoadObject<UBlueprint>(nullptr, *InterfacePath))
    {
        InterfaceClass = InterfaceBP->GeneratedClass;
    }
    return InterfaceClass ? InterfaceClass : FUEAgentGraphUtils::ResolveClass(InterfacePath);
}
}

// MCP:implement_interface - Add a Blueprint Interface to a Blueprint's implemented list (asset path or class name). NOT idempotent: an interface already implemented comes back as an error, unlike remove_interface. It only registers the contract -- the graphs that IMPLEMENT each member are created by add_blueprint_function with override=true, which is also the only correct route for interface EVENTS.
//   @param blueprint_name - Blueprint asset name or path
//   @param interface_path - Blueprint interface asset path (e.g. /Game/Dir/BPI_Foo.BPI_Foo), or a resolvable interface class name/path
//   @returns { blueprint_name, interface, implemented } -- implemented is always true on success; failure is an error response. Confirm the member set with get_blueprint_functions (interface graphs live in ImplementedInterfaces, not FunctionGraphs).
TSharedPtr<FJsonObject> HandleImplementInterface(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, InterfacePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("interface_path"), InterfacePath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UBlueprint* Blueprint = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::FindBlueprint(BlueprintName, Blueprint)) { return Error; }

    // Resolve the interface to a loaded UClass (ImplementNewInterface uses FindObject + check()).
    UClass* InterfaceClass = UEAgentGraphCommandsImpl::ResolveInterfaceClass(InterfacePath);
    if (!InterfaceClass)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Could not resolve interface: %s"), *InterfacePath));

    FScopedTransaction Transaction(LOCTEXT("ImplementInterface", "MCP: Implement Interface"));
    Blueprint->Modify();
    const bool bOk = FBlueprintEditorUtils::ImplementNewInterface(Blueprint, InterfaceClass->GetClassPathName());
    if (!bOk)
        return mcp_common::MakeErrorReply(TEXT("ImplementNewInterface failed (already implemented, or not a valid interface)"));
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    TSharedPtr<FJsonObject> Result = UEAgentGraphCommandsImpl::BlueprintReply(BlueprintName, TEXT("interface"), InterfaceClass->GetName());
    Result->SetBoolField(TEXT("implemented"), true);
    return Result;
}

// MCP:remove_interface - Remove an implemented Blueprint Interface (inverse of implement_interface) and, unless preserve_functions is set, DELETE the graphs that implemented it -- work is lost, so pass preserve_functions=true to keep them as ordinary Blueprint functions. Idempotent, unlike implement_interface: an interface that is not implemented returns removed:false instead of erroring.
//   @param blueprint_name - Blueprint asset name or path
//   @param interface_path - Blueprint interface asset path (or class name) to remove
//   @param preserve_functions - When true, keep the interface's function graphs as normal Blueprint functions instead of deleting them; defaults to false
//   @returns { blueprint_name, interface, removed } -- removed:false means it was not implemented (no-op), not a failure.
TSharedPtr<FJsonObject> HandleRemoveInterface(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, InterfacePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("interface_path"), InterfacePath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    bool bPreserveFunctions = false;
    Params->TryGetBoolField(TEXT("preserve_functions"), bPreserveFunctions);

    UBlueprint* Blueprint = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::FindBlueprint(BlueprintName, Blueprint)) { return Error; }

    UClass* InterfaceClass = UEAgentGraphCommandsImpl::ResolveInterfaceClass(InterfacePath);
    if (!InterfaceClass)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Could not resolve interface: %s"), *InterfacePath));

    // Idempotency: is this interface actually implemented on the blueprint? The engine's
    // FBPInterfaceDescription::Interface is a TSubclassOf<UInterface> holding this class.
    const bool bWasImplemented = Blueprint->ImplementedInterfaces.ContainsByPredicate(
        [InterfaceClass](const FBPInterfaceDescription& Desc) { return Desc.Interface == InterfaceClass; });
    if (!bWasImplemented)
    {
        // Already absent -- report a no-op rather than error (re-running must be safe).
        TSharedPtr<FJsonObject> Result = UEAgentGraphCommandsImpl::BlueprintReply(BlueprintName, TEXT("interface"), InterfaceClass->GetName());
        Result->SetBoolField(TEXT("removed"), false);
        return Result;
    }

    // Exact inverse of ImplementNewInterface: RemoveInterface drops the interface entry AND
    // its associated member function graphs (unless bPreserveFunctions promotes them).
    FScopedTransaction Transaction(LOCTEXT("RemoveInterface", "MCP: Remove Interface"));
    Blueprint->Modify();
    FBlueprintEditorUtils::RemoveInterface(Blueprint, InterfaceClass->GetClassPathName(), bPreserveFunctions);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    TSharedPtr<FJsonObject> Result = UEAgentGraphCommandsImpl::BlueprintReply(BlueprintName, TEXT("interface"), InterfaceClass->GetName());
    Result->SetBoolField(TEXT("removed"), true);
    return Result;
}

namespace
{
    UEdGraph* FindFunctionGraph(UBlueprint* Blueprint, const FString& FunctionName)
    {
        if (!Blueprint)
        {
            return nullptr;
        }
        const FName FName_(*FunctionName);
        for (UEdGraph* G : Blueprint->FunctionGraphs)
        {
            if (G && G->GetFName() == FName_)
            {
                return G;
            }
        }
        // Interface-override function graphs live in ImplementedInterfaces[].Graphs, not in
        // FunctionGraphs -- so add_function_parameter / add_function_local_variable can also
        // target an interface override (e.g. add a local var to BPI_MyInterface::MyFunction).
        for (const FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
        {
            for (UEdGraph* G : InterfaceDesc.Graphs)
            {
                if (G && G->GetFName() == FName_)
                {
                    return G;
                }
            }
        }
        return nullptr;
    }

    /** The macro graph of Blueprint (Blueprint->MacroGraphs) called MacroName, or null. */
    UEdGraph* FindMacroGraph(UBlueprint* Blueprint, const FString& MacroName)
    {
        if (!Blueprint)
        {
            return nullptr;
        }
        const FName MacroFName(*MacroName);
        for (UEdGraph* G : Blueprint->MacroGraphs)
        {
            if (G && G->GetFName() == MacroFName)
            {
                return G;
            }
        }
        return nullptr;
    }

    /**
     * The tunnel node that carries a macro's inputs (bOutputs false: the entry tunnel, whose pins are outputs) or its
     * outputs (the exit tunnel, whose pins are inputs). Exact class: macro instances and composites are tunnels too.
     */
    UK2Node_Tunnel* FindMacroTunnel(UEdGraph* MacroGraph, bool bOutputs)
    {
        for (UEdGraphNode* Node : MacroGraph->Nodes)
        {
            UK2Node_Tunnel* Tunnel = Cast<UK2Node_Tunnel>(Node);
            if (!Tunnel || Tunnel->GetClass() != UK2Node_Tunnel::StaticClass())
            {
                continue;
            }
            if (bOutputs ? (Tunnel->bCanHaveInputs && !Tunnel->bCanHaveOutputs) : (Tunnel->bCanHaveOutputs && !Tunnel->bCanHaveInputs))
            {
                return Tunnel;
            }
        }
        return nullptr;
    }

    /**
     * The nodes whose user-defined pins are one side of a graph's signature: a function's entry node (inputs) or its
     * result nodes (outputs; a function can have several), or a macro's entry or exit tunnel.
     */
    TArray<UK2Node_EditablePinBase*> SignatureNodes(UEdGraph* FuncGraph, UEdGraph* MacroGraph, bool bIsOutput)
    {
        TArray<UK2Node_EditablePinBase*> Out;
        if (MacroGraph)
        {
            if (UK2Node_Tunnel* Tunnel = FindMacroTunnel(MacroGraph, bIsOutput))
            {
                Out.Add(Tunnel);
            }
        }
        else if (!bIsOutput)
        {
            TArray<UK2Node_FunctionEntry*> Entries;
            FuncGraph->GetNodesOfClass<UK2Node_FunctionEntry>(Entries);
            for (UK2Node_FunctionEntry* Entry : Entries)
            {
                Out.Add(Entry);
            }
        }
        else
        {
            TArray<UK2Node_FunctionResult*> Results;
            FuncGraph->GetNodesOfClass<UK2Node_FunctionResult>(Results);
            for (UK2Node_FunctionResult* Result : Results)
            {
                Out.Add(Result);
            }
        }
        return Out;
    }

    /** The names of the user-defined pins of Nodes, each once: the parameters of one side of a signature. */
    TArray<FString> UserPinNames(const TArray<UK2Node_EditablePinBase*>& Nodes)
    {
        TArray<FString> Names;
        for (const UK2Node_EditablePinBase* Node : Nodes)
        {
            for (const TSharedPtr<FUserPinInfo>& Info : Node->UserDefinedPins)
            {
                if (Info.IsValid())
                {
                    Names.AddUnique(Info->PinName.ToString());
                }
            }
        }
        return Names;
    }
}

// MCP:add_function_parameter - Add an input or output (return) parameter to an existing Blueprint FUNCTION graph -- interface-override graphs included -- or to a MACRO graph (create_blueprint_macro), and reconstruct its Entry/Result node or the macro's input/output tunnel. A macro also takes param_type "exec", an execution pin, so it can get several exec inputs and outputs. TWO HARD LIMITS, both silent-ish: it cannot create an ARRAY parameter (there is no is_array; the param comes out SCALAR with success), so redesign the signature to read the array where it lives; and it cannot reach a CUSTOM EVENT ("Function graph not found"), so a Server/Client RPC WITH arguments must be authored as a function plus set_blueprint_function_rpc. On a Blueprint just given a new function, compile_blueprint between creation and this call. Inverse: remove_function_parameter.
//   @param blueprint_name - Blueprint name or path.
//   @param function_name - Name of an existing function graph (FunctionGraphs or an interface-override graph), or of a macro graph of this Blueprint when no function has that name. A custom event is NOT a function graph and is rejected here.
//   @param param_name - New parameter name; made unique, so the applied name is the one echoed back in the response.
//   @param param_type - Type string: bool, int, int64, float, string, name, text, byte, Vector, or a struct/enum/class/object name or path; "exec" on a macro. Arrays are NOT expressible.
//   @param is_output - When true, add an output/return param (an input pin on the Result node) instead of an input; defaults to false
//   @returns { blueprint_name, function_name, param_name, param_type, is_output, graph_kind? } -- param_name is the pin name actually created (uniquified); graph_kind is "macro" when the parameter went on a macro. Verify with get_graph_nodes at compact=false: compact hides the new, still-unlinked pin and makes it look like nothing was created.
TSharedPtr<FJsonObject> HandleAddFunctionParameter(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, FunctionName, ParamName, ParamType;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("function_name"), FunctionName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("param_name"), ParamName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("param_type"), ParamType, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UBlueprint* Blueprint = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::FindBlueprint(BlueprintName, Blueprint)) { return Error; }

    // A function first, as before; a macro only when no function graph has that name.
    UEdGraph* FuncGraph = FindFunctionGraph(Blueprint, FunctionName);
    UEdGraph* MacroGraph = FuncGraph ? nullptr : FindMacroGraph(Blueprint, FunctionName);
    if (!FuncGraph && !MacroGraph)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Function graph not found: %s (no macro graph has that name either)"), *FunctionName));

    FEdGraphPinType PinType;
    if (MacroGraph && ParamType.TrimStartAndEnd().Equals(TEXT("exec"), ESearchCase::IgnoreCase))
    {
        PinType.PinCategory = UEdGraphSchema_K2::PC_Exec;
    }
    else if (!FUEAgentGraphUtils::ResolvePinType(ParamType, PinType))
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Could not resolve param_type: %s"), *ParamType));

    bool bIsOutput = false;
    Params->TryGetBoolField(TEXT("is_output"), bIsOutput);

    if (MacroGraph)
    {
        // A macro's inputs are OUTPUT pins of its entry tunnel, its outputs INPUT pins of its exit tunnel.
        UK2Node_Tunnel* Tunnel = FindMacroTunnel(MacroGraph, bIsOutput);
        if (!Tunnel)
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Macro '%s' has no %s tunnel node"),
                *FunctionName, bIsOutput ? TEXT("output") : TEXT("input")));
        const EEdGraphPinDirection PinDir = bIsOutput ? EGPD_Input : EGPD_Output;
        for (const UEdGraphPin* P : Tunnel->Pins)
        {
            if (P && P->Direction == PinDir && P->PinName == FName(*ParamName))
            {
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("Macro '%s' already has an %s named '%s'; nothing was added."),
                    *FunctionName, bIsOutput ? TEXT("output") : TEXT("input"), *ParamName));
            }
        }

        FScopedTransaction Transaction(LOCTEXT("AddMacroParam", "MCP: Add Macro Parameter"));
        Blueprint->Modify();
        Tunnel->Modify();
        UEdGraphPin* NewMacroPin = Tunnel->CreateUserDefinedPin(FName(*ParamName), PinType, PinDir, /*bUseUniqueName=*/false);
        if (!NewMacroPin)
            return mcp_common::MakeErrorReply(TEXT("Failed to create the macro parameter pin (invalid type or duplicate name)"));
        Tunnel->ReconstructNode();
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

        TSharedPtr<FJsonObject> MacroResult = UEAgentGraphCommandsImpl::BlueprintReply(BlueprintName, TEXT("function_name"), FunctionName);
        MacroResult->SetStringField(TEXT("param_name"), NewMacroPin->PinName.ToString());
        MacroResult->SetStringField(TEXT("param_type"), ParamType);
        MacroResult->SetBoolField(TEXT("is_output"), bIsOutput);
        MacroResult->SetStringField(TEXT("graph_kind"), TEXT("macro"));
        return MacroResult;
    }

    TArray<UK2Node_FunctionEntry*> EntryNodes;
    FuncGraph->GetNodesOfClass<UK2Node_FunctionEntry>(EntryNodes);
    if (EntryNodes.Num() == 0)
        return mcp_common::MakeErrorReply(TEXT("Function has no entry node"));
    UK2Node_FunctionEntry* EntryNode = EntryNodes[0];

    FScopedTransaction Transaction(LOCTEXT("AddFnParam", "MCP: Add Function Parameter"));
    Blueprint->Modify();

    // bUseUniqueName=true means a name that is ALREADY TAKEN comes back with a numeric suffix
    // (K2Node_FunctionTerminator::CreateUniquePinName), so "add the parameter Required" on a graph
    // that already has `Required` would silently yield `Required1` -- a phantom parameter on a
    // public contract. Refuse up front instead: the pin the caller asked for already exists, and
    // saying so is the only answer that lets them tell "already done" from "done under another name".
    {
        const EEdGraphPinDirection ExistingDir = bIsOutput ? EGPD_Input : EGPD_Output;
        const UK2Node_EditablePinBase* NodeToCheck = bIsOutput
            ? static_cast<const UK2Node_EditablePinBase*>(FBlueprintEditorUtils::FindOrCreateFunctionResultNode(EntryNode))
            : static_cast<const UK2Node_EditablePinBase*>(EntryNode);
        if (NodeToCheck)
        {
            for (const UEdGraphPin* P : NodeToCheck->Pins)
            {
                if (P && P->Direction == ExistingDir && P->PinName == FName(*ParamName))
                {
                    return mcp_common::MakeErrorReply(FString::Printf(
                        TEXT("Function '%s' already has a %s pin named '%s'; nothing was added. Renaming it to "
                             "'%s1' would leave a phantom parameter on the signature."),
                        *FunctionName, bIsOutput ? TEXT("output") : TEXT("input"), *ParamName, *ParamName));
                }
            }
        }
    }

    UEdGraphPin* NewPin = nullptr;
    UK2Node_EditablePinBase* ModifiedNode = nullptr;
    if (!bIsOutput)
    {
        // A function INPUT is an OUTPUT pin on the entry node (direction inverted).
        EntryNode->Modify();
        NewPin = EntryNode->CreateUserDefinedPin(FName(*ParamName), PinType, EGPD_Output, /*bUseUniqueName=*/false);
        ModifiedNode = EntryNode;
    }
    else
    {
        // A function OUTPUT/return is an INPUT pin on the result node (direction inverted).
        UK2Node_FunctionResult* ResultNode = FBlueprintEditorUtils::FindOrCreateFunctionResultNode(EntryNode);
        if (!ResultNode)
            return mcp_common::MakeErrorReply(TEXT("Could not find or create the function result node"));
        ResultNode->Modify();
        NewPin = ResultNode->CreateUserDefinedPin(FName(*ParamName), PinType, EGPD_Input, /*bUseUniqueName=*/false);
        ModifiedNode = ResultNode;
    }

    if (!NewPin)
        return mcp_common::MakeErrorReply(TEXT("Failed to create parameter pin (invalid type or duplicate name)"));

    ModifiedNode->ReconstructNode();
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    TSharedPtr<FJsonObject> Result = UEAgentGraphCommandsImpl::BlueprintReply(BlueprintName, TEXT("function_name"), FunctionName);
    Result->SetStringField(TEXT("param_name"), NewPin->PinName.ToString());
    Result->SetStringField(TEXT("param_type"), ParamType);
    Result->SetBoolField(TEXT("is_output"), bIsOutput);
    return Result;
}

// MCP:add_function_local_variable - Add a variable scoped to ONE function graph (interface-override graphs included), instead of a class member like add_blueprint_variable -- use it for scratch state that no other graph should see. Reach it from the graph with add_graph_node node_type=variable_get/variable_set and scope="local" (the default scope="auto" already prefers a local over a member of the same name). Inverse: remove_function_local_variable, whose reply lists the function's locals.
//   @param blueprint_name - Blueprint asset name or path
//   @param function_name - Name of an existing function graph (FunctionGraphs or an interface-override graph)
//   @param variable_name - Variable name; the call fails if the name already exists in that function
//   @param variable_type - Type string: bool, int, int64, float, string, name, text, byte, Vector, or a struct/enum/class/object name or path
//   @param default_value - Initial default value for the local variable, in exported-text form; optional
//   @returns { blueprint_name, function_name, variable_name, variable_type } -- an echo of the request; local variables do NOT appear in get_blueprint_variables (which lists members), so verify from the graph itself.
TSharedPtr<FJsonObject> HandleAddFunctionLocalVariable(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, FunctionName, VarName, VarType;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("function_name"), FunctionName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("variable_name"), VarName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("variable_type"), VarType, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UBlueprint* Blueprint = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::FindBlueprint(BlueprintName, Blueprint)) { return Error; }

    UEdGraph* FuncGraph = FindFunctionGraph(Blueprint, FunctionName);
    if (!FuncGraph)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Function graph not found: %s"), *FunctionName));

    FEdGraphPinType PinType;
    if (!FUEAgentGraphUtils::ResolvePinType(VarType, PinType))
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Could not resolve variable_type: %s"), *VarType));

    FString DefaultValue;
    Params->TryGetStringField(TEXT("default_value"), DefaultValue);

    FScopedTransaction Transaction(LOCTEXT("AddFnLocalVar", "MCP: Add Function Local Variable"));
    Blueprint->Modify();
    const bool bOk = FBlueprintEditorUtils::AddLocalVariable(Blueprint, FuncGraph, FName(*VarName), PinType, DefaultValue);
    if (!bOk)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("AddLocalVariable failed for '%s' (name may already exist)"), *VarName));

    TSharedPtr<FJsonObject> Result = UEAgentGraphCommandsImpl::BlueprintReply(BlueprintName, TEXT("function_name"), FunctionName);
    Result->SetStringField(TEXT("variable_name"), VarName);
    Result->SetStringField(TEXT("variable_type"), VarType);
    return Result;
}

// MCP:remove_function_parameter - Remove an input or output parameter from a Blueprint function graph -- interface-override graphs included -- or from a macro graph, in one undo step: the user pin leaves the Entry node (inputs) or every Result node (outputs), or the macro's input or output tunnel, and the node is reconstructed, so links to that pin break. Inverse of add_function_parameter; for an override of a C++ or interface function, reconcile_function_signature fits the signature to the parent instead. Nothing is compiled or saved: call sites update at the next compile_blueprint. A parameter that is not there is an honest no-op (removed=false, the parameters it does have in params); a missing Blueprint or graph is an error. The reply re-reads the signature (params_left). Read the pins: get_graph_nodes at compact=false. Refused during PIE unless force=true.
//   @param blueprint_name - Blueprint name or path.
//   @param function_name - Name of the function graph, or of a macro graph when no function has that name.
//   @param param_name - Name of the parameter to remove, as add_function_parameter returned it.
//   @param is_output - true to remove an output/return parameter, false (default) an input.
//   @returns { blueprint_name, function_name, param_name, is_output, graph_kind, removed, params_left: [string], params?: [string], note? }
TSharedPtr<FJsonObject> HandleRemoveFunctionParameter(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, FunctionName, ParamName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("function_name"), FunctionName)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("param_name"), ParamName)) { return Missing; }
    bool bIsOutput = false;
    Params->TryGetBoolField(TEXT("is_output"), bIsOutput);

    UBlueprint* Blueprint = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::FindBlueprint(BlueprintName, Blueprint)) { return Error; }

    UEdGraph* FuncGraph = FindFunctionGraph(Blueprint, FunctionName);
    UEdGraph* MacroGraph = FuncGraph ? nullptr : FindMacroGraph(Blueprint, FunctionName);
    if (!FuncGraph && !MacroGraph)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Function graph not found: %s (no macro graph has that name either); nothing was changed"), *FunctionName));

    const TArray<UK2Node_EditablePinBase*> Nodes = SignatureNodes(FuncGraph, MacroGraph, bIsOutput);
    const FName PinFName(*ParamName);
    TArray<UK2Node_EditablePinBase*> Holders;
    for (UK2Node_EditablePinBase* Node : Nodes)
    {
        if (Node->UserDefinedPins.ContainsByPredicate([&PinFName](const TSharedPtr<FUserPinInfo>& Info) { return Info.IsValid() && Info->PinName == PinFName; }))
        {
            Holders.Add(Node);
        }
    }

    TSharedPtr<FJsonObject> Result = UEAgentGraphCommandsImpl::BlueprintReply(BlueprintName, TEXT("function_name"), FunctionName);
    Result->SetStringField(TEXT("param_name"), ParamName);
    Result->SetBoolField(TEXT("is_output"), bIsOutput);
    Result->SetStringField(TEXT("graph_kind"), MacroGraph ? TEXT("macro") : TEXT("function"));
    if (Holders.Num() == 0)
    {
        const TArray<FString> Existing = UserPinNames(Nodes);
        Result->SetBoolField(TEXT("removed"), false);
        Result->SetArrayField(TEXT("params"), mcp_common::StringsToJson(Existing));
        Result->SetArrayField(TEXT("params_left"), mcp_common::StringsToJson(Existing));
        Result->SetStringField(TEXT("note"), FString::Printf(TEXT("the %s has no %s parameter named '%s'; nothing changed"),
            MacroGraph ? TEXT("macro") : TEXT("function"), bIsOutput ? TEXT("output") : TEXT("input"), *ParamName));
        return Result;
    }

    FScopedTransaction Transaction(LOCTEXT("RemoveFnParam", "MCP: Remove Function Parameter"));
    Blueprint->Modify();
    for (UK2Node_EditablePinBase* Node : Holders)
    {
        Node->Modify();
        Node->RemoveUserDefinedPinByName(PinFName);
        Node->ReconstructNode();
    }
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    // The re-read: no signature node keeps the parameter.
    const TArray<FString> Left = UserPinNames(SignatureNodes(FuncGraph, MacroGraph, bIsOutput));
    if (Left.Contains(ParamName))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("'%s' is still on the signature after RemoveUserDefinedPinByName"), *ParamName));
    }
    Result->SetBoolField(TEXT("removed"), true);
    Result->SetArrayField(TEXT("params_left"), mcp_common::StringsToJson(Left));
    return Result;
}

// MCP:remove_function_local_variable - Remove a local variable from one Blueprint function graph (interface-override graphs included), with the get/set nodes that read or write it in that function, in one undo step -- what deleting it from the My Blueprint panel does. Inverse of add_function_local_variable; delete_blueprint_variable removes MEMBER variables only. Nothing is compiled or saved. A local that is not there is an honest no-op (removed=false); a missing Blueprint or function is an error. The reply re-reads the function's locals (local_variables), the only reader of them: get_blueprint_variables lists members. Refused during PIE unless force=true.
//   @param blueprint_name - Blueprint asset name or path
//   @param function_name - Name of the function graph (FunctionGraphs or an interface-override graph)
//   @param variable_name - Name of the local variable to remove
//   @returns { blueprint_name, function_name, variable_name, removed, local_variables: [string], note? }
TSharedPtr<FJsonObject> HandleRemoveFunctionLocalVariable(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, FunctionName, VarName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("function_name"), FunctionName)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("variable_name"), VarName)) { return Missing; }

    UBlueprint* Blueprint = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::FindBlueprint(BlueprintName, Blueprint)) { return Error; }

    UEdGraph* FuncGraph = FindFunctionGraph(Blueprint, FunctionName);
    if (!FuncGraph)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Function graph not found: %s; nothing was changed"), *FunctionName));

    const auto LocalNames = [FuncGraph]()
    {
        TArray<FString> Names;
        TArray<UK2Node_FunctionEntry*> Entries;
        FuncGraph->GetNodesOfClass<UK2Node_FunctionEntry>(Entries);
        for (const UK2Node_FunctionEntry* Entry : Entries)
        {
            for (const FBPVariableDescription& Local : Entry->LocalVariables)
            {
                Names.AddUnique(Local.VarName.ToString());
            }
        }
        return Names;
    };

    const FName VarFName(*VarName);
    UK2Node_FunctionEntry* Entry = nullptr;
    FBPVariableDescription* Local = FBlueprintEditorUtils::FindLocalVariable(Blueprint, FuncGraph, VarFName, &Entry);
    TSharedPtr<FJsonObject> Result = UEAgentGraphCommandsImpl::BlueprintReply(BlueprintName, TEXT("function_name"), FunctionName);
    Result->SetStringField(TEXT("variable_name"), VarName);
    if (!Local || !Entry)
    {
        Result->SetBoolField(TEXT("removed"), false);
        Result->SetArrayField(TEXT("local_variables"), mcp_common::StringsToJson(LocalNames()));
        Result->SetStringField(TEXT("note"), TEXT("the function has no local variable of that name; nothing changed"));
        return Result;
    }

    // FBlueprintEditorUtils::RemoveLocalVariable, without its scope lookup: that one needs the function compiled into
    // the skeleton class, and a function just made has no UFunction there yet.
    FScopedTransaction Transaction(LOCTEXT("RemoveFnLocalVar", "MCP: Remove Function Local Variable"));
    Blueprint->Modify();
    Entry->Modify();
    Entry->LocalVariables.RemoveAll([&VarFName](const FBPVariableDescription& Var) { return Var.VarName == VarFName; });
    FBlueprintEditorUtils::RemoveVariableNodes(Blueprint, VarFName, /*bForSelfOnly=*/true, FuncGraph);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    if (FBlueprintEditorUtils::FindLocalVariable(Blueprint, FuncGraph, VarFName) != nullptr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Local variable '%s' is still on the function after the removal"), *VarName));
    }
    Result->SetBoolField(TEXT("removed"), true);
    Result->SetArrayField(TEXT("local_variables"), mcp_common::StringsToJson(LocalNames()));
    return Result;
}

// MCP:remove_blueprint_macro - Delete a macro graph owned by the Blueprint (Blueprint->MacroGraphs), in one undo step. Inverse of create_blueprint_macro; remove_blueprint_function deletes function graphs only. The macro's instances in the Blueprint's graphs go with it, so a macro still in use is refused (instance_count says how many) unless delete_instances=true. Nothing is compiled or saved. A macro that is not there is an honest no-op (removed=false); a missing Blueprint is an error. The reply re-reads MacroGraphs; list the graphs with get_blueprint_functions. Refused during PIE unless force=true.
//   @param blueprint_name - Blueprint name or path.
//   @param macro_name - Name of the macro graph, as create_blueprint_macro returned it in graph_name.
//   @param delete_instances - true to delete the macro even when nodes of this Blueprint instance it (they are deleted too); default false refuses.
//   @returns { blueprint_name, macro_name, removed, instance_count, instances_removed, macro_count, note? }
TSharedPtr<FJsonObject> HandleRemoveBlueprintMacro(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, MacroName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("macro_name"), MacroName)) { return Missing; }
    bool bDeleteInstances = false;
    Params->TryGetBoolField(TEXT("delete_instances"), bDeleteInstances);

    UBlueprint* Blueprint = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::FindBlueprint(BlueprintName, Blueprint)) { return Error; }

    TSharedPtr<FJsonObject> Result = UEAgentGraphCommandsImpl::BlueprintReply(BlueprintName, TEXT("macro_name"), MacroName);
    UEdGraph* MacroGraph = FindMacroGraph(Blueprint, MacroName);
    if (!MacroGraph)
    {
        Result->SetBoolField(TEXT("removed"), false);
        Result->SetNumberField(TEXT("instance_count"), 0);
        Result->SetNumberField(TEXT("instances_removed"), 0);
        Result->SetNumberField(TEXT("macro_count"), Blueprint->MacroGraphs.Num());
        Result->SetStringField(TEXT("note"), TEXT("the Blueprint owns no macro of that name; nothing changed"));
        return Result;
    }

    TArray<UK2Node_MacroInstance*> MacroNodes;
    FBlueprintEditorUtils::GetAllNodesOfClass(Blueprint, MacroNodes);
    int32 InstanceCount = 0;
    for (const UK2Node_MacroInstance* Node : MacroNodes)
    {
        if (Node && Node->GetMacroGraph() == MacroGraph)
        {
            ++InstanceCount;
        }
    }
    if (InstanceCount > 0 && !bDeleteInstances)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Macro '%s' is instanced by %d node(s) of this Blueprint, which would be deleted with it; pass delete_instances=true to delete them too. Nothing was changed."),
            *MacroName, InstanceCount));
    }

    // RemoveGraph also deletes the macro's instance nodes; MarkTransient without Recompile, as create_blueprint_macro
    // compiles nothing either.
    FScopedTransaction Transaction(LOCTEXT("RemoveBlueprintMacro", "MCP: Remove Blueprint Macro"));
    Blueprint->Modify();
    FBlueprintEditorUtils::RemoveGraph(Blueprint, MacroGraph, EGraphRemoveFlags::MarkTransient);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    if (FindMacroGraph(Blueprint, MacroName) != nullptr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Macro '%s' is still in MacroGraphs after RemoveGraph"), *MacroName));
    }
    Result->SetBoolField(TEXT("removed"), true);
    Result->SetNumberField(TEXT("instance_count"), InstanceCount);
    Result->SetNumberField(TEXT("instances_removed"), InstanceCount);
    Result->SetNumberField(TEXT("macro_count"), Blueprint->MacroGraphs.Num());
    return Result;
}

// ---------------------------------------------------------------------------
// Blueprint Timeline tracks: add_blueprint_timeline_track / remove_blueprint_timeline_track and the reader
// get_blueprint_timeline_info. A track lives on the UTimelineTemplate (FloatTracks, VectorTracks,
// LinearColorTracks, EventTracks) with its curve outered to the generated class, as the Timeline editor makes it
// (STimelineEditor::CreateNewTrack); the UK2Node_Timeline is reconstructed so the track's pin appears, and the
// Blueprint is compiled, as add_timeline_to_blueprint does.
// ---------------------------------------------------------------------------
namespace
{
    const TCHAR* TimelineTrackTypeName(int32 TrackType)
    {
        switch (TrackType)
        {
        case FTTTrackBase::TT_Event:             return TEXT("event");
        case FTTTrackBase::TT_FloatInterp:       return TEXT("float");
        case FTTTrackBase::TT_VectorInterp:      return TEXT("vector");
        case FTTTrackBase::TT_LinearColorInterp: return TEXT("color");
        default:                                 return TEXT("unknown");
        }
    }

    /** The timeline TimelineName names (its variable name) and its node; the error reply when the timeline is missing. */
    TSharedPtr<FJsonObject> FindTimelineTarget(UBlueprint* Blueprint, const FString& TimelineName,
        UTimelineTemplate*& OutTemplate, UK2Node_Timeline*& OutNode)
    {
        OutTemplate = Blueprint->FindTimelineTemplateByVariableName(FName(*TimelineName));
        OutNode = OutTemplate ? FBlueprintEditorUtils::FindNodeForTimeline(Blueprint, OutTemplate) : nullptr;
        if (!OutTemplate)
        {
            TArray<FString> Names;
            for (const UTimelineTemplate* Template : Blueprint->Timelines)
            {
                if (Template)
                {
                    Names.Add(Template->GetVariableName().ToString());
                }
            }
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("No timeline '%s' on this Blueprint (its timelines: %s); nothing was changed"),
                *TimelineName, Names.Num() ? *FString::Join(Names, TEXT(", ")) : TEXT("none")));
        }
        return nullptr;
    }

    /** One key as {time, value}: Value is a number, or an array for a vector or color track. */
    TSharedPtr<FJsonValue> TimelineKeyJson(double Time, const TSharedPtr<FJsonValue>& Value)
    {
        TSharedPtr<FJsonObject> Key = MakeShared<FJsonObject>();
        Key->SetNumberField(TEXT("time"), Time);
        Key->SetField(TEXT("value"), Value);
        return MakeShared<FJsonValueObject>(Key);
    }

    /** The keys of a multi-channel curve as {time, value:[...]}, one per distinct key time across the channels. */
    TArray<TSharedPtr<FJsonValue>> MultiChannelKeysJson(const FRichCurve* Channels, int32 NumChannels)
    {
        TArray<float> Times;
        for (int32 Channel = 0; Channel < NumChannels; ++Channel)
        {
            for (const FRichCurveKey& Key : Channels[Channel].GetConstRefOfKeys())
            {
                Times.AddUnique(Key.Time);
            }
        }
        Times.Sort();
        TArray<TSharedPtr<FJsonValue>> Out;
        for (const float Time : Times)
        {
            TArray<TSharedPtr<FJsonValue>> Values;
            for (int32 Channel = 0; Channel < NumChannels; ++Channel)
            {
                Values.Add(MakeShared<FJsonValueNumber>(Channels[Channel].Eval(Time)));
            }
            Out.Add(TimelineKeyJson(Time, MakeShared<FJsonValueArray>(Values)));
        }
        return Out;
    }

    TSharedPtr<FJsonObject> TimelineTrackJson(const TCHAR* Type, const FTTTrackBase& Track, const UCurveBase* Curve, TArray<TSharedPtr<FJsonValue>> Keys)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetStringField(TEXT("track_name"), Track.GetTrackName().ToString());
        Out->SetStringField(TEXT("track_type"), Type);
        Out->SetStringField(TEXT("curve"), Curve ? Curve->GetPathName() : FString());
        Out->SetBoolField(TEXT("external_curve"), Track.bIsExternalCurve);
        Out->SetNumberField(TEXT("key_count"), Keys.Num());
        Out->SetArrayField(TEXT("keys"), Keys);
        return Out;
    }

    /** Every track of Template as JSON, event tracks first, as the Timeline editor groups them. */
    TArray<TSharedPtr<FJsonValue>> TimelineTracksJson(const UTimelineTemplate* Template)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        for (const FTTEventTrack& Track : Template->EventTracks)
        {
            TArray<TSharedPtr<FJsonValue>> Keys;
            if (Track.CurveKeys)
            {
                for (const FRichCurveKey& Key : Track.CurveKeys->FloatCurve.GetConstRefOfKeys())
                {
                    Keys.Add(TimelineKeyJson(Key.Time, MakeShared<FJsonValueNumber>(Key.Value)));
                }
            }
            Out.Add(MakeShared<FJsonValueObject>(TimelineTrackJson(TEXT("event"), Track, Track.CurveKeys, Keys)));
        }
        for (const FTTFloatTrack& Track : Template->FloatTracks)
        {
            TArray<TSharedPtr<FJsonValue>> Keys;
            if (Track.CurveFloat)
            {
                for (const FRichCurveKey& Key : Track.CurveFloat->FloatCurve.GetConstRefOfKeys())
                {
                    Keys.Add(TimelineKeyJson(Key.Time, MakeShared<FJsonValueNumber>(Key.Value)));
                }
            }
            Out.Add(MakeShared<FJsonValueObject>(TimelineTrackJson(TEXT("float"), Track, Track.CurveFloat, Keys)));
        }
        for (const FTTVectorTrack& Track : Template->VectorTracks)
        {
            Out.Add(MakeShared<FJsonValueObject>(TimelineTrackJson(TEXT("vector"), Track, Track.CurveVector,
                Track.CurveVector ? MultiChannelKeysJson(Track.CurveVector->FloatCurves, 3) : TArray<TSharedPtr<FJsonValue>>())));
        }
        for (const FTTLinearColorTrack& Track : Template->LinearColorTracks)
        {
            Out.Add(MakeShared<FJsonValueObject>(TimelineTrackJson(TEXT("color"), Track, Track.CurveLinearColor,
                Track.CurveLinearColor ? MultiChannelKeysJson(Track.CurveLinearColor->FloatCurves, 4) : TArray<TSharedPtr<FJsonValue>>())));
        }
        return Out;
    }

    /** The type and index of the track named TrackName on Template; false when no track has that name. */
    bool FindTimelineTrack(const UTimelineTemplate* Template, const FName TrackName, int32& OutType, int32& OutIndex)
    {
        OutIndex = Template->EventTracks.IndexOfByPredicate([&TrackName](const FTTEventTrack& T) { return T.GetTrackName() == TrackName; });
        if (OutIndex != INDEX_NONE) { OutType = FTTTrackBase::TT_Event; return true; }
        OutIndex = Template->FloatTracks.IndexOfByPredicate([&TrackName](const FTTFloatTrack& T) { return T.GetTrackName() == TrackName; });
        if (OutIndex != INDEX_NONE) { OutType = FTTTrackBase::TT_FloatInterp; return true; }
        OutIndex = Template->VectorTracks.IndexOfByPredicate([&TrackName](const FTTVectorTrack& T) { return T.GetTrackName() == TrackName; });
        if (OutIndex != INDEX_NONE) { OutType = FTTTrackBase::TT_VectorInterp; return true; }
        OutIndex = Template->LinearColorTracks.IndexOfByPredicate([&TrackName](const FTTLinearColorTrack& T) { return T.GetTrackName() == TrackName; });
        if (OutIndex != INDEX_NONE) { OutType = FTTTrackBase::TT_LinearColorInterp; return true; }
        return false;
    }

    /** The timeline's settings, tracks and node pins: what get_blueprint_timeline_info returns and the writers re-read. */
    void AddTimelineState(const TSharedPtr<FJsonObject>& Out, const UTimelineTemplate* Template, const UK2Node_Timeline* Node)
    {
        Out->SetNumberField(TEXT("length"), Template->TimelineLength);
        Out->SetStringField(TEXT("length_mode"), Template->LengthMode == ETimelineLengthMode::TL_LastKeyFrame ? TEXT("LastKeyFrame") : TEXT("TimelineLength"));
        Out->SetBoolField(TEXT("auto_play"), Template->bAutoPlay != 0);
        Out->SetBoolField(TEXT("loop"), Template->bLoop != 0);
        Out->SetBoolField(TEXT("replicated"), Template->bReplicated != 0);
        Out->SetBoolField(TEXT("ignore_time_dilation"), Template->bIgnoreTimeDilation != 0);
        const TArray<TSharedPtr<FJsonValue>> Tracks = TimelineTracksJson(Template);
        Out->SetNumberField(TEXT("track_count"), Tracks.Num());
        Out->SetArrayField(TEXT("tracks"), Tracks);
        if (Node)
        {
            Out->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
            Out->SetStringField(TEXT("graph_name"), Node->GetGraph() ? Node->GetGraph()->GetName() : FString());
            TArray<FString> PinNames;
            for (const UEdGraphPin* Pin : Node->Pins)
            {
                if (Pin && Pin->Direction == EGPD_Output)
                {
                    PinNames.Add(Pin->PinName.ToString());
                }
            }
            Out->SetArrayField(TEXT("output_pins"), mcp_common::StringsToJson(PinNames));
        }
    }

    /** The channel values of one key value into Out: a number for one component, else an array of that many (a color may omit alpha). */
    bool ReadKeyComponents(const TSharedPtr<FJsonValue>& Value, int32 NumComponents, TArray<float>& Out)
    {
        Out.Reset();
        if (NumComponents == 1)
        {
            double Number = 0.0;
            if (!Value.IsValid() || !Value->TryGetNumber(Number)) { return false; }
            Out.Add(static_cast<float>(Number));
            return true;
        }
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (!Value.IsValid() || !Value->TryGetArray(Arr)) { return false; }
        // A color takes 3 or 4 components (alpha 1 when omitted); a vector exactly 3.
        if (Arr->Num() != NumComponents && !(NumComponents == 4 && Arr->Num() == 3)) { return false; }
        for (const TSharedPtr<FJsonValue>& Component : *Arr)
        {
            double Number = 0.0;
            if (!Component.IsValid() || !Component->TryGetNumber(Number)) { return false; }
            Out.Add(static_cast<float>(Number));
        }
        if (Out.Num() == 3 && NumComponents == 4) { Out.Add(1.0f); }
        return true;
    }
}

// MCP:add_blueprint_timeline_track - Add a track to a Timeline of a Blueprint, with optional keys, as the Timeline editor's "+ Track" does: a float, vector, color or event track whose curve is made inside the Blueprint, then the timeline node is reconstructed (a float/vector/color track gets an output pin of its name, an event track an exec output) and the Blueprint compiled; one undo step (the compile is not undone). The reply re-reads the timeline (tracks, output_pins). keys is [[time, value], ...]: value a number for float, [x,y,z] for vector, [r,g,b] or [r,g,b,a] for color, nothing for event ([time] or a bare time). A track name already used on the timeline, a malformed key or an unknown track_type is an error and nothing changes; so is a missing Blueprint or timeline. Keys past the timeline length stay but play only if length grows. Refused during PIE unless force=true. Timelines come from add_timeline_to_blueprint; read: get_blueprint_timeline_info; inverse: remove_blueprint_timeline_track.
//   @param blueprint_name - Name (or full object path) of an Actor Blueprint.
//   @param timeline_name - The timeline's variable name, as add_timeline_to_blueprint returned it.
//   @param track_type - float | vector | color | event
//   @param track_name - Name of the new track, unique on this timeline; it names the node's output pin.
//   @param keys [array] - Optional keys, [[time, value], ...] (see the description for value by track type).
//   @param interpolation - Optional key interpolation for float/vector/color: "linear" (default) | "cubic" | "constant".
//   @returns { blueprint_name, timeline_name, track_name, track_type, added, keys_added, curve, length, length_mode, auto_play, loop, replicated, ignore_time_dilation, track_count, tracks: [{track_name, track_type, curve, external_curve, key_count, keys: [{time, value}]}], node_id, graph_name, output_pins: [string] }
TSharedPtr<FJsonObject> HandleAddBlueprintTimelineTrack(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, TimelineName, TrackTypeRaw, TrackName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("timeline_name"), TimelineName)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("track_type"), TrackTypeRaw)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("track_name"), TrackName)) { return Missing; }

    const FString TrackType = TrackTypeRaw.ToLower();
    int32 Type = INDEX_NONE;
    int32 NumComponents = 1;
    if (TrackType == TEXT("float")) { Type = FTTTrackBase::TT_FloatInterp; }
    else if (TrackType == TEXT("vector")) { Type = FTTTrackBase::TT_VectorInterp; NumComponents = 3; }
    else if (TrackType == TEXT("color") || TrackType == TEXT("linearcolor")) { Type = FTTTrackBase::TT_LinearColorInterp; NumComponents = 4; }
    else if (TrackType == TEXT("event")) { Type = FTTTrackBase::TT_Event; NumComponents = 0; }
    if (Type == INDEX_NONE)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Unknown track_type '%s' (use float, vector, color or event); nothing was changed"), *TrackTypeRaw));
    }

    FString InterpRaw = TEXT("linear");
    Params->TryGetStringField(TEXT("interpolation"), InterpRaw);
    const FString Interp = InterpRaw.ToLower();
    ERichCurveInterpMode InterpMode = RCIM_Linear;
    if (Interp == TEXT("cubic")) { InterpMode = RCIM_Cubic; }
    else if (Interp == TEXT("constant")) { InterpMode = RCIM_Constant; }
    else if (Interp != TEXT("linear"))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Unknown interpolation '%s' (use linear, cubic or constant); nothing was changed"), *InterpRaw));
    }

    // Every key parses before anything is touched.
    struct FParsedKey { float Time = 0.0f; TArray<float> Values; };
    TArray<FParsedKey> Keys;
    const TArray<TSharedPtr<FJsonValue>>* KeysJson = nullptr;
    if (Params->TryGetArrayField(TEXT("keys"), KeysJson))
    {
        for (int32 Index = 0; Index < KeysJson->Num(); ++Index)
        {
            const TSharedPtr<FJsonValue>& Entry = (*KeysJson)[Index];
            FParsedKey Parsed;
            double Time = 0.0;
            const TArray<TSharedPtr<FJsonValue>>* Pair = nullptr;
            bool bOk = false;
            if (Entry.IsValid() && Entry->TryGetArray(Pair) && Pair->Num() >= 1 && (*Pair)[0]->TryGetNumber(Time))
            {
                Parsed.Time = static_cast<float>(Time);
                bOk = NumComponents == 0
                    ? Pair->Num() <= 2
                    : (Pair->Num() == 2 && ReadKeyComponents((*Pair)[1], NumComponents, Parsed.Values));
            }
            else if (NumComponents == 0 && Entry.IsValid() && Entry->TryGetNumber(Time))
            {
                Parsed.Time = static_cast<float>(Time);
                bOk = true;
            }
            if (!bOk)
            {
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("keys[%d] is not a valid %s key: give [time, %s]; nothing was changed"), Index, *TrackType,
                    NumComponents == 0 ? TEXT("(no value)") : NumComponents == 1 ? TEXT("number") : NumComponents == 3 ? TEXT("[x,y,z]") : TEXT("[r,g,b(,a)]")));
            }
            Keys.Add(MoveTemp(Parsed));
        }
    }

    UBlueprint* Blueprint = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::FindBlueprint(BlueprintName, Blueprint)) { return Error; }
    UTimelineTemplate* Template = nullptr;
    UK2Node_Timeline* Node = nullptr;
    if (TSharedPtr<FJsonObject> Error = FindTimelineTarget(Blueprint, TimelineName, Template, Node)) { return Error; }
    if (!Node || !Node->GetGraph())
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Timeline '%s' has no node in any graph to reconstruct; nothing was changed"), *TimelineName));
    }
    const FName TrackFName(*TrackName);
    if (!Template->IsNewTrackNameValid(TrackFName))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Timeline '%s' already has a track named '%s'; nothing was changed"), *TimelineName, *TrackName));
    }
    UClass* OwnerClass = Blueprint->GeneratedClass;
    if (!OwnerClass)
    {
        return mcp_common::MakeErrorReply(TEXT("The Blueprint has no generated class yet (compile_blueprint first); nothing was changed"));
    }

    FScopedTransaction Transaction(LOCTEXT("AddTimelineTrack", "MCP: Add Blueprint Timeline Track"));
    Blueprint->Modify();
    Node->Modify();
    Template->Modify();

    // The curve is public so the timeline instances of placed actors can reference it, as the Timeline editor makes it.
    const auto AddScalarKeys = [&Keys, InterpMode](FRichCurve& Curve, int32 Component, bool bEvent)
    {
        for (const FParsedKey& Key : Keys)
        {
            const FKeyHandle Handle = Curve.AddKey(Key.Time, bEvent ? 0.0f : Key.Values[Component]);
            if (!bEvent)
            {
                Curve.SetKeyInterpMode(Handle, InterpMode);
            }
        }
    };
    FTTTrackId NewTrackId;
    NewTrackId.TrackType = Type;
    UCurveBase* Curve = nullptr;
    if (Type == FTTTrackBase::TT_Event)
    {
        NewTrackId.TrackIndex = Template->EventTracks.Num();
        FTTEventTrack NewTrack;
        NewTrack.SetTrackName(TrackFName, Template);
        NewTrack.CurveKeys = NewObject<UCurveFloat>(OwnerClass, NAME_None, RF_Public);
        NewTrack.CurveKeys->bIsEventCurve = true;
        AddScalarKeys(NewTrack.CurveKeys->FloatCurve, 0, /*bEvent=*/true);
        Curve = NewTrack.CurveKeys;
        Template->EventTracks.Add(NewTrack);
    }
    else if (Type == FTTTrackBase::TT_FloatInterp)
    {
        NewTrackId.TrackIndex = Template->FloatTracks.Num();
        FTTFloatTrack NewTrack;
        NewTrack.SetTrackName(TrackFName, Template);
        NewTrack.CurveFloat = NewObject<UCurveFloat>(OwnerClass, NAME_None, RF_Public);
        AddScalarKeys(NewTrack.CurveFloat->FloatCurve, 0, /*bEvent=*/false);
        Curve = NewTrack.CurveFloat;
        Template->FloatTracks.Add(NewTrack);
    }
    else if (Type == FTTTrackBase::TT_VectorInterp)
    {
        NewTrackId.TrackIndex = Template->VectorTracks.Num();
        FTTVectorTrack NewTrack;
        NewTrack.SetTrackName(TrackFName, Template);
        NewTrack.CurveVector = NewObject<UCurveVector>(OwnerClass, NAME_None, RF_Public);
        for (int32 Component = 0; Component < 3; ++Component)
        {
            AddScalarKeys(NewTrack.CurveVector->FloatCurves[Component], Component, /*bEvent=*/false);
        }
        Curve = NewTrack.CurveVector;
        Template->VectorTracks.Add(NewTrack);
    }
    else
    {
        NewTrackId.TrackIndex = Template->LinearColorTracks.Num();
        FTTLinearColorTrack NewTrack;
        NewTrack.SetTrackName(TrackFName, Template);
        NewTrack.CurveLinearColor = NewObject<UCurveLinearColor>(OwnerClass, NAME_None, RF_Public);
        for (int32 Component = 0; Component < 4; ++Component)
        {
            AddScalarKeys(NewTrack.CurveLinearColor->FloatCurves[Component], Component, /*bEvent=*/false);
        }
        Curve = NewTrack.CurveLinearColor;
        Template->LinearColorTracks.Add(NewTrack);
    }
    Template->AddDisplayTrack(NewTrackId);
    Node->ReconstructNode();
    UEAgentGraphCommandsImpl::MarkModifiedAndCompile(Blueprint, Node->GetGraph());

    // The re-read, after the compile: the template the Blueprint holds now.
    UTimelineTemplate* After = Blueprint->FindTimelineTemplateByVariableName(FName(*TimelineName));
    int32 FoundType = INDEX_NONE, FoundIndex = INDEX_NONE;
    if (!After || !FindTimelineTrack(After, TrackFName, FoundType, FoundIndex))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Track '%s' is not on timeline '%s' after the compile"), *TrackName, *TimelineName));
    }
    TSharedPtr<FJsonObject> Result = UEAgentGraphCommandsImpl::BlueprintReply(BlueprintName, TEXT("timeline_name"), TimelineName);
    Result->SetStringField(TEXT("track_name"), TrackName);
    Result->SetStringField(TEXT("track_type"), TimelineTrackTypeName(FoundType));
    Result->SetBoolField(TEXT("added"), true);
    Result->SetNumberField(TEXT("keys_added"), Keys.Num());
    Result->SetStringField(TEXT("curve"), Curve ? Curve->GetPathName() : FString());
    AddTimelineState(Result, After, FBlueprintEditorUtils::FindNodeForTimeline(Blueprint, After));
    return Result;
}

// MCP:remove_blueprint_timeline_track - Remove a track (float, vector, color or event) from a Timeline of a Blueprint by its name, as the Timeline editor's Delete Track does, then reconstruct the timeline node (the track's output pin goes, and its links with it) and compile the Blueprint; one undo step (the compile is not undone). Inverse of add_blueprint_timeline_track. A track that is not there is an honest no-op (removed=false, nothing compiled); a missing Blueprint or timeline is an error. The reply re-reads the timeline (tracks, output_pins). Refused during PIE unless force=true. The whole timeline: delete_node on its node; read: get_blueprint_timeline_info.
//   @param blueprint_name - Name (or full object path) of an Actor Blueprint.
//   @param timeline_name - The timeline's variable name.
//   @param track_name - Name of the track to remove (get_blueprint_timeline_info lists them).
//   @returns { blueprint_name, timeline_name, track_name, track_type?, removed, length, length_mode, auto_play, loop, replicated, ignore_time_dilation, track_count, tracks: [...], node_id, graph_name, output_pins: [string], note? }
TSharedPtr<FJsonObject> HandleRemoveBlueprintTimelineTrack(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, TimelineName, TrackName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("timeline_name"), TimelineName)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("track_name"), TrackName)) { return Missing; }

    UBlueprint* Blueprint = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::FindBlueprint(BlueprintName, Blueprint)) { return Error; }
    UTimelineTemplate* Template = nullptr;
    UK2Node_Timeline* Node = nullptr;
    if (TSharedPtr<FJsonObject> Error = FindTimelineTarget(Blueprint, TimelineName, Template, Node)) { return Error; }

    TSharedPtr<FJsonObject> Result = UEAgentGraphCommandsImpl::BlueprintReply(BlueprintName, TEXT("timeline_name"), TimelineName);
    Result->SetStringField(TEXT("track_name"), TrackName);
    const FName TrackFName(*TrackName);
    int32 Type = INDEX_NONE, Index = INDEX_NONE;
    if (!FindTimelineTrack(Template, TrackFName, Type, Index))
    {
        Result->SetBoolField(TEXT("removed"), false);
        Result->SetStringField(TEXT("note"), TEXT("the timeline has no track of that name; nothing changed"));
        AddTimelineState(Result, Template, Node);
        return Result;
    }
    if (!Node || !Node->GetGraph())
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Timeline '%s' has no node in any graph to reconstruct; nothing was changed"), *TimelineName));
    }

    FScopedTransaction Transaction(LOCTEXT("RemoveTimelineTrack", "MCP: Remove Blueprint Timeline Track"));
    Blueprint->Modify();
    Node->Modify();
    Template->Modify();

    // The display order indexes each track array, so its entry goes first (RemoveDisplayTrack shifts the later
    // entries of the same type down), then the track itself.
    for (int32 Display = 0; Display < Template->GetNumDisplayTracks(); ++Display)
    {
        const FTTTrackId Id = Template->GetDisplayTrackId(Display);
        if (Id.TrackType == Type && Id.TrackIndex == Index)
        {
            Template->RemoveDisplayTrack(Display);
            break;
        }
    }
    switch (Type)
    {
    case FTTTrackBase::TT_Event:       Template->EventTracks.RemoveAt(Index); break;
    case FTTTrackBase::TT_FloatInterp: Template->FloatTracks.RemoveAt(Index); break;
    case FTTTrackBase::TT_VectorInterp: Template->VectorTracks.RemoveAt(Index); break;
    default:                           Template->LinearColorTracks.RemoveAt(Index); break;
    }
    Node->ReconstructNode();
    UEAgentGraphCommandsImpl::MarkModifiedAndCompile(Blueprint, Node->GetGraph());

    UTimelineTemplate* After = Blueprint->FindTimelineTemplateByVariableName(FName(*TimelineName));
    int32 StillType = INDEX_NONE, StillIndex = INDEX_NONE;
    if (!After || FindTimelineTrack(After, TrackFName, StillType, StillIndex))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Track '%s' is still on timeline '%s' after the removal"), *TrackName, *TimelineName));
    }
    Result->SetStringField(TEXT("track_type"), TimelineTrackTypeName(Type));
    Result->SetBoolField(TEXT("removed"), true);
    AddTimelineState(Result, After, FBlueprintEditorUtils::FindNodeForTimeline(Blueprint, After));
    return Result;
}

// MCP:get_blueprint_timeline_info - Read one Timeline of a Blueprint: its length and length mode, auto_play, loop, replicated and ignore_time_dilation, every track (float, vector, color, event) with its curve and keys ({time, value}; value an array for vector and color), and its node (node_id, graph_name, output_pins: Update, Finished, Direction and one pin per track). Read-only. Timelines are made by add_timeline_to_blueprint and their tracks by add_blueprint_timeline_track / remove_blueprint_timeline_track; get_timeline_info is about Level Sequences. A missing Blueprint or timeline is an error listing the timelines it has.
//   @param blueprint_name - Name (or full object path) of the Blueprint.
//   @param timeline_name - The timeline's variable name, as add_timeline_to_blueprint returned it.
//   @returns { blueprint_name, timeline_name, length, length_mode, auto_play, loop, replicated, ignore_time_dilation, track_count, tracks: [{track_name, track_type, curve, external_curve, key_count, keys: [{time, value}]}], node_id?, graph_name?, output_pins?: [string] }
TSharedPtr<FJsonObject> HandleGetBlueprintTimelineInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, TimelineName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("timeline_name"), TimelineName)) { return Missing; }

    UBlueprint* Blueprint = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::FindBlueprint(BlueprintName, Blueprint)) { return Error; }
    UTimelineTemplate* Template = nullptr;
    UK2Node_Timeline* Node = nullptr;
    if (TSharedPtr<FJsonObject> Error = FindTimelineTarget(Blueprint, TimelineName, Template, Node)) { return Error; }

    TSharedPtr<FJsonObject> Result = UEAgentGraphCommandsImpl::BlueprintReply(BlueprintName, TEXT("timeline_name"), TimelineName);
    AddTimelineState(Result, Template, Node);
    return Result;
}

// MCP:add_event_dispatcher - Create an event dispatcher (multicast delegate) on a Blueprint: adds BOTH the delegate member variable and its signature graph, the way the editor's "+ Event Dispatcher" does. It starts with NO parameters -- give it a signature with set_event_dispatcher_signature. Everything downstream needs the class REGENERATED first, so compile_blueprint before add_broadcast_node (to fire it) or add_graph_node node_type="add_delegate" / add_object_bound_event (to listen).
//   @param blueprint_name - Blueprint asset name or path
//   @param dispatcher_name - Name of the new event dispatcher; fails if a member variable of that name already exists
//   @returns { blueprint_name, dispatcher_name, created } -- created is always true on success; confirm with get_event_dispatchers.
TSharedPtr<FJsonObject> HandleAddEventDispatcher(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, DispatcherName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("dispatcher_name"), DispatcherName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UBlueprint* Blueprint = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::FindBlueprint(BlueprintName, Blueprint)) { return Error; }

    const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
    const FName Name(*DispatcherName);

    // Mirror FBlueprintEditor::OnAddNewDelegate.
    FScopedTransaction Transaction(LOCTEXT("AddDispatcher", "MCP: Add Event Dispatcher"));
    Blueprint->Modify();

    FEdGraphPinType DelegateType;
    DelegateType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
    if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, Name, DelegateType))
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Could not create delegate variable '%s' (name may already exist)"), *DispatcherName));

    UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, Name, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    if (!NewGraph)
    {
        FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, Name);
        return mcp_common::MakeErrorReply(TEXT("Failed to create the delegate signature graph"));
    }

    NewGraph->bEditable = false;
    K2Schema->CreateDefaultNodesForGraph(*NewGraph);
    K2Schema->CreateFunctionGraphTerminators(*NewGraph, (UClass*)nullptr);
    K2Schema->AddExtraFunctionFlags(NewGraph, (FUNC_BlueprintCallable | FUNC_BlueprintEvent | FUNC_Public));
    K2Schema->MarkFunctionEntryAsEditable(NewGraph, true);

    Blueprint->DelegateSignatureGraphs.Add(NewGraph);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    TSharedPtr<FJsonObject> Result = UEAgentGraphCommandsImpl::BlueprintReply(BlueprintName, TEXT("dispatcher_name"), DispatcherName);
    Result->SetBoolField(TEXT("created"), true);
    return Result;
}

// MCP:remove_event_dispatcher - Remove an event dispatcher, dropping BOTH artifacts add_event_dispatcher created: the delegate member variable AND its signature graph (delete_blueprint_variable removes only the variable and strands the graph, which is why this exists). Idempotent -- when neither artifact is present it returns removed:false instead of erroring. Broadcast/bind nodes that referenced it are left broken: compile_blueprint with return_messages=true after.
//   @param blueprint_name - Blueprint asset name or path
//   @param dispatcher_name - Name of the event dispatcher to remove (must be one this Blueprint owns; inherited ones have no local artifacts)
//   @returns { blueprint_name, dispatcher_name, removed } -- removed:false means nothing was there (no-op), not a failure.
TSharedPtr<FJsonObject> HandleRemoveEventDispatcher(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, DispatcherName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("dispatcher_name"), DispatcherName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UBlueprint* Blueprint = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::FindBlueprint(BlueprintName, Blueprint)) { return Error; }

    const FName Name(*DispatcherName);

    // add_event_dispatcher creates TWO artifacts: a member-variable multicast delegate AND a
    // signature graph in DelegateSignatureGraphs (named identically). Remove BOTH, mirroring
    // the editor's SMyBlueprint delegate-delete path; dropping only the variable (like
    // delete_blueprint_variable) would strand the signature graph.
    const bool bHasVar = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, Name) != INDEX_NONE;
    UEdGraph* SignatureGraph = nullptr;
    for (UEdGraph* Graph : Blueprint->DelegateSignatureGraphs)
    {
        if (Graph && Graph->GetFName() == Name)
        {
            SignatureGraph = Graph;
            break;
        }
    }

    // Idempotency: neither artifact present -> no-op, safe to re-run.
    if (!bHasVar && !SignatureGraph)
    {
        TSharedPtr<FJsonObject> Result = UEAgentGraphCommandsImpl::BlueprintReply(BlueprintName, TEXT("dispatcher_name"), DispatcherName);
        Result->SetBoolField(TEXT("removed"), false);
        return Result;
    }

    FScopedTransaction Transaction(LOCTEXT("RemoveDispatcher", "MCP: Remove Event Dispatcher"));
    Blueprint->Modify();

    // RemoveGraph internally erases the graph from Blueprint->DelegateSignatureGraphs (see
    // FBlueprintEditorUtils::RemoveGraph); Recompile flag rebuilds the blueprint afterwards.
    if (SignatureGraph)
    {
        FBlueprintEditorUtils::RemoveGraph(Blueprint, SignatureGraph, EGraphRemoveFlags::Recompile);
    }
    if (bHasVar)
    {
        FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, Name);
    }
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    TSharedPtr<FJsonObject> Result = UEAgentGraphCommandsImpl::BlueprintReply(BlueprintName, TEXT("dispatcher_name"), DispatcherName);
    Result->SetBoolField(TEXT("removed"), true);
    return Result;
}

// MCP:set_event_dispatcher_signature - Add to (mode="add", the idempotent default) or rewrite (mode="replace") the parameter list of a dispatcher this Blueprint OWNS -- inherited dispatchers have no local signature graph and are refused with the list of owned ones. Every requested type is resolved BEFORE anything is mutated, so a bad type aborts cleanly. The parameters become the OUTPUT pins of every bound handler and the INPUT pins of add_broadcast_node; existing nodes keep stale pins until compile_blueprint reconstructs them.
//   @param blueprint_name - Blueprint asset name or path that OWNS the dispatcher (inherited dispatchers cannot be edited here).
//   @param dispatcher_name - Name of an existing event dispatcher (created by add_event_dispatcher or the editor).
//   @param params - Array of parameter specs to add: [ { name: string, type: string } ]. Dispatcher params are input-only; 'type' accepts the same strings as add_function_parameter (bool, int, int64, float, string, name, text, byte, Vector, Array<T>, an object/class/struct/enum name or path).
//   @param mode - "add" (default, idempotent: skip a param whose name already exists) or "replace" (clear the existing signature first, then add the given params).
//   @returns { blueprint_name, dispatcher_name, mode, added: [string], params: [ { name, type } ], param_count }
//   Note: changing a dispatcher already bound in existing graphs can leave stale broadcast/bound-event pins; a follow-up compile_blueprint surfaces and reconstructs them (standard BP behavior).
TSharedPtr<FJsonObject> HandleSetEventDispatcherSignature(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, DispatcherName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("dispatcher_name"), DispatcherName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    const TArray<TSharedPtr<FJsonValue>>* ParamSpecs = nullptr;
    if (!Params->TryGetArrayField(TEXT("params"), ParamSpecs) || ParamSpecs->Num() == 0)
        return mcp_common::MakeErrorReply(TEXT("Missing or empty required parameter: params (array of { name, type })"));

    FString Mode = TEXT("add");
    Params->TryGetStringField(TEXT("mode"), Mode);
    Mode = Mode.ToLower();
    const bool bReplace = (Mode == TEXT("replace") || Mode == TEXT("mirror"));
    if (Mode != TEXT("add") && !bReplace)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Invalid mode '%s'. Expected 'add' or 'replace'."), *Mode));

    UBlueprint* Blueprint = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::FindBlueprint(BlueprintName, Blueprint)) { return Error; }

    // The signature of a dispatcher lives in its per-dispatcher UEdGraph in DelegateSignatureGraphs
    // (an OWN dispatcher). Inherited dispatchers have no local graph -> not editable here. This mirrors
    // how get_event_dispatchers computes OwnNames.
    const FName DName(*DispatcherName);
    UEdGraph* SignatureGraph = nullptr;
    for (UEdGraph* G : Blueprint->DelegateSignatureGraphs)
    {
        if (G && G->GetFName() == DName) { SignatureGraph = G; break; }
    }
    if (!SignatureGraph)
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Event dispatcher '%s' not found among this Blueprint's own dispatchers (inherited dispatchers cannot be edited)"), *DispatcherName));

    TArray<UK2Node_FunctionEntry*> EntryNodes;
    SignatureGraph->GetNodesOfClass<UK2Node_FunctionEntry>(EntryNodes);
    if (EntryNodes.Num() == 0)
        return mcp_common::MakeErrorReply(TEXT("Dispatcher signature graph has no entry node"));
    UK2Node_FunctionEntry* EntryNode = EntryNodes[0];

    // Pre-resolve every requested pin type before mutating, so a bad type aborts cleanly.
    struct FParamReq { FString Name; FString Type; FEdGraphPinType PinType; };
    TArray<FParamReq> Reqs;
    for (const TSharedPtr<FJsonValue>& V : *ParamSpecs)
    {
        const TSharedPtr<FJsonObject> Obj = V.IsValid() ? V->AsObject() : nullptr;
        if (!Obj.IsValid())
            return mcp_common::MakeErrorReply(TEXT("Each params[] entry must be an object { name, type }"));

        FParamReq R;
        if (!Obj->TryGetStringField(TEXT("name"), R.Name) || R.Name.IsEmpty())
            return mcp_common::MakeErrorReply(TEXT("A params[] entry is missing 'name'"));
        if (!Obj->TryGetStringField(TEXT("type"), R.Type) || R.Type.IsEmpty())
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("params[] entry '%s' is missing 'type'"), *R.Name));

        bool bIsOutput = false;
        if (Obj->TryGetBoolField(TEXT("is_output"), bIsOutput) && bIsOutput)
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("params[] entry '%s': event dispatchers are input-only (is_output is not supported)"), *R.Name));

        if (!FUEAgentGraphUtils::ResolvePinType(R.Type, R.PinType))
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Could not resolve param type '%s' for '%s'"), *R.Type, *R.Name));
        Reqs.Add(R);
    }

    FScopedTransaction Transaction(LOCTEXT("SetDispatcherSig", "MCP: Set Event Dispatcher Signature"));
    Blueprint->Modify();
    EntryNode->Modify();

    // replace/mirror: drop the current user-defined pins first (copy the array; RemoveUserDefinedPin mutates it).
    if (bReplace)
    {
        TArray<TSharedPtr<FUserPinInfo>> Existing = EntryNode->UserDefinedPins;
        for (const TSharedPtr<FUserPinInfo>& PinInfo : Existing)
        {
            EntryNode->RemoveUserDefinedPin(PinInfo);
        }
    }

    TArray<TSharedPtr<FJsonValue>> Added;
    for (const FParamReq& R : Reqs)
    {
        // add mode is idempotent: skip a name that already exists on the entry node.
        if (!bReplace)
        {
            bool bExists = false;
            for (const TSharedPtr<FUserPinInfo>& PinInfo : EntryNode->UserDefinedPins)
            {
                if (PinInfo.IsValid() && PinInfo->PinName == FName(*R.Name)) { bExists = true; break; }
            }
            if (bExists) { continue; }
        }
        // Dispatcher params are broadcast INPUTS -> OUTPUT pins on the entry tunnel (direction inverted,
        // same as a function input in add_function_parameter). bUseUniqueName=false keeps the requested name.
        UEdGraphPin* NewPin = EntryNode->CreateUserDefinedPin(FName(*R.Name), R.PinType, EGPD_Output, /*bUseUniqueName=*/false);
        if (NewPin)
        {
            Added.Add(MakeShared<FJsonValueString>(NewPin->PinName.ToString()));
        }
    }

    EntryNode->ReconstructNode();
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    // Report the resulting signature straight from the entry node's user-defined pins (authoritative pre-compile).
    TArray<TSharedPtr<FJsonValue>> ParamsOut;
    for (const TSharedPtr<FUserPinInfo>& PinInfo : EntryNode->UserDefinedPins)
    {
        if (!PinInfo.IsValid()) { continue; }
        TSharedPtr<FJsonObject> PJ = MakeShared<FJsonObject>();
        PJ->SetStringField(TEXT("name"), PinInfo->PinName.ToString());
        PJ->SetStringField(TEXT("type"), PinInfo->PinType.PinCategory.ToString());
        ParamsOut.Add(MakeShared<FJsonValueObject>(PJ));
    }

    TSharedPtr<FJsonObject> Result = UEAgentGraphCommandsImpl::BlueprintReply(BlueprintName, TEXT("dispatcher_name"), DispatcherName);
    Result->SetStringField(TEXT("mode"), bReplace ? TEXT("replace") : TEXT("add"));
    Result->SetArrayField(TEXT("added"), Added);
    Result->SetArrayField(TEXT("params"), ParamsOut);
    Result->SetNumberField(TEXT("param_count"), ParamsOut.Num());
    return Result;
}

// ===================================================================== //
//  Blueprint replication & RPC authoring                                //
// ===================================================================== //

// MCP:set_blueprint_variable_replication - Set a Blueprint variable's replication mode -- None / Replicated / RepNotify (RepNotify also creates the OnRep_<Variable> function graph if missing) -- then compiles and reads CPF_Net/CPF_RepNotify back. TRAP: the full green report can come back WITHOUT the flag surviving into the generated class (a variable reported is_replicated:true that never replicates; re-running this same call fixes it). The ground truth is audit_replication (its payload's replicated_variables list), then compile and re-audit -- never this return, and never one client alone.
//   @param blueprint_name - Name (or full object path) of the Blueprint.
//   @param variable_name - Name of a variable DECLARED on this Blueprint (inherited/native variables are refused).
//   @param mode - One of "None", "Replicated", "RepNotify" (case-insensitive). "None" also clears the RepNotify function and resets the lifetime condition.
//   @returns { blueprint_name, variable_name, mode, is_replicated, is_rep_notify, rep_notify_func, property_flags } -- read back from the variable's flags after compiling, which is necessary but NOT sufficient (see the trap above).
TSharedPtr<FJsonObject> HandleSetBpVariableReplication(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, VarName, Mode;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("variable_name"), VarName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("mode"), Mode, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UBlueprint* Blueprint = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::FindBlueprint(BlueprintName, Blueprint)) { return Error; }

    const FName VName(*VarName);
    const int32 VarIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VName);
    if (VarIndex == INDEX_NONE)
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Variable '%s' is not declared on this blueprint (cannot set replication on inherited/native variables)"), *VarName));

    const FString ModeLower = Mode.ToLower();
    enum class EReplMode { None, Replicated, RepNotify };
    EReplMode ReplMode;
    if (ModeLower == TEXT("none"))             { ReplMode = EReplMode::None; }
    else if (ModeLower == TEXT("replicated")) { ReplMode = EReplMode::Replicated; }
    else if (ModeLower == TEXT("repnotify"))  { ReplMode = EReplMode::RepNotify; }
    else
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Invalid mode '%s'. Expected one of: None, Replicated, RepNotify"), *Mode));

    // GetBlueprintVariablePropertyFlags returns a live pointer into the variable's
    // FBPVariableDescription.PropertyFlags (the same approach the engine's variable
    // details panel uses in FBlueprintVarActionDetails::OnChangeReplication).
    uint64* PropFlagPtr = FBlueprintEditorUtils::GetBlueprintVariablePropertyFlags(Blueprint, VName);
    if (!PropFlagPtr)
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Could not access property flags for variable '%s'"), *VarName));

    FScopedTransaction Transaction(LOCTEXT("SetVarReplication", "MCP: Set Variable Replication"));
    Blueprint->Modify();

    FString RepNotifyFuncName;
    switch (ReplMode)
    {
    case EReplMode::None:
        *PropFlagPtr &= ~CPF_Net;
        *PropFlagPtr &= ~CPF_RepNotify;
        // Mirror the engine: clear the RepNotify function and reset the condition.
        FBlueprintEditorUtils::SetBlueprintVariableRepNotifyFunc(Blueprint, VName, NAME_None);
        Blueprint->NewVariables[VarIndex].ReplicationCondition = COND_None;
        break;

    case EReplMode::Replicated:
        *PropFlagPtr |= CPF_Net;
        *PropFlagPtr &= ~CPF_RepNotify;
        FBlueprintEditorUtils::SetBlueprintVariableRepNotifyFunc(Blueprint, VName, NAME_None);
        break;

    case EReplMode::RepNotify:
        *PropFlagPtr |= CPF_Net;
        *PropFlagPtr |= CPF_RepNotify;
        // Convention OnRep_<VarName>; create the notify function graph if missing,
        // matching FBlueprintVarActionDetails::OnChangeReplication.
        RepNotifyFuncName = FString::Printf(TEXT("OnRep_%s"), *VarName);
        {
            const FName RepFName(*RepNotifyFuncName);
            UEdGraph* FuncGraph = nullptr;
            for (UEdGraph* G : Blueprint->FunctionGraphs)
            {
                if (G && G->GetFName() == RepFName) { FuncGraph = G; break; }
            }
            if (!FuncGraph)
            {
                FuncGraph = FBlueprintEditorUtils::CreateNewGraph(
                    Blueprint, RepFName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
                if (FuncGraph)
                {
                    FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, FuncGraph, /*bIsUserCreated=*/false, (UClass*)nullptr);
                }
            }
            FBlueprintEditorUtils::SetBlueprintVariableRepNotifyFunc(Blueprint, VName, RepFName);
        }
        break;
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None);

    // Read the flags back to verify the mutation actually took effect.
    uint64* VerifyPtr = FBlueprintEditorUtils::GetBlueprintVariablePropertyFlags(Blueprint, VName);
    const uint64 FinalFlags = VerifyPtr ? *VerifyPtr : 0;
    const bool bIsNet = (FinalFlags & CPF_Net) != 0;
    const bool bIsRepNotify = (FinalFlags & CPF_RepNotify) != 0;
    const FName VerifyRepFunc = FBlueprintEditorUtils::GetBlueprintVariableRepNotifyFunc(Blueprint, VName);

    TSharedPtr<FJsonObject> Result = UEAgentGraphCommandsImpl::BlueprintReply(BlueprintName, TEXT("variable_name"), VarName);
    Result->SetStringField(TEXT("mode"), Mode);
    Result->SetBoolField(TEXT("is_replicated"), bIsNet);
    Result->SetBoolField(TEXT("is_rep_notify"), bIsRepNotify);
    Result->SetStringField(TEXT("rep_notify_func"), VerifyRepFunc.ToString());
    Result->SetNumberField(TEXT("property_flags"), static_cast<double>(FinalFlags));
    return Result;
}

// MCP:set_blueprint_function_rpc - Turn a Blueprint FUNCTION into an RPC (rpc_type Server / Client / NetMulticast, or NotReplicated to undo it, plus reliable), then compile and read the FUNC_Net* flags back off the entry node. This is the ONLY headless way to author an RPC THAT TAKES ARGUMENTS: add_function_parameter cannot reach a custom event, so build the RPC as add_blueprint_function -> compile_blueprint -> add_function_parameter -> here. Engine limits that follow: a replicated function cannot return a value and cannot host latent nodes (put a Delay in a separate, parameterless custom event).
//   @param blueprint_name - Name (or full object path) of the Blueprint.
//   @param function_name - Name of an existing FUNCTION graph on the Blueprint (a custom event has no entry node here and is refused).
//   @param rpc_type - One of "NotReplicated", "Server", "Client", "NetMulticast" (case-insensitive).
//   @param reliable - Mark the RPC reliable; ignored (and cleared) when rpc_type is NotReplicated; defaults to false
//   @returns { blueprint_name, function_name, rpc_type, is_net, reliable, function_flags } -- re-read from the entry node's flags after compiling; rpc_type is the RESOLVED type, so compare it with what you asked for. Proof that the call really crosses the wire is not here: drive it in PIE and read the resulting state on the OTHER side.
TSharedPtr<FJsonObject> HandleSetBpFunctionRpc(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, FunctionName, RpcType;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("function_name"), FunctionName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("rpc_type"), RpcType, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    bool bReliable = false;
    Params->TryGetBoolField(TEXT("reliable"), bReliable);

    UBlueprint* Blueprint = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::FindBlueprint(BlueprintName, Blueprint)) { return Error; }

    UEdGraph* FuncGraph = FindFunctionGraph(Blueprint, FunctionName);
    if (!FuncGraph)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Function graph not found: %s"), *FunctionName));

    TArray<UK2Node_FunctionEntry*> EntryNodes;
    FuncGraph->GetNodesOfClass<UK2Node_FunctionEntry>(EntryNodes);
    if (EntryNodes.Num() == 0)
        return mcp_common::MakeErrorReply(TEXT("Function has no entry node"));
    UK2Node_FunctionEntry* EntryNode = EntryNodes[0];

    // Resolve the requested net specifier (mirrors FBlueprintGraphActionDetails::SetNetFlags).
    const FString RpcLower = RpcType.ToLower();
    int32 NetSpecifier = 0; // 0 == NotReplicated
    if (RpcLower == TEXT("notreplicated"))      { NetSpecifier = 0; }
    else if (RpcLower == TEXT("server"))        { NetSpecifier = FUNC_NetServer; }
    else if (RpcLower == TEXT("client"))        { NetSpecifier = FUNC_NetClient; }
    else if (RpcLower == TEXT("netmulticast"))  { NetSpecifier = FUNC_NetMulticast; }
    else
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Invalid rpc_type '%s'. Expected one of: NotReplicated, Server, Client, NetMulticast"), *RpcType));

    FScopedTransaction Transaction(LOCTEXT("SetFnRpc", "MCP: Set Function RPC"));
    Blueprint->Modify();
    EntryNode->Modify();

    // Net flags: clear all four, then set FUNC_Net|<specifier> (NotReplicated -> 0).
    const int32 NetClearMask = FUNC_Net | FUNC_NetMulticast | FUNC_NetServer | FUNC_NetClient;
    const int32 NetFlagsToSet = (NetSpecifier != 0) ? (FUNC_Net | NetSpecifier) : 0;

    int32 ExtraFlags = EntryNode->GetExtraFlags();
    ExtraFlags &= ~NetClearMask;
    ExtraFlags |= NetFlagsToSet;

    // Reliability only applies when the function is actually replicated.
    if (NetSpecifier != 0 && bReliable)
    {
        ExtraFlags |= FUNC_NetReliable;
    }
    else
    {
        ExtraFlags &= ~FUNC_NetReliable;
    }
    EntryNode->SetExtraFlags(ExtraFlags);

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None);

    // Read back from the entry node to verify.
    const int32 VerifyFlags = EntryNode->GetExtraFlags();
    const bool bIsNet = (VerifyFlags & FUNC_Net) != 0;
    const bool bServer = (VerifyFlags & FUNC_NetServer) != 0;
    const bool bClient = (VerifyFlags & FUNC_NetClient) != 0;
    const bool bMulticast = (VerifyFlags & FUNC_NetMulticast) != 0;
    const bool bIsReliable = (VerifyFlags & FUNC_NetReliable) != 0;

    FString ResolvedType = TEXT("NotReplicated");
    if (bServer)        { ResolvedType = TEXT("Server"); }
    else if (bClient)   { ResolvedType = TEXT("Client"); }
    else if (bMulticast){ ResolvedType = TEXT("NetMulticast"); }

    TSharedPtr<FJsonObject> Result = UEAgentGraphCommandsImpl::BlueprintReply(BlueprintName, TEXT("function_name"), FunctionName);
    Result->SetStringField(TEXT("rpc_type"), ResolvedType);
    Result->SetBoolField(TEXT("is_net"), bIsNet);
    Result->SetBoolField(TEXT("reliable"), bIsReliable);
    Result->SetNumberField(TEXT("function_flags"), static_cast<double>(VerifyFlags));
    return Result;
}

// ---------------------------------------------------------------------------
// set_blueprint_function_pure
//
// Why it exists. get_blueprint_functions REPORTS is_pure by reading FUNC_BlueprintPure from
// the UK2Node_FunctionEntry; this is the matching writer. A UMG property binding needs it at the
// second-to-last step: add_blueprint_function creates the function, add_function_parameter gives
// it the return value, and FDelegateEditorBinding::IsBindingValid REQUIRES the function to be pure
// (WidgetBlueprint.cpp) -- this is the headless way to mark it.
// ---------------------------------------------------------------------------
// MCP:set_blueprint_function_pure - Mark a Blueprint function as PURE (or back to impure), the editor's "Pure" checkbox: a pure function has no exec pins and is re-evaluated wherever its output is read. REQUIRED to finish a UMG property binding -- FDelegateEditorBinding::IsBindingValid refuses any function that is not FUNC_BlueprintPure or const, so add_widget_property_binding will report valid:false until this has been run on the function. The counterpart reader is get_blueprint_functions's is_pure, which reads the same flag off the same UK2Node_FunctionEntry. WARNING: Turning a function pure REMOVES its exec pins: any existing wire into or out of them is dropped by the reconstruct, so do this BEFORE building the function's body, not after.
//   @param blueprint_name - Name (or full object path) of the Blueprint owning the function.
//   @param function_name - Name of the function GRAPH (not an event; events cannot be pure).
//   @param is_pure [bool] - true to mark pure (default true), false to make it impure again.
//   @returns { blueprint_name, function_name, is_pure, was_pure, changed, function_flags } -- is_pure is RE-READ from the entry node after the compile, so it is evidence and not an echo.
TSharedPtr<FJsonObject> HandleSetBlueprintFunctionPure(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, FunctionName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("function_name"), FunctionName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    bool bWantPure = true;
    Params->TryGetBoolField(TEXT("is_pure"), bWantPure);

    UBlueprint* Blueprint = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::FindBlueprint(BlueprintName, Blueprint)) { return Error; }

    UEdGraph* FuncGraph = FindFunctionGraph(Blueprint, FunctionName);
    if (!FuncGraph)
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Function graph not found: %s. Only FUNCTION GRAPHS can be pure -- an event never can."),
            *FunctionName));

    TArray<UK2Node_FunctionEntry*> EntryNodes;
    FuncGraph->GetNodesOfClass<UK2Node_FunctionEntry>(EntryNodes);
    if (EntryNodes.Num() == 0)
        return mcp_common::MakeErrorReply(TEXT("Function has no entry node"));
    UK2Node_FunctionEntry* EntryNode = EntryNodes[0];

    const int32 FlagsBefore = EntryNode->GetExtraFlags();
    const bool bWasPure = (FlagsBefore & FUNC_BlueprintPure) != 0;

    FScopedTransaction Transaction(LOCTEXT("SetFnPure", "MCP: Set Function Pure"));
    Blueprint->Modify();
    EntryNode->Modify();

    int32 ExtraFlags = FlagsBefore;
    if (bWantPure)
    {
        // Pure implies const in the editor's model: that is what guarantees that calling it while
        // painting a widget cannot mutate anything.
        ExtraFlags |= (FUNC_BlueprintPure | FUNC_Const);
    }
    else
    {
        ExtraFlags &= ~static_cast<int32>(FUNC_BlueprintPure);
    }
    EntryNode->SetExtraFlags(ExtraFlags);

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None);

    // Re-read from the node AFTER compiling: the compiler can reject purity (e.g. a function
    // with by-reference output parameters), and then echoing the request would lie.
    const int32 FlagsAfter = EntryNode->GetExtraFlags();
    const bool bIsPure = (FlagsAfter & FUNC_BlueprintPure) != 0;

    TSharedPtr<FJsonObject> Result = UEAgentGraphCommandsImpl::BlueprintReply(BlueprintName, TEXT("function_name"), FunctionName);
    Result->SetBoolField(TEXT("is_pure"), bIsPure);
    Result->SetBoolField(TEXT("was_pure"), bWasPure);
    Result->SetBoolField(TEXT("changed"), bIsPure != bWasPure);
    Result->SetNumberField(TEXT("function_flags"), static_cast<double>(FlagsAfter));
    return Result;
}

// MCP:set_blueprint_class_replication - Set class-level ACTOR replication on a Blueprint's CDO (bReplicates, optionally bAlwaysRelevant), compile, and read the values back off the RE-FETCHED post-compile CDO. Actor Blueprints only: a component Blueprint is refused (component replication is set_component_replication). Related reader trap: audit_replication's class_replicates reads the ACTOR flag, so it reports false for every component Blueprint whatever its CDO says.
//   @param blueprint_name - Name (or full object path) of an Actor Blueprint (must be compiled at least once).
//   @param replicates - Whether the Actor class replicates to network clients. Required.
//   @param always_relevant - Sets the Actor's bAlwaysRelevant flag; only applied when the field is present
//   @returns { blueprint_name, replicates, always_relevant_set, always_relevant } -- values re-read from the CDO after compiling; always_relevant is only present when it was requested.
TSharedPtr<FJsonObject> HandleSetBpClassReplication(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    bool bReplicates = false;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredBool(Params, TEXT("replicates"), bReplicates)) { return Missing; }

    UBlueprint* Blueprint = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::FindBlueprint(BlueprintName, Blueprint)) { return Error; }

    UClass* GeneratedClass = Blueprint->GeneratedClass;
    if (!GeneratedClass)
        return mcp_common::MakeErrorReply(TEXT("Blueprint not compiled yet; compile it before setting class replication"));

    AActor* ActorCDO = Cast<AActor>(GeneratedClass->GetDefaultObject(false));
    if (!ActorCDO)
        return mcp_common::MakeErrorReply(TEXT("Blueprint's class is not an AActor subclass; class replication only applies to Actors"));

    FScopedTransaction Transaction(LOCTEXT("SetClassRepl", "MCP: Set Class Replication"));
    Blueprint->Modify();
    ActorCDO->Modify();

    // SetReplicates is safe on a CDO: on a CDO GetLocalRole() == ROLE_Authority and
    // bActorInitialized == false, so it only sets bReplicates + RemoteRole and skips
    // all net-driver/world side effects (see AActor::SetReplicates).
    ActorCDO->SetReplicates(bReplicates);

    bool bAlwaysRelevantSet = false;
    bool bAlwaysRelevantValue = false;
    if (Params->HasField(TEXT("always_relevant")))
    {
        bool bAlwaysRelevant = false;
        Params->TryGetBoolField(TEXT("always_relevant"), bAlwaysRelevant);
        // bAlwaysRelevant is a reflected public UPROPERTY on AActor; set it directly.
        ActorCDO->bAlwaysRelevant = bAlwaysRelevant;
        bAlwaysRelevantSet = true;
        bAlwaysRelevantValue = bAlwaysRelevant;
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None);

    // Re-fetch the CDO after compile (compile may reinstance) and read back to verify.
    AActor* VerifyCDO = Cast<AActor>(Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetDefaultObject(false) : nullptr);
    const bool bVerifyReplicates = VerifyCDO ? VerifyCDO->GetIsReplicated() : bReplicates;
    const bool bVerifyAlwaysRelevant = VerifyCDO ? (VerifyCDO->bAlwaysRelevant != 0) : bAlwaysRelevantValue;

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("blueprint_name"), BlueprintName);
    Result->SetBoolField(TEXT("replicates"), bVerifyReplicates);
    Result->SetBoolField(TEXT("always_relevant_set"), bAlwaysRelevantSet);
    if (bAlwaysRelevantSet)
    {
        Result->SetBoolField(TEXT("always_relevant"), bVerifyAlwaysRelevant);
    }
    return Result;
}

// MCP:set_component_replication - Set bReplicates on ONE SCS component TEMPLATE of a Blueprint (the archetype, not a spawned actor's instance), compile, and read it back off the template. It governs the component's own PROPERTY replication; note that a component declared in the SCS already routes its RPCs through its stable network name WITHOUT this flag (a component added at RUNTIME does not, and loses them). For the owning actor use set_blueprint_class_replication.
//   @param blueprint_name - Name (or full object path) of the Blueprint.
//   @param component_name - SCS component variable name on this Blueprint (inherited-only components have no local SCS node and are not found).
//   @param replicates - Whether the component should replicate. Required.
//   @returns { blueprint_name, component_name, replicates } -- replicates is re-read from the template after compiling.
TSharedPtr<FJsonObject> HandleSetComponentReplication(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, ComponentName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("component_name"), ComponentName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    bool bReplicates = false;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredBool(Params, TEXT("replicates"), bReplicates)) { return Missing; }

    UBlueprint* Blueprint = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::FindBlueprint(BlueprintName, Blueprint)) { return Error; }
    USimpleConstructionScript* Scs = Blueprint->SimpleConstructionScript;
    if (!Scs)
    {
        return mcp_common::MakeErrorReply(TEXT("Blueprint has no SimpleConstructionScript (no components)"));
    }
    // Only nodes this Blueprint declares itself are visible here (inherited components have none).
    USCS_Node* OwnNode = UEAgentGraphCommandsImpl::FindNodeByVariableName(Scs, ComponentName);
    if (!OwnNode)
    {
        return UEAgentGraphCommandsImpl::ComponentNotFound(ComponentName);
    }

    UActorComponent* Template = OwnNode->ComponentTemplate;
    if (!Template)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Component '%s' has no template object"), *ComponentName));

    FScopedTransaction Transaction(LOCTEXT("SetCompRepl", "MCP: Set Component Replication"));
    Blueprint->Modify();
    Template->Modify();

    // SetIsReplicated sets bReplicates on the component template (the SCS archetype).
    Template->SetIsReplicated(bReplicates);

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None);

    // Read the value back off the template to verify.
    const bool bVerify = Template->GetIsReplicated();

    TSharedPtr<FJsonObject> Result = UEAgentGraphCommandsImpl::BlueprintReply(BlueprintName, TEXT("component_name"), ComponentName);
    Result->SetBoolField(TEXT("replicates"), bVerify);
    return Result;
}

// ===================================================================== //
//  Blueprint advanced graph nodes                                       //
// ===================================================================== //

// MCP:add_timeline_to_blueprint - Add a Timeline to an Actor Blueprint: creates BOTH the UTimelineTemplate variable and its UK2Node_Timeline node in one call, sets a fixed length in seconds, compiles, and verifies the template really registered (timeline_created). Actor-derived Blueprints only. It authors no curves or tracks itself -- the node comes with Play/Stop/Reverse inputs and Update/Finished outputs and nothing driven; add tracks with add_blueprint_timeline_track and read them with get_blueprint_timeline_info. A requested name already in use is silently uniquified, so wire the RETURNED timeline_name.
//   @param blueprint_name - Name (or full object path) of an Actor Blueprint.
//   @param timeline_name - Desired variable name for the timeline; if it is taken, a unique one is generated and returned instead.
//   @param length - Timeline length in seconds (sets fixed-length mode); defaults to 5.0
//   @param graph_name - Target graph name; defaults to the Blueprint's event graph when omitted
//   @param position_x - Node X position on the graph canvas (ignored when node_position is set)
//   @param position_y - Node Y position on the graph canvas (ignored when node_position is set)
//   @param node_position - Node canvas position as [x, y]; takes precedence over position_x/position_y
//   @returns { node_id, class, title, pins:[...], blueprint_name, graph_name, timeline_name, length, timeline_created, timeline_guid } -- timeline_name is the name ACTUALLY used and timeline_created is a post-compile re-lookup of the template, so false means the variable did not survive.
TSharedPtr<FJsonObject> HandleAddTimelineToBlueprint(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, TimelineName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("timeline_name"), TimelineName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    double Length = 5.0;
    Params->TryGetNumberField(TEXT("length"), Length);

    UBlueprint* Blueprint = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::FindBlueprint(BlueprintName, Blueprint)) { return Error; }

    if (!FBlueprintEditorUtils::DoesSupportTimelines(Blueprint))
        return mcp_common::MakeErrorReply(TEXT("This Blueprint type does not support timelines (timelines require an Actor-derived Blueprint)"));

    // Timelines live in the event (ubergraph). Default to the event graph.
    UEdGraph* Graph = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::FindTargetGraph(Params, Blueprint, Graph)) { return Error; }

    const FVector2D Position = ReadPosition(Params);

    // If a timeline template with the requested variable name already exists, generate a
    // unique one (mirrors the engine spawner, and keeps the command idempotent on re-run).
    FName TimelineVarName(*TimelineName);
    if (Blueprint->FindTimelineTemplateByVariableName(TimelineVarName) != nullptr)
    {
        TimelineVarName = FBlueprintEditorUtils::FindUniqueTimelineName(Blueprint);
    }

    FScopedTransaction Transaction(LOCTEXT("AddTimeline", "MCP: Add Timeline To Blueprint"));
    Blueprint->Modify();
    Graph->Modify();

    // Spawn the K2 node first (adds it to the graph so GetBlueprint() resolves), set the
    // timeline variable name, add the template, THEN Finalize() (which calls
    // AllocateDefaultPins, which reads the now-existing template). This mirrors the engine's
    // UK2Node_Timeline node-spawner CustomizeNodeDelegate exactly.
    FGraphNodeCreator<UK2Node_Timeline> Creator(*Graph);
    UK2Node_Timeline* Node = Creator.CreateNode();
    Node->TimelineName = TimelineVarName;
    Node->NodePosX = Position.X;
    Node->NodePosY = Position.Y;

    UTimelineTemplate* Template = FBlueprintEditorUtils::AddNewTimeline(Blueprint, TimelineVarName);
    if (!Template)
    {
        // Roll back the half-built node.
        Node->DestroyNode();
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("AddNewTimeline failed for '%s'"), *TimelineVarName.ToString()));
    }

    // Apply requested length (fixed-length mode) and link the node to the template GUID.
    Template->Modify();
    Template->TimelineLength = (float)Length;
    Template->LengthMode = ETimelineLengthMode::TL_TimelineLength;
    Node->TimelineGuid = Template->TimelineGuid;

    Creator.Finalize();

    UEAgentGraphCommandsImpl::MarkModifiedAndCompile(Blueprint, Graph);

    // Verify the template is registered on the blueprint after compile.
    UTimelineTemplate* Verify = Blueprint->FindTimelineTemplateByVariableName(TimelineVarName);

    TSharedPtr<FJsonObject> Result = UEAgentGraphCommandsImpl::NodeReply(Node, BlueprintName, Graph);
    Result->SetStringField(TEXT("timeline_name"), TimelineVarName.ToString());
    Result->SetNumberField(TEXT("length"), Verify ? Verify->TimelineLength : (float)Length);
    Result->SetBoolField(TEXT("timeline_created"), Verify != nullptr);
    Result->SetStringField(TEXT("timeline_guid"), Node->TimelineGuid.ToString());
    return Result;
}

// ===================================================================== //
//  Blueprint component / input / delegate event nodes                   //
// ===================================================================== //

// MCP:add_component_bound_event - Bind a multicast delegate OF ONE OF THIS BLUEPRINT'S OWN COMPONENTS (OnComponentBeginOverlap, OnComponentHit, a vendor component's dispatcher...) to a generated handler event in the event graph -- the editor's component-event flow -- then compile and re-check that the binding registered (bound). ALSO WORKS ON A WIDGET BLUEPRINT: pass the WBP and a WIDGET's name as component_name to wire a Button's OnClicked, a Slider's OnValueChanged, a CheckBox's OnCheckStateChanged -- exactly what the Designer's "+ OnClicked" green button does, and the only headless route to it (bound:true, with a BndEvt__..._OnButtonClickedEvent created). A UMG widget is not an SCS component but IS a property of the generated class, which is all this needs. Idempotent: an identical binding is returned instead of duplicated. The Blueprint must already be compiled, since the component/widget has to exist as a property of the generated class. For a dispatcher on an object that is NOT self and NOT one of its components, use add_object_bound_event; for one on SELF, add_graph_node node_type="add_delegate".
//   @param blueprint_name - Name (or full object path) of an Actor Blueprint OR a Widget Blueprint (already compiled).
//   @param component_name - SCS component variable name on this Blueprint (e.g. "BodyMesh"), OR -- on a Widget Blueprint -- the name of a widget in its tree (e.g. "Btn_Play"); resolved on the skeleton class, so inherited components and widgets work too.
//   @param delegate_name - Exact multicast delegate property name on the COMPONENT's class (e.g. "OnComponentBeginOverlap"). On a miss the error names the component class so the right delegate can be looked up.
//   @param graph_name - Target graph name; defaults to the Blueprint's event graph when omitted
//   @param position_x - Node X position on the graph canvas (ignored when node_position is set)
//   @param position_y - Node Y position on the graph canvas (ignored when node_position is set)
//   @param node_position - Node canvas position as [x, y]; takes precedence over position_x/position_y
//   @returns { node_id, class, title, pins:[...the delegate's params as OUTPUT pins...], blueprint_name, graph_name, component_name, delegate, custom_function_name, bound, already_exists? } -- bound is a post-compile re-lookup (the real oracle); already_exists is only present when an existing binding was reused.
TSharedPtr<FJsonObject> HandleAddComponentBoundEvent(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, ComponentName, DelegateName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("component_name"), ComponentName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("delegate_name"), DelegateName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UBlueprint* Blueprint = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::FindBlueprint(BlueprintName, Blueprint)) { return Error; }

    // The component SCS variable becomes an FObjectProperty on the (skeleton) generated
    // class once compiled. This mirrors the engine's own SSCSEditor::ConstructEvent path.
    UClass* SearchClass = Blueprint->SkeletonGeneratedClass ? Blueprint->SkeletonGeneratedClass : Blueprint->GeneratedClass;
    if (!SearchClass)
        return mcp_common::MakeErrorReply(TEXT("Blueprint not compiled yet; compile it before binding a component event"));

    FObjectProperty* ComponentProperty = FindFProperty<FObjectProperty>(SearchClass, FName(*ComponentName));
    if (!ComponentProperty)
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Component variable '%s' not found on the blueprint class (is it an SCS component?)"), *ComponentName));

    // Resolve the component's class so we can find the multicast delegate property on it.
    UClass* ComponentClass = ComponentProperty->PropertyClass;
    if (!ComponentClass)
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Component property '%s' has no resolvable component class"), *ComponentName));

    FMulticastDelegateProperty* DelegateProperty = FindFProperty<FMulticastDelegateProperty>(ComponentClass, FName(*DelegateName));
    if (!DelegateProperty)
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Delegate '%s' not found on component class '%s' (e.g. OnComponentBeginOverlap, OnComponentHit)"),
            *DelegateName, *ComponentClass->GetName()));

    // If an identical bound event already exists, return it (idempotent).
    if (const UK2Node_ComponentBoundEvent* Existing =
            FKismetEditorUtilities::FindBoundEventForComponent(Blueprint, FName(*DelegateName), ComponentProperty->GetFName()))
    {
        TSharedPtr<FJsonObject> ExistingResult = FUEAgentGraphUtils::NodeToJson(const_cast<UK2Node_ComponentBoundEvent*>(Existing), true);
        ExistingResult->SetStringField(TEXT("blueprint_name"), BlueprintName);
        ExistingResult->SetStringField(TEXT("component_name"), ComponentName);
        ExistingResult->SetStringField(TEXT("delegate"), DelegateName);
        ExistingResult->SetBoolField(TEXT("already_exists"), true);
        ExistingResult->SetBoolField(TEXT("bound"), true);
        return ExistingResult;
    }

    // Component bound events live in the event (uber) graph.
    UEdGraph* Graph = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::FindTargetGraph(Params, Blueprint, Graph)) { return Error; }

    const FVector2D Position = ReadPosition(Params);

    FScopedTransaction Transaction(LOCTEXT("AddComponentBoundEvent", "MCP: Add Component Bound Event"));
    Blueprint->Modify();
    Graph->Modify();

    // Spawn the node, then initialize it exactly the way the engine spawner does
    // (UBlueprintBoundEventNodeSpawner::BindToNode -> InitializeComponentBoundEventParams).
    UK2Node_ComponentBoundEvent* Node = UEAgentGraphCommandsImpl::SpawnNode<UK2Node_ComponentBoundEvent>(Graph, Position,
        [ComponentProperty, DelegateProperty](UK2Node_ComponentBoundEvent* BoundEvent)
        {
            BoundEvent->InitializeComponentBoundEventParams(ComponentProperty, DelegateProperty);
        });

    UEAgentGraphCommandsImpl::MarkModifiedAndCompile(Blueprint, Graph);

    // Verify the bound event is registered after compile.
    const bool bBound = FKismetEditorUtilities::FindBoundEventForComponent(
        Blueprint, FName(*DelegateName), ComponentProperty->GetFName()) != nullptr;

    TSharedPtr<FJsonObject> Result = UEAgentGraphCommandsImpl::NodeReply(Node, BlueprintName, Graph);
    Result->SetStringField(TEXT("component_name"), ComponentName);
    Result->SetStringField(TEXT("delegate"), DelegateName);
    Result->SetStringField(TEXT("custom_function_name"), Node->CustomFunctionName.ToString());
    Result->SetBoolField(TEXT("bound"), bBound);
    return Result;
}

// MCP:add_object_bound_event - Bind a BlueprintAssignable event dispatcher of an EXTERNAL object (not self, not a component) to a handler custom event -- the editor's right-click "Assign" flow (FEdGraphSchemaAction_K2AssignDelegate). Use this to react to a dispatcher on a widget/actor instance held in a variable (e.g. a UMG widget's "On Radial Menu Item Selected"): it spawns a UK2Node_AddDelegate bound to target_class' dispatcher with its self/target pin LEFT UNCONNECTED (you wire the instance) and a UK2Node_CustomEvent whose OUTPUT pins mirror the dispatcher's params (e.g. the segment-data struct), already wired to the AddDelegate's Delegate pin. Unlike the add_delegate node_type (self-context only), this reaches a dispatcher on a foreign class. Does NOT compile the Blueprint (mirrors the engine action); compile afterward.
//   @param blueprint_name - Name (or full object path) of the Blueprint being edited (the object that will bind the event, e.g. the hub component BP).
//   @param target_class - The EXTERNAL class that owns the dispatcher. Accepts a Blueprint/widget short name, a full asset path, a full class path (/Game/.../WBP.WBP_C), a native /Script path, or the generated "..._C" class name. Prefer a full path or the "_C" class for reliable resolution.
//   @param dispatcher_name - The multicast delegate (event dispatcher) property to assign. Matched tolerantly: exact name, spaces stripped, or the display name (so "On Item Selected" resolves to the spaceless FName). On failure the error lists the assignable delegates found on target_class.
//   @param event_name - Optional desired name for the handler custom event; made unique in the Blueprint. Defaults to "<Dispatcher>_Event".
//   @param graph_name - Target graph name; defaults to the Blueprint's event graph when omitted.
//   @param position_x - Node X position on the graph canvas (ignored when node_position is set)
//   @param position_y - Node Y position on the graph canvas (ignored when node_position is set)
//   @param node_position - Node canvas position as [x, y]; takes precedence over position_x/position_y
//   @returns { blueprint_name, graph_name, target_class, dispatcher, add_delegate_node_id, custom_event_node_id, custom_event_name, wired, node_id, pins:[...] (the AddDelegate node's pins -- wire 'self'/Target = the widget instance and 'execute'/'then' into your exec flow), custom_event:{ node_id, pins:[...] } (hang handler logic off its 'then'/'output_delegate' and read the dispatcher's param output pins) }
TSharedPtr<FJsonObject> HandleAddObjectBoundEvent(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, TargetClassName, DispatcherName, EventName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("target_class"), TargetClassName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("dispatcher_name"), DispatcherName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    Params->TryGetStringField(TEXT("event_name"), EventName);

    UBlueprint* Blueprint = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::FindBlueprint(BlueprintName, Blueprint)) { return Error; }

    UClass* TargetClass = ResolveTargetClassFlexible(TargetClassName);
    if (!TargetClass)
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("target_class not found: %s (try a Blueprint short name, a full asset path, or the generated ..._C class)"), *TargetClassName));

    FString FoundDispatcher;
    FMulticastDelegateProperty* DProp = FindMulticastDelegatePropertyTolerant(TargetClass, DispatcherName, FoundDispatcher);
    if (!DProp)
    {
        TArray<FString> Names;
        for (TFieldIterator<FMulticastDelegateProperty> It(TargetClass); It; ++It) { Names.Add(It->GetName()); }
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Dispatcher '%s' (assignable multicast delegate) not found on class '%s'. Assignable delegates: [%s]"),
            *DispatcherName, *TargetClass->GetName(), *FString::Join(Names, TEXT(", "))));
    }

    UFunction* SignatureFunction = DProp->SignatureFunction;
    if (!SignatureFunction)
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Dispatcher '%s' on '%s' has no signature function"), *FoundDispatcher, *TargetClass->GetName()));

    UEdGraph* Graph = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::FindTargetGraph(Params, Blueprint, Graph)) { return Error; }

    const FVector2D Position = ReadPosition(Params);

    // Unique handler custom-event name (default "<Dispatcher>_Event", mirroring the engine action).
    const FString DesiredEventName = EventName.IsEmpty()
        ? FString::Printf(TEXT("%s_Event"), *FoundDispatcher) : EventName;
    const FName UniqueEventName = FBlueprintEditorUtils::FindUniqueKismetName(Blueprint, DesiredEventName);

    FScopedTransaction Transaction(LOCTEXT("AddObjectBoundEvent", "MCP: Add Object Bound Event"));
    Blueprint->Modify();
    Graph->Modify();

    // Spawn the AddDelegate node bound to the EXTERNAL class' dispatcher. bSelfContext=false -> the
    // self/target pin (PN_Self, friendly "Target") is typed to TargetClass and LEFT UNCONNECTED for the
    // caller to wire the widget/actor instance. SetFromProperty runs BEFORE Finalize because
    // AllocateDefaultPins reads the DelegateReference to type the self pin + Delegate pin.
    UK2Node_AddDelegate* BindNode = UEAgentGraphCommandsImpl::SpawnNode<UK2Node_AddDelegate>(Graph, Position,
        [DProp, TargetClass](UK2Node_AddDelegate* AddDelegateNode) { AddDelegateNode->SetFromProperty(DProp, /*bSelfContext=*/false, TargetClass); });
    if (!BindNode->NodeGuid.IsValid()) { BindNode->CreateNewGuid(); }

    // Build the handler custom event from the dispatcher's signature so its OUTPUT params mirror the
    // dispatcher's (e.g. a "Picked Entry Info" struct output pin), then wire the event's delegate
    // output pin to the AddDelegate node's Delegate input pin -- exactly the engine "Assign" flow.
    UK2Node_CustomEvent* EventNode = UK2Node_CustomEvent::CreateFromFunction(
        FVector2D(Position.X - 150.0f, Position.Y + 150.0f), Graph,
        UniqueEventName.ToString(), SignatureFunction, /*bSelectNewNode=*/false);
    if (!EventNode)
        return mcp_common::MakeErrorReply(TEXT("Failed to create the handler custom event from the dispatcher signature"));

    const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
    UEdGraphPin* OutDelegatePin = EventNode->FindPin(UK2Node_CustomEvent::DelegateOutputName, EGPD_Output);
    UEdGraphPin* InDelegatePin = BindNode->GetDelegatePin();
    bool bWired = false;
    if (OutDelegatePin && InDelegatePin)
    {
        bWired = K2Schema->TryCreateConnection(OutDelegatePin, InDelegatePin);
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    Graph->NotifyGraphChanged();

    // Top-level payload IS the AddDelegate node (node_id + pins), plus both node identities and the
    // handler custom event nested (its node_id + the dispatcher-param output pins).
    TSharedPtr<FJsonObject> Result = FUEAgentGraphUtils::NodeToJson(BindNode, true);
    Result->SetStringField(TEXT("add_delegate_node_id"), BindNode->NodeGuid.ToString());
    Result->SetStringField(TEXT("custom_event_node_id"), EventNode->NodeGuid.ToString());
    Result->SetStringField(TEXT("custom_event_name"), EventNode->CustomFunctionName.ToString());
    Result->SetObjectField(TEXT("custom_event"), FUEAgentGraphUtils::NodeToJson(EventNode, true));
    Result->SetStringField(TEXT("blueprint_name"), BlueprintName);
    Result->SetStringField(TEXT("graph_name"), Graph->GetName());
    Result->SetStringField(TEXT("target_class"), TargetClass->GetPathName());
    Result->SetStringField(TEXT("dispatcher"), FoundDispatcher);
    Result->SetBoolField(TEXT("wired"), bWired);
    return Result;
}

namespace UEAgentGraphCommandsImpl
{
// The list_widget_bound_events entry of a *BoundEvent node. Such a node is its own handler, so it is
// always wired. "Resolved" means OwnerClass still declares the delegate, not that the field has
// text: renaming the dispatcher leaves the name in place and the binding dead.
TSharedPtr<FJsonObject> MakeBoundEventBinding(const UEdGraph* Graph, const UK2Node_Event* Node, const TCHAR* BindingKind,
                                              const FString& BoundTo, FName DelegateName, UClass* OwnerClass)
{
    TSharedPtr<FJsonObject> Binding = MakeShared<FJsonObject>();
    Binding->SetStringField(TEXT("graph_name"), Graph->GetName());
    Binding->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
    Binding->SetStringField(TEXT("binding_kind"), BindingKind);
    Binding->SetStringField(TEXT("bound_to"), BoundTo);
    Binding->SetStringField(TEXT("delegate_name"), DelegateName.ToString());
    Binding->SetStringField(TEXT("delegate_owner_class"), OwnerClass ? OwnerClass->GetName() : FString());
    const bool bResolved = OwnerClass != nullptr && OwnerClass->FindPropertyByName(DelegateName) != nullptr;
    Binding->SetBoolField(TEXT("delegate_resolved"), bResolved);
    Binding->SetBoolField(TEXT("wired"), true);
    Binding->SetStringField(TEXT("custom_event_name"), Node->CustomFunctionName.ToString());
    Binding->SetStringField(TEXT("custom_event_node_id"), Node->NodeGuid.ToString());
    return Binding;
}
}

// MCP:list_widget_bound_events - List every event-dispatcher BINDING wired in a Blueprint, across the three node kinds that make one in practice: UK2Node_AddDelegate (from add_object_bound_event / add_graph_node node_type="add_delegate"), UK2Node_ComponentBoundEvent (from add_component_bound_event -- a component's dispatcher OR a UMG widget's, e.g. a Button's OnClicked) and UK2Node_ActorBoundEvent (a level actor's, bound from the Level Blueprint). Each entry reports which kind it is (binding_kind), what it is bound ON (bound_to: the component/widget variable, or the actor), the delegate's owner class and name, and the handler on the other end. Despite the name it works on ANY Blueprint -- but it only walks Ubergraph pages and FunctionGraphs, so a binding inside a macro or interface graph is out of scope. Run it BEFORE add_object_bound_event / add_component_bound_event: double-binding one delegate to two handlers compiles fine and silently runs BOTH. A reader that cannot see what the writer wrote is worse than no reader: a 0 reads as "not wired" and sends you to re-wire what is already there.
//   @param blueprint_name - Blueprint asset name or path (Actor Blueprint, Widget Blueprint or Level Blueprint).
//   @returns { blueprint_name, bindings:[{ graph_name, node_id, binding_kind, bound_to, delegate_owner_class, delegate_name, delegate_resolved, wired, custom_event_name, custom_event_node_id }], binding_count, kind_counts:{add_delegate,component_bound_event,actor_bound_event} } -- binding_kind names the node class behind the entry. bound_to is the component/widget variable name (component_bound_event), the actor name (actor_bound_event), or empty for add_delegate, whose target pin is left unconnected by design. delegate_resolved=false means the delegate no longer resolves live (renamed/removed). wired=false is a DANGLING add_delegate bind (its Delegate pin has no handler); the two *BoundEvent kinds ARE their own handler, so they are always wired and custom_event_name is the generated BndEvt__ function.
TSharedPtr<FJsonObject> HandleListWidgetBoundEvents(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UBlueprint* Blueprint = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::FindBlueprint(BlueprintName, Blueprint)) { return Error; }

    TArray<UEdGraph*> GraphsToScan;
    GraphsToScan.Append(Blueprint->UbergraphPages);
    GraphsToScan.Append(Blueprint->FunctionGraphs);

    TArray<TSharedPtr<FJsonValue>> Bindings;
    int32 KindAddDelegate = 0;
    int32 KindComponentBound = 0;
    int32 KindActorBound = 0;

    for (UEdGraph* Graph : GraphsToScan)
    {
        if (!Graph)
        {
            continue;
        }

        TArray<UK2Node_AddDelegate*> AddDelegateNodes;
        Graph->GetNodesOfClass<UK2Node_AddDelegate>(AddDelegateNodes);

        for (UK2Node_AddDelegate* Node : AddDelegateNodes)
        {
            if (!Node)
            {
                continue;
            }

            TSharedPtr<FJsonObject> Binding = MakeShared<FJsonObject>();
            Binding->SetStringField(TEXT("graph_name"), Graph->GetName());
            Binding->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());

            // GetProperty() re-resolves the bound FMulticastDelegateProperty live (DelegateReference
            // itself is a private member of UK2Node_BaseMCDelegate -- not directly readable). Its
            // FField::GetOwnerClass() walks the outer chain to the class that actually DECLARES the
            // delegate, whether that is this Blueprint's own generated class (self bind) or an
            // external one (add_object_bound_event). GetPropertyName() always returns the raw
            // referenced name even if resolution fails, so delegate_name is never empty on a real bind.
            FProperty* DelegateProp = Node->GetProperty();
            UClass* OwnerClass = DelegateProp ? DelegateProp->GetOwnerClass() : nullptr;
            Binding->SetStringField(TEXT("delegate_owner_class"), OwnerClass ? OwnerClass->GetName() : FString());
            Binding->SetStringField(TEXT("delegate_name"), Node->GetPropertyName().ToString());
            Binding->SetBoolField(TEXT("delegate_resolved"), DelegateProp != nullptr);

            // Walk the Delegate pin's link to the handler custom event on the other end, mirroring how
            // add_object_bound_event wires EventNode's output Delegate pin into this pin.
            FString CustomEventName;
            FString HandlerNodeId;
            bool bWired = false;
            if (UEdGraphPin* DelegatePin = Node->GetDelegatePin())
            {
                for (UEdGraphPin* Linked : DelegatePin->LinkedTo)
                {
                    if (!Linked)
                    {
                        continue;
                    }
                    if (UK2Node_CustomEvent* Handler = Cast<UK2Node_CustomEvent>(Linked->GetOwningNode()))
                    {
                        CustomEventName = Handler->CustomFunctionName.ToString();
                        HandlerNodeId = Handler->NodeGuid.ToString();
                        bWired = true;
                        break;
                    }
                }
            }
            Binding->SetBoolField(TEXT("wired"), bWired);
            Binding->SetStringField(TEXT("custom_event_name"), CustomEventName);
            Binding->SetStringField(TEXT("custom_event_node_id"), HandlerNodeId);
            Binding->SetStringField(TEXT("binding_kind"), TEXT("add_delegate"));
            // The target pin of an AddDelegate is left UNCONNECTED on purpose (the caller wires
            // it), so there is no "what" to report here without making it up.
            Binding->SetStringField(TEXT("bound_to"), FString());

            Bindings.Add(MakeShared<FJsonValueObject>(Binding));
            ++KindAddDelegate;
        }

        // --- UK2Node_ComponentBoundEvent: the one add_component_bound_event creates -- i.e. EVERY
        // UMG button in the project. It does not share a base with AddDelegate (UK2Node_Event vs
        // UK2Node_BaseMCDelegate), hence the separate walk. Here the node IS its own handler: there
        // is no Delegate pin to follow, so always wired.
        TArray<UK2Node_ComponentBoundEvent*> ComponentBoundNodes;
        Graph->GetNodesOfClass<UK2Node_ComponentBoundEvent>(ComponentBoundNodes);

        for (UK2Node_ComponentBoundEvent* Node : ComponentBoundNodes)
        {
            if (!Node)
            {
                continue;
            }

            Bindings.Add(MakeShared<FJsonValueObject>(UEAgentGraphCommandsImpl::MakeBoundEventBinding(
                Graph, Node, TEXT("component_bound_event"), Node->ComponentPropertyName.ToString(),
                Node->DelegatePropertyName, Node->DelegateOwnerClass)));
            ++KindComponentBound;
        }

        // --- UK2Node_ActorBoundEvent: the dispatcher of a PLACED actor, bound from the Level
        // Blueprint. Same case as the previous one (it is its own handler); what changes is what it
        // is bound to: a world instance, not a class variable.
        TArray<UK2Node_ActorBoundEvent*> ActorBoundNodes;
        Graph->GetNodesOfClass<UK2Node_ActorBoundEvent>(ActorBoundNodes);

        for (UK2Node_ActorBoundEvent* Node : ActorBoundNodes)
        {
            if (!Node)
            {
                continue;
            }

            AActor* Owner = Node->EventOwner;
            Bindings.Add(MakeShared<FJsonValueObject>(UEAgentGraphCommandsImpl::MakeBoundEventBinding(
                Graph, Node, TEXT("actor_bound_event"), Owner ? Owner->GetActorNameOrLabel() : FString(),
                Node->DelegatePropertyName, Owner ? Owner->GetClass() : nullptr)));
            ++KindActorBound;
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("blueprint_name"), BlueprintName);
    Result->SetArrayField(TEXT("bindings"), Bindings);
    Result->SetNumberField(TEXT("binding_count"), Bindings.Num());

    // Breakdown by node class: if a total does not match what was just written, it says
    // which of the three walks is the one that did not see it.
    TSharedPtr<FJsonObject> KindCounts = MakeShared<FJsonObject>();
    KindCounts->SetNumberField(TEXT("add_delegate"), KindAddDelegate);
    KindCounts->SetNumberField(TEXT("component_bound_event"), KindComponentBound);
    KindCounts->SetNumberField(TEXT("actor_bound_event"), KindActorBound);
    Result->SetObjectField(TEXT("kind_counts"), KindCounts);

    return Result;
}

// MCP:add_input_action_event_node - Add the LEGACY action-mapping event node (UK2Node_InputAction) bound to an action mapping NAME, and compile. WARNING: this node does NOT fire under Enhanced Input, which is what UE5 projects use -- it compiles clean and simply never runs. For an Enhanced Input action asset use add_enhanced_input_action_node, whose Triggered/Started/Completed pins actually execute. Only reach for this one on a project still using the legacy input mappings.
//   @param blueprint_name - Name (or full object path) of an Actor/Pawn Blueprint (SupportsInputEvents is enforced).
//   @param input_action_name - Name of the LEGACY action mapping. It is not validated against the project's mappings: a typo yields a node that never fires.
//   @param graph_name - Target graph name; defaults to the Blueprint's event graph when omitted
//   @param position_x - Node X position on the graph canvas (ignored when node_position is set)
//   @param position_y - Node Y position on the graph canvas (ignored when node_position is set)
//   @param node_position - Node canvas position as [x, y]; takes precedence over position_x/position_y
//   @returns { node_id, class, title, pins:[Pressed, Released, Key], blueprint_name, graph_name, input_action_name, node_class } -- node_class should read K2Node_InputAction (the legacy node); nothing here proves the mapping exists.
TSharedPtr<FJsonObject> HandleAddInputActionEventNode(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, InputActionName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("input_action_name"), InputActionName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UBlueprint* Blueprint = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::FindBlueprint(BlueprintName, Blueprint)) { return Error; }

    if (!Blueprint->SupportsInputEvents())
        return mcp_common::MakeErrorReply(TEXT("This Blueprint type does not support input events (requires an Actor/Pawn-derived Blueprint)"));

    // Input action events live in the event (uber) graph.
    UEdGraph* Graph = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::FindTargetGraph(Params, Blueprint, Graph)) { return Error; }

    const FVector2D Position = ReadPosition(Params);

    FScopedTransaction Transaction(LOCTEXT("AddInputActionEvent", "MCP: Add Input Action Event"));
    Blueprint->Modify();
    Graph->Modify();

    // The legacy action-mapping event node (UK2Node_InputAction) lives in BlueprintGraph,
    // requires no open editor, and is a real, compilable node bound to a named action.
    UK2Node_InputAction* Node = UEAgentGraphCommandsImpl::SpawnNode<UK2Node_InputAction>(Graph, Position,
        [&InputActionName](UK2Node_InputAction* ActionNode) { ActionNode->InputActionName = FName(*InputActionName); });

    UEAgentGraphCommandsImpl::MarkModifiedAndCompile(Blueprint, Graph);

    TSharedPtr<FJsonObject> Result = UEAgentGraphCommandsImpl::NodeReply(Node, BlueprintName, Graph);
    Result->SetStringField(TEXT("input_action_name"), Node->InputActionName.ToString());
    Result->SetStringField(TEXT("node_class"), Node->GetClass()->GetName());
    return Result;
}

// MCP:add_enhanced_input_action_node - Add an Enhanced Input action event node (UK2Node_EnhancedInputAction) bound to a UInputAction asset. Unlike add_input_action_event_node -- which creates the LEGACY UK2Node_InputAction that does NOT fire under Enhanced Input -- this node exposes the real ETriggerEvent exec pins (Triggered/Started/Ongoing/Canceled/Completed) that fire from the Enhanced Input pipeline. Use this instead of polling IsInputKeyDown in a timer; it is rebind-aware (bound to the InputAction asset, not a raw key).
//   @param blueprint_name - Name (or full object path) of an Actor/Pawn Blueprint.
//   @param input_action - Asset path (or short name) of the UInputAction to bind, e.g. "/Game/Input/IA_Crouch".
//   @param graph_name - Target graph name; defaults to the Blueprint's event graph when omitted.
//   @param position_x - Node X position on the graph canvas (ignored when node_position is set)
//   @param position_y - Node Y position on the graph canvas (ignored when node_position is set)
//   @param node_position - Node canvas position as [x, y]; takes precedence over position_x/position_y
//   @returns { node_id, pins:[{name, direction, type, linked_to, default}], blueprint_name, graph_name, input_action, node_class } -- the created node WITH its freshly-allocated pins: ETriggerEvent exec outputs (Triggered/Started/Ongoing/Canceled/Completed) plus data outputs (ActionValue/ElapsedSeconds/TriggeredSeconds/InputAction). Wire it via connect_pins WITHOUT a follow-up get_graph_nodes.
TSharedPtr<FJsonObject> HandleAddEnhancedInputActionNode(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, InputActionRef;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("input_action"), InputActionRef, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UBlueprint* Blueprint = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::FindBlueprint(BlueprintName, Blueprint)) { return Error; }

    if (!Blueprint->SupportsInputEvents())
        return mcp_common::MakeErrorReply(TEXT("This Blueprint type does not support input events (requires an Actor/Pawn-derived Blueprint)"));

    // Resolve the UInputAction asset (accept an object path, package path, or short asset name).
    UInputAction* InputAction = LoadObject<UInputAction>(nullptr, *InputActionRef);
    if (!InputAction)
    {
        FSoftObjectPath SoftPath(InputActionRef);
        if (SoftPath.IsValid())
        {
            InputAction = Cast<UInputAction>(SoftPath.TryLoad());
        }
    }
    if (!InputAction)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("UInputAction not found: %s"), *InputActionRef));

    // Enhanced Input events live in the event (uber) graph.
    UEdGraph* Graph = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::FindTargetGraph(Params, Blueprint, Graph)) { return Error; }

    const FVector2D Position = ReadPosition(Params);

    FScopedTransaction Transaction(LOCTEXT("AddEnhancedInputActionEvent", "MCP: Add Enhanced Input Action Event"));
    Blueprint->Modify();
    Graph->Modify();

    // Crash-safe manual creation (same order as spawn_actor / add_anim_graph_node): do
    // NOT use FGraphNodeCreator, whose Finalize() runs PostPlacedNewNode() BEFORE AllocateDefaultPins().
    // Set InputAction FIRST -- UK2Node_EnhancedInputAction::AllocateDefaultPins() reads it to build the
    // ActionValue/InputAction data pins -- then allocate pins BEFORE PostPlacedNewNode().
    UK2Node_EnhancedInputAction* Node = NewObject<UK2Node_EnhancedInputAction>(Graph);
    Node->InputAction = InputAction;
    UEAgentGraphCommandsImpl::PlaceNodePinsFirst(Graph, Node, Position);

    UEAgentGraphCommandsImpl::MarkModifiedAndCompile(Blueprint, Graph);

    TSharedPtr<FJsonObject> Result = UEAgentGraphCommandsImpl::NodeReply(Node, BlueprintName, Graph);
    Result->SetStringField(TEXT("input_action"), InputAction->GetPathName());
    Result->SetStringField(TEXT("node_class"), Node->GetClass()->GetName());
    return Result;
}

// ===================================================================== //
//  Blueprint math / utility / flow nodes                                //
// ===================================================================== //

// MCP:add_reroute_node - Add a reroute/knot node: pure layout, it re-emits whatever is wired into it and changes no behaviour. Does NOT compile. Note when reading graphs back: get_graph_nodes with compact=true excludes knots by default, so a chain that looks broken there may simply route through one -- re-read with exclude_classes=[].
//   @param blueprint_name - Name (or full object path) of the Blueprint.
//   @param graph_name - Target graph name; defaults to the Blueprint's event graph when omitted
//   @param position_x - Node X position on the graph canvas (ignored when node_position is set)
//   @param position_y - Node Y position on the graph canvas (ignored when node_position is set)
//   @param node_position - Node canvas position as [x, y]; takes precedence over position_x/position_y
//   @returns { node_id, class, title, pins:[...wildcard in/out until wired...], blueprint_name, graph_name }
TSharedPtr<FJsonObject> HandleAddRerouteNode(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UBlueprint* Blueprint = nullptr;
    UEdGraph* Graph = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::ResolveBlueprintGraph(Params, BlueprintName, Blueprint, Graph)) { return Error; }

    const FVector2D Position = ReadPosition(Params);

    FScopedTransaction Transaction(LOCTEXT("AddRerouteNode", "MCP: Add Reroute Node"));
    Blueprint->Modify();
    Graph->Modify();

    UK2Node_Knot* Node = UEAgentGraphCommandsImpl::SpawnNode<UK2Node_Knot>(Graph, Position);

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    Graph->NotifyGraphChanged();

    TSharedPtr<FJsonObject> Result = UEAgentGraphCommandsImpl::NodeReply(Node, BlueprintName, Graph);
    return Result;
}

// MCP:get_event_dispatchers - List a Blueprint's event dispatchers (multicast delegates) WITH each one's full parameter signature (name, C++ type, is_output) and whether it is inherited. Read from the SKELETON class, so it reflects edits made since the last full compile. Use it before add_broadcast_node / add_object_bound_event / set_event_dispatcher_signature, whose pins mirror this signature; only the dispatchers this Blueprint DECLARES can have their signature edited.
//   @param blueprint_name - Blueprint asset name or path
//   @param include_inherited - Also include dispatchers inherited from parent classes; default false = only the ones this Blueprint declares (i.e. the editable ones)
//   @returns { blueprint_name: string, dispatchers: [ { name: string, is_inherited: bool, param_count: int, params: [ { name: string, type: string, is_output: bool } ] } ], dispatcher_count: int }
TSharedPtr<FJsonObject> HandleGetEventDispatchers(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    // Default: list only the dispatchers this Blueprint declares (the editable ones).
    bool bListInherited = false;
    Params->TryGetBoolField(TEXT("include_inherited"), bListInherited);

    UBlueprint* Blueprint = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::FindBlueprint(BlueprintName, Blueprint)) { return Error; }

    // Prefer the skeleton class (always reflects current editor-time signatures). Its multicast-delegate
    // properties ARE the dispatchers; each SignatureFunction carries the parameter list.
    UClass* Class = Blueprint->SkeletonGeneratedClass ? Blueprint->SkeletonGeneratedClass : Blueprint->GeneratedClass;

    // Names OWNED by this Blueprint (own dispatchers == the DelegateSignatureGraphs).
    TSet<FName> OwnNames;
    for (UEdGraph* G : Blueprint->DelegateSignatureGraphs)
    {
        if (G) { OwnNames.Add(G->GetFName()); }
    }

    TArray<TSharedPtr<FJsonValue>> Arr;
    if (Class)
    {
        for (TFieldIterator<FMulticastDelegateProperty> It(Class); It; ++It)
        {
            FMulticastDelegateProperty* DelegateProp = *It;
            const FName DName = DelegateProp->GetFName();
            const bool bIsInherited = !OwnNames.Contains(DName);
            if (bIsInherited && !bListInherited)
            {
                continue;
            }

            TSharedPtr<FJsonObject> D = MakeShared<FJsonObject>();
            D->SetStringField(TEXT("name"), DName.ToString());
            D->SetBoolField(TEXT("is_inherited"), bIsInherited);

            TArray<TSharedPtr<FJsonValue>> ParamsArr;
            int32 ParamCount = 0;
            if (UFunction* Sig = DelegateProp->SignatureFunction)
            {
                for (TFieldIterator<FProperty> PIt(Sig); PIt && (PIt->PropertyFlags & CPF_Parm); ++PIt)
                {
                    FProperty* P = *PIt;
                    TSharedPtr<FJsonObject> PJ = MakeShared<FJsonObject>();
                    PJ->SetStringField(TEXT("name"), P->GetName());
                    PJ->SetStringField(TEXT("type"), P->GetCPPType());
                    PJ->SetBoolField(TEXT("is_output"),
                        (P->PropertyFlags & CPF_OutParm) != 0 && (P->PropertyFlags & CPF_ReturnParm) == 0);
                    ParamsArr.Add(MakeShared<FJsonValueObject>(PJ));
                    ++ParamCount;
                }
            }
            D->SetArrayField(TEXT("params"), ParamsArr);
            D->SetNumberField(TEXT("param_count"), ParamCount);
            Arr.Add(MakeShared<FJsonValueObject>(D));
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("blueprint_name"), BlueprintName);
    Result->SetArrayField(TEXT("dispatchers"), Arr);
    Result->SetNumberField(TEXT("dispatcher_count"), Arr.Num());
    return Result;
}

// MCP:add_broadcast_node - Add the node that BROADCASTS (fires) an event dispatcher on self -- own or inherited -- with one input pin per signature parameter, so listeners run. Does NOT compile. The dispatcher must already exist in the compiled class (add_event_dispatcher then compile_blueprint), and its parameter pins only appear after set_event_dispatcher_signature. The opposite direction (LISTENING) is add_graph_node node_type="add_delegate" for self, add_object_bound_event for another object's dispatcher.
//   @param blueprint_name - Blueprint that owns (or inherits) the dispatcher
//   @param dispatcher_name - Exact name of the multicast delegate to broadcast; resolved on the skeleton class, so inherited dispatchers work
//   @param graph_name - Target graph; defaults to the Blueprint's event graph when omitted
//   @param position_x - Node X position on the graph canvas (ignored when node_position is set)
//   @param position_y - Node Y position on the graph canvas (ignored when node_position is set)
//   @param node_position - Node canvas position as [x, y]; takes precedence over position_x/position_y
//   @returns { node_id, class, title, pins:[execute, then, one input per signature param], blueprint_name, graph_name, dispatcher_name } -- an empty parameter list means the dispatcher has no signature yet.
TSharedPtr<FJsonObject> HandleAddBroadcastNode(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, DispatcherName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("dispatcher_name"), DispatcherName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UBlueprint* Blueprint = nullptr;
    UEdGraph* Graph = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::ResolveBlueprintGraph(Params, BlueprintName, Blueprint, Graph)) { return Error; }

    // Resolve the multicast delegate property (own or inherited) from the up-to-date skeleton class.
    UClass* Class = Blueprint->SkeletonGeneratedClass ? Blueprint->SkeletonGeneratedClass : Blueprint->GeneratedClass;
    FMulticastDelegateProperty* DelegateProp = Class
        ? FindFProperty<FMulticastDelegateProperty>(Class, FName(*DispatcherName)) : nullptr;
    if (!DelegateProp)
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Event dispatcher '%s' not found on Blueprint '%s'"), *DispatcherName, *BlueprintName));

    const FVector2D Position = ReadPosition(Params);

    FScopedTransaction Transaction(LOCTEXT("AddBroadcastNode", "MCP: Add Broadcast Node"));
    Blueprint->Modify();
    Graph->Modify();

    UK2Node_CallDelegate* Node = UEAgentGraphCommandsImpl::SpawnNode<UK2Node_CallDelegate>(Graph, Position, [DelegateProp](UK2Node_CallDelegate* CallNode)
    {
        CallNode->SetFromProperty(DelegateProp, /*bSelfContext=*/true, DelegateProp->GetOwnerClass());
    });

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    Graph->NotifyGraphChanged();

    TSharedPtr<FJsonObject> Result = UEAgentGraphCommandsImpl::NodeReply(Node, BlueprintName, Graph);
    Result->SetStringField(TEXT("dispatcher_name"), DispatcherName);
    return Result;
}

// MCP:add_parent_call_node - Add a "Parent: <Function>" node that calls the parent-class implementation of an overridden function/event -- the second half of the child-first shape "Event X -> Parent: X -> our logic" that keeps a vendor base class working while the child adds behaviour. Searches the WHOLE ancestor chain, not just the immediate parent. Does NOT compile, and IS allowed inside build_blueprint_graph, so the event + super + logic can be built in one transaction.
//   @param blueprint_name - Blueprint whose parent function is called
//   @param function_name - Name of the overridden function/event whose parent implementation to call; must exist on some ancestor or the call errors
//   @param graph_name - Target graph (usually the override's own graph); defaults to the event graph
//   @param position_x - Node X position on the graph canvas (ignored when node_position is set)
//   @param position_y - Node Y position on the graph canvas (ignored when node_position is set)
//   @param node_position - Node canvas position as [x, y]; takes precedence over position_x/position_y
//   @returns { node_id, class, title, pins:[execute, then, the parent's params], blueprint_name, graph_name, function_name, parent_class } -- parent_class names the ancestor that actually declares the implementation being called.
TSharedPtr<FJsonObject> HandleAddParentCallNode(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, FunctionName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("function_name"), FunctionName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UBlueprint* Blueprint = nullptr;
    UEdGraph* Graph = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::ResolveBlueprintGraph(Params, BlueprintName, Blueprint, Graph)) { return Error; }

    // Resolve the parent's UFunction (walk the ancestor chain past the immediate parent).
    UFunction* ParentFunc = nullptr;
    for (UClass* C = Blueprint->ParentClass; C; C = C->GetSuperClass())
    {
        if (UFunction* F = C->FindFunctionByName(FName(*FunctionName)))
        {
            ParentFunc = F;
            break;
        }
    }
    if (!ParentFunc)
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Parent function '%s' not found on any ancestor of '%s'"), *FunctionName, *BlueprintName));

    const FVector2D Position = ReadPosition(Params);

    FScopedTransaction Transaction(LOCTEXT("AddParentCallNode", "MCP: Add Parent Function Call"));
    Blueprint->Modify();
    Graph->Modify();

    UK2Node_CallParentFunction* Node = UEAgentGraphCommandsImpl::SpawnNode<UK2Node_CallParentFunction>(Graph, Position,
        [ParentFunc](UK2Node_CallParentFunction* ParentCall) { ParentCall->SetFromFunction(ParentFunc); });

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    Graph->NotifyGraphChanged();

    UClass* OwnerClass = ParentFunc->GetOwnerClass();
    TSharedPtr<FJsonObject> Result = UEAgentGraphCommandsImpl::NodeReply(Node, BlueprintName, Graph);
    Result->SetStringField(TEXT("function_name"), FunctionName);
    Result->SetStringField(TEXT("parent_class"), OwnerClass ? OwnerClass->GetName() : TEXT(""));
    return Result;
}

// Graph authoring and hygiene

// MCP:get_function_pins - Preview the EXACT pins an add_graph_node call_function node WOULD expose for a UFunction WITHOUT creating anything: owner_class, is_pure, is_static, exec_input/exec_output ("execute"/"then", null when pure), self_pin (null when static), input_params, output_params and return_value. This is what lets a whole build_blueprint_graph (nodes + connections + pin_defaults) be written in ONE call instead of create -> get_graph_nodes -> connect round-trips. It answers "what pins would it have", NOT "can a Blueprint call it": some engine functions resolve and preview here and are still rejected by the compiler, so the oracle for callability is compile_blueprint with return_messages=true.
//   @param function_name - UFunction name, matched EXACTLY against the reflected name (e.g. "PrintString", "K2_GetActorLocation", or a Blueprint function graph's own name, which may contain spaces like "Emit Noise At Location"). A guessed name that does not exist just errors -- it never returns an empty preview.
//   @param target_class - Optional class searched FIRST: native name ("KismetSystemLibrary"), "/Script/Module.Class", a Blueprint class path ("/Game/...X_C"), or a user BFL. Also always searched: the blueprint's own class + KismetSystem/GameplayStatics/KismetMath.
//   @param blueprint_name - Optional Blueprint whose generated class joins the search (for self / BP-local functions).
//   @returns { function_name, owner_class, is_pure, is_static, exec_input, exec_output, self_pin, input_params:[{name,type}], output_params:[{name,type}], return_value:{name,type}|null, note }
TSharedPtr<FJsonObject> HandleGetFunctionPins(const TSharedPtr<FJsonObject>& Params)
{
    FString FunctionName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("function_name"), FunctionName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    FString TargetClass, BlueprintName;
    Params->TryGetStringField(TEXT("target_class"), TargetClass);
    Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName);

    UBlueprint* Blueprint = BlueprintName.IsEmpty() ? nullptr : FindBlueprintFlexible(BlueprintName);

    UFunction* Function = ResolveFunction(FunctionName, TargetClass, Blueprint);
    if (!Function)
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Function not found: '%s'. %s%s"),
            *FunctionName,
            *([&]() -> FString
            {
                // Name the classes ACTUALLY searched, not "target_class and the
                // standard libraries". Functions such as DestroyComponent or
                // SetScalarParameterValueOnMaterials exist in the engine but hang
                // off a COMPONENT, which is not in this list: without seeing it written, the
                // reader cannot deduce that target_class is missing.
                TArray<FString> Names;
                for (UClass* C : BuildFunctionSearchClasses(TargetClass, Blueprint))
                {
                    if (C) { Names.AddUnique(C->GetName()); }
                }
                return FString::Printf(
                    TEXT("Searched exactly these %d class(es): %s. If the function belongs to "
                         "anything else -- a component, an actor subclass, another Blueprint "
                         "function library -- it will NOT be found until you pass target_class."),
                    Names.Num(), *FString::Join(Names, TEXT(", ")));
            }()),
            *([&]() -> FString
            {
                const FString Near = SuggestSimilarFunctions(
                    FunctionName, BuildFunctionSearchClasses(TargetClass, Blueprint));
                return Near.IsEmpty()
                    ? FString(TEXT(" Nothing with a similar name in those classes either."))
                    : FString::Printf(TEXT(" Did you mean: %s?"), *Near);
            }())));

    const bool bPure = Function->HasAnyFunctionFlags(FUNC_BlueprintPure);
    const bool bStatic = Function->HasAnyFunctionFlags(FUNC_Static);

    // Split the parameter properties the same way a K2Node_CallFunction does: a return value and any
    // pure-out params become OUTPUT pins; a UPARAM(ref)/const-ref param stays an INPUT pin; the rest are inputs.
    TArray<TSharedPtr<FJsonValue>> InputParams, OutputParams;
    TSharedPtr<FJsonObject> ReturnValue;
    for (TFieldIterator<FProperty> It(Function); It && (It->PropertyFlags & CPF_Parm); ++It)
    {
        FProperty* P = *It;
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), P->GetName());
        Entry->SetStringField(TEXT("type"), P->GetCPPType());
        if (P->HasAnyPropertyFlags(CPF_ReturnParm))
        {
            Entry->SetStringField(TEXT("name"), UEdGraphSchema_K2::PN_ReturnValue.ToString());
            ReturnValue = Entry;
        }
        else if (P->HasAnyPropertyFlags(CPF_OutParm) && !P->HasAnyPropertyFlags(CPF_ReferenceParm))
        {
            OutputParams.Add(MakeShared<FJsonValueObject>(Entry));
        }
        else
        {
            InputParams.Add(MakeShared<FJsonValueObject>(Entry));
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("function_name"), Function->GetName());
    Result->SetStringField(TEXT("owner_class"),
        Function->GetOwnerClass() ? Function->GetOwnerClass()->GetPathName() : FString());
    Result->SetBoolField(TEXT("is_pure"), bPure);
    Result->SetBoolField(TEXT("is_static"), bStatic);
    if (bPure)
    {
        Result->SetField(TEXT("exec_input"), MakeShared<FJsonValueNull>());
        Result->SetField(TEXT("exec_output"), MakeShared<FJsonValueNull>());
    }
    else
    {
        Result->SetStringField(TEXT("exec_input"), UEdGraphSchema_K2::PN_Execute.ToString());
        Result->SetStringField(TEXT("exec_output"), UEdGraphSchema_K2::PN_Then.ToString());
    }
    if (bStatic) { Result->SetField(TEXT("self_pin"), MakeShared<FJsonValueNull>()); }
    else         { Result->SetStringField(TEXT("self_pin"), UEdGraphSchema_K2::PN_Self.ToString()); }
    Result->SetArrayField(TEXT("input_params"), InputParams);
    Result->SetArrayField(TEXT("output_params"), OutputParams);
    if (ReturnValue) { Result->SetObjectField(TEXT("return_value"), ReturnValue); }
    else             { Result->SetField(TEXT("return_value"), MakeShared<FJsonValueNull>()); }
    Result->SetStringField(TEXT("note"), TEXT("Pin names match a K2Node_CallFunction: exec_input/exec_output = 'execute'/'then' (impure only), self_pin = 'self' (member fns only), each param by its listed name, return_value = 'ReturnValue'. Feed these straight into build_blueprint_graph's connections/pin_defaults."));
    return Result;
}

// MCP:lint_blueprint_graph - Read-only lint of a Blueprint's graphs for four authoring bugs that COMPILE CLEAN and only show up in play: eclipsed_interface_event and custom_event_shadowing_override (a node that looks like an override and is not, so the base contract never runs), unreachable_exec_node (a node no exec chain reaches from any entry) and function_exit_without_return (a live exec path that falls off the end of a value-returning function, so the caller silently gets false/0/None). Scans ALL ubergraph + function + macro graphs by default. HONESTY LIMIT: findings:[] is NOT a clean bill of health -- by deliberate design it never flags a merely unconnected exec output, so a Branch or Is Valid with dangling outputs can pass; check linked_to yourself with get_graph_nodes (compact=false, or node_id for one node).
//   @param blueprint_name - Name (or full object path) of the Blueprint to analyze.
//   @param graph_name - Optional single graph to analyze; when omitted, scans ALL ubergraph (event) pages + function graphs + macro graphs owned by this Blueprint. Interface-override graphs are NOT in that default set -- name one explicitly to lint it.
//   @returns { findings: [ { kind, node_id, node_title, graph_name, why, fix_hint, pin? } ] } -- kind is one of eclipsed_interface_event, custom_event_shadowing_override, unreachable_exec_node, function_exit_without_return (this one also reports the offending exec pin in "pin"); "why" carries a best-effort pointer at the likely upstream break. No counters and no coverage flags: an empty array only means these four rules found nothing.
//   NOTE (impl): what each rule detects, and why it is shaped the way it is:
//     kind="eclipsed_interface_event": a non-override event node whose name matches an overridable
//     inherited/interface function -- it is dead/eclipsed by the real override. Fix via add_blueprint_function(override=true).
//     kind="custom_event_shadowing_override": a Custom Event whose name matches an overridable inherited
//     function -- rename it or convert it into a proper override.
//     kind="unreachable_exec_node": a node with an exec INPUT pin that is unreachable, via
//     exec-link BFS, from any graph entry point. Entries are detected generically off the pin schema
//     (>=1 exec output pin AND zero exec input pins) rather than a node-class allowlist, so Event/
//     CustomEvent/FunctionEntry/ComponentBoundEvent/InputAction AND Tunnel/Composite entry-boundary
//     nodes all qualify for free, while exit-boundary tunnels are correctly treated as ordinary sinks
//     (never misreported as orphaned entries). This is deliberately a REACHABILITY
//     rule, NOT an "empty exec output pin" rule: a lone unconnected output (a Branch's unwired False,
//     the last node of a chain) is completely legitimate, and reporting on that alone would flood every
//     graph with noise, making the linter useless -- do NOT "improve" this into flagging empty outputs
//     directly; only report nodes that are themselves unreachable. `why` best-effort-points at the likely
//     break: reachable nodes in the same graph with 2+ exec-output pins that are ALL unconnected (e.g.
//     a K2Node_MacroInstance "Is Valid" with BOTH Is Valid/Is Not Valid outputs unlinked, stranding
//     the rest of a function graph -- which is why function graphs are in the default scope).
//     kind="function_exit_without_return": inside a function graph that RETURNS a value,
//     a REACHABLE node whose exec OUTPUT pin is unconnected -- that execution path falls off the end of
//     the function without ever hitting the Return node, so the caller silently receives the DEFAULT
//     output values (false/0/None) and no error at all. Typical shape: a lookup function whose 'Not
//     Found' output hangs while the Return node IS reachable through the Found branch -- so
//     unreachable_exec_node legitimately reports nothing on a graph that is broken. It is NOT the blanket "flag every empty
//     exec output" rule the note above forbids: the GATE is what makes it safe -- see the inline comment
//     on the rule for the gate and for the three false-positive cases it is built to reject (a void
//     function with an empty Branch/False, a dangling 'then' on an EventGraph event, macro graphs).
//     Reports the offending pin in an extra "pin" field.
//   (redundant_cast detection is intentionally omitted: it cannot be determined reliably headless.)
TSharedPtr<FJsonObject> HandleLintBlueprintGraph(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UBlueprint* Blueprint = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::FindBlueprint(BlueprintName, Blueprint)) { return Error; }

    // Build the set of graphs to scan.
    TArray<UEdGraph*> Graphs;
    FString GraphName;
    if (Params->TryGetStringField(TEXT("graph_name"), GraphName) && !GraphName.IsEmpty())
    {
        UEdGraph* G = FUEAgentGraphUtils::FindGraphByNameDeep(Blueprint, GraphName);
        if (!G)
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Graph not found: %s"), *GraphName));
        Graphs.Add(G);
    }
    else
    {
        // Default scope: the event graphs AND the function + macro graphs -- unreachable code in a
        // component's function lives in a FUNCTION graph, not an event graph.
        for (UEdGraph* G : Blueprint->UbergraphPages)
        {
            if (G) { Graphs.Add(G); }
        }
        for (UEdGraph* G : Blueprint->FunctionGraphs)
        {
            if (G) { Graphs.Add(G); }
        }
        for (UEdGraph* G : Blueprint->MacroGraphs)
        {
            if (G) { Graphs.Add(G); }
        }
    }

    TArray<TSharedPtr<FJsonValue>> Findings;

    // The scan is multi-graph by default, so every finding carries the graph it came from.
    // CurrentGraph is set at the top of the per-graph loop below. AddFinding also RETURNS the
    // finding object so a rule can attach extra fields (function_exit_without_return attaches
    // "pin"); the other call sites ignore the return value.
    UEdGraph* CurrentGraph = nullptr;
    auto AddFinding = [&Findings, &CurrentGraph](const TCHAR* Kind, UEdGraphNode* Node, const FString& Why, const FString& FixHint) -> TSharedPtr<FJsonObject>
    {
        TSharedPtr<FJsonObject> F = MakeShared<FJsonObject>();
        F->SetStringField(TEXT("kind"), Kind);
        F->SetStringField(TEXT("node_id"), Node ? Node->NodeGuid.ToString() : FString());
        F->SetStringField(TEXT("node_title"),
            Node ? Node->GetNodeTitle(ENodeTitleType::ListView).ToString() : FString());
        F->SetStringField(TEXT("graph_name"), CurrentGraph ? CurrentGraph->GetName() : FString());
        F->SetStringField(TEXT("why"), Why);
        F->SetStringField(TEXT("fix_hint"), FixHint);
        Findings.Add(MakeShared<FJsonValueObject>(F));
        return F;
    };

    for (UEdGraph* Graph : Graphs)
    {
        if (!Graph) { continue; }
        CurrentGraph = Graph;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node) { continue; }

            // (b) A Custom Event whose name collides with an overridable inherited function.
            if (UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node))
            {
                const FName CustomName = CustomEvent->CustomFunctionName;
                if (!CustomName.IsNone() &&
                    FBlueprintEditorUtils::GetOverrideFunctionClass(Blueprint, CustomName) != nullptr)
                {
                    AddFinding(TEXT("custom_event_shadowing_override"), Node,
                        FString::Printf(TEXT("Custom Event '%s' shadows an overridable inherited/interface function of the same name; the base contract is never actually overridden."),
                            *CustomName.ToString()),
                        TEXT("Rename this Custom Event, or override the real member with add_blueprint_function(override=true)."));
                }
                continue; // handled as custom event; skip the event branch below
            }

            // (a) A non-override event node whose EventReference name matches an overridable member.
            if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
            {
                if (!EventNode->bOverrideFunction)
                {
                    const FName MemberName = EventNode->EventReference.GetMemberName();
                    if (!MemberName.IsNone() &&
                        FBlueprintEditorUtils::GetOverrideFunctionClass(Blueprint, MemberName) != nullptr)
                    {
                        AddFinding(TEXT("eclipsed_interface_event"), Node,
                            FString::Printf(TEXT("Event '%s' references an overridable inherited/interface function but is NOT flagged as an override, so it is dead/eclipsed at runtime."),
                                *MemberName.ToString()),
                            TEXT("Delete this node and create the real override via add_blueprint_function(override=true) (kind=event)."));
                    }
                }
            }
        }

        // (c) unreachable_exec_node -- a REACHABILITY rule, deliberately NOT an "empty exec
        // output pin" rule. An unconnected exec OUTPUT pin is legitimate all over the place (a
        // Branch's unwired False, the last action of a chain) and flagging those directly would bury
        // the linter in noise. Instead: seed a BFS from every node that has >=1 exec output pin and
        // ZERO exec input pins -- by construction those are graph roots (Event/CustomEvent/
        // FunctionEntry/ComponentBoundEvent/InputAction, and -- for free, with no class-list needed --
        // the entry-boundary Tunnel of a Composite/macro graph, since only the entry side of a
        // Tunnel/Composite pair has zero exec-input pins; the exit side has an exec input and no
        // output, so it is correctly treated as an ordinary sink that must itself be reached).
        // Anything left over with an exec INPUT pin that the BFS never touched is truly orphaned --
        // e.g. everything after a K2Node_MacroInstance "Is Valid" whose Is Valid/Is Not Valid exec
        // outputs are BOTH linked_to:[], which leaves the rest of the function with no execution entry.
        {
            auto IsExecPin = [](const UEdGraphPin* Pin, EEdGraphPinDirection Dir)
            {
                return Pin && !Pin->bOrphanedPin
                    && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
                    && Pin->Direction == Dir;
            };
            auto HasExecPinDir = [&IsExecPin](UEdGraphNode* Node, EEdGraphPinDirection Dir)
            {
                for (UEdGraphPin* Pin : Node->Pins)
                {
                    if (IsExecPin(Pin, Dir)) { return true; }
                }
                return false;
            };

            TSet<UEdGraphNode*> Reachable;
            TArray<UEdGraphNode*> Queue;
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (Node && HasExecPinDir(Node, EGPD_Output) && !HasExecPinDir(Node, EGPD_Input))
                {
                    bool bAlready = false;
                    Reachable.Add(Node, &bAlready);
                    if (!bAlready) { Queue.Add(Node); }
                }
            }
            while (Queue.Num() > 0)
            {
                UEdGraphNode* Cur = Queue.Pop();
                for (UEdGraphPin* Pin : Cur->Pins)
                {
                    if (!IsExecPin(Pin, EGPD_Output)) { continue; }
                    for (UEdGraphPin* Linked : Pin->LinkedTo)
                    {
                        UEdGraphNode* Next = Linked ? Linked->GetOwningNode() : nullptr;
                        if (Next)
                        {
                            bool bAlready = false;
                            Reachable.Add(Next, &bAlready);
                            if (!bAlready) { Queue.Add(Next); }
                        }
                    }
                }
            }

            // Best-effort "why" hint: reachable nodes with 2+ exec-output pins that are ALL
            // unconnected are unusual (a single dangling "then" ending a chain is normal; a
            // Branch/Sequence/MacroInstance wired in but going NOWHERE on every branch is not), so
            // they are surfaced as likely upstream suspects.
            TArray<FString> Suspects;
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (!Node || !Reachable.Contains(Node)) { continue; }
                int32 ExecOutCount = 0, ExecOutEmptyCount = 0;
                for (UEdGraphPin* Pin : Node->Pins)
                {
                    if (!IsExecPin(Pin, EGPD_Output)) { continue; }
                    ++ExecOutCount;
                    if (Pin->LinkedTo.Num() == 0) { ++ExecOutEmptyCount; }
                }
                if (ExecOutCount >= 2 && ExecOutCount == ExecOutEmptyCount)
                {
                    Suspects.Add(Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
                }
            }

            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (!Node || Reachable.Contains(Node)) { continue; }
                if (!HasExecPinDir(Node, EGPD_Input)) { continue; } // pure/data-only nodes never qualify

                FString Why = TEXT("This node has an execution input pin, but no chain of exec links reaches it from any event/FunctionEntry/tunnel-entry in this graph -- it will never run.");
                if (Suspects.Num() > 0)
                {
                    Why += FString::Printf(
                        TEXT(" Likely upstream cause: %d node(s) in this graph have 2+ exec outputs that are ALL unconnected (e.g. '%s') -- check whether one of them should route into this chain."),
                        Suspects.Num(), *Suspects[0]);
                }

                AddFinding(TEXT("unreachable_exec_node"), Node, Why,
                    TEXT("Reconnect this node's exec input to the live chain -- use get_graph_nodes (node_id for one node) to find the correct upstream output pin, then connect_pins."));
            }

            // (d) function_exit_without_return -- the SILENT failure the reachability rule above
            // structurally CANNOT see: a lookup function that leaves the 'Not Found' exec output
            // unconnected. The Return node is
            // still perfectly reachable through the Found branch, so `Reachable` covers every node and
            // unreachable_exec_node correctly emits findings:[] -- on a graph that is broken. What
            // actually happens at runtime is that the Not-Found path falls off the end of the function
            // and the caller gets the DEFAULT outputs (false/0/None) with no error whatsoever.
            //
            // This deliberately does NOT contradict the "do NOT flag empty exec outputs" warning on
            // rule (c): the GATE is the whole rule. It fires only when
            //   1. this graph owns a UK2Node_FunctionResult (Return node), AND
            //   2. that Return node exposes >=1 NON-exec pin, i.e. the function really returns data.
            // The three false positives that gate exists to reject:
            //   * a VOID function with a Branch whose False output is empty -> a void function has
            //     either no Result node at all or one carrying only its exec pin, so condition 2 fails
            //     and it yields ZERO findings of this kind. Falling off the end of a void function is
            //     not a data bug, so there is nothing to report.
            //   * an EventGraph event with a dangling 'then' -> ubergraph pages never contain a
            //     UK2Node_FunctionResult (events return nothing), so condition 1 fails: ZERO findings.
            //   * macro graphs -> they terminate in Tunnel nodes, never in a FunctionResult, so
            //     condition 1 fails there too.
            // Only nodes ALREADY in `Reachable` are considered: an unreachable node's dangling outputs
            // are the symptom of rule (c) and reporting them here as well would just double-count.
            {
                TArray<UK2Node_FunctionResult*> ResultNodes;
                Graph->GetNodesOfClass<UK2Node_FunctionResult>(ResultNodes);

                bool bReturnsValue = false;
                for (UK2Node_FunctionResult* ResultNode : ResultNodes)
                {
                    if (!ResultNode) { continue; }
                    for (UEdGraphPin* Pin : ResultNode->Pins)
                    {
                        if (Pin && !Pin->bOrphanedPin
                            && Pin->Direction == EGPD_Input
                            && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
                        {
                            bReturnsValue = true;
                            break;
                        }
                    }
                    if (bReturnsValue) { break; }
                }

                if (bReturnsValue)
                {
                    for (UEdGraphNode* Node : Graph->Nodes)
                    {
                        if (!Node || !Reachable.Contains(Node)) { continue; }
                        // The Return node itself has no exec OUTPUT pin, so this skip is belt-and-braces.
                        if (Node->IsA<UK2Node_FunctionResult>()) { continue; }

                        for (UEdGraphPin* Pin : Node->Pins)
                        {
                            if (!IsExecPin(Pin, EGPD_Output)) { continue; }
                            if (Pin->LinkedTo.Num() > 0) { continue; }

                            const FString PinLabel = Pin->PinFriendlyName.IsEmpty()
                                ? Pin->PinName.ToString()
                                : Pin->PinFriendlyName.ToString();

                            TSharedPtr<FJsonObject> F = AddFinding(TEXT("function_exit_without_return"), Node,
                                FString::Printf(TEXT("Exec output pin '%s' is unconnected inside '%s', a function that RETURNS a value. Execution taking this path falls off the end of the function without ever reaching a Return node, so the caller silently receives the DEFAULT output values (false/0/None) and never learns anything went wrong."),
                                    *PinLabel, *Graph->GetName()),
                                FString::Printf(TEXT("Wire pin '%s' either into the remaining logic or straight into this function's Return node (setting the output values that path should report): connect_pins from this node's '%s' output."),
                                    *PinLabel, *Pin->PinName.ToString()));
                            if (F.IsValid())
                            {
                                F->SetStringField(TEXT("pin"), Pin->PinName.ToString());
                            }
                        }
                    }
                }
            }
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("findings"), Findings);
    return Result;
}

// Graph-builder primitives: pin-connection validation, macro authoring, pin split/recombine and the
// per-variable replication condition

// MCP:validate_pin_connection - Dry-run the K2 schema's verdict on a pin pair WITHOUT touching the graph: returns valid plus the schema's own message (which names the incompatible types) and its raw response code. connect_pins runs the same check and then connects, so use this one to test a plan before committing a build_blueprint_graph batch whose rollback can be refused, or to find out WHY a connection was rejected without leaving anything behind.
//   @param blueprint_name - Blueprint name or path.
//   @param source_node_id - NodeGuid of the source node.
//   @param source_pin - Output pin name on the source node (falls back to any pin with that name).
//   @param target_node_id - NodeGuid of the target node; must be in the same graph as the source.
//   @param target_pin - Input pin name on the target node (falls back to any pin with that name).
//   @param graph_name - Target graph name; defaults to the Blueprint's event graph when omitted
//   @returns { valid, message, response_code } -- valid is false only for an outright DISALLOW; a response that would connect WITH a conversion node or by breaking an existing link still reports valid:true, and "message" is where that shows.
TSharedPtr<FJsonObject> HandleValidatePinConnection(const TSharedPtr<FJsonObject>& Params)
{
    UEAgentGraphCommandsImpl::FPinPair Pair;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::ReadPinPair(Params, Pair)) { return Error; }

    const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
    const FPinConnectionResponse CanConnect = Schema->CanCreateConnection(Pair.SourcePin, Pair.TargetPin);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("valid"), CanConnect.Response != CONNECT_RESPONSE_DISALLOW);
    Result->SetStringField(TEXT("message"), CanConnect.Message.ToString());
    Result->SetNumberField(TEXT("response_code"), static_cast<int32>(CanConnect.Response));
    return Result;
}

// MCP:create_blueprint_macro - Create an EMPTY, Tunnel-terminated macro graph owned by this Blueprint's MacroGraphs (a reusable snippet, not a function: macros inline at compile time and can carry multiple exec outputs). Idempotent -- an existing macro of that name is returned with already_existed:true. Fill it with add_graph_node using graph_name=<macro>, then instantiate it with add_graph_node node_type="macro_instance", which finds the Blueprint's own macros. Give it inputs and outputs (exec included) with add_function_parameter function_name=<macro>; inverse: remove_blueprint_macro.
//   @param blueprint_name - Blueprint name or path.
//   @param macro_name - Name for the new macro graph.
//   @returns { blueprint_name, macro_name, graph_name, already_existed } -- graph_name is what to pass as graph_name / macro_graph downstream.
TSharedPtr<FJsonObject> HandleCreateBlueprintMacro(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, MacroName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("macro_name"), MacroName)) { return Missing; }

    UBlueprint* Blueprint = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::FindBlueprint(BlueprintName, Blueprint)) { return Error; }

    const FName MacroFName(*MacroName);
    for (UEdGraph* G : Blueprint->MacroGraphs)
    {
        if (G && G->GetFName() == MacroFName)
        {
            TSharedPtr<FJsonObject> Result = UEAgentGraphCommandsImpl::BlueprintReply(BlueprintName, TEXT("macro_name"), MacroName);
            Result->SetStringField(TEXT("graph_name"), G->GetName());
            Result->SetBoolField(TEXT("already_existed"), true);
            return Result;
        }
    }

    FScopedTransaction Transaction(LOCTEXT("CreateBlueprintMacro", "MCP: Create Blueprint Macro"));
    Blueprint->Modify();
    UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
        Blueprint, MacroFName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    if (!NewGraph)
        return mcp_common::MakeErrorReply(TEXT("FBlueprintEditorUtils::CreateNewGraph failed"));
    FBlueprintEditorUtils::AddMacroGraph(Blueprint, NewGraph, /*bIsUserCreated=*/true, /*SignatureFromClass=*/nullptr);

    TSharedPtr<FJsonObject> Result = UEAgentGraphCommandsImpl::BlueprintReply(BlueprintName, TEXT("macro_name"), MacroName);
    Result->SetStringField(TEXT("graph_name"), NewGraph->GetName());
    Result->SetBoolField(TEXT("already_existed"), false);
    return Result;
}

// MCP:split_blueprint_pin - Split a struct-typed pin (Vector, Rotator, Transform...) into its scalar sub-pins and return their names, so each component can be wired or defaulted on its own instead of writing the whole struct literal. Refused when the schema says the pin is not splittable. Note the interaction: set_pin_default_value REFUSES a pin that is currently split -- recombine_blueprint_pin first if you want to write the struct as one value.
//   @param blueprint_name - Blueprint name or path.
//   @param node_id - NodeGuid of the node owning the pin.
//   @param pin_name - Name of the struct pin to split (either direction).
//   @param graph_name - Target graph name; defaults to the Blueprint's event graph when omitted
//   @returns { split, sub_pins:[string] } -- sub_pins are the names to pass to connect_pins / set_pin_default_value from now on (e.g. "NewLocation_X").
TSharedPtr<FJsonObject> HandleSplitBlueprintPin(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, NodeId, PinName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("node_id"), NodeId, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("pin_name"), PinName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UBlueprint* Blueprint = nullptr;
    UEdGraph* Graph = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::ResolveBlueprintGraph(Params, BlueprintName, Blueprint, Graph)) { return Error; }
    UEdGraphNode* Node = nullptr;
    if (TSharedPtr<FJsonObject> Error = GraphNodePinLookup::FindNode(Graph, NodeId, Node)) { return Error; }
    UEdGraphPin* Pin = nullptr;
    if (TSharedPtr<FJsonObject> Error = GraphNodePinLookup::FindPin(Node, PinName, EGPD_MAX, Pin)) { return Error; }

    const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
    if (!Schema->CanSplitStructPin(*Pin))
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Pin '%s' cannot be split (not a splittable struct type, or the node disallows it)"), *PinName));

    FScopedTransaction Transaction(LOCTEXT("SplitBlueprintPin", "MCP: Split Pin"));
    Blueprint->Modify();
    Graph->Modify();
    Schema->SplitPin(Pin);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    Graph->NotifyGraphChanged();

    TArray<TSharedPtr<FJsonValue>> SubPinNames;
    UEdGraphPin* ResolvedPin = FUEAgentGraphUtils::FindPinByName(Node, PinName);
    if (ResolvedPin)
    {
        for (UEdGraphPin* Sub : ResolvedPin->SubPins)
        {
            if (Sub) { SubPinNames.Add(MakeShared<FJsonValueString>(Sub->PinName.ToString())); }
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("split"), true);
    Result->SetArrayField(TEXT("sub_pins"), SubPinNames);
    return Result;
}

// MCP:recombine_blueprint_pin - Collapse a previously split struct pin back into ONE pin (inverse of split_blueprint_pin), which is what set_pin_default_value needs before it will write a struct literal. Accepts either the parent pin's name or any of its sub-pin names; links on the sub-pins are dropped by the engine as part of recombining.
//   @param blueprint_name - Blueprint name or path.
//   @param node_id - NodeGuid of the node owning the pin.
//   @param pin_name - Name of the parent pin OR of any of its sub-pins.
//   @param graph_name - Target graph name; defaults to the Blueprint's event graph when omitted
//   @returns { recombined } -- always true on the success path; a pin that is not split is an error response instead.
TSharedPtr<FJsonObject> HandleRecombineBlueprintPin(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, NodeId, PinName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("node_id"), NodeId, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("pin_name"), PinName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UBlueprint* Blueprint = nullptr;
    UEdGraph* Graph = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::ResolveBlueprintGraph(Params, BlueprintName, Blueprint, Graph)) { return Error; }
    UEdGraphNode* Node = nullptr;
    if (TSharedPtr<FJsonObject> Error = GraphNodePinLookup::FindNode(Graph, NodeId, Node)) { return Error; }
    UEdGraphPin* Pin = nullptr;
    if (TSharedPtr<FJsonObject> Error = GraphNodePinLookup::FindPin(Node, PinName, EGPD_MAX, Pin)) { return Error; }

    // CanRecombineStructPin requires a SUB-pin (ParentPin != nullptr) -- if the caller
    // passed the parent pin's own name, walk to its first sub-pin for the check only.
    // RecombinePin() itself tolerates either and auto-redirects to SubPins[0].
    UEdGraphPin* CheckPin = Pin;
    if (!CheckPin->ParentPin && CheckPin->SubPins.Num() > 0)
    {
        CheckPin = CheckPin->SubPins[0];
    }

    const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
    if (!Schema->CanRecombineStructPin(*CheckPin))
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Pin '%s' cannot be recombined (not a split struct pin)"), *PinName));

    FScopedTransaction Transaction(LOCTEXT("RecombineBlueprintPin", "MCP: Recombine Pin"));
    Blueprint->Modify();
    Graph->Modify();
    Schema->RecombinePin(Pin);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    Graph->NotifyGraphChanged();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("recombined"), true);
    return Result;
}

// MCP:set_replication_condition - Set the ELifetimeCondition (COND_OwnerOnly, COND_SkipOwner, COND_InitialOnly...) that decides WHICH clients get a replicated variable, then compile. It is the second half of set_blueprint_variable_replication, which decides WHETHER it replicates at all: on a variable that is not Replicated/RepNotify the condition is stored and completely INERT, and the response says so through is_replicated:false plus a "note". Blueprint-declared variables only.
//   @param blueprint_name - Blueprint name or path.
//   @param variable_name - Name of a variable DECLARED on this Blueprint (inherited ones are refused).
//   @param condition - ELifetimeCondition name, with or without the COND_ prefix (e.g. "OwnerOnly" or "COND_OwnerOnly"); an unknown name is an error.
//   @returns { blueprint_name, variable_name, condition, is_replicated, note? } -- condition is the canonical COND_ name that was stored; is_replicated:false means the setting has no effect yet.
TSharedPtr<FJsonObject> HandleSetReplicationCondition(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, VarName, ConditionStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("variable_name"), VarName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("condition"), ConditionStr, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UBlueprint* Blueprint = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::FindBlueprint(BlueprintName, Blueprint)) { return Error; }

    const FName VName(*VarName);
    const int32 VarIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VName);
    if (VarIndex == INDEX_NONE)
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Variable '%s' is not declared on this blueprint"), *VarName));

    UEnum* CondEnum = StaticEnum<ELifetimeCondition>();
    if (!CondEnum) { return mcp_common::MakeErrorReply(TEXT("StaticEnum<ELifetimeCondition>() returned null")); }
    FString Canonical = ConditionStr;
    if (!Canonical.StartsWith(TEXT("COND_"))) { Canonical = TEXT("COND_") + Canonical; }
    int64 EnumVal = CondEnum->GetValueByNameString(Canonical, EGetByNameFlags::CheckAuthoredName);
    if (EnumVal == INDEX_NONE)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Unknown replication condition '%s'"), *ConditionStr));

    uint64* PropFlagPtr = FBlueprintEditorUtils::GetBlueprintVariablePropertyFlags(Blueprint, VName);
    const bool bIsReplicated = PropFlagPtr && (*PropFlagPtr & CPF_Net) != 0;

    FScopedTransaction Transaction(LOCTEXT("SetReplicationCondition", "MCP: Set Replication Condition"));
    Blueprint->Modify();
    Blueprint->NewVariables[VarIndex].ReplicationCondition = static_cast<ELifetimeCondition>(EnumVal);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None);

    TSharedPtr<FJsonObject> Result = UEAgentGraphCommandsImpl::BlueprintReply(BlueprintName, TEXT("variable_name"), VarName);
    Result->SetStringField(TEXT("condition"), CondEnum->GetNameStringByValue(EnumVal));
    Result->SetBoolField(TEXT("is_replicated"), bIsReplicated);
    if (!bIsReplicated)
    {
        Result->SetStringField(TEXT("note"), TEXT("Variable is not Replicated/RepNotify -- condition is stored but has no effect until set_blueprint_variable_replication enables CPF_Net."));
    }
    return Result;
}

// MCP:build_blueprint_graph - Build several nodes, wire their pins, and set pin defaults in one pass, wrapped in a single undo transaction -- the way to author a whole graph shape without create -> get_graph_nodes -> connect round-trips (get_function_pins gives you the pin names up front). THE ORDER IS FIXED AND MATTERS: every node first, then ALL connections, then pin_defaults -- so a node whose TYPE depends on a pin default (a generic GetComponentByClass) is connected while still untyped, fails with "... is not compatible with ..." and, under stop_on_error's default true, takes the whole call down; use add_graph_node node_type="get_component_by_class", which is born typed. add_comment_node has no pins at all, so a pin_defaults entry aimed at one also kills the batch. Self-contained: each sub-step runs its command directly, never through the bridge (only these: add_graph_node, add_timeline_to_blueprint, add_component_bound_event, add_input_action_event_node, add_enhanced_input_action_node, add_reroute_node, add_comment_node, add_parent_call_node, add_broadcast_node, add_object_bound_event), and no step compiles: an add_graph_node step's compile param is ignored, so compile_blueprint once afterwards.
//   @param blueprint_name - Blueprint name or path.
//   @param graph_name - Target graph name; defaults to the Blueprint's event graph when omitted
//   @param nodes [array!] - [{id: string, command_type: string, params?: object}, ...] (required, non-empty). 'id' is a caller-chosen local key used to reference the node from 'connections'/'pin_defaults'; 'params' is that command's own param object (blueprint_name/graph_name are auto-filled if omitted). WORKED EXAMPLE (print "hi" on BeginPlay, one call): nodes=[{"id":"beg","command_type":"add_graph_node","params":{"node_type":"event","event_name":"ReceiveBeginPlay"}},{"id":"print","command_type":"add_graph_node","params":{"node_type":"print_string"}}], connections=[{"from_node":"beg","from_pin":"then","to_node":"print","to_pin":"execute"}], pin_defaults=[{"node":"print","pin":"InString","value":"hi"}]. Pin names for a call_function node are knowable up front via get_function_pins (so no create->get_graph_nodes->connect round-trips). Child-override canonical shape (call the parent first), one call: nodes=[{"id":"evt","command_type":"add_graph_node","params":{"node_type":"event","event_name":"ReceiveBeginPlay"}},{"id":"super","command_type":"add_parent_call_node","params":{"function_name":"ReceiveBeginPlay"}}], connections=[{"from_node":"evt","from_pin":"then","to_node":"super","to_pin":"execute"}]. NOTE on add_object_bound_event: it creates TWO nodes; the step's local 'id' resolves to the UK2Node_AddDelegate (wire the external instance into its 'self' pin and the exec flow into 'execute'), while the handler custom event is addressable as "<id>.custom_event" and its real NodeGuid is echoed back as custom_event_node_id.
//   @param connections - [{from_node, from_pin, to_node, to_pin}, ...]; from_node/to_node may be a local 'id' from 'nodes' (or "<id>.custom_event" for an add_object_bound_event step's handler event) or a pre-existing real NodeGuid.
//   @param pin_defaults - [{node, pin, value}, ...]; node may be a local 'id' from 'nodes' (or "<id>.custom_event") or a pre-existing real NodeGuid.
//   @param stop_on_error [bool] - Stop (and try to roll back the whole transaction) on the first failing step; defaults to true, unlike batch's default false, since connections/pin_defaults depend on earlier nodes existing. With stop_on_error:false there is NO rollback at all -- the editor's global undo is never touched, every step is attempted, and whatever half-built shape remains is cleaned up with delete_node/disconnect_pin. That is the safer mode when other work is live in the editor's undo buffer.
//   @returns { blueprint_name, graph_name, nodes: [{index, id, command_type, ok, node_id?, custom_event_node_id?, error?}], connections: [{index, ok, error?}], pin_defaults: [{index, ok, error?}], stopped_early, rollback_required, rolled_back, transaction_opened, undo_queue_len_before, undo_queue_len_after, top_transaction_title, rollback_refused_reason?, warning? }. rolled_back reports whether the undo ACTUALLY ran, not the intention: the rollback is refused (rolled_back:false + rollback_refused_reason + warning) whenever the top of the editor's undo stack is not this call's own transaction, because GEditor->UndoTransaction is global and would otherwise revert another agent's or another session's work in memory while the disk stays clean. A payload that fails pre-validation returns instead { success:false, error, validation_errors: [{section, index, id?, error}], transaction_opened:false, blueprint_name, graph_name } and never opens a transaction.
TSharedPtr<FJsonObject> HandleBuildBlueprintGraph(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UBlueprint* Blueprint = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::FindBlueprint(BlueprintName, Blueprint)) { return Error; }

    FString DefaultGraphName;
    Params->TryGetStringField(TEXT("graph_name"), DefaultGraphName);
    UEdGraph* Graph = FUEAgentGraphUtils::FindGraphByNameDeep(Blueprint, DefaultGraphName);
    if (!Graph)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Graph not found: %s"), *DefaultGraphName));

    const TArray<TSharedPtr<FJsonValue>>* NodesArray = nullptr;
    if (!Params->TryGetArrayField(TEXT("nodes"), NodesArray) || !NodesArray || NodesArray->Num() == 0)
        return mcp_common::MakeErrorReply(TEXT("build_blueprint_graph requires a non-empty 'nodes' array"));

    const TArray<TSharedPtr<FJsonValue>>* ConnectionsArray = nullptr;
    Params->TryGetArrayField(TEXT("connections"), ConnectionsArray);
    const TArray<TSharedPtr<FJsonValue>>* PinDefaultsArray = nullptr;
    Params->TryGetArrayField(TEXT("pin_defaults"), PinDefaultsArray);

    bool bStopOnError = true;
    Params->TryGetBoolField(TEXT("stop_on_error"), bStopOnError);

    // Every entry MUST be a node-creating command of this family (run directly, never through the
    // bridge) AND must return a "node_id" field, or its local 'id' would silently fail to resolve in
    // connections/pin_defaults.
    static const TSet<FString> AllowedNodeCommands = {
        TEXT("add_graph_node"), TEXT("add_timeline_to_blueprint"),
        TEXT("add_component_bound_event"), TEXT("add_input_action_event_node"),
        TEXT("add_enhanced_input_action_node"),
        TEXT("add_reroute_node"), TEXT("add_comment_node"),
        // Three node-creating commands that a build must reach too, or the graphs that need them
        // lose the single-transaction + real-rollback guarantee. add_parent_call_node is the
        // important one: the canonical child-override graph is "Event X -> Parent: X -> our logic",
        // and without it that exact shape (a very common one) cannot be built atomically.
        // Note on add_object_bound_event: it creates TWO nodes (a UK2Node_AddDelegate plus the handler
        // UK2Node_CustomEvent, already wired together). Its top-level "node_id" is the AddDelegate
        // node, so a local id declared for this step resolves to the AddDelegate -- that is what you
        // wire the external instance and the exec flow into. The handler custom event is reachable in
        // the same call under the alias "<id>.custom_event" (registered in the node loop below), and
        // its real NodeGuid is echoed back as the step's "custom_event_node_id".
        TEXT("add_parent_call_node"),
        TEXT("add_broadcast_node"), TEXT("add_object_bound_event")
    };

    // ======================================================================================
    // PRE-VALIDATION -- runs to completion BEFORE any transaction is opened.
    //
    // A malformed payload must NEVER touch the undo stack: a call that fails while validating its
    // FIRST node and then falls through to the rollback path pushes the editor's GLOBAL undo,
    // which undoes the caller's PREVIOUS successful calls and can leave a function graph
    // inconsistent (a Return with an output pin that no longer exists) -- the next compile then
    // crashes the editor under KismetCompiler. The usual trigger is a JSON slip: each
    // sub-command's parameters written at the item's ROOT level instead of nested inside
    // "params":{}. So this pass mutates nothing, accumulates EVERY problem (not just the first, so
    // one round-trip shows the whole list) and returns them with transaction_opened:false.
    // ======================================================================================
    TArray<TSharedPtr<FJsonValue>> ValidationErrors;
    auto AddValidationError = [&ValidationErrors](const TCHAR* Section, int32 ItemIndex, const FString& ItemId, const FString& Message)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("section"), Section);
        Entry->SetNumberField(TEXT("index"), ItemIndex);
        if (!ItemId.IsEmpty()) { Entry->SetStringField(TEXT("id"), ItemId); }
        Entry->SetStringField(TEXT("error"), Message);
        ValidationErrors.Add(MakeShared<FJsonValueObject>(Entry));
    };

    // Local ids declared by this batch, including the "<id>.custom_event" alias that an
    // add_object_bound_event step registers in the node loop below -- connections/pin_defaults
    // are allowed to reference either form.
    TSet<FString> DeclaredIds;

    for (int32 Index = 0; Index < NodesArray->Num(); ++Index)
    {
        const TSharedPtr<FJsonObject> Item = (*NodesArray)[Index].IsValid() ? (*NodesArray)[Index]->AsObject() : nullptr;
        if (!Item.IsValid())
        {
            AddValidationError(TEXT("nodes"), Index, FString(), TEXT("node item must be a JSON object"));
            continue;
        }

        FString LocalId;
        const bool bHasId = Item->TryGetStringField(TEXT("id"), LocalId) && !LocalId.IsEmpty();
        if (!bHasId)
        {
            AddValidationError(TEXT("nodes"), Index, FString(),
                TEXT("node item requires a non-empty string field 'id'"));
        }

        FString SubCommandType;
        const bool bHasCommand = Item->TryGetStringField(TEXT("command_type"), SubCommandType) && !SubCommandType.IsEmpty();
        if (!bHasCommand)
        {
            AddValidationError(TEXT("nodes"), Index, LocalId,
                TEXT("node item requires a non-empty string field 'command_type'"));
        }
        else if (!AllowedNodeCommands.Contains(FMcpToolCatalog::ResolveCallName(SubCommandType)))
        {
            AddValidationError(TEXT("nodes"), Index, LocalId, FString::Printf(
                TEXT("'%s' is not a node-creating command usable inside build_blueprint_graph's 'nodes' array"),
                *SubCommandType));
        }

        // 'params' must be an OBJECT when present. And when it is ABSENT while the item still
        // carries other root-level keys, that IS the typo described above: those keys are silently
        // dropped (only id/command_type/params are ever read) and the sub-command then dies on a
        // missing required parameter -- which, past this check, would push the global undo.
        // Counting keys instead of naming them keeps this independent of FJsonObject's key type
        // (it stores UE::FSharedString keys, not FString).
        if (Item->HasField(TEXT("params")))
        {
            const TSharedPtr<FJsonObject>* ParamsObject = nullptr;
            if (!Item->TryGetObjectField(TEXT("params"), ParamsObject) || !ParamsObject || !ParamsObject->IsValid())
            {
                AddValidationError(TEXT("nodes"), Index, LocalId,
                    TEXT("node item's 'params' must be a JSON object, e.g. \"params\":{\"node_type\":\"event\"}"));
            }
        }
        else
        {
            int32 KnownKeyCount = 0;
            if (Item->HasField(TEXT("id")))           { ++KnownKeyCount; }
            if (Item->HasField(TEXT("command_type"))) { ++KnownKeyCount; }
            const int32 StrayKeyCount = Item->Values.Num() - KnownKeyCount;
            if (StrayKeyCount > 0)
            {
                AddValidationError(TEXT("nodes"), Index, LocalId, FString::Printf(
                    TEXT("node item has no 'params' object but carries %d other root-level key(s); a sub-command's parameters must be NESTED: {\"id\":..., \"command_type\":..., \"params\":{...}}"),
                    StrayKeyCount));
            }
        }

        if (bHasId)
        {
            if (DeclaredIds.Contains(LocalId))
            {
                AddValidationError(TEXT("nodes"), Index, LocalId, FString::Printf(
                    TEXT("duplicate node id '%s' -- ids must be unique inside a batch or connections/pin_defaults silently resolve to the wrong node"),
                    *LocalId));
            }
            else
            {
                DeclaredIds.Add(LocalId);
                if (bHasCommand && SubCommandType == TEXT("add_object_bound_event"))
                {
                    DeclaredIds.Add(LocalId + TEXT(".custom_event"));
                }
            }
        }
    }

    // A node reference is legal when it names a local id declared above (or its
    // "<id>.custom_event" alias) OR is a pre-existing real NodeGuid. FindNodeByGuid resolves the
    // latter through FGuid::Parse and NodeGuid.ToString() is always parseable, so FGuid::Parse is
    // exactly the same oracle the resolver uses -- anything else is a dangling local id.
    auto IsResolvableNodeRef = [&DeclaredIds](const FString& Ref) -> bool
    {
        if (DeclaredIds.Contains(Ref)) { return true; }
        FGuid ParsedGuid;
        return FGuid::Parse(Ref, ParsedGuid);
    };

    if (ConnectionsArray)
    {
        for (int32 Index = 0; Index < ConnectionsArray->Num(); ++Index)
        {
            const TSharedPtr<FJsonObject> Item = (*ConnectionsArray)[Index].IsValid() ? (*ConnectionsArray)[Index]->AsObject() : nullptr;
            if (!Item.IsValid())
            {
                AddValidationError(TEXT("connections"), Index, FString(), TEXT("connection item must be a JSON object"));
                continue;
            }

            FString FromNode, FromPin, ToNode, ToPin;
            const bool bHasFromNode = Item->TryGetStringField(TEXT("from_node"), FromNode) && !FromNode.IsEmpty();
            const bool bHasFromPin  = Item->TryGetStringField(TEXT("from_pin"),  FromPin)  && !FromPin.IsEmpty();
            const bool bHasToNode   = Item->TryGetStringField(TEXT("to_node"),   ToNode)   && !ToNode.IsEmpty();
            const bool bHasToPin    = Item->TryGetStringField(TEXT("to_pin"),    ToPin)    && !ToPin.IsEmpty();
            if (!bHasFromNode || !bHasFromPin || !bHasToNode || !bHasToPin)
            {
                AddValidationError(TEXT("connections"), Index, FString(),
                    TEXT("connection item requires non-empty string fields from_node, from_pin, to_node, to_pin"));
            }
            if (bHasFromNode && !IsResolvableNodeRef(FromNode))
            {
                AddValidationError(TEXT("connections"), Index, FromNode, FString::Printf(
                    TEXT("from_node '%s' is neither a local 'id' declared in this batch's 'nodes' nor a NodeGuid"),
                    *FromNode));
            }
            if (bHasToNode && !IsResolvableNodeRef(ToNode))
            {
                AddValidationError(TEXT("connections"), Index, ToNode, FString::Printf(
                    TEXT("to_node '%s' is neither a local 'id' declared in this batch's 'nodes' nor a NodeGuid"),
                    *ToNode));
            }
        }
    }

    if (PinDefaultsArray)
    {
        for (int32 Index = 0; Index < PinDefaultsArray->Num(); ++Index)
        {
            const TSharedPtr<FJsonObject> Item = (*PinDefaultsArray)[Index].IsValid() ? (*PinDefaultsArray)[Index]->AsObject() : nullptr;
            if (!Item.IsValid())
            {
                AddValidationError(TEXT("pin_defaults"), Index, FString(), TEXT("pin_defaults item must be a JSON object"));
                continue;
            }

            FString NodeRef, PinName, Value;
            const bool bHasNode = Item->TryGetStringField(TEXT("node"), NodeRef) && !NodeRef.IsEmpty();
            const bool bHasPin  = Item->TryGetStringField(TEXT("pin"),  PinName) && !PinName.IsEmpty();
            // 'value' may legitimately be the empty string (clearing a default), so presence as a
            // string is the whole requirement -- same semantics as the executing loop below.
            const bool bHasValue = Item->TryGetStringField(TEXT("value"), Value);
            if (!bHasNode || !bHasPin || !bHasValue)
            {
                AddValidationError(TEXT("pin_defaults"), Index, FString(),
                    TEXT("pin_defaults item requires string fields node, pin, value (node and pin non-empty)"));
            }
            if (bHasNode && !IsResolvableNodeRef(NodeRef))
            {
                AddValidationError(TEXT("pin_defaults"), Index, NodeRef, FString::Printf(
                    TEXT("node '%s' is neither a local 'id' declared in this batch's 'nodes' nor a NodeGuid"),
                    *NodeRef));
            }
        }
    }

    if (ValidationErrors.Num() > 0)
    {
        TSharedPtr<FJsonObject> Rejection = mcp_common::MakeErrorReply(FString::Printf(
            TEXT("build_blueprint_graph payload rejected: %d validation problem(s). Nothing ran and the editor's undo stack was NOT touched."),
            ValidationErrors.Num()));
        Rejection->SetArrayField(TEXT("validation_errors"), ValidationErrors);
        Rejection->SetBoolField(TEXT("transaction_opened"), false);
        Rejection->SetStringField(TEXT("blueprint_name"), BlueprintName);
        Rejection->SetStringField(TEXT("graph_name"), Graph->GetName());
        return Rejection;
    }

    // An error response is the ONLY case any GraphCommands handler sets "success" at all
    // (mcp_common::MakeErrorReply sets success:false; every happy-path handler here
    // returns raw data fields with no "success" key whatsoever).
    auto IsErrorResponse = [](const TSharedPtr<FJsonObject>& R) -> bool
    {
        bool bSuccess = true;
        return R.IsValid() && R->TryGetBoolField(TEXT("success"), bSuccess) && !bSuccess;
    };

    TMap<FString, FString> NodeIdMap; // caller's local 'id' -> real NodeGuid string
    TArray<TSharedPtr<FJsonValue>> NodeResults;
    TArray<TSharedPtr<FJsonValue>> ConnectionResults;
    TArray<TSharedPtr<FJsonValue>> PinDefaultResults;
    bool bStoppedEarly = false;
    bool bAnyError = false;

    // Scoped so the transaction ENDS (commits normally) before we decide whether to roll
    // back. FScopedTransaction::Cancel() does NOT restore object state -- per the engine source
    // (UTransBuffer::Cancel, EditorTransaction.cpp) it only discards the transaction record
    // from the undo/redo history; changes already applied to live objects (e.g. nodes already
    // added to Graph->Nodes) remain in place. Real rollback requires ending the transaction
    // normally and then calling GEditor->UndoTransaction(false) below, which actually applies
    // the recorded "before" snapshots (Transaction.Apply()) and then discards the entry
    // entirely (bCanRedo=false) so no half-built graph is reachable via Ctrl+Z/Ctrl+Y.
    //
    // ...but UndoTransaction is the editor's GLOBAL undo, and nothing guarantees the top of
    // the stack is OURS. Undoing blind can revert ANOTHER task's or session's entries still in
    // the buffer (an "Undo MCP: Set Variable Replication", an "Undo MCP: Connect Pins", an SCS
    // node added earlier) -- blueprints broken IN MEMORY with the disk intact and `git status`
    // clean, and PIE stuck on the compile-error modal. So we fingerprint our own transaction
    // on the way in and refuse to undo anything that is not it.
    UTransactor* Transactor = nullptr;
    if (GEditor) { Transactor = GEditor->Trans; }
    const int32 UndoQueueLenBefore = Transactor ? Transactor->GetQueueLength() : INDEX_NONE;
    // Built once and reused for the top-of-stack title comparison, so the check stays correct
    // under localization (both sides resolve through the same LOCTEXT key).
    const FText BuildTransactionTitle = LOCTEXT("BuildBlueprintGraph", "MCP: Build Blueprint Graph");
    FGuid OurTransactionId;
    {
    FScopedTransaction Transaction(BuildTransactionTitle);
    // UTransBuffer::Begin emplaces the new entry and points GUndo at it, so GUndo is OUR
    // transaction -- but only when we are the outermost one. Its TransactionId is the only
    // unambiguous fingerprint: titles repeat across calls, and a transaction that turns out to be
    // transient is POPPED on End() (UTransBuffer::End -> FTransaction::IsTransient), which would
    // otherwise leave a PREVIOUS build's identically-titled entry sitting on top.
    if (GUndo)
    {
        OurTransactionId = GUndo->GetContext().TransactionId;
    }
    Blueprint->Modify();

    for (int32 Index = 0; Index < NodesArray->Num(); ++Index)
    {
        const TSharedPtr<FJsonObject> Item = (*NodesArray)[Index].IsValid() ? (*NodesArray)[Index]->AsObject() : nullptr;
        TSharedPtr<FJsonObject> NodeResult = MakeShared<FJsonObject>();
        NodeResult->SetNumberField(TEXT("index"), Index);

        FString LocalId, SubCommandType;
        if (!Item.IsValid() || !Item->TryGetStringField(TEXT("id"), LocalId) || LocalId.IsEmpty()
            || !Item->TryGetStringField(TEXT("command_type"), SubCommandType) || SubCommandType.IsEmpty())
        {
            bAnyError = true;
            NodeResult->SetBoolField(TEXT("ok"), false);
            NodeResult->SetStringField(TEXT("error"), TEXT("node item requires string fields 'id' and 'command_type'"));
            NodeResults.Add(MakeShared<FJsonValueObject>(NodeResult));
            if (bStopOnError) { bStoppedEarly = true; break; }
            continue;
        }
        NodeResult->SetStringField(TEXT("id"), LocalId);
        NodeResult->SetStringField(TEXT("command_type"), SubCommandType);

        // A renamed node command's old name runs the new one.
        SubCommandType = FMcpToolCatalog::ResolveCallName(SubCommandType);
        if (!AllowedNodeCommands.Contains(SubCommandType))
        {
            bAnyError = true;
            NodeResult->SetBoolField(TEXT("ok"), false);
            NodeResult->SetStringField(TEXT("error"), FString::Printf(
                TEXT("'%s' is not a node-creating command usable inside build_blueprint_graph's 'nodes' array"), *SubCommandType));
            NodeResults.Add(MakeShared<FJsonValueObject>(NodeResult));
            if (bStopOnError) { bStoppedEarly = true; break; }
            continue;
        }

        TSharedPtr<FJsonObject> SubParams = Item->HasField(TEXT("params")) ? Item->GetObjectField(TEXT("params")) : nullptr;
        if (!SubParams.IsValid()) { SubParams = MakeShared<FJsonObject>(); }
        if (!SubParams->HasField(TEXT("blueprint_name"))) { SubParams->SetStringField(TEXT("blueprint_name"), BlueprintName); }
        // A step never compiles: the whole batch is one transaction, compiled once by the caller afterwards.
        SubParams->RemoveField(TEXT("compile"));
        if (!DefaultGraphName.IsEmpty() && !SubParams->HasField(TEXT("graph_name"))) { SubParams->SetStringField(TEXT("graph_name"), DefaultGraphName); }

        TSharedPtr<FJsonObject> SubResult = McpCommandFamilies::Run(SubCommandType, SubParams);
        const bool bOk = !IsErrorResponse(SubResult);
        NodeResult->SetBoolField(TEXT("ok"), bOk);
        if (bOk)
        {
            FString RealNodeId;
            if (SubResult.IsValid() && SubResult->TryGetStringField(TEXT("node_id"), RealNodeId))
            {
                NodeIdMap.Add(LocalId, RealNodeId);
                NodeResult->SetStringField(TEXT("node_id"), RealNodeId);
            }
            // add_object_bound_event is the one whitelisted step that creates TWO nodes -- the
            // UK2Node_AddDelegate (which IS this step's node_id) and the handler UK2Node_CustomEvent
            // already wired to its Delegate pin. Echo the handler's real NodeGuid back and register the
            // alias "<id>.custom_event" so the SAME call can hang logic off the handler. The plain
            // local id resolves to the AddDelegate node, and the alias only exists for steps that
            // actually reported a custom_event_node_id.
            FString CustomEventNodeId;
            if (SubResult.IsValid()
                && SubResult->TryGetStringField(TEXT("custom_event_node_id"), CustomEventNodeId)
                && !CustomEventNodeId.IsEmpty())
            {
                NodeIdMap.Add(LocalId + TEXT(".custom_event"), CustomEventNodeId);
                NodeResult->SetStringField(TEXT("custom_event_node_id"), CustomEventNodeId);
            }
        }
        else
        {
            bAnyError = true;
            FString ErrorMessage;
            if (SubResult.IsValid()) { SubResult->TryGetStringField(TEXT("error"), ErrorMessage); }
            NodeResult->SetStringField(TEXT("error"), ErrorMessage);
        }
        NodeResults.Add(MakeShared<FJsonValueObject>(NodeResult));

        if (!bOk && bStopOnError) { bStoppedEarly = true; break; }
    }

    if (!bStoppedEarly && ConnectionsArray)
    {
        for (int32 Index = 0; Index < ConnectionsArray->Num(); ++Index)
        {
            const TSharedPtr<FJsonObject> Item = (*ConnectionsArray)[Index].IsValid() ? (*ConnectionsArray)[Index]->AsObject() : nullptr;
            TSharedPtr<FJsonObject> ConnResult = MakeShared<FJsonObject>();
            ConnResult->SetNumberField(TEXT("index"), Index);

            FString FromNode, FromPin, ToNode, ToPin;
            if (!Item.IsValid()
                || !Item->TryGetStringField(TEXT("from_node"), FromNode)
                || !Item->TryGetStringField(TEXT("from_pin"), FromPin)
                || !Item->TryGetStringField(TEXT("to_node"), ToNode)
                || !Item->TryGetStringField(TEXT("to_pin"), ToPin))
            {
                bAnyError = true;
                ConnResult->SetBoolField(TEXT("ok"), false);
                ConnResult->SetStringField(TEXT("error"), TEXT("connection item requires string fields from_node, from_pin, to_node, to_pin"));
                ConnectionResults.Add(MakeShared<FJsonValueObject>(ConnResult));
                if (bStopOnError) { bStoppedEarly = true; break; }
                continue;
            }

            const FString* MappedFrom = NodeIdMap.Find(FromNode);
            const FString* MappedTo = NodeIdMap.Find(ToNode);

            TSharedPtr<FJsonObject> ConnParams = MakeShared<FJsonObject>();
            ConnParams->SetStringField(TEXT("blueprint_name"), BlueprintName);
            if (!DefaultGraphName.IsEmpty()) { ConnParams->SetStringField(TEXT("graph_name"), DefaultGraphName); }
            ConnParams->SetStringField(TEXT("source_node_id"), MappedFrom ? *MappedFrom : FromNode);
            ConnParams->SetStringField(TEXT("source_pin"), FromPin);
            ConnParams->SetStringField(TEXT("target_node_id"), MappedTo ? *MappedTo : ToNode);
            ConnParams->SetStringField(TEXT("target_pin"), ToPin);

            TSharedPtr<FJsonObject> SubResult = HandleConnectPins(ConnParams);
            const bool bOk = !IsErrorResponse(SubResult);
            ConnResult->SetBoolField(TEXT("ok"), bOk);
            if (!bOk)
            {
                bAnyError = true;
                FString ErrorMessage;
                if (SubResult.IsValid()) { SubResult->TryGetStringField(TEXT("error"), ErrorMessage); }
                ConnResult->SetStringField(TEXT("error"), ErrorMessage);
            }
            ConnectionResults.Add(MakeShared<FJsonValueObject>(ConnResult));

            if (!bOk && bStopOnError) { bStoppedEarly = true; break; }
        }
    }

    if (!bStoppedEarly && PinDefaultsArray)
    {
        for (int32 Index = 0; Index < PinDefaultsArray->Num(); ++Index)
        {
            const TSharedPtr<FJsonObject> Item = (*PinDefaultsArray)[Index].IsValid() ? (*PinDefaultsArray)[Index]->AsObject() : nullptr;
            TSharedPtr<FJsonObject> PdResult = MakeShared<FJsonObject>();
            PdResult->SetNumberField(TEXT("index"), Index);

            FString NodeRef, PinName, Value;
            if (!Item.IsValid()
                || !Item->TryGetStringField(TEXT("node"), NodeRef)
                || !Item->TryGetStringField(TEXT("pin"), PinName)
                || !Item->TryGetStringField(TEXT("value"), Value))
            {
                bAnyError = true;
                PdResult->SetBoolField(TEXT("ok"), false);
                PdResult->SetStringField(TEXT("error"), TEXT("pin_defaults item requires string fields node, pin, value"));
                PinDefaultResults.Add(MakeShared<FJsonValueObject>(PdResult));
                if (bStopOnError) { bStoppedEarly = true; break; }
                continue;
            }

            const FString* MappedNode = NodeIdMap.Find(NodeRef);

            TSharedPtr<FJsonObject> PdParams = MakeShared<FJsonObject>();
            PdParams->SetStringField(TEXT("blueprint_name"), BlueprintName);
            if (!DefaultGraphName.IsEmpty()) { PdParams->SetStringField(TEXT("graph_name"), DefaultGraphName); }
            PdParams->SetStringField(TEXT("node_id"), MappedNode ? *MappedNode : NodeRef);
            PdParams->SetStringField(TEXT("pin_name"), PinName);
            PdParams->SetStringField(TEXT("value"), Value);

            TSharedPtr<FJsonObject> SubResult = HandleSetPinDefaultValue(PdParams);
            const bool bOk = !IsErrorResponse(SubResult);
            PdResult->SetBoolField(TEXT("ok"), bOk);
            if (!bOk)
            {
                bAnyError = true;
                FString ErrorMessage;
                if (SubResult.IsValid()) { SubResult->TryGetStringField(TEXT("error"), ErrorMessage); }
                PdResult->SetStringField(TEXT("error"), ErrorMessage);
            }
            PinDefaultResults.Add(MakeShared<FJsonValueObject>(PdResult));

            if (!bOk && bStopOnError) { bStoppedEarly = true; break; }
        }
    }
    } // end transaction scope -- Transaction destructs here and commits normally (End()).

    const bool bRollbackRequired = bStoppedEarly && bAnyError;

    // OWNERSHIP CHECK. UTransBuffer::Undo applies UndoBuffer[QueueLength - UndoCount - 1]
    // (it indexes UndoBuffer[Num - ++UndoCount]), so that index IS what UndoTransaction would
    // revert. We only undo it when its TransactionId is the one we captured on the way in AND its
    // title is ours. Undoing blind is strictly worse than not undoing: it rewrites objects that
    // belong to other work in memory while the disk -- and `git status` -- stay clean.
    // The title check is not redundant with the id: if we ran NESTED inside somebody else's
    // transaction, GUndo (and therefore OurTransactionId) is THEIRS, and only the title tells the
    // two apart. build_blueprint_graph never nests inside itself -- it is not in AllowedNodeCommands.
    const int32 UndoQueueLenAfter = Transactor ? Transactor->GetQueueLength() : INDEX_NONE;
    const int32 UndoCountAfter    = Transactor ? Transactor->GetUndoCount()   : INDEX_NONE;
    const int32 TopIndex = Transactor ? (UndoQueueLenAfter - UndoCountAfter - 1) : INDEX_NONE;
    const FTransaction* TopTransaction = (Transactor && TopIndex >= 0) ? Transactor->GetTransaction(TopIndex) : nullptr;
    const FString TopTransactionTitle = TopTransaction ? TopTransaction->GetTitle().ToString() : FString();
    const FGuid   TopTransactionId    = TopTransaction ? TopTransaction->GetContext().TransactionId : FGuid();

    FString RollbackRefusedReason;
    bool bTopOfStackIsOurs = false;
    if (!Transactor)
    {
        RollbackRefusedReason = TEXT("GEditor->Trans is unavailable, so the undo stack could not be inspected");
    }
    else if (!OurTransactionId.IsValid())
    {
        RollbackRefusedReason = TEXT("this call never got its own entry on the undo stack (GUndo was null: the editor could not transact, or another transaction was already open around us)");
    }
    else if (!TopTransaction)
    {
        RollbackRefusedReason = TEXT("the editor's undo stack is empty at the rollback point");
    }
    else if (TopTransactionId != OurTransactionId)
    {
        RollbackRefusedReason = FString::Printf(
            TEXT("the top of the editor's undo stack is '%s' (transaction %s), NOT this call's transaction (%s). Most likely ours recorded no change and was dropped as transient, or something else pushed a transaction in between -- either way undoing it would revert work that is not ours"),
            *TopTransactionTitle, *TopTransactionId.ToString(), *OurTransactionId.ToString());
    }
    else if (TopTransactionTitle != BuildTransactionTitle.ToString())
    {
        RollbackRefusedReason = FString::Printf(
            TEXT("the top of the editor's undo stack carries this call's transaction id but the title '%s' instead of '%s'"),
            *TopTransactionTitle, *BuildTransactionTitle.ToString());
    }
    else if (UndoQueueLenAfter <= 0)
    {
        RollbackRefusedReason = TEXT("the undo queue is empty, so there is nothing this call can safely undo");
    }
    else
    {
        bTopOfStackIsOurs = true;
    }

    // rolled_back reports what HAPPENED, never the intention: UEditorEngine::UndoTransaction
    // itself returns false when it declines (package save or GC in flight, an outer transaction
    // still open, or an undo barrier in the way).
    bool bRolledBack = false;
    if (bRollbackRequired && bTopOfStackIsOurs)
    {
        bRolledBack = GEditor && GEditor->UndoTransaction(/*bCanRedo=*/false);
        if (!bRolledBack)
        {
            RollbackRefusedReason = TEXT("the top of the stack was ours but GEditor->UndoTransaction declined (package save or GC in progress, an outer transaction still open, or an undo barrier blocks it)");
        }
    }

    if (!bRollbackRequired)
    {
        RollbackRefusedReason.Empty();
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        Graph->NotifyGraphChanged();
    }

    TSharedPtr<FJsonObject> Result = UEAgentGraphCommandsImpl::BlueprintReply(BlueprintName, TEXT("graph_name"), Graph->GetName());
    Result->SetArrayField(TEXT("nodes"), NodeResults);
    Result->SetArrayField(TEXT("connections"), ConnectionResults);
    Result->SetArrayField(TEXT("pin_defaults"), PinDefaultResults);
    Result->SetBoolField(TEXT("stopped_early"), bStoppedEarly);
    Result->SetBoolField(TEXT("rollback_required"), bRollbackRequired);
    Result->SetBoolField(TEXT("rolled_back"), bRolledBack);
    Result->SetBoolField(TEXT("transaction_opened"), true);
    // Readable evidence for the two numbers the ownership check is built on.
    Result->SetNumberField(TEXT("undo_queue_len_before"), UndoQueueLenBefore);
    Result->SetNumberField(TEXT("undo_queue_len_after"), UndoQueueLenAfter);
    Result->SetStringField(TEXT("top_transaction_title"), TopTransactionTitle);
    if (bRollbackRequired && !bRolledBack)
    {
        Result->SetStringField(TEXT("rollback_refused_reason"), RollbackRefusedReason);
        Result->SetStringField(TEXT("warning"), FString::Printf(
            TEXT("Rollback REFUSED, so the graph may have been left HALF-BUILT and nothing was undone. Inspect it before doing anything else: get_graph_nodes (compact=false, watching for linked_to entries pointing at GUIDs that no longer exist) and get_blueprint_components; delete orphan nodes with delete_node and clear dead links with disconnect_pin; then compile_blueprint with return_messages=true. Reason: %s"),
            *RollbackRefusedReason));
    }
    return Result;
}

// ===========================================================================
// PropertyAccess path read/write + dynamic-pin node authoring
// ===========================================================================

namespace
{
    // Resolve a UK2Node_PropertyAccess node by blueprint/graph/node_id and verify its class.
    // UK2Node_PropertyAccess is MinimalAPI with a PRIVATE header (PropertyAccessNode module), so
    // its member functions can't be linked from here -- we identify it by class name and edit its
    // Path/TextPath UPROPERTYs by reflection. Answers with the error reply of the first miss, or null.
    TSharedPtr<FJsonObject> PA_ResolveNode(const TSharedPtr<FJsonObject>& Params,
                                           UBlueprint*& OutBlueprint, UEdGraph*& OutGraph, UEdGraphNode*& OutNode)
    {
        FString BlueprintName, NodeId;
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("node_id"), NodeId, mcp_common::EAllowEmpty::Yes)) { return Missing; }
        if (TSharedPtr<FJsonObject> Error = UEAgentGraphCommandsImpl::ResolveBlueprintGraph(Params, BlueprintName, OutBlueprint, OutGraph))
        {
            return Error;
        }
        if (TSharedPtr<FJsonObject> Error = GraphNodePinLookup::FindNode(OutGraph, NodeId, OutNode))
        {
            return Error;
        }

        // MinimalAPI + private header -> identify by class name (its subclass, if any, would still
        // carry the reflected Path/TextPath UPROPERTYs, but a plain name check is the documented shape).
        if (OutNode->GetClass()->GetName() != TEXT("K2Node_PropertyAccess"))
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Node %s is a %s, not a K2Node_PropertyAccess (property-access nodes only)"),
                *NodeId, *OutNode->GetClass()->GetName()));
        }
        return nullptr;
    }

    // Read Path (array of segments), TextPath (display string), and the resolved output pin.
    void PA_FillPayload(UEdGraphNode* Node, const TSharedPtr<FJsonObject>& Out)
    {
        if (FProperty* PathProp = Node->GetClass()->FindPropertyByName(TEXT("Path")))
        {
            Out->SetField(TEXT("path"),
                mcp_common::PropertyValueToJson(PathProp, PathProp->ContainerPtrToValuePtr<void>(Node)));
        }
        if (FProperty* TextPathProp = Node->GetClass()->FindPropertyByName(TEXT("TextPath")))
        {
            FString TextPathStr;
            if (FTextProperty* TP = CastField<FTextProperty>(TextPathProp))
            {
                TextPathStr = TP->GetPropertyValue_InContainer(Node).ToString();
            }
            Out->SetStringField(TEXT("text_path"), TextPathStr);
        }
        // Resolved leaf type shows up on the single "Value" output pin (wildcard until resolvable).
        if (UEdGraphPin* ValuePin = FUEAgentGraphUtils::FindPinByName(Node, TEXT("Value"), EGPD_Output))
        {
            Out->SetObjectField(TEXT("resolved_pin"), FUEAgentGraphUtils::PinToJson(ValuePin));
            const bool bResolved = ValuePin->PinType.PinCategory != UEdGraphSchema_K2::PC_Wildcard;
            Out->SetBoolField(TEXT("resolved"), bResolved);
        }
        else
        {
            Out->SetBoolField(TEXT("resolved"), false);
        }
    }
}

// MCP:get_property_access_path - Read a UK2Node_PropertyAccess node's binding Path -- the anim-Blueprint property binding (InAnimInstance.Foo, InDeltaTime...) that no ordinary pin read exposes -- as segments plus its display text_path, and say whether the leaf actually RESOLVED (a still-wildcard Value pin means the path points at nothing). Read-only; errors on any node that is not a K2Node_PropertyAccess. Writer twin: set_property_access_path.
//   @param blueprint_name - Blueprint name or path (usually an Animation Blueprint).
//   @param node_id - NodeGuid of the property-access node (from get_graph_nodes).
//   @param graph_name - Target graph name; defaults to the Blueprint's event graph when omitted.
//   @returns { node_id, graph_name, path:[string], text_path, resolved, resolved_pin:{name,direction,type,...} } -- resolved:false (Value pin still wildcard) is the signal that the binding is broken.
TSharedPtr<FJsonObject> HandleGetPropertyAccessPath(const TSharedPtr<FJsonObject>& Params)
{
    UBlueprint* Blueprint = nullptr;
    UEdGraph* Graph = nullptr;
    UEdGraphNode* Node = nullptr;
    if (TSharedPtr<FJsonObject> Error = PA_ResolveNode(Params, Blueprint, Graph, Node)) { return Error; }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
    Result->SetStringField(TEXT("graph_name"), Graph->GetName());
    PA_FillPayload(Node, Result);
    return Result;
}

// MCP:set_property_access_path - Rewrite a UK2Node_PropertyAccess node's binding Path and reconstruct the node so the leaf property re-resolves and the Value pin re-types, then return the path READ BACK -- so resolved:true is real evidence the new binding points at something. It does NOT recompile: follow with compile_blueprint with return_messages=true. reconstructed:false means the PropertyAccessEditor feature was unavailable: the path WAS written but text_path/resolved_pin stay stale until the graph reopens.
//   @param blueprint_name - Blueprint name or path (usually an Animation Blueprint).
//   @param node_id - NodeGuid of the property-access node; any other node class is refused by name.
//   @param path - The new path: either an array of segment strings (["InAnimInstance","Foo"]) OR a dotted string ("InAnimInstance.Foo"). An empty result is an error, never a cleared binding.
//   @param graph_name - Target graph name; defaults to the Blueprint's event graph when omitted.
//   @returns { node_id, graph_name, reconstructed, path:[string], text_path, resolved, resolved_pin, note? } -- path/text_path/resolved are re-read AFTER the reconstruct, so they are the oracle rather than an echo.
TSharedPtr<FJsonObject> HandleSetPropertyAccessPath(const TSharedPtr<FJsonObject>& Params)
{
    UBlueprint* Blueprint = nullptr;
    UEdGraph* Graph = nullptr;
    UEdGraphNode* Node = nullptr;
    if (TSharedPtr<FJsonObject> Error = PA_ResolveNode(Params, Blueprint, Graph, Node)) { return Error; }

    // Accept path as an array of segment strings OR a single dotted string.
    TArray<FString> Segments;
    const TArray<TSharedPtr<FJsonValue>>* PathArr = nullptr;
    if (Params->TryGetArrayField(TEXT("path"), PathArr) && PathArr)
    {
        for (const TSharedPtr<FJsonValue>& V : *PathArr)
        {
            FString S;
            if (V.IsValid() && V->TryGetString(S) && !S.IsEmpty())
            {
                Segments.Add(S);
            }
        }
    }
    else
    {
        FString PathStr;
        if (Params->TryGetStringField(TEXT("path"), PathStr) && !PathStr.IsEmpty())
        {
            PathStr.ParseIntoArray(Segments, TEXT("."), /*InCullEmpty=*/true);
        }
    }
    if (Segments.Num() == 0)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Parameter 'path' is required as a non-empty array of segment strings or a dotted string"));
    }

    FScopedTransaction Transaction(LOCTEXT("SetPropertyAccessPath", "MCP: Set Property Access Path"));
    Blueprint->Modify();
    Graph->Modify();
    Node->Modify();

    // Set the Path UPROPERTY (TArray<FString>) via the canonical JSON->property route.
    TSharedPtr<FJsonValue> ArrayValue = MakeShared<FJsonValueArray>(mcp_common::StringsToJson(Segments));
    FString SetError;
    if (!mcp_common::SetObjectProperty(Node, TEXT("Path"), ArrayValue, SetError))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Failed to set Path on property-access node: %s"), *SetError));
    }

    // ReconstructNode re-resolves the leaf property, refreshes TextPath, and re-types the Value pin
    // (UK2Node_PropertyAccess::AllocatePins). AllocatePins hard-requires the "PropertyAccessEditor"
    // modular feature (GetModularFeature asserts if missing), so guard the call to stay crash-safe.
    bool bReconstructed = false;
    if (IModularFeatures::Get().IsModularFeatureAvailable(TEXT("PropertyAccessEditor")))
    {
        Node->ReconstructNode();
        bReconstructed = true;
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    Graph->NotifyGraphChanged();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
    Result->SetStringField(TEXT("graph_name"), Graph->GetName());
    Result->SetBoolField(TEXT("reconstructed"), bReconstructed);
    if (!bReconstructed)
    {
        Result->SetStringField(TEXT("note"),
            TEXT("Path set but node NOT reconstructed: the 'PropertyAccessEditor' modular feature is not available (TextPath/resolved pin may be stale until the graph reopens)."));
    }
    PA_FillPayload(Node, Result);
    return Result;
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_blueprint_variable"), &HandleAddBlueprintVariable },
    { TEXT("add_broadcast_node"), &HandleAddBroadcastNode },
    { TEXT("add_comment_node"), &HandleAddCommentNode },
    { TEXT("add_component_bound_event"), &HandleAddComponentBoundEvent },
    { TEXT("add_enhanced_input_action_node"), &HandleAddEnhancedInputActionNode },
    { TEXT("add_event_dispatcher"), &HandleAddEventDispatcher },
    { TEXT("add_function_local_variable"), &HandleAddFunctionLocalVariable },
    { TEXT("add_function_parameter"), &HandleAddFunctionParameter },
    { TEXT("add_blueprint_timeline_track"), &HandleAddBlueprintTimelineTrack },
    { TEXT("get_blueprint_timeline_info"), &HandleGetBlueprintTimelineInfo },
    { TEXT("remove_blueprint_macro"), &HandleRemoveBlueprintMacro },
    { TEXT("remove_blueprint_timeline_track"), &HandleRemoveBlueprintTimelineTrack },
    { TEXT("remove_function_local_variable"), &HandleRemoveFunctionLocalVariable },
    { TEXT("remove_function_parameter"), &HandleRemoveFunctionParameter },
    { TEXT("add_graph_node"), &HandleAddGraphNode },
    { TEXT("add_input_action_event_node"), &HandleAddInputActionEventNode },
    { TEXT("add_object_bound_event"), &HandleAddObjectBoundEvent },
    { TEXT("add_parent_call_node"), &HandleAddParentCallNode },
    { TEXT("add_reroute_node"), &HandleAddRerouteNode },
    { TEXT("add_timeline_to_blueprint"), &HandleAddTimelineToBlueprint },
    { TEXT("build_blueprint_graph"), &HandleBuildBlueprintGraph },
    { TEXT("connect_pins"), &HandleConnectPins },
    { TEXT("create_blueprint_macro"), &HandleCreateBlueprintMacro },
    { TEXT("delete_blueprint_variable"), &HandleDeleteBpVariable },
    { TEXT("delete_node"), &HandleDeleteNode },
    { TEXT("disconnect_pin"), &HandleDisconnectPin },
    { TEXT("get_event_dispatchers"), &HandleGetEventDispatchers },
    { TEXT("get_function_pins"), &HandleGetFunctionPins },
    { TEXT("get_graph_nodes"), &HandleGetGraphNodes },
    { TEXT("get_property_access_path"), &HandleGetPropertyAccessPath },
    { TEXT("implement_interface"), &HandleImplementInterface },
    { TEXT("lint_blueprint_graph"), &HandleLintBlueprintGraph },
    { TEXT("list_widget_bound_events"), &HandleListWidgetBoundEvents },
    { TEXT("move_node"), &HandleMoveNode },
    { TEXT("recombine_blueprint_pin"), &HandleRecombineBlueprintPin },
    { TEXT("remove_event_dispatcher"), &HandleRemoveEventDispatcher },
    { TEXT("remove_interface"), &HandleRemoveInterface },
    { TEXT("rename_blueprint_variable"), &HandleRenameBpVariable },
    { TEXT("rename_function_parameter"), &HandleRenameFunctionParameter },
    { TEXT("set_blueprint_function_pure"), &HandleSetBlueprintFunctionPure },
    { TEXT("set_blueprint_class_replication"), &HandleSetBpClassReplication },
    { TEXT("set_blueprint_function_rpc"), &HandleSetBpFunctionRpc },
    { TEXT("set_blueprint_variable_default"), &HandleSetBpVariableDefault },
    { TEXT("set_blueprint_variable_metadata"), &HandleSetBpVariableMetadata },
    { TEXT("set_blueprint_variable_replication"), &HandleSetBpVariableReplication },
    { TEXT("set_component_replication"), &HandleSetComponentReplication },
    { TEXT("set_event_dispatcher_signature"), &HandleSetEventDispatcherSignature },
    { TEXT("set_pin_default_value"), &HandleSetPinDefaultValue },
    { TEXT("set_property_access_path"), &HandleSetPropertyAccessPath },
    { TEXT("set_replication_condition"), &HandleSetReplicationCondition },
    { TEXT("split_blueprint_pin"), &HandleSplitBlueprintPin },
    { TEXT("validate_pin_connection"), &HandleValidatePinConnection },
};
} // namespace UEAgentGraphCommands

#undef LOCTEXT_NAMESPACE
