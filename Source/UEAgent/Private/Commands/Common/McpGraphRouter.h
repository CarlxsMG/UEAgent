// Copyright (c) 2026 CarlxsMG.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * One graph API for every graph asset. The graph tools -- get_graph_nodes, add_graph_node, remove_graph_node,
 * connect_pins, disconnect_pin, set_pin_default_value -- edit a Blueprint (an Animation or Widget Blueprint included)
 * with their own code, and hand a call on any other graph asset to the tool of that asset's editor, translating the
 * parameters: a material or material function, a sound cue, a MetaSound Source, a PCG graph, a Control Rig.
 *
 * In every family a node is named by `node_id` (a Blueprint's NodeGuid, a material expression's name, a sound cue
 * node's index, a MetaSound node's GUID, a PCG node's name, a RigVM node's name) and a pin by its name. The reserved
 * node ids `@input` and `@output` name a graph's own input and output: a material's main inputs (BaseColor...), a
 * sound cue's first node, a MetaSound's graph inputs and outputs, a PCG graph's input and output nodes. `@` cannot be
 * in an object name, and a GUID has none.
 *
 * The family tool makes the call (mcp_common::ForwardCall), so its guards, its saves and its READBACK contract apply,
 * and the reply is that tool's, with `graph_kind` and `node_id` added.
 */
namespace McpGraphRouter
{
	/**
	 * Null when Params name a Blueprint, or an asset that is not loaded by path (a Blueprint's short name): the tool's
	 * own code handles it. Otherwise the reply of the call made by the asset's own tool, or the error that says why
	 * the call does not apply to that asset. Tool is the graph tool's name.
	 */
	TSharedPtr<FJsonObject> Route(const TCHAR* Tool, const TSharedPtr<FJsonObject>& Params);
}
