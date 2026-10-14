#include "McpBlueprintGraphLibrary.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"

TArray<UEdGraphNode*> UMcpBlueprintGraphLibrary::ListGraphNodes(UEdGraph* Graph)
{
	TArray<UEdGraphNode*> Result;
	if (Graph)
	{
		// UEdGraph::Nodes is TArray<TObjectPtr<UEdGraphNode>> (protected for Python); copy it
		// into a reflected TArray<UEdGraphNode*>. The ranged-for converts each TObjectPtr to a raw ptr.
		Result.Reserve(Graph->Nodes.Num());
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node)
			{
				Result.Add(Node);
			}
		}
	}
	return Result;
}
