#pragma once

#include "CoreMinimal.h"
#include "Json.h"
#include "EdGraph/EdGraphPin.h"

class UBlueprint;
class UClass;
class UScriptStruct;
class UEdGraph;
class UEdGraphNode;

/**
 * Shared helpers for the Blueprint-graph and user-type command handlers:
 * robust class/struct resolution, a string -> FEdGraphPinType resolver
 * (FPinTypeResolver), graph/node lookup, and node/pin JSON serialization.
 */
class UEAGENT_API FUEAgentGraphUtils
{
public:
    // --- Type resolution -------------------------------------------------
    /** Resolve a UClass from: full path (/Script/Engine.Actor, /Game/.../BP.BP_C),
     *  a Blueprint asset path (returns its GeneratedClass), or a bare/native name
     *  (with or without A/U prefixes). Returns nullptr if unresolved. */
    static UClass* ResolveClass(const FString& Identifier);

    /** Resolve a UScriptStruct from a path, bare name, or a common engine struct
     *  alias (Vector, Rotator, Transform, Vector2D, LinearColor, Color, Quat). */
    static UScriptStruct* ResolveScriptStruct(const FString& Identifier);

    /** Resolve a UEnum from a path or bare name. */
    static UEnum* ResolveEnum(const FString& Identifier);

    /**
     * Build the error message for a class identifier ResolveClass could NOT resolve.
     * Deliberately not a dead end: sweeps the loaded UClasses for near misses
     * (substring OR subsequence, both directions), reports a USTRUCT/UENUM carrying
     * the same name, and states which spellings are accepted.
     *
     * Why it exists: a class can be LOADED and still not resolve under the name the caller
     * used. The registered UClass FName of MetaHuman's character instance is
     * `MetaHumanInstance` (/Script/MetaHumanCharacterPalette.MetaHumanInstance), while the C++
     * header and the Python bindings use the TYPE name `MetaHumanCharacterInstance`. A bare
     * "Class not found" cannot tell "no such class" from "you spelled the type name", so the
     * caller reads an ABSENCE where there is a NAME MISMATCH. Subsequence matching is the rule
     * that catches it: "MetaHumanInstance" is a SUBSEQUENCE of "MetaHumanCharacterInstance",
     * not a substring, so Contains() alone misses it.
     *
     * The sweep is O(loaded classes) and runs ONLY after resolution already failed, so the
     * success path pays nothing. Call it wherever a bare "Class not found: %s" would go.
     */
    static FString DescribeClassNotFound(const FString& Identifier);

    /**
     * FPinTypeResolver: map a human type string to an FEdGraphPinType. Covers
     * bool, int/int32, int64, float/double/real, string, name, text, byte,
     * object/class references (/Game/... or native names), structs, enums, and
     * a trailing container suffix ("[]" / "<Array>"/"<Set>"). Returns false if
     * the base type could not be resolved.
     */
    static bool ResolvePinType(const FString& TypeName, FEdGraphPinType& OutPinType);

    // --- Graph / node lookup --------------------------------------------
    /** Find a graph by name across Ubergraph/Function/Macro/Delegate pages.
     *  Empty or "EventGraph" returns (creating if needed) the event graph. */
    static UEdGraph* FindGraphByName(UBlueprint* Blueprint, const FString& GraphName);

    /** Deep variant of FindGraphByName: additionally descends into collapsed
     *  composite subgraphs (UEdGraph::SubGraphs) and macro-instance graphs. A
     *  name matches by graph object name -- which, for a collapsed composite,
     *  equals the K2Node_Composite's displayed title (ListView). The whole name is
     *  tried as an exact title FIRST (so a title that literally contains '/', e.g.
     *  "Mesh/Actor Spawning", resolves as-is), then as a path form "Outer > Inner"
     *  whose separator is ">" ONLY (never "/") to disambiguate nested graphs that
     *  share a name. Empty / "EventGraph" => the event graph. This is the single
     *  resolver shared by the graph read AND write commands. */
    static UEdGraph* FindGraphByNameDeep(UBlueprint* Blueprint, const FString& GraphName);

    /** If Node owns a nested graph -- a collapsed composite's bound graph or a
     *  macro instance's macro graph -- return it; otherwise nullptr. */
    static UEdGraph* GetNodeSubGraph(UEdGraphNode* Node);

    /** Find a node by its NodeGuid string within a graph. */
    static UEdGraphNode* FindNodeByGuid(UEdGraph* Graph, const FString& NodeId);

    /** Find a pin on a node by name (optionally constrained by direction). */
    static UEdGraphPin* FindPinByName(UEdGraphNode* Node, const FString& PinName,
                                      EEdGraphPinDirection Direction = EGPD_MAX);

    // --- Serialization ---------------------------------------------------
    // Trailing bCompact (default false, ABI-safe) drives a token-lean
    // serialization for get_graph_nodes. Compact PinToJson emits only
    // name+direction+linked_to (+default_value only when non-empty); compact
    // NodeToJson drops pos_x/pos_y/comment and skips unlinked-defaultless pins.
    // Every existing caller passes neither arg, so output stays byte-identical.
    static TSharedPtr<FJsonObject> PinToJson(UEdGraphPin* Pin, bool bCompact = false);
    static TSharedPtr<FJsonObject> NodeToJson(UEdGraphNode* Node, bool bIncludePins = true, bool bCompact = false);
};
