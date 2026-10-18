#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

#include "EditorAssetLibrary.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "Kismet2/KismetEditorUtilities.h"

// Control Rig blueprint (ControlRigDeveloper). UControlRigBlueprint : URigVMBlueprint.
#include "ControlRigBlueprintLegacy.h"

// RigVM model + controller (RigVMDeveloper). GetController/GetDefaultModel are
// inherited on URigVMBlueprint; the model read API lives in these headers.
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMNode.h"
#include "RigVMModel/RigVMPin.h"
#include "RigVMModel/RigVMLink.h"
#include "RigVMModel/Nodes/RigVMUnitNode.h"

// Rig unit structs (ControlRig module). StaticStruct() of each is the node type
// passed to URigVMController::AddUnitNode.
#include "Units/Execution/RigUnit_BeginExecution.h"
#include "Units/Execution/RigUnit_PrepareForExecution.h"
#include "Units/Hierarchy/RigUnit_GetTransform.h"
#include "Units/Hierarchy/RigUnit_SetTransform.h"
#include "Units/Highlevel/Hierarchy/RigUnit_AimBone.h"
#include "Units/Highlevel/Hierarchy/RigUnit_TwoBoneIKSimple.h"

/**
 * Control Rig RigVM solver-graph authoring commands.
 *
 * They EXTEND the Control Rig hierarchy commands (UEAgentControlRigCommands) with the RigVM logic graph that
 * actually solves the rig, authoring the graph entirely headless via the engine's own scripting path:
 *   - UControlRigBlueprint (ControlRigBlueprintLegacy.h) is a URigVMBlueprint (RigVMBlueprintLegacy.h,
 *     RigVMDeveloper), loaded with mcp_common::LoadAssetOrSoftPath like control_rig_path elsewhere.
 *   - URigVMController* Controller = CRBP->GetController();  // controller for the
 *     default model (GetController/GetDefaultModel inherited from URigVMBlueprint).
 *   - URigVMGraph* Model = CRBP->GetDefaultModel();          // for reads.
 *   - Controller->AddUnitNode(UScriptStruct*, "Execute", Pos, Name) adds a rig unit
 *     node; SetPinDefaultValue / AddLink / RemoveNode mutate the graph.
 * Each mutation recompiles the blueprint (FKismetEditorUtilities::CompileBlueprint)
 * and persists the package to disk, then reads the model back for verification.
 *
 * Supported rig-unit node types (name -> UScriptStruct via StaticStruct()):
 *   "BeginExecution"      FRigUnit_BeginExecution      (Units/Execution)
 *   "PrepareForExecution" FRigUnit_PrepareForExecution (Units/Execution)
 *   "GetTransform"        FRigUnit_GetTransform        (Units/Hierarchy)
 *   "SetTransform"        FRigUnit_SetTransform        (Units/Hierarchy)
 *   "AimBone"             FRigUnit_AimBone             (Units/Highlevel/Hierarchy)
 *   "TwoBoneIKSimple"     FRigUnit_TwoBoneIKSimple     (Units/Highlevel/Hierarchy)
 */
namespace UEAgentRigVMCommands
{

// MCP-DOMAIN: Animation

namespace
{
    /** The control rig blueprint at RigPath. Null on success, else the error reply. */
    TSharedPtr<FJsonObject> RigVM_LoadBlueprint(const FString& RigPath, UControlRigBlueprint*& OutBlueprint)
    {
        OutBlueprint = mcp_common::LoadAssetOrSoftPath<UControlRigBlueprint>(RigPath);
        if (!OutBlueprint)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Could not load control rig blueprint: %s"), *RigPath));
        }
        return nullptr;
    }

    /** The control rig blueprint at RigPath and its RigVM controller. Null on success, else the error reply. */
    TSharedPtr<FJsonObject> RigVM_LoadController(const FString& RigPath, UControlRigBlueprint*& OutBlueprint,
        URigVMController*& OutController)
    {
        if (TSharedPtr<FJsonObject> Error = RigVM_LoadBlueprint(RigPath, OutBlueprint))
        {
            return Error;
        }
        OutController = OutBlueprint->GetController();
        if (!OutController)
        {
            return mcp_common::MakeErrorReply(TEXT("Could not get RigVM controller for the control rig"));
        }
        return nullptr;
    }

    /** Recompiles the rig blueprint, then saves its package; true when the package reached disk. */
    bool RigVM_CompileAndSave(UControlRigBlueprint* CRBP, FString& OutSaveError)
    {
        FKismetEditorUtilities::CompileBlueprint(CRBP, EBlueprintCompileOptions::SkipGarbageCollection);
        return mcp_common::SaveAssetPackage(CRBP->GetOutermost(), CRBP, OutSaveError);
    }

    /** A reply that opens with the rig blueprint's asset_path. */
    TSharedPtr<FJsonObject> RigVM_StartReply(const UControlRigBlueprint* CRBP)
    {
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetStringField(TEXT("asset_path"), CRBP->GetPathName());
        return Resp;
    }

    /** Map a supported node-type label to its rig-unit UScriptStruct (nullptr if unknown). */
    UScriptStruct* RigVMNodeTypeToStruct(const FString& In)
    {
        const FString Lower = In.ToLower();
        if (Lower == TEXT("beginexecution") ||
            Lower == TEXT("forwardssolve") ||
            Lower == TEXT("forwardsolve"))      return FRigUnit_BeginExecution::StaticStruct();
        if (Lower == TEXT("prepareforexecution") ||
            Lower == TEXT("construction") ||
            Lower == TEXT("setup"))             return FRigUnit_PrepareForExecution::StaticStruct();
        if (Lower == TEXT("gettransform")) { return FRigUnit_GetTransform::StaticStruct(); }
        if (Lower == TEXT("settransform")) { return FRigUnit_SetTransform::StaticStruct(); }
        if (Lower == TEXT("aimbone") ||
            Lower == TEXT("aim"))               return FRigUnit_AimBone::StaticStruct();
        if (Lower == TEXT("twoboneiksimple") ||
            Lower == TEXT("twoboneik"))         return FRigUnit_TwoBoneIKSimple::StaticStruct();
        return nullptr;
    }

    /** Direction enum -> stable label. */
    FString PinDirectionToString(ERigVMPinDirection Dir)
    {
        switch (Dir)
        {
            case ERigVMPinDirection::Input:   return TEXT("Input");
            case ERigVMPinDirection::Output:  return TEXT("Output");
            case ERigVMPinDirection::IO:      return TEXT("IO");
            case ERigVMPinDirection::Visible: return TEXT("Visible");
            case ERigVMPinDirection::Hidden:  return TEXT("Hidden");
            default:                          return TEXT("Invalid");
        }
    }

    /**
     * Serialize one URigVMNode into a JSON object: name, title, type, pin_count
     * plus a pins[] array of {name, path, direction, is_execute} so callers can
     * discover the exact pin paths (e.g. the execution pin) for wiring AddLink.
     */
    TSharedPtr<FJsonObject> NodeToJson(URigVMNode* Node)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        if (!Node)
        {
            return Obj;
        }
        Obj->SetStringField(TEXT("name"), Node->GetName());
        Obj->SetStringField(TEXT("title"), Node->GetNodeTitle());

        FString TypeName;
        if (URigVMUnitNode* UnitNode = Cast<URigVMUnitNode>(Node))
        {
            if (UScriptStruct* SS = UnitNode->GetScriptStruct())
            {
                TypeName = SS->GetName();
            }
        }
        if (TypeName.IsEmpty())
        {
            TypeName = Node->GetClass()->GetName();
        }
        Obj->SetStringField(TEXT("type"), TypeName);

        const TArray<URigVMPin*>& Pins = Node->GetPins();
        Obj->SetNumberField(TEXT("pin_count"), Pins.Num());

        TArray<TSharedPtr<FJsonValue>> PinArr;
        for (URigVMPin* Pin : Pins)
        {
            if (!Pin)
            {
                continue;
            }
            TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
            PinObj->SetStringField(TEXT("name"), Pin->GetName());
            PinObj->SetStringField(TEXT("path"), Pin->GetPinPath(/*bUseNodePath*/ true));
            PinObj->SetStringField(TEXT("direction"), PinDirectionToString(Pin->GetDirection()));
            PinObj->SetBoolField(TEXT("is_execute"), Pin->IsExecuteContext());
            PinArr.Add(MakeShared<FJsonValueObject>(PinObj));
        }
        Obj->SetArrayField(TEXT("pins"), PinArr);
        return Obj;
    }
}

// ---------------------------------------------------------------------------
// add_rigvm_node
// ---------------------------------------------------------------------------
// MCP:add_rigvm_node - Add a rig-unit node to a Control Rig's RigVM solver graph
//   @param control_rig_path - Path to the UControlRigBlueprint asset
//   @param node_type - BeginExecution, PrepareForExecution, GetTransform, SetTransform, AimBone, or TwoBoneIKSimple
//   @param node_name - Optional explicit node name
//   @param position - Optional [x, y] editor graph position
//   @returns { asset_path: string, node_name: string, node_title: string, node_type: string, pin_count: int, node_count: int, saved: bool, already_exists: bool, save_error: string }
TSharedPtr<FJsonObject> HandleAddRigVMNode(const TSharedPtr<FJsonObject>& Params)
{
    FString RigPath, NodeType;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("control_rig_path"), RigPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("node_type"), NodeType, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UScriptStruct* UnitStruct = RigVMNodeTypeToStruct(NodeType);
    if (!UnitStruct)
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Unsupported node_type '%s' (expected one of: BeginExecution, PrepareForExecution, GetTransform, SetTransform, AimBone, TwoBoneIKSimple)"), *NodeType));

    UControlRigBlueprint* CRBP = nullptr;
    URigVMController* Controller = nullptr;
    if (TSharedPtr<FJsonObject> Error = RigVM_LoadController(RigPath, CRBP, Controller))
    {
        return Error;
    }

    FString NodeName;
    Params->TryGetStringField(TEXT("node_name"), NodeName);

    FVector2D Position(0.0, 0.0);
    if (Params->HasField(TEXT("position")))
    {
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (Params->TryGetArrayField(TEXT("position"), Arr) && Arr && Arr->Num() >= 2)
        {
            Position.X = (*Arr)[0]->AsNumber();
            Position.Y = (*Arr)[1]->AsNumber();
        }
    }

    URigVMUnitNode* NewNode = Controller->AddUnitNode(
        UnitStruct, TEXT("Execute"), Position, NodeName,
        /*bSetupUndoRedo*/ false, /*bPrintPythonCommand*/ false);

    if (!NewNode)
    {
        // Idempotent: check if a node with the same name already exists.
        URigVMGraph* Model = CRBP->GetDefaultModel();
        if (Model && !NodeName.IsEmpty())
        {
            for (URigVMNode* ExistingNode : Model->GetNodes())
            {
                if (ExistingNode && ExistingNode->GetName() == NodeName)
                {
                    // Node already exists — return success (idempotent)
                    TSharedPtr<FJsonObject> Resp = RigVM_StartReply(CRBP);
                    Resp->SetStringField(TEXT("node_name"), ExistingNode->GetName());
                    Resp->SetStringField(TEXT("node_title"), ExistingNode->GetNodeTitle());
                    Resp->SetStringField(TEXT("node_type"), UnitStruct->GetName());
                    Resp->SetNumberField(TEXT("pin_count"), ExistingNode->GetPins().Num());
                    Resp->SetNumberField(TEXT("node_count"), Model->GetNodes().Num());
                    Resp->SetBoolField(TEXT("saved"), true);
                    Resp->SetBoolField(TEXT("already_exists"), true);
                    return mcp_common::MakeOkReply(Resp);
                }
            }
        }
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("AddUnitNode failed for node_type '%s'"), *NodeType));
    }

    FString SaveErr;
    const bool bSaved = RigVM_CompileAndSave(CRBP, SaveErr);

    URigVMGraph* Model = CRBP->GetDefaultModel();

    TSharedPtr<FJsonObject> Resp = RigVM_StartReply(CRBP);
    Resp->SetStringField(TEXT("node_name"), NewNode->GetName());          // ACTUAL graph node name
    Resp->SetStringField(TEXT("node_title"), NewNode->GetNodeTitle());
    Resp->SetStringField(TEXT("node_type"), UnitStruct->GetName());
    Resp->SetNumberField(TEXT("pin_count"), NewNode->GetPins().Num());
    Resp->SetNumberField(TEXT("node_count"), Model ? Model->GetNodes().Num() : 0);
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("add_rigvm_node"), SaveErr);
}

// ---------------------------------------------------------------------------
// set_rigvm_pin_default
// ---------------------------------------------------------------------------
// MCP:set_rigvm_pin_default - Set the default value of a pin in the main RigVM graph of a Control Rig Blueprint (URigVMController::SetPinDefaultValue, no undo entry), then compile and save it. success reflects the save even when the write failed: check set, pin_found and current_value, which is read back.
//   @param control_rig_path - Asset path of the UControlRigBlueprint, e.g. "/Game/Rigs/CR_Mannequin".
//   @param pin_path - Pin path NodeName.PinName, e.g. SetTransform.bInitial
//   @param value - Value in UE export-text form, e.g. "true", "1.0" or "(X=0.0,Y=0.0,Z=1.0)"; array pins are resized to the given element count.
//   @returns { asset_path: string, pin_path: string, value: string, set: bool, pin_found: bool, current_value: string, saved: bool, save_error: string }
TSharedPtr<FJsonObject> HandleSetRigVMPinDefault(const TSharedPtr<FJsonObject>& Params)
{
    FString RigPath, PinPath, Value;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("control_rig_path"), RigPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("pin_path"), PinPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("value"), Value, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UControlRigBlueprint* CRBP = nullptr;
    URigVMController* Controller = nullptr;
    if (TSharedPtr<FJsonObject> Error = RigVM_LoadController(RigPath, CRBP, Controller))
    {
        return Error;
    }

    const bool bSet = Controller->SetPinDefaultValue(
        PinPath, Value, /*bResizeArrays*/ true, /*bSetupUndoRedo*/ false);

    FString SaveErr;
    const bool bSaved = RigVM_CompileAndSave(CRBP, SaveErr);

    // Read back the actual default value to verify (some 5.7 setters return false on success).
    URigVMGraph* Model = CRBP->GetDefaultModel();
    FString ReadBack;
    bool bPinFound = false;
    if (Model)
    {
        if (URigVMPin* Pin = Model->FindPin(PinPath))
        {
            bPinFound = true;
            ReadBack = Pin->GetDefaultValue();
        }
    }

    TSharedPtr<FJsonObject> Resp = RigVM_StartReply(CRBP);
    Resp->SetStringField(TEXT("pin_path"), PinPath);
    Resp->SetStringField(TEXT("value"), Value);
    Resp->SetBoolField(TEXT("set"), bSet || (bPinFound && ReadBack == Value));
    Resp->SetBoolField(TEXT("pin_found"), bPinFound);
    Resp->SetStringField(TEXT("current_value"), ReadBack);
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("set_rigvm_pin_default"), SaveErr);
}

// ---------------------------------------------------------------------------
// add_rigvm_link
// ---------------------------------------------------------------------------
// MCP:add_rigvm_link - Link an output pin to an input pin in the main RigVM graph of a Control Rig Blueprint (URigVMController::AddLink with its undo, as the editor links, so a new link is one undo step); a link already into that input pin, into a pin inside it or into the pin it is part of, or out of an execute output pin, is replaced. Pins are NodeName.PinName paths as get_rigvm_graph lists them. A new link is then compiled and saved; a failed link (pins missing, incompatible or already linked) is an error with linked and saved false, the graph is left as it was (links the attempt broke are restored) and nothing is compiled or saved. Refused during PIE unless force. Add nodes with add_rigvm_node and set unlinked inputs with set_rigvm_pin_default; Blueprint graphs use connect_pins.
//   @param control_rig_path - Path to the UControlRigBlueprint asset
//   @param source_pin - Output pin path NodeA.PinName
//   @param target_pin - Input pin path NodeB.PinName
//   @returns { asset_path: string, source_pin: string, target_pin: string, linked: bool, link_count: int, saved: bool, save_error: string }
TSharedPtr<FJsonObject> HandleAddRigVMLink(const TSharedPtr<FJsonObject>& Params)
{
    FString RigPath, SourcePin, TargetPin;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("control_rig_path"), RigPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("source_pin"), SourcePin, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("target_pin"), TargetPin, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UControlRigBlueprint* CRBP = nullptr;
    URigVMController* Controller = nullptr;
    if (TSharedPtr<FJsonObject> Error = RigVM_LoadController(RigPath, CRBP, Controller))
    {
        return Error;
    }

    // With undo set up, AddLink records what it changes (the links it breaks on the target pin, the types it
    // resolves) into its own action, and cancelling that action on every failure past its first change undoes
    // them; without it a link that fails after those steps leaves them in the graph. A new link is one action
    // on the rig's action stack, and so one undo step.
    const bool bLinked = Controller->AddLink(
        SourcePin, TargetPin, /*bSetupUndoRedo*/ true);

    TSharedPtr<FJsonObject> Resp = RigVM_StartReply(CRBP);
    Resp->SetStringField(TEXT("source_pin"), SourcePin);
    Resp->SetStringField(TEXT("target_pin"), TargetPin);
    Resp->SetBoolField(TEXT("linked"), bLinked);
    if (!bLinked)
    {
        // An incompatible, missing or already-linked pairing is the engine returning false here. The graph is
        // as it was, so the rig is neither compiled nor saved; the payload still says so (saved=false).
        URigVMGraph* Model = CRBP->GetDefaultModel();
        Resp->SetNumberField(TEXT("link_count"), Model ? Model->GetLinks().Num() : 0);
        Resp->SetBoolField(TEXT("saved"), false);
        return mcp_common::MakeOutcomeReply(Resp, false, FString::Printf(
            TEXT("AddLink returned false for '%s' -> '%s' (pins incompatible, missing, or already linked); the graph was left as it was and the rig was not compiled or saved"),
            *SourcePin, *TargetPin));
    }

    FString SaveErr;
    const bool bSaved = RigVM_CompileAndSave(CRBP, SaveErr);

    URigVMGraph* Model = CRBP->GetDefaultModel();
    Resp->SetNumberField(TEXT("link_count"), Model ? Model->GetLinks().Num() : 0);
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("add_rigvm_link"), SaveErr);
}

// ---------------------------------------------------------------------------
// get_rigvm_graph
// ---------------------------------------------------------------------------
// MCP:get_rigvm_graph - Read a Control Rig's RigVM solver graph nodes pins and links
//   @param control_rig_path - Path to the UControlRigBlueprint asset
//   @returns { asset_path: string, nodes: [object], node_count: int, link_count: int }
TSharedPtr<FJsonObject> HandleGetRigVMGraph(const TSharedPtr<FJsonObject>& Params)
{
    FString RigPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("control_rig_path"), RigPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UControlRigBlueprint* CRBP = nullptr;
    if (TSharedPtr<FJsonObject> Error = RigVM_LoadBlueprint(RigPath, CRBP))
    {
        return Error;
    }

    URigVMGraph* Model = CRBP->GetDefaultModel();
    if (!Model)
        return mcp_common::MakeErrorReply(TEXT("Control rig blueprint has no default RigVM model"));

    TArray<TSharedPtr<FJsonValue>> Nodes;
    for (URigVMNode* Node : Model->GetNodes())
    {
        if (Node)
        {
            Nodes.Add(MakeShared<FJsonValueObject>(NodeToJson(Node)));
        }
    }

    TSharedPtr<FJsonObject> Resp = RigVM_StartReply(CRBP);
    Resp->SetArrayField(TEXT("nodes"), Nodes);
    Resp->SetNumberField(TEXT("node_count"), Model->GetNodes().Num());
    Resp->SetNumberField(TEXT("link_count"), Model->GetLinks().Num());
    return mcp_common::MakeOkReply(Resp);
}

// ---------------------------------------------------------------------------
// remove_rigvm_node
// ---------------------------------------------------------------------------
// MCP:remove_rigvm_node - Remove a node from a Control Rig's RigVM solver graph, then compile and save the rig. A node that is not found, or that the controller fails to remove, is an error and nothing is compiled or saved.
//   @param control_rig_path - Path to the UControlRigBlueprint asset
//   @param node_name - Graph node name to remove, as reported by add_rigvm_node
//   @returns { asset_path: string, node_name: string, removed: bool, node_count: int, saved: bool, save_error: string }
TSharedPtr<FJsonObject> HandleRemoveRigVMNode(const TSharedPtr<FJsonObject>& Params)
{
    FString RigPath, NodeName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("control_rig_path"), RigPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("node_name"), NodeName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UControlRigBlueprint* CRBP = nullptr;
    URigVMController* Controller = nullptr;
    if (TSharedPtr<FJsonObject> Error = RigVM_LoadController(RigPath, CRBP, Controller))
    {
        return Error;
    }

    URigVMGraph* Model = CRBP->GetDefaultModel();
    if (!Model)
        return mcp_common::MakeErrorReply(TEXT("Control rig blueprint has no default RigVM model"));

    URigVMNode* Node = Model->FindNodeByName(FName(*NodeName));
    if (!Node)
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Node '%s' not found in RigVM graph"), *NodeName));

    const bool bRemoved = Controller->RemoveNode(Node, /*bSetupUndoRedo*/ false, /*bPrintPythonCommand*/ false);

    TSharedPtr<FJsonObject> Resp = RigVM_StartReply(CRBP);
    Resp->SetStringField(TEXT("node_name"), NodeName);
    Resp->SetBoolField(TEXT("removed"), bRemoved);
    if (!bRemoved)
    {
        // Nothing was removed, so the rig is neither compiled nor saved; the payload still says so.
        Resp->SetNumberField(TEXT("node_count"), Model->GetNodes().Num());
        Resp->SetBoolField(TEXT("saved"), false);
        return mcp_common::MakeOutcomeReply(Resp, false, FString::Printf(
            TEXT("RemoveNode returned false for '%s'; the rig was not compiled or saved"), *NodeName));
    }

    FString SaveErr;
    const bool bSaved = RigVM_CompileAndSave(CRBP, SaveErr);
    Resp->SetNumberField(TEXT("node_count"), Model->GetNodes().Num());
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("remove_rigvm_node"), SaveErr);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_rigvm_link"), &HandleAddRigVMLink },
    { TEXT("add_rigvm_node"), &HandleAddRigVMNode },
    { TEXT("get_rigvm_graph"), &HandleGetRigVMGraph },
    { TEXT("remove_rigvm_node"), &HandleRemoveRigVMNode },
    { TEXT("set_rigvm_pin_default"), &HandleSetRigVMPinDefault },
};
} // namespace UEAgentRigVMCommands
