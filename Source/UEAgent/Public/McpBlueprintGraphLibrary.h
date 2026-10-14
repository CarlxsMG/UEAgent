#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "McpBlueprintGraphLibrary.generated.h"

class UEdGraph;
class UEdGraphNode;

// Fills the ONE primitive missing from the UE 5.8 graph API for editor Python.
// UE 5.8 already exposes almost all of the live Blueprint->graph->node->pin model through the engine
// module /Script/BlueprintEditorLibrary (unreal.BlueprintEditorLibrary.list_graphs / list_all_pins /
// find_*_pin / get_node_title ... + the unreal.BlueprintGraphPin struct) and unreal.K2Node.*.
// What it lacks is a "graph -> its nodes" enumerator: UEdGraph::Nodes is a protected UPROPERTY
// (get_editor_property('Nodes') -> "protected and cannot be read") and there is no list_nodes(graph) in
// any engine library (the only enumerator is get_nodes_in_comment, limited to a comment box).
// This reflected library (the plugin's own) closes that gap by returning the nodes as LIVE objects,
// so a script does not have to go through the JSON output of the get_graph_nodes MCP tool.
// It is a Python API (surfaced by UE's Python plugin at editor startup), NOT an MCP tool:
// after a rebuild and an editor restart it is available in execute_python.
UCLASS()
class UEAGENT_API UMcpBlueprintGraphLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Returns ALL the nodes of a graph as live objects (unreal.EdGraphNode / concrete subclasses
	 * such as unreal.K2Node_CallFunction), ready to combine with the engine API:
	 * BlueprintEditorLibrary.list_all_pins(node) / get_node_title(node), node.list_all_pins(), etc.
	 *
	 * meta=(ScriptMethod) => exposed both as a method on the graph itself
	 * (graph.list_graph_nodes()) and as a static (unreal.McpBlueprintGraphLibrary.list_graph_nodes(graph)),
	 * just as the engine does with list_all_pins.
	 *
	 * Scope: the nodes of the given graph (top level: the ubergraph, function, macro or delegate graph
	 * that BlueprintEditorLibrary.list_graphs returns). To drill into composites/macros, iterate the
	 * subgraph of the composite node (not done here).
	 *
	 * @param Graph  A live graph (e.g. from unreal.BlueprintEditorLibrary.list_graphs(bp)).
	 * @return       The nodes of the graph; an empty array if Graph is null.
	 */
	UFUNCTION(BlueprintCallable, Category = "MCP|Blueprint Graph", meta = (ScriptMethod))
	static TArray<UEdGraphNode*> ListGraphNodes(UEdGraph* Graph);
};
