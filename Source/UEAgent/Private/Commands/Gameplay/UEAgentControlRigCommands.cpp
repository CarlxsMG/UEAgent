#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Modules/ModuleManager.h"

#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"

#include "Kismet2/KismetEditorUtilities.h"

// Control Rig plugin (build_cs_public_deps: ControlRig ; editor deps: ControlRigDeveloper, ControlRigEditor)
#include "ControlRigBlueprintFactory.h"         // ControlRigEditor: CreateNewControlRigAsset (static); also pulls in ControlRigBlueprintLegacy.h
#include "ControlRigBlueprintLegacy.h"           // ControlRigDeveloper: UControlRigBlueprint (GetHierarchyController / GetHierarchy)
#include "ControlRig.h"                          // ControlRig: UControlRig base class
#include "Rigs/RigHierarchy.h"                   // ControlRig: URigHierarchy
#include "Rigs/RigHierarchyController.h"          // ControlRig: URigHierarchyController
#include "Rigs/RigHierarchyDefines.h"             // ControlRig: FRigElementKey / FRigControlSettings / FRigControlValue / enums
#include "Rigs/RigHierarchyElements.h"            // ControlRig: FRigControlSettings full definition

/**
 * Control Rig authoring commands.
 *
 * Authors UControlRigBlueprint assets entirely headless via the engine's own scripting path:
 *   - UControlRigBlueprintFactory::CreateNewControlRigAsset (ControlRigEditor) registers + returns the
 *     asset with no modal picker.
 *   - UControlRigBlueprint::GetHierarchyController (ControlRigDeveloper) gives a URigHierarchyController
 *     whose AddBone/AddNull/AddControl_ForBlueprint/RemoveElement edit the rig hierarchy directly
 *     (ControlRig module).
 * Each mutation recompiles the blueprint (FKismetEditorUtilities::CompileBlueprint) and persists the
 * package to disk, then reads the hierarchy back for verification.
 */
namespace UEAgentControlRigCommands
{

// MCP-DOMAIN: Animation

namespace
{
    /** Normalize a package folder path: strip trailing slash. */
    FString CRNormalizeFolder(const FString& In)
    {
        FString Out = In;
        Out.RemoveFromEnd(TEXT("/"));
        return Out;
    }

    /** Map an ERigElementType to a stable lower-case label string for payloads. */
    FString RigElementTypeToString(ERigElementType Type)
    {
        switch (Type)
        {
            case ERigElementType::Bone:      return TEXT("Bone");
            case ERigElementType::Null:      return TEXT("Null");
            case ERigElementType::Control:   return TEXT("Control");
            case ERigElementType::Curve:     return TEXT("Curve");
            case ERigElementType::Physics:   return TEXT("Physics");
            case ERigElementType::Reference: return TEXT("Reference");
            case ERigElementType::Connector: return TEXT("Connector");
            case ERigElementType::Socket:    return TEXT("Socket");
            default:                         return TEXT("None");
        }
    }

    /** Parse a string element type ("Bone"/"Null"/"Control") to ERigElementType. */
    ERigElementType StringToRigElementType(const FString& In)
    {
        const FString Lower = In.ToLower();
        if (Lower == TEXT("bone")) { return ERigElementType::Bone; }
        if (Lower == TEXT("null") ||
            Lower == TEXT("space"))     return ERigElementType::Null;
        if (Lower == TEXT("control")) { return ERigElementType::Control; }
        if (Lower == TEXT("curve")) { return ERigElementType::Curve; }
        if (Lower == TEXT("socket")) { return ERigElementType::Socket; }
        if (Lower == TEXT("connector")) { return ERigElementType::Connector; }
        return ERigElementType::None;
    }

    /** Parse a control type string to ERigControlType (defaults to Float). bRecognized=false if unknown. */
    ERigControlType StringToRigControlType(const FString& In, bool& bRecognized)
    {
        bRecognized = true;
        const FString Lower = In.ToLower();
        if (Lower == TEXT("bool")) { return ERigControlType::Bool; }
        if (Lower == TEXT("float")) { return ERigControlType::Float; }
        if (Lower == TEXT("scalefloat")) { return ERigControlType::ScaleFloat; }
        if (Lower == TEXT("integer") ||
            Lower == TEXT("int"))            return ERigControlType::Integer;
        if (Lower == TEXT("vector2d")) { return ERigControlType::Vector2D; }
        if (Lower == TEXT("position")) { return ERigControlType::Position; }
        if (Lower == TEXT("scale")) { return ERigControlType::Scale; }
        if (Lower == TEXT("rotator")) { return ERigControlType::Rotator; }
        if (Lower == TEXT("transform")) { return ERigControlType::Transform; }
        if (Lower == TEXT("eulertransform")) { return ERigControlType::EulerTransform; }
        bRecognized = false;
        return ERigControlType::Float;
    }

    FString RigControlTypeToString(ERigControlType Type)
    {
        switch (Type)
        {
            case ERigControlType::Bool:           return TEXT("Bool");
            case ERigControlType::Float:          return TEXT("Float");
            case ERigControlType::ScaleFloat:     return TEXT("ScaleFloat");
            case ERigControlType::Integer:        return TEXT("Integer");
            case ERigControlType::Vector2D:       return TEXT("Vector2D");
            case ERigControlType::Position:       return TEXT("Position");
            case ERigControlType::Scale:          return TEXT("Scale");
            case ERigControlType::Rotator:        return TEXT("Rotator");
            case ERigControlType::Transform:      return TEXT("Transform");
            case ERigControlType::EulerTransform: return TEXT("EulerTransform");
            default:                              return TEXT("Float");
        }
    }

    /** Build a default FRigControlValue appropriate to the control type. */
    FRigControlValue MakeDefaultControlValue(ERigControlType Type)
    {
        switch (Type)
        {
            case ERigControlType::Bool:
                return FRigControlValue::Make<bool>(false);
            case ERigControlType::Float:
            case ERigControlType::ScaleFloat:
                return FRigControlValue::Make<float>(0.f);
            case ERigControlType::Integer:
                return FRigControlValue::Make<int32>(0);
            case ERigControlType::Vector2D:
            {
                // Vector2D storage uses FVector3f (X,Y used).
                const FVector3f V(0.f, 0.f, 0.f);
                return FRigControlValue::Make<FVector3f>(V);
            }
            case ERigControlType::Position:
            case ERigControlType::Rotator:
            {
                const FVector3f V(0.f, 0.f, 0.f);
                return FRigControlValue::Make<FVector3f>(V);
            }
            case ERigControlType::Scale:
            {
                const FVector3f V(1.f, 1.f, 1.f);
                return FRigControlValue::Make<FVector3f>(V);
            }
            case ERigControlType::Transform:
                // Default-constructed FTransform_Float == identity transform.
                return FRigControlValue::Make<FRigControlValue::FTransform_Float>(
                    FRigControlValue::FTransform_Float());
            case ERigControlType::EulerTransform:
                // Default-constructed FEulerTransform_Float == identity transform.
                return FRigControlValue::Make<FRigControlValue::FEulerTransform_Float>(
                    FRigControlValue::FEulerTransform_Float());
            default:
                return FRigControlValue::Make<float>(0.f);
        }
    }

    /** Count elements of each type in the hierarchy and fill a JSON object; does nothing without a hierarchy. */
    void FillHierarchyCounts(URigHierarchy* Hierarchy, const TSharedPtr<FJsonObject>& Out, TArray<TSharedPtr<FJsonValue>>* OutElements)
    {
        if (!Hierarchy)
        {
            return;
        }
        int32 BoneCount = 0, NullCount = 0, ControlCount = 0, CurveCount = 0, OtherCount = 0;
        const TArray<FRigElementKey> Keys = Hierarchy->GetAllKeys(true);
        for (const FRigElementKey& Key : Keys)
        {
            switch (Key.Type)
            {
                case ERigElementType::Bone:    ++BoneCount; break;
                case ERigElementType::Null:    ++NullCount; break;
                case ERigElementType::Control: ++ControlCount; break;
                case ERigElementType::Curve:   ++CurveCount; break;
                default:                       ++OtherCount; break;
            }
            if (OutElements)
            {
                TSharedPtr<FJsonObject> ElemObj = MakeShared<FJsonObject>();
                ElemObj->SetStringField(TEXT("name"), Key.Name.ToString());
                ElemObj->SetStringField(TEXT("type"), RigElementTypeToString(Key.Type));
                OutElements->Add(MakeShared<FJsonValueObject>(ElemObj));
            }
        }
        Out->SetNumberField(TEXT("bone_count"), BoneCount);
        Out->SetNumberField(TEXT("null_count"), NullCount);
        Out->SetNumberField(TEXT("control_count"), ControlCount);
        Out->SetNumberField(TEXT("curve_count"), CurveCount);
        Out->SetNumberField(TEXT("other_count"), OtherCount);
        Out->SetNumberField(TEXT("total"), Keys.Num());
    }

    /**
     * The parent named by "parent_name", of the element type "parent_type" names (DefaultType when that is absent or
     * unknown); the empty key, which parents to the root, when parent_name is absent or empty.
     */
    FRigElementKey ControlRig_ReadParentKey(const TSharedPtr<FJsonObject>& Params, ERigElementType DefaultType)
    {
        FRigElementKey ParentKey;
        FString ParentName;
        if (Params->TryGetStringField(TEXT("parent_name"), ParentName) && !ParentName.IsEmpty())
        {
            FString ParentType = RigElementTypeToString(DefaultType);
            Params->TryGetStringField(TEXT("parent_type"), ParentType);
            ERigElementType PType = StringToRigElementType(ParentType);
            if (PType == ERigElementType::None)
            {
                PType = DefaultType;
            }
            ParentKey = FRigElementKey(FName(*ParentName), PType);
        }
        return ParentKey;
    }

    /**
     * Null when ParentKey is the empty key (the root) or names an element of the rig's hierarchy, else the error reply.
     * The hierarchy controller puts an element whose parent it cannot find at the root without a word, so a parent that
     * is not there is refused before anything is added. Hint tells the caller what to check.
     */
    TSharedPtr<FJsonObject> ControlRig_RefuseMissingParent(UControlRigBlueprint* CRBP, const FRigElementKey& ParentKey,
        const TCHAR* NewElementLabel, const TCHAR* Hint)
    {
        if (!ParentKey.IsValid())
        {
            return nullptr;
        }
        const URigHierarchy* Hierarchy = CRBP->GetHierarchy();
        if (Hierarchy != nullptr && Hierarchy->GetIndex(ParentKey) != INDEX_NONE)
        {
            return nullptr;
        }
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("The rig has no %s named '%s' to parent the new %s under; nothing was added. %s"),
            *RigElementTypeToString(ParentKey.Type), *ParentKey.Name.ToString(), NewElementLabel, Hint));
    }

    /** The parent the hierarchy gives Key, read back: its name, or empty for an element at the root. */
    FString ControlRig_ReadParentName(UControlRigBlueprint* CRBP, const FRigElementKey& Key)
    {
        const URigHierarchy* Hierarchy = CRBP->GetHierarchy();
        const FRigElementKey Parent = Hierarchy != nullptr ? Hierarchy->GetFirstParent(Key) : FRigElementKey();
        return Parent.IsValid() ? Parent.Name.ToString() : FString();
    }

    /** The rig blueprint at RigPath. Null on success, else the error reply. */
    TSharedPtr<FJsonObject> ControlRig_LoadBlueprint(const FString& RigPath, UControlRigBlueprint*& OutBlueprint)
    {
        OutBlueprint = mcp_common::LoadAssetOrSoftPath<UControlRigBlueprint>(RigPath);
        if (!OutBlueprint)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Could not load control rig blueprint: %s"), *RigPath));
        }
        return nullptr;
    }

    /** The rig blueprint at RigPath and its hierarchy controller. Null on success, else the error reply. */
    TSharedPtr<FJsonObject> ControlRig_LoadController(const FString& RigPath, UControlRigBlueprint*& OutBlueprint,
        URigHierarchyController*& OutController)
    {
        if (TSharedPtr<FJsonObject> Error = ControlRig_LoadBlueprint(RigPath, OutBlueprint))
        {
            return Error;
        }
        OutController = OutBlueprint->GetHierarchyController();
        if (!OutController)
        {
            return mcp_common::MakeErrorReply(TEXT("Could not get hierarchy controller for the control rig"));
        }
        return nullptr;
    }

    /**
     * Reads "control_rig_path" and "control_name" and resolves that control: the rig blueprint, its hierarchy
     * controller and hierarchy, and the control's key. Null on success, else the error reply.
     */
    TSharedPtr<FJsonObject> ControlRig_ResolveControl(const TSharedPtr<FJsonObject>& Params, FString& OutControlName,
        UControlRigBlueprint*& OutBlueprint, URigHierarchyController*& OutController, URigHierarchy*& OutHierarchy,
        FRigElementKey& OutKey)
    {
        FString RigPath;
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("control_rig_path"), RigPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("control_name"), OutControlName)) { return Missing; }

        if (TSharedPtr<FJsonObject> Error = ControlRig_LoadController(RigPath, OutBlueprint, OutController))
        {
            return Error;
        }
        OutHierarchy = OutBlueprint->GetHierarchy();
        if (!OutHierarchy)
        {
            return mcp_common::MakeErrorReply(TEXT("Could not get hierarchy controller for the control rig"));
        }

        OutKey = FRigElementKey(FName(*OutControlName), ERigElementType::Control);
        if (OutHierarchy->GetIndex(OutKey) == INDEX_NONE)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Control '%s' not found in the rig hierarchy"), *OutControlName));
        }
        return nullptr;
    }

    /** Recompiles the rig blueprint, then saves its package; true when the package reached disk. */
    bool ControlRig_CompileAndSave(UControlRigBlueprint* CRBP, FString& OutSaveError)
    {
        FKismetEditorUtilities::CompileBlueprint(CRBP, EBlueprintCompileOptions::SkipGarbageCollection);
        return mcp_common::SaveAssetPackage(CRBP->GetOutermost(), CRBP, OutSaveError);
    }

    /** A reply that opens with the rig blueprint's asset_path. */
    TSharedPtr<FJsonObject> ControlRig_StartReply(const UControlRigBlueprint* CRBP)
    {
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetStringField(TEXT("asset_path"), CRBP->GetPathName());
        return Resp;
    }

    /** The fields create_control_rig_blueprint reports for CRBP; bCreated tells whether this call made it. */
    TSharedPtr<FJsonObject> ControlRig_CreateReply(const UControlRigBlueprint* CRBP, const FString& Name, bool bCreated)
    {
        const bool bChild = CRBP->GeneratedClass &&
            CRBP->GeneratedClass->IsChildOf(UControlRig::StaticClass());
        TSharedPtr<FJsonObject> Resp = ControlRig_StartReply(CRBP);
        Resp->SetStringField(TEXT("name"), Name);
        Resp->SetStringField(TEXT("generated_class"),
            CRBP->GeneratedClass ? CRBP->GeneratedClass->GetName() : TEXT(""));
        Resp->SetBoolField(TEXT("is_control_rig"), bChild);
        Resp->SetBoolField(TEXT("created"), bCreated);
        Resp->SetBoolField(TEXT("already_existed"), !bCreated);
        return Resp;
    }
}

// ---------------------------------------------------------------------------
// create_control_rig_blueprint
// ---------------------------------------------------------------------------
// MCP:create_control_rig_blueprint - Create a Control Rig Blueprint (non-modular) at package_path/name with UControlRigBlueprintFactory, compile it and save it. Idempotent: an existing Control Rig Blueprint there is returned unchanged (already_existed=true). Returns asset_path, generated_class, is_control_rig and saved; refused while PIE runs unless force is set.
//   @param package_path - Content folder starting with "/", e.g. "/Game/Rigs"; a trailing slash is ignored
//   @param name - Asset name of the Control Rig Blueprint, e.g. "CR_Hero"
//   @returns { asset_path: string, name: string, generated_class: string, is_control_rig: bool, created: bool, already_existed: bool, saved: bool, save_error: string }

TSharedPtr<FJsonObject> HandleCreateControlRigBlueprint(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath, Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("package_path"), PackagePath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (!PackagePath.StartsWith(TEXT("/")))
        return mcp_common::MakeErrorReply(TEXT("package_path must be a UE long path, e.g. /Game/Foo"));

    const FString Folder = CRNormalizeFolder(PackagePath);
    const FString FullPath = FString::Printf(TEXT("%s/%s"), *Folder, *Name);
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(FullPath)) { return Invalid; }

    // Idempotent: if it already exists, load and return a stable payload.
    if (UEditorAssetLibrary::DoesAssetExist(FullPath))
    {
        UControlRigBlueprint* Existing = mcp_common::LoadAssetOrSoftPath<UControlRigBlueprint>(FullPath);
        if (Existing)
        {
            return mcp_common::MakeOkReply(ControlRig_CreateReply(Existing, Name, /*bCreated=*/false));
        }
    }

    // CreateNewControlRigAsset takes the FULL desired asset path (folder + name),
    // creates + registers the asset (no modal), and returns the blueprint.
    UControlRigBlueprint* CRBP = UControlRigBlueprintFactory::CreateNewControlRigAsset(FullPath, /*bModularRig*/ false);
    if (!CRBP)
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Failed to create control rig blueprint at %s"), *FullPath));

    // Compile so the generated UControlRig class is up to date, then persist.
    FKismetEditorUtilities::CompileBlueprint(CRBP, EBlueprintCompileOptions::SkipGarbageCollection);

    FAssetRegistryModule::AssetCreated(CRBP);

    FString SaveErr;
    const bool bSaved = mcp_common::SaveAssetPackage(CRBP->GetOutermost(), CRBP, SaveErr);

    return mcp_common::MakeSavedReply(ControlRig_CreateReply(CRBP, Name, /*bCreated=*/true), bSaved,
        TEXT("create_control_rig_blueprint"), SaveErr);
}

// ---------------------------------------------------------------------------
// add_rig_bone
// ---------------------------------------------------------------------------
// MCP:add_rig_bone - Add a bone to the hierarchy of a Control Rig Blueprint, under parent_name or at the root, then compile and save the blueprint; there is no undo step. The engine keeps names unique, so element_key is the name the bone really got, and parent is the parent read back from the hierarchy. A parent_name that is not a bone of the rig is an error and nothing is added; so is a rig that cannot be loaded, and a failed save makes success false with save_error. Refused while PIE runs unless force is set. Nulls and controls: add_rig_null, add_rig_control; inspect: get_rig_hierarchy.
//   @param control_rig_path - Control Rig Blueprint asset path, e.g. "/Game/Rigs/CR_Hero"
//   @param bone_name - Name of the new bone
//   @param parent_name - Existing BONE to parent the new bone under; omitted or empty puts it at the root, and a name that is not a bone of the rig is an error
//   @param location - Optional [x, y, z] world location for the new bone.
//   @param rotation - Optional [pitch, yaw, roll] world rotation for the new bone.
//   @param scale - Optional [x, y, z] scale for the new bone.
//   @returns { asset_path: string, element_key: string, element_type: string, parent: string, bone_count: int, null_count: int, control_count: int, curve_count: int, other_count: int, total: int, saved: bool, save_error: string }

TSharedPtr<FJsonObject> HandleAddRigBone(const TSharedPtr<FJsonObject>& Params)
{
    FString RigPath, BoneName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("control_rig_path"), RigPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("bone_name"), BoneName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UControlRigBlueprint* CRBP = nullptr;
    URigHierarchyController* Controller = nullptr;
    if (TSharedPtr<FJsonObject> Error = ControlRig_LoadController(RigPath, CRBP, Controller))
    {
        return Error;
    }

    // Parent: empty key (= root) unless a parent_name is provided (parented under a bone).
    FRigElementKey ParentKey;
    FString ParentName;
    if (Params->TryGetStringField(TEXT("parent_name"), ParentName) && !ParentName.IsEmpty())
    {
        ParentKey = FRigElementKey(FName(*ParentName), ERigElementType::Bone);
    }
    if (TSharedPtr<FJsonObject> NoParent = ControlRig_RefuseMissingParent(CRBP, ParentKey, TEXT("bone"),
        TEXT("A bone is parented under a bone only; check the names with get_rig_hierarchy, or omit parent_name for the root.")))
    {
        return NoParent;
    }

    const FVector Location = mcp_common::ReadJsonVector(Params, TEXT("location"));
    const FRotator Rotation = mcp_common::ReadJsonRotator(Params, TEXT("rotation"));
    FVector Scale(1.f, 1.f, 1.f);
    if (Params->HasField(TEXT("scale")))
    {
        Scale = mcp_common::ReadJsonVector(Params, TEXT("scale"));
    }
    const FTransform Transform(Rotation.Quaternion(), Location, Scale);

    const FRigElementKey NewKey = Controller->AddBone(
        FName(*BoneName), ParentKey, Transform, /*bTransformInGlobal*/ true,
        ERigBoneType::User, /*bSetupUndo*/ false, /*bPrintPythonCommand*/ false);

    if (!NewKey.IsValid())
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("AddBone failed for '%s'"), *BoneName));

    FString SaveErr;
    const bool bSaved = ControlRig_CompileAndSave(CRBP, SaveErr);

    TSharedPtr<FJsonObject> Resp = ControlRig_StartReply(CRBP);
    Resp->SetStringField(TEXT("element_key"), NewKey.Name.ToString());
    Resp->SetStringField(TEXT("element_type"), RigElementTypeToString(NewKey.Type));
    Resp->SetStringField(TEXT("parent"), ControlRig_ReadParentName(CRBP, NewKey));
    FillHierarchyCounts(CRBP->GetHierarchy(), Resp, nullptr);
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("add_rig_bone"), SaveErr);
}

// ---------------------------------------------------------------------------
// add_rig_null
// ---------------------------------------------------------------------------
// MCP:add_rig_null - Add a null (a space: a transform-only element) to the hierarchy of a Control Rig Blueprint at location in rig global space, with no rotation and unit scale, then compile and save the blueprint; there is no undo step and it is refused while PIE runs unless force is set. The engine keeps names unique, so element_key is the name the null really got. parent_name puts it under that element, of parent_type; a parent the hierarchy does not have is an error and nothing is added, and parent is the parent read back. A rig that cannot be loaded is an error; a failed save makes success false with save_error. Each call adds another null. Controls and bones: add_rig_control, add_rig_bone; rename or take out: rename_rig_element, remove_rig_element.
//   @param control_rig_path - Control Rig Blueprint asset path, e.g. "/Game/Rigs/CR_Hero"
//   @param null_name - Name of the new null; a name already in use gets a numbered suffix
//   @param parent_name - Optional existing element name to parent the new null under; omitted or empty puts it at the root, and a name the hierarchy lacks is an error.
//   @param parent_type - Element type of the parent ("Bone" default, "Null"/"Control"/etc); case is ignored, "Space" means Null, and an unknown type falls back to Bone.
//   @param location - Optional [x, y, z] location in rig global space for the new null; defaults to the origin.
//   @returns { asset_path: string, element_key: string, element_type: string, parent: string, bone_count: int, null_count: int, control_count: int, curve_count: int, other_count: int, total: int, saved: bool, save_error: string }

TSharedPtr<FJsonObject> HandleAddRigNull(const TSharedPtr<FJsonObject>& Params)
{
    FString RigPath, NullName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("control_rig_path"), RigPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("null_name"), NullName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UControlRigBlueprint* CRBP = nullptr;
    URigHierarchyController* Controller = nullptr;
    if (TSharedPtr<FJsonObject> Error = ControlRig_LoadController(RigPath, CRBP, Controller))
    {
        return Error;
    }

    // Parent: empty key (= root) unless a parent_name is provided. Default parent type is Bone;
    // callers can parent a null under a null/control by passing parent_type.
    const FRigElementKey ParentKey = ControlRig_ReadParentKey(Params, ERigElementType::Bone);
    if (TSharedPtr<FJsonObject> NoParent = ControlRig_RefuseMissingParent(CRBP, ParentKey, TEXT("null"),
        TEXT("Check parent_name and parent_type (Bone by default) with get_rig_hierarchy, or omit parent_name for the root.")))
    {
        return NoParent;
    }

    const FVector Location = mcp_common::ReadJsonVector(Params, TEXT("location"));
    const FTransform Transform(FQuat::Identity, Location, FVector(1.f, 1.f, 1.f));

    const FRigElementKey NewKey = Controller->AddNull(
        FName(*NullName), ParentKey, Transform, /*bTransformInGlobal*/ true,
        /*bSetupUndo*/ false, /*bPrintPythonCommand*/ false);

    if (!NewKey.IsValid())
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("AddNull failed for '%s'"), *NullName));

    FString SaveErr;
    const bool bSaved = ControlRig_CompileAndSave(CRBP, SaveErr);

    TSharedPtr<FJsonObject> Resp = ControlRig_StartReply(CRBP);
    Resp->SetStringField(TEXT("element_key"), NewKey.Name.ToString());
    Resp->SetStringField(TEXT("element_type"), RigElementTypeToString(NewKey.Type));
    Resp->SetStringField(TEXT("parent"), ControlRig_ReadParentName(CRBP, NewKey));
    FillHierarchyCounts(CRBP->GetHierarchy(), Resp, nullptr);
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("add_rig_null"), SaveErr);
}

// ---------------------------------------------------------------------------
// add_rig_control
// ---------------------------------------------------------------------------
// MCP:add_rig_control - Add an animation control to the hierarchy of a Control Rig Blueprint, with a zero or identity value and no limits, then compile and save the blueprint; there is no undo step and it is refused while PIE runs unless force is set. The engine keeps names unique, so element_key is the name the control really got. An unrecognised control_type makes a Float control with control_type_recognized false instead of failing. parent_name puts it under that element, of parent_type; a parent the hierarchy does not have is an error and nothing is added, and parent is the parent read back. A rig that cannot be loaded is an error; a failed save makes success false with save_error. Set it up with set_rig_control_value, set_rig_control_limits and set_rig_control_settings; bones and nulls: add_rig_bone, add_rig_null; inspect: get_rig_hierarchy; take it out: remove_rig_element.
//   @param control_rig_path - Control Rig Blueprint asset path, e.g. "/Game/Rigs/CR_Hero"
//   @param control_name - Name of the new control, also used as its display name; a name already in use gets a numbered suffix
//   @param control_type - One of: "bool", "float", "scalefloat", "integer", "int", "vector2d", "position", "scale", "rotator", "transform", "eulertransform" (default "float").
//   @param parent_name - Optional existing element name to parent the new control under; omitted or empty puts it at the root, and a name the hierarchy lacks is an error.
//   @param parent_type - Element type of the parent (default "Null"); case is ignored, "Space" means Null, and an unknown type falls back to Null.
//   @returns { asset_path: string, element_key: string, element_type: string, control_type: string, control_type_recognized: bool, parent: string, bone_count: int, null_count: int, control_count: int, curve_count: int, other_count: int, total: int, saved: bool, save_error: string }

TSharedPtr<FJsonObject> HandleAddRigControl(const TSharedPtr<FJsonObject>& Params)
{
    FString RigPath, ControlName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("control_rig_path"), RigPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("control_name"), ControlName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UControlRigBlueprint* CRBP = nullptr;
    URigHierarchyController* Controller = nullptr;
    if (TSharedPtr<FJsonObject> Error = ControlRig_LoadController(RigPath, CRBP, Controller))
    {
        return Error;
    }

    FString ControlTypeStr = TEXT("Float");
    Params->TryGetStringField(TEXT("control_type"), ControlTypeStr);
    bool bRecognized = true;
    const ERigControlType ControlType = StringToRigControlType(ControlTypeStr, bRecognized);

    // Parent: empty (root) unless parent_name provided; parent type can be Null/Control/Bone.
    const FRigElementKey ParentKey = ControlRig_ReadParentKey(Params, ERigElementType::Null);
    if (TSharedPtr<FJsonObject> NoParent = ControlRig_RefuseMissingParent(CRBP, ParentKey, TEXT("control"),
        TEXT("Check parent_name and parent_type (Null by default) with get_rig_hierarchy, or omit parent_name for the root.")))
    {
        return NoParent;
    }

    FRigControlSettings Settings;
    Settings.AnimationType = ERigControlAnimationType::AnimationControl;
    Settings.ControlType = ControlType;
    Settings.DisplayName = FName(*ControlName);
    // Keep the LimitEnabled array sized consistently for this control type (all limits off).
    Settings.SetupLimitArrayForType(false, false, false);

    const FRigControlValue Value = MakeDefaultControlValue(ControlType);

    const FRigElementKey NewKey = Controller->AddControl_ForBlueprint(
        FName(*ControlName), ParentKey, Settings, Value,
        /*bSetupUndo*/ false, /*bPrintPythonCommand*/ false);

    if (!NewKey.IsValid())
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("AddControl failed for '%s'"), *ControlName));

    FString SaveErr;
    const bool bSaved = ControlRig_CompileAndSave(CRBP, SaveErr);

    TSharedPtr<FJsonObject> Resp = ControlRig_StartReply(CRBP);
    Resp->SetStringField(TEXT("element_key"), NewKey.Name.ToString());
    Resp->SetStringField(TEXT("element_type"), RigElementTypeToString(NewKey.Type));
    Resp->SetStringField(TEXT("control_type"), RigControlTypeToString(ControlType));
    Resp->SetBoolField(TEXT("control_type_recognized"), bRecognized);
    Resp->SetStringField(TEXT("parent"), ControlRig_ReadParentName(CRBP, NewKey));
    FillHierarchyCounts(CRBP->GetHierarchy(), Resp, nullptr);
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("add_rig_control"), SaveErr);
}

// ---------------------------------------------------------------------------
// get_rig_hierarchy
// ---------------------------------------------------------------------------
// MCP:get_rig_hierarchy - Read every element of a Control Rig Blueprint hierarchy in traversal order as elements [{name, type}], plus bone_count, null_count, control_count, curve_count, other_count (sockets, connectors and the rest) and total. Read-only: nothing is compiled or saved.
//   @param control_rig_path - Control Rig Blueprint asset path, e.g. "/Game/Rigs/CR_Hero"
//   @returns { asset_path: string, bone_count: int, null_count: int, control_count: int, curve_count: int, other_count: int, total: int, elements: [{name, type}] }

TSharedPtr<FJsonObject> HandleGetRigHierarchy(const TSharedPtr<FJsonObject>& Params)
{
    FString RigPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("control_rig_path"), RigPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UControlRigBlueprint* CRBP = nullptr;
    if (TSharedPtr<FJsonObject> Error = ControlRig_LoadBlueprint(RigPath, CRBP))
    {
        return Error;
    }

    URigHierarchy* Hierarchy = CRBP->GetHierarchy();
    if (!Hierarchy)
        return mcp_common::MakeErrorReply(TEXT("Control rig blueprint has no hierarchy"));

    TArray<TSharedPtr<FJsonValue>> Elements;
    TSharedPtr<FJsonObject> Resp = ControlRig_StartReply(CRBP);
    FillHierarchyCounts(Hierarchy, Resp, &Elements);
    Resp->SetArrayField(TEXT("elements"), Elements);
    return mcp_common::MakeOkReply(Resp);
}

// ---------------------------------------------------------------------------
// remove_rig_element
// ---------------------------------------------------------------------------
// MCP:remove_rig_element - Remove one element (by name and type) from a Control Rig Blueprint hierarchy with RemoveElement, then compile and save the blueprint. Errors on an unknown type or a missing element. Returns the per-type counts afterwards; refused while PIE runs unless force is set.
//   @param control_rig_path - Control Rig Blueprint asset path, e.g. "/Game/Rigs/CR_Hero"
//   @param element_name - Name of the element to remove
//   @param element_type - Element type, any case: "Bone", "Null" (or "Space"), "Control", "Curve", "Socket" or "Connector"
//   @returns { asset_path: string, element_key: string, element_type: string, removed: bool, bone_count: int, null_count: int, control_count: int, curve_count: int, other_count: int, total: int, saved: bool, save_error: string }

TSharedPtr<FJsonObject> HandleRemoveRigElement(const TSharedPtr<FJsonObject>& Params)
{
    FString RigPath, ElementName, ElementTypeStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("control_rig_path"), RigPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("element_name"), ElementName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("element_type"), ElementTypeStr, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    const ERigElementType ElemType = StringToRigElementType(ElementTypeStr);
    if (ElemType == ERigElementType::None)
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Unknown element_type: %s (expected Bone/Null/Control/Curve/Socket/Connector)"), *ElementTypeStr));

    UControlRigBlueprint* CRBP = nullptr;
    URigHierarchyController* Controller = nullptr;
    if (TSharedPtr<FJsonObject> Error = ControlRig_LoadController(RigPath, CRBP, Controller))
    {
        return Error;
    }

    const FRigElementKey Key(FName(*ElementName), ElemType);
    const bool bRemoved = Controller->RemoveElement(Key, /*bSetupUndo*/ false, /*bPrintPythonCommand*/ false);

    if (!bRemoved)
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("RemoveElement failed for '%s' (%s) - not found?"), *ElementName, *ElementTypeStr));

    FString SaveErr;
    const bool bSaved = ControlRig_CompileAndSave(CRBP, SaveErr);

    TSharedPtr<FJsonObject> Resp = ControlRig_StartReply(CRBP);
    Resp->SetStringField(TEXT("element_key"), ElementName);
    Resp->SetStringField(TEXT("element_type"), RigElementTypeToString(ElemType));
    Resp->SetBoolField(TEXT("removed"), bRemoved);
    FillHierarchyCounts(CRBP->GetHierarchy(), Resp, nullptr);
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("remove_rig_element"), SaveErr);
}

// ---------------------------------------------------------------------------
// rename_rig_element
// ---------------------------------------------------------------------------
// MCP:rename_rig_element - Rename an element in a Control Rig's hierarchy.  Idempotent: renaming to the current name is a no-op ({ renamed:false, already_named:true }). The engine disambiguates name collisions with a numeric suffix rather than failing, reported honestly via name_adjusted.
//   @param control_rig_path - Path to the UControlRigBlueprint asset
//   @param element_type - Element type ("Bone"/"Null"/"Control"/"Curve"/"Socket"/"Connector")
//   @param element_name - Current name of the element to rename
//   @param new_name - Desired new name for the element
//   @returns { asset_path: string, old_name: string, element_key: string, element_type: string, renamed: bool, already_named: bool, name_adjusted: bool, bone_count: int, null_count: int, control_count: int, curve_count: int, other_count: int, total: int, saved: bool, save_error: string }

TSharedPtr<FJsonObject> HandleRenameRigElement(const TSharedPtr<FJsonObject>& Params)
{
    FString RigPath, ElementTypeStr, ElementName, NewName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("control_rig_path"), RigPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("element_type"), ElementTypeStr, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("element_name"), ElementName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("new_name"), NewName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UControlRigBlueprint* CRBP = nullptr;
    URigHierarchyController* Controller = nullptr;
    if (TSharedPtr<FJsonObject> Error = ControlRig_LoadController(RigPath, CRBP, Controller))
    {
        return Error;
    }

    const ERigElementType ElemType = StringToRigElementType(ElementTypeStr);
    if (ElemType == ERigElementType::None)
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Unknown element_type: %s (expected Bone/Null/Control/Curve/Socket/Connector)"), *ElementTypeStr));

    const FRigElementKey Key(FName(*ElementName), ElemType);
    if (Controller->GetHierarchy()->GetIndex(Key) == INDEX_NONE)
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Element '%s' (%s) not found in the rig hierarchy"), *ElementName, *ElementTypeStr));

    // Idempotent no-op: RenameElement returns an invalid key when the target name
    // equals the current name (internal early-out), so short-circuit before mutating.
    if (FName(*NewName) == FName(*ElementName))
    {
        TSharedPtr<FJsonObject> Resp = ControlRig_StartReply(CRBP);
        Resp->SetStringField(TEXT("old_name"), ElementName);
        Resp->SetStringField(TEXT("element_key"), ElementName);
        Resp->SetStringField(TEXT("element_type"), RigElementTypeToString(ElemType));
        Resp->SetBoolField(TEXT("renamed"), false);
        Resp->SetBoolField(TEXT("already_named"), true);
        Resp->SetBoolField(TEXT("name_adjusted"), false);
        FillHierarchyCounts(CRBP->GetHierarchy(), Resp, nullptr);
        return mcp_common::MakeOkReply(Resp);
    }

    const FRigElementKey NewKey = Controller->RenameElement(Key, FName(*NewName), /*bSetupUndo*/ false, /*bPrintPythonCommand*/ false);
    if (!NewKey.IsValid())
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("RenameElement failed for '%s' (%s)"), *ElementName, *ElementTypeStr));

    // RenameElement never fails on collision: it appends a numeric suffix via
    // GetSafeNewName. Report honestly when the applied name differs from the request.
    const bool bNameAdjusted = (NewKey.Name != FName(*NewName));

    FString SaveErr;
    const bool bSaved = ControlRig_CompileAndSave(CRBP, SaveErr);

    TSharedPtr<FJsonObject> Resp = ControlRig_StartReply(CRBP);
    Resp->SetStringField(TEXT("old_name"), ElementName);
    Resp->SetStringField(TEXT("element_key"), NewKey.Name.ToString());
    Resp->SetStringField(TEXT("element_type"), RigElementTypeToString(NewKey.Type));
    Resp->SetBoolField(TEXT("renamed"), true);
    Resp->SetBoolField(TEXT("already_named"), false);
    Resp->SetBoolField(TEXT("name_adjusted"), bNameAdjusted);
    FillHierarchyCounts(CRBP->GetHierarchy(), Resp, nullptr);
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("rename_rig_element"), SaveErr);
}

// ===========================================================================
// Edit an existing control after add_rig_control (shape/color/gizmo/
// limits + shape transform). Read-modify-write on the control's FRigControlSettings
// via URigHierarchyController::Get/SetControlSettings, and its gizmo placement via
// URigHierarchy::SetControlShapeTransform.
// ===========================================================================

// MCP:set_rig_control_settings - Edit an existing Control Rig control's shape/gizmo/color/limit-draw settings (the bits add_rig_control can't set afterwards).  Read-modify-write: reads the control's FRigControlSettings, applies the provided fields, writes back via URigHierarchyController::SetControlSettings, recompiles + saves, and returns the verified read-back. Only the supplied fields change.
//   @param control_rig_path - Path to the UControlRigBlueprint asset.
//   @param control_name - Name of an existing control element.
//   @param display_name - Optional display name (FName) for the control.
//   @param shape_name - Optional gizmo shape name (e.g. "Default", "Box", "Sphere", "Circle").
//   @param shape_color [array] - Optional shape color [R, G, B] or [R, G, B, A] floats (0..1).
//   @param shape_visible - Optional bool: whether the gizmo shape is visible in 3D.
//   @param draw_limits - Optional bool: whether to draw the control's limits in debug.
//   @returns { asset_path: string, control_name: string, settings_applied: { display_name, shape_name, shape_color: [float], shape_visible, draw_limits }, applied_count: int, saved: bool, save_error?: string }

TSharedPtr<FJsonObject> HandleSetRigControlSettings(const TSharedPtr<FJsonObject>& Params)
{
    FString ControlName;
    UControlRigBlueprint* CRBP = nullptr;
    URigHierarchyController* Controller = nullptr;
    URigHierarchy* Hierarchy = nullptr;
    FRigElementKey Key;
    if (TSharedPtr<FJsonObject> Error = ControlRig_ResolveControl(Params, ControlName, CRBP, Controller, Hierarchy, Key))
    {
        return Error;
    }

    FRigControlSettings Settings = Controller->GetControlSettings(Key);
    int32 AppliedCount = 0;

    FString Str;
    if (Params->TryGetStringField(TEXT("display_name"), Str)) { Settings.DisplayName = FName(*Str); ++AppliedCount; }
    if (Params->TryGetStringField(TEXT("shape_name"), Str))   { Settings.ShapeName = FName(*Str); ++AppliedCount; }
    bool B = false;
    if (Params->TryGetBoolField(TEXT("shape_visible"), B))    { Settings.bShapeVisible = B; ++AppliedCount; }
    if (Params->TryGetBoolField(TEXT("draw_limits"), B))      { Settings.bDrawLimits = B; ++AppliedCount; }

    const TArray<TSharedPtr<FJsonValue>>* ColorArr = nullptr;
    if (Params->TryGetArrayField(TEXT("shape_color"), ColorArr) && ColorArr && ColorArr->Num() >= 3)
    {
        // R, G and B are always given here; the default only supplies the alpha.
        Settings.ShapeColor = mcp_common::LinearColorFromJsonArray(ColorArr, FLinearColor::Black);
        ++AppliedCount;
    }

    if (AppliedCount == 0)
        return mcp_common::MakeErrorReply(
            TEXT("No settings provided. Supply at least one of: display_name, shape_name, shape_color, shape_visible, draw_limits"));

    if (!Controller->SetControlSettings(Key, Settings))
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("SetControlSettings failed for '%s'"), *ControlName));

    FString SaveErr;
    const bool bSaved = ControlRig_CompileAndSave(CRBP, SaveErr);

    // Verified read-back of the current settings.
    const FRigControlSettings Now = Controller->GetControlSettings(Key);
    TSharedPtr<FJsonObject> Applied = MakeShared<FJsonObject>();
    Applied->SetStringField(TEXT("display_name"), Now.DisplayName.ToString());
    Applied->SetStringField(TEXT("shape_name"), Now.ShapeName.ToString());
    Applied->SetBoolField(TEXT("shape_visible"), Now.bShapeVisible);
    Applied->SetBoolField(TEXT("draw_limits"), Now.bDrawLimits);
    Applied->SetArrayField(TEXT("shape_color"), mcp_common::ColorToJsonArray(Now.ShapeColor));

    TSharedPtr<FJsonObject> Resp = ControlRig_StartReply(CRBP);
    Resp->SetStringField(TEXT("control_name"), ControlName);
    Resp->SetObjectField(TEXT("settings_applied"), Applied);
    Resp->SetNumberField(TEXT("applied_count"), AppliedCount);
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("set_rig_control_settings"), SaveErr);
}

// MCP:set_rig_control_shape_transform - Set the local shape (gizmo) transform of an existing Control Rig control.  Reads the current local shape transform, overrides only the supplied location/rotation/scale, writes back via URigHierarchy::SetControlShapeTransform, recompiles + saves, and returns the verified read-back. This positions the manipulator gizmo relative to the control, independent of the control's value.
//   @param control_rig_path - Path to the UControlRigBlueprint asset.
//   @param control_name - Name of an existing control element.
//   @param location - Optional [X, Y, Z] gizmo location.
//   @param rotation - Optional [Pitch, Yaw, Roll] gizmo rotation.
//   @param scale - Optional [X, Y, Z] gizmo scale.
//   @returns { asset_path: string, control_name: string, shape_transform: { location: [float], rotation: [float], scale: [float] }, applied_count: int, saved: bool, save_error?: string }

TSharedPtr<FJsonObject> HandleSetRigControlShapeTransform(const TSharedPtr<FJsonObject>& Params)
{
    FString RigPath, ControlName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("control_rig_path"), RigPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("control_name"), ControlName)) { return Missing; }

    UControlRigBlueprint* CRBP = nullptr;
    if (TSharedPtr<FJsonObject> Error = ControlRig_LoadBlueprint(RigPath, CRBP))
    {
        return Error;
    }
    URigHierarchy* Hierarchy = CRBP->GetHierarchy();
    if (!Hierarchy)
        return mcp_common::MakeErrorReply(TEXT("Could not get hierarchy for the control rig"));

    const FRigElementKey Key(FName(*ControlName), ERigElementType::Control);
    if (Hierarchy->GetIndex(Key) == INDEX_NONE)
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Control '%s' not found in the rig hierarchy"), *ControlName));

    // Read-modify-write the current LOCAL shape transform (what SetControlShapeTransform sets).
    FTransform Xform = Hierarchy->GetLocalControlShapeTransform(Key, /*bInitial=*/false);
    int32 AppliedCount = 0;
    if (Params->HasField(TEXT("location")))
    {
        Xform.SetLocation(mcp_common::ReadJsonVector(Params, TEXT("location")));
        ++AppliedCount;
    }
    if (Params->HasField(TEXT("rotation")))
    {
        Xform.SetRotation(FQuat(mcp_common::ReadJsonRotator(Params, TEXT("rotation"))));
        ++AppliedCount;
    }
    if (Params->HasField(TEXT("scale")))
    {
        Xform.SetScale3D(mcp_common::ReadJsonVector(Params, TEXT("scale")));
        ++AppliedCount;
    }

    if (AppliedCount == 0)
        return mcp_common::MakeErrorReply(
            TEXT("No transform provided. Supply at least one of: location, rotation, scale"));

    Hierarchy->SetControlShapeTransform(Key, Xform, /*bInitial=*/false);

    FString SaveErr;
    const bool bSaved = ControlRig_CompileAndSave(CRBP, SaveErr);

    // Verified read-back.
    const FTransform Now = Hierarchy->GetLocalControlShapeTransform(Key, /*bInitial=*/false);

    TSharedPtr<FJsonObject> Xf = MakeShared<FJsonObject>();
    Xf->SetArrayField(TEXT("location"), mcp_common::VectorToJsonArray(Now.GetLocation()));
    Xf->SetArrayField(TEXT("rotation"), mcp_common::RotatorToJsonArray(Now.GetRotation().Rotator()));
    Xf->SetArrayField(TEXT("scale"), mcp_common::VectorToJsonArray(Now.GetScale3D()));

    TSharedPtr<FJsonObject> Resp = ControlRig_StartReply(CRBP);
    Resp->SetStringField(TEXT("control_name"), ControlName);
    Resp->SetObjectField(TEXT("shape_transform"), Xf);
    Resp->SetNumberField(TEXT("applied_count"), AppliedCount);
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("set_rig_control_shape_transform"), SaveErr);
}

// ===========================================================================
// Control value + limits.
// add_rig_control seeded the control with the identity value and all-off limits;
// set_rig_control_settings did shape/color/visibility but explicitly deferred
// "real limits (FRigControlValue)". This authors the control's pose value and its
// min/max via the SAME controller/hierarchy path. The FRigControlValue MUST be built
// type-correctly per ERigControlType (a known gotcha) — reuse the
// switch already in MakeDefaultControlValue.
// ===========================================================================

// Build a type-correct FRigControlValue from Params[Field] for the given control type.
static bool MakeRigControlValueFromParams(
    const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, ERigControlType Type,
    FRigControlValue& Out, FString& Err)
{
    switch (Type)
    {
        case ERigControlType::Bool:
        {
            bool B = false;
            if (!Params->TryGetBoolField(Field, B)) { Err = FString::Printf(TEXT("'%s' (bool) required for a Bool control"), Field); return false; }
            Out = FRigControlValue::Make<bool>(B);
            return true;
        }
        case ERigControlType::Float:
        case ERigControlType::ScaleFloat:
        {
            double D = 0.0;
            if (!Params->TryGetNumberField(Field, D)) { Err = FString::Printf(TEXT("'%s' (number) required for a Float control"), Field); return false; }
            Out = FRigControlValue::Make<float>((float)D);
            return true;
        }
        case ERigControlType::Integer:
        {
            double D = 0.0;
            if (!Params->TryGetNumberField(Field, D)) { Err = FString::Printf(TEXT("'%s' (number) required for an Integer control"), Field); return false; }
            Out = FRigControlValue::Make<int32>((int32)D);
            return true;
        }
        case ERigControlType::Vector2D:
        case ERigControlType::Position:
        case ERigControlType::Scale:
        case ERigControlType::Rotator:
        {
            const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
            if (!Params->TryGetArrayField(Field, Arr) || !Arr || Arr->Num() < 2)
            { Err = FString::Printf(TEXT("'%s' expects [x, y(, z)] for a %s control"), Field, *RigControlTypeToString(Type)); return false; }
            const float X = (float)(*Arr)[0]->AsNumber();
            const float Y = (float)(*Arr)[1]->AsNumber();
            const float Z = Arr->Num() >= 3 ? (float)(*Arr)[2]->AsNumber() : 0.f;
            Out = FRigControlValue::Make<FVector3f>(FVector3f(X, Y, Z));
            return true;
        }
        default:
            Err = FString::Printf(TEXT("value/limits authoring is not supported for control type %s (use Bool/Float/ScaleFloat/Integer/Vector2D/Position/Scale/Rotator)"), *RigControlTypeToString(Type));
            return false;
    }
}

// Serialize an FRigControlValue into Out[Field], typed per ERigControlType.
static void SerializeRigControlValue(
    const FRigControlValue& V, ERigControlType Type, const TSharedPtr<FJsonObject>& Out, const TCHAR* Field)
{
    switch (Type)
    {
        case ERigControlType::Bool:
            Out->SetBoolField(Field, V.Get<bool>());
            break;
        case ERigControlType::Float:
        case ERigControlType::ScaleFloat:
            Out->SetNumberField(Field, V.Get<float>());
            break;
        case ERigControlType::Integer:
            Out->SetNumberField(Field, V.Get<int32>());
            break;
        case ERigControlType::Vector2D:
        case ERigControlType::Position:
        case ERigControlType::Scale:
        case ERigControlType::Rotator:
        {
            const FVector3f F = V.Get<FVector3f>();
            Out->SetArrayField(Field, mcp_common::VectorToJsonArray(F));
            break;
        }
        default:
            Out->SetStringField(Field, TEXT("(unsupported control type for value readout)"));
            break;
    }
}

// MCP:set_rig_control_value - Set a Control Rig control's pose value (Initial + Current), type-correct per the control's ERigControlType.  add_rig_control only seeds the identity value. SetControlValue applies the control's limits, so a value outside an enabled min/max is clamped (set limits first with set_rig_control_limits). Verified read-back.
//   @param control_rig_path - Path to the UControlRigBlueprint asset.
//   @param control_name - Name of an existing control element.
//   @param value - The value: bool (Bool), number (Float/ScaleFloat/Integer), or [x,y(,z)] (Vector2D/Position/Scale/Rotator).
//   @returns { asset_path: string, control_name: string, control_type: string, value: any, saved: bool, save_error?: string }
TSharedPtr<FJsonObject> HandleSetRigControlValue(const TSharedPtr<FJsonObject>& Params)
{
    // value is needed whatever the control type (MakeRigControlValueFromParams fails without it). A raw read in a
    // guard, so the catalog publishes it as required and of any JSON type: its type follows the control's.
    if (!Params->TryGetField(TEXT("value")))
    {
        return mcp_common::MissingParameterReply(TEXT("value"));
    }

    FString ControlName;
    UControlRigBlueprint* CRBP = nullptr;
    URigHierarchyController* Controller = nullptr;
    URigHierarchy* Hierarchy = nullptr;
    FRigElementKey Key;
    if (TSharedPtr<FJsonObject> Error = ControlRig_ResolveControl(Params, ControlName, CRBP, Controller, Hierarchy, Key))
    {
        return Error;
    }

    const ERigControlType Type = Controller->GetControlSettings(Key).ControlType;

    FRigControlValue Value;
    FString Err;
    if (!MakeRigControlValueFromParams(Params, TEXT("value"), Type, Value, Err))
        return mcp_common::MakeErrorReply(Err);

    // Author both Initial (persisted authoring value) and Current. SetControlValue
    // applies the control's limits, so an out-of-range value is clamped.
    Hierarchy->SetControlValue(Key, Value, ERigControlValueType::Initial, /*bSetupUndo*/ false);
    Hierarchy->SetControlValue(Key, Value, ERigControlValueType::Current, /*bSetupUndo*/ false);

    FString SaveErr;
    const bool bSaved = ControlRig_CompileAndSave(CRBP, SaveErr);

    // Verified read-back of the current (clamped) value.
    const FRigControlValue Now = Hierarchy->GetControlValue(Key, ERigControlValueType::Current);
    TSharedPtr<FJsonObject> Resp = ControlRig_StartReply(CRBP);
    Resp->SetStringField(TEXT("control_name"), ControlName);
    Resp->SetStringField(TEXT("control_type"), RigControlTypeToString(Type));
    SerializeRigControlValue(Now, Type, Resp, TEXT("value"));
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("set_rig_control_value"), SaveErr);
}

// MCP:set_rig_control_limits - Set a scalar Control Rig control's min/max limits (Float/ScaleFloat/Integer).  Enables the limit array for the control type and sets MinimumValue/MaximumValue type-correctly; once enabled, set_rig_control_value clamps via ApplyLimits. Verified read-back.
//   @param control_rig_path - Path to the UControlRigBlueprint asset.
//   @param control_name - Name of an existing control element (Float/ScaleFloat/Integer).
//   @param min - Minimum value (number).
//   @param max - Maximum value (number).
//   @param enable - Whether to enable the limits (optional, default true).
//   @returns { asset_path: string, control_name: string, control_type: string, min: number, max: number, limit_enabled: bool, saved: bool, save_error?: string }
TSharedPtr<FJsonObject> HandleSetRigControlLimits(const TSharedPtr<FJsonObject>& Params)
{
    FString ControlName;
    UControlRigBlueprint* CRBP = nullptr;
    URigHierarchyController* Controller = nullptr;
    URigHierarchy* Hierarchy = nullptr;
    FRigElementKey Key;
    if (TSharedPtr<FJsonObject> Error = ControlRig_ResolveControl(Params, ControlName, CRBP, Controller, Hierarchy, Key))
    {
        return Error;
    }

    FRigControlSettings Settings = Controller->GetControlSettings(Key);
    const ERigControlType Type = Settings.ControlType;

    // Scalar-only for this command: min/max are single numbers.
    if (Type != ERigControlType::Float && Type != ERigControlType::ScaleFloat && Type != ERigControlType::Integer)
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("set_rig_control_limits supports scalar controls (Float/ScaleFloat/Integer); '%s' is %s"), *ControlName, *RigControlTypeToString(Type)));

    double MinD = 0.0, MaxD = 0.0;
    if (!Params->TryGetNumberField(TEXT("min"), MinD))
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: min (number)"));
    if (!Params->TryGetNumberField(TEXT("max"), MaxD))
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: max (number)"));

    bool bEnable = true;
    Params->TryGetBoolField(TEXT("enable"), bEnable);

    // Enable/disable the limit array for this control type, then set min/max type-correctly.
    Settings.SetupLimitArrayForType(bEnable, bEnable, bEnable);
    if (Type == ERigControlType::Integer)
    {
        Settings.MinimumValue = FRigControlValue::Make<int32>((int32)MinD);
        Settings.MaximumValue = FRigControlValue::Make<int32>((int32)MaxD);
    }
    else
    {
        Settings.MinimumValue = FRigControlValue::Make<float>((float)MinD);
        Settings.MaximumValue = FRigControlValue::Make<float>((float)MaxD);
    }

    if (!Controller->SetControlSettings(Key, Settings))
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("SetControlSettings failed for '%s'"), *ControlName));

    FString SaveErr;
    const bool bSaved = ControlRig_CompileAndSave(CRBP, SaveErr);

    // Verified read-back.
    const FRigControlSettings Now = Controller->GetControlSettings(Key);
    const bool bLimitEnabled = Now.LimitEnabled.Num() > 0 && (Now.LimitEnabled[0].bMinimum || Now.LimitEnabled[0].bMaximum);
    const double MinNow = (Type == ERigControlType::Integer) ? (double)Now.MinimumValue.Get<int32>() : (double)Now.MinimumValue.Get<float>();
    const double MaxNow = (Type == ERigControlType::Integer) ? (double)Now.MaximumValue.Get<int32>() : (double)Now.MaximumValue.Get<float>();

    TSharedPtr<FJsonObject> Resp = ControlRig_StartReply(CRBP);
    Resp->SetStringField(TEXT("control_name"), ControlName);
    Resp->SetStringField(TEXT("control_type"), RigControlTypeToString(Type));
    Resp->SetNumberField(TEXT("min"), MinNow);
    Resp->SetNumberField(TEXT("max"), MaxNow);
    Resp->SetBoolField(TEXT("limit_enabled"), bLimitEnabled);
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("set_rig_control_limits"), SaveErr);
}

// MCP:get_rig_control_value - Read a Control Rig control's current value (typed per ERigControlType) and its limit min/max.  Round-trips set_rig_control_value / set_rig_control_limits.
//   @param control_rig_path - Path to the UControlRigBlueprint asset.
//   @param control_name - Name of an existing control element.
//   @returns { asset_path: string, control_name: string, control_type: string, value: any, limit_enabled: bool, min: number?, max: number? }
TSharedPtr<FJsonObject> HandleGetRigControlValue(const TSharedPtr<FJsonObject>& Params)
{
    FString ControlName;
    UControlRigBlueprint* CRBP = nullptr;
    URigHierarchyController* Controller = nullptr;
    URigHierarchy* Hierarchy = nullptr;
    FRigElementKey Key;
    if (TSharedPtr<FJsonObject> Error = ControlRig_ResolveControl(Params, ControlName, CRBP, Controller, Hierarchy, Key))
    {
        return Error;
    }

    const FRigControlSettings Settings = Controller->GetControlSettings(Key);
    const ERigControlType Type = Settings.ControlType;
    const FRigControlValue Now = Hierarchy->GetControlValue(Key, ERigControlValueType::Current);

    TSharedPtr<FJsonObject> Resp = ControlRig_StartReply(CRBP);
    Resp->SetStringField(TEXT("control_name"), ControlName);
    Resp->SetStringField(TEXT("control_type"), RigControlTypeToString(Type));
    SerializeRigControlValue(Now, Type, Resp, TEXT("value"));

    const bool bLimitEnabled = Settings.LimitEnabled.Num() > 0 && (Settings.LimitEnabled[0].bMinimum || Settings.LimitEnabled[0].bMaximum);
    Resp->SetBoolField(TEXT("limit_enabled"), bLimitEnabled);
    if (Type == ERigControlType::Float || Type == ERigControlType::ScaleFloat)
    {
        Resp->SetNumberField(TEXT("min"), Settings.MinimumValue.Get<float>());
        Resp->SetNumberField(TEXT("max"), Settings.MaximumValue.Get<float>());
    }
    else if (Type == ERigControlType::Integer)
    {
        Resp->SetNumberField(TEXT("min"), Settings.MinimumValue.Get<int32>());
        Resp->SetNumberField(TEXT("max"), Settings.MaximumValue.Get<int32>());
    }
    return mcp_common::MakeOkReply(Resp);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_rig_bone"), &HandleAddRigBone },
    { TEXT("add_rig_control"), &HandleAddRigControl },
    { TEXT("add_rig_null"), &HandleAddRigNull },
    { TEXT("create_control_rig_blueprint"), &HandleCreateControlRigBlueprint },
    { TEXT("get_rig_control_value"), &HandleGetRigControlValue },
    { TEXT("get_rig_hierarchy"), &HandleGetRigHierarchy },
    { TEXT("remove_rig_element"), &HandleRemoveRigElement },
    { TEXT("rename_rig_element"), &HandleRenameRigElement },
    { TEXT("set_rig_control_limits"), &HandleSetRigControlLimits },
    { TEXT("set_rig_control_settings"), &HandleSetRigControlSettings },
    { TEXT("set_rig_control_shape_transform"), &HandleSetRigControlShapeTransform },
    { TEXT("set_rig_control_value"), &HandleSetRigControlValue },
};
} // namespace UEAgentControlRigCommands
