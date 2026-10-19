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
// MCP:create_sound_cue - Create an empty USoundCue (no nodes, no first node) at package_path/name and save it; an existing cue there is returned unchanged (already_existed:true). Returns cue_path, asset_path, node_count and saved. Build the graph with add_sound_cue_node, connect_sound_cue_nodes and set_sound_cue_first_node.
//   @param name - Asset name, e.g. SC_Footsteps
//   @param package_path - Target content path, defaults to /Game/SoundCues
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
// MCP:add_sound_cue_node - Append a new node of node_type to the AllNodes of a USoundCue asset, in one undo step, and save the cue; node_id is its index there, the id the other sound cue tools take. node_type is matched without case, and an unknown type or a cue that does not load is an error. The node starts unconnected and is not the cue output: wire it with connect_sound_cue_nodes and make the root with set_sound_cue_first_node. For a WavePlayer, sound_wave_path sets its wave; a wave that does not load is not an error (sound_wave_set=false), and other types ignore it. Each call adds another node (not idempotent). A failed save, as during PIE, makes success false with save_error. Node settings: set_sound_cue_node_properties; read back: get_sound_cue_graph; a new cue: create_sound_cue.
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
// MCP:connect_sound_cue_nodes - Link the node child_node_id into an input slot of the node parent_node_id in a USoundCue asset (both ids from add_sound_cue_node or get_sound_cue_graph), in one undo step, then save the cue. With input_index that slot is overwritten even when it holds another child (missing slots up to it are added empty); without it the first empty slot is used, or a new one is appended. A cue that does not load, an id out of range, a parent that takes no children (a WavePlayer), an input_index outside [0, max_child_nodes) or a full parent is an error. Nothing checks for cycles or earlier links: repeating the call without input_index links the child again in another slot. A failed save, as during PIE, makes success false with save_error. Inverse: disconnect_sound_cue_nodes; the cue output: set_sound_cue_first_node; read back: get_sound_cue_graph.
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

    const FScopedTransaction Transaction(LOCTEXT("ConnectSoundCueNodes", "Connect Sound Cue Nodes"));
    Cue->Modify();
    ParentNode->Modify();

    if (InputIndex < 0)
    {
        // Append into the next free slot (reuse a null entry if one exists).
        int32 FreeSlot = INDEX_NONE;
        for (int32 i = 0; i < ParentNode->ChildNodes.Num(); ++i)
        {
            if (ParentNode->ChildNodes[i] == nullptr)
            {
                FreeSlot = i;
                break;
            }
        }
        if (FreeSlot == INDEX_NONE)
        {
            if (ParentNode->ChildNodes.Num() >= MaxChildNodes)
            {
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("Parent node %s already has the maximum %d children"), *ParentNode->GetClass()->GetName(), MaxChildNodes));
            }
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
// MCP:disconnect_sound_cue_nodes - Clear a wired child out of a parent node's input slot (inverse of connect_sound_cue_nodes)
//   @param cue_path - Full asset path of the cue
//   @param parent_node_id - node_id of the upstream node whose input slot is cleared
//   @param child_node_id - Optional node_id of the wired child; its slot is located via IndexOfByKey
//   @param input_index - Optional slot to clear directly; supply this OR child_node_id
//   @returns { cue_path: string, parent_node_id: int, child_node_id: int, input_index: int, parent_class: string, child_class: string, parent_child_count: int, disconnected: bool, already_disconnected: bool, saved: bool }
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
    Obj->SetBoolField(TEXT("disconnected"), true);
    Obj->SetBoolField(TEXT("already_disconnected"), false);
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("disconnect_sound_cue_nodes"), SaveError);
}

// ---------------------------------------------------------------------------
// set_sound_cue_first_node
// ---------------------------------------------------------------------------
// MCP:set_sound_cue_first_node - Make the node at node_id the root (FirstNode, the output) of a USoundCue graph, undoably, and save the asset. Returns first_node_class, first_node_set and saved; an out-of-range node_id is an error. Get node ids from add_sound_cue_node or get_sound_cue_graph.
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
// MCP:set_sound_cue_node_properties - Set the EditAnywhere properties of a Sound Cue node (located by node_id) via reflection.  The writer twin of get_sound_cue_graph, which reads them. Partial update (idempotent): only keys present in ``properties`` are written, and only editable (CPF_Edit) props are touched (ChildNodes/graph wiring are unreachable here — use connect_sound_cue_nodes). Keys are the camelCase UPROPERTY names, the same ones get_sound_cue_graph reports under each node's ``properties`` (e.g. a Modulator: pitchMin/pitchMax/volumeMin/volumeMax; a Delay: delayMin/delayMax).
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
// MCP:get_sound_cue_graph - Read the node graph of a USoundCue without changing it: every node in AllNodes with its id (the node_id other sound cue tools take), class, child_ids (-1 for an empty slot), max_child_nodes and editable properties, plus first_node_id (-1 when unset) and node_count.
//   @param cue_path - Full asset path of the cue
//   @returns { cue_path: string, asset_path: string, nodes: [object], node_count: int, first_node_id: int, has_first_node: bool }
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
    { TEXT("set_sound_cue_first_node"), &HandleSetSoundCueFirstNode },
    { TEXT("set_sound_cue_node_properties"), &HandleSetSoundCueNodeProperties },
};
} // namespace UEAgentSoundCueCommands

#undef LOCTEXT_NAMESPACE
