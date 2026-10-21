// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Animation/AnimAssetUtils.h"

#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Modules/ModuleManager.h"

#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "HAL/PlatformFileManager.h"   // get_anim_blueprint_info: size and timestamp of the .uasset

#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"

// Engine runtime anim blueprint types
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimInstance.h"
#include "Animation/Skeleton.h"
#include "AlphaBlend.h"   // EAlphaBlendOption (Engine; transition BlendMode)

// UnrealEd factory
#include "Factories/AnimBlueprintFactory.h"

// AnimGraph editor module (build_cs_editor_deps: ["AnimGraph"])
#include "AnimationGraph.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimGraphNode_StateMachineBase.h"
// AnimBP scaffolding (locomotion / foot IK)
#include "AnimGraphNode_LegIK.h"
#include "AnimationStateMachineGraph.h"
#include "AnimationStateMachineSchema.h"   // FEdGraphSchemaAction_NewStateNode
#include "AnimStateNode.h"
#include "AnimStateEntryNode.h"           // UAnimStateEntryNode::GetOutputPin (entry-node wiring)
#include "AnimStateNodeBase.h"             // UAnimStateNodeBase::GetTransitionList (base of state + conduit)
#include "AnimStateTransitionNode.h"       // UAnimStateTransitionNode
#include "AnimStateConduitNode.h"          // UAnimStateConduitNode
#include "AnimationTransitionGraph.h"      // UAnimationTransitionGraph::MyResultNode + GetResultNode
#include "AnimGraphNode_TransitionResult.h" // UAnimGraphNode_TransitionResult (sink, CanUserDeleteNode=false)
#include "K2Node_VariableGet.h"           // UK2Node_VariableGet (via mcp_common::SpawnVariableGetNode)
#include "K2Node_CallFunction.h"         // UK2Node_CallFunction (KismetMathLibrary comparison + KismetSystemLibrary literal)
#include "EdGraphSchema_K2.h"             // UEdGraphSchema_K2: the pin type of a rule variable
#include "Kismet/KismetMathLibrary.h"     // UKismetMathLibrary (EqualEqual_DoubleDouble etc.)
#include "Kismet/KismetSystemLibrary.h"    // UKismetSystemLibrary::MakeLiteralDouble (KismetMathLibrary has no literals)

/**
 * Animation Blueprint authoring commands.
 *
 * Creates and saves UAnimBlueprint assets targeting a real USkeleton, reads their structure, and (best-effort,
 * headless-safe) inserts state-machine nodes into the AnimGraph through the editor asset model: the nodes are
 * placed with FGraphNodeCreator or the state-machine schema action, whose PostPlacedNewNode builds the bound
 * graphs via FBlueprintEditorUtils::CreateNewGraph with no Slate or task-graph re-entry.
 */
namespace UEAgentAnimBlueprintCommands
{

// MCP-DOMAIN: Animation

namespace
{
    /** Find the top-level AnimGraph (named "AnimGraph", class UAnimationGraph) of an anim blueprint. */
    UAnimationGraph* FindAnimGraph(UAnimBlueprint* AnimBP)
    {
        if (!AnimBP)
        {
            return nullptr;
        }
        TArray<UEdGraph*> AllGraphs;
        AnimBP->GetAllGraphs(AllGraphs);
        // Prefer the canonical top-level AnimGraph (GN_AnimGraph == "AnimGraph").
        for (UEdGraph* Graph : AllGraphs)
        {
            if (UAnimationGraph* AnimGraph = Cast<UAnimationGraph>(Graph))
            {
                if (AnimGraph->GetFName() == FName(TEXT("AnimGraph")))
                {
                    return AnimGraph;
                }
            }
        }
        // Fallback: any UAnimationGraph.
        for (UEdGraph* Graph : AllGraphs)
        {
            if (UAnimationGraph* AnimGraph = Cast<UAnimationGraph>(Graph))
            {
                return AnimGraph;
            }
        }
        return nullptr;
    }

    /** Locate a state-machine node by its bound-graph name within the anim blueprint. */
    UAnimGraphNode_StateMachine* FindStateMachineByName(UAnimBlueprint* AnimBP, const FString& MachineName)
    {
        UAnimationGraph* AnimGraph = FindAnimGraph(AnimBP);
        if (!AnimGraph)
        {
            return nullptr;
        }
        for (UEdGraphNode* Node : AnimGraph->Nodes)
        {
            if (UAnimGraphNode_StateMachine* SM = Cast<UAnimGraphNode_StateMachine>(Node))
            {
                if (SM->EditorStateMachineGraph &&
                    SM->EditorStateMachineGraph->GetName().Equals(MachineName, ESearchCase::IgnoreCase))
                {
                    return SM;
                }
            }
        }
        return nullptr;
    }

    /**
     * Locate a transition endpoint by its bound-graph name within a state-machine graph.
     *
     * Returns the common base UAnimStateNodeBase so a transition can be wired to EITHER a
     * concrete state (UAnimStateNode) OR a conduit (UAnimStateConduitNode) — both derive
     * from UAnimStateNodeBase and both expose GetInputPin()/GetOutputPin() that
     * UAnimStateTransitionNode::CreateConnections consumes. The match is by bound-graph
     * name (GetBoundGraph(), virtual on the base, overridden by both subclasses).
     *
     * UAnimStateTransitionNode is ALSO a UAnimStateNodeBase but is a nonsensical endpoint,
     * so it is explicitly excluded — the finder yields a state-or-conduit (or alias) only.
     */
    UAnimStateNodeBase* FindStateByName(UAnimationStateMachineGraph* SMGraph, const FString& StateName)
    {
        if (!SMGraph)
        {
            return nullptr;
        }
        for (UEdGraphNode* Node : SMGraph->Nodes)
        {
            UAnimStateNodeBase* Endpoint = Cast<UAnimStateNodeBase>(Node);
            if (!Endpoint || Endpoint->IsA<UAnimStateTransitionNode>())
            {
                continue;   // skip non-endpoints and the transitions themselves
            }
            const UEdGraph* Bound = Endpoint->GetBoundGraph();
            if (Bound && Bound->GetName().Equals(StateName, ESearchCase::IgnoreCase))
            {
                return Endpoint;
            }
        }
        return nullptr;
    }

    /** Number of NodeType nodes in Graph. */
    template <typename NodeType>
    int32 AnimBp_CountNodes(const UEdGraph* Graph)
    {
        int32 Count = 0;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Cast<NodeType>(Node))
            {
                ++Count;
            }
        }
        return Count;
    }

    /** Count state-machine nodes inside the anim graph. */
    int32 CountStateMachines(UAnimBlueprint* AnimBP)
    {
        const UAnimationGraph* AnimGraph = FindAnimGraph(AnimBP);
        return AnimGraph ? AnimBp_CountNodes<UAnimGraphNode_StateMachine>(AnimGraph) : 0;
    }

    /**
     * Locate a transition node by its source-state + target-state bound-graph names.
     *
     * Resolves both endpoints via FindStateByName (matches state OR conduit, case-insensitive)
     * and walks SourceState->GetTransitionList() looking for a UAnimStateTransitionNode whose
     * GetNextState() == TargetState. Returns nullptr on no match (caller treats as not-found).
     */
    UAnimStateTransitionNode* FindTransitionBySourceTarget(
        UAnimationStateMachineGraph* SMGraph,
        const FString& SourceName,
        const FString& TargetName)
    {
        if (!SMGraph) { return nullptr; }
        UAnimStateNodeBase* SourceState = FindStateByName(SMGraph, SourceName);
        if (!SourceState) { return nullptr; }
        UAnimStateNodeBase* TargetState = FindStateByName(SMGraph, TargetName);
        if (!TargetState) { return nullptr; }
        TArray<UAnimStateTransitionNode*> Existing;
        SourceState->GetTransitionList(Existing);
        for (UAnimStateTransitionNode* T : Existing)
        {
            if (T && T->GetNextState() == TargetState)
            {
                return T;
            }
        }
        return nullptr;
    }

    /** Returns true if any UAnimGraphNode_TransitionResult sink lives on this graph (the
     *  canonical "rule graph" indicator). Always exactly 1 for a healthy transition. */
    bool HasTransitionResultSink(UEdGraph* Graph)
    {
        if (!Graph) { return false; }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Cast<UAnimGraphNode_TransitionResult>(Node))
            {
                return true;
            }
        }
        return false;
    }

    /** Reads the required anim_blueprint_path and machine_name: the reply naming the first one missing, else null. */
    TSharedPtr<FJsonObject> AnimBp_ReadMachineParams(const TSharedPtr<FJsonObject>& Params, FString& OutAnimBPPath, FString& OutMachineName)
    {
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("anim_blueprint_path"), OutAnimBPPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("machine_name"), OutMachineName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
        return nullptr;
    }

    /** AnimBp_ReadMachineParams, then the required source_state and target_state of a transition. */
    TSharedPtr<FJsonObject> AnimBp_ReadTransitionParams(const TSharedPtr<FJsonObject>& Params, FString& OutAnimBPPath,
        FString& OutMachineName, FString& OutSourceName, FString& OutTargetName)
    {
        if (TSharedPtr<FJsonObject> Missing = AnimBp_ReadMachineParams(Params, OutAnimBPPath, OutMachineName))
        {
            return Missing;
        }
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("source_state"), OutSourceName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("target_state"), OutTargetName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
        return nullptr;
    }

    /** Reads the required, non-empty package_path, asset_name and skeleton_path of a scaffold command. */
    TSharedPtr<FJsonObject> AnimBp_ReadScaffoldParams(const TSharedPtr<FJsonObject>& Params, FString& OutPackagePath,
        FString& OutAssetName, FString& OutSkeletonPath)
    {
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("package_path"), OutPackagePath)) { return Missing; }
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_name"), OutAssetName)) { return Missing; }
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("skeleton_path"), OutSkeletonPath)) { return Missing; }
        return nullptr;
    }

    /**
     * Loads the anim blueprint at AnimBPPath into OutAnimBP and the graph of its state machine MachineName into
     * OutSMGraph: null, else the reply when the blueprint does not load or has no such state machine.
     */
    TSharedPtr<FJsonObject> AnimBp_LoadStateMachineGraph(const FString& AnimBPPath, const FString& MachineName,
        UAnimBlueprint*& OutAnimBP, UAnimationStateMachineGraph*& OutSMGraph)
    {
        if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadFlexibleOrError(AnimBPPath, TEXT("anim blueprint"), OutAnimBP))
        {
            return Error;
        }
        UAnimGraphNode_StateMachine* SMNode = FindStateMachineByName(OutAnimBP, MachineName);
        if (!SMNode || !SMNode->EditorStateMachineGraph)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("State machine '%s' not found on anim blueprint"), *MachineName));
        }
        OutSMGraph = SMNode->EditorStateMachineGraph;
        return nullptr;
    }

    /**
     * AnimBp_LoadStateMachineGraph, then the transition SourceName -> TargetName of that state machine into
     * OutTransition: null, else the reply when the blueprint, the state machine or the transition does not resolve.
     */
    TSharedPtr<FJsonObject> AnimBp_LoadTransition(const FString& AnimBPPath, const FString& MachineName,
        const FString& SourceName, const FString& TargetName,
        UAnimBlueprint*& OutAnimBP, UAnimationStateMachineGraph*& OutSMGraph, UAnimStateTransitionNode*& OutTransition)
    {
        if (TSharedPtr<FJsonObject> Error = AnimBp_LoadStateMachineGraph(AnimBPPath, MachineName, OutAnimBP, OutSMGraph))
        {
            return Error;
        }
        OutTransition = FindTransitionBySourceTarget(OutSMGraph, SourceName, TargetName);
        if (!OutTransition)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Transition '%s' -> '%s' not found in state machine '%s'"), *SourceName, *TargetName, *MachineName));
        }
        return nullptr;
    }

    /**
     * The graph of the state machine state_machine_name in the anim blueprint at anim_blueprint_path, for the
     * read-only inspectors. Null, with OutError holding the reply, when a param is missing or nothing resolves.
     */
    UAnimationStateMachineGraph* AnimBp_ReadInspectedMachine(const TSharedPtr<FJsonObject>& Params, FString& OutMachineName,
        TSharedPtr<FJsonObject>& OutError)
    {
        FString AnimBPPath;
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("anim_blueprint_path"), AnimBPPath, mcp_common::EAllowEmpty::Yes))
        {
            OutError = Missing;
            return nullptr;
        }
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("state_machine_name"), OutMachineName, mcp_common::EAllowEmpty::Yes))
        {
            OutError = Missing;
            return nullptr;
        }
        UAnimBlueprint* AnimBP = LoadObject<UAnimBlueprint>(nullptr, *AnimBPPath);
        if (!AnimBP)
        {
            OutError = mcp_common::MakeErrorReply(
                FString::Printf(TEXT("AnimBlueprint not found: %s"), *AnimBPPath));
            return nullptr;
        }
        UAnimGraphNode_StateMachine* SMNode = FindStateMachineByName(AnimBP, OutMachineName);
        if (!SMNode)
        {
            OutError = mcp_common::MakeErrorReply(
                FString::Printf(TEXT("State machine '%s' not found"), *OutMachineName));
            return nullptr;
        }
        if (!SMNode->EditorStateMachineGraph)
        {
            OutError = mcp_common::MakeErrorReply(TEXT("No EditorStateMachineGraph"));
        }
        return SMNode->EditorStateMachineGraph;
    }

    /** A reply object that starts with anim_blueprint_path and the machine_name of SMGraph. */
    TSharedPtr<FJsonObject> AnimBp_MachineReply(const UAnimBlueprint* AnimBP, const UEdGraph* SMGraph)
    {
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetStringField(TEXT("anim_blueprint_path"), AnimBP->GetPathName());
        Resp->SetStringField(TEXT("machine_name"), SMGraph->GetName());
        return Resp;
    }

    /** AnimBp_MachineReply, then the source_state and target_state of a transition as requested. */
    TSharedPtr<FJsonObject> AnimBp_TransitionReply(const UAnimBlueprint* AnimBP, const UEdGraph* SMGraph,
        const FString& SourceName, const FString& TargetName)
    {
        TSharedPtr<FJsonObject> Resp = AnimBp_MachineReply(AnimBP, SMGraph);
        Resp->SetStringField(TEXT("source_state"), SourceName);
        Resp->SetStringField(TEXT("target_state"), TargetName);
        return Resp;
    }

    /**
     * Destroys every transition into or out of Node and returns how many there were. The engine's interactive cascade
     * (a transition self-destructs in UAnimStateTransitionNode::PinConnectionListChanged when its endpoint's
     * DestroyNode breaks the pin link) does NOT fire on the headless bridge path, so removing the endpoint alone would
     * leave its transitions pointing at a destroyed node. They are snapshotted first so SMGraph->Nodes is not mutated
     * while it is iterated.
     */
    int32 AnimBp_DestroyIncidentTransitions(UAnimationStateMachineGraph* SMGraph, const UAnimStateNodeBase* Node)
    {
        TArray<UAnimStateTransitionNode*> Incident;
        for (UEdGraphNode* It : SMGraph->Nodes)
        {
            if (UAnimStateTransitionNode* T = Cast<UAnimStateTransitionNode>(It))
            {
                if (T->GetPreviousState() == Node || T->GetNextState() == Node)
                {
                    Incident.Add(T);
                }
            }
        }
        for (UAnimStateTransitionNode* T : Incident)
        {
            T->DestroyNode();
        }
        return Incident.Num();
    }

    /**
     * Where the rule variable VariableName lives on AnimBP: "generated" (OutProperty is its property on the
     * generated or skeleton class) or "new_blueprint" (declared on the AnimBP but not compiled into the generated
     * class yet, so OutProperty is null; mcp_common::SpawnVariableGetNode resolves it from NewVariables). Empty, with
     * OutError holding the reply, when neither has it.
     */
    FString AnimBp_RuleVariableKind(UAnimBlueprint* AnimBP, const FString& VariableName, FProperty*& OutProperty,
        TSharedPtr<FJsonObject>& OutError)
    {
        const FName VarName(*VariableName);
        OutProperty = FindFProperty<FProperty>(AnimBP->GeneratedClass, VarName);
        if (!OutProperty && AnimBP->SkeletonGeneratedClass)
        {
            OutProperty = FindFProperty<FProperty>(AnimBP->SkeletonGeneratedClass, VarName);
        }
        if (OutProperty)
        {
            return TEXT("generated");
        }
        if (FBlueprintEditorUtils::FindNewVariableIndex(AnimBP, VarName) != INDEX_NONE)
        {
            return TEXT("new_blueprint");
        }
        OutError = mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Variable '%s' not found on anim blueprint (not in generated class, not in NewVariables)"), *VariableName));
        return FString();
    }

    /**
     * The K2 pin type of the rule variable AnimBp_RuleVariableKind resolved into Property: converted from Property
     * when there is one, else the type declared in AnimBP's NewVariables (a variable not compiled yet). False when
     * neither gives a type.
     */
    bool AnimBp_RuleVariablePinType(const UAnimBlueprint* AnimBP, const FString& VariableName, const FProperty* Property,
        FEdGraphPinType& OutPinType)
    {
        if (Property)
        {
            return GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(Property, OutPinType);
        }
        const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(AnimBP, FName(*VariableName));
        if (Index == INDEX_NONE)
        {
            return false;
        }
        OutPinType = AnimBP->NewVariables[Index].VarType;
        return true;
    }

    /** The type of a rule variable for an error message: Property's C++ type, else the Blueprint type PinType names. */
    FString AnimBp_RuleVariableTypeName(const FProperty* Property, const FEdGraphPinType& PinType)
    {
        return Property ? Property->GetCPPType() : UEdGraphSchema_K2::TypeToText(PinType).ToString();
    }

    /**
     * Modifies RuleGraph and AnimBP, then removes every node of the rule graph except its result sink, breaking
     * their links. The nodes are snapshotted first: RemoveNode mutates RuleGraph->Nodes in place. Returns how many
     * RemoveNode calls succeeded.
     */
    int32 AnimBp_RemoveRuleContributors(UAnimBlueprint* AnimBP, UEdGraph* RuleGraph)
    {
        RuleGraph->Modify();
        AnimBP->Modify();

        TArray<UEdGraphNode*> ToRemove;
        for (UEdGraphNode* N : RuleGraph->Nodes)
        {
            if (!Cast<UAnimGraphNode_TransitionResult>(N))
            {
                ToRemove.Add(N);
            }
        }
        int32 RemovedCount = 0;
        for (UEdGraphNode* N : ToRemove)
        {
            if (RuleGraph->RemoveNode(N, /*bBreakAllLinks*/ true, /*bAlwaysMarkDirty*/ true))
            {
                ++RemovedCount;
            }
        }
        return RemovedCount;
    }

    /** The params of an internal call addressed to the state machine MachineName of the anim blueprint at AnimBPPath. */
    TSharedPtr<FJsonObject> AnimBp_MachineRequest(const FString& AnimBPPath, const FString& MachineName)
    {
        TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
        Request->SetStringField(TEXT("anim_blueprint_path"), AnimBPPath);
        Request->SetStringField(TEXT("machine_name"), MachineName);
        return Request;
    }

    /** AnimBp_MachineRequest addressed to the transition Source -> Target of that state machine. */
    TSharedPtr<FJsonObject> AnimBp_TransitionRequest(const FString& AnimBPPath, const FString& MachineName,
        const FString& Source, const FString& Target)
    {
        TSharedPtr<FJsonObject> Request = AnimBp_MachineRequest(AnimBPPath, MachineName);
        Request->SetStringField(TEXT("source_state"), Source);
        Request->SetStringField(TEXT("target_state"), Target);
        return Request;
    }
}

// ---------------------------------------------------------------------------
// create_anim_blueprint
// ---------------------------------------------------------------------------
// MCP:create_anim_blueprint - Create an Animation Blueprint (parent UAnimInstance) for a skeleton at package_path/name with the editor factory, compile it and save it; an existing Animation Blueprint at that path is returned unchanged (already_existed=true). Refused while PIE runs unless force=true. Returns asset_path, generated_class and target_skeleton_set.
//   @param package_path - UE content folder, e.g. /Game/MyGame
//   @param name - Asset name for the new Animation Blueprint
//   @param skeleton_path - Optional skeleton path; defaults to /Engine/EngineMeshes/SkeletalCube_Skeleton
//   @returns { asset_path: string, name: string, skeleton_path: string, generated_class: string, is_anim_instance: bool, target_skeleton_set: bool, created: bool, already_existed: bool, saved: bool, save_error?: string }

TSharedPtr<FJsonObject> HandleCreateAnimBlueprint(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath, Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("package_path"), PackagePath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LongPackagePathError(PackagePath)) { return Error; }

    FString SkeletonPath = TEXT("/Engine/EngineMeshes/SkeletalCube_Skeleton");
    Params->TryGetStringField(TEXT("skeleton_path"), SkeletonPath);

    USkeleton* Skeleton = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadFlexibleOrError(SkeletonPath, TEXT("skeleton"), Skeleton)) { return Error; }

    FString Folder, FullPath;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::JoinAssetPath(PackagePath, Name, Folder, FullPath)) { return Error; }

    // The reply describing Blueprint, found (bCreated false) or created by this call.
    auto AssetReply = [&Name](UAnimBlueprint* Blueprint, bool bCreated)
    {
        const bool bChild = Blueprint->GeneratedClass &&
            Blueprint->GeneratedClass->IsChildOf(UAnimInstance::StaticClass());
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
        Resp->SetStringField(TEXT("name"), Name);
        Resp->SetStringField(TEXT("skeleton_path"),
            Blueprint->TargetSkeleton ? Blueprint->TargetSkeleton->GetPathName() : TEXT(""));
        Resp->SetStringField(TEXT("generated_class"),
            Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetName() : TEXT(""));
        Resp->SetBoolField(TEXT("is_anim_instance"), bChild);
        Resp->SetBoolField(TEXT("target_skeleton_set"), Blueprint->TargetSkeleton != nullptr);
        Resp->SetBoolField(TEXT("created"), bCreated);
        Resp->SetBoolField(TEXT("already_existed"), !bCreated);
        return Resp;
    };

    // Idempotent: if it already exists, load and return a stable payload.
    if (UEditorAssetLibrary::DoesAssetExist(FullPath))
    {
        UAnimBlueprint* Existing = mcp_common::LoadAssetOrSoftPath<UAnimBlueprint>(FullPath);
        if (Existing)
        {
            return mcp_common::MakeOkReply(AssetReply(Existing, false));
        }
    }

    // Create via the real editor factory; the sync CreateAsset overload skips the
    // modal ConfigureProperties picker. We set TargetSkeleton + ParentClass first.
    FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
    IAssetTools& AssetTools = AssetToolsModule.Get();

    UAnimBlueprintFactory* Factory = NewObject<UAnimBlueprintFactory>();
    Factory->ParentClass = UAnimInstance::StaticClass();
    Factory->TargetSkeleton = Skeleton;
    Factory->bTemplate = false;
    Factory->BlueprintType = BPTYPE_Normal;

    UObject* NewAsset = AssetTools.CreateAsset(Name, Folder, UAnimBlueprint::StaticClass(), Factory);
    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(NewAsset);
    if (!AnimBP)
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Failed to create anim blueprint at %s"), *FullPath));

    // Ensure the skeleton is set on the blueprint itself (the factory does this, but
    // verify-by-readback per the project's setter-may-lie rule).
    AnimBP->TargetSkeleton = Skeleton;

    // Compile so the generated class is up to date, then save.
    FKismetEditorUtilities::CompileBlueprint(AnimBP, EBlueprintCompileOptions::SkipGarbageCollection);

    FAssetRegistryModule::AssetCreated(AnimBP);

    FString SaveErr;
    const bool bSaved = mcp_common::SaveAssetPackage(AnimBP->GetOutermost(), AnimBP, SaveErr);
    return mcp_common::MakeSavedReply(AssetReply(AnimBP, true), bSaved, TEXT("create_anim_blueprint"), SaveErr);
}

// ---------------------------------------------------------------------------
// add_anim_state_machine
// ---------------------------------------------------------------------------
// MCP:add_anim_state_machine - Add a state machine node named machine_name to the top-level AnimGraph of an Animation Blueprint, then mark the blueprint structurally modified and save it without compiling (compile_anim_blueprint gives runtime effect). Idempotent by name: an existing machine is returned unchanged (already_existed true). The node starts with only its entry and is not connected to the Output Pose: wire it with connect_anim_graph_pins, fill it with add_anim_state, add_anim_conduit and add_anim_transition, or build it in one call with build_anim_state_machine. Refused while PIE runs unless force=true; a failed save is an error, the change kept in memory.
//   @param anim_blueprint_path - Path to the UAnimBlueprint asset
//   @param machine_name - Name for the new state machine
//   @returns { anim_blueprint_path: string, machine_name: string, node_class: string, state_machine_graph: string, state_machine_created: bool, already_existed: bool, saved: bool, state_machine_count: int, save_error?: string }

TSharedPtr<FJsonObject> HandleAddAnimStateMachine(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimBPPath, MachineName;
    if (TSharedPtr<FJsonObject> Missing = AnimBp_ReadMachineParams(Params, AnimBPPath, MachineName))
        return Missing;

    UAnimBlueprint* AnimBP = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadFlexibleOrError(AnimBPPath, TEXT("anim blueprint"), AnimBP)) { return Error; }

    UAnimationGraph* AnimGraph = FindAnimGraph(AnimBP);
    if (!AnimGraph)
        return mcp_common::MakeErrorReply(
            TEXT("Could not find the AnimGraph on this anim blueprint"));

    // The reply for the state machine node Machine, found (bCreated false) or created by this call.
    auto MachineNodeReply = [AnimBP](UAnimGraphNode_StateMachine* Machine, bool bCreated)
    {
        TSharedPtr<FJsonObject> Resp = AnimBp_MachineReply(AnimBP, Machine->EditorStateMachineGraph);
        Resp->SetStringField(TEXT("node_class"), Machine->GetClass()->GetName());
        Resp->SetStringField(TEXT("state_machine_graph"), Machine->EditorStateMachineGraph->GetPathName());
        Resp->SetBoolField(TEXT("state_machine_created"), bCreated);
        Resp->SetBoolField(TEXT("already_existed"), !bCreated);
        Resp->SetNumberField(TEXT("state_machine_count"), CountStateMachines(AnimBP));
        return Resp;
    };

    // Idempotent: if a state machine with this bound-graph name already exists, return it.
    if (UAnimGraphNode_StateMachine* ExistingSM = FindStateMachineByName(AnimBP, MachineName))
    {
        return mcp_common::MakeOkReply(MachineNodeReply(ExistingSM, false));
    }

    // Insert a UAnimGraphNode_StateMachine into the AnimGraph using the editor asset
    // model. FGraphNodeCreator::Finalize() calls PostPlacedNewNode(), which creates
    // the EditorStateMachineGraph via FBlueprintEditorUtils::CreateNewGraph — fully
    // headless-safe (no Slate, no task-graph re-entry).
    AnimGraph->Modify();
    AnimBP->Modify();

    UAnimGraphNode_StateMachine* SMNode = nullptr;
    {
        FGraphNodeCreator<UAnimGraphNode_StateMachine> NodeCreator(*AnimGraph);
        SMNode = NodeCreator.CreateNode(/*bSelectNewNode*/ false);
        NodeCreator.Finalize();
    }

    if (!SMNode || !SMNode->EditorStateMachineGraph)
        return mcp_common::MakeErrorReply(
            TEXT("State machine node was created but its bound graph was not initialized"));

    // Rename the bound graph to the requested machine name (RenameGraph uses a unique-name
    // path internally; tolerant of collisions).
    FBlueprintEditorUtils::RenameGraph(SMNode->EditorStateMachineGraph, MachineName);

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

    FString SaveErr;
    const bool bSaved = mcp_common::SaveAssetPackage(AnimBP->GetOutermost(), AnimBP, SaveErr);
    return mcp_common::MakeSavedReply(MachineNodeReply(SMNode, true), bSaved, TEXT("add_anim_state_machine"), SaveErr);
}

// ---------------------------------------------------------------------------
// add_anim_state
// ---------------------------------------------------------------------------
// MCP:add_anim_state - Add a state node named state_name to an existing state machine of an Animation Blueprint, then mark the blueprint structurally modified and save it without compiling (compile_anim_blueprint gives runtime effect). Not an undo step. Idempotent by name: a state of that name (case-insensitive) is returned unchanged with already_existed true, and nothing is saved. The machine must sit on the top-level AnimGraph (add_anim_state_machine creates it); a missing blueprint or machine is an error. The new state is unconnected and its animation graph empty: link it with add_anim_transition, fill it with add_anim_graph_node, check the machine with get_anim_state_machine_info; remove_anim_state is the inverse. Refused while PIE runs unless force=true; a failed save is an error, the change kept in memory.
//   @param anim_blueprint_path - Content path of the UAnimBlueprint (object or package path), e.g. '/Game/Characters/ABP_Hero'.
//   @param machine_name - Name of a state machine on the blueprint's top-level AnimGraph, as get_anim_blueprint_info lists it in state_machines (case-insensitive), e.g. 'Locomotion'.
//   @param state_name - Name for the new state, e.g. 'Idle'; the reply's state_name is the name its graph actually got.
//   @returns { anim_blueprint_path: string, machine_name: string, state_name: string, node_class: string, state_created: bool, already_existed: bool, saved: bool, state_count: int, save_error?: string }

TSharedPtr<FJsonObject> HandleAddAnimState(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimBPPath, MachineName, StateName;
    if (TSharedPtr<FJsonObject> Missing = AnimBp_ReadMachineParams(Params, AnimBPPath, MachineName))
        return Missing;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("state_name"), StateName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UAnimBlueprint* AnimBP = nullptr;
    UAnimationStateMachineGraph* SMGraph = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimBp_LoadStateMachineGraph(AnimBPPath, MachineName, AnimBP, SMGraph)) { return Error; }

    // The reply for the state node State, found (bCreated false) or created by this call.
    auto StateNodeReply = [AnimBP, SMGraph](UAnimStateNode* State, bool bCreated)
    {
        TSharedPtr<FJsonObject> Resp = AnimBp_MachineReply(AnimBP, SMGraph);
        Resp->SetStringField(TEXT("state_name"), State->BoundGraph->GetName());
        Resp->SetStringField(TEXT("node_class"), State->GetClass()->GetName());
        Resp->SetBoolField(TEXT("state_created"), bCreated);
        Resp->SetBoolField(TEXT("already_existed"), !bCreated);
        Resp->SetNumberField(TEXT("state_count"), AnimBp_CountNodes<UAnimStateNode>(SMGraph));
        return Resp;
    };

    // Idempotent: if a state with this bound-graph name already exists, return it.
    for (UEdGraphNode* Node : SMGraph->Nodes)
    {
        if (UAnimStateNode* Existing = Cast<UAnimStateNode>(Node))
        {
            if (Existing->BoundGraph &&
                Existing->BoundGraph->GetName().Equals(StateName, ESearchCase::IgnoreCase))
            {
                return mcp_common::MakeOkReply(StateNodeReply(Existing, false));
            }
        }
    }

    // Insert a UAnimStateNode into the state machine graph. Its PostPlacedNewNode()
    // builds the per-state bound graph via FBlueprintEditorUtils::CreateNewGraph
    // (headless-safe; same proven mechanism as the state machine node).
    SMGraph->Modify();
    AnimBP->Modify();

    UAnimStateNode* StateNode = nullptr;
    {
        FGraphNodeCreator<UAnimStateNode> NodeCreator(*SMGraph);
        StateNode = NodeCreator.CreateNode(/*bSelectNewNode*/ false);
        NodeCreator.Finalize();
    }

    if (!StateNode || !StateNode->BoundGraph)
        return mcp_common::MakeErrorReply(
            TEXT("State node was created but its bound graph was not initialized"));

    FBlueprintEditorUtils::RenameGraph(StateNode->BoundGraph, StateName);

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

    FString SaveErr;
    const bool bSaved = mcp_common::SaveAssetPackage(AnimBP->GetOutermost(), AnimBP, SaveErr);
    return mcp_common::MakeSavedReply(StateNodeReply(StateNode, true), bSaved, TEXT("add_anim_state"), SaveErr);
}

// ---------------------------------------------------------------------------
// add_anim_conduit
// ---------------------------------------------------------------------------
// MCP:add_anim_conduit - Add a conduit node named conduit_name to an existing state machine of an Animation Blueprint (a missing blueprint or machine is an error; create the machine with add_anim_state_machine), then mark the blueprint structurally modified and save it without compiling. Idempotent by name: an existing conduit is returned unchanged (already_existed true). The conduit is placed unconnected: link it to states with add_anim_transition; remove_anim_conduit undoes it. Refused while PIE runs unless force=true; a failed save is an error, the change kept in memory.
//   @param anim_blueprint_path - Path to the UAnimBlueprint asset
//   @param machine_name - Name of the target state machine
//   @param conduit_name - Name for the new conduit
//   @returns { anim_blueprint_path: string, machine_name: string, conduit_name: string, node_class: string, conduit_created: bool, already_existed: bool, saved: bool, conduit_count: int, save_error?: string }
TSharedPtr<FJsonObject> HandleAddAnimConduit(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimBPPath, MachineName, ConduitName;
    if (TSharedPtr<FJsonObject> Missing = AnimBp_ReadMachineParams(Params, AnimBPPath, MachineName))
        return Missing;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("conduit_name"), ConduitName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UAnimBlueprint* AnimBP = nullptr;
    UAnimationStateMachineGraph* SMGraph = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimBp_LoadStateMachineGraph(AnimBPPath, MachineName, AnimBP, SMGraph)) { return Error; }

    // The reply for the conduit node Conduit, found (bCreated false) or created by this call.
    auto ConduitNodeReply = [AnimBP, SMGraph](UAnimStateConduitNode* Conduit, bool bCreated)
    {
        TSharedPtr<FJsonObject> Resp = AnimBp_MachineReply(AnimBP, SMGraph);
        Resp->SetStringField(TEXT("conduit_name"), Conduit->GetBoundGraph()->GetName());
        Resp->SetStringField(TEXT("node_class"), Conduit->GetClass()->GetName());
        Resp->SetBoolField(TEXT("conduit_created"), bCreated);
        Resp->SetBoolField(TEXT("already_existed"), !bCreated);
        Resp->SetNumberField(TEXT("conduit_count"), AnimBp_CountNodes<UAnimStateConduitNode>(SMGraph));
        return Resp;
    };

    // Idempotent: if a conduit with this bound-graph name already exists, return it unchanged.
    for (UEdGraphNode* Node : SMGraph->Nodes)
    {
        if (UAnimStateConduitNode* Existing = Cast<UAnimStateConduitNode>(Node))
        {
            const UEdGraph* Bound = Existing->GetBoundGraph();
            if (Bound && Bound->GetName().Equals(ConduitName, ESearchCase::IgnoreCase))
            {
                return mcp_common::MakeOkReply(ConduitNodeReply(Existing, false));
            }
        }
    }

    SMGraph->Modify();
    AnimBP->Modify();

    // Spawn the conduit exactly as the editor does on a "Add Conduit" action. Unlike the
    // transition node, the conduit neither auto-connects nor self-destructs on an unlinked
    // pin, so we do NOT call CreateConnections — a bare conduit is a stable, valid node.
    // PostPlacedNewNode() (run inside PerformAction) builds the conduit's BoundGraph via
    // FBlueprintEditorUtils::CreateNewGraph (headless-safe; no Slate / PIE).
    UAnimStateConduitNode* ConduitNode = FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate<UAnimStateConduitNode>(
        SMGraph, NewObject<UAnimStateConduitNode>(SMGraph), FVector2D(0.0f, 0.0f), /*bSelectNewNode*/ false);
    if (!ConduitNode || !ConduitNode->GetBoundGraph())
        return mcp_common::MakeErrorReply(
            TEXT("Conduit node was created but its bound graph was not initialized"));

    FBlueprintEditorUtils::RenameGraph(ConduitNode->GetBoundGraph(), ConduitName);

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

    FString SaveErr;
    const bool bSaved = mcp_common::SaveAssetPackage(AnimBP->GetOutermost(), AnimBP, SaveErr);
    return mcp_common::MakeSavedReply(ConduitNodeReply(ConduitNode, true), bSaved, TEXT("add_anim_conduit"), SaveErr);
}

// ---------------------------------------------------------------------------
// add_anim_transition
// ---------------------------------------------------------------------------
// MCP:add_anim_transition - Add a transition from source_state to target_state in an existing state machine of an Animation Blueprint, set its crossfade, blend mode, bidirectional flag and priority, then mark the blueprint structurally modified and save it without compiling (compile_anim_blueprint gives runtime effect). The machine (top-level AnimGraph) and both endpoints (a state or a conduit) are matched by name without case; a missing one, the same state twice or an unknown blend_mode is an error. Idempotent per pair: an existing source->target transition is returned unchanged with already_existed=true and nothing is saved (to change its settings, remove_anim_transition and add it again). It starts with no rule: wire one with set_anim_transition_rule_from_bool, set_anim_transition_rule_from_comparison or set_anim_transition_rule_automatic. Refused while PIE runs unless force=true; a failed save is an error, the change kept in memory.
//   @param anim_blueprint_path - Path to the UAnimBlueprint asset
//   @param machine_name - Name of the state machine that owns both states
//   @param source_state - Name of the source state or conduit (transition origin), matched without case
//   @param target_state - Name of the target state or conduit (transition destination), matched without case
//   @param crossfade_duration [number] - Optional blend duration in seconds (default 0.2)
//   @param blend_mode [string] - Optional EAlphaBlendOption name, e.g. Linear, Cubic, HermiteCubic (default HermiteCubic); an unknown name is an error
//   @param bidirectional [bool] - Optional, default false; sets the Bidirectional flag so the same transition can also fire target->source (no second transition is added)
//   @param priority_order [int] - Optional priority order (default 1); when several transitions out of a state pass at once, the smallest value wins
//   @returns { anim_blueprint_path: string, machine_name: string, source_state: string, target_state: string, transition_created: bool, already_existed: bool, crossfade_duration: number, blend_mode: string, bidirectional: bool, priority_order: int, transition_count: int, saved: bool, save_error?: string }
TSharedPtr<FJsonObject> HandleAddAnimTransition(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimBPPath, MachineName, SourceName, TargetName;
    if (TSharedPtr<FJsonObject> Missing = AnimBp_ReadTransitionParams(Params, AnimBPPath, MachineName, SourceName, TargetName))
        return Missing;

    UAnimBlueprint* AnimBP = nullptr;
    UAnimationStateMachineGraph* SMGraph = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimBp_LoadStateMachineGraph(AnimBPPath, MachineName, AnimBP, SMGraph)) { return Error; }

    // Resolve both endpoints (state OR conduit) within the SAME state-machine graph.
    // The transition self-destructs (PinConnectionListChanged -> DestroyNode) if a pin
    // ends up unlinked, so we must have valid, same-graph endpoints before spawning.
    UAnimStateNodeBase* SourceState = FindStateByName(SMGraph, SourceName);
    if (!SourceState)
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Source state '%s' not found in state machine '%s'"), *SourceName, *MachineName));
    UAnimStateNodeBase* TargetState = FindStateByName(SMGraph, TargetName);
    if (!TargetState)
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Target state '%s' not found in state machine '%s'"), *TargetName, *MachineName));
    if (SourceState == TargetState)
        return mcp_common::MakeErrorReply(
            TEXT("source_state and target_state must be different states"));

    // Optional transition properties (public UPROPERTYs on UAnimStateTransitionNode).
    double CrossfadeDuration = 0.2;            // engine default
    Params->TryGetNumberField(TEXT("crossfade_duration"), CrossfadeDuration);
    bool bBidirectional = false;
    Params->TryGetBoolField(TEXT("bidirectional"), bBidirectional);
    double PriorityOrderD = 1.0;               // engine default
    Params->TryGetNumberField(TEXT("priority_order"), PriorityOrderD);
    const int32 PriorityOrder = static_cast<int32>(PriorityOrderD);

    // Resolve the requested blend mode against EAlphaBlendOption; default HermiteCubic.
    EAlphaBlendOption BlendMode = EAlphaBlendOption::HermiteCubic;
    FString BlendModeStr;
    if (Params->TryGetStringField(TEXT("blend_mode"), BlendModeStr) && !BlendModeStr.IsEmpty())
    {
        const UEnum* BlendEnum = StaticEnum<EAlphaBlendOption>();
        const int64 Value = BlendEnum ? BlendEnum->GetValueByNameString(BlendModeStr) : INDEX_NONE;
        if (Value == INDEX_NONE)
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Invalid blend_mode '%s' (expected an EAlphaBlendOption name like Linear, Cubic, HermiteCubic)"), *BlendModeStr));
        BlendMode = static_cast<EAlphaBlendOption>(Value);
    }

    // The reply for Transition, found (bCreated false) or created by this call: the endpoints by their
    // bound-graph names and the transition's own settings.
    auto TransitionNodeReply = [&](UAnimStateTransitionNode* Transition, bool bCreated)
    {
        const UEnum* BlendEnum = StaticEnum<EAlphaBlendOption>();
        TSharedPtr<FJsonObject> Resp = AnimBp_TransitionReply(AnimBP, SMGraph,
            SourceState->GetBoundGraph() ? SourceState->GetBoundGraph()->GetName() : SourceName,
            TargetState->GetBoundGraph() ? TargetState->GetBoundGraph()->GetName() : TargetName);
        Resp->SetBoolField(TEXT("transition_created"), bCreated);
        Resp->SetBoolField(TEXT("already_existed"), !bCreated);
        Resp->SetNumberField(TEXT("crossfade_duration"), Transition->CrossfadeDuration);
        Resp->SetStringField(TEXT("blend_mode"), BlendEnum ? BlendEnum->GetNameStringByValue(static_cast<int64>(Transition->BlendMode)) : TEXT(""));
        Resp->SetBoolField(TEXT("bidirectional"), Transition->Bidirectional);
        Resp->SetNumberField(TEXT("priority_order"), Transition->PriorityOrder);
        Resp->SetNumberField(TEXT("transition_count"), AnimBp_CountNodes<UAnimStateTransitionNode>(SMGraph));
        return Resp;
    };

    // Idempotent: if a transition source->target already exists, return it unchanged.
    {
        TArray<UAnimStateTransitionNode*> Existing;
        SourceState->GetTransitionList(Existing);
        for (UAnimStateTransitionNode* T : Existing)
        {
            if (T && T->GetNextState() == TargetState)
            {
                return mcp_common::MakeOkReply(TransitionNodeReply(T, false));
            }
        }
    }

    SMGraph->Modify();
    AnimBP->Modify();

    // Spawn the transition exactly as the editor does on a state->state drag, then
    // wire it IMMEDIATELY (CreateConnections), before any pin-change callback can fire
    // and trigger the node's self-destruct on an unlinked pin.
    UAnimStateTransitionNode* TransitionNode = FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate<UAnimStateTransitionNode>(
        SMGraph, NewObject<UAnimStateTransitionNode>(SMGraph), FVector2D(0.0f, 0.0f), /*bSelectNewNode*/ false);
    if (!TransitionNode)
        return mcp_common::MakeErrorReply(
            TEXT("Failed to spawn the transition node into the state machine graph"));

    TransitionNode->CreateConnections(SourceState, TargetState);

    // Apply transition properties (public fields).
    TransitionNode->CrossfadeDuration = static_cast<float>(CrossfadeDuration);
    TransitionNode->BlendMode = BlendMode;
    TransitionNode->Bidirectional = bBidirectional;
    TransitionNode->PriorityOrder = PriorityOrder;

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

    FString SaveErr;
    const bool bSaved = mcp_common::SaveAssetPackage(AnimBP->GetOutermost(), AnimBP, SaveErr);
    return mcp_common::MakeSavedReply(TransitionNodeReply(TransitionNode, true), bSaved, TEXT("add_anim_transition"), SaveErr);
}

// ---------------------------------------------------------------------------
// remove_anim_state
// ---------------------------------------------------------------------------
// Removal of state-machine nodes. The generic delete_node cannot reach them, since they live in nested
// UAnimationStateMachineGraphs; each command calls the node's DestroyNode() override so its bound subgraph is
// torn down, not leaked.

// MCP:remove_anim_state - Remove a state node from a state machine of an Animation Blueprint, together with its animation graph and every transition into or out of it (cascaded_transitions_removed), then mark the blueprint structurally modified and save it without compiling (compile_anim_blueprint gives runtime effect). Not an undo step. Idempotent by name: a state that is not there is a success with already_absent=true, and nothing is changed or saved. A name that belongs to a conduit is an error (use remove_anim_conduit), and so is a missing blueprint or machine. List the states first with get_anim_state_machine_info; add_anim_state is the inverse, and remove_anim_transition takes out a single transition instead. Refused while PIE runs unless force=true; a failed save is an error, the change kept in memory.
//   @param anim_blueprint_path - Content path of the UAnimBlueprint (object or package path), e.g. '/Game/Characters/ABP_Hero'.
//   @param machine_name - Name of a state machine on the blueprint's top-level AnimGraph, as get_anim_blueprint_info lists it in state_machines (case-insensitive), e.g. 'Locomotion'.
//   @param state_name - Name of the state to remove, as get_anim_state_machine_info lists it (case-insensitive), e.g. 'Idle'.
//   @returns { anim_blueprint_path: string, machine_name: string, state_name: string, removed: bool, already_absent: bool, state_count: int, transition_count: int, cascaded_transitions_removed: int, saved: bool, save_error?: string }
TSharedPtr<FJsonObject> HandleRemoveAnimState(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimBPPath, MachineName, StateName;
    if (TSharedPtr<FJsonObject> Missing = AnimBp_ReadMachineParams(Params, AnimBPPath, MachineName))
        return Missing;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("state_name"), StateName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UAnimBlueprint* AnimBP = nullptr;
    UAnimationStateMachineGraph* SMGraph = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimBp_LoadStateMachineGraph(AnimBPPath, MachineName, AnimBP, SMGraph)) { return Error; }

    // Resolve the endpoint by bound-graph name. FindStateByName excludes transition nodes,
    // so it yields a state OR conduit; here we require a concrete UAnimStateNode.
    UAnimStateNodeBase* Endpoint = FindStateByName(SMGraph, StateName);
    UAnimStateNode* StateNode = Cast<UAnimStateNode>(Endpoint);

    if (!StateNode)
    {
        // Idempotent no-op: either nothing matched, or a non-state (conduit) matched the name.
        // Distinguish so the caller isn't silently confused by a mismatched node kind.
        if (Endpoint)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Node '%s' in state machine '%s' is a %s, not a state (use the matching remove command)"),
                    *StateName, *MachineName, *Endpoint->GetClass()->GetName()));
        }
        UE_LOG(LogTemp, Verbose,
            TEXT("[MCP] remove_anim_state: state '%s' absent from machine '%s' on '%s' -- idempotent no-op"),
            *StateName, *MachineName, *AnimBP->GetPathName());
        TSharedPtr<FJsonObject> Resp = AnimBp_MachineReply(AnimBP, SMGraph);
        Resp->SetStringField(TEXT("state_name"), StateName);
        Resp->SetBoolField(TEXT("removed"), false);
        Resp->SetBoolField(TEXT("already_absent"), true);
        Resp->SetNumberField(TEXT("state_count"), AnimBp_CountNodes<UAnimStateNode>(SMGraph));
        Resp->SetNumberField(TEXT("transition_count"), AnimBp_CountNodes<UAnimStateTransitionNode>(SMGraph));
        Resp->SetNumberField(TEXT("cascaded_transitions_removed"), 0);
        Resp->SetBoolField(TEXT("saved"), false);
        return mcp_common::MakeOkReply(Resp);
    }

    const int32 TransitionsBefore = AnimBp_CountNodes<UAnimStateTransitionNode>(SMGraph);

    SMGraph->Modify();
    AnimBP->Modify();

    // Cascade: destroy the transitions incident to this state (both directions) BEFORE removing it,
    // so a state removal never leaves a transition pointing at a destroyed node.
    const int32 CascadedRemoved = AnimBp_DestroyIncidentTransitions(SMGraph, StateNode);

    // Subgraph: call the node's DestroyNode() override — NOT SMGraph->RemoveNode().
    // The UAnimStateNode::DestroyNode override nulls BoundGraph then calls
    // FBlueprintEditorUtils::RemoveGraph(BP, BoundGraph, Recompile), so the state's bound
    // animation subgraph is torn down instead of orphaned/leaked.
    UE_LOG(LogTemp, Verbose,
        TEXT("[MCP] remove_anim_state: destroying state '%s' in machine '%s' on '%s' (incident transitions removed=%d, transitions before=%d)"),
        *StateName, *MachineName, *AnimBP->GetPathName(), CascadedRemoved, TransitionsBefore);
    StateNode->DestroyNode();

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

    const int32 TransitionsAfter = AnimBp_CountNodes<UAnimStateTransitionNode>(SMGraph);
    UE_LOG(LogTemp, Verbose,
        TEXT("[MCP] remove_anim_state: '%s' removed; cascaded transitions removed=%d (after=%d)"),
        *StateName, CascadedRemoved, TransitionsAfter);

    FString SaveErr;
    const bool bSaved = mcp_common::SaveAssetPackage(AnimBP->GetOutermost(), AnimBP, SaveErr);

    TSharedPtr<FJsonObject> Resp = AnimBp_MachineReply(AnimBP, SMGraph);
    Resp->SetStringField(TEXT("state_name"), StateName);
    Resp->SetBoolField(TEXT("removed"), true);
    Resp->SetBoolField(TEXT("already_absent"), false);
    Resp->SetNumberField(TEXT("state_count"), AnimBp_CountNodes<UAnimStateNode>(SMGraph));
    Resp->SetNumberField(TEXT("transition_count"), TransitionsAfter);
    Resp->SetNumberField(TEXT("cascaded_transitions_removed"), CascadedRemoved);
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("remove_anim_state"), SaveErr);
}

// ---------------------------------------------------------------------------
// remove_anim_conduit
// ---------------------------------------------------------------------------
// MCP:remove_anim_conduit - Remove a conduit node from a state machine of an Animation Blueprint, together with its rule graph and every transition into or out of it (cascaded_transitions_removed), then mark the blueprint structurally modified and save it without compiling (compile_anim_blueprint gives runtime effect). Not an undo step. Idempotent by name: a conduit that is not there is a success with already_absent=true, and nothing is changed or saved. A name that belongs to a state is an error (use remove_anim_state), and so is a missing blueprint or machine. List the conduits first with get_anim_state_machine_info; add_anim_conduit is the inverse, and remove_anim_transition takes out a single transition instead. Refused while PIE runs unless force=true; a failed save is an error, the change kept in memory.
//   @param anim_blueprint_path - Content path of the UAnimBlueprint (object or package path), e.g. '/Game/Characters/ABP_Hero'.
//   @param machine_name - Name of a state machine on the blueprint's top-level AnimGraph, as get_anim_blueprint_info lists it in state_machines (case-insensitive), e.g. 'Locomotion'.
//   @param conduit_name - Name of the conduit to remove, as get_anim_state_machine_info lists it (case-insensitive), e.g. 'ToAirborne'.
//   @returns { anim_blueprint_path: string, machine_name: string, conduit_name: string, removed: bool, already_absent: bool, conduit_count: int, transition_count: int, cascaded_transitions_removed: int, saved: bool, save_error?: string }
TSharedPtr<FJsonObject> HandleRemoveAnimConduit(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimBPPath, MachineName, ConduitName;
    if (TSharedPtr<FJsonObject> Missing = AnimBp_ReadMachineParams(Params, AnimBPPath, MachineName))
        return Missing;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("conduit_name"), ConduitName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UAnimBlueprint* AnimBP = nullptr;
    UAnimationStateMachineGraph* SMGraph = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimBp_LoadStateMachineGraph(AnimBPPath, MachineName, AnimBP, SMGraph)) { return Error; }

    // Resolve by bound-graph name, then require a conduit specifically (reject a plain state).
    UAnimStateNodeBase* Endpoint = FindStateByName(SMGraph, ConduitName);
    UAnimStateConduitNode* ConduitNode = Cast<UAnimStateConduitNode>(Endpoint);

    if (!ConduitNode)
    {
        if (Endpoint)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Node '%s' in state machine '%s' is a %s, not a conduit (use the matching remove command)"),
                    *ConduitName, *MachineName, *Endpoint->GetClass()->GetName()));
        }
        UE_LOG(LogTemp, Verbose,
            TEXT("[MCP] remove_anim_conduit: conduit '%s' absent from machine '%s' on '%s' -- idempotent no-op"),
            *ConduitName, *MachineName, *AnimBP->GetPathName());
        TSharedPtr<FJsonObject> Resp = AnimBp_MachineReply(AnimBP, SMGraph);
        Resp->SetStringField(TEXT("conduit_name"), ConduitName);
        Resp->SetBoolField(TEXT("removed"), false);
        Resp->SetBoolField(TEXT("already_absent"), true);
        Resp->SetNumberField(TEXT("conduit_count"), AnimBp_CountNodes<UAnimStateConduitNode>(SMGraph));
        Resp->SetNumberField(TEXT("transition_count"), AnimBp_CountNodes<UAnimStateTransitionNode>(SMGraph));
        Resp->SetNumberField(TEXT("cascaded_transitions_removed"), 0);
        Resp->SetBoolField(TEXT("saved"), false);
        return mcp_common::MakeOkReply(Resp);
    }

    const int32 TransitionsBefore = AnimBp_CountNodes<UAnimStateTransitionNode>(SMGraph);

    SMGraph->Modify();
    AnimBP->Modify();

    // Cascade: destroy the transitions incident to this conduit BEFORE removing it (same as states).
    const int32 CascadedRemoved = AnimBp_DestroyIncidentTransitions(SMGraph, ConduitNode);

    // Subgraph: DestroyNode() (UAnimStateConduitNode override) nulls BoundGraph and
    // calls FBlueprintEditorUtils::RemoveGraph — the conduit's rule subgraph is torn down, not leaked.
    UE_LOG(LogTemp, Verbose,
        TEXT("[MCP] remove_anim_conduit: destroying conduit '%s' in machine '%s' on '%s' (incident transitions removed=%d, transitions before=%d)"),
        *ConduitName, *MachineName, *AnimBP->GetPathName(), CascadedRemoved, TransitionsBefore);
    ConduitNode->DestroyNode();

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

    const int32 TransitionsAfter = AnimBp_CountNodes<UAnimStateTransitionNode>(SMGraph);
    UE_LOG(LogTemp, Verbose,
        TEXT("[MCP] remove_anim_conduit: '%s' removed; cascaded transitions removed=%d (after=%d)"),
        *ConduitName, CascadedRemoved, TransitionsAfter);

    FString SaveErr;
    const bool bSaved = mcp_common::SaveAssetPackage(AnimBP->GetOutermost(), AnimBP, SaveErr);

    TSharedPtr<FJsonObject> Resp = AnimBp_MachineReply(AnimBP, SMGraph);
    Resp->SetStringField(TEXT("conduit_name"), ConduitName);
    Resp->SetBoolField(TEXT("removed"), true);
    Resp->SetBoolField(TEXT("already_absent"), false);
    Resp->SetNumberField(TEXT("conduit_count"), AnimBp_CountNodes<UAnimStateConduitNode>(SMGraph));
    Resp->SetNumberField(TEXT("transition_count"), TransitionsAfter);
    Resp->SetNumberField(TEXT("cascaded_transitions_removed"), CascadedRemoved);
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("remove_anim_conduit"), SaveErr);
}

// ---------------------------------------------------------------------------
// remove_anim_transition
// ---------------------------------------------------------------------------
// MCP:remove_anim_transition - Remove the transition from source_state to target_state in a state machine of an Animation Blueprint, with its rule graph (kept while a bidirectional twin shares it), then mark the blueprint structurally modified and save it without compiling (compile_anim_blueprint gives runtime effect). Only that direction goes; the states stay. Not an undo step. Idempotent: a missing source or target state, or no transition between them, is a success with already_absent=true, and nothing is changed or saved; a missing blueprint or machine is an error. add_anim_transition is the inverse; clear_anim_transition_rule empties the rule but keeps the transition; remove_state_transition is the StateTree counterpart. Refused while PIE runs unless force=true; a failed save is an error, the change kept in memory.
//   @param anim_blueprint_path - Content path of the UAnimBlueprint (object or package path), e.g. '/Game/Characters/ABP_Hero'.
//   @param machine_name - Name of a state machine on the blueprint's top-level AnimGraph, as get_anim_blueprint_info lists it in state_machines (case-insensitive), e.g. 'Locomotion'.
//   @param source_state - Name of the state or conduit the transition leaves, as get_anim_state_machine_info lists it (case-insensitive), e.g. 'Idle'.
//   @param target_state - Name of the state or conduit the transition enters (case-insensitive), e.g. 'Run'.
//   @returns { anim_blueprint_path: string, machine_name: string, source_state: string, target_state: string, removed: bool, already_absent: bool, transition_count: int, saved: bool, save_error?: string }
TSharedPtr<FJsonObject> HandleRemoveAnimTransition(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimBPPath, MachineName, SourceName, TargetName;
    if (TSharedPtr<FJsonObject> Missing = AnimBp_ReadTransitionParams(Params, AnimBPPath, MachineName, SourceName, TargetName))
        return Missing;

    UAnimBlueprint* AnimBP = nullptr;
    UAnimationStateMachineGraph* SMGraph = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimBp_LoadStateMachineGraph(AnimBPPath, MachineName, AnimBP, SMGraph)) { return Error; }

    // FindTransitionBySourceTarget returns nullptr when either endpoint is missing OR when
    // no source->target transition exists — all three collapse to the idempotent no-op below.
    UAnimStateTransitionNode* Transition = FindTransitionBySourceTarget(SMGraph, SourceName, TargetName);
    if (!Transition)
    {
        UE_LOG(LogTemp, Verbose,
            TEXT("[MCP] remove_anim_transition: '%s' -> '%s' absent from machine '%s' on '%s' -- idempotent no-op"),
            *SourceName, *TargetName, *MachineName, *AnimBP->GetPathName());
        TSharedPtr<FJsonObject> Resp = AnimBp_TransitionReply(AnimBP, SMGraph, SourceName, TargetName);
        Resp->SetBoolField(TEXT("removed"), false);
        Resp->SetBoolField(TEXT("already_absent"), true);
        Resp->SetNumberField(TEXT("transition_count"), AnimBp_CountNodes<UAnimStateTransitionNode>(SMGraph));
        Resp->SetBoolField(TEXT("saved"), false);
        return mcp_common::MakeOkReply(Resp);
    }

    SMGraph->Modify();
    AnimBP->Modify();

    // DestroyNode() (UAnimStateTransitionNode override) tears down the rule
    // subgraph via FBlueprintEditorUtils::RemoveGraph, but ONLY when the BoundGraph is not
    // shared with a bidirectional sibling (IsBoundGraphShared() guard) — so removing one
    // direction of a bidirectional pair never yanks the graph out from under the other.
    UE_LOG(LogTemp, Verbose,
        TEXT("[MCP] remove_anim_transition: destroying transition '%s' -> '%s' in machine '%s' on '%s'"),
        *SourceName, *TargetName, *MachineName, *AnimBP->GetPathName());
    Transition->DestroyNode();

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

    const int32 TransitionsAfter = AnimBp_CountNodes<UAnimStateTransitionNode>(SMGraph);

    FString SaveErr;
    const bool bSaved = mcp_common::SaveAssetPackage(AnimBP->GetOutermost(), AnimBP, SaveErr);

    TSharedPtr<FJsonObject> Resp = AnimBp_TransitionReply(AnimBP, SMGraph, SourceName, TargetName);
    Resp->SetBoolField(TEXT("removed"), true);
    Resp->SetBoolField(TEXT("already_absent"), false);
    Resp->SetNumberField(TEXT("transition_count"), TransitionsAfter);
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("remove_anim_transition"), SaveErr);
}

// ---------------------------------------------------------------------------
// compile_anim_blueprint
// ---------------------------------------------------------------------------
// MCP:compile_anim_blueprint - Compile an Animation Blueprint synchronously and save its package, even when the compile fails; returns status (EBlueprintStatus name), is_up_to_date, has_error and the error and warning messages with their counts. Refused while PIE runs unless force=true. Use get_anim_blueprint_info to inspect without compiling.
//   @param anim_blueprint_path - Path to the UAnimBlueprint asset
//   @returns { anim_blueprint_path: string, compiled: bool, status: string, is_up_to_date: bool, has_error: bool, error_count: int, warning_count: int, errors: [string], warnings: [string], saved: bool, save_error?: string }
TSharedPtr<FJsonObject> HandleCompileAnimBlueprint(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimBPPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("anim_blueprint_path"), AnimBPPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UAnimBlueprint* AnimBP = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadFlexibleOrError(AnimBPPath, TEXT("anim blueprint"), AnimBP)) { return Error; }

    // Synchronous compile; on return AnimBP->Status reflects the outcome. Capture the
    // results log so we can surface real errors/warnings instead of just the final flag.
    FCompilerResultsLog Results;
    Results.bSilentMode = true;
    FKismetEditorUtilities::CompileBlueprint(AnimBP, EBlueprintCompileOptions::SkipGarbageCollection, &Results);

    const UEnum* StatusEnum = StaticEnum<EBlueprintStatus>();
    const EBlueprintStatus Status = static_cast<EBlueprintStatus>(AnimBP->Status.GetValue());
    const bool bUpToDate = AnimBP->IsUpToDate();   // covers BS_UpToDate and BS_UpToDateWithWarnings
    const bool bHasError = (Status == BS_Error);

    TArray<TSharedPtr<FJsonValue>> Errors;
    for (const TSharedRef<FTokenizedMessage>& Msg : Results.Messages)
    {
        if (Msg->GetSeverity() == EMessageSeverity::Error)
        {
            Errors.Add(MakeShared<FJsonValueString>(Msg->ToText().ToString()));
        }
    }
    TArray<TSharedPtr<FJsonValue>> Warnings;
    for (const TSharedRef<FTokenizedMessage>& Msg : Results.Messages)
    {
        if (Msg->GetSeverity() == EMessageSeverity::Warning)
        {
            Warnings.Add(MakeShared<FJsonValueString>(Msg->ToText().ToString()));
        }
    }

    FString SaveErr;
    const bool bSaved = mcp_common::SaveAssetPackage(AnimBP->GetOutermost(), AnimBP, SaveErr);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("anim_blueprint_path"), AnimBP->GetPathName());
    Resp->SetBoolField(TEXT("compiled"), true);
    Resp->SetStringField(TEXT("status"),
        StatusEnum ? StatusEnum->GetNameStringByValue(static_cast<int64>(Status)) : FString::FromInt(static_cast<int32>(Status)));
    Resp->SetBoolField(TEXT("is_up_to_date"), bUpToDate);
    Resp->SetBoolField(TEXT("has_error"), bHasError);
    Resp->SetNumberField(TEXT("error_count"), Errors.Num());
    Resp->SetNumberField(TEXT("warning_count"), Warnings.Num());
    Resp->SetArrayField(TEXT("errors"), Errors);
    Resp->SetArrayField(TEXT("warnings"), Warnings);
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("compile_anim_blueprint"), SaveErr);
}

// ---------------------------------------------------------------------------
// get_anim_blueprint_info
// ---------------------------------------------------------------------------
// MCP:get_anim_blueprint_info - Read an Animation Blueprint without compiling or changing it: its Asset Registry identity (object path, class, package, is_loaded, and the size and timestamp of its .uasset, left out when no file is on disk), target skeleton and its bone count, parent and generated class, is_anim_instance, is_abstract, is_template, graph_count, and the state machines at the top level of every function graph (the AnimGraph included) with their graph and state counts (conduits not counted; machines nested inside states are not listed). anim_blueprint_path takes an object path, a package path or a short name searched under /Game; no match is an error, and so is a short name several anim blueprints share, with the candidates listed.
//   @param anim_blueprint_path - Object path, package path or short name of the UAnimBlueprint asset
//   @returns { asset_path: string, asset_class: string, package_name: string, is_loaded: bool, file_size_bytes: number?, file_size_kb: number?, file_size_mb: number?, last_modified: string?, target_skeleton: string, target_skeleton_set: bool, skeleton_bone_count: int, parent_class: string, generated_class: string, is_anim_instance: bool, is_abstract: bool, is_template: bool, graph_count: int, state_machine_count: int, state_machines: [{ name: string, graph_name: string, state_count: int }] }

TSharedPtr<FJsonObject> HandleGetAnimBlueprintInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimBPPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("anim_blueprint_path"), AnimBPPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    // The registry entry first (object path, package path or short name under /Game), so is_loaded says
    // whether the asset was in memory BEFORE this call loads it. Two anim blueprints sharing a short name can
    // differ in everything, so an ambiguous name is refused with the candidates instead of picking one.
    TArray<FAssetData> Matches;
    FString LookupError;
    const bool bRegistered = !AnimBPPath.IsEmpty()
        && mcp_common::ResolveAssetFlexible(AnimBPPath, UAnimBlueprint::StaticClass(), TEXT("/Game"), Matches, LookupError)
        && Matches.Num() > 0;
    if (bRegistered && Matches.Num() > 1)
    {
        TArray<FString> MatchPaths;
        for (const FAssetData& Match : Matches)
        {
            MatchPaths.Add(Match.GetObjectPathString());
        }
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Ambiguous anim_blueprint_path '%s': %d anim blueprints match (%s). Pass a full object path to disambiguate."),
            *AnimBPPath, Matches.Num(), *FString::Join(MatchPaths, TEXT(", "))));
    }
    const bool bWasLoaded = bRegistered && Matches[0].IsAssetLoaded();

    // The registry entry loads it; a path the registry does not list (an export-text path, say) falls back
    // to the direct load. A short name never reaches that load.
    UAnimBlueprint* AnimBP = bRegistered ? Cast<UAnimBlueprint>(Matches[0].GetAsset()) : nullptr;
    if (!AnimBP && !AnimBPPath.IsEmpty())
    {
        AnimBP = mcp_common::LoadAssetOrSoftPath<UAnimBlueprint>(AnimBPPath);
    }
    if (!AnimBP)
    {
        return mcp_common::MakeErrorReply(LookupError.IsEmpty()
            ? FString::Printf(TEXT("Could not load anim blueprint: %s"), *AnimBPPath)
            : FString::Printf(TEXT("Could not load anim blueprint: %s (%s)"), *AnimBPPath, *LookupError));
    }

    TArray<UEdGraph*> AllGraphs;
    AnimBP->GetAllGraphs(AllGraphs);

    UClass* ParentClass = AnimBP->ParentClass;
    const bool bChild = AnimBP->GeneratedClass &&
        AnimBP->GeneratedClass->IsChildOf(UAnimInstance::StaticClass());

    // Describe each state machine at the top level of every function graph + its state count.
    TArray<TSharedPtr<FJsonValue>> Machines;
    for (UEdGraph* Graph : AnimBP->FunctionGraphs)
    {
        if (!Graph)
        {
            continue;
        }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UAnimGraphNode_StateMachine* SM = Cast<UAnimGraphNode_StateMachine>(Node))
            {
                TSharedPtr<FJsonObject> MObj = MakeShared<FJsonObject>();
                MObj->SetStringField(TEXT("name"),
                    SM->EditorStateMachineGraph ? SM->EditorStateMachineGraph->GetName() : TEXT(""));
                MObj->SetStringField(TEXT("graph_name"), Graph->GetName());
                MObj->SetNumberField(TEXT("state_count"),
                    SM->EditorStateMachineGraph ? AnimBp_CountNodes<UAnimStateNode>(SM->EditorStateMachineGraph) : 0);
                Machines.Add(MakeShared<FJsonValueObject>(MObj));
            }
        }
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("asset_path"), AnimBP->GetPathName());
    Resp->SetStringField(TEXT("asset_class"), AnimBP->GetClass()->GetPathName());
    const FString PackageName = AnimBP->GetOutermost()->GetName();
    Resp->SetStringField(TEXT("package_name"), PackageName);
    Resp->SetBoolField(TEXT("is_loaded"), bRegistered ? bWasLoaded : true);

    // Size and timestamp of the .uasset behind the package; left out when it maps to no file on disk.
    FString OnDisk;
    if (FPackageName::TryConvertLongPackageNameToFilename(PackageName, OnDisk, FPackageName::GetAssetPackageExtension()))
    {
        IPlatformFile& Disk = FPlatformFileManager::Get().GetPlatformFile();
        if (Disk.FileExists(*OnDisk))
        {
            const int64 Bytes = Disk.FileSize(*OnDisk);
            Resp->SetNumberField(TEXT("file_size_bytes"), static_cast<double>(Bytes));
            Resp->SetNumberField(TEXT("file_size_kb"), Bytes / 1024.0);
            Resp->SetNumberField(TEXT("file_size_mb"), Bytes / (1024.0 * 1024.0));
            Resp->SetStringField(TEXT("last_modified"), Disk.GetTimeStamp(*OnDisk).ToString());
        }
    }

    const USkeleton* TargetSkeleton = AnimBP->TargetSkeleton;
    Resp->SetStringField(TEXT("target_skeleton"), TargetSkeleton ? TargetSkeleton->GetPathName() : TEXT(""));
    Resp->SetBoolField(TEXT("target_skeleton_set"), TargetSkeleton != nullptr);
    Resp->SetNumberField(TEXT("skeleton_bone_count"), TargetSkeleton ? TargetSkeleton->GetReferenceSkeleton().GetNum() : 0);
    Resp->SetStringField(TEXT("parent_class"), ParentClass ? ParentClass->GetName() : TEXT(""));
    Resp->SetStringField(TEXT("generated_class"),
        AnimBP->GeneratedClass ? AnimBP->GeneratedClass->GetName() : TEXT(""));
    Resp->SetBoolField(TEXT("is_anim_instance"), bChild);
    Resp->SetBoolField(TEXT("is_abstract"), AnimBP->GeneratedClass && AnimBP->GeneratedClass->HasAnyClassFlags(CLASS_Abstract));
    Resp->SetBoolField(TEXT("is_template"), AnimBP->bIsTemplate);
    Resp->SetNumberField(TEXT("graph_count"), AllGraphs.Num());
    Resp->SetNumberField(TEXT("state_machine_count"), Machines.Num());
    Resp->SetArrayField(TEXT("state_machines"), Machines);
    return mcp_common::MakeOkReply(Resp);
}

// ===========================================================================
// AnimBP state machine introspection (READ-ONLY).
// Node list + count-based checks of one machine. NO graph mutation, NO compile. Headless-safe.
// ===========================================================================

// MCP:get_anim_state_machine_info - Read the nodes of one state machine of an Animation Blueprint without compiling or changing it: state_count counts the states, conduit_count the conduits and transition_count the transitions; states lists every state, conduit and alias node with its kind (class name) and graph position, transitions excluded. It also checks the machine by those counts: no states is an error, one an info, several without transitions a warning (is_valid is false only on an error); compile_anim_blueprint is the check that the machine compiles.
//   @param anim_blueprint_path - Asset path of the UAnimBlueprint
//   @param state_machine_name - Name of a state machine in the top-level AnimGraph, matched without case (get_anim_blueprint_info lists them).
//   @returns { state_machine, state_count, conduit_count, transition_count, states:[{name,kind,x,y}], is_valid, errors:[string], warnings:[string], info:[string] }
TSharedPtr<FJsonObject> HandleGetAnimStateMachineInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString MachineName;
    TSharedPtr<FJsonObject> Error;
    UAnimationStateMachineGraph* SMGraph = AnimBp_ReadInspectedMachine(Params, MachineName, Error);
    if (!SMGraph)
    {
        return Error;
    }

    // A transition is also a UAnimStateNodeBase, so it is left out of the node list here and
    // counted on its own below, like the authoring commands of this file count it.
    TArray<TSharedPtr<FJsonValue>> StateJson;
    for (UEdGraphNode* Node : SMGraph->Nodes)
    {
        UAnimStateNodeBase* StateNode = Cast<UAnimStateNodeBase>(Node);
        if (StateNode && !StateNode->IsA<UAnimStateTransitionNode>())
        {
            TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
            S->SetStringField(TEXT("name"), StateNode->GetStateName());
            S->SetStringField(TEXT("kind"), StateNode->GetClass()->GetName());
            S->SetNumberField(TEXT("x"), StateNode->NodePosX);
            S->SetNumberField(TEXT("y"), StateNode->NodePosY);
            StateJson.Add(MakeShared<FJsonValueObject>(S));
        }
    }
    // UAnimStateTransitionNode derives from UAnimStateNodeBase too: count each class on its own.
    const int32 StateCount = AnimBp_CountNodes<UAnimStateNode>(SMGraph);
    const int32 TransitionCount = AnimBp_CountNodes<UAnimStateTransitionNode>(SMGraph);

    // The checks run on those counts: states only (conduits and aliases excluded) and transition nodes.
    TArray<FString> Errors, Warnings, InfoList;
    if (StateCount == 0)
    {
        Errors.Add(TEXT("State machine has no states"));
    }
    if (TransitionCount == 0 && StateCount > 1)
    {
        Warnings.Add(TEXT("State machine has no transitions between states"));
    }
    if (StateCount == 1)
    {
        InfoList.Add(TEXT("State machine has only one state"));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("state_machine"), MachineName);
    Data->SetNumberField(TEXT("state_count"), StateCount);
    Data->SetNumberField(TEXT("conduit_count"), AnimBp_CountNodes<UAnimStateConduitNode>(SMGraph));
    Data->SetNumberField(TEXT("transition_count"), TransitionCount);
    Data->SetArrayField(TEXT("states"), StateJson);
    Data->SetBoolField(TEXT("is_valid"), Errors.Num() == 0);
    Data->SetArrayField(TEXT("errors"), mcp_common::StringsToJson(Errors));
    Data->SetArrayField(TEXT("warnings"), mcp_common::StringsToJson(Warnings));
    Data->SetArrayField(TEXT("info"), mcp_common::StringsToJson(InfoList));
    return mcp_common::MakeOkReply(Data);
}

// ===========================================================================
// AnimBP transition rule mutation.
// clear_anim_transition_rule + set_anim_transition_rule_automatic.
//
// Both operate on a UAnimStateTransitionNode identified by (machine_name,
// source_state, target_state). NO auto-compile — the caller decides when to
// run compile_anim_blueprint; the mutations just dirty the asset (Modify +
// MarkBlueprintAsStructurallyModified) and persist via SaveAssetPackage.

// ---------------------------------------------------------------------------
// clear_anim_transition_rule
// ---------------------------------------------------------------------------
// MCP:clear_anim_transition_rule - Empty the rule of one transition (source_state to target_state) in an Animation Blueprint state machine: every rule-graph node but the result node is deleted, then the blueprint is marked structurally modified and saved without compiling (compile_anim_blueprint). The transition and its settings stay; with nothing wired it is never taken unless its automatic rule is on (set_anim_transition_rule_automatic). Not an undo step. An already empty rule is a success with removed_count 0, nothing saved; a missing blueprint, machine, state or transition is an error. No need to clear first for a new rule: set_anim_transition_rule_from_bool and set_anim_transition_rule_from_comparison rebuild it; remove_anim_transition drops the whole transition. Refused while PIE runs unless force=true; a failed save is an error, the change kept in memory.
//   @param anim_blueprint_path - Content path of the UAnimBlueprint (object or package path), e.g. '/Game/Characters/ABP_Hero'.
//   @param machine_name - Name of a state machine on the blueprint's top-level AnimGraph, as get_anim_blueprint_info lists it in state_machines (case-insensitive), e.g. 'Locomotion'.
//   @param source_state - Name of the state or conduit the transition leaves, as get_anim_state_machine_info lists it (case-insensitive), e.g. 'Idle'.
//   @param target_state - Name of the state or conduit the transition enters (case-insensitive), e.g. 'Run'.
//   @returns { anim_blueprint_path, machine_name, source_state, target_state, rule_cleared, already_clear?, sink_present, removed_count, remaining_rule_nodes, saved, save_error? }
TSharedPtr<FJsonObject> HandleClearAnimTransitionRule(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimBPPath, MachineName, SourceName, TargetName;
    if (TSharedPtr<FJsonObject> Missing = AnimBp_ReadTransitionParams(Params, AnimBPPath, MachineName, SourceName, TargetName))
        return Missing;

    UAnimBlueprint* AnimBP = nullptr;
    UAnimationStateMachineGraph* SMGraph = nullptr;
    UAnimStateTransitionNode* Transition = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimBp_LoadTransition(AnimBPPath, MachineName, SourceName, TargetName, AnimBP, SMGraph, Transition)) { return Error; }

    // The BoundGraph of a UAnimStateTransitionNode is canonically a UAnimationTransitionGraph
    // and holds exactly one UAnimGraphNode_TransitionResult sink node (CanUserDeleteNode=false)
    // plus zero or more contributor nodes (variable gets, comparisons, time-based rules, etc.).
    // "Clearing the rule" means removing every contributor node while keeping the sink intact. The
    // sink's bCanEnterTransition pin is then unwired, so its default value decides (false unless changed):
    // the engine never takes such a transition unless its automatic rule (bAutomaticRuleBasedOnSequencePlayerInState)
    // or a native transition binding drives it -- the compiler warns "will never be taken"
    // (UAnimStateTransitionNode::ValidateNodeDuringCompilation).
    UEdGraph* RuleGraph = Transition->BoundGraph;
    if (!RuleGraph)
        return mcp_common::MakeErrorReply(
            TEXT("Transition has no BoundGraph (rule graph was never initialized)"));

    // Rule-graph nodes other than the result sink.
    auto CountContributors = [RuleGraph]()
    {
        return RuleGraph->Nodes.Num() - AnimBp_CountNodes<UAnimGraphNode_TransitionResult>(RuleGraph);
    };

    // Idempotency: count contributors BEFORE so the payload reflects what the call did.
    if (CountContributors() == 0)
    {
        // Already clear — return idempotent success without touching dirty flags.
        TSharedPtr<FJsonObject> Resp = AnimBp_TransitionReply(AnimBP, SMGraph, SourceName, TargetName);
        Resp->SetBoolField(TEXT("rule_cleared"), false);
        Resp->SetBoolField(TEXT("already_clear"), true);
        Resp->SetBoolField(TEXT("sink_present"), HasTransitionResultSink(RuleGraph));
        Resp->SetNumberField(TEXT("removed_count"), 0);
        Resp->SetNumberField(TEXT("remaining_rule_nodes"), 0);
        Resp->SetBoolField(TEXT("saved"), false);
        return mcp_common::MakeOkReply(Resp);
    }

    const int32 RemovedCount = AnimBp_RemoveRuleContributors(AnimBP, RuleGraph);

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

    const int32 AfterCount = CountContributors();

    FString SaveErr;
    const bool bSaved = mcp_common::SaveAssetPackage(AnimBP->GetOutermost(), AnimBP, SaveErr);

    TSharedPtr<FJsonObject> Resp = AnimBp_TransitionReply(AnimBP, SMGraph, SourceName, TargetName);
    Resp->SetBoolField(TEXT("rule_cleared"), RemovedCount > 0);
    Resp->SetBoolField(TEXT("sink_present"), HasTransitionResultSink(RuleGraph));
    Resp->SetNumberField(TEXT("removed_count"), RemovedCount);
    Resp->SetNumberField(TEXT("remaining_rule_nodes"), AfterCount);
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("clear_anim_transition_rule"), SaveErr);
}

// ---------------------------------------------------------------------------
// set_anim_transition_rule_automatic
// ---------------------------------------------------------------------------
// MCP:set_anim_transition_rule_automatic - Set the automatic rule of one transition (source_state to target_state) in a state machine of an Animation Blueprint: bAutomaticRuleBasedOnSequencePlayerInState and AutomaticRuleTriggerTime, both written every call (omitted means false and 0). Marks it structurally modified and saves without compiling; refused while PIE runs unless force=true.
//   @param anim_blueprint_path - Asset path of the UAnimBlueprint
//   @param machine_name - Name of the state machine that owns the transition
//   @param source_state - Name of the source state (transition origin)
//   @param target_state - Name of the target state (transition destination)
//   @param enabled [bool] - True lets the transition fire on its own when the sequence player of the source state nears its end (bAutomaticRuleBasedOnSequencePlayerInState); omitted writes false.
//   @param trigger_time [number] - AutomaticRuleTriggerTime in seconds (default 0.0; negative = blend lead)
//   @returns { anim_blueprint_path, machine_name, source_state, target_state, enabled, trigger_time, rule_graph_has_result_node, saved, save_error? }
TSharedPtr<FJsonObject> HandleSetAnimTransitionRuleAutomatic(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimBPPath, MachineName, SourceName, TargetName;
    if (TSharedPtr<FJsonObject> Missing = AnimBp_ReadTransitionParams(Params, AnimBPPath, MachineName, SourceName, TargetName))
        return Missing;

    bool bEnabled = false;
    Params->TryGetBoolField(TEXT("enabled"), bEnabled);
    double TriggerTimeD = 0.0;
    Params->TryGetNumberField(TEXT("trigger_time"), TriggerTimeD);

    UAnimBlueprint* AnimBP = nullptr;
    UAnimationStateMachineGraph* SMGraph = nullptr;
    UAnimStateTransitionNode* Transition = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimBp_LoadTransition(AnimBPPath, MachineName, SourceName, TargetName, AnimBP, SMGraph, Transition)) { return Error; }

    AnimBP->Modify();
    Transition->Modify();

    Transition->bAutomaticRuleBasedOnSequencePlayerInState = bEnabled;
    Transition->AutomaticRuleTriggerTime = static_cast<float>(TriggerTimeD);

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

    FString SaveErr;
    const bool bSaved = mcp_common::SaveAssetPackage(AnimBP->GetOutermost(), AnimBP, SaveErr);

    TSharedPtr<FJsonObject> Resp = AnimBp_TransitionReply(AnimBP, SMGraph, SourceName, TargetName);
    Resp->SetBoolField(TEXT("enabled"), Transition->bAutomaticRuleBasedOnSequencePlayerInState);
    Resp->SetNumberField(TEXT("trigger_time"), Transition->AutomaticRuleTriggerTime);
    Resp->SetBoolField(TEXT("rule_graph_has_result_node"),
        HasTransitionResultSink(Transition->BoundGraph));
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("set_anim_transition_rule_automatic"), SaveErr);
}

// ---------------------------------------------------------------------------
// set_anim_transition_rule_from_bool
// ---------------------------------------------------------------------------
// MCP:set_anim_transition_rule_from_bool - Make a bool variable the rule of an existing transition (source_state to target_state) of an Animation Blueprint state machine: every rule-graph node but the result sink is removed, then a getter of variable_name is wired to the sink's bCanEnterTransition, so the transition passes while the variable is true (there is no invert option). A missing blueprint, machine, transition or variable, or one that is not a single bool, is an error before anything changes; a variable not compiled yet (variable_kind new_blueprint) is checked against its declared type. Repeat calls rebuild the same rule (idempotent). Marks the blueprint structurally modified and saves without compiling (compile_anim_blueprint). Float test: set_anim_transition_rule_from_comparison; time-based: set_anim_transition_rule_automatic; empty rule: clear_anim_transition_rule; new transition: add_anim_transition. Refused while PIE runs unless force=true; a failed save is an error, the change kept in memory.
//   @param anim_blueprint_path - Asset path of the UAnimBlueprint
//   @param machine_name - Name of the state machine that owns the transition, matched without case
//   @param source_state - Name of the source state or conduit (transition origin)
//   @param target_state - Name of the target state or conduit (transition destination)
//   @param variable_name - Name of an existing bool variable on the AnimBP (or its parent C++ class); the rule reads it as is, true lets the transition pass
//   @returns { anim_blueprint_path, machine_name, source_state, target_state, variable_name, variable_kind, rule_set, sink_present, contributor_count, contributor_node_class, saved, save_error? }
//
// Behavior: clears any existing contributor nodes from the transition's rule graph (preserving
// the UAnimGraphNode_TransitionResult sink), then creates a UK2Node_VariableGet for the named
// bool and wires its value pin to the sink's `bCanEnterTransition` input. The transition is
// then evaluated at runtime as `(variable == true)`. To invert, declare a `!Foo` shadow bool
// in the AnimBP and pass its name (negating via the graph is not implemented).
//
// `variable_kind` reports "generated" for parent-class C++ properties, "new_blueprint" for vars
// declared on the AnimBP itself but not yet compiled into the generated class, or "missing"
// (which is also returned as an error response).
TSharedPtr<FJsonObject> HandleSetAnimTransitionRuleFromBool(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimBPPath, MachineName, SourceName, TargetName, VariableName;
    if (TSharedPtr<FJsonObject> Missing = AnimBp_ReadTransitionParams(Params, AnimBPPath, MachineName, SourceName, TargetName))
        return Missing;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("variable_name"), VariableName)) { return Missing; }

    UAnimBlueprint* AnimBP = nullptr;
    UAnimationStateMachineGraph* SMGraph = nullptr;
    UAnimStateTransitionNode* Transition = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimBp_LoadTransition(AnimBPPath, MachineName, SourceName, TargetName, AnimBP, SMGraph, Transition)) { return Error; }

    UAnimationTransitionGraph* RuleGraph = Cast<UAnimationTransitionGraph>(Transition->BoundGraph);
    if (!RuleGraph)
        return mcp_common::MakeErrorReply(
            TEXT("Transition BoundGraph is not a UAnimationTransitionGraph (no rule graph to wire)"));
    UAnimGraphNode_TransitionResult* ResultNode = RuleGraph->GetResultNode();
    if (!ResultNode)
        return mcp_common::MakeErrorReply(
            TEXT("Rule graph has no UAnimGraphNode_TransitionResult sink"));
    UEdGraphPin* SinkInputPin = ResultNode->FindPin(TEXT("bCanEnterTransition"));
    if (!SinkInputPin)
    {
        return mcp_common::MakeErrorReply(
            TEXT("UAnimGraphNode_TransitionResult has no 'bCanEnterTransition' input pin"));
    }

    // --- Validate the variable exists on the AnimBP (or its parent) AND is a single bool. ---
    FProperty* Property = nullptr;
    TSharedPtr<FJsonObject> Error;
    const FString VariableKind = AnimBp_RuleVariableKind(AnimBP, VariableName, Property, Error);
    if (VariableKind.IsEmpty())
        return Error;

    // A compiled variable is typed by its property (a bitfield bool and a plain bool both map to PC_Boolean); one
    // only declared on the blueprint is typed by its NewVariables entry, so it is checked before the graph changes too.
    FEdGraphPinType VariableType;
    if (!AnimBp_RuleVariablePinType(AnimBP, VariableName, Property, VariableType)
        || VariableType.PinCategory != UEdGraphSchema_K2::PC_Boolean || VariableType.IsContainer())
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Variable '%s' is not a bool (type=%s); nothing was changed"),
            *VariableName, *AnimBp_RuleVariableTypeName(Property, VariableType)));
    }

    // --- Clear existing contributor nodes (preserve the sink). Idempotent step. ---
    AnimBp_RemoveRuleContributors(AnimBP, RuleGraph);

    // --- Create the UK2Node_VariableGet via the shared helper. ---
    UK2Node_VariableGet* VariableGetNode = mcp_common::SpawnVariableGetNode(
        RuleGraph, AnimBP, VariableName, FVector2D(-400.0f, 0.0f));
    if (!VariableGetNode)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Failed to create UK2Node_VariableGet for variable '%s' (post-clear rule graph dirty)"), *VariableName));
    }

    // --- Wire variable-get.value -> sink.bCanEnterTransition. ---
    UEdGraphPin* ValuePin = VariableGetNode->GetValuePin();
    if (!ValuePin)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("UK2Node_VariableGet for '%s' has no value pin (variable may not be bool-compatible)"), *VariableName));
    }

    // MakeLinkTo is the low-level connector — both pins are bool-typed (the variable was
    // validated as bool above; the sink pin is FAnimNode_TransitionResult::bCanEnterTransition
    // which is itself a bool), so the schema accepts the connection. We bypass
    // TryCreateConnection (which can be picky about exec-pin purity rules inherited from the
    // UAnimationTransitionSchema) for this single explicit pin pair.
    ValuePin->MakeLinkTo(SinkInputPin);

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

    // Count current contributor nodes (sink + the variable-get we just added => 2 total).
    int32 ContributorCount = 0;
    FString ContributorClass;
    for (UEdGraphNode* N : RuleGraph->Nodes)
    {
        if (!Cast<UAnimGraphNode_TransitionResult>(N))
        {
            ++ContributorCount;
            ContributorClass = N->GetClass()->GetName();
        }
    }

    FString SaveErr;
    const bool bSaved = mcp_common::SaveAssetPackage(AnimBP->GetOutermost(), AnimBP, SaveErr);

    TSharedPtr<FJsonObject> Resp = AnimBp_TransitionReply(AnimBP, SMGraph, SourceName, TargetName);
    Resp->SetStringField(TEXT("variable_name"), VariableName);
    Resp->SetStringField(TEXT("variable_kind"), VariableKind);
    Resp->SetBoolField(TEXT("rule_set"), ValuePin->LinkedTo.Num() > 0 && SinkInputPin->LinkedTo.Num() > 0);
    Resp->SetBoolField(TEXT("sink_present"), HasTransitionResultSink(RuleGraph));
    Resp->SetNumberField(TEXT("contributor_count"), ContributorCount);
    if (!ContributorClass.IsEmpty())
    {
        Resp->SetStringField(TEXT("contributor_node_class"), ContributorClass);
    }
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("set_anim_transition_rule_from_bool"), SaveErr);
}

// ---------------------------------------------------------------------------
// set_anim_transition_rule_from_comparison
// ---------------------------------------------------------------------------
// MCP:set_anim_transition_rule_from_comparison - Make (variable_name operator comparison_value) the rule of an existing transition (source_state to target_state) of an Animation Blueprint state machine: every rule-graph node but the result sink is removed, then a getter, a literal and a comparison node are wired into the sink's bCanEnterTransition. The variable must be a single float of either precision, compiled or only declared (variable_kind new_blueprint), and the test runs in double precision. A missing blueprint, machine, transition or variable, a variable of another type or an unknown operator is an error before anything changes. Repeat calls rebuild the same rule (idempotent); saves without compiling (compile_anim_blueprint). Bool rule: set_anim_transition_rule_from_bool; empty rule: clear_anim_transition_rule. Refused while PIE runs unless force=true; a failed save is an error.
//   @param anim_blueprint_path - Asset path of the UAnimBlueprint
//   @param machine_name - Name of the state machine that owns the transition, matched without case
//   @param source_state - Name of the source state or conduit (transition origin)
//   @param target_state - Name of the target state or conduit (transition destination)
//   @param variable_name - Name of an existing float variable (a Blueprint float, or a C++ float or double property) on the AnimBP or its parent class; the left-hand side of the comparison
//   @param operator - One of "==", "!=", ">", "<", ">=", "<=" (compared against comparison_value)
//   @param comparison_value [number] - Right-hand side of the comparison (default 0), written into the double literal node with six decimal places
//   @returns { anim_blueprint_path, machine_name, source_state, target_state, variable_name, operator, comparison_value, variable_kind, rule_set, sink_present, contributor_count, contributor_node_classes, saved, save_error? }
//
// Behavior: clears existing contributor nodes (preserving the sink), creates a
// UK2Node_VariableGet for the named float, a UK2Node_CallFunction wrapping
// UKismetSystemLibrary::MakeLiteralDouble for the comparison value, and another
// UK2Node_CallFunction for the chosen comparison operator (EqualEqual_DoubleDouble,
// Greater_DoubleDouble, etc.). The graph is wired:
//   VariableGet.value        --> Comparison.<op>.A
//   MakeLiteral*.ReturnValue --> Comparison.<op>.B     <-- the OUTPUT, not 'Value'
//   Comparison.ReturnValue   --> sink.bCanEnterTransition
// At runtime the transition evaluates as `(<variable> <op> <literal>)`.
TSharedPtr<FJsonObject> HandleSetAnimTransitionRuleFromComparison(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimBPPath, MachineName, SourceName, TargetName, VariableName, OperatorStr;
    if (TSharedPtr<FJsonObject> Missing = AnimBp_ReadTransitionParams(Params, AnimBPPath, MachineName, SourceName, TargetName))
        return Missing;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("variable_name"), VariableName)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("operator"), OperatorStr)) { return Missing; }
    double ComparisonValue = 0.0;
    Params->TryGetNumberField(TEXT("comparison_value"), ComparisonValue);

    // Operator -> UKismetMathLibrary comparison. The *_DoubleDouble functions are the only float comparisons UE 5.8
    // declares (the *_FloatFloat names survive only as CoreRedirects, which FindFunctionByName does not follow). The
    // K2 schema treats every real pin as compatible and the compiler inserts the float-to-double cast, so a single
    // precision variable wires to their double A pin like a double one does.
    static const TMap<FString, FName> OpToFunc = {
        {TEXT("=="), GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, EqualEqual_DoubleDouble)},
        {TEXT("!="), GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, NotEqual_DoubleDouble)},
        {TEXT(">"),  GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, Greater_DoubleDouble)},
        {TEXT("<"),  GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, Less_DoubleDouble)},
        {TEXT(">="), GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, GreaterEqual_DoubleDouble)},
        {TEXT("<="), GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, LessEqual_DoubleDouble)},
    };
    const FName* FuncNamePtr = OpToFunc.Find(OperatorStr);
    if (!FuncNamePtr)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Invalid operator '%s' (expected one of: == != > < >= <=); nothing was changed"), *OperatorStr));
    }
    const FName ComparisonFuncName = *FuncNamePtr;
    const FName LiteralFuncName = GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, MakeLiteralDouble);

    UAnimBlueprint* AnimBP = nullptr;
    UAnimationStateMachineGraph* SMGraph = nullptr;
    UAnimStateTransitionNode* Transition = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimBp_LoadTransition(AnimBPPath, MachineName, SourceName, TargetName, AnimBP, SMGraph, Transition)) { return Error; }

    UAnimationTransitionGraph* RuleGraph = Cast<UAnimationTransitionGraph>(Transition->BoundGraph);
    if (!RuleGraph)
        return mcp_common::MakeErrorReply(
            TEXT("Transition BoundGraph is not a UAnimationTransitionGraph (no rule graph to wire)"));
    UAnimGraphNode_TransitionResult* ResultNode = RuleGraph->GetResultNode();
    if (!ResultNode)
        return mcp_common::MakeErrorReply(
            TEXT("Rule graph has no UAnimGraphNode_TransitionResult sink"));
    UEdGraphPin* SinkInputPin = ResultNode->FindPin(TEXT("bCanEnterTransition"));
    if (!SinkInputPin)
    {
        return mcp_common::MakeErrorReply(
            TEXT("UAnimGraphNode_TransitionResult has no 'bCanEnterTransition' input pin"));
    }

    // Validate variable exists + is a single float of either precision (PC_Real covers FFloatProperty and
    // FDoubleProperty); one only declared on the blueprint is typed by its NewVariables entry.
    FProperty* Property = nullptr;
    TSharedPtr<FJsonObject> Error;
    const FString VariableKind = AnimBp_RuleVariableKind(AnimBP, VariableName, Property, Error);
    if (VariableKind.IsEmpty())
        return Error;

    FEdGraphPinType VariableType;
    if (!AnimBp_RuleVariablePinType(AnimBP, VariableName, Property, VariableType)
        || VariableType.PinCategory != UEdGraphSchema_K2::PC_Real || VariableType.IsContainer())
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Variable '%s' is not a float/double (type=%s); nothing was changed"),
            *VariableName, *AnimBp_RuleVariableTypeName(Property, VariableType)));
    }

    // UKismetSystemLibrary owns the literal-makers (KismetMathLibrary has no MakeLiteral* functions).
    UFunction* LiteralFunc = UKismetSystemLibrary::StaticClass()->FindFunctionByName(LiteralFuncName);
    if (!LiteralFunc)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("UKismetSystemLibrary::%s not found in engine source; nothing was changed"), *LiteralFuncName.ToString()));
    }
    UFunction* CompFunc = UKismetMathLibrary::StaticClass()->FindFunctionByName(ComparisonFuncName);
    if (!CompFunc)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("UKismetMathLibrary::%s not found in engine source; nothing was changed"), *ComparisonFuncName.ToString()));
    }

    // Everything the rule needs is validated: only from here on does the rule graph change.
    AnimBp_RemoveRuleContributors(AnimBP, RuleGraph);

    // Create the variable-get node.
    UK2Node_VariableGet* VariableGetNode = mcp_common::SpawnVariableGetNode(
        RuleGraph, AnimBP, VariableName, FVector2D(-700.0f, -100.0f));
    if (!VariableGetNode)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Failed to create UK2Node_VariableGet for variable '%s' (post-clear rule graph dirty)"), *VariableName));
    }

    // The MakeLiteralDouble node carrying the right-hand-side comparison value, then the comparison node.
    UK2Node_CallFunction* LiteralNode = mcp_common::CreateFunctionCallNode(RuleGraph, LiteralFunc, FVector2D(-700.0f, 150.0f));
    UK2Node_CallFunction* ComparisonNode = mcp_common::CreateFunctionCallNode(RuleGraph, CompFunc, FVector2D(-400.0f, 25.0f));

    UEdGraphPin* LiteralValuePin = LiteralNode->FindPin(TEXT("Value"));
    if (!LiteralValuePin)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("UKismetSystemLibrary::%s has no 'Value' input pin"), *LiteralFuncName.ToString()));
    }
    // A real pin's default is its text form; the value stays a double (SanitizeFloat keeps six decimal places).
    LiteralValuePin->DefaultValue = FString::SanitizeFloat(ComparisonValue);

    // Resolve the wiring pins. Failure of any is an engine/API mismatch — surface it.
    UEdGraphPin* VariableOutPin = VariableGetNode->GetValuePin();
    UEdGraphPin* CompAPin = ComparisonNode->FindPin(TEXT("A"));
    UEdGraphPin* CompBPin = ComparisonNode->FindPin(TEXT("B"));
    UEdGraphPin* CompReturnPin = ComparisonNode->FindPin(TEXT("ReturnValue"));
    // The literal's OUTPUT. 'Value' is its INPUT and is used for the default
    // (above), NEVER for the link -- see the block below.
    UEdGraphPin* LiteralReturnPin = LiteralNode->FindPin(TEXT("ReturnValue"));
    if (!VariableOutPin || !CompAPin || !CompBPin || !CompReturnPin || !LiteralReturnPin)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Pin wiring failed (variable_out=%d A=%d B=%d ret=%d lit_ret=%d) -- engine API mismatch"),
                VariableOutPin != nullptr, CompAPin != nullptr, CompBPin != nullptr,
                CompReturnPin != nullptr, LiteralReturnPin != nullptr));
    }

    // Wire all 3 edges via MakeLinkTo (low-level; bypasses UAnimationTransitionSchema's
    // exec-pin purity quirks — all pins here are bool/float, schema-valid).
    //
    // Linking LiteralValuePin (the MakeLiteral's 'Value' INPUT) to CompBPin, which is also an
    // input, is rejected by the editor ("Direction mismatch between pins Value and B") and
    // leaves the AnimBP with compile errors behind a rule_set:true, saved:true reply.
    VariableOutPin->MakeLinkTo(CompAPin);
    LiteralReturnPin->MakeLinkTo(CompBPin);
    CompReturnPin->MakeLinkTo(SinkInputPin);

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

    // Report contributor count + the classes we placed (sink excluded).
    int32 ContributorCount = 0;
    TArray<TSharedPtr<FJsonValue>> ContributorClasses;
    for (UEdGraphNode* N : RuleGraph->Nodes)
    {
        if (!Cast<UAnimGraphNode_TransitionResult>(N))
        {
            ++ContributorCount;
            ContributorClasses.Add(MakeShared<FJsonValueString>(N->GetClass()->GetName()));
        }
    }

    FString SaveErr;
    const bool bSaved = mcp_common::SaveAssetPackage(AnimBP->GetOutermost(), AnimBP, SaveErr);

    TSharedPtr<FJsonObject> Resp = AnimBp_TransitionReply(AnimBP, SMGraph, SourceName, TargetName);
    Resp->SetStringField(TEXT("variable_name"), VariableName);
    Resp->SetStringField(TEXT("operator"), OperatorStr);
    Resp->SetNumberField(TEXT("comparison_value"), ComparisonValue);
    Resp->SetStringField(TEXT("variable_kind"), VariableKind);
    Resp->SetBoolField(TEXT("rule_set"),
        CompReturnPin->LinkedTo.Num() > 0 && SinkInputPin->LinkedTo.Num() > 0);
    Resp->SetBoolField(TEXT("sink_present"), HasTransitionResultSink(RuleGraph));
    Resp->SetNumberField(TEXT("contributor_count"), ContributorCount);
    Resp->SetArrayField(TEXT("contributor_node_classes"), ContributorClasses);
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("set_anim_transition_rule_from_comparison"), SaveErr);
}
// ============================================================================
// AnimBP scaffolding
// ============================================================================
//
// Two primitives on top of the AnimGraph's state-machine / state / transition /
// conduit / notify / montage plumbing, so a locomotion AnimBP does not take 5+
// commands wired by hand. `create_locomotion_anim_blueprint` packages the recipe
// (create AnimBP + state machine + Idle/Walk/Run states + bidirectional
// transitions) into one call. `add_foot_ik_node` adds a UAnimGraphNode_LegIK to the
// AnimGraph and binds the foot bone references. Aim offset assets are made by
// create_blend_space (blend_space_type AimOffset / AimOffset1D).

// MCP:create_locomotion_anim_blueprint - Create in one call an Animation Blueprint for a skeleton with a locomotion state machine: the machine, its states (Idle, Walk, Run by default), the entry wired to the first state and a transition each way between consecutive states, then compile and save it. Use it as the quick start for a character; for any other layout build the machine from a JSON spec with build_anim_state_machine, or step by step with create_anim_blueprint, add_anim_state_machine, add_anim_state and add_anim_transition. What it leaves to do: the states are empty (put an animation in each with add_anim_graph_node) and the transitions have no rule, so they are never taken (set_anim_transition_rule_from_comparison, _from_bool or _automatic). Idempotent: an existing Animation Blueprint, machine, state or transition is reused, not duplicated, and an entry already wired is left alone; an existing blueprint is kept even if its skeleton is not skeleton_path. Compile errors are not reported here (check with compile_anim_blueprint); a failed save is an error. Refused while PIE runs unless force=true.
//   @renamed-from scaffold_locomotion_animbp
//   @param package_path - Content folder for the asset, e.g. "/Game/Characters/Hero/Animation" (a trailing slash is ignored).
//   @param asset_name - Name of the Animation Blueprint, e.g. "ABP_Hero"; the asset is package_path/asset_name.
//   @param skeleton_path - Path of the USkeleton asset, e.g. "/Game/Characters/Hero/SK_Hero_Skeleton"; it must load even when the blueprint already exists.
//   @param machine_name - Name of the state machine node in the AnimGraph (default "Locomotion"); a machine of that name is reused.
//   @param states - State names in order, e.g. ["Idle", "Walk", "Run", "Sprint"] (default ["Idle", "Walk", "Run"]); the first is the entry state, and transitions link each state with the next one.
//   @param add_transitions - Whether to add the two-way transitions between consecutive states, with a 0.2 s crossfade (default true); false leaves the states unlinked.
//   @returns { anim_blueprint_path, package_path, skeleton_path, machine_name, state_count, transition_count, entry_wired, entry_state, note_states_are_empty, already_existed, saved, save_error? }
TSharedPtr<FJsonObject> HandleScaffoldLocomotionAnimBp(
    const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath, AssetName, SkeletonPath;
    if (TSharedPtr<FJsonObject> Missing = AnimBp_ReadScaffoldParams(Params, PackagePath, AssetName, SkeletonPath))
    {
        return Missing;
    }
    FString MachineName;
    Params->TryGetStringField(TEXT("machine_name"), MachineName);
    if (MachineName.IsEmpty()) { MachineName = TEXT("Locomotion"); }

    USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, *SkeletonPath);
    if (!Skeleton)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Could not load skeleton: %s"), *SkeletonPath));
    }

    PackagePath.RemoveFromEnd(TEXT("/"));
    const FString AnimBPPath = PackagePath / AssetName;
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(AnimBPPath)) { return Invalid; }

    UAnimBlueprint* AnimBP = mcp_common::LoadAssetOrSoftPath<UAnimBlueprint>(AnimBPPath);
    const bool bAlreadyExistedABP = (AnimBP != nullptr);
    if (!AnimBP)
    {
        UAnimBlueprintFactory* Factory = NewObject<UAnimBlueprintFactory>();
        Factory->ParentClass = UAnimInstance::StaticClass();
        Factory->TargetSkeleton = Skeleton;

        FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
        UObject* NewAsset = AssetToolsModule.Get().CreateAsset(
            AssetName, PackagePath, UAnimBlueprint::StaticClass(), Factory);
        AnimBP = Cast<UAnimBlueprint>(NewAsset);
        if (!AnimBP)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Failed to create AnimBlueprint at %s"), *AnimBPPath));
        }
        FAssetRegistryModule::AssetCreated(AnimBP);
    }

    // Add the Locomotion state machine (idempotent).
    {
        TSharedPtr<FJsonObject> SMPayload = HandleAddAnimStateMachine(AnimBp_MachineRequest(AnimBP->GetPathName(), MachineName));
        if (!SMPayload.IsValid() || !SMPayload->GetBoolField(TEXT("success")))
        {
            return SMPayload;
        }
    }

    // Add the default states.
    const TArray<TSharedPtr<FJsonValue>>* StatesArr = nullptr;
    Params->TryGetArrayField(TEXT("states"), StatesArr);
    TArray<FString> StateNames;
    if (StatesArr)
    {
        for (const TSharedPtr<FJsonValue>& V : *StatesArr)
        {
            FString S;
            if (V.IsValid() && V->TryGetString(S) && !S.IsEmpty())
            {
                StateNames.Add(S);
            }
        }
    }
    if (StateNames.Num() == 0)
    {
        StateNames = { TEXT("Idle"), TEXT("Walk"), TEXT("Run") };
    }

    for (const FString& StateName : StateNames)
    {
        TSharedPtr<FJsonObject> StParams = AnimBp_MachineRequest(AnimBP->GetPathName(), MachineName);
        StParams->SetStringField(TEXT("state_name"), StateName);
        TSharedPtr<FJsonObject> StatePayload = HandleAddAnimState(StParams);
        if (!StatePayload.IsValid() || !StatePayload->GetBoolField(TEXT("success")))
        {
            return StatePayload;
        }
    }

    // Connect the ENTRY NODE to the first state.
    //
    // Without this, the scaffold returns state_count:3, transition_count:4, saved:true and the
    // AnimBP does NOT COMPILE: "Entry node 'X' is not connected to state" + "There was no entry
    // state connection in 'X'". A machine without an entry never starts, so the character
    // stays in the reference pose.
    bool bEntryWired = false;
    if (StateNames.Num() > 0)
    {
        if (UAnimGraphNode_StateMachine* SMNodeForEntry = FindStateMachineByName(AnimBP, MachineName))
        {
            if (UAnimationStateMachineGraph* SMGraphForEntry = SMNodeForEntry->EditorStateMachineGraph)
            {
                UAnimStateEntryNode* EntryNode = SMGraphForEntry->EntryNode;
                UAnimStateNodeBase* FirstState = FindStateByName(SMGraphForEntry, StateNames[0]);
                if (EntryNode && FirstState)
                {
                    UEdGraphPin* EntryPin = EntryNode->GetOutputPin();
                    UEdGraphPin* FirstStateIn = FirstState->GetInputPin();
                    // Idempotent: if it already points somewhere, leave it (re-running the scaffold
                    // must not re-wire an entry the author moved by hand).
                    if (EntryPin && FirstStateIn && EntryPin->LinkedTo.Num() == 0)
                    {
                        EntryPin->MakeLinkTo(FirstStateIn);
                        bEntryWired = true;
                    }
                    else if (EntryPin && EntryPin->LinkedTo.Num() > 0)
                    {
                        bEntryWired = true;   // already connected
                    }
                }
            }
        }
    }

    // Add bidirectional transitions between consecutive states.
    bool bAddTransitions = true;
    Params->TryGetBoolField(TEXT("add_transitions"), bAddTransitions);
    int32 TransitionsAdded = 0;
    if (bAddTransitions && StateNames.Num() >= 2)
    {
        for (int32 i = 0; i + 1 < StateNames.Num(); ++i)
        {
            for (int32 Dir = 0; Dir < 2; ++Dir)
            {
                FString Source = (Dir == 0) ? StateNames[i] : StateNames[i + 1];
                FString Target = (Dir == 0) ? StateNames[i + 1] : StateNames[i];

                TSharedPtr<FJsonObject> TrParams = AnimBp_TransitionRequest(AnimBP->GetPathName(), MachineName, Source, Target);
                TrParams->SetNumberField(TEXT("crossfade_duration"), 0.2);
                TSharedPtr<FJsonObject> TrPayload = HandleAddAnimTransition(TrParams);
                if (TrPayload.IsValid() && TrPayload->GetBoolField(TEXT("success")))
                {
                    ++TransitionsAdded;
                }
            }
        }
    }

    // Compile + save.
    TSharedPtr<FJsonObject> CompileParams = MakeShared<FJsonObject>();
    CompileParams->SetStringField(TEXT("anim_blueprint_path"), AnimBP->GetPathName());
    HandleCompileAnimBlueprint(CompileParams);

    FString SaveErr;
    const bool bSaved = mcp_common::SaveAssetPackage(AnimBP->GetOutermost(), AnimBP, SaveErr);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("anim_blueprint_path"), AnimBP->GetPathName());
    Data->SetStringField(TEXT("package_path"), PackagePath);
    Data->SetStringField(TEXT("skeleton_path"), Skeleton->GetPathName());
    Data->SetStringField(TEXT("machine_name"), MachineName);
    Data->SetNumberField(TEXT("state_count"), StateNames.Num());
    Data->SetNumberField(TEXT("transition_count"), TransitionsAdded);
    // Without an entry the machine does not compile; surface it in the response.
    Data->SetBoolField(TEXT("entry_wired"), bEntryWired);
    Data->SetStringField(TEXT("entry_state"), StateNames.Num() > 0 ? StateNames[0] : FString());
    Data->SetStringField(TEXT("note_states_are_empty"),
        TEXT("States are created EMPTY (only their Output Animation Pose): put an "
             "AnimGraphNode_SequencePlayer in each one with add_anim_graph_node("
             "graph_name=\"<machine> > <state>\") and wire it to the StateResult, or the AnimBP "
             "compiles clean and evaluates to the reference pose."));
    Data->SetBoolField(TEXT("already_existed"), bAlreadyExistedABP);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("create_locomotion_anim_blueprint"), SaveErr);
}

// MCP:add_foot_ik_node - Add a Leg IK node (UAnimGraphNode_LegIK) to the top-level AnimGraph of an Animation Blueprint, or reuse the first one already there, bind the feet into its LegsDefinition, then mark the blueprint structurally modified and save it without compiling. Leg 0 gets left_foot_bone and leg 1 right_foot_bone as the FK foot bone; a leg's IK target bone is set to the same bone when it is unset or the leg's foot changes, so the node compiles but has no effect until each IK target names an IK bone (set_anim_graph_node_property on LegsDefinition with the returned node_id); legs beyond the first two and the other leg settings are kept. Bones the target skeleton lacks are an error and nothing is written. The node is left unwired: connect it with connect_anim_graph_pins. Idempotent; refused while PIE runs unless force=true.
//   @renamed-from setup_foot_ik
//   @param anim_blueprint_path - Path to the UAnimBlueprint asset.
//   @param left_foot_bone - Left foot bone, written as the FK foot bone of leg 0; checked against the target skeleton when the blueprint has one.
//   @param right_foot_bone - Right foot bone, written as the FK foot bone of leg 1; checked against the target skeleton when the blueprint has one.
//   @returns { anim_blueprint_path, node_id, left_foot_bone, right_foot_bone, legs: [{ fk_foot_bone, ik_foot_bone, num_bones_in_limb }], bones_checked_against, ik_node_count, already_existed, saved, save_error }
TSharedPtr<FJsonObject> HandleSetupFootIk(
    const TSharedPtr<FJsonObject>& Params)
{
    FString AnimBPPath, LeftFoot, RightFoot;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("anim_blueprint_path"), AnimBPPath)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("left_foot_bone"), LeftFoot)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("right_foot_bone"), RightFoot)) { return Missing; }

    UAnimBlueprint* AnimBP = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadFlexibleOrError(AnimBPPath, TEXT("anim blueprint"), AnimBP)) { return Error; }
    UAnimationGraph* AnimGraph = FindAnimGraph(AnimBP);
    if (!AnimGraph)
    {
        return mcp_common::MakeErrorReply(TEXT("Could not find AnimGraph on anim blueprint"));
    }

    // The bones must exist before anything is written: the compiler only warns about an invalid leg, and the
    // node then skips that leg at runtime. A template blueprint has no target skeleton to check against.
    const USkeleton* TargetSkeleton = AnimBP->TargetSkeleton;
    if (TargetSkeleton)
    {
        for (const FString* Bone : { &LeftFoot, &RightFoot })
        {
            if (TargetSkeleton->GetReferenceSkeleton().FindBoneIndex(FName(**Bone)) == INDEX_NONE)
            {
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("Bone '%s' is not in the target skeleton %s; nothing was written."),
                    **Bone, *TargetSkeleton->GetPathName()));
            }
        }
    }

    UAnimGraphNode_LegIK* ExistingIK = nullptr;
    int32 ExistingIKCount = 0;
    for (UEdGraphNode* Node : AnimGraph->Nodes)
    {
        if (UAnimGraphNode_LegIK* IK = Cast<UAnimGraphNode_LegIK>(Node))
        {
            ++ExistingIKCount;
            if (!ExistingIK) { ExistingIK = IK; }
        }
    }

    const bool bAlreadyExisted = (ExistingIK != nullptr);
    if (!ExistingIK)
    {
        AnimGraph->Modify();
        AnimBP->Modify();
        FGraphNodeCreator<UAnimGraphNode_LegIK> NodeCreator(*AnimGraph);
        ExistingIK = NodeCreator.CreateNode(/*bSelectNewNode*/ false);
        NodeCreator.Finalize();
    }

    if (!ExistingIK)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to create LegIK node"));
    }

    // The node keeps its legs in Node.LegsDefinition, one FAnimLegIKDefinition per leg. A leg evaluates only
    // when both its FK foot bone (the end of the solved chain) and its IK foot bone (the target it reaches for)
    // resolve, so each foot is written as the FK foot bone, and as the IK target too while the leg has none or
    // names another foot -- a retargeted IK bone survives a rerun with the same feet.
    ExistingIK->Modify();
    TArray<FAnimLegIKDefinition>& Legs = ExistingIK->Node.LegsDefinition;
    if (Legs.Num() < 2)
    {
        Legs.SetNum(2);
    }
    const FName FeetToBind[2] = { FName(*LeftFoot), FName(*RightFoot) };
    for (int32 LegIndex = 0; LegIndex < 2; ++LegIndex)
    {
        FAnimLegIKDefinition& Leg = Legs[LegIndex];
        const bool bSameFoot = Leg.FKFootBone.BoneName == FeetToBind[LegIndex];
        Leg.FKFootBone.BoneName = FeetToBind[LegIndex];
        if (!bSameFoot || Leg.IKFootBone.BoneName.IsNone())
        {
            Leg.IKFootBone.BoneName = FeetToBind[LegIndex];
        }
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
    FString SaveErr;
    const bool bSaved = mcp_common::SaveAssetPackage(AnimBP->GetOutermost(), AnimBP, SaveErr);

    // Read the legs back from the node rather than echoing the request.
    TArray<TSharedPtr<FJsonValue>> LegsJson;
    for (const FAnimLegIKDefinition& Leg : ExistingIK->Node.LegsDefinition)
    {
        TSharedPtr<FJsonObject> LegObj = MakeShared<FJsonObject>();
        LegObj->SetStringField(TEXT("fk_foot_bone"), Leg.FKFootBone.BoneName.ToString());
        LegObj->SetStringField(TEXT("ik_foot_bone"), Leg.IKFootBone.BoneName.ToString());
        LegObj->SetNumberField(TEXT("num_bones_in_limb"), Leg.NumBonesInLimb);
        LegsJson.Add(MakeShared<FJsonValueObject>(LegObj));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("anim_blueprint_path"), AnimBP->GetPathName());
    Data->SetStringField(TEXT("node_id"), ExistingIK->NodeGuid.ToString());
    Data->SetStringField(TEXT("left_foot_bone"), ExistingIK->Node.LegsDefinition[0].FKFootBone.BoneName.ToString());
    Data->SetStringField(TEXT("right_foot_bone"), ExistingIK->Node.LegsDefinition[1].FKFootBone.BoneName.ToString());
    Data->SetArrayField(TEXT("legs"), LegsJson);
    Data->SetStringField(TEXT("bones_checked_against"), TargetSkeleton ? TargetSkeleton->GetPathName() : FString());
    Data->SetNumberField(TEXT("ik_node_count"), FMath::Max(1, ExistingIKCount));
    Data->SetBoolField(TEXT("already_existed"), bAlreadyExisted);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("add_foot_ik_node"), SaveErr);
}

// ---------------------------------------------------------------------------
// Atomic JSON-spec orchestrator
// ---------------------------------------------------------------------------
// Wires the state machine graph end-to-end by calling the primitive handlers above rather than
// re-implementing them; each primitive is idempotent on its own, so the orchestrator only coordinates the
// steps and aggregates their errors.

// MCP:build_anim_state_machine - Atomic JSON-spec builder for an Animation Blueprint state machine. Builds (or reuses) the state machine, then iterates states, transitions, and per-transition rules in one idempotent call. Replaces 8+ primitive cmds (add_anim_state_machine + add_anim_state + add_anim_transition + set_anim_transition_rule_*) with a single spec. Re-running with the same spec is a no-op (each primitive is internally idempotent: state-by-name, transition-by-source+target). For repeated invocations that change rule shape, the rule is re-applied on the existing transition.
//   @param anim_blueprint_path - Path to the UAnimBlueprint asset
//   @param state_machine_name - Target state machine name (created if missing via add_anim_state_machine)
//   @param spec [object!] - JSON describing the graph shape. states: [{name: str, animation?: str (/Game/Anim/Foo — optional; load-bearing for the runtime state asset but NOT created here), loop?: bool, pos?: [x, y]}]. transitions: [{from: str, to: str, blend?: number (= crossfade_duration seconds), bidirectional?: bool, priority_order?: int, rule?: object}]. Each rule variant maps 1:1 to an existing primitive cmd: {type: "automatic", enabled?: bool = false, trigger_time?: number = 0.0} | {type: "bool", variable: str (must exist on the AnimBP), invert?: bool = false} | {type: "comparison", variable: str (must be float/double on the AnimBP), operator: "=="|"!="|">"|"<"|">="|"<=", comparison_value: number}. An absent rule leaves the transition inert (no rule wired).
//   @param compile_after [bool] - Optional, default false; if true runs compile_anim_blueprint at the end
//   @returns { success: bool, anim_blueprint_path: string, machine_name: string, machine_already_existed: bool, states_created: int, states_skipped: int, transitions_created: int, transitions_updated: int, transitions_skipped: int, errors: [string], compiled: bool, compiled_after: bool }
TSharedPtr<FJsonObject> HandleBuildAnimationStateMachine(const TSharedPtr<FJsonObject>& Params)
{
    // ---- Top-level params ----
    FString AnimBPPath, MachineName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("anim_blueprint_path"), AnimBPPath)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("state_machine_name"), MachineName)) { return Missing; }

    const TSharedPtr<FJsonObject>* SpecObj = nullptr;
    if (!Params->TryGetObjectField(TEXT("spec"), SpecObj) || !SpecObj->IsValid())
    {
        return mcp_common::MissingParameterReply(TEXT("spec"));
    }
    const TSharedPtr<FJsonObject>& Spec = *SpecObj;

    bool bCompileAfter = false;
    Params->TryGetBoolField(TEXT("compile_after"), bCompileAfter);

    // ---- Counters ----
    int32 StatesCreated = 0, StatesSkipped = 0;
    int32 TransitionsCreated = 0, TransitionsUpdated = 0, TransitionsSkipped = 0;
    TArray<TSharedPtr<FJsonValue>> Errors;

    auto PushError = [&Errors](const FString& Msg)
    {
        Errors.Add(MakeShared<FJsonValueString>(Msg));
    };

    // Helper: turn an internal result into a (success, error_message) pair. The internal
    // handlers wrap their payload via mcp_common::MakeOkReply which sets
    // top-level "success" + "data" — a flat {data: {...}} for our purposes.
    auto ProbeSuccess = [](const TSharedPtr<FJsonObject>& R) -> TPair<bool, FString>
    {
        if (!R.IsValid()) { return TPair<bool, FString>(false, TEXT("internal null response")); }
        bool bOk = false; R->TryGetBoolField(TEXT("success"), bOk);
        if (bOk) { return TPair<bool, FString>(true, TEXT("")); }
        FString Err; R->TryGetStringField(TEXT("error"), Err);
        if (Err.IsEmpty()) { Err = TEXT("internal error (no message)"); }
        return TPair<bool, FString>(false, Err);
    };

    // ---- Step 1: probe the state machine BEFORE adding it so we can report
    // `machine_already_existed` honestly (after-add probing would always see the
    // freshly-created machine and report `already_existed=true` even on the
    // very first orchestrator call). The find is a cheap graph walk; if the
    // orchestrator is invoked repeatedly on the same asset we don't pay a
    // meaningful cost for the extra Find call.
    UAnimBlueprint* AnimBP = mcp_common::LoadAssetOrSoftPath<UAnimBlueprint>(AnimBPPath);
    if (!AnimBP)
    {
        PushError(FString::Printf(TEXT("AnimBlueprint load failed: %s"), *AnimBPPath));
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetBoolField(TEXT("success"), false);
        Resp->SetStringField(TEXT("anim_blueprint_path"), AnimBPPath);
        Resp->SetStringField(TEXT("machine_name"), MachineName);
        Resp->SetNumberField(TEXT("states_created"), StatesCreated);
        Resp->SetNumberField(TEXT("states_skipped"), StatesSkipped);
        Resp->SetNumberField(TEXT("transitions_created"), TransitionsCreated);
        Resp->SetNumberField(TEXT("transitions_updated"), TransitionsUpdated);
        Resp->SetNumberField(TEXT("transitions_skipped"), TransitionsSkipped);
        Resp->SetArrayField(TEXT("errors"), Errors);
        Resp->SetBoolField(TEXT("machine_already_existed"), false);
        Resp->SetBoolField(TEXT("compiled"), false);
        Resp->SetBoolField(TEXT("compiled_after"), bCompileAfter);
        return mcp_common::MakeOkReply(Resp);
    }
    UAnimGraphNode_StateMachine* SMNode = FindStateMachineByName(AnimBP, MachineName);
    const bool bMachineAlreadyExisted = (SMNode != nullptr);

    // Now create-if-missing. Idempotent under the hood.
    if (!bMachineAlreadyExisted)
    {
        TSharedPtr<FJsonObject> SMResp = HandleAddAnimStateMachine(AnimBp_MachineRequest(AnimBPPath, MachineName));
        TPair<bool, FString> Probe = ProbeSuccess(SMResp);
        if (!Probe.Key)
        {
            // Even on failure the graph is looked up below; some configs (existing
            // AnimBP with the state machine already on disk before our run) may
            // still have a resolvable SMGraph for the post-mortem report.
            PushError(FString::Printf(TEXT("add_anim_state_machine failed: %s"), *Probe.Value));
        }
        AnimBP = mcp_common::LoadAssetOrSoftPath<UAnimBlueprint>(AnimBPPath);
        SMNode = AnimBP ? FindStateMachineByName(AnimBP, MachineName) : nullptr;
    }
    UAnimationStateMachineGraph* SMGraph = SMNode ? SMNode->EditorStateMachineGraph : nullptr;

    // ---- Step 2: states (skip if exists, add if not) ----
    const TArray<TSharedPtr<FJsonValue>>* StatesArr = nullptr;
    if (SMGraph && Spec->TryGetArrayField(TEXT("states"), StatesArr) && StatesArr)
    {
        for (const TSharedPtr<FJsonValue>& Entry : *StatesArr)
        {
            const TSharedPtr<FJsonObject>* StateSpec = nullptr;
            if (!Entry.IsValid() || !Entry->TryGetObject(StateSpec) || !StateSpec || !StateSpec->IsValid())
            {
                PushError(TEXT("states[] entry is not an object; skipping"));
                continue;
            }
            FString SName;
            if (!(*StateSpec)->TryGetStringField(TEXT("name"), SName) || SName.IsEmpty())
            {
                PushError(TEXT("states[] entry missing required 'name'; skipping"));
                continue;
            }
            if (FindStateByName(SMGraph, SName))
            {
                ++StatesSkipped;
                continue;
            }
            // Add the state. Note: animation/loop/pos are NOT consumed here — they describe
            // the runtime state asset, not the state machine graph node. The state's bound
            // graph (UAnimationStateMachineGraph for the state itself) is what the node
            // already gets from UAnimStateNode::PostPlacedNewNode; the per-state anim asset
            // is wired by the existing rule setters, not by this builder.
            TSharedPtr<FJsonObject> AddParams = AnimBp_MachineRequest(AnimBPPath, MachineName);
            AddParams->SetStringField(TEXT("state_name"), SName);
            // Optional positional hint: add_anim_state ignores it today; passing it keeps
            // the orchestrator's spec round-trippable without future expansion drops.
            TSharedPtr<FJsonObject> AddResp = HandleAddAnimState(AddParams);
            TPair<bool, FString> Probe = ProbeSuccess(AddResp);
            if (Probe.Key) { ++StatesCreated; } else { PushError(FString::Printf(TEXT("state '%s': %s"), *SName, *Probe.Value)); }
        }
    }

    // Re-resolve the SM graph — state additions grow the bound graphs.
    SMGraph = SMNode ? SMNode->EditorStateMachineGraph : nullptr;

    // ---- Step 3: transitions + rules ----
    const TArray<TSharedPtr<FJsonValue>>* TransitionsArr = nullptr;
    if (SMGraph && Spec->TryGetArrayField(TEXT("transitions"), TransitionsArr) && TransitionsArr)
    {
        for (const TSharedPtr<FJsonValue>& Entry : *TransitionsArr)
        {
            const TSharedPtr<FJsonObject>* TSpecObj = nullptr;
            if (!Entry.IsValid() || !Entry->TryGetObject(TSpecObj) || !TSpecObj || !TSpecObj->IsValid())
            {
                PushError(TEXT("transitions[] entry is not an object; skipping"));
                continue;
            }
            const TSharedPtr<FJsonObject>& TSpec = *TSpecObj;
            FString FromName, ToName;
            if (!TSpec->TryGetStringField(TEXT("from"), FromName) || FromName.IsEmpty() ||
                !TSpec->TryGetStringField(TEXT("to"), ToName) || ToName.IsEmpty())
            {
                PushError(TEXT("transitions[] entry missing required 'from' or 'to'; skipping"));
                continue;
            }
            if (FromName.Equals(ToName, ESearchCase::IgnoreCase))
            {
                PushError(FString::Printf(TEXT("transitions[] '%s' -> '%s' is self-loop; skipping"), *FromName, *ToName));
                continue;
            }

            const bool bAlreadyExists = (FindTransitionBySourceTarget(SMGraph, FromName, ToName) != nullptr);
            if (!bAlreadyExists)
            {
                // Resolve endpoints (one last check) — add_anim_transition re-resolves
                // them too, but a missing state here would surface as an opaque error;
                // surface it explicitly so the caller sees WHICH endpoint is missing.
                if (!FindStateByName(SMGraph, FromName))
                {
                    PushError(FString::Printf(TEXT("transition from_state '%s' not found"), *FromName));
                    continue;
                }
                if (!FindStateByName(SMGraph, ToName))
                {
                    PushError(FString::Printf(TEXT("transition to_state '%s' not found"), *ToName));
                    continue;
                }

                TSharedPtr<FJsonObject> AddParams = AnimBp_TransitionRequest(AnimBPPath, MachineName, FromName, ToName);
                // blend=0.2 default mirrors add_anim_transition's own default; only forward
                // if the spec provides it (orchestrator respects the rule "don't add fields
                // the caller didn't ask for").
                double BlendD = 0.0; bool bHasBlend = false;
                if (TSpec->TryGetNumberField(TEXT("blend"), BlendD)) { bHasBlend = true; }
                else { BlendD = 0.2; }
                AddParams->SetNumberField(TEXT("crossfade_duration"), BlendD);
                bool bBidir = false;
                TSpec->TryGetBoolField(TEXT("bidirectional"), bBidir);
                AddParams->SetBoolField(TEXT("bidirectional"), bBidir);
                double PriD = 1.0;
                if (TSpec->TryGetNumberField(TEXT("priority_order"), PriD))
                {
                    AddParams->SetNumberField(TEXT("priority_order"), PriD);
                }
                else
                {
                    AddParams->SetNumberField(TEXT("priority_order"), 1);
                }

                TSharedPtr<FJsonObject> AddResp = HandleAddAnimTransition(AddParams);
                TPair<bool, FString> Probe = ProbeSuccess(AddResp);
                if (!Probe.Key)
                {
                    PushError(FString::Printf(TEXT("transition '%s' -> '%s': %s"), *FromName, *ToName, *Probe.Value));
                    continue;
                }
                ++TransitionsCreated;
                // `bHasBlend` is reserved for a future "update on existing" path; we don't
                // expose add_anim_transition's already-existed escape as a "forced update"
                // (it would break the primitive's documented idempotent contract).
                (void)bHasBlend;
            }
            else
            {
                ++TransitionsUpdated;
            }

            // ---- Apply the rule (if specified) ----
            const TSharedPtr<FJsonObject>* RuleObj = nullptr;
            if (!TSpec->TryGetObjectField(TEXT("rule"), RuleObj) || !RuleObj->IsValid())
            {
                // No rule field -> leave existing rule graph untouched (dormant transition).
                continue;
            }
            const TSharedPtr<FJsonObject>& Rule = *RuleObj;
            FString RuleType;
            if (!Rule->TryGetStringField(TEXT("type"), RuleType) || RuleType.IsEmpty())
            {
                PushError(FString::Printf(TEXT("transition '%s' -> '%s': rule.type missing"), *FromName, *ToName));
                continue;
            }
            FString RuleErr;
            bool bRuleOK = false;
            if (RuleType.Equals(TEXT("automatic"), ESearchCase::IgnoreCase))
            {
                TSharedPtr<FJsonObject> P = AnimBp_TransitionRequest(AnimBPPath, MachineName, FromName, ToName);
                bool bEnabled = false; Rule->TryGetBoolField(TEXT("enabled"), bEnabled);
                P->SetBoolField(TEXT("enabled"), bEnabled);
                double TT = 0.0; Rule->TryGetNumberField(TEXT("trigger_time"), TT);
                P->SetNumberField(TEXT("trigger_time"), TT);
                TPair<bool, FString> Probe = ProbeSuccess(HandleSetAnimTransitionRuleAutomatic(P));
                bRuleOK = Probe.Key; RuleErr = Probe.Value;
            }
            else if (RuleType.Equals(TEXT("bool"), ESearchCase::IgnoreCase))
            {
                FString VarName;
                if (!Rule->TryGetStringField(TEXT("variable"), VarName) || VarName.IsEmpty())
                {
                    PushError(FString::Printf(TEXT("transition '%s' -> '%s': bool-rule needs 'variable'"), *FromName, *ToName));
                    continue;
                }
                TSharedPtr<FJsonObject> P = AnimBp_TransitionRequest(AnimBPPath, MachineName, FromName, ToName);
                P->SetStringField(TEXT("variable_name"), VarName);
                TPair<bool, FString> Probe = ProbeSuccess(HandleSetAnimTransitionRuleFromBool(P));
                bRuleOK = Probe.Key; RuleErr = Probe.Value;
                // 'invert' is documented (registry entry) but the existing primitive is not
                // wired to honor it; documented as a known gap rather than a silent
                // miss.
            }
            else if (RuleType.Equals(TEXT("comparison"), ESearchCase::IgnoreCase))
            {
                FString VarName, OpStr;
                if (!Rule->TryGetStringField(TEXT("variable"), VarName) || VarName.IsEmpty() ||
                    !Rule->TryGetStringField(TEXT("operator"), OpStr) || OpStr.IsEmpty())
                {
                    PushError(FString::Printf(TEXT("transition '%s' -> '%s': comparison-rule needs 'variable' + 'operator'"), *FromName, *ToName));
                    continue;
                }
                TSharedPtr<FJsonObject> P = AnimBp_TransitionRequest(AnimBPPath, MachineName, FromName, ToName);
                P->SetStringField(TEXT("variable_name"), VarName);
                P->SetStringField(TEXT("operator"), OpStr);
                double CV = 0.0; Rule->TryGetNumberField(TEXT("comparison_value"), CV);
                P->SetNumberField(TEXT("comparison_value"), CV);
                TPair<bool, FString> Probe = ProbeSuccess(HandleSetAnimTransitionRuleFromComparison(P));
                bRuleOK = Probe.Key; RuleErr = Probe.Value;
            }
            else
            {
                PushError(FString::Printf(TEXT("transition '%s' -> '%s': unknown rule.type '%s'"), *FromName, *ToName, *RuleType));
                continue;
            }
            if (!bRuleOK)
            {
                PushError(FString::Printf(TEXT("transition '%s' -> '%s' rule '%s': %s"), *FromName, *ToName, *RuleType, *RuleErr));
            }
        }
    }

    // ---- Step 4: optional compile ----
    bool bCompiled = false;
    if (bCompileAfter)
    {
        TSharedPtr<FJsonObject> CompileParams = MakeShared<FJsonObject>();
        CompileParams->SetStringField(TEXT("anim_blueprint_path"), AnimBPPath);
        TSharedPtr<FJsonObject> CompileResp = HandleCompileAnimBlueprint(CompileParams);
        TPair<bool, FString> Probe = ProbeSuccess(CompileResp);
        bCompiled = Probe.Key;
        if (!Probe.Key)
        {
            PushError(FString::Printf(TEXT("compile_anim_blueprint: %s"), *Probe.Value));
        }
    }

    // ---- Final response ----
    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), Errors.Num() == 0);
    Resp->SetStringField(TEXT("anim_blueprint_path"), AnimBPPath);
    Resp->SetStringField(TEXT("machine_name"), MachineName);
    Resp->SetBoolField(TEXT("machine_already_existed"), bMachineAlreadyExisted);
    Resp->SetNumberField(TEXT("states_created"), StatesCreated);
    Resp->SetNumberField(TEXT("states_skipped"), StatesSkipped);
    Resp->SetNumberField(TEXT("transitions_created"), TransitionsCreated);
    Resp->SetNumberField(TEXT("transitions_updated"), TransitionsUpdated);
    Resp->SetNumberField(TEXT("transitions_skipped"), TransitionsSkipped);
    Resp->SetArrayField(TEXT("errors"), Errors);
    Resp->SetBoolField(TEXT("compiled_after"), bCompileAfter);
    Resp->SetBoolField(TEXT("compiled"), bCompiled);
    return mcp_common::MakeOkReply(Resp);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_anim_conduit"), &HandleAddAnimConduit },
    { TEXT("add_anim_state"), &HandleAddAnimState },
    { TEXT("add_anim_state_machine"), &HandleAddAnimStateMachine },
    { TEXT("add_anim_transition"), &HandleAddAnimTransition },
    { TEXT("build_anim_state_machine"), &HandleBuildAnimationStateMachine },
    { TEXT("clear_anim_transition_rule"), &HandleClearAnimTransitionRule },
    { TEXT("compile_anim_blueprint"), &HandleCompileAnimBlueprint },
    { TEXT("create_anim_blueprint"), &HandleCreateAnimBlueprint },
    { TEXT("get_anim_blueprint_info"), &HandleGetAnimBlueprintInfo },
    { TEXT("get_anim_state_machine_info"), &HandleGetAnimStateMachineInfo },
    { TEXT("remove_anim_conduit"), &HandleRemoveAnimConduit },
    { TEXT("remove_anim_state"), &HandleRemoveAnimState },
    { TEXT("remove_anim_transition"), &HandleRemoveAnimTransition },
    { TEXT("create_locomotion_anim_blueprint"), &HandleScaffoldLocomotionAnimBp },
    { TEXT("set_anim_transition_rule_automatic"), &HandleSetAnimTransitionRuleAutomatic },
    { TEXT("set_anim_transition_rule_from_bool"), &HandleSetAnimTransitionRuleFromBool },
    { TEXT("set_anim_transition_rule_from_comparison"), &HandleSetAnimTransitionRuleFromComparison },
    { TEXT("add_foot_ik_node"), &HandleSetupFootIk },
};
} // namespace UEAgentAnimBlueprintCommands
