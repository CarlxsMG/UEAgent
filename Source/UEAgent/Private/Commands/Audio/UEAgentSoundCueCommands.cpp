// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

#include "Sound/SoundCue.h"
#include "Sound/SoundNode.h"
#include "Sound/SoundNodeWavePlayer.h"
#include "Sound/SoundNodeMixer.h"
#include "Sound/SoundNodeRandom.h"
#include "Sound/SoundNodeLooping.h"
#include "Sound/SoundNodeModulator.h"
#include "Sound/SoundNodeDelay.h"
#include "Sound/SoundNodeConcatenator.h"
// Additional headless USoundNode types
#include "Sound/SoundNodeAttenuation.h"
#include "Sound/SoundNodeBranch.h"
#include "Sound/SoundNodeDistanceCrossFade.h"
#include "Sound/SoundNodeDoppler.h"
#include "Sound/SoundNodeEnveloper.h"
#include "Sound/SoundNodeGroupControl.h"
#include "Sound/SoundNodeModulatorContinuous.h"
#include "Sound/SoundNodeOscillator.h"
#include "Sound/SoundNodeParamCrossFade.h"
#include "Sound/SoundNodeQualityLevel.h"
#include "Sound/SoundNodeSoundClass.h"
#include "Sound/SoundNodeSwitch.h"
#include "Sound/SoundNodeWaveParam.h"
#include "Sound/SoundWave.h"

#include "JsonObjectConverter.h"   // FJsonObjectConverter (generic node-property get/set)
#include "UObject/UnrealType.h"    // TFieldIterator / FProperty (applied_keys reflection)

#include "UObject/UObjectGlobals.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "EdGraph/EdGraph.h"         // remove_sound_cue_node: the cue editor graph node goes with the sound node
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"       // disconnect_sound_cue_nodes: the editor graph's wire of the emptied slot

#define LOCTEXT_NAMESPACE "UEAgentSoundCueCommands"

/**
 * Sound Cue authoring commands.
 *
 * Authors USoundCue node graphs structurally and headlessly (no USoundWave is required to build a graph),
 * on USoundCue (FirstNode, AllNodes, RecursiveFindAllNodes, CacheAggregateValues), USoundNode
 * (ChildNodes, GetMaxChildNodes) and USoundNodeWavePlayer::SetSoundWave.
 *
 * Node objects are created via NewObject<T>(Cue, ..., RF_Transactional) and added to USoundCue::AllNodes
 * manually, deliberately AVOIDING USoundCue::ConstructSoundNode / SetupSoundNode: they require the
 * AudioEditor's ISoundCueAudioEditor and an EdGraph, which are only registered lazily, so going around
 * them keeps the path headless-safe.
 */
namespace UEAgentSoundCueCommands
{

// MCP-DOMAIN: Audio

namespace
{
    // Load a USoundCue from a /Game asset path (idempotent helper).
    USoundCue* LoadSoundCue(const FString& CuePath)
    {
        return Cast<USoundCue>(StaticLoadObject(USoundCue::StaticClass(), nullptr, *CuePath));
    }

    // LoadSoundCue: null once OutCue holds the cue, otherwise the error reply.
    TSharedPtr<FJsonObject> SoundCue_Load(const FString& CuePath, USoundCue*& OutCue)
    {
        OutCue = LoadSoundCue(CuePath);
        if (!OutCue)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("SoundCue not found: %s"), *CuePath));
        }
        return nullptr;
    }

    // The node NodeId of Cue: null once OutNode holds it, otherwise the error reply naming the IdParam that
    // is out of range, or the Role of a null node.
    TSharedPtr<FJsonObject> SoundCue_FindNode(USoundCue* Cue, int32 NodeId, const TCHAR* IdParam, const TCHAR* Role,
        USoundNode*& OutNode)
    {
        if (!Cue->AllNodes.IsValidIndex(NodeId))
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("%s %d out of range (node_count=%d)"), IdParam, NodeId, Cue->AllNodes.Num()));
        }
        OutNode = Cue->AllNodes[NodeId];
        if (!OutNode)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Resolved %s was null"), Role));
        }
        return nullptr;
    }

    // Whether Target is among the inputs of Start, directly or through other nodes: reached from Start through
    // ChildNodes, so Target feeds Start. Each node is visited once, so a loop already in the graph does not
    // hang the walk.
    bool SoundCue_FeedsInto(const USoundNode* Target, const USoundNode* Start)
    {
        TArray<const USoundNode*> Pending;
        TSet<const USoundNode*> Visited;
        Pending.Add(Start);
        while (Pending.Num() > 0)
        {
            const USoundNode* Node = Pending.Pop();
            if (Node == nullptr || Visited.Contains(Node))
            {
                continue;
            }
            Visited.Add(Node);
            for (const TObjectPtr<USoundNode>& Child : Node->ChildNodes)
            {
                if (Child.Get() == Target)
                {
                    return true;
                }
                Pending.Add(Child.Get());
            }
        }
        return false;
    }

#if WITH_EDITORONLY_DATA
    // The editor-graph twin of emptying input slot SlotIndex of Node: a cue made in the Sound Cue editor keeps a graph
    // node per sound node whose input pins, in order, are its ChildNodes slots, and the editor rebuilds ChildNodes
    // from those pins, so the wire into that pin goes too. Returns how many links were broken (0 when the node has no
    // graph node or the pin holds no link).
    int32 SoundCue_BreakGraphInputLinks(USoundNode* Node, int32 SlotIndex)
    {
        UEdGraphNode* GraphNode = Node ? Node->GraphNode.Get() : nullptr;
        if (!GraphNode)
        {
            return 0;
        }
        int32 InputOrdinal = 0;
        for (UEdGraphPin* Pin : GraphNode->Pins)
        {
            if (!Pin || Pin->Direction != EGPD_Input)
            {
                continue;
            }
            if (InputOrdinal++ != SlotIndex)
            {
                continue;
            }
            const int32 Links = Pin->LinkedTo.Num();
            if (Links > 0)
            {
                GraphNode->Modify();
                Pin->BreakAllPinLinks();   // Modify()s both ends' nodes, so undo restores the wire
            }
            return Links;
        }
        return 0;
    }
#endif

    // Map a node_type token -> concrete USoundNode subclass. Returns nullptr if unknown.
    UClass* ResolveSoundNodeClass(const FString& NodeType)
    {
        const FString T = NodeType.ToLower();
        if (T == TEXT("waveplayer"))   { return USoundNodeWavePlayer::StaticClass(); }
        if (T == TEXT("mixer"))        { return USoundNodeMixer::StaticClass(); }
        if (T == TEXT("random"))       { return USoundNodeRandom::StaticClass(); }
        if (T == TEXT("looping"))      { return USoundNodeLooping::StaticClass(); }
        if (T == TEXT("modulator"))    { return USoundNodeModulator::StaticClass(); }
        if (T == TEXT("delay"))        { return USoundNodeDelay::StaticClass(); }
        if (T == TEXT("concatenator")) { return USoundNodeConcatenator::StaticClass(); }
        // The rest of the headless-constructible USoundNode taxonomy.
        if (T == TEXT("attenuation"))        { return USoundNodeAttenuation::StaticClass(); }
        if (T == TEXT("branch"))             { return USoundNodeBranch::StaticClass(); }
        if (T == TEXT("distancecrossfade"))  { return USoundNodeDistanceCrossFade::StaticClass(); }
        if (T == TEXT("doppler"))            { return USoundNodeDoppler::StaticClass(); }
        if (T == TEXT("enveloper"))          { return USoundNodeEnveloper::StaticClass(); }
        if (T == TEXT("groupcontrol"))       { return USoundNodeGroupControl::StaticClass(); }
        if (T == TEXT("modulatorcontinuous")){ return USoundNodeModulatorContinuous::StaticClass(); }
        if (T == TEXT("oscillator"))         { return USoundNodeOscillator::StaticClass(); }
        if (T == TEXT("paramcrossfade"))     { return USoundNodeParamCrossFade::StaticClass(); }
        if (T == TEXT("qualitylevel"))       { return USoundNodeQualityLevel::StaticClass(); }
        if (T == TEXT("soundclass"))         { return USoundNodeSoundClass::StaticClass(); }
        if (T == TEXT("switch"))             { return USoundNodeSwitch::StaticClass(); }
        if (T == TEXT("waveparam"))          { return USoundNodeWaveParam::StaticClass(); }
        return nullptr;
    }
}

// ---------------------------------------------------------------------------
// create_sound_cue
// ---------------------------------------------------------------------------
// MCP:create_sound_cue - Create an empty USoundCue asset (no nodes and no first node, so it plays nothing yet) at package_path/name and save it. Idempotent: a sound cue already at that path is returned unchanged with created=false and already_existed=true, and nothing is saved; an asset of another class there is an error and nothing is written. Not an undo step. Build the graph next with add_graph_node and connect_pins (to @output for the first node), and read it back with get_graph_nodes; cue_path is their asset_path. A failed save makes success false, with saved=false and the new cue left in memory.
//   @param name - Asset name, e.g. 'SC_Footsteps'; package_path/name must form a valid long package name.
//   @param package_path - Content folder as a long package path, e.g. '/Game/Audio/Cues' (default /Game/SoundCues); trailing slashes are ignored.
//   @returns { name: string, package_path: string, asset_path: string, cue_path: string, created: bool, already_existed: bool, node_count: int, saved: bool, message: string }
TSharedPtr<FJsonObject> HandleCreateSoundCue(const TSharedPtr<FJsonObject>& Params)
{
    FString Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name)) { return Missing; }

    FString PackagePath;
    if (!Params->TryGetStringField(TEXT("package_path"), PackagePath) || PackagePath.IsEmpty())
    {
        PackagePath = TEXT("/Game/SoundCues");
    }
    // Normalize: strip any trailing slash.
    while (PackagePath.EndsWith(TEXT("/")))
    {
        PackagePath = PackagePath.LeftChop(1);
    }

    const FString FullPath = FString::Printf(TEXT("%s/%s"), *PackagePath, *Name);
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(FullPath)) { return Invalid; }

    // Idempotent: if it already exists, load + return it.
    if (UEditorAssetLibrary::DoesAssetExist(FullPath))
    {
        USoundCue* Existing = LoadSoundCue(FullPath);
        if (Existing)
        {
            TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
            Obj->SetStringField(TEXT("name"), Name);
            Obj->SetStringField(TEXT("package_path"), PackagePath);
            Obj->SetStringField(TEXT("asset_path"), Existing->GetPathName());
            Obj->SetStringField(TEXT("cue_path"), FullPath);
            Obj->SetBoolField(TEXT("created"), false);
            Obj->SetBoolField(TEXT("already_existed"), true);
            Obj->SetNumberField(TEXT("node_count"), Existing->AllNodes.Num());
            Obj->SetStringField(TEXT("message"), TEXT("SoundCue already exists; loaded existing asset"));
            return mcp_common::MakeOkReply(Obj);
        }
    }

    // Created directly, not through USoundCueFactoryNew (which adds no nodes without a wave anyway).
    FString CreateError;
    USoundCue* NewCue = mcp_common::NewAssetInPackage<USoundCue>(FullPath, Name, TEXT("Failed to create USoundCue"), CreateError);
    if (!NewCue)
    {
        return mcp_common::MakeErrorReply(CreateError);
    }

    FAssetRegistryModule::AssetCreated(NewCue);

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(NewCue->GetPackage(), NewCue, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("name"), Name);
    Obj->SetStringField(TEXT("package_path"), PackagePath);
    Obj->SetStringField(TEXT("asset_path"), NewCue->GetPathName());
    Obj->SetStringField(TEXT("cue_path"), FullPath);
    Obj->SetBoolField(TEXT("created"), true);
    Obj->SetBoolField(TEXT("already_existed"), false);
    Obj->SetNumberField(TEXT("node_count"), NewCue->AllNodes.Num());
    Obj->SetStringField(TEXT("message"), bSaved
        ? TEXT("SoundCue created and saved to disk")
        : FString::Printf(TEXT("SoundCue created but save failed: %s"), *SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("create_sound_cue"), SaveError);
}

// ---------------------------------------------------------------------------
// add_sound_cue_node
// ---------------------------------------------------------------------------
// MCP:add_sound_cue_node - Append a new node of node_type to the AllNodes of a USoundCue asset, in one undo step, and save the cue; node_id is its index there, the id the other sound cue tools take. node_type is matched without case, and an unknown type or a cue that does not load is an error. The node starts unconnected and is not the cue output: wire it with connect_sound_cue_nodes and make the root with set_sound_cue_first_node. For a WavePlayer, sound_wave_path sets its wave; a wave that does not load is not an error (sound_wave_set=false), and other types ignore it. Each call adds another node (not idempotent). A failed save, as during PIE, makes success false with save_error. Node settings: set_sound_cue_node_properties; read back: get_sound_cue_graph; a new cue: create_sound_cue. Inverse: remove_sound_cue_node.
//   @retired add_graph_node - asset_path=<sound cue>, node_type=<WavePlayer, Mixer, Random, Looping, Modulator...>, sound_wave_path=<sound wave, WavePlayer only>; the reply's node_id is the node index
//   @param cue_path - Full asset path of the cue, e.g. /Game/SoundCues/SC_Foo
//   @param node_type - One of WavePlayer|Mixer|Random|Looping|Modulator|Delay|Concatenator|Attenuation|Branch|DistanceCrossFade|Doppler|Enveloper|GroupControl|ModulatorContinuous|Oscillator|ParamCrossFade|QualityLevel|SoundClass|Switch|WaveParam (matched without case)
//   @param sound_wave_path - Optional USoundWave path for WavePlayer nodes only; one that does not load leaves the node without a wave (sound_wave_set false)
//   @returns { cue_path: string, node_id: int, node_type: string, node_class: string, max_child_nodes: int, child_count: int, sound_wave_set: bool, sound_wave_path: string, node_count: int, saved: bool }
TSharedPtr<FJsonObject> HandleAddSoundCueNode(const TSharedPtr<FJsonObject>& Params)
{
    FString CuePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("cue_path"), CuePath)) { return Missing; }

    FString NodeType;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("node_type"), NodeType)) { return Missing; }

    USoundCue* Cue = nullptr;
    if (TSharedPtr<FJsonObject> LoadError = SoundCue_Load(CuePath, Cue)) { return LoadError; }

    UClass* NodeClass = ResolveSoundNodeClass(NodeType);
    if (!NodeClass)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Unknown node_type '%s'. Valid: WavePlayer, Mixer, Random, Looping, Modulator, Delay, Concatenator, "
                 "Attenuation, Branch, DistanceCrossFade, Doppler, Enveloper, GroupControl, ModulatorContinuous, "
                 "Oscillator, ParamCrossFade, QualityLevel, SoundClass, Switch, WaveParam"), *NodeType));
    }

    const FScopedTransaction Transaction(LOCTEXT("AddSoundCueNode", "Add Sound Cue Node"));
    Cue->Modify();

    // Construct the node directly (NOT via ConstructSoundNode/SetupSoundNode which
    // require the AudioEditor graph). Add to AllNodes ourselves so the cue tracks it.
    USoundNode* NewNode = NewObject<USoundNode>(Cue, NodeClass, NAME_None, RF_Transactional);
    if (!NewNode)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Failed to construct node of type %s"), *NodeType));
    }
    NewNode->Modify();

    Cue->AllNodes.Add(NewNode);

    // For a fresh WavePlayer optionally attach a USoundWave.
    bool bWaveSet = false;
    FString SoundWavePath;
    if (NodeType.ToLower() == TEXT("waveplayer") &&
        Params->TryGetStringField(TEXT("sound_wave_path"), SoundWavePath) && !SoundWavePath.IsEmpty())
    {
        USoundWave* Wave = Cast<USoundWave>(StaticLoadObject(USoundWave::StaticClass(), nullptr, *SoundWavePath));
        if (Wave)
        {
            if (USoundNodeWavePlayer* WavePlayer = Cast<USoundNodeWavePlayer>(NewNode))
            {
                WavePlayer->SetSoundWave(Wave);
                bWaveSet = true;
            }
        }
    }

    // The node_id is the node's stable index within AllNodes (we only append).
    const int32 NodeId = Cue->AllNodes.IndexOfByKey(NewNode);

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Cue->GetPackage(), Cue, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("cue_path"), CuePath);
    Obj->SetNumberField(TEXT("node_id"), NodeId);
    Obj->SetStringField(TEXT("node_type"), NodeType);
    Obj->SetStringField(TEXT("node_class"), NewNode->GetClass()->GetName());
    Obj->SetNumberField(TEXT("max_child_nodes"), NewNode->GetMaxChildNodes());
    Obj->SetNumberField(TEXT("child_count"), NewNode->ChildNodes.Num());
    Obj->SetBoolField(TEXT("sound_wave_set"), bWaveSet);
    if (!SoundWavePath.IsEmpty())
    {
        Obj->SetStringField(TEXT("sound_wave_path"), SoundWavePath);
    }
    Obj->SetNumberField(TEXT("node_count"), Cue->AllNodes.Num());
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("add_sound_cue_node"), SaveError);
}

// ---------------------------------------------------------------------------
// connect_sound_cue_nodes
// ---------------------------------------------------------------------------
// MCP:connect_sound_cue_nodes - Link the node child_node_id into an input slot of the node parent_node_id in a USoundCue asset (ids from add_sound_cue_node or get_sound_cue_graph), in one undo step, then save the cue. With input_index that slot is overwritten even when it holds another child (missing slots up to it are added empty); without it the first empty slot is used, or a new one is appended. A cue that does not load, an id out of range, a parent that takes no children (a WavePlayer), an input_index outside [0, max_child_nodes), a full parent, or a cycle (a node into itself or into a node feeding it) is an error, with nothing changed. Duplicate links are allowed: a repeat without input_index adds the child again. A failed save, as during PIE, makes success false with save_error. Inverse: disconnect_sound_cue_nodes; cue output: set_sound_cue_first_node; read back: get_sound_cue_graph.
//   @retired connect_pins - asset_path=<sound cue>, source_node_id=<child index>, source_pin="", target_node_id=<parent index>, target_pin=<input slot index, "" for the first free one>
//   @param cue_path - Full asset path of the cue
//   @param parent_node_id - node_id of the upstream node with inputs (its index in the cue AllNodes)
//   @param child_node_id - node_id of the node to feed into the parent
//   @param input_index - Optional zero-based target slot; an occupied slot is overwritten. Omitted: the first empty slot, else a new slot appended
//   @returns { cue_path: string, parent_node_id: int, child_node_id: int, input_index: int, parent_class: string, child_class: string, parent_child_count: int, connected: bool, saved: bool }
TSharedPtr<FJsonObject> HandleConnectSoundCueNodes(const TSharedPtr<FJsonObject>& Params)
{
    FString CuePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("cue_path"), CuePath)) { return Missing; }

    double ParentIdD = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("parent_node_id"), ParentIdD)) { return Missing; }
    double ChildIdD = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("child_node_id"), ChildIdD)) { return Missing; }
    const int32 ParentId = static_cast<int32>(ParentIdD);
    const int32 ChildId = static_cast<int32>(ChildIdD);

    USoundCue* Cue = nullptr;
    if (TSharedPtr<FJsonObject> LoadError = SoundCue_Load(CuePath, Cue)) { return LoadError; }

    if (!Cue->AllNodes.IsValidIndex(ParentId))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("parent_node_id %d out of range (node_count=%d)"), ParentId, Cue->AllNodes.Num()));
    }
    if (!Cue->AllNodes.IsValidIndex(ChildId))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("child_node_id %d out of range (node_count=%d)"), ChildId, Cue->AllNodes.Num()));
    }
    USoundNode* ParentNode = Cue->AllNodes[ParentId];
    USoundNode* ChildNode = Cue->AllNodes[ChildId];

    if (!ParentNode || !ChildNode)
    {
        return mcp_common::MakeErrorReply(TEXT("Resolved parent or child node was null"));
    }

    const int32 MaxChildNodes = ParentNode->GetMaxChildNodes();
    if (MaxChildNodes <= 0)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Parent node %s accepts no children"), *ParentNode->GetClass()->GetName()));
    }

    // A node fed into itself, or into a node that already feeds it (the parent among the child's inputs, directly
    // or through other nodes), closes a loop that USoundCue::CacheAggregateValues and every other recursive walk
    // of the cue (GetDuration, playback) never leaves, so either is refused before anything changes.
    if (ParentNode == ChildNode)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("parent_node_id and child_node_id are both %d: a node cannot feed itself. Nothing was changed."), ParentId));
    }
    if (SoundCue_FeedsInto(ParentNode, ChildNode))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Node %d already feeds node %d through its inputs, so linking %d into %d would make a cycle. ")
            TEXT("Nothing was changed."), ParentId, ChildId, ChildId, ParentId));
    }

    // Resolve target input slot. Default to first free slot (append) when not given.
    int32 InputIndex = -1;
    double InputIdxD = 0.0;
    if (Params->TryGetNumberField(TEXT("input_index"), InputIdxD))
    {
        InputIndex = static_cast<int32>(InputIdxD);
        if (InputIndex < 0 || InputIndex >= MaxChildNodes)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("input_index %d out of range [0, %d) for %s"), InputIndex, MaxChildNodes, *ParentNode->GetClass()->GetName()));
        }
    }

    // Without input_index: the first empty slot, else a new one appended; a full parent is refused here,
    // before anything changes.
    int32 FreeSlot = INDEX_NONE;
    if (InputIndex < 0)
    {
        for (int32 i = 0; i < ParentNode->ChildNodes.Num(); ++i)
        {
            if (ParentNode->ChildNodes[i] == nullptr)
            {
                FreeSlot = i;
                break;
            }
        }
        if (FreeSlot == INDEX_NONE && ParentNode->ChildNodes.Num() >= MaxChildNodes)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Parent node %s already has the maximum %d children"), *ParentNode->GetClass()->GetName(), MaxChildNodes));
        }
    }

    const FScopedTransaction Transaction(LOCTEXT("ConnectSoundCueNodes", "Connect Sound Cue Nodes"));
    Cue->Modify();
    ParentNode->Modify();

    if (InputIndex < 0)
    {
        // Append into the next free slot (reuse a null entry if one exists).
        if (FreeSlot == INDEX_NONE)
        {
            FreeSlot = ParentNode->ChildNodes.Add(ChildNode);
        }
        else
        {
            ParentNode->ChildNodes[FreeSlot] = ChildNode;
        }
        InputIndex = FreeSlot;
    }
    else
    {
        // Ensure the array is large enough to address InputIndex.
        while (ParentNode->ChildNodes.Num() <= InputIndex)
        {
            ParentNode->ChildNodes.Add(nullptr);
        }
        ParentNode->ChildNodes[InputIndex] = ChildNode;
    }

    Cue->CacheAggregateValues();

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Cue->GetPackage(), Cue, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("cue_path"), CuePath);
    Obj->SetNumberField(TEXT("parent_node_id"), ParentId);
    Obj->SetNumberField(TEXT("child_node_id"), ChildId);
    Obj->SetNumberField(TEXT("input_index"), InputIndex);
    Obj->SetStringField(TEXT("parent_class"), ParentNode->GetClass()->GetName());
    Obj->SetStringField(TEXT("child_class"), ChildNode->GetClass()->GetName());
    Obj->SetNumberField(TEXT("parent_child_count"), ParentNode->ChildNodes.Num());
    Obj->SetBoolField(TEXT("connected"), true);
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("connect_sound_cue_nodes"), SaveError);
}

// ---------------------------------------------------------------------------
// disconnect_sound_cue_nodes
// ---------------------------------------------------------------------------
// MCP:disconnect_sound_cue_nodes - Unwires one input of a node in a USoundCue asset: the parent's input slot, picked by the child wired into it or by its index, is emptied, in one undo step, then the cue is saved; a failed save, as during PIE, makes success false with save_error. Inverse of connect_sound_cue_nodes. When the cue has an editor graph (it was made or edited in the Sound Cue editor), the wire into that input pin is broken too (graph_links_broken counts it), so the editor does not rebuild the slot from its graph. The slot itself is kept (empty, -1 in get_sound_cue_graph) so other input indexes do not shift, and the child node stays in the cue: remove_sound_cue_node deletes a node. A slot that is already empty or out of range, or a child not wired into the parent, succeeds with already_disconnected true and nothing changed or saved. A cue that does not load, a node id out of range, or neither child_node_id nor input_index is an error. Read ids and wiring first with get_sound_cue_graph; the cue's output node is set with set_sound_cue_first_node.
//   @retired disconnect_pin - asset_path=<sound cue>, node_id=<parent index>, pin_name=<input slot index>; or pin_name="" with linked_node_id=<child index>
//   @param cue_path - Object or package path of the Sound Cue asset (e.g. "/Game/Audio/SC_Footsteps").
//   @param parent_node_id - node_id (index in the cue's AllNodes, as get_sound_cue_graph lists it) of the node whose input slot is emptied.
//   @param child_node_id - Optional node_id of the wired child; the first slot of the parent holding it is emptied. Wins over input_index when both are given.
//   @param input_index - Optional 0-based input slot of the parent to empty directly (its position in that node's child_ids); supply this OR child_node_id.
//   @returns { cue_path: string, parent_node_id: int, child_node_id: int, input_index: int, parent_class: string, child_class: string, parent_child_count: int, graph_links_broken: int, disconnected: bool, already_disconnected: bool, saved: bool, save_error?: string }
TSharedPtr<FJsonObject> HandleDisconnectSoundCueNodes(const TSharedPtr<FJsonObject>& Params)
{
    FString CuePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("cue_path"), CuePath)) { return Missing; }

    double ParentIdD = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("parent_node_id"), ParentIdD)) { return Missing; }
    const int32 ParentId = static_cast<int32>(ParentIdD);

    // The slot is located either by the wired child (child_node_id) or directly (input_index).
    double ChildIdD = 0.0;
    const bool bHasChildId = Params->TryGetNumberField(TEXT("child_node_id"), ChildIdD);
    double InputIdxD = 0.0;
    const bool bHasInputIndex = Params->TryGetNumberField(TEXT("input_index"), InputIdxD);
    if (!bHasChildId && !bHasInputIndex)
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: supply either child_node_id or input_index"));
    }

    USoundCue* Cue = nullptr;
    if (TSharedPtr<FJsonObject> LoadError = SoundCue_Load(CuePath, Cue)) { return LoadError; }

    USoundNode* ParentNode = nullptr;
    if (TSharedPtr<FJsonObject> NodeError = SoundCue_FindNode(Cue, ParentId, TEXT("parent_node_id"), TEXT("parent node"), ParentNode)) { return NodeError; }

    // Resolve the target slot. child_node_id wins when both are supplied (the wire is the intent).
    int32 ChildId = INDEX_NONE;
    int32 Idx = INDEX_NONE;
    if (bHasChildId)
    {
        ChildId = static_cast<int32>(ChildIdD);
        USoundNode* ChildNode = nullptr;
        if (TSharedPtr<FJsonObject> NodeError = SoundCue_FindNode(Cue, ChildId, TEXT("child_node_id"), TEXT("child node"), ChildNode)) { return NodeError; }
        Idx = ParentNode->ChildNodes.IndexOfByKey(ChildNode);
    }
    else
    {
        Idx = static_cast<int32>(InputIdxD);
    }

    // Idempotent guard: nothing wired at that slot -> report already-disconnected, no Modify/save.
    if (Idx == INDEX_NONE || !ParentNode->ChildNodes.IsValidIndex(Idx) || ParentNode->ChildNodes[Idx] == nullptr)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("cue_path"), CuePath);
        Obj->SetNumberField(TEXT("parent_node_id"), ParentId);
        Obj->SetNumberField(TEXT("child_node_id"), ChildId);
        Obj->SetNumberField(TEXT("input_index"), Idx);
        Obj->SetStringField(TEXT("parent_class"), ParentNode->GetClass()->GetName());
        Obj->SetStringField(TEXT("child_class"), TEXT("None"));
        Obj->SetNumberField(TEXT("parent_child_count"), ParentNode->ChildNodes.Num());
        Obj->SetBoolField(TEXT("disconnected"), false);
        Obj->SetBoolField(TEXT("already_disconnected"), true);
        Obj->SetBoolField(TEXT("saved"), false);
        return mcp_common::MakeOkReply(Obj);
    }

    // Capture the child class before we null the slot (null the pin, never
    // RemoveAt/RemoveChildNode, which compact the array and would corrupt input_index).
    USoundNode* ChildNode = ParentNode->ChildNodes[Idx];
    const FString ChildClass = ChildNode->GetClass()->GetName();
    if (ChildId == INDEX_NONE)
    {
        ChildId = Cue->AllNodes.IndexOfByKey(ChildNode);
    }

    const FScopedTransaction Transaction(LOCTEXT("DisconnectSoundCueNodes", "Disconnect Sound Cue Nodes"));
    Cue->Modify();
    ParentNode->Modify();

    ParentNode->ChildNodes[Idx] = nullptr;

    // A cue with an editor graph wires the same slot there; it is unwired too, or the Sound Cue editor would rebuild
    // the slot from the graph and bring the child back.
    int32 GraphLinksBroken = 0;
#if WITH_EDITORONLY_DATA
    GraphLinksBroken = SoundCue_BreakGraphInputLinks(ParentNode, Idx);
#endif

    Cue->CacheAggregateValues();

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Cue->GetPackage(), Cue, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("cue_path"), CuePath);
    Obj->SetNumberField(TEXT("parent_node_id"), ParentId);
    Obj->SetNumberField(TEXT("child_node_id"), ChildId);
    Obj->SetNumberField(TEXT("input_index"), Idx);
    Obj->SetStringField(TEXT("parent_class"), ParentNode->GetClass()->GetName());
    Obj->SetStringField(TEXT("child_class"), ChildClass);
    Obj->SetNumberField(TEXT("parent_child_count"), ParentNode->ChildNodes.Num());
    Obj->SetNumberField(TEXT("graph_links_broken"), GraphLinksBroken);
    // Read back from the slot.
    const bool bDisconnected = ParentNode->ChildNodes.IsValidIndex(Idx) && ParentNode->ChildNodes[Idx] == nullptr;
    Obj->SetBoolField(TEXT("disconnected"), bDisconnected);
    Obj->SetBoolField(TEXT("already_disconnected"), false);
    if (!bDisconnected)
    {
        Obj->SetBoolField(TEXT("saved"), bSaved);
        return mcp_common::MakeOutcomeReply(Obj, false,
            mcp_common::MeasuredOutcomeError(TEXT("disconnect_sound_cue_nodes"), TEXT("disconnected")));
    }
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("disconnect_sound_cue_nodes"), SaveError);
}

// ---------------------------------------------------------------------------
// remove_sound_cue_node: the inverse of add_sound_cue_node. node_id is an index into AllNodes, so every node after
// the removed one moves down by one; the reply carries that remap.
// ---------------------------------------------------------------------------
// MCP:remove_sound_cue_node - Delete one node from a USoundCue asset, in one undo step, then save the cue. Inverse of add_sound_cue_node: every input slot that held the node is emptied (slots are kept, as disconnect_sound_cue_nodes keeps them), the cue output (FirstNode) is cleared when it was this node, the node leaves AllNodes, and its node in the cue editor's graph goes too when the cue has one. node_id is an index into AllNodes, so the nodes after it move down by one: id_remap lists {old_id, new_id} for each, and the ids from earlier calls are stale. A cue that does not load or a node_id out of range is an error and nothing changes. A failed save, as during PIE, makes success false with save_error. Read back: get_sound_cue_graph.
//   @retired remove_graph_node - asset_path=<sound cue>, node_id=<node index>; the later ids shift down (read id_remap)
//   @param cue_path - Full asset path of the cue
//   @param node_id - node_id of the node to delete (its index in the cue AllNodes, from add_sound_cue_node or get_sound_cue_graph)
//   @returns { cue_path: string, node_id: int, node_class: string, removed: bool, slots_cleared: int, first_node_cleared: bool, graph_node_removed: bool, id_remap: [{old_id, new_id}], node_count: int, saved: bool, save_error?: string }
TSharedPtr<FJsonObject> HandleRemoveSoundCueNode(const TSharedPtr<FJsonObject>& Params)
{
    FString CuePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("cue_path"), CuePath)) { return Missing; }
    double NodeIdD = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("node_id"), NodeIdD)) { return Missing; }
    const int32 NodeId = static_cast<int32>(NodeIdD);

    USoundCue* Cue = nullptr;
    if (TSharedPtr<FJsonObject> LoadError = SoundCue_Load(CuePath, Cue)) { return LoadError; }
    USoundNode* Doomed = nullptr;
    if (TSharedPtr<FJsonObject> NodeError = SoundCue_FindNode(Cue, NodeId, TEXT("node_id"), TEXT("node"), Doomed)) { return NodeError; }
    const FString NodeClass = Doomed->GetClass()->GetName();

    const FScopedTransaction Transaction(LOCTEXT("RemoveSoundCueNode", "Remove Sound Cue Node"));
    Cue->Modify();

    // Empty every slot that held it (set, never RemoveAt: an input_index stays the slot it was).
    int32 SlotsCleared = 0;
    for (USoundNode* Node : Cue->AllNodes)
    {
        if (!Node || Node == Doomed)
        {
            continue;
        }
        for (int32 Slot = 0; Slot < Node->ChildNodes.Num(); ++Slot)
        {
            if (Node->ChildNodes[Slot] == Doomed)
            {
                Node->Modify();
                Node->ChildNodes[Slot] = nullptr;
                ++SlotsCleared;
            }
        }
    }
    const bool bFirstNodeCleared = Cue->FirstNode == Doomed;
    if (bFirstNodeCleared)
    {
        Cue->FirstNode = nullptr;
    }

    // A cue made in the Sound Cue editor also holds an editor graph node per sound node; it goes with the node, or
    // the editor would rebuild AllNodes from the graph with the node back in it.
    bool bGraphNodeRemoved = false;
#if WITH_EDITORONLY_DATA
    if (Cue->SoundCueGraph && Doomed->GraphNode)
    {
        Cue->SoundCueGraph->Modify();
        bGraphNodeRemoved = Cue->SoundCueGraph->RemoveNode(Doomed->GraphNode);
    }
#endif

    Cue->AllNodes.RemoveAt(NodeId);
    Cue->CacheAggregateValues();

    // The re-read: nothing in the cue still points at the node.
    bool bStillReferenced = Cue->AllNodes.Contains(Doomed) || Cue->FirstNode == Doomed;
    for (const USoundNode* Node : Cue->AllNodes)
    {
        bStillReferenced |= Node && Node->ChildNodes.Contains(Doomed);
    }
    if (bStillReferenced)
    {
        return mcp_common::MakeErrorReply(TEXT("The node is still referenced by the cue after the removal; nothing was saved"));
    }

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Cue->GetPackage(), Cue, SaveError);

    TArray<TSharedPtr<FJsonValue>> Remap;
    for (int32 NewId = NodeId; NewId < Cue->AllNodes.Num(); ++NewId)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetNumberField(TEXT("old_id"), NewId + 1);
        Entry->SetNumberField(TEXT("new_id"), NewId);
        Remap.Add(MakeShared<FJsonValueObject>(Entry));
    }
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("cue_path"), CuePath);
    Obj->SetNumberField(TEXT("node_id"), NodeId);
    Obj->SetStringField(TEXT("node_class"), NodeClass);
    Obj->SetBoolField(TEXT("removed"), true);
    Obj->SetNumberField(TEXT("slots_cleared"), SlotsCleared);
    Obj->SetBoolField(TEXT("first_node_cleared"), bFirstNodeCleared);
    Obj->SetBoolField(TEXT("graph_node_removed"), bGraphNodeRemoved);
    Obj->SetArrayField(TEXT("id_remap"), Remap);
    Obj->SetNumberField(TEXT("node_count"), Cue->AllNodes.Num());
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("remove_sound_cue_node"), SaveError);
}

// ---------------------------------------------------------------------------
// set_sound_cue_first_node
// ---------------------------------------------------------------------------
// MCP:set_sound_cue_first_node - Make the node at node_id the root (FirstNode, the output) of a USoundCue graph, undoably, and save the asset. Returns first_node_class, first_node_set and saved; an out-of-range node_id is an error. Get node ids from add_sound_cue_node or get_sound_cue_graph.
//   @retired connect_pins - asset_path=<sound cue>, source_node_id=<node index>, source_pin="", target_node_id="@output", target_pin=""
//   @param cue_path - Full asset path of the cue
//   @param node_id - node_id (index in the cue AllNodes array) to use as the root of the cue graph
//   @returns { cue_path: string, node_id: int, first_node_class: string, first_node_set: bool, saved: bool }
TSharedPtr<FJsonObject> HandleSetSoundCueFirstNode(const TSharedPtr<FJsonObject>& Params)
{
    FString CuePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("cue_path"), CuePath)) { return Missing; }
    double NodeIdD = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("node_id"), NodeIdD)) { return Missing; }
    const int32 NodeId = static_cast<int32>(NodeIdD);

    USoundCue* Cue = nullptr;
    if (TSharedPtr<FJsonObject> LoadError = SoundCue_Load(CuePath, Cue)) { return LoadError; }

    USoundNode* Node = nullptr;
    if (TSharedPtr<FJsonObject> NodeError = SoundCue_FindNode(Cue, NodeId, TEXT("node_id"), TEXT("node"), Node)) { return NodeError; }

    const FScopedTransaction Transaction(LOCTEXT("SetSoundCueFirstNode", "Set Sound Cue First Node"));
    Cue->Modify();
    Cue->FirstNode = Node;
    Cue->CacheAggregateValues();

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Cue->GetPackage(), Cue, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("cue_path"), CuePath);
    Obj->SetNumberField(TEXT("node_id"), NodeId);
    Obj->SetStringField(TEXT("first_node_class"), Node->GetClass()->GetName());
    Obj->SetBoolField(TEXT("first_node_set"), Cue->FirstNode == Node);
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("set_sound_cue_first_node"), SaveError);
}

// ---------------------------------------------------------------------------
// set_sound_cue_node_properties
// ---------------------------------------------------------------------------
// MCP:set_sound_cue_node_properties - Set EditAnywhere properties of one Sound Cue node, located by node_id, then refresh the cue's aggregate values and save it -- the writer twin of get_sound_cue_graph. Use it instead of set_properties, which needs the node's object path rather than the node_id that get_sound_cue_graph and add_sound_cue_node report. Partial update (idempotent): only keys present in properties are written, only CPF_Edit props are touched, and applied_keys lists the editable fields matched (ChildNodes/graph wiring are unreachable here -- use connect_sound_cue_nodes). Keys are the camelCase UPROPERTY names get_sound_cue_graph reports under each node's properties (e.g. a Modulator: pitchMin/pitchMax/volumeMin/volumeMax; a Delay: delayMin/delayMax).
//   @retired set_properties - object_path=<the node's object_path from get_graph_nodes>, properties={"PitchMin":0.9,"PitchMax":1.1} (the UPROPERTY names), save=true; the cue recaches its values
//   @param cue_path - Full asset path of the cue, e.g. /Game/SoundCues/SC_Foo
//   @param node_id - node_id (index in AllNodes) from add_sound_cue_node / get_sound_cue_graph.
//   @param properties - Object of {camelCaseName: value} to apply to the node.
//   @returns { cue_path: string, node_id: int, node_class: string, applied_keys: [string], saved: bool }
TSharedPtr<FJsonObject> HandleSetSoundCueNodeProperties(const TSharedPtr<FJsonObject>& Params)
{
    FString CuePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("cue_path"), CuePath)) { return Missing; }
    double NodeIdD = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("node_id"), NodeIdD)) { return Missing; }
    const int32 NodeId = static_cast<int32>(NodeIdD);

    const TSharedPtr<FJsonObject>* PropsObj = nullptr;
    if (!Params->TryGetObjectField(TEXT("properties"), PropsObj) || !*PropsObj)
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: properties (object)"));
    }

    USoundCue* Cue = nullptr;
    if (TSharedPtr<FJsonObject> LoadError = SoundCue_Load(CuePath, Cue)) { return LoadError; }

    USoundNode* Node = nullptr;
    if (TSharedPtr<FJsonObject> NodeError = SoundCue_FindNode(Cue, NodeId, TEXT("node_id"), TEXT("node"), Node)) { return NodeError; }

    const FScopedTransaction Transaction(LOCTEXT("SetSoundCueNodeProperties", "Set Sound Cue Node Properties"));
    Cue->Modify();
    Node->Modify();

    // Merge only the editable props present in the JSON (partial -> idempotent; CPF_Edit
    // restricts to EditAnywhere fields, so ChildNodes/GraphNode can't be clobbered here).
    FString JsonErr;
    const bool bOk = mcp_common::JsonToStruct(
        *PropsObj, Node->GetClass(), Node, JsonErr, CPF_Edit, 0);
    if (!bOk)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Failed to apply properties to node %d (%s): %s"), NodeId, *Node->GetClass()->GetName(), *JsonErr));
    }

    Cue->CacheAggregateValues();

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Cue->GetPackage(), Cue, SaveError);

    // applied_keys = the editable props the request actually addressed (honest, not just echoed).
    TArray<TSharedPtr<FJsonValue>> AppliedKeys;
    for (TFieldIterator<FProperty> It(Node->GetClass()); It; ++It)
    {
        if (It->HasAnyPropertyFlags(CPF_Edit) && (*PropsObj)->HasField(It->GetAuthoredName()))
        {
            AppliedKeys.Add(MakeShared<FJsonValueString>(It->GetAuthoredName()));
        }
    }

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("cue_path"), CuePath);
    Obj->SetNumberField(TEXT("node_id"), NodeId);
    Obj->SetStringField(TEXT("node_class"), Node->GetClass()->GetName());
    Obj->SetArrayField(TEXT("applied_keys"), AppliedKeys);
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("set_sound_cue_node_properties"), SaveError);
}

// ---------------------------------------------------------------------------
// get_sound_cue_graph
// ---------------------------------------------------------------------------
// MCP:get_sound_cue_graph - Reads the node graph of a USoundCue without changing it: every node in AllNodes with its id (the node_id the other sound cue tools take), class, child_ids (the wired child's id per input slot, -1 for an empty slot), max_child_nodes and editable properties (the keys set_sound_cue_node_properties accepts), plus first_node_id (the output node, -1 when unset) and node_count. Read-only: nothing is modified or saved; a cue that does not load is an error. Call it before connect_sound_cue_nodes, disconnect_sound_cue_nodes, remove_sound_cue_node, set_sound_cue_first_node or set_sound_cue_node_properties to get ids, and after them to check the result; ids are positions, so re-read after remove_sound_cue_node shifts them. Create a cue with create_sound_cue and add nodes with add_sound_cue_node.
//   @retired get_graph_nodes - asset_path=<sound cue>; the reply is the same, and each node also has node_id (its index) and object_path
//   @param cue_path - Object or package path of the Sound Cue asset (e.g. "/Game/Audio/SC_Footsteps" or "/Game/Audio/SC_Footsteps.SC_Footsteps").
//   @returns { cue_path: string, asset_path: string, nodes: [{ id, object_path, class, child_count, max_child_nodes, child_ids, properties }], node_count: int, first_node_id: int, has_first_node: bool }
TSharedPtr<FJsonObject> HandleGetSoundCueGraph(const TSharedPtr<FJsonObject>& Params)
{
    FString CuePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("cue_path"), CuePath)) { return Missing; }

    USoundCue* Cue = nullptr;
    if (TSharedPtr<FJsonObject> LoadError = SoundCue_Load(CuePath, Cue)) { return LoadError; }

    TArray<TSharedPtr<FJsonValue>> NodesJson;

    // Build a node-pointer -> id lookup so we can report each node's children by id.
    for (int32 Index = 0; Index < Cue->AllNodes.Num(); ++Index)
    {
        USoundNode* Node = Cue->AllNodes[Index];
        TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
        NodeObj->SetNumberField(TEXT("id"), Index);
        if (Node)
        {
            NodeObj->SetStringField(TEXT("class"), Node->GetClass()->GetName());
            // The path set_properties and get_properties take to write or read the node's own settings.
            NodeObj->SetStringField(TEXT("object_path"), Node->GetPathName());
            NodeObj->SetNumberField(TEXT("child_count"), Node->ChildNodes.Num());
            NodeObj->SetNumberField(TEXT("max_child_nodes"), Node->GetMaxChildNodes());

            TArray<TSharedPtr<FJsonValue>> ChildIds;
            for (USoundNode* Child : Node->ChildNodes)
            {
                const int32 ChildId = Child ? Cue->AllNodes.IndexOfByKey(Child) : INDEX_NONE;
                ChildIds.Add(MakeShared<FJsonValueNumber>(ChildId));
            }
            NodeObj->SetArrayField(TEXT("child_ids"), ChildIds);

            // Per-node editable props (CPF_Edit only -> excludes ChildNodes/GraphNode);
            // the exact keys set_sound_cue_node_properties accepts (round-trip).
            TSharedPtr<FJsonObject> PropsObj = mcp_common::StructToJson(
                Node->GetClass(), Node, CPF_Edit, 0);
            NodeObj->SetObjectField(TEXT("properties"), PropsObj);
        }
        else
        {
            NodeObj->SetStringField(TEXT("class"), TEXT("None"));
            NodeObj->SetNumberField(TEXT("child_count"), 0);
        }
        NodesJson.Add(MakeShared<FJsonValueObject>(NodeObj));
    }
    const int32 NodeCount = Cue->AllNodes.Num();
    const int32 FirstNodeId = Cue->FirstNode ? Cue->AllNodes.IndexOfByKey(Cue->FirstNode) : INDEX_NONE;

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("cue_path"), CuePath);
    Obj->SetStringField(TEXT("asset_path"), Cue->GetPathName());
    Obj->SetArrayField(TEXT("nodes"), NodesJson);
    Obj->SetNumberField(TEXT("node_count"), NodeCount);
    Obj->SetNumberField(TEXT("first_node_id"), FirstNodeId);
    Obj->SetBoolField(TEXT("has_first_node"), Cue->FirstNode != nullptr);
    return mcp_common::MakeOkReply(Obj);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_sound_cue_node"), &HandleAddSoundCueNode },
    { TEXT("connect_sound_cue_nodes"), &HandleConnectSoundCueNodes },
    { TEXT("create_sound_cue"), &HandleCreateSoundCue },
    { TEXT("disconnect_sound_cue_nodes"), &HandleDisconnectSoundCueNodes },
    { TEXT("get_sound_cue_graph"), &HandleGetSoundCueGraph },
    { TEXT("remove_sound_cue_node"), &HandleRemoveSoundCueNode },
    { TEXT("set_sound_cue_first_node"), &HandleSetSoundCueFirstNode },
    { TEXT("set_sound_cue_node_properties"), &HandleSetSoundCueNodeProperties },
};
} // namespace UEAgentSoundCueCommands

#undef LOCTEXT_NAMESPACE
