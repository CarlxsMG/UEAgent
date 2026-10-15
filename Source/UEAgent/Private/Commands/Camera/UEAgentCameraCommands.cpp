#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/Actor.h"
#include "GameFramework/SpringArmComponent.h"  // runtime-facing spring arm
#include "UObject/SavePackage.h"
#include "AssetRegistry/AssetRegistryModule.h"  // register new asset with the registry

// CineCamera & CameraRig includes
#include "CineCameraActor.h"
#include "CineCameraComponent.h"
#include "CineCameraSettings.h"
#include "CameraRig_Rail.h"
#include "CameraRig_Crane.h"
#include "Components/SplineComponent.h"
// Reflection over FPostProcessSettings (value props + bOverride_* companions)
#include "UObject/UnrealType.h"
// FScopedTransaction for undo-correctness on RootNode/property mutation.
#include "ScopedTransaction.h"
// Gameplay Camera System (Experimental plugin GameplayCameras): asset -> director -> component authoring.
#include "Core/CameraRigAsset.h"
#include "Core/CameraAsset.h"
#include "Core/CameraDirector.h"
#include "Core/CameraAssetReference.h"
#include "Directors/SingleCameraDirector.h"
#include "GameFramework/GameplayCameraComponent.h"
#include "Build/CameraBuildStatus.h"
#include "Core/CameraNode.h"          // UCameraNode (rig RootNode + node-authoring)
#include "Nodes/Common/ArrayCameraNode.h"  // composite rig node whose Children we populate
#include "UObject/Package.h"
#include "Commands/Camera/ClassReflectionUtils.h"  // property writes by reflection
// Camera shake Blueprint authoring.
#include "Camera/CameraShakeBase.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EditorAssetLibrary.h"

/**
 * Camera commands: plain camera actors, cine cameras and camera rigs, gameplay camera components, camera
 * shake Blueprints, and the assets of the Gameplay Camera System.
 */
namespace UEAgentCameraCommands
{

// MCP-DOMAIN: Camera

// ============================================================================
// Gameplay Camera System (Experimental plugin GameplayCameras) — asset+director+component
// ============================================================================

namespace
{
    // Reads name (required) and package_path (default "/Game/Cameras"), then creates the AssetClass asset there and
    // saves it, or finds the one already there. Returns the reply data, or nullptr with OutError set; ClassLabel
    // names the class in the error of a failed creation.
    TSharedPtr<FJsonObject> GC_CreateAsset(const TSharedPtr<FJsonObject>& Params, UClass* AssetClass, const TCHAR* ClassLabel,
                                           bool& bOutSaved, FString& OutError)
    {
        FString Name;
        if (!Params->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
        {
            OutError = TEXT("Missing 'name'");
            return nullptr;
        }
        FString PackagePath;
        Params->TryGetStringField(TEXT("package_path"), PackagePath);

        auto CreationFailed = [&OutError, ClassLabel](const FString& Reason)
        {
            OutError = FString::Printf(TEXT("Failed to create %s: %s"), ClassLabel, *Reason);
            return TSharedPtr<FJsonObject>();
        };

        const FString Folder = PackagePath.IsEmpty() ? FString(TEXT("/Game/Cameras")) : PackagePath;
        const FString PkgPath = Folder / Name;
        UObject* Asset = mcp_common::ResolveObjectByPath(PkgPath, AssetClass);
        const bool bAlreadyExisted = Asset != nullptr && Asset->IsA(AssetClass);
        if (bAlreadyExisted)
        {
            bOutSaved = true;
        }
        else
        {
            const FString Conflict = mcp_common::AssetNameConflict(PkgPath, Name, AssetClass);
            if (!Conflict.IsEmpty())
            {
                return CreationFailed(Conflict);
            }
            UPackage* Package = CreatePackage(*PkgPath);
            if (!Package)
            {
                return CreationFailed(TEXT("CreatePackage failed"));
            }
            Asset = NewObject<UObject>(Package, AssetClass, *Name, RF_Public | RF_Standalone | RF_Transactional);
            if (!Asset)
            {
                return CreationFailed(TEXT("NewObject failed"));
            }
            FAssetRegistryModule::AssetCreated(Asset);
            FString SaveError;
            bOutSaved = mcp_common::SaveAssetPackage(Package, Asset, SaveError);
        }

        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("name"), Name);
        Data->SetStringField(TEXT("asset_path"), Asset->GetPathName());
        Data->SetStringField(TEXT("asset_class"), Asset->GetClass()->GetName());
        Data->SetBoolField(TEXT("already_existed"), bAlreadyExisted);
        return Data;
    }

    // Marks Asset's package dirty and saves it. The commands that call this do not report the save.
    void GC_MarkDirtyAndSave(UObject* Asset)
    {
        Asset->MarkPackageDirty();
        FString SaveError;
        if (UPackage* Package = Asset->GetPackage())
        {
            mcp_common::SaveAssetPackage(Package, Asset, SaveError);
        }
    }
}

// MCP:create_camera_rig_asset - Creates a UCameraRigAsset (Gameplay Camera System, Experimental). Idempotent (returns the existing asset).
//   @param name - Asset name (required)
//   @param package_path - Target content folder (default "/Game/Cameras")
//   @returns { name, asset_path, asset_class, already_existed, saved }
TSharedPtr<FJsonObject> HandleCreateCameraRigAsset(const TSharedPtr<FJsonObject>& Params)
{
    bool bSaved = false;
    FString Error;
    const TSharedPtr<FJsonObject> Data =
        GC_CreateAsset(Params, UCameraRigAsset::StaticClass(), TEXT("UCameraRigAsset"), bSaved, Error);
    if (!Data.IsValid())
    {
        return mcp_common::MakeErrorReply(Error);
    }
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("create_camera_rig_asset"));
}

// MCP:create_camera_asset - Create a UCameraAsset (Gameplay Camera System, Experimental plugin) at package_path/name and save it; one already there is returned unchanged (already_existed:true), another asset class there is an error. Returns asset_path and saved. Next: set_camera_asset_director, then add_gameplay_camera_component.
//   @param name - Name of the UCameraAsset to create, e.g. "CA_Player"; also the last segment of its path
//   @param package_path - Target content folder (default "/Game/Cameras")
//   @returns { name, asset_path, asset_class, already_existed, saved }
TSharedPtr<FJsonObject> HandleCreateCameraAsset(const TSharedPtr<FJsonObject>& Params)
{
    bool bSaved = false;
    FString Error;
    const TSharedPtr<FJsonObject> Data =
        GC_CreateAsset(Params, UCameraAsset::StaticClass(), TEXT("UCameraAsset"), bSaved, Error);
    if (!Data.IsValid())
    {
        return mcp_common::MakeErrorReply(Error);
    }
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("create_camera_asset"));
}

// MCP:set_camera_asset_director - Wires a USingleCameraDirector on a UCameraAsset, pointing it at a UCameraRigAsset (the "one rig" director). Verified via GetCameraDirector read-back.
//   @param camera_asset_path - /Game/... path to the UCameraAsset (required)
//   @param camera_rig_path - /Game/... path to the UCameraRigAsset the director runs (required)
//   @returns { camera_asset_path, camera_rig_path, director_class, director_set }
TSharedPtr<FJsonObject> HandleSetCameraAssetDirector(const TSharedPtr<FJsonObject>& Params)
{
    FString CamPath, RigPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("camera_asset_path"), CamPath)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("camera_rig_path"), RigPath)) { return Missing; }

    UCameraAsset* Cam = mcp_common::ResolveAsset<UCameraAsset>(CamPath);
    if (!Cam)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("UCameraAsset not found: %s"), *CamPath));
    UCameraRigAsset* Rig = mcp_common::ResolveAsset<UCameraRigAsset>(RigPath);
    if (!Rig)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("UCameraRigAsset not found: %s"), *RigPath));

    USingleCameraDirector* Director = NewObject<USingleCameraDirector>(Cam, USingleCameraDirector::StaticClass(), NAME_None, RF_Transactional);
    Director->CameraRig = Rig;
    Cam->SetCameraDirector(Director);
    const bool bSet = (Cam->GetCameraDirector() == Director);

    Cam->Modify();
    GC_MarkDirtyAndSave(Cam);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("camera_asset_path"), Cam->GetPathName());
    Data->SetStringField(TEXT("camera_rig_path"), Rig->GetPathName());
    Data->SetStringField(TEXT("director_class"), Director->GetClass()->GetName());
    Data->SetBoolField(TEXT("director_set"), bSet);
    return mcp_common::MakeOkReply(Data);
}

// MCP:build_camera_rig - Builds a UCameraRigAsset (UCameraRigAsset::BuildCameraRig) and reports the resulting build status.
//   @param camera_rig_path - /Game/... path to the UCameraRigAsset (required)
//   @returns { camera_rig_path, built, build_status }
TSharedPtr<FJsonObject> HandleBuildCameraRig(const TSharedPtr<FJsonObject>& Params)
{
    FString RigPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("camera_rig_path"), RigPath)) { return Missing; }
    UCameraRigAsset* Rig = mcp_common::ResolveAsset<UCameraRigAsset>(RigPath);
    if (!Rig)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("UCameraRigAsset not found: %s"), *RigPath));

    Rig->BuildCameraRig();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("camera_rig_path"), Rig->GetPathName());
    Data->SetBoolField(TEXT("built"), true);
    Data->SetStringField(TEXT("build_status"),
        StaticEnum<ECameraBuildStatus>()->GetNameStringByValue(static_cast<int64>(Rig->GetBuildStatus())));
    return mcp_common::MakeOkReply(Data);
}

// MCP:add_gameplay_camera_component - Adds a UGameplayCameraComponent to an actor and points it at a UCameraAsset. Idempotent. Note: activation needs a running game/PIE and is out of scope here.
//   @param actor_name - Display label or internal name of the target editor-world actor (required)
//   @param camera_asset_path - /Game/... path to the UCameraAsset to reference (required)
//   @returns { actor_name, component_name, already_existed, camera_asset_path, camera_set }
TSharedPtr<FJsonObject> HandleAddGameplayCameraComponent(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName, CamPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("camera_asset_path"), CamPath)) { return Missing; }

    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World)) { return NoWorld; }
    AActor* Actor = mcp_common::FindActorByName(World, ActorName);
    if (!Actor)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor '%s' not found"), *ActorName));
    UCameraAsset* Cam = mcp_common::ResolveAsset<UCameraAsset>(CamPath);
    if (!Cam)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("UCameraAsset not found: %s"), *CamPath));

    UGameplayCameraComponent* Comp = Actor->FindComponentByClass<UGameplayCameraComponent>();
    const bool bAE = (Comp != nullptr);
    if (!Comp)
    {
        Comp = NewObject<UGameplayCameraComponent>(Actor, UGameplayCameraComponent::StaticClass(), NAME_None, RF_Transactional);
        Actor->AddInstanceComponent(Comp);
        Comp->RegisterComponent();
        Actor->Modify();
    }
    Comp->CameraReference.SetCameraAsset(Cam);
    const bool bCamSet = (Comp->CameraReference.GetCameraAsset() == Cam);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("actor_name"), Actor->GetName());
    Data->SetStringField(TEXT("component_name"), Comp->GetName());
    Data->SetBoolField(TEXT("already_existed"), bAE);
    Data->SetStringField(TEXT("camera_asset_path"), Cam->GetPathName());
    Data->SetBoolField(TEXT("camera_set"), bCamSet);
    return mcp_common::MakeOkReply(Data);
}

// MCP:get_camera_asset_info - Reports a UCameraAsset's director and, for a SingleCameraDirector, the rig it runs. Read-only.
//   @param camera_asset_path - /Game/... path to the UCameraAsset (required)
//   @returns { camera_asset_path, has_director, director_class, camera_rig_path }
TSharedPtr<FJsonObject> HandleGetCameraAssetInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString CamPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("camera_asset_path"), CamPath)) { return Missing; }
    UCameraAsset* Cam = mcp_common::ResolveAsset<UCameraAsset>(CamPath);
    if (!Cam)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("UCameraAsset not found: %s"), *CamPath));

    UCameraDirector* Director = Cam->GetCameraDirector();
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("camera_asset_path"), Cam->GetPathName());
    Data->SetBoolField(TEXT("has_director"), Director != nullptr);
    Data->SetStringField(TEXT("director_class"), Director ? Director->GetClass()->GetName() : TEXT("None"));
    FString RigPath;
    if (USingleCameraDirector* Single = Cast<USingleCameraDirector>(Director))
    {
        RigPath = Single->CameraRig ? Single->CameraRig->GetPathName() : FString();
    }
    Data->SetStringField(TEXT("camera_rig_path"), RigPath);
    return mcp_common::MakeOkReply(Data);
}

// ============================================================================
// Gameplay Camera node-authoring — camera rig RootNode + FXxxCameraParameter.Value
// ============================================================================

namespace
{
    UClass* GC_ResolveCameraNodeClass(const FString& InName)
    {
        if (InName.IsEmpty()) { return nullptr; }
        UClass* Found = nullptr;
        if (InName.StartsWith(TEXT("/Script/"))) { Found = LoadObject<UClass>(nullptr, *InName); }
        if (!Found)
        {
            Found = UClass::TryFindTypeSlow<UClass>(InName);
            if (!Found && !InName.StartsWith(TEXT("U"))) { Found = UClass::TryFindTypeSlow<UClass>(FString(TEXT("U")) + InName); }
        }
        if (!Found) { Found = LoadObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/GameplayCameras.%s"), *InName)); }
        if (Found && Found->IsChildOf(UCameraNode::StaticClass())
            && !Found->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
        {
            return Found;
        }
        return nullptr;
    }

    // Set a camera-node property. FXxxCameraParameter structs carry the value in a nested
    // "Value" field; write that. Everything else falls to the canonical SetObjectProperty.
    bool GC_SetNodeProperty(UObject* Node, const FString& Key, const TSharedPtr<FJsonValue>& Val, FString& OutErr)
    {
        FProperty* Prop = Node->GetClass()->FindPropertyByName(*Key);
        if (!Prop) { OutErr = FString::Printf(TEXT("Property not found: %s"), *Key); return false; }
        FStructProperty* SP = CastField<FStructProperty>(Prop);
        if (SP && SP->Struct && SP->Struct->GetName().EndsWith(TEXT("CameraParameter")))
        {
            FProperty* Inner = SP->Struct->FindPropertyByName(TEXT("Value"));
            if (!Inner) { OutErr = TEXT("CameraParameter has no Value field"); return false; }
            void* InnerAddr = Inner->ContainerPtrToValuePtr<void>(SP->ContainerPtrToValuePtr<void>(Node));
            if (FBoolProperty* BP = CastField<FBoolProperty>(Inner))
            {
                BP->SetPropertyValue(InnerAddr, Val->AsBool());
                return true;
            }
            if (FNumericProperty* NP = CastField<FNumericProperty>(Inner))
            {
                if (NP->IsFloatingPoint()) { NP->SetFloatingPointPropertyValue(InnerAddr, Val->AsNumber()); }
                else { NP->SetIntPropertyValue(InnerAddr, static_cast<int64>(Val->AsNumber())); }
                return true;
            }
            OutErr = FString::Printf(TEXT("Unsupported CameraParameter Value type for '%s' (bool/numeric only)"), *Key);
            return false;
        }
        return mcp_common::SetObjectProperty(Node, Key, Val, OutErr);
    }
}

// MCP:list_camera_node_types - Lists all concrete UCameraNode subclasses usable in a camera rig. Read-only reflection (GetDerivedClasses).
//   @returns { base_class, count, types:[{name, path}] }
TSharedPtr<FJsonObject> HandleListCameraNodeTypes(const TSharedPtr<FJsonObject>& Params)
{
    const TArray<TSharedPtr<FJsonValue>> Types = mcp_common::ConcreteSubclassesToJson(UCameraNode::StaticClass());

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("base_class"), TEXT("CameraNode"));
    Data->SetNumberField(TEXT("count"), Types.Num());
    Data->SetArrayField(TEXT("types"), Types);
    return mcp_common::MakeOkReply(Data);
}

// MCP:set_camera_rig_root_node - Sets a UCameraRigAsset's RootNode to a new node of the given UCameraNode subclass.
//   @param camera_rig_path - /Game/... path to the UCameraRigAsset (required)
//   @param node_type - A concrete UCameraNode subclass name, e.g. "FieldOfViewCameraNode", "OffsetCameraNode", "ArrayCameraNode" (required)
//   @returns { camera_rig_path, node_type, node_class, root_set }
TSharedPtr<FJsonObject> HandleSetCameraRigRootNode(const TSharedPtr<FJsonObject>& Params)
{
    FString RigPath, NodeType;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("camera_rig_path"), RigPath)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("node_type"), NodeType)) { return Missing; }

    UCameraRigAsset* Rig = mcp_common::ResolveAsset<UCameraRigAsset>(RigPath);
    if (!Rig)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("UCameraRigAsset not found: %s"), *RigPath));
    UClass* NodeClass = GC_ResolveCameraNodeClass(NodeType);
    if (!NodeClass)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Could not resolve '%s' to a concrete UCameraNode subclass"), *NodeType));

    // Modify() must run inside an open transaction, before the mutation, or
    // nothing lands in the undo buffer (UObject::Modify only snapshots when GUndo
    // is set by a live FScopedTransaction).
    const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetCameraRigRootNode", "Set Camera Rig Root Node (MCP)"));
    Rig->Modify();
    UCameraNode* Node = NewObject<UCameraNode>(Rig, NodeClass, NAME_None, RF_Transactional);
    Rig->RootNode = Node;
    const bool bSet = (Rig->RootNode == Node);
    GC_MarkDirtyAndSave(Rig);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("camera_rig_path"), Rig->GetPathName());
    Data->SetStringField(TEXT("node_type"), NodeType);
    Data->SetStringField(TEXT("node_class"), NodeClass->GetName());
    Data->SetBoolField(TEXT("root_set"), bSet);
    return mcp_common::MakeOkReply(Data);
}

// MCP:set_camera_node_parameters - Sets parameters on a camera rig's RootNode by reflection. FXxxCameraParameter props take the raw value (written to the nested .Value); other props set directly. Reads the stored values back.
//   @param camera_rig_path - /Game/... path to the UCameraRigAsset whose RootNode is edited (required)
//   @param properties - JSON object { PropertyName: value, ... } (e.g. {"FieldOfView": 90})
//   @returns { camera_rig_path, node_class, set:[name], failed:[{name, error}], applied:{name: stored_value_text} }
TSharedPtr<FJsonObject> HandleSetCameraNodeParameters(const TSharedPtr<FJsonObject>& Params)
{
    FString RigPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("camera_rig_path"), RigPath)) { return Missing; }
    const TSharedPtr<FJsonObject>* PropsObj = nullptr;
    if (!Params->TryGetObjectField(TEXT("properties"), PropsObj) || !PropsObj || !PropsObj->IsValid())
        return mcp_common::MakeErrorReply(TEXT("Missing 'properties' object"));

    UCameraRigAsset* Rig = mcp_common::ResolveAsset<UCameraRigAsset>(RigPath);
    if (!Rig)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("UCameraRigAsset not found: %s"), *RigPath));
    UCameraNode* Node = Rig->RootNode;
    if (!Node)
        return mcp_common::MakeErrorReply(TEXT("The rig has no RootNode (call set_camera_rig_root_node first)"));

    // Same undo rule as set_camera_rig_root_node: Modify() before
    // the mutation loop, inside an open transaction.
    const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetCameraNodeParameters", "Set Camera Node Parameters (MCP)"));
    Node->Modify();
    Rig->Modify();

    const ClassReflectionUtils::FPropertyWrites Writes = ClassReflectionUtils::WriteProperties(Node, **PropsObj, &GC_SetNodeProperty);
    GC_MarkDirtyAndSave(Rig);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("camera_rig_path"), Rig->GetPathName());
    Data->SetStringField(TEXT("node_class"), Node->GetClass()->GetName());
    Data->SetArrayField(TEXT("set"), Writes.Set);
    Data->SetArrayField(TEXT("failed"), Writes.Failed);
    Data->SetObjectField(TEXT("applied"), Writes.Applied);
    return mcp_common::MakeOkReply(Data);
}

// MCP:add_camera_node_child - Populates the Children of a composite rig node (the rig RootNode must be a UArrayCameraNode ("Sequence"); set it via set_camera_rig_root_node node_type=ArrayCameraNode). Creates a new UCameraNode of child_node_type outered to the rig and appends it. Idempotent via optional index: index<Children.Num() REPLACES that slot instead of appending. Gameplay Camera System is EXPERIMENTAL (plugin GameplayCameras).
//   @param camera_rig_path - /Game/... path to the UCameraRigAsset whose composite RootNode is edited (required)
//   @param child_node_type - A concrete UCameraNode subclass name for the new child, e.g. "OffsetCameraNode", "FieldOfViewCameraNode" (required)
//   @param index - Optional child slot. If given and < current child count, REPLACES that slot (deterministic re-runs); otherwise appends.
//   @returns { camera_rig_path, child_node_type, child_class, child_index, children_count, added, replaced }
TSharedPtr<FJsonObject> HandleAddCameraNodeChild(const TSharedPtr<FJsonObject>& Params)
{
    FString RigPath, ChildType;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("camera_rig_path"), RigPath)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("child_node_type"), ChildType)) { return Missing; }

    // Optional idempotency index. Absent => append.
    int32 Index = INDEX_NONE;
    const bool bHasIndex = Params->TryGetNumberField(TEXT("index"), Index);

    UE_LOG(LogTemp, Verbose,
        TEXT("add_camera_node_child: rig='%s' child_type='%s' index=%s"),
        *RigPath, *ChildType, bHasIndex ? *FString::FromInt(Index) : TEXT("<append>"));

    UCameraRigAsset* Rig = mcp_common::ResolveAsset<UCameraRigAsset>(RigPath);
    if (!Rig)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("UCameraRigAsset not found: %s"), *RigPath));

    // MVP: the composite parent is the rig RootNode. Only UArrayCameraNode owns a child list.
    UArrayCameraNode* ArrayNode = Cast<UArrayCameraNode>(Rig->RootNode);
    if (!ArrayNode)
        return mcp_common::MakeErrorReply(
            TEXT("rig root is not a composite ArrayCameraNode; set it first via set_camera_rig_root_node with node_type=ArrayCameraNode"));

    UClass* ChildClass = GC_ResolveCameraNodeClass(ChildType);
    if (!ChildClass)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Could not resolve '%s' to a concrete UCameraNode subclass"), *ChildType));

    // Create the child outered to the rig (mirrors set_camera_rig_root_node); NewObject is required
    // because SetObjectProperty cannot construct an instanced UCameraNode inside the array.
    UCameraNode* NewChild = NewObject<UCameraNode>(Rig, ChildClass, NAME_None, RF_Transactional);
    if (!NewChild)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("NewObject failed for '%s'"), *ChildType));

    // ArrayNode->Children is a PUBLIC bare UPROPERTY TArray<TObjectPtr<UCameraNode>> (ArrayCameraNode.h) —
    // direct member access, NOT reflection, and NOT GetChildren() (returns a by-value view that would not persist).
    ArrayNode->Modify();
    Rig->Modify();

    bool bReplaced = false;
    int32 ChildIndex = INDEX_NONE;
    if (bHasIndex && Index >= 0 && Index < ArrayNode->Children.Num())
    {
        // REPLACE the slot => re-running with the same index is deterministic (no duplicate append).
        ArrayNode->Children[Index] = NewChild;
        bReplaced = true;
        ChildIndex = Index;
    }
    else
    {
        ArrayNode->Children.Add(NewChild);
        ChildIndex = ArrayNode->Children.Num() - 1;
    }
    const int32 ChildrenCount = ArrayNode->Children.Num();

    GC_MarkDirtyAndSave(Rig);

    UE_LOG(LogTemp, Verbose,
        TEXT("add_camera_node_child: %s child_class='%s' at index=%d, children_count=%d"),
        bReplaced ? TEXT("replaced") : TEXT("appended"), *ChildClass->GetName(), ChildIndex, ChildrenCount);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("camera_rig_path"), Rig->GetPathName());
    Data->SetStringField(TEXT("child_node_type"), ChildType);
    Data->SetStringField(TEXT("child_class"), ChildClass->GetName());
    Data->SetNumberField(TEXT("child_index"), ChildIndex);
    Data->SetNumberField(TEXT("children_count"), ChildrenCount);
    Data->SetBoolField(TEXT("added"), true);
    Data->SetBoolField(TEXT("replaced"), bReplaced);
    return mcp_common::MakeOkReply(Data);
}

// MCP:get_camera_rig_nodes - Reports a camera rig's RootNode class and its camera-parameter fields (name + value), plus, for a composite ArrayCameraNode root, its Children (index + class). Read-only.
//   @param camera_rig_path - /Game/... path to the UCameraRigAsset (required)
//   @returns { camera_rig_path, has_root_node, root_node_class, parameters:[{name, value}], children:[{index, class}] }
TSharedPtr<FJsonObject> HandleGetCameraRigNodes(const TSharedPtr<FJsonObject>& Params)
{
    FString RigPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("camera_rig_path"), RigPath)) { return Missing; }
    UCameraRigAsset* Rig = mcp_common::ResolveAsset<UCameraRigAsset>(RigPath);
    if (!Rig)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("UCameraRigAsset not found: %s"), *RigPath));

    UCameraNode* Node = Rig->RootNode;
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("camera_rig_path"), Rig->GetPathName());
    Data->SetBoolField(TEXT("has_root_node"), Node != nullptr);
    Data->SetStringField(TEXT("root_node_class"), Node ? Node->GetClass()->GetName() : TEXT("None"));

    TArray<TSharedPtr<FJsonValue>> ParamArr;
    if (Node)
    {
        for (TFieldIterator<FProperty> It(Node->GetClass()); It; ++It)
        {
            FStructProperty* SP = CastField<FStructProperty>(*It);
            if (!SP || !SP->Struct || !SP->Struct->GetName().EndsWith(TEXT("CameraParameter")))
                continue;
            FString ValStr;
            It->ExportTextItem_Direct(ValStr, It->ContainerPtrToValuePtr<void>(Node), nullptr, Node, PPF_None);
            TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
            P->SetStringField(TEXT("name"), It->GetName());
            P->SetStringField(TEXT("value"), ValStr);
            ParamArr.Add(MakeShared<FJsonValueObject>(P));
        }
    }
    Data->SetArrayField(TEXT("parameters"), ParamArr);

    // Enumerate composite children so add_camera_node_child writes can be verified.
    // Read the persistent Children UPROPERTY directly (not GetChildren(), whose by-value view is transient).
    TArray<TSharedPtr<FJsonValue>> ChildArr;
    if (UArrayCameraNode* ArrayNode = Cast<UArrayCameraNode>(Node))
    {
        for (int32 i = 0; i < ArrayNode->Children.Num(); ++i)
        {
            UCameraNode* Child = ArrayNode->Children[i];
            TSharedPtr<FJsonObject> C = MakeShared<FJsonObject>();
            C->SetNumberField(TEXT("index"), i);
            C->SetStringField(TEXT("class"), Child ? Child->GetClass()->GetName() : TEXT("None"));
            ChildArr.Add(MakeShared<FJsonValueObject>(C));
        }
    }
    Data->SetArrayField(TEXT("children"), ChildArr);
    return mcp_common::MakeOkReply(Data);
}

// ============================================================================
// Camera actors: create / list / transform / field of view
// ============================================================================

namespace UEAgentCameraImpl
{
    // Commands that place an actor refuse any coordinate beyond +/- this many units.
    constexpr double PlacementLimit = 100000.0;
    const TCHAR* const OutOfBoundsMessage = TEXT("Location coordinates must be between -100000.0 and 100000.0");

    FString MissingParameter(const TCHAR* Name)
    {
        return FString::Printf(TEXT("Missing required parameter: %s"), Name);
    }

    FString CameraNotFound(const FString& Name)
    {
        return FString::Printf(TEXT("Camera '%s' not found"), *Name);
    }

    bool IsWithinPlacementLimit(const FVector& Point)
    {
        for (int32 Axis = 0; Axis < 3; ++Axis)
        {
            if (Point[Axis] < -PlacementLimit || Point[Axis] > PlacementLimit)
            {
                return false;
            }
        }
        return true;
    }

    // Copies the first three numbers of a JSON array into A, B and C. A shorter array (which is
    // also what a missing or non-array field reads as) leaves all three untouched.
    void CopyTriple(const TArray<TSharedPtr<FJsonValue>>& Numbers, double& A, double& B, double& C)
    {
        if (Numbers.Num() < 3)
        {
            return;
        }
        A = Numbers[0]->AsNumber();
        B = Numbers[1]->AsNumber();
        C = Numbers[2]->AsNumber();
    }

    // The optional [X, Y, Z] location of a spawned camera rig: the origin unless it holds at least three numbers.
    FVector ReadRigLocation(const TSharedPtr<FJsonObject>& Params)
    {
        FVector Location(0.0f, 0.0f, 0.0f);
        const TArray<TSharedPtr<FJsonValue>>* Numbers = nullptr;
        if (Params->TryGetArrayField(TEXT("location"), Numbers) && Numbers)
        {
            CopyTriple(*Numbers, Location.X, Location.Y, Location.Z);
        }
        return Location;
    }

    TArray<TSharedPtr<FJsonValue>> JsonTriple(const double A, const double B, const double C)
    {
        TArray<TSharedPtr<FJsonValue>> Triple;
        Triple.Reserve(3);
        for (const double Value : { A, B, C })
        {
            Triple.Add(MakeShared<FJsonValueNumber>(Value));
        }
        return Triple;
    }

    // The error of a command that replies with its measured outcome: empty when bHolds, else
    // mcp_common::MeasuredOutcomeError. The measured verdict is the emitted one: a constant success:true would hide
    // it, and callers only check that flag.
    FString MeasuredFailure(const bool bHolds, const TCHAR* Command, const TCHAR* Outcome)
    {
        return bHolds ? FString() : mcp_common::MeasuredOutcomeError(Command, Outcome);
    }

    // Read-back tolerances: a position within 0.1 units, every rotation axis within 0.1 degrees.
    bool SameSpot(const FVector& Actual, const FVector& Wanted)
    {
        return FVector::Dist(Actual, Wanted) < 0.1f;
    }

    bool SameFacing(const FRotator& Actual, const FRotator& Wanted)
    {
        return FMath::IsNearlyEqual(Actual.Pitch, Wanted.Pitch, 0.1f)
            && FMath::IsNearlyEqual(Actual.Yaw, Wanted.Yaw, 0.1f)
            && FMath::IsNearlyEqual(Actual.Roll, Wanted.Roll, 0.1f);
    }

    // Object name first, then editor label; FString equality ignores case.
    bool IsNamed(const AActor* Actor, const FString& Name)
    {
        return Actor != nullptr && (Actor->GetName() == Name || Actor->GetActorLabel() == Name);
    }

    // One get_cameras entry: identity, transform and, when there is a camera component, its FOV.
    TSharedPtr<FJsonObject> DescribeCamera(ACameraActor* Camera)
    {
        const FVector Where = Camera->GetActorLocation();
        const FRotator Facing = Camera->GetActorRotation();

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Camera->GetName());
        Entry->SetStringField(TEXT("label"), Camera->GetActorLabel());
        Entry->SetBoolField(TEXT("is_valid"), IsValid(Camera));
        Entry->SetArrayField(TEXT("location"), JsonTriple(Where.X, Where.Y, Where.Z));
        Entry->SetArrayField(TEXT("rotation"), JsonTriple(Facing.Pitch, Facing.Yaw, Facing.Roll));
        if (const UCameraComponent* Lens = Camera->GetCameraComponent())
        {
            Entry->SetNumberField(TEXT("fov"), Lens->FieldOfView);
        }
        return Entry;
    }
}

// MCP:create_camera - Spawn a new camera actor in the editor world with optional initial transform.
//   @param camera_name - Name to assign to the new camera actor.
//   @param location - Optional [X, Y, Z] world location for the new camera (defaults to origin)
//   @param rotation - Optional [Pitch, Yaw, Roll] world rotation for the new camera (defaults to zero)
//   @returns { camera_name: string, camera_created: bool, actor_name: string, location_set: bool, rotation_set: bool, label_set: bool, actual_label: string, actual_location: [number, number, number] }

TSharedPtr<FJsonObject> HandleCreateCamera(const TSharedPtr<FJsonObject>& Params)
{
    FString Label;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("camera_name"), Label, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    // Both halves of the transform are optional: by default the camera spawns at the origin, unrotated.
    FVector SpawnAt(0, 0, 0);
    FRotator SpawnFacing(0, 0, 0);
    if (Params->HasField(TEXT("location")))
    {
        UEAgentCameraImpl::CopyTriple(Params->GetArrayField(TEXT("location")), SpawnAt.X, SpawnAt.Y, SpawnAt.Z);
    }
    if (Params->HasField(TEXT("rotation")))
    {
        UEAgentCameraImpl::CopyTriple(Params->GetArrayField(TEXT("rotation")), SpawnFacing.Pitch, SpawnFacing.Yaw, SpawnFacing.Roll);
    }

    if (Label.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("camera_name cannot be empty"));
    }
    if (!UEAgentCameraImpl::IsWithinPlacementLimit(SpawnAt))
    {
        return mcp_common::MakeErrorReply(UEAgentCameraImpl::OutOfBoundsMessage);
    }
    UWorld* EditorWorld = mcp_common::GetEditorWorld();
    if (EditorWorld == nullptr || !EditorWorld->IsValidLowLevel())
    {
        return mcp_common::MakeErrorReply(TEXT("No valid world found"));
    }

    ACameraActor* Spawned = EditorWorld->SpawnActor<ACameraActor>(SpawnAt, SpawnFacing);
    if (Spawned == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to create camera"));
    }
    Spawned->SetActorLabel(Label);

    // Everything below is read back from the spawned actor, not echoed from the request.
    const FVector LandedAt = Spawned->GetActorLocation();
    const FString LabelNow = Spawned->GetActorLabel();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("camera_name"), Label);
    Data->SetBoolField(TEXT("camera_created"), true);
    Data->SetStringField(TEXT("actor_name"), Spawned->GetName());
    Data->SetBoolField(TEXT("location_set"), UEAgentCameraImpl::SameSpot(LandedAt, SpawnAt));
    Data->SetBoolField(TEXT("rotation_set"), UEAgentCameraImpl::SameFacing(Spawned->GetActorRotation(), SpawnFacing));
    Data->SetBoolField(TEXT("label_set"), LabelNow == Label);
    Data->SetStringField(TEXT("actual_label"), LabelNow);
    Data->SetArrayField(TEXT("actual_location"), UEAgentCameraImpl::JsonTriple(LandedAt.X, LandedAt.Y, LandedAt.Z));
    return mcp_common::MakeOkReply(Data);
}

// MCP:get_cameras - List all camera actors in the editor world with their transform and FOV.
//   @param max_results - Maximum number of cameras to return (optional, default 1000, valid 1-10000)
//   @returns { cameras: [{ name: string, label: string, is_valid: bool, location: [number, number, number], rotation: [number, number, number], fov: number? }], total_count: number, max_results: number, truncated: bool }

TSharedPtr<FJsonObject> HandleGetCameras(const TSharedPtr<FJsonObject>& Params)
{
    int32 Limit = 1000;
    Params->TryGetNumberField(TEXT("max_results"), Limit);
    if (Limit <= 0 || Limit > 10000)
    {
        return mcp_common::MakeErrorReply(TEXT("max_results must be between 1 and 10000"));
    }
    UWorld* EditorWorld = mcp_common::GetEditorWorld();
    if (EditorWorld == nullptr || !EditorWorld->IsValidLowLevel())
    {
        return mcp_common::MakeErrorReply(TEXT("No valid world found"));
    }

    // The walk stops at the first camera past Limit, which is what makes the list truncated.
    TArray<TSharedPtr<FJsonValue>> Listed;
    bool bTruncated = false;
    for (TActorIterator<ACameraActor> Cam(EditorWorld); Cam; ++Cam)
    {
        if (!IsValid(*Cam))
        {
            continue;
        }
        if (Listed.Num() >= Limit)
        {
            bTruncated = true;
            break;
        }
        Listed.Add(MakeShared<FJsonValueObject>(UEAgentCameraImpl::DescribeCamera(*Cam)));
    }

    const int32 ListedCount = Listed.Num();
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("cameras"), Listed);
    Data->SetNumberField(TEXT("total_count"), ListedCount);
    Data->SetNumberField(TEXT("max_results"), Limit);
    Data->SetBoolField(TEXT("truncated"), bTruncated);
    return mcp_common::MakeOkReply(Data);
}

// Lookup helper shared by the handlers below: matches the object name or the editor label of a camera
// actor in the editor world.
ACameraActor* FindCameraByName(const FString& CameraName)
{
    UWorld* EditorWorld = mcp_common::GetEditorWorld();
    if (EditorWorld == nullptr)
    {
        return nullptr;
    }
    for (TActorIterator<ACameraActor> Cam(EditorWorld); Cam; ++Cam)
    {
        if (UEAgentCameraImpl::IsNamed(*Cam, CameraName))
        {
            return *Cam;
        }
    }
    return nullptr;
}

// MCP:set_camera_transform - Update the location and/or rotation of an existing camera.
//   @param camera_name - Name of the camera to modify (matches actor name or label).
//   @param location - Optional new [X, Y, Z] world location; if omitted, location is preserved
//   @param rotation - Optional new [Pitch, Yaw, Roll] world rotation; if omitted, rotation is preserved
//   @returns { camera_name: string, transform_set: bool, location_set: bool, rotation_set: bool, actual_location: [number, number, number], actual_rotation: [number, number, number] }

TSharedPtr<FJsonObject> HandleSetCameraTransform(const TSharedPtr<FJsonObject>& Params)
{
    FString Wanted;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("camera_name"), Wanted, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (Wanted.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("camera_name cannot be empty"));
    }
    ACameraActor* Camera = FindCameraByName(Wanted);
    if (Camera == nullptr)
    {
        return mcp_common::MakeErrorReply(UEAgentCameraImpl::CameraNotFound(Wanted));
    }

    // Start from the current transform so that an omitted half stays exactly where it is.
    FVector Target = Camera->GetActorLocation();
    FRotator TargetFacing = Camera->GetActorRotation();
    if (Params->HasField(TEXT("location")))
    {
        UEAgentCameraImpl::CopyTriple(Params->GetArrayField(TEXT("location")), Target.X, Target.Y, Target.Z);
    }
    if (Params->HasField(TEXT("rotation")))
    {
        UEAgentCameraImpl::CopyTriple(Params->GetArrayField(TEXT("rotation")), TargetFacing.Pitch, TargetFacing.Yaw, TargetFacing.Roll);
    }
    if (!UEAgentCameraImpl::IsWithinPlacementLimit(Target))
    {
        return mcp_common::MakeErrorReply(UEAgentCameraImpl::OutOfBoundsMessage);
    }

    Camera->SetActorLocationAndRotation(Target, TargetFacing);

    const FVector Now = Camera->GetActorLocation();
    const FRotator FacingNow = Camera->GetActorRotation();
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("camera_name"), Wanted);
    Data->SetBoolField(TEXT("transform_set"), true);
    Data->SetBoolField(TEXT("location_set"), UEAgentCameraImpl::SameSpot(Now, Target));
    Data->SetBoolField(TEXT("rotation_set"), UEAgentCameraImpl::SameFacing(FacingNow, TargetFacing));
    Data->SetArrayField(TEXT("actual_location"), UEAgentCameraImpl::JsonTriple(Now.X, Now.Y, Now.Z));
    Data->SetArrayField(TEXT("actual_rotation"), UEAgentCameraImpl::JsonTriple(FacingNow.Pitch, FacingNow.Yaw, FacingNow.Roll));
    return mcp_common::MakeOkReply(Data);
}

// MCP:get_camera_transform - Read the world location and rotation of a camera actor (ACameraActor or CineCameraActor) in the editor world, found by object name or label (case-insensitive, first match). Returns location [X, Y, Z] and rotation [Pitch, Yaw, Roll] in degrees; set_camera_transform is the writer.
//   @param camera_name - Name of the camera to query (matches actor name or label).
//   @returns { camera_name: string, location: [number, number, number], rotation: [number, number, number] }

TSharedPtr<FJsonObject> HandleGetCameraTransform(const TSharedPtr<FJsonObject>& Params)
{
    FString Wanted;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("camera_name"), Wanted, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    const ACameraActor* Camera = FindCameraByName(Wanted);
    if (Camera == nullptr)
    {
        return mcp_common::MakeErrorReply(UEAgentCameraImpl::CameraNotFound(Wanted));
    }

    const FVector Where = Camera->GetActorLocation();
    const FRotator Facing = Camera->GetActorRotation();
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("camera_name"), Wanted);
    Data->SetArrayField(TEXT("location"), UEAgentCameraImpl::JsonTriple(Where.X, Where.Y, Where.Z));
    Data->SetArrayField(TEXT("rotation"), UEAgentCameraImpl::JsonTriple(Facing.Pitch, Facing.Yaw, Facing.Roll));
    return mcp_common::MakeOkReply(Data);
}

// MCP:set_camera_fov - Set the field of view (degrees, above 0 up to 180) of the camera component of an ACameraActor or CineCameraActor in the editor world, found by object name or label. No undo transaction and nothing is saved. Returns previous_fov, actual_fov and fov_set; get_camera_fov reads it.
//   @param camera_name - Name of the camera to modify (matches actor name or label).
//   @param fov - Field of view in degrees. Must be between 0.0 and 180.0.
//   @returns { camera_name: string, requested_fov: number, previous_fov: number, actual_fov: number, fov_set: bool, camera_valid: bool }

TSharedPtr<FJsonObject> HandleSetCameraFOV(const TSharedPtr<FJsonObject>& Params)
{
    FString Wanted;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("camera_name"), Wanted, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    double RequestedFov = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("fov"), RequestedFov)) { return Missing; }
    if (Wanted.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("camera_name cannot be empty"));
    }
    if (RequestedFov <= 0.0 || RequestedFov > 180.0)
    {
        return mcp_common::MakeErrorReply(TEXT("FOV must be between 0.0 and 180.0 degrees"));
    }
    ACameraActor* Camera = FindCameraByName(Wanted);
    if (Camera == nullptr)
    {
        return mcp_common::MakeErrorReply(UEAgentCameraImpl::CameraNotFound(Wanted));
    }
    UCameraComponent* Lens = Camera->GetCameraComponent();
    if (Lens == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("Camera does not have a camera component"));
    }

    // The component stores a float; the read-back is compared at that precision.
    const float FovBefore = Lens->FieldOfView;
    const float FovApplied = static_cast<float>(RequestedFov);
    Lens->SetFieldOfView(FovApplied);
    Lens->MarkRenderStateDirty();
    const float FovNow = Lens->FieldOfView;

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("camera_name"), Wanted);
    Data->SetNumberField(TEXT("requested_fov"), RequestedFov);
    Data->SetNumberField(TEXT("previous_fov"), FovBefore);
    Data->SetNumberField(TEXT("actual_fov"), FovNow);
    Data->SetBoolField(TEXT("fov_set"), FMath::IsNearlyEqual(FovNow, FovApplied, 0.1f));
    Data->SetBoolField(TEXT("camera_valid"), IsValid(Lens));
    return mcp_common::MakeOkReply(Data);
}

// MCP:get_camera_fov - Retrieve the current field of view (FOV) of a camera.
//   @param camera_name - Name of the camera to query (matches actor name or label).
//   @returns { camera_name: string, fov: number }

TSharedPtr<FJsonObject> HandleGetCameraFOV(const TSharedPtr<FJsonObject>& Params)
{
    FString Wanted;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("camera_name"), Wanted, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    const ACameraActor* Camera = FindCameraByName(Wanted);
    if (Camera == nullptr)
    {
        return mcp_common::MakeErrorReply(UEAgentCameraImpl::CameraNotFound(Wanted));
    }

    // A camera actor without a camera component reports the 90-degree default.
    const UCameraComponent* Lens = Camera->GetCameraComponent();
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("camera_name"), Wanted);
    Data->SetNumberField(TEXT("fov"), Lens != nullptr ? Lens->FieldOfView : 90.0f);
    return mcp_common::MakeOkReply(Data);
}

// ============================================================================
// CineCamera & CameraRig command implementations
// ============================================================================

// Helper: locate a UCineCameraComponent on an actor found by name.
// Returns nullptr and sets OutError on failure.
static UCineCameraComponent* FindCineCameraComponent(const FString& ActorName, FString& OutError)
{
    UWorld* World = mcp_common::GetEditorWorld();
    if (!World)
    {
        OutError = TEXT("No valid editor world");
        return nullptr;
    }

    AActor* Actor = mcp_common::FindActorByName(World, ActorName);
    if (!Actor)
    {
        OutError = FString::Printf(TEXT("Actor '%s' not found"), *ActorName);
        return nullptr;
    }
    // Try ACineCameraActor first
    if (ACineCameraActor* CineActor = Cast<ACineCameraActor>(Actor))
    {
        UCineCameraComponent* Comp = CineActor->GetCineCameraComponent();
        if (Comp)
        {
            return Comp;
        }
    }
    // Fallback: search components
    UCineCameraComponent* Comp = Actor->FindComponentByClass<UCineCameraComponent>();
    if (Comp)
    {
        return Comp;
    }

    OutError = FString::Printf(TEXT("Actor '%s' has no CineCameraComponent"), *ActorName);
    return nullptr;
}

// ---------------------------------------------------------------------------
// 1. set_cine_camera_focal_length
// ---------------------------------------------------------------------------
// MCP:set_cine_camera_focal_length - Set the focal length (mm) on a CineCameraActor's CineCameraComponent.
//   @param camera_actor_name - Name or label of the CineCameraActor to modify.
//   @param focal_length - Focal length in millimetres. Must be between 0.0 and 1000.0.
//   @returns { camera_actor_name: string, focal_length: number, horizontal_fov: number, vertical_fov: number, success: bool }

TSharedPtr<FJsonObject> HandleSetCineCameraFocalLength(const TSharedPtr<FJsonObject>& Params)
{
    FString CameraActorName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("camera_actor_name"), CameraActorName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    double FocalLengthD = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("focal_length"), FocalLengthD)) { return Missing; }
    float FocalLength = static_cast<float>(FocalLengthD);

    if (CameraActorName.IsEmpty()) {
        return mcp_common::MakeErrorReply(TEXT("camera_actor_name cannot be empty"));
    }
    if (FocalLength <= 0.0f || FocalLength > 1000.0f) {
        return mcp_common::MakeErrorReply(TEXT("focal_length must be between 0.0 and 1000.0 mm"));
    }

    FString Error;
    UCineCameraComponent* CineComp = FindCineCameraComponent(CameraActorName, Error);
    if (!CineComp) {
        return mcp_common::MakeErrorReply(Error);
    }

    CineComp->SetCurrentFocalLength(FocalLength);

    float ActualFocalLength = CineComp->CurrentFocalLength;
    float HFOV = CineComp->GetHorizontalFieldOfView();
    float VFOV = CineComp->GetVerticalFieldOfView();
    bool FocalLengthSet = FMath::IsNearlyEqual(ActualFocalLength, FocalLength, 0.01f);

    TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetStringField(TEXT("camera_actor_name"), CameraActorName);
    Response->SetNumberField(TEXT("focal_length"), ActualFocalLength);
    Response->SetNumberField(TEXT("horizontal_fov"), HFOV);
    Response->SetNumberField(TEXT("vertical_fov"), VFOV);
    Response->SetBoolField(TEXT("success"), FocalLengthSet);
    return mcp_common::MakeOutcomeReply(Response, FocalLengthSet,
        UEAgentCameraImpl::MeasuredFailure(FocalLengthSet, TEXT("set_cine_camera_focal_length"), TEXT("FocalLengthSet")));
}

// ---------------------------------------------------------------------------
// 2. set_cine_camera_aperture
// ---------------------------------------------------------------------------
// MCP:set_cine_camera_aperture - Set the aperture (f-stop) on a CineCameraActor for depth-of-field control.
//   @param camera_actor_name - Name or label of the CineCameraActor to modify.
//   @param aperture - F-stop value. Must be between 0.01 and 512.0.
//   @returns { camera_actor_name: string, aperture: number, success: bool }

TSharedPtr<FJsonObject> HandleSetCineCameraAperture(const TSharedPtr<FJsonObject>& Params)
{
    FString CameraActorName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("camera_actor_name"), CameraActorName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    double ApertureD = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("aperture"), ApertureD)) { return Missing; }
    float Aperture = static_cast<float>(ApertureD);

    if (CameraActorName.IsEmpty()) {
        return mcp_common::MakeErrorReply(TEXT("camera_actor_name cannot be empty"));
    }
    if (Aperture < 0.01f || Aperture > 512.0f) {
        return mcp_common::MakeErrorReply(TEXT("aperture (f-stop) must be between 0.01 and 512.0"));
    }

    FString Error;
    UCineCameraComponent* CineComp = FindCineCameraComponent(CameraActorName, Error);
    if (!CineComp) {
        return mcp_common::MakeErrorReply(Error);
    }

    CineComp->SetCurrentAperture(Aperture);

    float ActualAperture = CineComp->CurrentAperture;
    bool ApertureSet = FMath::IsNearlyEqual(ActualAperture, Aperture, 0.001f);

    TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetStringField(TEXT("camera_actor_name"), CameraActorName);
    Response->SetNumberField(TEXT("aperture"), ActualAperture);
    Response->SetBoolField(TEXT("success"), ApertureSet);
    return mcp_common::MakeOutcomeReply(Response, ApertureSet,
        UEAgentCameraImpl::MeasuredFailure(ApertureSet, TEXT("set_cine_camera_aperture"), TEXT("ApertureSet")));
}

// ---------------------------------------------------------------------------
// 3. set_cine_camera_focus
// ---------------------------------------------------------------------------
// MCP:set_cine_camera_focus - Replace the focus settings of the CineCameraComponent on an editor-world actor found by name or label: method, manual focus distance and smoothing; other focus fields, including the tracked actor, reset to defaults. Returns the read-back focus_method and focus_distance; no undo, nothing saved.
//   @param camera_actor_name - Name or label of the CineCameraActor to modify.
//   @param focus_method - Focus method — one of "Manual", "Tracking", or "Disable".
//   @param focus_distance - Manual focus distance in cm (optional, default 1000.0)
//   @param smoothing_speed - Focus smoothing interpolation speed; 0 disables smoothing (optional, default 4.0)
//   @returns { camera_actor_name: string, focus_method: string, focus_distance: number, success: bool }

TSharedPtr<FJsonObject> HandleSetCineCameraFocus(const TSharedPtr<FJsonObject>& Params)
{
    FString CameraActorName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("camera_actor_name"), CameraActorName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    FString FocusMethodStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("focus_method"), FocusMethodStr, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    double FocusDistanceD = 1000.0;
    Params->TryGetNumberField(TEXT("focus_distance"), FocusDistanceD);
    float FocusDistance = static_cast<float>(FocusDistanceD);

    double SmoothingSpeedD = 4.0;
    Params->TryGetNumberField(TEXT("smoothing_speed"), SmoothingSpeedD);
    float SmoothingSpeed = static_cast<float>(SmoothingSpeedD);

    if (CameraActorName.IsEmpty()) {
        return mcp_common::MakeErrorReply(TEXT("camera_actor_name cannot be empty"));
    }

    // Parse focus method
    FCameraFocusSettings FocusSettings;
    FocusMethodStr = FocusMethodStr.ToLower();
    if (FocusMethodStr == TEXT("manual")) {
        FocusSettings.FocusMethod = ECameraFocusMethod::Manual;
    } else if (FocusMethodStr == TEXT("tracking")) {
        FocusSettings.FocusMethod = ECameraFocusMethod::Tracking;
    } else if (FocusMethodStr == TEXT("disable")) {
        FocusSettings.FocusMethod = ECameraFocusMethod::Disable;
    } else {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Invalid focus_method '%s'. Use Manual, Tracking, or Disable."), *FocusMethodStr));
    }
    FocusSettings.ManualFocusDistance = FocusDistance;
    FocusSettings.bSmoothFocusChanges = (SmoothingSpeed > 0.0f);
    FocusSettings.FocusSmoothingInterpSpeed = SmoothingSpeed;

    FString Error;
    UCineCameraComponent* CineComp = FindCineCameraComponent(CameraActorName, Error);
    if (!CineComp) {
        return mcp_common::MakeErrorReply(Error);
    }

    CineComp->SetFocusSettings(FocusSettings);

    const FCameraFocusSettings& ActualFocus = CineComp->FocusSettings;
    FString ActualMethodStr;
    switch (ActualFocus.FocusMethod) {
        case ECameraFocusMethod::Manual:   ActualMethodStr = TEXT("Manual"); break;
        case ECameraFocusMethod::Tracking: ActualMethodStr = TEXT("Tracking"); break;
        case ECameraFocusMethod::Disable:  ActualMethodStr = TEXT("Disable"); break;
        default:                           ActualMethodStr = TEXT("DoNotOverride"); break;
    }
    bool MethodSet = (ActualFocus.FocusMethod == FocusSettings.FocusMethod);
    bool DistanceSet = FMath::IsNearlyEqual(ActualFocus.ManualFocusDistance, FocusDistance, 0.1f);

    TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetStringField(TEXT("camera_actor_name"), CameraActorName);
    Response->SetStringField(TEXT("focus_method"), ActualMethodStr);
    Response->SetNumberField(TEXT("focus_distance"), ActualFocus.ManualFocusDistance);
    Response->SetBoolField(TEXT("success"), MethodSet && DistanceSet);
    return mcp_common::MakeOutcomeReply(Response, MethodSet && DistanceSet,
        UEAgentCameraImpl::MeasuredFailure(MethodSet && DistanceSet, TEXT("set_cine_camera_focus"), TEXT("MethodSet && DistanceSet")));
}

// ---------------------------------------------------------------------------
// 4. set_cine_camera_filmback
// ---------------------------------------------------------------------------
// MCP:set_cine_camera_filmback - Set the sensor size (filmback, in mm) of the CineCameraComponent on an editor-world actor found by name or label; sensor offsets are reset to 0. No undo transaction, nothing saved. Returns the read-back sensor_width, sensor_height and aspect_ratio; success is false when they do not match.
//   @param camera_actor_name - Name or label of the CineCameraActor to modify.
//   @param sensor_width - Sensor width in mm. Must be positive and <= 200.
//   @param sensor_height - Sensor height in mm. Must be positive and <= 200.
//   @returns { camera_actor_name: string, sensor_width: number, sensor_height: number, aspect_ratio: number, success: bool }

TSharedPtr<FJsonObject> HandleSetCineCameraFilmback(const TSharedPtr<FJsonObject>& Params)
{
    FString CameraActorName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("camera_actor_name"), CameraActorName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    double SensorWidthD = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("sensor_width"), SensorWidthD)) { return Missing; }

    double SensorHeightD = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("sensor_height"), SensorHeightD)) { return Missing; }

    float SensorWidth = static_cast<float>(SensorWidthD);
    float SensorHeight = static_cast<float>(SensorHeightD);

    if (CameraActorName.IsEmpty()) {
        return mcp_common::MakeErrorReply(TEXT("camera_actor_name cannot be empty"));
    }
    if (SensorWidth <= 0.0f || SensorHeight <= 0.0f) {
        return mcp_common::MakeErrorReply(TEXT("sensor_width and sensor_height must be positive"));
    }
    if (SensorWidth > 200.0f || SensorHeight > 200.0f) {
        return mcp_common::MakeErrorReply(TEXT("sensor dimensions must be <= 200 mm"));
    }

    FString Error;
    UCineCameraComponent* CineComp = FindCineCameraComponent(CameraActorName, Error);
    if (!CineComp) {
        return mcp_common::MakeErrorReply(Error);
    }

    FCameraFilmbackSettings Filmback;
    Filmback.SensorWidth = SensorWidth;
    Filmback.SensorHeight = SensorHeight;
    Filmback.SensorHorizontalOffset = 0.0f;
    Filmback.SensorVerticalOffset = 0.0f;
    CineComp->SetFilmback(Filmback);

    const FCameraFilmbackSettings& Actual = CineComp->Filmback;
    bool WidthSet = FMath::IsNearlyEqual(Actual.SensorWidth, SensorWidth, 0.01f);
    bool HeightSet = FMath::IsNearlyEqual(Actual.SensorHeight, SensorHeight, 0.01f);
    float AspectRatio = (Actual.SensorHeight > 0.0f) ? (Actual.SensorWidth / Actual.SensorHeight) : 0.0f;

    TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetStringField(TEXT("camera_actor_name"), CameraActorName);
    Response->SetNumberField(TEXT("sensor_width"), Actual.SensorWidth);
    Response->SetNumberField(TEXT("sensor_height"), Actual.SensorHeight);
    Response->SetNumberField(TEXT("aspect_ratio"), AspectRatio);
    Response->SetBoolField(TEXT("success"), WidthSet && HeightSet);
    return mcp_common::MakeOutcomeReply(Response, WidthSet && HeightSet,
        UEAgentCameraImpl::MeasuredFailure(WidthSet && HeightSet, TEXT("set_cine_camera_filmback"), TEXT("WidthSet && HeightSet")));
}

// ---------------------------------------------------------------------------
// 5. set_cine_camera_lens
// ---------------------------------------------------------------------------
// MCP:set_cine_camera_lens - Replace the lens limits of the CineCameraComponent on an editor-world actor found by name or label: focal length and f-stop ranges plus blade count; minimum focus distance resets to 0 and squeeze factor to 1. Returns the read-back focal range; no undo, nothing saved.
//   @param camera_actor_name - Name or label of the CineCameraActor to modify.
//   @param min_focal_length - Minimum focal length in mm. Must be positive.
//   @param max_focal_length - Maximum focal length in mm. Must be >= min_focal_length.
//   @param min_fstop - Minimum f-stop. Must be positive.
//   @param max_fstop - Maximum f-stop. Must be >= min_fstop.
//   @param diaphragm_blades - Number of diaphragm blades for bokeh shape (optional, default 8, valid 4-16)
//   @returns { camera_actor_name: string, min_focal_length: number, max_focal_length: number, success: bool }

TSharedPtr<FJsonObject> HandleSetCineCameraLens(const TSharedPtr<FJsonObject>& Params)
{
    FString CameraActorName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("camera_actor_name"), CameraActorName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    double MinFocalD = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("min_focal_length"), MinFocalD)) { return Missing; }

    double MaxFocalD = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("max_focal_length"), MaxFocalD)) { return Missing; }

    double MinFStopD = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("min_fstop"), MinFStopD)) { return Missing; }

    double MaxFStopD = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("max_fstop"), MaxFStopD)) { return Missing; }

    int32 DiaphragmBlades = 8;
    if (Params->HasField(TEXT("diaphragm_blades"))) {
        DiaphragmBlades = static_cast<int32>(Params->GetNumberField(TEXT("diaphragm_blades")));
    }

    float MinFocal = static_cast<float>(MinFocalD);
    float MaxFocal = static_cast<float>(MaxFocalD);
    float MinFStop = static_cast<float>(MinFStopD);
    float MaxFStop = static_cast<float>(MaxFStopD);

    if (CameraActorName.IsEmpty()) {
        return mcp_common::MakeErrorReply(TEXT("camera_actor_name cannot be empty"));
    }
    if (MinFocal <= 0.0f || MaxFocal <= 0.0f) {
        return mcp_common::MakeErrorReply(TEXT("focal lengths must be positive"));
    }
    if (MinFocal > MaxFocal) {
        return mcp_common::MakeErrorReply(TEXT("min_focal_length must be <= max_focal_length"));
    }
    if (MinFStop <= 0.0f || MaxFStop <= 0.0f) {
        return mcp_common::MakeErrorReply(TEXT("f-stop values must be positive"));
    }
    if (MinFStop > MaxFStop) {
        return mcp_common::MakeErrorReply(TEXT("min_fstop must be <= max_fstop"));
    }
    if (DiaphragmBlades < 4 || DiaphragmBlades > 16) {
        return mcp_common::MakeErrorReply(TEXT("diaphragm_blades must be between 4 and 16"));
    }

    FString Error;
    UCineCameraComponent* CineComp = FindCineCameraComponent(CameraActorName, Error);
    if (!CineComp) {
        return mcp_common::MakeErrorReply(Error);
    }

    FCameraLensSettings Lens;
    Lens.MinFocalLength = MinFocal;
    Lens.MaxFocalLength = MaxFocal;
    Lens.MinFStop = MinFStop;
    Lens.MaxFStop = MaxFStop;
    Lens.MinimumFocusDistance = 0.0f;
    Lens.SqueezeFactor = 1.0f;
    Lens.DiaphragmBladeCount = DiaphragmBlades;
    CineComp->SetLensSettings(Lens);

    const FCameraLensSettings& Actual = CineComp->LensSettings;
    bool MinFocalSet = FMath::IsNearlyEqual(Actual.MinFocalLength, MinFocal, 0.01f);
    bool MaxFocalSet = FMath::IsNearlyEqual(Actual.MaxFocalLength, MaxFocal, 0.01f);

    TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetStringField(TEXT("camera_actor_name"), CameraActorName);
    Response->SetNumberField(TEXT("min_focal_length"), Actual.MinFocalLength);
    Response->SetNumberField(TEXT("max_focal_length"), Actual.MaxFocalLength);
    Response->SetBoolField(TEXT("success"), MinFocalSet && MaxFocalSet);
    return mcp_common::MakeOutcomeReply(Response, MinFocalSet && MaxFocalSet,
        UEAgentCameraImpl::MeasuredFailure(MinFocalSet && MaxFocalSet, TEXT("set_cine_camera_lens"), TEXT("MinFocalSet && MaxFocalSet")));
}

// ---------------------------------------------------------------------------
// 6. create_camera_rig_rail
// ---------------------------------------------------------------------------
// MCP:create_camera_rig_rail - Spawn an unrotated CameraRig_Rail actor in the editor world; with more than 2 points the rail is rebuilt as a straight 500-unit line along local X. Always labelled "CameraRigRail_MCP", so address it by the returned internal_name, e.g. in set_camera_rig_rail_position. Not saved.
//   @param location - Optional [X, Y, Z] world location (defaults to origin; each axis within -100000 to 100000)
//   @param num_spline_points - Number of spline points on the rail (optional, default 2, valid 2-100)
//   @returns { actor_name: string, internal_name: string, location: [number, number, number], num_spline_points: number, success: bool }

TSharedPtr<FJsonObject> HandleCreateCameraRigRail(const TSharedPtr<FJsonObject>& Params)
{
    const FVector Location = UEAgentCameraImpl::ReadRigLocation(Params);

    int32 NumSplinePoints = 2;
    if (Params->HasField(TEXT("num_spline_points"))) {
        NumSplinePoints = static_cast<int32>(Params->GetNumberField(TEXT("num_spline_points")));
    }

    if (NumSplinePoints < 2 || NumSplinePoints > 100) {
        return mcp_common::MakeErrorReply(TEXT("num_spline_points must be between 2 and 100"));
    }
    if (!UEAgentCameraImpl::IsWithinPlacementLimit(Location)) {
        return mcp_common::MakeErrorReply(UEAgentCameraImpl::OutOfBoundsMessage);
    }

    UWorld* EditorWorld = mcp_common::GetEditorWorld();
    if (EditorWorld == nullptr || !EditorWorld->IsValidLowLevel()) {
        return mcp_common::MakeErrorReply(TEXT("No valid world found"));
    }

    // Spawn rig rail (unrotated)
    ACameraRig_Rail* RigRail = EditorWorld->SpawnActor<ACameraRig_Rail>(Location, FRotator::ZeroRotator);
    if (!RigRail) {
        return mcp_common::MakeErrorReply(TEXT("Failed to spawn CameraRig_Rail"));
    }

    // Configure spline (optional extra points)
    USplineComponent* Spline = RigRail->GetRailSplineComponent();
    if (Spline && NumSplinePoints > 2) {
        // Default rail has 2 points; add more evenly spaced along X
        float RailLength = 500.0f;
        Spline->ClearSplinePoints(false);
        for (int32 i = 0; i < NumSplinePoints; ++i) {
            float T = (NumSplinePoints > 1) ? static_cast<float>(i) / static_cast<float>(NumSplinePoints - 1) : 0.0f;
            FVector PointPos(T * RailLength - RailLength * 0.5f, 0.0f, 0.0f);
            Spline->AddSplinePoint(PointPos, ESplineCoordinateSpace::Local, false);
        }
        Spline->UpdateSpline();
    }

    // Name the actor
    FString ActorName = RigRail->GetName();
    RigRail->SetActorLabel(TEXT("CameraRigRail_MCP"));

    FVector ActualLoc = RigRail->GetActorLocation();
    bool LocationSet = FVector::Dist(ActualLoc, Location) < 0.1f;
    int32 ActualSplinePoints = Spline ? Spline->GetNumberOfSplinePoints() : 0;

    TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetStringField(TEXT("actor_name"), RigRail->GetActorLabel());
    Response->SetStringField(TEXT("internal_name"), ActorName);

    Response->SetArrayField(TEXT("location"), mcp_common::VectorToJsonArray(ActualLoc));

    Response->SetNumberField(TEXT("num_spline_points"), ActualSplinePoints);
    Response->SetBoolField(TEXT("success"), LocationSet);
    return mcp_common::MakeOutcomeReply(Response, LocationSet,
        UEAgentCameraImpl::MeasuredFailure(LocationSet, TEXT("create_camera_rig_rail"), TEXT("LocationSet")));
}

// ---------------------------------------------------------------------------
// 7. set_camera_rig_rail_position
// ---------------------------------------------------------------------------
// MCP:set_camera_rig_rail_position - Set the camera position along a CameraRig_Rail (0.0 = start, 1.0 = end).
//   @param rig_actor_name - Name or label of the CameraRig_Rail actor.
//   @param position - Normalised position on the rail, 0.0 to 1.0.
//   @returns { rig_actor_name: string, position: number, success: bool }

TSharedPtr<FJsonObject> HandleSetCameraRigRailPosition(const TSharedPtr<FJsonObject>& Params)
{
    FString RigActorName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("rig_actor_name"), RigActorName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    double PositionD = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("position"), PositionD)) { return Missing; }
    float Position = static_cast<float>(PositionD);

    if (RigActorName.IsEmpty()) {
        return mcp_common::MakeErrorReply(TEXT("rig_actor_name cannot be empty"));
    }
    if (Position < 0.0f || Position > 1.0f) {
        return mcp_common::MakeErrorReply(TEXT("position must be between 0.0 and 1.0"));
    }

    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World, TEXT("No valid world found"))) { return NoWorld; }

    ACameraRig_Rail* RigRail = nullptr;
    for (TActorIterator<ACameraRig_Rail> It(World); It; ++It) {
        ACameraRig_Rail* Candidate = *It;
        if (Candidate && (Candidate->GetName() == RigActorName || Candidate->GetActorLabel() == RigActorName)) {
            RigRail = Candidate;
            break;
        }
    }

    if (!RigRail) {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("CameraRig_Rail '%s' not found"), *RigActorName));
    }

    RigRail->CurrentPositionOnRail = Position;

    float ActualPosition = RigRail->CurrentPositionOnRail;
    bool PositionSet = FMath::IsNearlyEqual(ActualPosition, Position, 0.001f);

    TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetStringField(TEXT("rig_actor_name"), RigActorName);
    Response->SetNumberField(TEXT("position"), ActualPosition);
    Response->SetBoolField(TEXT("success"), PositionSet);
    return mcp_common::MakeOutcomeReply(Response, PositionSet,
        UEAgentCameraImpl::MeasuredFailure(PositionSet, TEXT("set_camera_rig_rail_position"), TEXT("PositionSet")));
}

// ---------------------------------------------------------------------------
// 8. create_camera_rig_crane
// ---------------------------------------------------------------------------
// MCP:create_camera_rig_crane - Spawn an unrotated CameraRig_Crane actor in the editor world with the given pitch, yaw and arm length; it is always labelled "CameraRigCrane_MCP", so address it later by the returned internal_name. Returns location and the read-back crane values; the level is not saved.
//   @param location - Optional [X, Y, Z] world location (defaults to origin; each axis within -100000 to 100000)
//   @param crane_pitch - Crane pitch angle in degrees (optional, default 0)
//   @param crane_yaw - Crane yaw angle in degrees (optional, default 0)
//   @param arm_length - Crane arm length (optional, default 500.0, valid 0-100000)
//   @returns { actor_name: string, internal_name: string, location: [number, number, number], crane_pitch: number, crane_yaw: number, arm_length: number, success: bool }

TSharedPtr<FJsonObject> HandleCreateCameraRigCrane(const TSharedPtr<FJsonObject>& Params)
{
    const FVector Location = UEAgentCameraImpl::ReadRigLocation(Params);

    double CranePitchD = 0.0;
    Params->TryGetNumberField(TEXT("crane_pitch"), CranePitchD);
    float CranePitch = static_cast<float>(CranePitchD);

    double CraneYawD = 0.0;
    Params->TryGetNumberField(TEXT("crane_yaw"), CraneYawD);
    float CraneYaw = static_cast<float>(CraneYawD);

    double ArmLengthD = 500.0;
    Params->TryGetNumberField(TEXT("arm_length"), ArmLengthD);
    float ArmLength = static_cast<float>(ArmLengthD);

    if (!UEAgentCameraImpl::IsWithinPlacementLimit(Location))
    {
        return mcp_common::MakeErrorReply(UEAgentCameraImpl::OutOfBoundsMessage);
    }
    if (ArmLength < 0.0f || ArmLength > 100000.0f) {
        return mcp_common::MakeErrorReply(TEXT("arm_length must be between 0.0 and 100000.0"));
    }

    UWorld* World = mcp_common::GetEditorWorld();
    if (!World || !World->IsValidLowLevel()) {
        return mcp_common::MakeErrorReply(TEXT("No valid world found"));
    }

    FRotator Rotation(0.0f, 0.0f, 0.0f);
    ACameraRig_Crane* RigCrane = World->SpawnActor<ACameraRig_Crane>(
        ACameraRig_Crane::StaticClass(), Location, Rotation);
    if (!RigCrane) {
        return mcp_common::MakeErrorReply(TEXT("Failed to spawn CameraRig_Crane"));
    }

    RigCrane->CranePitch = CranePitch;
    RigCrane->CraneYaw = CraneYaw;
    RigCrane->CraneArmLength = ArmLength;
    RigCrane->SetActorLabel(TEXT("CameraRigCrane_MCP"));

    FVector ActualLoc = RigCrane->GetActorLocation();
    bool LocationSet = FVector::Dist(ActualLoc, Location) < 0.1f;
    bool PitchSet = FMath::IsNearlyEqual(RigCrane->CranePitch, CranePitch, 0.01f);
    bool YawSet = FMath::IsNearlyEqual(RigCrane->CraneYaw, CraneYaw, 0.01f);
    bool ArmSet = FMath::IsNearlyEqual(RigCrane->CraneArmLength, ArmLength, 0.01f);

    TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetStringField(TEXT("actor_name"), RigCrane->GetActorLabel());
    Response->SetStringField(TEXT("internal_name"), RigCrane->GetName());

    Response->SetArrayField(TEXT("location"), mcp_common::VectorToJsonArray(ActualLoc));

    Response->SetNumberField(TEXT("crane_pitch"), RigCrane->CranePitch);
    Response->SetNumberField(TEXT("crane_yaw"), RigCrane->CraneYaw);
    Response->SetNumberField(TEXT("arm_length"), RigCrane->CraneArmLength);
    Response->SetBoolField(TEXT("success"), LocationSet && PitchSet && YawSet && ArmSet);
    return mcp_common::MakeOutcomeReply(Response, LocationSet && PitchSet && YawSet && ArmSet,
        UEAgentCameraImpl::MeasuredFailure(LocationSet && PitchSet && YawSet && ArmSet, TEXT("create_camera_rig_crane"),
            TEXT("LocationSet && PitchSet && YawSet && ArmSet")));
}

// ============================================================================
// CineCamera readers + PostProcessSettings read/write (reflection)
// ============================================================================
//
// A CineCameraComponent owns Filmback/LensSettings/FocusSettings + the inherited
// FPostProcessSettings (from UCameraComponent). The PP write path mirrors how a
// PostProcessVolume is authored: each value property X has a companion bitfield
// bOverride_X that must be turned on for the value to take effect. In
// FPostProcessSettings every value UPROPERTY declares meta=(editcondition="bOverride_X"),
// so the companion is discovered by reflection (canonical link) rather than by
// string-munging the name. Writes are absolute (idempotent: re-applying the same
// value yields the same state).

namespace
{
    // Public snake_case PP field name  ->  FPostProcessSettings UPROPERTY name.
    // Covers the scalar/color knobs an LLM reaches for; the setter/reader are
    // reflection-driven, so extending this table is the only change needed to
    // expose more fields.
    struct FPPFieldMap { const TCHAR* Json; const TCHAR* Prop; };
    static const FPPFieldMap GPPFields[] =
    {
        // Bloom
        { TEXT("bloom_intensity"),                TEXT("BloomIntensity") },
        { TEXT("bloom_threshold"),                TEXT("BloomThreshold") },
        // Lens image effects
        { TEXT("vignette_intensity"),             TEXT("VignetteIntensity") },
        { TEXT("film_grain_intensity"),           TEXT("FilmGrainIntensity") },
        // Chromatic aberration (a.k.a. scene fringe)
        { TEXT("chromatic_aberration_intensity"), TEXT("SceneFringeIntensity") },
        { TEXT("chromatic_aberration_start_offset"), TEXT("ChromaticAberrationStartOffset") },
        // Exposure
        { TEXT("exposure_bias"),                  TEXT("AutoExposureBias") },
        { TEXT("exposure_min_brightness"),        TEXT("AutoExposureMinBrightness") },
        { TEXT("exposure_max_brightness"),        TEXT("AutoExposureMaxBrightness") },
        { TEXT("exposure_speed_up"),              TEXT("AutoExposureSpeedUp") },
        { TEXT("exposure_speed_down"),            TEXT("AutoExposureSpeedDown") },
        // Ambient occlusion
        { TEXT("ambient_occlusion_intensity"),    TEXT("AmbientOcclusionIntensity") },
        { TEXT("ambient_occlusion_radius"),       TEXT("AmbientOcclusionRadius") },
        // Color grading wheels (FVector4 [R,G,B,master]) + scene tint (FLinearColor)
        { TEXT("color_saturation"),               TEXT("ColorSaturation") },
        { TEXT("color_contrast"),                 TEXT("ColorContrast") },
        { TEXT("color_gamma"),                    TEXT("ColorGamma") },
        { TEXT("color_gain"),                     TEXT("ColorGain") },
        { TEXT("color_offset"),                   TEXT("ColorOffset") },
        { TEXT("scene_color_tint"),               TEXT("SceneColorTint") },
        // White balance
        { TEXT("white_temp"),                     TEXT("WhiteTemp") },
        { TEXT("white_tint"),                     TEXT("WhiteTint") },
        // Depth of field
        { TEXT("depth_of_field_fstop"),           TEXT("DepthOfFieldFstop") },
        { TEXT("depth_of_field_focal_distance"),  TEXT("DepthOfFieldFocalDistance") },
    };

    // The companion bOverride_* FBoolProperty for a value property, via its
    // editcondition meta (canonical). Returns nullptr if there is no companion.
    FBoolProperty* FindOverrideProperty(UStruct* Struct, FProperty* ValueProp)
    {
        const FString EditCond = ValueProp->GetMetaData(TEXT("editcondition"));
        if (!EditCond.IsEmpty())
        {
            if (FProperty* P = Struct->FindPropertyByName(*EditCond))
            {
                return CastField<FBoolProperty>(P);
            }
        }
        // No metadata available (cooked build) — fall back to the documented
        // bOverride_<Name> convention, which FPostProcessSettings follows exactly.
        const FString Companion = FString::Printf(TEXT("bOverride_%s"), *ValueProp->GetName());
        return CastField<FBoolProperty>(Struct->FindPropertyByName(*Companion));
    }

    // FVector4 is an LWC alias (TVector4<double>); a reflected FVector4 property may
    // report either the canonical FVector4 struct or the FVector4d variant struct.
    // Engine code (MaterialExpressions.cpp) checks both — mirror that.
    bool IsVector4Struct(UScriptStruct* S)
    {
        return S == TBaseStructure<FVector4>::Get() || S == TVariantStructure<FVector4d>::Get();
    }

    // Read a JSON number array of N entries into the float slots of an FVector4 /
    // FLinearColor. Returns false if the JSON value is not a sufficiently-long array.
    bool ReadFloatArray(const TSharedPtr<FJsonValue>& Val, int32 N, float* Out)
    {
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (!Val.IsValid() || !Val->TryGetArray(Arr) || !Arr || Arr->Num() < N)
        {
            return false;
        }
        for (int32 i = 0; i < N; ++i)
        {
            Out[i] = static_cast<float>((*Arr)[i]->AsNumber());
        }
        return true;
    }

    // Apply one JSON value to a value property on the FPostProcessSettings instance.
    // Handles float/double, int, bool, FLinearColor (RGBA array) and FVector4
    // (4-array). Returns false (no write) if the JSON type does not fit the property.
    bool ApplyPPValue(FProperty* Prop, void* PPData, const TSharedPtr<FJsonValue>& Val)
    {
        if (FFloatProperty* FP = CastField<FFloatProperty>(Prop))
        {
            double D; if (!Val->TryGetNumber(D)) return false;
            FP->SetPropertyValue_InContainer(PPData, static_cast<float>(D));
            return true;
        }
        if (FDoubleProperty* DP = CastField<FDoubleProperty>(Prop))
        {
            double D; if (!Val->TryGetNumber(D)) return false;
            DP->SetPropertyValue_InContainer(PPData, D);
            return true;
        }
        if (FIntProperty* IP = CastField<FIntProperty>(Prop))
        {
            double D; if (!Val->TryGetNumber(D)) return false;
            IP->SetPropertyValue_InContainer(PPData, static_cast<int32>(D));
            return true;
        }
        if (FBoolProperty* BP = CastField<FBoolProperty>(Prop))
        {
            bool B; if (!Val->TryGetBool(B)) return false;
            BP->SetPropertyValue_InContainer(PPData, B);
            return true;
        }
        if (FStructProperty* SP = CastField<FStructProperty>(Prop))
        {
            void* Dest = SP->ContainerPtrToValuePtr<void>(PPData);
            if (SP->Struct == TBaseStructure<FLinearColor>::Get())
            {
                float C[4] = { 0, 0, 0, 1 };
                if (!ReadFloatArray(Val, 4, C)) { return false; }
                *static_cast<FLinearColor*>(Dest) = FLinearColor(C[0], C[1], C[2], C[3]);
                return true;
            }
            if (IsVector4Struct(SP->Struct))
            {
                float V[4] = { 0, 0, 0, 0 };
                if (!ReadFloatArray(Val, 4, V)) { return false; }
                *static_cast<FVector4*>(Dest) = FVector4(V[0], V[1], V[2], V[3]);
                return true;
            }
        }
        return false;
    }

    // Serialize one value property to JSON for the reader (mirror of ApplyPPValue).
    TSharedPtr<FJsonValue> ReadPPValue(FProperty* Prop, const void* PPData)
    {
        if (FFloatProperty* FP = CastField<FFloatProperty>(Prop))
            return MakeShared<FJsonValueNumber>(FP->GetPropertyValue_InContainer(PPData));
        if (FDoubleProperty* DP = CastField<FDoubleProperty>(Prop))
            return MakeShared<FJsonValueNumber>(DP->GetPropertyValue_InContainer(PPData));
        if (FIntProperty* IP = CastField<FIntProperty>(Prop))
            return MakeShared<FJsonValueNumber>(IP->GetPropertyValue_InContainer(PPData));
        if (FBoolProperty* BP = CastField<FBoolProperty>(Prop))
            return MakeShared<FJsonValueBoolean>(BP->GetPropertyValue_InContainer(PPData));
        if (FStructProperty* SP = CastField<FStructProperty>(Prop))
        {
            const void* Src = SP->ContainerPtrToValuePtr<void>(PPData);
            TArray<TSharedPtr<FJsonValue>> A;
            if (SP->Struct == TBaseStructure<FLinearColor>::Get())
            {
                const FLinearColor& C = *static_cast<const FLinearColor*>(Src);
                A = { MakeShared<FJsonValueNumber>(C.R), MakeShared<FJsonValueNumber>(C.G),
                      MakeShared<FJsonValueNumber>(C.B), MakeShared<FJsonValueNumber>(C.A) };
                return MakeShared<FJsonValueArray>(A);
            }
            if (IsVector4Struct(SP->Struct))
            {
                const FVector4& V = *static_cast<const FVector4*>(Src);
                A = { MakeShared<FJsonValueNumber>(V.X), MakeShared<FJsonValueNumber>(V.Y),
                      MakeShared<FJsonValueNumber>(V.Z), MakeShared<FJsonValueNumber>(V.W) };
                return MakeShared<FJsonValueArray>(A);
            }
        }
        return nullptr;
    }

    // String name for an ECameraFocusMethod value.
    FString FocusMethodToString(ECameraFocusMethod Method)
    {
        switch (Method)
        {
            case ECameraFocusMethod::Manual:        return TEXT("Manual");
            case ECameraFocusMethod::Tracking:      return TEXT("Tracking");
            case ECameraFocusMethod::Disable:       return TEXT("Disable");
            case ECameraFocusMethod::DoNotOverride: return TEXT("DoNotOverride");
            default:                                return TEXT("DoNotOverride");
        }
    }
}

namespace UEAgentCameraImpl
{
    // The cine camera component of the actor the actor_name param names. Null, with OutError set, when the param is
    // missing or empty or that actor has no cine camera component.
    UCineCameraComponent* FindCineCameraOfActorParam(const TSharedPtr<FJsonObject>& Params, FString& OutActorName, FString& OutError)
    {
        if (!Params->TryGetStringField(TEXT("actor_name"), OutActorName))
        {
            OutError = MissingParameter(TEXT("actor_name"));
            return nullptr;
        }
        if (OutActorName.IsEmpty())
        {
            OutError = TEXT("actor_name cannot be empty");
            return nullptr;
        }
        return FindCineCameraComponent(OutActorName, OutError);
    }
}

// MCP:get_cine_camera_settings - Read the full cine-camera configuration off a CineCameraActor's CineCameraComponent: filmback (sensor size/offsets/aspect + preset), lens (focal/f-stop range, squeeze, blades, preset), focus (method/manual distance/offset/smoothing) and the current focal length / aperture / focus distance. Pure reader; mutates nothing.
//   @param actor_name - Name or label of the CineCameraActor to query.
//   @returns { actor_name: string, filmback: { sensor_width, sensor_height, sensor_horizontal_offset, sensor_vertical_offset, sensor_aspect_ratio, preset_name }, lens: { min_focal_length, max_focal_length, min_fstop, max_fstop, minimum_focus_distance, squeeze_factor, diaphragm_blade_count, preset_name }, focus: { method, manual_focus_distance, focus_offset, smooth_focus_changes, focus_smoothing_interp_speed }, current_focal_length, current_aperture, current_focus_distance, horizontal_fov, vertical_fov, note }
TSharedPtr<FJsonObject> HandleGetCineCameraSettings(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    FString Error;
    UCineCameraComponent* CineComp = UEAgentCameraImpl::FindCineCameraOfActorParam(Params, ActorName, Error);
    if (!CineComp)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    const FCameraFilmbackSettings& FB = CineComp->Filmback;
    TSharedPtr<FJsonObject> Filmback = MakeShared<FJsonObject>();
    Filmback->SetNumberField(TEXT("sensor_width"), FB.SensorWidth);
    Filmback->SetNumberField(TEXT("sensor_height"), FB.SensorHeight);
    Filmback->SetNumberField(TEXT("sensor_horizontal_offset"), FB.SensorHorizontalOffset);
    Filmback->SetNumberField(TEXT("sensor_vertical_offset"), FB.SensorVerticalOffset);
    Filmback->SetNumberField(TEXT("sensor_aspect_ratio"), FB.SensorAspectRatio);
    Filmback->SetStringField(TEXT("preset_name"), CineComp->GetFilmbackPresetName());

    const FCameraLensSettings& Lens = CineComp->LensSettings;
    TSharedPtr<FJsonObject> LensJson = MakeShared<FJsonObject>();
    LensJson->SetNumberField(TEXT("min_focal_length"), Lens.MinFocalLength);
    LensJson->SetNumberField(TEXT("max_focal_length"), Lens.MaxFocalLength);
    LensJson->SetNumberField(TEXT("min_fstop"), Lens.MinFStop);
    LensJson->SetNumberField(TEXT("max_fstop"), Lens.MaxFStop);
    LensJson->SetNumberField(TEXT("minimum_focus_distance"), Lens.MinimumFocusDistance);
    LensJson->SetNumberField(TEXT("squeeze_factor"), Lens.SqueezeFactor);
    LensJson->SetNumberField(TEXT("diaphragm_blade_count"), Lens.DiaphragmBladeCount);
    LensJson->SetStringField(TEXT("preset_name"), CineComp->GetLensPresetName());

    // Focus (omit editor-only debug-plane fields)
    const FCameraFocusSettings& Focus = CineComp->FocusSettings;
    TSharedPtr<FJsonObject> FocusJson = MakeShared<FJsonObject>();
    FocusJson->SetStringField(TEXT("method"), FocusMethodToString(Focus.FocusMethod));
    FocusJson->SetNumberField(TEXT("manual_focus_distance"), Focus.ManualFocusDistance);
    FocusJson->SetNumberField(TEXT("focus_offset"), Focus.FocusOffset);
    FocusJson->SetBoolField(TEXT("smooth_focus_changes"), Focus.bSmoothFocusChanges != 0);
    FocusJson->SetNumberField(TEXT("focus_smoothing_interp_speed"), Focus.FocusSmoothingInterpSpeed);

    TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetStringField(TEXT("actor_name"), ActorName);
    Response->SetObjectField(TEXT("filmback"), Filmback);
    Response->SetObjectField(TEXT("lens"), LensJson);
    Response->SetObjectField(TEXT("focus"), FocusJson);
    Response->SetNumberField(TEXT("current_focal_length"), CineComp->CurrentFocalLength);
    Response->SetNumberField(TEXT("current_aperture"), CineComp->CurrentAperture);
    Response->SetNumberField(TEXT("current_focus_distance"), CineComp->CurrentFocusDistance);
    Response->SetNumberField(TEXT("horizontal_fov"), CineComp->GetHorizontalFieldOfView());
    Response->SetNumberField(TEXT("vertical_fov"), CineComp->GetVerticalFieldOfView());
    Response->SetStringField(TEXT("note"),
        TEXT("current_focus_distance is BlueprintReadOnly and reflects the desired focus distance; in the editor (no game tick) it tracks the requested value, not a live ray-traced distance."));
    return mcp_common::MakeOkReply(Response);
}

// MCP:set_camera_post_process - Write PostProcessSettings on a camera actor's camera component (plain or cine) by field presence, turning on each field's bOverride_* companion so it takes effect (like a PostProcess Volume). Supplied fields only; scalars are numbers, color-grading wheels are [R,G,B,master] arrays, scene_color_tint is [R,G,B,A]. Absolute + idempotent. Forces PostProcessBlendWeight>0 and marks the render state dirty.
//   @param actor_name - Name or label of the camera actor to modify.
//   @param bloom_intensity - Optional bloom intensity (number).
//   @param bloom_threshold - Optional bloom threshold (number).
//   @param vignette_intensity - Optional vignette intensity 0..1 (number).
//   @param film_grain_intensity - Optional film grain intensity 0..1 (number).
//   @param chromatic_aberration_intensity - Optional chromatic aberration / scene fringe intensity (number).
//   @param chromatic_aberration_start_offset - Optional chromatic aberration start offset 0..1 (number).
//   @param exposure_bias - Optional auto-exposure bias / EV compensation (number).
//   @param exposure_min_brightness - Optional auto-exposure minimum brightness (number).
//   @param exposure_max_brightness - Optional auto-exposure maximum brightness (number).
//   @param exposure_speed_up - Optional auto-exposure adaptation speed up (number).
//   @param exposure_speed_down - Optional auto-exposure adaptation speed down (number).
//   @param ambient_occlusion_intensity - Optional ambient occlusion intensity 0..1 (number).
//   @param ambient_occlusion_radius - Optional ambient occlusion radius (number).
//   @param color_saturation - Optional color saturation wheel [R,G,B,master] (array).
//   @param color_contrast - Optional color contrast wheel [R,G,B,master] (array).
//   @param color_gamma - Optional color gamma wheel [R,G,B,master] (array).
//   @param color_gain - Optional color gain wheel [R,G,B,master] (array).
//   @param color_offset - Optional color offset wheel [R,G,B,master] (array).
//   @param scene_color_tint - Optional scene color tint [R,G,B,A] (array).
//   @param white_temp - Optional white balance temperature (number).
//   @param white_tint - Optional white balance tint (number).
//   @param depth_of_field_fstop - Optional depth-of-field f-stop (number).
//   @param depth_of_field_focal_distance - Optional depth-of-field focal distance in cm (number).
//   @returns { actor_name: string, applied_count: int, applied: object (field -> value read back), post_process_blend_weight: number, success: bool }
TSharedPtr<FJsonObject> HandleSetCameraPostProcess(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    FString Error;
    UCineCameraComponent* CineComp = UEAgentCameraImpl::FindCineCameraOfActorParam(Params, ActorName, Error);
    if (!CineComp)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    // Reflect over FPostProcessSettings
    UScriptStruct* PPStruct = FPostProcessSettings::StaticStruct();
    FPostProcessSettings& PP = CineComp->PostProcessSettings;
    void* PPData = &PP;

    TSharedPtr<FJsonObject> Applied = MakeShared<FJsonObject>();
    int32 AppliedCount = 0;

    for (const FPPFieldMap& Field : GPPFields)
    {
        const TSharedPtr<FJsonValue> Val = Params->TryGetField(Field.Json);
        if (!Val.IsValid() || Val->IsNull())
        {
            continue;
        }
        FProperty* ValueProp = PPStruct->FindPropertyByName(Field.Prop);
        if (!ValueProp)
        {
            // Source-of-truth drift: a mapped property does not exist. Fail honestly.
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("PostProcess property '%s' (json '%s') not found on FPostProcessSettings"),
                    Field.Prop, Field.Json));
        }
        if (!ApplyPPValue(ValueProp, PPData, Val))
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Field '%s' has the wrong JSON type for property '%s'"),
                    Field.Json, Field.Prop));
        }
        // Turn on the bOverride_* companion so the value is actually applied.
        if (FBoolProperty* OverrideProp = FindOverrideProperty(PPStruct, ValueProp))
        {
            OverrideProp->SetPropertyValue_InContainer(PPData, true);
        }
        Applied->SetField(Field.Json, ReadPPValue(ValueProp, PPData));
        ++AppliedCount;
    }

    if (AppliedCount == 0)
    {
        return mcp_common::MakeErrorReply(
            TEXT("No post-process fields provided. Supply at least one (e.g. bloom_intensity, vignette_intensity, color_saturation, exposure_bias, film_grain_intensity, chromatic_aberration_intensity)."));
    }

    // A camera component needs a non-zero blend weight for its PP to contribute.
    if (CineComp->PostProcessBlendWeight <= 0.0f)
    {
        CineComp->PostProcessBlendWeight = 1.0f;
    }

    CineComp->MarkRenderStateDirty();

    // Response (values read back from the struct = verified)
    TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetStringField(TEXT("actor_name"), ActorName);
    Response->SetNumberField(TEXT("applied_count"), AppliedCount);
    Response->SetObjectField(TEXT("applied"), Applied);
    Response->SetNumberField(TEXT("post_process_blend_weight"), CineComp->PostProcessBlendWeight);
    Response->SetBoolField(TEXT("success"), true);
    return mcp_common::MakeOkReply(Response);
}

// MCP:get_camera_post_process - Read the overridden PostProcessSettings off a camera actor's camera component: only the fields whose bOverride_* companion is on are returned (those actually contributing). Mirrors set_camera_post_process's field set. Pure reader.
//   @param actor_name - Name or label of the camera actor to query.
//   @returns { actor_name: string, post_process_blend_weight: number, overridden_count: int, settings: object (overridden field -> value; scalars as numbers, wheels as [R,G,B,master], scene_color_tint as [R,G,B,A]) }
TSharedPtr<FJsonObject> HandleGetCameraPostProcess(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    FString Error;
    UCineCameraComponent* CineComp = UEAgentCameraImpl::FindCineCameraOfActorParam(Params, ActorName, Error);
    if (!CineComp)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    // Reflect: emit each mapped field whose bOverride_* is on.
    UScriptStruct* PPStruct = FPostProcessSettings::StaticStruct();
    const FPostProcessSettings& PP = CineComp->PostProcessSettings;
    const void* PPData = &PP;

    TSharedPtr<FJsonObject> Settings = MakeShared<FJsonObject>();
    int32 OverriddenCount = 0;

    for (const FPPFieldMap& Field : GPPFields)
    {
        FProperty* ValueProp = PPStruct->FindPropertyByName(Field.Prop);
        if (!ValueProp)
        {
            continue;
        }
        FBoolProperty* OverrideProp = FindOverrideProperty(PPStruct, ValueProp);
        if (!OverrideProp || !OverrideProp->GetPropertyValue_InContainer(PPData))
        {
            continue;
        }
        TSharedPtr<FJsonValue> ValJson = ReadPPValue(ValueProp, PPData);
        if (ValJson.IsValid())
        {
            Settings->SetField(Field.Json, ValJson);
            ++OverriddenCount;
        }
    }

    TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetStringField(TEXT("actor_name"), ActorName);
    Response->SetNumberField(TEXT("post_process_blend_weight"), CineComp->PostProcessBlendWeight);
    Response->SetNumberField(TEXT("overridden_count"), OverriddenCount);
    Response->SetObjectField(TEXT("settings"), Settings);
    return mcp_common::MakeOkReply(Response);
}

// ============================================================================
// Gameplay camera scaffolding (runtime-facing components)
// ============================================================================
//
// The "gameplay camera" primitives that the CineCamera + rig commands don't cover:
//   * add_spring_arm      — USpringArmComponent (camera boom / follow distance).
//   * add_camera_component — UCameraComponent (the actual viewpoint).
//
// Both configure the existing component of their class when there is one,
// instead of adding a second. They follow the same headless-safe pattern as
// the rest of the plugin (no PIE, no tick, no GPU state mutated).
//
// No reflection tricks: every UPROPERTY on USpringArmComponent / UCameraComponent
// is EditAnywhere + BlueprintReadWrite, so we write the field directly.

// Shared helper: locate a USceneComponent on an actor by name (label or object name).
// Returns the FIRST match (callers can be more specific if needed).
// GetComponents<T>(TArray<T*>& OutComponents, bool) — range-for over the template
// overload doesn't compile in this TToolchain (C2672/C2143), so use the array form.
static USceneComponent* FindSceneComponentByName(AActor* Actor, const FString& ComponentName)
{
    if (!Actor)
    {
        return nullptr;
    }
    if (ComponentName.IsEmpty())
    {
        return Actor->GetRootComponent();
    }
    TArray<USceneComponent*> Comps;
    Actor->GetComponents<USceneComponent>(Comps);
    for (USceneComponent* Comp : Comps)
    {
        if (!Comp) { continue; }
        if (Comp->GetName() == ComponentName)
        {
            return Comp;
        }
    }
    return nullptr;
}

// MCP:add_spring_arm - Add or configure a USpringArmComponent on an existing actor (camera boom / third-person follow distance / lag / collision probe).
//   @param actor_name - Name or label of the target actor in the editor world.
//   @param component_name - Optional name for the new component (defaults to "SpringArm"). If a component with this name already exists, only the provided fields are updated.
//   @param target_arm_length - Optional float. Natural length of the spring arm (uu). Default 300.
//   @param socket_offset - Optional [X, Y, Z] relative offset applied at the spring-arm tip.
//   @param target_offset - Optional [X, Y, Z] world-space offset applied at the start of the spring.
//   @param probe_size - Optional float. Collision probe radius.
//   @param probe_channel - Optional string name of ECollisionChannel (e.g. "Camera", "Visibility"). Default "Camera".
//   @param b_do_collision_test - Optional bool. Enable camera collision probe.
//   @param b_use_pawn_control_rotation - Optional bool. Use the parent pawn's view rotation.
//   @param b_inherit_pitch - Optional bool. Inherit pitch from parent.
//   @param b_inherit_yaw - Optional bool. Inherit yaw from parent.
//   @param b_inherit_roll - Optional bool. Inherit roll from parent.
//   @param b_enable_camera_lag - Optional bool. Enable positional lag.
//   @param b_enable_camera_rotation_lag - Optional bool. Enable rotational lag.
//   @param camera_lag_speed - Optional float. Positional lag speed (units/sec).
//   @param camera_rotation_lag_speed - Optional float. Rotational lag speed (deg/sec).
//   @returns { actor_name, component_name, already_existed, applied_count, target_arm_length, socket_offset: [x,y,z], target_offset: [x,y,z], probe_size, probe_channel, b_do_collision_test, b_use_pawn_control_rotation, b_inherit_pitch, b_inherit_yaw, b_inherit_roll, b_enable_camera_lag, b_enable_camera_rotation_lag, camera_lag_speed, camera_rotation_lag_speed }
TSharedPtr<FJsonObject> HandleAddSpringArm(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName)) { return Missing; }

    // The new component is called "SpringArm" unless a non-empty component_name says otherwise.
    FString ComponentName(TEXT("SpringArm"));
    FString RequestedComponentName;
    if (Params->TryGetStringField(TEXT("component_name"), RequestedComponentName) && !RequestedComponentName.IsEmpty())
    {
        ComponentName = RequestedComponentName;
    }

    AActor* Actor = mcp_common::FindActorByName(mcp_common::GetEditorWorld(), ActorName);
    if (!Actor)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Actor '%s' not found in editor world"), *ActorName));
    }

    // Find or create component (idempotent by name)
    USpringArmComponent* Spring = Actor->FindComponentByClass<USpringArmComponent>();
    bool bAlreadyExisted = (Spring != nullptr);
    if (!Spring)
    {
        Spring = NewObject<USpringArmComponent>(Actor, USpringArmComponent::StaticClass(), FName(*ComponentName));
        if (!Spring)
        {
            return mcp_common::MakeErrorReply(
                TEXT("Failed to instantiate USpringArmComponent"));
        }
        Spring->RegisterComponent();
        // Attach to root if the actor has one; otherwise the component sits at origin.
        if (USceneComponent* Root = Actor->GetRootComponent())
        {
            Spring->AttachToComponent(Root, FAttachmentTransformRules::KeepRelativeTransform);
        }
        Actor->AddInstanceComponent(Spring);
    }
    // No rename on a name mismatch: Obj.cpp crashes on a rename into a
    // name slot owned by a pending-GC USpringArmComponent from a prior
    // remove+re-add. NewObject<...>(Actor, Class, FName) already handles uniqueness
    // via MakeUniqueObjectName; the engine-assigned name is returned in
    // `component_name` of the response.

    // Apply only provided fields — preserves "partial update" idempotency.
    int32 AppliedCount = 0;

    double LengthVal;
    if (Params->TryGetNumberField(TEXT("target_arm_length"), LengthVal))
    {
        Spring->TargetArmLength = static_cast<float>(FMath::Max(0.0f, LengthVal));
        AppliedCount++;
    }

    FVector SocketOffset;
    if (Params->HasField(TEXT("socket_offset")))
    {
        SocketOffset = mcp_common::ReadJsonVector(Params, TEXT("socket_offset"));
        Spring->SocketOffset = SocketOffset;
        AppliedCount++;
    }

    FVector TargetOffset;
    if (Params->HasField(TEXT("target_offset")))
    {
        TargetOffset = mcp_common::ReadJsonVector(Params, TEXT("target_offset"));
        Spring->TargetOffset = TargetOffset;
        AppliedCount++;
    }

    double ProbeSizeVal;
    if (Params->TryGetNumberField(TEXT("probe_size"), ProbeSizeVal))
    {
        Spring->ProbeSize = static_cast<float>(FMath::Max(0.0f, ProbeSizeVal));
        AppliedCount++;
    }

    FString ProbeChannelName;
    if (Params->TryGetStringField(TEXT("probe_channel"), ProbeChannelName) && !ProbeChannelName.IsEmpty())
    {
        // Resolve the ECollisionChannel. Accept either the canonical enumerator name
        // ("ECC_Camera", "ECC_Visibility") or the friendly display name ("Camera",
        // "Visibility"). The enum's GetValueByNameString only matches the enumerator
        // name, so we fall back to a linear scan over the display labels.
        UEnum* ChannelEnum = StaticEnum<ECollisionChannel>();
        if (!ChannelEnum)
        {
            return mcp_common::MakeErrorReply(TEXT("StaticEnum<ECollisionChannel>() returned null"));
        }

        FString Canonical = ProbeChannelName;
        if (!Canonical.StartsWith(TEXT("ECC_")))
        {
            Canonical = TEXT("ECC_") + Canonical;
        }
        int64 EnumVal = ChannelEnum->GetValueByNameString(Canonical);

        if (EnumVal == INDEX_NONE)
        {
            // Fallback: scan display names ("Camera", "Visibility", ...).
            for (int32 i = 0; i < ChannelEnum->NumEnums() - 1; ++i)  // skip _MAX
            {
                const FString Display = ChannelEnum->GetDisplayNameTextByIndex(i).ToString();
                if (Display.Equals(ProbeChannelName, ESearchCase::IgnoreCase))
                {
                    EnumVal = ChannelEnum->GetValueByIndex(i);
                    break;
                }
            }
        }

        if (EnumVal == INDEX_NONE)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Unknown collision channel '%s'. Use one of: ECC_Camera, ECC_Visibility, ECC_WorldStatic, ECC_Pawn, etc."),
                    *ProbeChannelName));
        }
        Spring->ProbeChannel = static_cast<ECollisionChannel>(EnumVal);
        AppliedCount++;
    }

    bool BoolVal;
    if (Params->TryGetBoolField(TEXT("b_do_collision_test"), BoolVal))
    {
        Spring->bDoCollisionTest = BoolVal;
        AppliedCount++;
    }
    if (Params->TryGetBoolField(TEXT("b_use_pawn_control_rotation"), BoolVal))
    {
        Spring->bUsePawnControlRotation = BoolVal;
        AppliedCount++;
    }
    if (Params->TryGetBoolField(TEXT("b_inherit_pitch"), BoolVal))
    {
        Spring->bInheritPitch = BoolVal;
        AppliedCount++;
    }
    if (Params->TryGetBoolField(TEXT("b_inherit_yaw"), BoolVal))
    {
        Spring->bInheritYaw = BoolVal;
        AppliedCount++;
    }
    if (Params->TryGetBoolField(TEXT("b_inherit_roll"), BoolVal))
    {
        Spring->bInheritRoll = BoolVal;
        AppliedCount++;
    }
    if (Params->TryGetBoolField(TEXT("b_enable_camera_lag"), BoolVal))
    {
        Spring->bEnableCameraLag = BoolVal;
        AppliedCount++;
    }
    if (Params->TryGetBoolField(TEXT("b_enable_camera_rotation_lag"), BoolVal))
    {
        Spring->bEnableCameraRotationLag = BoolVal;
        AppliedCount++;
    }

    double LagSpeedVal;
    if (Params->TryGetNumberField(TEXT("camera_lag_speed"), LagSpeedVal))
    {
        Spring->CameraLagSpeed = static_cast<float>(FMath::Max(0.0f, LagSpeedVal));
        AppliedCount++;
    }
    if (Params->TryGetNumberField(TEXT("camera_rotation_lag_speed"), LagSpeedVal))
    {
        Spring->CameraRotationLagSpeed = static_cast<float>(FMath::Max(0.0f, LagSpeedVal));
        AppliedCount++;
    }

    Spring->MarkRenderStateDirty();
    Actor->Modify();
    if (UPackage* Pkg = Actor->GetPackage())
    {
        Pkg->MarkPackageDirty();
    }

    // Response — values read back from the component (= verified).
    TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetStringField(TEXT("actor_name"), ActorName);
    Response->SetStringField(TEXT("component_name"), Spring->GetName());
    Response->SetBoolField(TEXT("already_existed"), bAlreadyExisted);
    Response->SetNumberField(TEXT("applied_count"), AppliedCount);
    Response->SetNumberField(TEXT("target_arm_length"), Spring->TargetArmLength);
    Response->SetArrayField(TEXT("socket_offset"), mcp_common::VectorToJsonArray(Spring->SocketOffset));
    {
        Response->SetArrayField(TEXT("target_offset"), mcp_common::VectorToJsonArray(Spring->TargetOffset));
    }
    Response->SetNumberField(TEXT("probe_size"), Spring->ProbeSize);
    Response->SetStringField(TEXT("probe_channel"),
        StaticEnum<ECollisionChannel>()->GetNameStringByValue(static_cast<int64>(Spring->ProbeChannel.GetValue())));
    Response->SetBoolField(TEXT("b_do_collision_test"), Spring->bDoCollisionTest);
    Response->SetBoolField(TEXT("b_use_pawn_control_rotation"), Spring->bUsePawnControlRotation);
    Response->SetBoolField(TEXT("b_inherit_pitch"), Spring->bInheritPitch);
    Response->SetBoolField(TEXT("b_inherit_yaw"), Spring->bInheritYaw);
    Response->SetBoolField(TEXT("b_inherit_roll"), Spring->bInheritRoll);
    Response->SetBoolField(TEXT("b_enable_camera_lag"), Spring->bEnableCameraLag);
    Response->SetBoolField(TEXT("b_enable_camera_rotation_lag"), Spring->bEnableCameraRotationLag);
    Response->SetNumberField(TEXT("camera_lag_speed"), Spring->CameraLagSpeed);
    Response->SetNumberField(TEXT("camera_rotation_lag_speed"), Spring->CameraRotationLagSpeed);
    return mcp_common::MakeOkReply(Response);
}

// MCP:add_camera_component - Add or configure a UCameraComponent on an existing actor, optionally attaching it to a SpringArm (or any other USceneComponent) on the same actor.
//   @param actor_name - Name or label of the target actor.
//   @param component_name - Optional name for the new component (defaults to "Camera"). If a UCameraComponent with this name already exists on the actor, only the provided fields are updated.
//   @param parent_component - Optional name of a USceneComponent on the same actor to attach to (e.g. "SpringArm"). Default = actor root component.
//   @param field_of_view - Optional float. Horizontal FOV in degrees (1..170).
//   @param aspect_ratio - Optional float. Aspect ratio (e.g. 16/9). 0 = auto.
//   @param ortho_width - Optional float. Orthographic width (only used when projection_mode = "Orthographic").
//   @param projection_mode - Optional string. "Perspective" or "Orthographic".
//   @param location - Optional [X, Y, Z] relative location vs parent.
//   @param rotation - Optional [Pitch, Yaw, Roll] relative rotation vs parent.
//   @param b_use_controller_view_rotation - Optional bool. Match pawn control rotation.
//   @param b_use_pawn_control_rotation - Optional bool. Alias for b_use_controller_view_rotation — sets UCameraComponent::bUsePawnControlRotation.
//   @param b_lock_to_hmd - Optional bool. Lock orientation to the HMD.
//   @param post_process_settings - Optional object (PostProcess fields, see get_camera_post_process for the schema).
//   @param post_process_blend_weight - Optional float. Weight of post-process (0..1). 0 means post-process disabled.
//   @returns { actor_name, component_name, parent_component, already_existed, applied_count, field_of_view, aspect_ratio, projection_mode, location: [x,y,z], rotation: [pitch,yaw,roll] }
TSharedPtr<FJsonObject> HandleAddCameraComponent(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName)) { return Missing; }

    FString ComponentName;
    Params->TryGetStringField(TEXT("component_name"), ComponentName);
    if (ComponentName.IsEmpty())
    {
        ComponentName = TEXT("Camera");
    }

    FString ParentCompName;
    Params->TryGetStringField(TEXT("parent_component"), ParentCompName);

    AActor* Actor = mcp_common::FindActorByName(mcp_common::GetEditorWorld(), ActorName);
    if (!Actor)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Actor '%s' not found in editor world"), *ActorName));
    }

    // Resolve parent component (default = actor root)
    USceneComponent* Parent = nullptr;
    if (!ParentCompName.IsEmpty())
    {
        Parent = FindSceneComponentByName(Actor, ParentCompName);
        if (!Parent)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Parent component '%s' not found on actor '%s'"), *ParentCompName, *ActorName));
        }
    }
    else
    {
        Parent = Actor->GetRootComponent();
    }

    UCameraComponent* CameraComp = Actor->FindComponentByClass<UCameraComponent>();
    bool bAlreadyExisted = (CameraComp != nullptr);
    if (!CameraComp)
    {
        CameraComp = NewObject<UCameraComponent>(Actor, UCameraComponent::StaticClass(), FName(*ComponentName));
        if (!CameraComp)
        {
            return mcp_common::MakeErrorReply(
                TEXT("Failed to instantiate UCameraComponent"));
        }
        CameraComp->RegisterComponent();
        Actor->AddInstanceComponent(CameraComp);
    }
    // No rename on a name mismatch (same reason as the spring arm block above):
    // Obj.cpp crashes on a rename into a name slot owned by a pending-GC
    // UCameraComponent from a prior remove+re-add.

    // Attach to parent (idempotent — only re-attach if not already there).
    if (Parent && CameraComp->GetAttachParent() != Parent)
    {
        CameraComp->AttachToComponent(Parent, FAttachmentTransformRules::KeepRelativeTransform);
    }

    int32 AppliedCount = 0;

    double FovVal;
    if (Params->TryGetNumberField(TEXT("field_of_view"), FovVal))
    {
        CameraComp->SetFieldOfView(static_cast<float>(FMath::Clamp(FovVal, 1.0, 170.0)));
        AppliedCount++;
    }

    double AspectVal;
    if (Params->TryGetNumberField(TEXT("aspect_ratio"), AspectVal))
    {
        CameraComp->SetAspectRatio(static_cast<float>(FMath::Max(0.0f, AspectVal)));
        AppliedCount++;
    }

    double OrthoWidthVal;
    if (Params->TryGetNumberField(TEXT("ortho_width"), OrthoWidthVal))
    {
        CameraComp->SetOrthoWidth(static_cast<float>(FMath::Max(0.0f, OrthoWidthVal)));
        AppliedCount++;
    }

    FString ProjectionModeStr;
    if (Params->TryGetStringField(TEXT("projection_mode"), ProjectionModeStr) && !ProjectionModeStr.IsEmpty())
    {
        // ECameraProjectionMode::Perspective=0, Orthographic=1.
        if (ProjectionModeStr.Equals(TEXT("Orthographic"), ESearchCase::IgnoreCase))
        {
            CameraComp->SetProjectionMode(ECameraProjectionMode::Orthographic);
            AppliedCount++;
        }
        else if (ProjectionModeStr.Equals(TEXT("Perspective"), ESearchCase::IgnoreCase))
        {
            CameraComp->SetProjectionMode(ECameraProjectionMode::Perspective);
            AppliedCount++;
        }
        else
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Invalid projection_mode '%s'. Expected 'Perspective' or 'Orthographic'"), *ProjectionModeStr));
        }
    }

    FVector Loc;
    if (Params->HasField(TEXT("location")))
    {
        Loc = mcp_common::ReadJsonVector(Params, TEXT("location"));
        CameraComp->SetRelativeLocation(Loc);
        AppliedCount++;
    }

    FRotator Rot;
    if (Params->HasField(TEXT("rotation")))
    {
        Rot = mcp_common::ReadJsonRotator(Params, TEXT("rotation"));
        CameraComp->SetRelativeRotation(Rot);
        AppliedCount++;
    }

    bool BoolVal;
    // `bUseControllerViewRotation` is `_DEPRECATED`; the canonical field is
    // `bUsePawnControlRotation` (same semantics — match the parent pawn's view rotation).
    if (Params->TryGetBoolField(TEXT("b_use_controller_view_rotation"), BoolVal) ||
        Params->TryGetBoolField(TEXT("b_use_pawn_control_rotation"), BoolVal))
    {
        CameraComp->bUsePawnControlRotation = BoolVal;
        AppliedCount++;
    }
    if (Params->TryGetBoolField(TEXT("b_lock_to_hmd"), BoolVal))
    {
        CameraComp->bLockToHmd = BoolVal;
        AppliedCount++;
    }

    double PPWeightVal;
    if (Params->TryGetNumberField(TEXT("post_process_blend_weight"), PPWeightVal))
    {
        const float W = static_cast<float>(FMath::Clamp(PPWeightVal, 0.0, 1.0));
        CameraComp->PostProcessBlendWeight = W;
        AppliedCount++;
    }

    // Optional: post-process settings blob (forwarded to PostProcessSettings via the
    // existing FPostProcessSettings field — limited support: the values are JSON-encoded
    // and applied via FJsonObjectConverter if the schema matches FPostProcessSettings fields).
    const TSharedPtr<FJsonObject>* PPJson = nullptr;
    if (Params->TryGetObjectField(TEXT("post_process_settings"), PPJson) && PPJson && PPJson->IsValid())
    {
        FString PPError;
        if (!mcp_common::JsonToStruct(*PPJson,
                FPostProcessSettings::StaticStruct(),
                &CameraComp->PostProcessSettings, PPError))
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("post_process_settings rejected: %s"), *PPError));
        }
        AppliedCount++;
    }

    CameraComp->MarkRenderStateDirty();
    // (We don't unconditionally set bOverride_* companions — the user only opted
    // in to whatever post_process_settings keys they passed in, and the JsonToStruct
    // merge already turned on the relevant ones via FPostProcessSettings's metadata.)
    Actor->Modify();
    if (UPackage* Pkg = Actor->GetPackage())
    {
        Pkg->MarkPackageDirty();
    }

    // Response — values read back.
    TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetStringField(TEXT("actor_name"), ActorName);
    Response->SetStringField(TEXT("component_name"), CameraComp->GetName());
    Response->SetStringField(TEXT("parent_component"),
        CameraComp->GetAttachParent() ? CameraComp->GetAttachParent()->GetName() : TEXT(""));
    Response->SetBoolField(TEXT("already_existed"), bAlreadyExisted);
    Response->SetNumberField(TEXT("applied_count"), AppliedCount);
    Response->SetNumberField(TEXT("field_of_view"), CameraComp->FieldOfView);
    Response->SetNumberField(TEXT("aspect_ratio"), CameraComp->AspectRatio);
    Response->SetStringField(TEXT("projection_mode"),
        CameraComp->ProjectionMode == ECameraProjectionMode::Orthographic
            ? TEXT("Orthographic") : TEXT("Perspective"));
    {
        const FVector RL = CameraComp->GetRelativeLocation();
        TArray<TSharedPtr<FJsonValue>> Arr = mcp_common::VectorToJsonArray(RL);
        Response->SetArrayField(TEXT("location"), Arr);
    }
    {
        const FRotator RR = CameraComp->GetRelativeRotation();
        Response->SetArrayField(TEXT("rotation"), mcp_common::RotatorToJsonArray(RR));
    }
    Response->SetNumberField(TEXT("post_process_blend_weight"), CameraComp->PostProcessBlendWeight);
    return mcp_common::MakeOkReply(Response);
}

// create_camera_shake_blueprint
// ---------------------------------------------------------------------------- //
// MCP:create_camera_shake_blueprint - Create a Blueprint asset parented to UCameraShakeBase, compile and save it.  UCameraShakeBase is abstract but Blueprintable; the generated Blueprint subclass is concrete. Idempotent: if the asset already exists it is loaded and returned.
//   @param name - Asset name (e.g. BP_ExplosionShake)
//   @param package_path - Target /Game package path (optional, default "/Game/Blueprints")
//   @returns { name: string, asset_path: string, parent_class: string, generated_class: string, is_camera_shake: bool, already_existed: bool, created: bool }

TSharedPtr<FJsonObject> HandleCreateCameraShakeBlueprint(const TSharedPtr<FJsonObject>& Params)
{
    FString Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString PackagePath = TEXT("/Game/Blueprints");
    Params->TryGetStringField(TEXT("package_path"), PackagePath);
    PackagePath = mcp_common::NormalizeFolder(PackagePath, TEXT("/Game/Blueprints"));

    bool bAlreadyExisted = false;
    FString Error;
    // UCameraShakeBase is Abstract but Blueprintable; the generated BP child class is
    // concrete, which is exactly how the editor authors camera shake blueprints.
    UBlueprint* Blueprint = mcp_common::CreateOrLoadBlueprint(UCameraShakeBase::StaticClass(), PackagePath, Name, bAlreadyExisted, Error);
    if (!Blueprint)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    const bool bIsCameraShake = Blueprint->GeneratedClass && Blueprint->GeneratedClass->IsChildOf(UCameraShakeBase::StaticClass());

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("name"), Name);
    Result->SetStringField(TEXT("asset_path"), FString::Printf(TEXT("%s/%s"), *PackagePath, *Name));
    Result->SetStringField(TEXT("parent_class"), TEXT("CameraShakeBase"));
    Result->SetStringField(TEXT("generated_class"), Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetName() : FString());
    Result->SetBoolField(TEXT("is_camera_shake"), bIsCameraShake);
    Result->SetBoolField(TEXT("already_existed"), bAlreadyExisted);
    Result->SetBoolField(TEXT("created"), true);
    return mcp_common::MakeOkReply(Result);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_camera_component"), &HandleAddCameraComponent },
    { TEXT("add_camera_node_child"), &HandleAddCameraNodeChild },
    { TEXT("add_gameplay_camera_component"), &HandleAddGameplayCameraComponent },
    { TEXT("add_spring_arm"), &HandleAddSpringArm },
    { TEXT("build_camera_rig"), &HandleBuildCameraRig },
    { TEXT("create_camera"), &HandleCreateCamera },
    { TEXT("create_camera_asset"), &HandleCreateCameraAsset },
    { TEXT("create_camera_rig_asset"), &HandleCreateCameraRigAsset },
    { TEXT("create_camera_rig_crane"), &HandleCreateCameraRigCrane },
    { TEXT("create_camera_rig_rail"), &HandleCreateCameraRigRail },
    { TEXT("create_camera_shake_blueprint"), &HandleCreateCameraShakeBlueprint },
    { TEXT("get_camera_asset_info"), &HandleGetCameraAssetInfo },
    { TEXT("get_camera_fov"), &HandleGetCameraFOV },
    { TEXT("get_camera_post_process"), &HandleGetCameraPostProcess },
    { TEXT("get_camera_rig_nodes"), &HandleGetCameraRigNodes },
    { TEXT("get_camera_transform"), &HandleGetCameraTransform },
    { TEXT("get_cameras"), &HandleGetCameras },
    { TEXT("get_cine_camera_settings"), &HandleGetCineCameraSettings },
    { TEXT("list_camera_node_types"), &HandleListCameraNodeTypes },
    { TEXT("set_camera_asset_director"), &HandleSetCameraAssetDirector },
    { TEXT("set_camera_fov"), &HandleSetCameraFOV },
    { TEXT("set_camera_node_parameters"), &HandleSetCameraNodeParameters },
    { TEXT("set_camera_post_process"), &HandleSetCameraPostProcess },
    { TEXT("set_camera_rig_rail_position"), &HandleSetCameraRigRailPosition },
    { TEXT("set_camera_rig_root_node"), &HandleSetCameraRigRootNode },
    { TEXT("set_camera_transform"), &HandleSetCameraTransform },
    { TEXT("set_cine_camera_aperture"), &HandleSetCineCameraAperture },
    { TEXT("set_cine_camera_filmback"), &HandleSetCineCameraFilmback },
    { TEXT("set_cine_camera_focal_length"), &HandleSetCineCameraFocalLength },
    { TEXT("set_cine_camera_focus"), &HandleSetCineCameraFocus },
    { TEXT("set_cine_camera_lens"), &HandleSetCineCameraLens },
};
} // namespace UEAgentCameraCommands
