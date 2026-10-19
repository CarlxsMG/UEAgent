#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Physics/PhysicsConstraintUtils.h"

// Asset/package plumbing
#include "UObject/UObjectGlobals.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"

// Physics asset + collision
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "PhysicsEngine/PhysicsAsset.h"          // UPhysicsAsset (Runtime/Engine)
#include "PhysicsEngine/SkeletalBodySetup.h"     // USkeletalBodySetup
#include "PhysicsEngine/BodySetup.h"             // UBodySetup, FKAggregateGeom, DefaultInstance
#include "PhysicsEngine/BoxElem.h"               // FKBoxElem
#include "PhysicsEngine/SphereElem.h"            // FKSphereElem
#include "PhysicsEngine/SphylElem.h"             // FKSphylElem
#include "BodySetupEnums.h"                       // EPhysicsType (PhysicsCore)
#include "PhysicsEngine/AggregateGeom.h"          // FKAggregateGeom::EmptyElements / GetElementCount
#include "StaticMeshEditorSubsystem.h"            // UStaticMeshEditorSubsystem (STATICMESHEDITOR_API; convex + K-DOP)
#include "StaticMeshEditorSubsystemHelpers.h"     // EScriptCollisionShapeType (K-DOP shapes)
#include "Editor.h"                                // GEditor->GetEditorSubsystem<>()
#include "PhysicsAssetUtils.h"                    // FPhysicsAssetUtils (PhysicsUtilities)
#include "PhysicsEngine/PhysicsConstraintTemplate.h" // UPhysicsConstraintTemplate (DefaultInstance, SetDefaultProfile)
#include "PhysicsEngine/ConstraintInstance.h"     // FConstraintInstance (Set*Limit, SnapTransformsToDefault, EConstraintTransformComponentFlags)
#include "Chaos/ChaosEngineInterface.h"           // EAngularConstraintMotion / ELinearConstraintMotion (ACM_*/LCM_*)

/**
 * Physics Asset and static-mesh collision commands.
 *
 * Covers authoring of UPhysicsAsset assets for skeletal meshes (bodies and the ragdoll constraints between
 * them) and of the collision of UStaticMesh BodySetups (simple analytic primitives, convex decomposition,
 * K-DOP). Physical-material assets and world constraint actors belong to UEAgentPhysicsAuthoringCommands;
 * physics queries, impulses and ISM instances to UEAgentPhysicsCommands.
 *
 * Engine APIs: UPhysicsAsset, FPhysicsAssetUtils / FPhysAssetCreateParams (Developer/PhysicsUtilities),
 * UBodySetup (AggGeom, DefaultInstance; BoneName / PhysicsType / EPhysicsType from BodySetupCore.h) and
 * UStaticMesh::GetBodySetup / CreateBodySetup.
 */
namespace UEAgentPhysicsAssetCommands
{

// MCP-DOMAIN: Physics

namespace
{
    // Parse a Free|Limited|Locked string into an angular motion; returns false (and leaves
    // Out untouched) for an absent/unknown value, so callers can keep the current motion.
    bool ParseAngularMotion(const FString& In, EAngularConstraintMotion& Out)
    {
        if (In.Equals(TEXT("Free"), ESearchCase::IgnoreCase))    { Out = ACM_Free;    return true; }
        if (In.Equals(TEXT("Limited"), ESearchCase::IgnoreCase)) { Out = ACM_Limited; return true; }
        if (In.Equals(TEXT("Locked"), ESearchCase::IgnoreCase))  { Out = ACM_Locked;  return true; }
        return false;
    }

    bool ParseLinearMotion(const FString& In, ELinearConstraintMotion& Out)
    {
        if (In.Equals(TEXT("Free"), ESearchCase::IgnoreCase))    { Out = LCM_Free;    return true; }
        if (In.Equals(TEXT("Limited"), ESearchCase::IgnoreCase)) { Out = LCM_Limited; return true; }
        if (In.Equals(TEXT("Locked"), ESearchCase::IgnoreCase))  { Out = LCM_Locked;  return true; }
        return false;
    }

    // Describe one UPhysicsConstraintTemplate's authored state for round-trip reads.
    TSharedPtr<FJsonObject> MakeConstraintObject(int32 Index, const UPhysicsConstraintTemplate* CS)
    {
        TSharedPtr<FJsonObject> C = MakeShared<FJsonObject>();
        C->SetNumberField(TEXT("index"), Index);
        if (!CS) { return C; }
        const FConstraintInstance& CI = CS->DefaultInstance;
        C->SetStringField(TEXT("name"), CI.JointName.ToString());
        C->SetStringField(TEXT("child_bone"), CI.ConstraintBone1.ToString());
        C->SetStringField(TEXT("parent_bone"), CI.ConstraintBone2.ToString());
        C->SetStringField(TEXT("swing1_motion"), PhysicsConstraintUtils::AngularMotionName(CI.GetAngularSwing1Motion()));
        C->SetNumberField(TEXT("swing1_limit"), CI.GetAngularSwing1Limit());
        C->SetStringField(TEXT("swing2_motion"), PhysicsConstraintUtils::AngularMotionName(CI.GetAngularSwing2Motion()));
        C->SetNumberField(TEXT("swing2_limit"), CI.GetAngularSwing2Limit());
        C->SetStringField(TEXT("twist_motion"), PhysicsConstraintUtils::AngularMotionName(CI.GetAngularTwistMotion()));
        C->SetNumberField(TEXT("twist_limit"), CI.GetAngularTwistLimit());
        C->SetStringField(TEXT("linear_x_motion"), PhysicsConstraintUtils::LinearMotionName(CI.GetLinearXMotion()));
        C->SetStringField(TEXT("linear_y_motion"), PhysicsConstraintUtils::LinearMotionName(CI.GetLinearYMotion()));
        C->SetStringField(TEXT("linear_z_motion"), PhysicsConstraintUtils::LinearMotionName(CI.GetLinearZMotion()));
        C->SetNumberField(TEXT("linear_limit"), CI.GetLinearLimit());
        // Drives + breakable + projection summary (const-safe getters / public fields).
        C->SetStringField(TEXT("angular_drive_mode"), PhysicsConstraintUtils::AngularDriveModeName(CI.ProfileInstance.AngularDrive.AngularDriveMode));
        C->SetBoolField(TEXT("angular_orientation_drive"), CI.IsAngularOrientationDriveEnabled());
        C->SetBoolField(TEXT("angular_velocity_drive"), CI.IsAngularVelocityDriveEnabled());
        C->SetBoolField(TEXT("linear_position_drive"), CI.IsLinearPositionDriveEnabled());
        C->SetBoolField(TEXT("linear_velocity_drive"), CI.IsLinearVelocityDriveEnabled());
        C->SetBoolField(TEXT("linear_breakable"), CI.IsLinearBreakable());
        C->SetNumberField(TEXT("linear_break_threshold"), CI.GetLinearBreakThreshold());
        C->SetBoolField(TEXT("angular_breakable"), CI.IsAngularBreakable());
        C->SetNumberField(TEXT("angular_break_threshold"), CI.GetAngularBreakThreshold());
        C->SetBoolField(TEXT("projection_enabled"), CI.IsProjectionEnabled());
        return C;
    }

    // What ResolveWritableStaticMesh did on disk in this call: whether it created the /Game copy at dest_path,
    // and whether that copy's package was saved (SaveError says why not).
    struct FWritableMeshCopy
    {
        bool bCreated = false;
        bool bSaved = false;
        FString SaveError;
    };

    // Resolve a writable UStaticMesh from `static_mesh_path`, duplicating a read-only
    // /Engine source into the supplied `dest_path` (must be /Game) first, so the collision
    // commands never mutate a read-only engine asset. On failure, returns nullptr and
    // fills OutError with the response to return; on success returns the writable mesh and
    // sets bOutDuplicated (the mesh is the /Game copy, new or reused). OutCopy, when given,
    // says whether this call created and saved that copy: a caller that saves nothing itself
    // reports that save as its own.
    UStaticMesh* ResolveWritableStaticMesh(const TSharedPtr<FJsonObject>& Params,
                                           bool& bOutDuplicated,
                                           TSharedPtr<FJsonObject>& OutError,
                                           FWritableMeshCopy* OutCopy = nullptr)
    {
        bOutDuplicated = false;
        OutError.Reset();
        if (OutCopy)
        {
            *OutCopy = FWritableMeshCopy();
        }

        FString MeshPath;
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("static_mesh_path"), MeshPath))
        {
            OutError = Missing;
            return nullptr;
        }

        UStaticMesh* StaticMesh = Cast<UStaticMesh>(
            StaticLoadObject(UStaticMesh::StaticClass(), nullptr, *MeshPath));
        if (!StaticMesh)
        {
            OutError = mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Static mesh not found: %s"), *MeshPath));
            return nullptr;
        }

        if (StaticMesh->GetPathName().StartsWith(TEXT("/Game")))
        {
            return StaticMesh; // already writable
        }

        // Engine content under /Engine is read-only: require a /Game dest_path to copy into.
        // Read into a bool, not negated in the if: dest_path is needed only on this path (a /Game
        // mesh returned above), and a negated read would make the catalog require it on every call.
        FString DestPath;
        const bool bHasDestPath = Params->TryGetStringField(TEXT("dest_path"), DestPath);
        if (!bHasDestPath || DestPath.IsEmpty())
        {
            OutError = mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Static mesh is not writable (must be under /Game): %s. "
                    "Supply 'dest_path' (a /Game path) to duplicate it first."),
                    *StaticMesh->GetPathName()));
            return nullptr;
        }
        if (!DestPath.StartsWith(TEXT("/Game")))
        {
            OutError = mcp_common::MakeErrorReply(
                FString::Printf(TEXT("dest_path must be under /Game: %s"), *DestPath));
            return nullptr;
        }

        const FString DestName = FPackageName::GetShortName(DestPath);

        // Idempotent: reuse an already-duplicated copy if it exists.
        UStaticMesh* DestMesh = nullptr;
        if (UEditorAssetLibrary::DoesAssetExist(DestPath))
        {
            DestMesh = Cast<UStaticMesh>(
                StaticLoadObject(UStaticMesh::StaticClass(), nullptr, *DestPath));
        }
        if (!DestMesh)
        {
            FString PackageError;
            UPackage* DestPackage = mcp_common::CreateAssetPackage(DestPath, DestName, UStaticMesh::StaticClass(), PackageError);
            if (!DestPackage)
            {
                OutError = mcp_common::MakeErrorReply(PackageError);
                return nullptr;
            }
            DestPackage->FullyLoad();
            DestMesh = DuplicateObject<UStaticMesh>(StaticMesh, DestPackage, *DestName);
            if (!DestMesh)
            {
                OutError = mcp_common::MakeErrorReply(
                    TEXT("Failed to duplicate static mesh into /Game"));
                return nullptr;
            }
            DestMesh->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
            FAssetRegistryModule::AssetCreated(DestMesh);
            FString DupSaveError;
            const bool bDupSaved = mcp_common::SaveAssetPackage(DestPackage, DestMesh, DupSaveError);
            if (OutCopy)
            {
                OutCopy->bCreated = true;
                OutCopy->bSaved = bDupSaved;
                OutCopy->SaveError = DupSaveError;
            }
        }
        bOutDuplicated = true;
        return DestMesh;
    }

    // Map a K-DOP shape string to the subsystem enum. Returns false on an unknown value.
    bool ParseKDopShape(const FString& In, EScriptCollisionShapeType& Out)
    {
        if (In.Equals(TEXT("NDOP10_X"), ESearchCase::IgnoreCase)) { Out = EScriptCollisionShapeType::NDOP10_X; return true; }
        if (In.Equals(TEXT("NDOP10_Y"), ESearchCase::IgnoreCase)) { Out = EScriptCollisionShapeType::NDOP10_Y; return true; }
        if (In.Equals(TEXT("NDOP10_Z"), ESearchCase::IgnoreCase)) { Out = EScriptCollisionShapeType::NDOP10_Z; return true; }
        if (In.Equals(TEXT("NDOP18"),   ESearchCase::IgnoreCase)) { Out = EScriptCollisionShapeType::NDOP18;   return true; }
        if (In.Equals(TEXT("NDOP26"),   ESearchCase::IgnoreCase)) { Out = EScriptCollisionShapeType::NDOP26;   return true; }
        return false;
    }

    // Count constraints/bodies and fill a payload describing a physics asset.
    void FillPhysicsAssetPayload(UPhysicsAsset* PhysAsset, const TSharedPtr<FJsonObject>& Obj)
    {
        Obj->SetStringField(TEXT("asset_path"), PhysAsset->GetPathName());
        Obj->SetStringField(TEXT("asset_name"), PhysAsset->GetName());
        Obj->SetNumberField(TEXT("body_count"), PhysAsset->SkeletalBodySetups.Num());
        Obj->SetNumberField(TEXT("constraint_count"), PhysAsset->ConstraintSetup.Num());

        const USkeletalMesh* Preview = PhysAsset->GetPreviewMesh();
        Obj->SetStringField(TEXT("skeletal_mesh_path"),
            Preview ? Preview->GetPathName() : FString());

        // Per-body bone names so callers can target a body by name.
        TArray<TSharedPtr<FJsonValue>> BodyArr;
        for (int32 i = 0; i < PhysAsset->SkeletalBodySetups.Num(); ++i)
        {
            USkeletalBodySetup* BS = PhysAsset->SkeletalBodySetups[i];
            TSharedPtr<FJsonObject> BodyObj = MakeShared<FJsonObject>();
            BodyObj->SetNumberField(TEXT("index"), i);
            BodyObj->SetStringField(TEXT("bone_name"), BS ? BS->BoneName.ToString() : FString());
            if (BS)
            {
                BodyObj->SetNumberField(TEXT("primitive_count"), BS->AggGeom.GetElementCount());
            }
            BodyArr.Add(MakeShared<FJsonValueObject>(BodyObj));
        }
        Obj->SetArrayField(TEXT("bodies"), BodyArr);

        // Preview-mesh skeleton bone names — the addressable space for constraint bones.
        // (Reuse the `Preview` already resolved above; redeclaring it trips C4456 under /WX.)
        TArray<TSharedPtr<FJsonValue>> BoneArr;
        if (Preview)
        {
            const FReferenceSkeleton& RefSkel = Preview->GetRefSkeleton();
            for (int32 i = 0; i < RefSkel.GetNum(); ++i)
            {
                BoneArr.Add(MakeShared<FJsonValueString>(RefSkel.GetBoneName(i).ToString()));
            }
        }
        Obj->SetArrayField(TEXT("skeleton_bones"), BoneArr);

        // Authored constraints (ragdoll joints) with their bones + limits for round-trip.
        TArray<TSharedPtr<FJsonValue>> ConArr;
        for (int32 i = 0; i < PhysAsset->ConstraintSetup.Num(); ++i)
        {
            ConArr.Add(MakeShared<FJsonValueObject>(MakeConstraintObject(i, PhysAsset->ConstraintSetup[i])));
        }
        Obj->SetArrayField(TEXT("constraints"), ConArr);
    }

    // Loads the physics asset at AssetPath; null on success, else the error reply.
    TSharedPtr<FJsonObject> PhysicsAsset_LoadAsset(const FString& AssetPath, UPhysicsAsset*& OutPhysAsset)
    {
        OutPhysAsset = Cast<UPhysicsAsset>(StaticLoadObject(UPhysicsAsset::StaticClass(), nullptr, *AssetPath));
        if (!OutPhysAsset)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Physics asset not found: %s"), *AssetPath));
        }
        return nullptr;
    }

    // Reads the required "physics_asset_path" and loads that physics asset; null on success, else the error reply.
    TSharedPtr<FJsonObject> PhysicsAsset_ReadAsset(const TSharedPtr<FJsonObject>& Params, UPhysicsAsset*& OutPhysAsset)
    {
        FString AssetPath;
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("physics_asset_path"), AssetPath)) { return Missing; }
        return PhysicsAsset_LoadAsset(AssetPath, OutPhysAsset);
    }

    // Reads the physics asset and the body to configure in it, by "body_index" or else by "bone_name";
    // null on success, else the error reply.
    TSharedPtr<FJsonObject> PhysicsAsset_ReadBody(const TSharedPtr<FJsonObject>& Params, UPhysicsAsset*& OutPhysAsset,
        int32& OutBodyIndex, USkeletalBodySetup*& OutBodySetup)
    {
        if (TSharedPtr<FJsonObject> Error = PhysicsAsset_ReadAsset(Params, OutPhysAsset))
        {
            return Error;
        }

        if (OutPhysAsset->SkeletalBodySetups.Num() == 0)
        {
            return mcp_common::MakeErrorReply(
                TEXT("Physics asset has no bodies to configure"));
        }

        OutBodyIndex = INDEX_NONE;
        int32 ParsedIndex = INDEX_NONE;
        if (Params->TryGetNumberField(TEXT("body_index"), ParsedIndex))
        {
            OutBodyIndex = ParsedIndex;
        }
        else
        {
            FString BoneName;
            if (Params->TryGetStringField(TEXT("bone_name"), BoneName) && !BoneName.IsEmpty())
            {
                OutBodyIndex = OutPhysAsset->FindBodyIndex(FName(*BoneName));
            }
        }

        if (!OutPhysAsset->SkeletalBodySetups.IsValidIndex(OutBodyIndex))
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Invalid body target (resolved index %d); provide body_index or bone_name"), OutBodyIndex));
        }

        OutBodySetup = OutPhysAsset->SkeletalBodySetups[OutBodyIndex];
        if (!OutBodySetup)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Body setup at index %d is null"), OutBodyIndex));
        }
        return nullptr;
    }

    // Reads the physics asset and its constraint named by the required "constraint_name"; null on
    // success, else the error reply.
    TSharedPtr<FJsonObject> PhysicsAsset_ReadConstraint(const TSharedPtr<FJsonObject>& Params, UPhysicsAsset*& OutPhysAsset,
        FString& OutConstraintName, int32& OutIndex, UPhysicsConstraintTemplate*& OutConstraint)
    {
        FString AssetPath;
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("physics_asset_path"), AssetPath)) { return Missing; }
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("constraint_name"), OutConstraintName)) { return Missing; }
        if (TSharedPtr<FJsonObject> Error = PhysicsAsset_LoadAsset(AssetPath, OutPhysAsset))
        {
            return Error;
        }

        OutIndex = OutPhysAsset->FindConstraintIndex(FName(*OutConstraintName));
        if (!OutPhysAsset->ConstraintSetup.IsValidIndex(OutIndex))
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Constraint not found: %s"), *OutConstraintName));
        }
        OutConstraint = OutPhysAsset->ConstraintSetup[OutIndex];
        return nullptr;
    }

    // Reads the optional drive, breakable and projection params of set_physics_asset_constraint_drives; null on success,
    // else the error reply for an angular_drive_mode that is neither SLERP nor TwistAndSwing.
    TSharedPtr<FJsonObject> PhysicsAsset_ReadDriveSettings(const TSharedPtr<FJsonObject>& Params, PhysicsConstraintUtils::FDriveSettings& Out)
    {
        FString ModeStr;
        if (Params->TryGetStringField(TEXT("angular_drive_mode"), ModeStr) && !ModeStr.IsEmpty())
        {
            Out.AngularDriveMode.bGiven = PhysicsConstraintUtils::ParseAngularDriveMode(ModeStr, Out.AngularDriveMode.Value);
            if (!Out.AngularDriveMode.bGiven)
            {
                return mcp_common::MakeErrorReply(
                    FString::Printf(TEXT("Invalid angular_drive_mode '%s' (SLERP|TwistAndSwing)"), *ModeStr));
            }
        }
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        Out.AngularOrientationDrive.bGiven = Params->TryGetBoolField(TEXT("angular_orientation_drive"), Out.AngularOrientationDrive.Value);
        Out.AngularVelocityDrive.bGiven = Params->TryGetBoolField(TEXT("angular_velocity_drive"), Out.AngularVelocityDrive.Value);
        Out.AngularDriveSpring.bGiven = Params->TryGetNumberField(TEXT("angular_drive_spring"), Out.AngularDriveSpring.Value);
        Out.AngularDriveDamping.bGiven = Params->TryGetNumberField(TEXT("angular_drive_damping"), Out.AngularDriveDamping.Value);
        Out.AngularDriveForceLimit.bGiven = Params->TryGetNumberField(TEXT("angular_drive_force_limit"), Out.AngularDriveForceLimit.Value);
        Out.AngularOrientationTarget.bGiven = Params->TryGetArrayField(TEXT("angular_orientation_target"), Arr)
            && PhysicsConstraintUtils::ReadNumberTriple(Arr, Out.AngularOrientationTarget.Value);
        Out.LinearPositionDrive.bGiven = Params->TryGetBoolField(TEXT("linear_position_drive"), Out.LinearPositionDrive.Value);
        Out.LinearVelocityDrive.bGiven = Params->TryGetBoolField(TEXT("linear_velocity_drive"), Out.LinearVelocityDrive.Value);
        Out.LinearDriveSpring.bGiven = Params->TryGetNumberField(TEXT("linear_drive_spring"), Out.LinearDriveSpring.Value);
        Out.LinearDriveDamping.bGiven = Params->TryGetNumberField(TEXT("linear_drive_damping"), Out.LinearDriveDamping.Value);
        Out.LinearDriveForceLimit.bGiven = Params->TryGetNumberField(TEXT("linear_drive_force_limit"), Out.LinearDriveForceLimit.Value);
        Out.LinearPositionTarget.bGiven = Params->TryGetArrayField(TEXT("linear_position_target"), Arr)
            && PhysicsConstraintUtils::ReadNumberTriple(Arr, Out.LinearPositionTarget.Value);
        Out.LinearBreakable.bGiven = Params->TryGetBoolField(TEXT("linear_breakable"), Out.LinearBreakable.Value);
        Out.LinearBreakThreshold.bGiven = Params->TryGetNumberField(TEXT("linear_break_threshold"), Out.LinearBreakThreshold.Value);
        Out.AngularBreakable.bGiven = Params->TryGetBoolField(TEXT("angular_breakable"), Out.AngularBreakable.Value);
        Out.AngularBreakThreshold.bGiven = Params->TryGetNumberField(TEXT("angular_break_threshold"), Out.AngularBreakThreshold.Value);
        Out.ProjectionEnabled.bGiven = Params->TryGetBoolField(TEXT("projection_enabled"), Out.ProjectionEnabled.Value);
        Out.ProjectionLinearAlpha.bGiven = Params->TryGetNumberField(TEXT("projection_linear_alpha"), Out.ProjectionLinearAlpha.Value);
        Out.ProjectionAngularAlpha.bGiven = Params->TryGetNumberField(TEXT("projection_angular_alpha"), Out.ProjectionAngularAlpha.Value);
        Out.ProjectionLinearTolerance.bGiven = Params->TryGetNumberField(TEXT("projection_linear_tolerance"), Out.ProjectionLinearTolerance.Value);
        Out.ProjectionAngularTolerance.bGiven = Params->TryGetNumberField(TEXT("projection_angular_tolerance"), Out.ProjectionAngularTolerance.Value);
        Out.ParentDominates.bGiven = Params->TryGetBoolField(TEXT("parent_dominates"), Out.ParentDominates.Value);
        return nullptr;
    }

    // A constraint command's reply, opened with physics_asset_path, constraint_name and constraint_index.
    TSharedPtr<FJsonObject> PhysicsAsset_StartConstraintReply(const UPhysicsAsset* PhysAsset, const FString& ConstraintName, int32 ConstraintIndex)
    {
        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
        ResultObj->SetStringField(TEXT("physics_asset_path"), PhysAsset->GetPathName());
        ResultObj->SetStringField(TEXT("constraint_name"), ConstraintName);
        ResultObj->SetNumberField(TEXT("constraint_index"), ConstraintIndex);
        return ResultObj;
    }
}

// MCP:create_physics_asset - Create a UPhysicsAsset at package_path/name by generating bodies and constraints from a skeletal mesh with the engine's default settings (FPhysicsAssetUtils::CreateFromSkeletalMesh), set that mesh as its preview mesh and save it, with no undo transaction; the mesh itself is not pointed at the new asset (assign_skeletal_mesh_physics_asset does that). Idempotent: a physics asset already at that path is returned unchanged with already_existed=true and saved=false, nothing generated or saved and skeletal_mesh_path not read, while another asset class there is an error before any mesh loads. A generation failure is not an error: the asset is still saved, with bodies_generated=false and generation_message. A failed save, refused while PIE runs, makes success false with save_error. Inspect it with get_physics_asset_info; tune bodies with set_physics_body_properties and joints with add_physics_asset_constraint.
//   @param name - Asset name (e.g. 'PA_Cube').
//   @param package_path - Content folder for the new asset (default /Game/Physics; an empty string also uses it)
//   @param skeletal_mesh_path - Skeletal mesh to generate the bodies from (default /Engine/EngineMeshes/SkeletalCube); read only for a new asset, which fails when it does not load
//   @returns { asset_path: string, asset_name: string, package_path: string, body_count: number, constraint_count: number, skeletal_mesh_path: string, bodies: [{ index: number, bone_name: string, primitive_count?: number }], skeleton_bones: [string], constraints: [object], already_existed: bool, created: bool, saved: bool, bodies_generated?: bool, generation_message?: string, save_error?: string, note?: string }
TSharedPtr<FJsonObject> HandleCreatePhysicsAsset(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath;
    FString Name;
    if (!Params->TryGetStringField(TEXT("package_path"), PackagePath) || PackagePath.IsEmpty())
    {
        PackagePath = TEXT("/Game/Physics");
    }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name)) { return Missing; }

    const FString FullPath = FString::Printf(TEXT("%s/%s"), *PackagePath, *Name);
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(FullPath)) { return Invalid; }

    // What is already at the path is settled before the skeletal mesh loads: another asset class there is an
    // error, and an existing physics asset is returned as it is, with nothing generated or saved.
    const FString Conflict = mcp_common::AssetNameConflict(FullPath, Name, UPhysicsAsset::StaticClass());
    if (!Conflict.IsEmpty())
    {
        return mcp_common::MakeErrorReply(Conflict);
    }
    if (UEditorAssetLibrary::DoesAssetExist(FullPath))
    {
        UPhysicsAsset* Existing = Cast<UPhysicsAsset>(
            StaticLoadObject(UPhysicsAsset::StaticClass(), nullptr, *FullPath));
        if (!Existing)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("An asset exists at %s but does not load as a UPhysicsAsset"), *FullPath));
        }
        TSharedPtr<FJsonObject> ExObj = MakeShared<FJsonObject>();
        FillPhysicsAssetPayload(Existing, ExObj);
        ExObj->SetStringField(TEXT("package_path"), PackagePath);
        ExObj->SetBoolField(TEXT("already_existed"), true);
        ExObj->SetBoolField(TEXT("created"), false);
        ExObj->SetBoolField(TEXT("saved"), false);
        ExObj->SetStringField(TEXT("note"),
            TEXT("A physics asset already exists at this path; it was returned unchanged, skeletal_mesh_path was not read and nothing was saved"));
        return mcp_common::MakeOkReply(ExObj);
    }

    FString SkelMeshPath = TEXT("/Engine/EngineMeshes/SkeletalCube");
    Params->TryGetStringField(TEXT("skeletal_mesh_path"), SkelMeshPath);

    USkeletalMesh* SkelMesh = Cast<USkeletalMesh>(
        StaticLoadObject(USkeletalMesh::StaticClass(), nullptr, *SkelMeshPath));
    if (!SkelMesh)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Skeletal mesh not found: %s"), *SkelMeshPath));
    }

    FString CreateError;
    UPhysicsAsset* PhysAsset = mcp_common::NewAssetInPackage<UPhysicsAsset>(FullPath, Name,
        TEXT("Failed to create UPhysicsAsset"), CreateError);
    if (!PhysAsset)
    {
        return mcp_common::MakeErrorReply(CreateError);
    }
    UPackage* Package = PhysAsset->GetOutermost();

    // Generate bodies/constraints from the skeletal mesh geometry.
    // FPhysicsAssetUtils::CreateFromSkeletalMesh(PhysicsAsset, SkelMesh, Params, OutErrorMessage,
    //                                            bSetToMesh=true, bShowProgress=true)  [PHYSICSUTILITIES_API]
    FPhysAssetCreateParams CreateParams;        // defaults: Sphyl geom, create constraints
    FText ErrorMessage;
    const bool bGenerated = FPhysicsAssetUtils::CreateFromSkeletalMesh(
        PhysAsset, SkelMesh, CreateParams, ErrorMessage,
        /*bSetToMesh=*/false, /*bShowProgress=*/false);

    // Bind the preview/default skeletal mesh regardless of generation outcome.
    PhysAsset->SetPreviewMesh(SkelMesh, /*bMarkAsDirty=*/true);
    PhysAsset->UpdateBodySetupIndexMap();
    PhysAsset->UpdateBoundsBodiesArray();

    FAssetRegistryModule::AssetCreated(PhysAsset);

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Package, PhysAsset, SaveError);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    FillPhysicsAssetPayload(PhysAsset, ResultObj);
    ResultObj->SetStringField(TEXT("package_path"), PackagePath);
    ResultObj->SetBoolField(TEXT("already_existed"), false);
    ResultObj->SetBoolField(TEXT("created"), true);
    ResultObj->SetBoolField(TEXT("bodies_generated"), bGenerated);
    if (!bGenerated && !ErrorMessage.IsEmpty())
    {
        ResultObj->SetStringField(TEXT("generation_message"), ErrorMessage.ToString());
    }

    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("create_physics_asset"), SaveError);
}

// MCP:get_physics_asset_info - Read a UPhysicsAsset without changing it: body_count and constraint_count; each body's index, bone_name and primitive_count; every constraint with its name, child and parent bones, swing, twist and linear motions and limits, and its drive, breakable and projection settings; skeletal_mesh_path, the asset's preview mesh (empty when none, and not necessarily a mesh that uses the asset); and skeleton_bones, that mesh's bone names, the names body and constraint tools accept. A path that does not load as a physics asset is an error. Edit bodies with set_physics_body_properties or set_physics_body_advanced_properties, joints with add_physics_asset_constraint, set_physics_asset_constraint_limits or set_physics_asset_constraint_drives; a world constraint actor is get_physics_constraint_info.
//   @param physics_asset_path - Path of the UPhysicsAsset, e.g. "/Game/Physics/PA_Hero" or "/Game/Physics/PA_Hero.PA_Hero".
//   @returns { asset_path: string, asset_name: string, body_count: number, constraint_count: number, skeletal_mesh_path: string, bodies: [{ index: number, bone_name: string, primitive_count?: number }], skeleton_bones: [string], constraints: [object] }
TSharedPtr<FJsonObject> HandleGetPhysicsAssetInfo(const TSharedPtr<FJsonObject>& Params)
{
    UPhysicsAsset* PhysAsset = nullptr;
    if (TSharedPtr<FJsonObject> Error = PhysicsAsset_ReadAsset(Params, PhysAsset))
    {
        return Error;
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    FillPhysicsAssetPayload(PhysAsset, ResultObj);
    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:set_physics_body_properties - Set the physics type and/or a mass override on one body of a UPhysicsAsset, chosen by body_index or else bone_name, in an undo transaction, then save the asset. Only the fields you pass change; passing neither is an error and nothing is written. An asset with no bodies, no body found or an unknown physics_type is an error, checked before any write. mass_override_enabled is read back from the body. success is false, with save_error, when the save fails (during PIE, for one). For damping, gravity, mass scale or centre of mass use set_physics_body_advanced_properties; get_physics_asset_info reads the body count.
//   @param physics_asset_path - /Game path to the UPhysicsAsset.
//   @param body_index - 0-based index of the body in the asset; wins over bone_name when both are given
//   @param bone_name - Bone whose body to configure, used when body_index is absent
//   @param physics_type - Body physics type (Default|Kinematic|Simulated), case-insensitive; omit to keep the current one
//   @param mass - Mass override in kilograms; also switches the mass override on. Omit to keep the current mass
//   @returns { physics_asset_path, body_index, bone_name, physics_type, physics_type_applied, mass_override_enabled, mass, mass_applied, updated, saved, save_error }
TSharedPtr<FJsonObject> HandleSetPhysicsBodyProperties(const TSharedPtr<FJsonObject>& Params)
{
    UPhysicsAsset* PhysAsset = nullptr;
    int32 BodyIndex = INDEX_NONE;
    USkeletalBodySetup* BodySetup = nullptr;
    if (TSharedPtr<FJsonObject> Error = PhysicsAsset_ReadBody(Params, PhysAsset, BodyIndex, BodySetup))
    {
        return Error;
    }

    // Read and validate both fields before anything is written: a bad physics_type, or a call with neither
    // field, leaves the asset untouched (no transaction, no dirty flag, no save).
    bool bPhysicsTypeApplied = false;
    EPhysicsType NewType = PhysType_Default;
    FString PhysicsTypeStr;
    if (Params->TryGetStringField(TEXT("physics_type"), PhysicsTypeStr) && !PhysicsTypeStr.IsEmpty())
    {
        if (PhysicsTypeStr.Equals(TEXT("Kinematic"), ESearchCase::IgnoreCase))
        {
            NewType = PhysType_Kinematic;
        }
        else if (PhysicsTypeStr.Equals(TEXT("Simulated"), ESearchCase::IgnoreCase))
        {
            NewType = PhysType_Simulated;
        }
        else if (PhysicsTypeStr.Equals(TEXT("Default"), ESearchCase::IgnoreCase))
        {
            NewType = PhysType_Default;
        }
        else
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Invalid physics_type '%s' (Default|Kinematic|Simulated)"), *PhysicsTypeStr));
        }
        bPhysicsTypeApplied = true;
    }

    double Mass = 0.0;
    const bool bMassApplied = Params->TryGetNumberField(TEXT("mass"), Mass);

    if (!bPhysicsTypeApplied && !bMassApplied)
    {
        return mcp_common::MakeErrorReply(
            TEXT("No properties provided (physics_type|mass); nothing was changed"));
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetPhysicsBodyProperties", "Set Physics Body Properties"));
    PhysAsset->Modify();
    BodySetup->Modify();

    if (bPhysicsTypeApplied)
    {
        BodySetup->PhysicsType = NewType;
    }
    if (bMassApplied)
    {
        // FBodyInstance::SetMassOverride(MassInKG, bNewOverrideMass=true)  [ENGINE_API]
        BodySetup->DefaultInstance.SetMassOverride((float)Mass, /*bNewOverrideMass=*/true);
    }

    BodySetup->MarkPackageDirty();
    PhysAsset->MarkPackageDirty();

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(PhysAsset->GetOutermost(), PhysAsset, SaveError);

    // Verify by reading the values back off the (in-memory) body setup.
    FString AppliedPhysicsType;
    switch (BodySetup->PhysicsType)
    {
        case PhysType_Kinematic: AppliedPhysicsType = TEXT("Kinematic"); break;
        case PhysType_Simulated: AppliedPhysicsType = TEXT("Simulated"); break;
        default:                 AppliedPhysicsType = TEXT("Default");   break;
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("physics_asset_path"), PhysAsset->GetPathName());
    ResultObj->SetNumberField(TEXT("body_index"), BodyIndex);
    ResultObj->SetStringField(TEXT("bone_name"), BodySetup->BoneName.ToString());
    ResultObj->SetStringField(TEXT("physics_type"), AppliedPhysicsType);
    ResultObj->SetBoolField(TEXT("physics_type_applied"), bPhysicsTypeApplied);
    // bOverrideMass is a public FBodyInstanceCore bitfield: read it back rather than echo the request, so an
    // override that was already on (or that the write failed to turn on) shows as it is.
    ResultObj->SetBoolField(TEXT("mass_override_enabled"), BodySetup->DefaultInstance.bOverrideMass != 0);
    ResultObj->SetNumberField(TEXT("mass"), BodySetup->DefaultInstance.GetMassOverride());
    ResultObj->SetBoolField(TEXT("mass_applied"), bMassApplied);
    ResultObj->SetBoolField(TEXT("updated"), true);

    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("set_physics_body_properties"), SaveError);
}

// MCP:set_physics_body_advanced_properties - Writes only the given advanced FBodyInstance fields (damping, mass scale, inertia scale, COM nudge, gravity, mass update on scale) of one body of a UPhysicsAsset, chosen by body_index or bone_name, and saves it. Fails when no field is given. Returns the authored values read back, applied_count, saved.
//   @param physics_asset_path - /Game path to the UPhysicsAsset.
//   @param body_index [int] - Index of the body in the asset; wins over bone_name when both are given
//   @param bone_name - Bone name to resolve the body (alternative to body_index).
//   @param linear_damping [number] - Drag that slows linear movement (FBodyInstance LinearDamping, unitless, engine default 0.01); omit to keep the current value
//   @param angular_damping [number] - Drag that slows rotation (FBodyInstance AngularDamping, unitless, engine default 0); omit to keep the current value
//   @param mass_scale [number] - Per-instance mass multiplier (>= 0.001).
//   @param inertia_tensor_scale [array] - [x,y,z] inertia scale.
//   @param com_nudge [array] - [x,y,z] center-of-mass offset.
//   @param enable_gravity [bool] - Whether the body is affected by gravity.
//   @param update_mass_when_scale_changes [bool] - Recompute mass on scale change.
//   @returns { physics_asset_path: string, body_index: int, bone_name: string, linear_damping: number, angular_damping: number, mass_scale: number, inertia_tensor_scale: [number], com_nudge: [number], enable_gravity: bool, update_mass_when_scale_changes: bool, applied_count: int, updated: bool, saved: bool }
TSharedPtr<FJsonObject> HandleSetPhysicsBodyAdvancedProperties(const TSharedPtr<FJsonObject>& Params)
{
    UPhysicsAsset* PhysAsset = nullptr;
    int32 BodyIndex = INDEX_NONE;
    USkeletalBodySetup* BodySetup = nullptr;
    if (TSharedPtr<FJsonObject> Error = PhysicsAsset_ReadBody(Params, PhysAsset, BodyIndex, BodySetup))
    {
        return Error;
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetPhysicsBodyAdvancedProperties", "Set Physics Body Advanced Properties"));
    PhysAsset->Modify();
    BodySetup->Modify();

    FBodyInstance& BI = BodySetup->DefaultInstance;

    int32 Applied = 0;
    double Num = 0.0;
    bool Flag = false;

    if (Params->TryGetNumberField(TEXT("linear_damping"), Num)) { BI.LinearDamping = static_cast<float>(Num); ++Applied; }
    if (Params->TryGetNumberField(TEXT("angular_damping"), Num)) { BI.AngularDamping = static_cast<float>(Num); ++Applied; }
    if (Params->TryGetNumberField(TEXT("mass_scale"), Num)) { BI.SetMassScale(static_cast<float>(Num)); ++Applied; }   // ENGINE_API setter
    {
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (Params->TryGetArrayField(TEXT("inertia_tensor_scale"), Arr) && Arr && Arr->Num() >= 3)
        {
            BI.InertiaTensorScale = FVector((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber());
            ++Applied;
        }
    }
    {
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (Params->TryGetArrayField(TEXT("com_nudge"), Arr) && Arr && Arr->Num() >= 3)
        {
            BI.COMNudge = FVector((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber());
            ++Applied;
        }
    }
    if (Params->TryGetBoolField(TEXT("enable_gravity"), Flag)) { BI.SetEnableGravity(Flag); ++Applied; }   // ENGINE_API setter
    if (Params->TryGetBoolField(TEXT("update_mass_when_scale_changes"), Flag))
    {
        // Bitfield from FBodyInstanceCore base; direct write.
        BI.bUpdateMassWhenScaleChanges = Flag ? 1 : 0;
        ++Applied;
    }

    if (Applied == 0)
    {
        return mcp_common::MakeErrorReply(
            TEXT("No properties provided (linear_damping|angular_damping|mass_scale|inertia_tensor_scale|com_nudge|enable_gravity|update_mass_when_scale_changes)"));
    }

    BodySetup->MarkPackageDirty();
    PhysAsset->MarkPackageDirty();

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(PhysAsset->GetOutermost(), PhysAsset, SaveError);

    // READ-BACK: read AUTHORED struct fields directly off BI. Never call GetBodyMass()/
    // GetBodyInertiaTensor() — those read the live physics actor, which is 0/absent headless.
    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("physics_asset_path"), PhysAsset->GetPathName());
    ResultObj->SetNumberField(TEXT("body_index"), BodyIndex);
    ResultObj->SetStringField(TEXT("bone_name"), BodySetup->BoneName.ToString());
    ResultObj->SetNumberField(TEXT("linear_damping"), BI.LinearDamping);
    ResultObj->SetNumberField(TEXT("angular_damping"), BI.AngularDamping);
    ResultObj->SetNumberField(TEXT("mass_scale"), BI.MassScale);
    {
        ResultObj->SetArrayField(TEXT("inertia_tensor_scale"), mcp_common::VectorToJsonArray(BI.InertiaTensorScale));
    }
    {
        ResultObj->SetArrayField(TEXT("com_nudge"), mcp_common::VectorToJsonArray(BI.COMNudge));
    }
    ResultObj->SetBoolField(TEXT("enable_gravity"), BI.bEnableGravity);
    ResultObj->SetBoolField(TEXT("update_mass_when_scale_changes"), BI.bUpdateMassWhenScaleChanges);
    ResultObj->SetNumberField(TEXT("applied_count"), Applied);
    ResultObj->SetBoolField(TEXT("updated"), true);

    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("set_physics_body_advanced_properties"), SaveError);
}

// MCP:add_simple_collision_to_static_mesh - Appends one simple collision primitive (Box, Sphere or Capsule) sized to the bounds of a UStaticMesh to its BodySetup and saves the mesh; a mesh outside /Game is first duplicated to dest_path. Not idempotent: each call adds one more (clear them with remove_static_mesh_collision).
//   @param static_mesh_path - Path to the source UStaticMesh.
//   @param dest_path - Path under /Game to copy a mesh that is not under /Game into before editing (required then; an existing mesh there is reused and edited)
//   @param shape - Box, Sphere or Capsule, case-insensitive, sized from the mesh bounds (default Box; any other value also adds a Box)
//   @returns { static_mesh_path, shape, box_count, sphere_count, capsule_count, element_count, collision_added, duplicated_to_game, saved, save_error }
TSharedPtr<FJsonObject> HandleAddSimpleCollisionToStaticMesh(const TSharedPtr<FJsonObject>& Params)
{
    bool bDuplicated = false;
    TSharedPtr<FJsonObject> Error;
    UStaticMesh* StaticMesh = ResolveWritableStaticMesh(Params, bDuplicated, Error);
    if (!StaticMesh)
    {
        return Error;
    }

    FString ShapeStr = TEXT("Box");
    Params->TryGetStringField(TEXT("shape"), ShapeStr);

    UBodySetup* BodySetup = StaticMesh->GetBodySetup();
    if (!BodySetup)
    {
        StaticMesh->CreateBodySetup();
        BodySetup = StaticMesh->GetBodySetup();
    }
    if (!BodySetup)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Failed to obtain/create BodySetup for static mesh"));
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "AddSimpleCollision", "Add Simple Collision To Static Mesh"));
    StaticMesh->Modify();
    BodySetup->Modify();

    // Size the primitive from the mesh's local bounds so it is a reasonable fit.
    const FBoxSphereBounds Bounds = StaticMesh->GetBounds();
    const FVector Extent = Bounds.BoxExtent;
    const FVector Center = Bounds.Origin;

    FString AppliedShape;
    if (ShapeStr.Equals(TEXT("Sphere"), ESearchCase::IgnoreCase))
    {
        FKSphereElem Elem;
        Elem.Center = Center;
        Elem.Radius = FMath::Max3(Extent.X, Extent.Y, Extent.Z);
        BodySetup->AggGeom.SphereElems.Add(Elem);
        AppliedShape = TEXT("Sphere");
    }
    else if (ShapeStr.Equals(TEXT("Capsule"), ESearchCase::IgnoreCase))
    {
        FKSphylElem Elem;
        Elem.Center = Center;
        Elem.Radius = FMath::Max(Extent.X, Extent.Y);
        // Length is the distance between the two hemisphere centers (cylinder part).
        Elem.Length = FMath::Max(0.0f, (float)(2.0 * Extent.Z - 2.0 * Elem.Radius));
        BodySetup->AggGeom.SphylElems.Add(Elem);
        AppliedShape = TEXT("Capsule");
    }
    else // default Box
    {
        FKBoxElem Elem;
        Elem.Center = Center;
        Elem.X = (float)(2.0 * Extent.X);
        Elem.Y = (float)(2.0 * Extent.Y);
        Elem.Z = (float)(2.0 * Extent.Z);
        BodySetup->AggGeom.BoxElems.Add(Elem);
        AppliedShape = TEXT("Box");
    }

    // Rebuild the runtime physics representation and persist.
    BodySetup->InvalidatePhysicsData();
    BodySetup->CreatePhysicsMeshes();
    BodySetup->MarkPackageDirty();
    StaticMesh->MarkPackageDirty();

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(StaticMesh->GetOutermost(), StaticMesh, SaveError);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("static_mesh_path"), StaticMesh->GetPathName());
    ResultObj->SetStringField(TEXT("shape"), AppliedShape);
    ResultObj->SetNumberField(TEXT("box_count"), BodySetup->AggGeom.BoxElems.Num());
    ResultObj->SetNumberField(TEXT("sphere_count"), BodySetup->AggGeom.SphereElems.Num());
    ResultObj->SetNumberField(TEXT("capsule_count"), BodySetup->AggGeom.SphylElems.Num());
    ResultObj->SetNumberField(TEXT("element_count"), BodySetup->AggGeom.GetElementCount());
    ResultObj->SetBoolField(TEXT("collision_added"), true);
    ResultObj->SetBoolField(TEXT("duplicated_to_game"), bDuplicated);

    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("add_simple_collision_to_static_mesh"), SaveError);
}

// Static-mesh collision CRUD: clear (RemoveSimpleCollision), convex decomposition, K-DOP.

// MCP:remove_static_mesh_collision - Clear ALL simple/convex collision primitives from a UStaticMesh's BodySetup (the 'clear' half of the collision CRUD).  Calls UBodySetup::RemoveSimpleCollision (InvalidatePhysicsData + AggGeom.EmptyElements), rebuilds the physics meshes and saves; a failed save makes success false with save_error.  Reuses the same duplicate-first logic as add_simple_collision_to_static_mesh so a read-only /Engine source is copied into dest_path first (an existing copy there is reused); duplicated is true when this call created that copy.  Idempotent: clearing an already-empty BodySetup (or a mesh with no BodySetup) is a no-op reported via already_empty:true, where saved reports only the save of a copy this call created and is false, with nothing saved, when there is none.
//   @param static_mesh_path - Path to the source UStaticMesh.
//   @param dest_path - /Game path to duplicate a read-only /Engine source mesh into first (required only when the source is not under /Game).
//   @returns { static_mesh_path, removed_count, element_count, collision_removed, already_empty, duplicated_to_game, duplicated, saved, save_error?, note? }
TSharedPtr<FJsonObject> HandleRemoveStaticMeshCollision(const TSharedPtr<FJsonObject>& Params)
{
    bool bDuplicated = false;
    TSharedPtr<FJsonObject> Error;
    FWritableMeshCopy Copy;
    UStaticMesh* StaticMesh = ResolveWritableStaticMesh(Params, bDuplicated, Error, &Copy);
    if (!StaticMesh)
    {
        return Error;
    }

    // No BodySetup, or an empty one, means there is no simple collision to clear -> honest
    // no-op (do NOT create a BodySetup just to empty it; nothing to dirty or save). Report
    // already_empty so re-runs are stable. The only thing such a call can have saved is the
    // /Game copy of an /Engine mesh it created on the way, so that save is what `saved` reports.
    UBodySetup* BodySetup = StaticMesh->GetBodySetup();
    const int32 BeforeCount = BodySetup ? BodySetup->AggGeom.GetElementCount() : 0;
    if (BeforeCount == 0)
    {
        TSharedPtr<FJsonObject> EmptyObj = MakeShared<FJsonObject>();
        EmptyObj->SetStringField(TEXT("static_mesh_path"), StaticMesh->GetPathName());
        EmptyObj->SetNumberField(TEXT("removed_count"), 0);
        EmptyObj->SetNumberField(TEXT("element_count"), 0);
        EmptyObj->SetBoolField(TEXT("collision_removed"), false);
        EmptyObj->SetBoolField(TEXT("already_empty"), true);
        EmptyObj->SetBoolField(TEXT("duplicated_to_game"), bDuplicated);
        EmptyObj->SetBoolField(TEXT("duplicated"), Copy.bCreated);
        if (Copy.bCreated)
        {
            EmptyObj->SetStringField(TEXT("note"),
                TEXT("The mesh had no simple collision to clear; this call only created the /Game copy at dest_path"));
            return mcp_common::MakeSavedReply(EmptyObj, Copy.bSaved, TEXT("remove_static_mesh_collision"), Copy.SaveError);
        }
        EmptyObj->SetBoolField(TEXT("saved"), false);
        EmptyObj->SetStringField(TEXT("note"), TEXT("The mesh had no simple collision to clear; nothing was changed or saved"));
        return mcp_common::MakeOkReply(EmptyObj);
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "RemoveStaticMeshCollision", "Remove Static Mesh Collision"));
    StaticMesh->Modify();
    BodySetup->Modify();

    // RemoveSimpleCollision() == InvalidatePhysicsData() + AggGeom.EmptyElements().
    BodySetup->RemoveSimpleCollision();
    // Rebuild the (now-empty) runtime physics representation and persist.
    BodySetup->CreatePhysicsMeshes();
    BodySetup->MarkPackageDirty();
    StaticMesh->MarkPackageDirty();

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(StaticMesh->GetOutermost(), StaticMesh, SaveError);

    const int32 AfterCount = BodySetup->AggGeom.GetElementCount();

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("static_mesh_path"), StaticMesh->GetPathName());
    ResultObj->SetNumberField(TEXT("removed_count"), BeforeCount - AfterCount);
    ResultObj->SetNumberField(TEXT("element_count"), AfterCount);
    ResultObj->SetBoolField(TEXT("collision_removed"), AfterCount < BeforeCount);
    ResultObj->SetBoolField(TEXT("already_empty"), false);
    ResultObj->SetBoolField(TEXT("duplicated_to_game"), bDuplicated);
    ResultObj->SetBoolField(TEXT("duplicated"), Copy.bCreated);

    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("remove_static_mesh_collision"), SaveError);
}

// MCP:set_static_mesh_convex_collision - Replace a UStaticMesh's simple collision with a V-HACD convex decomposition (the "Auto Convex Collision" editor action) via UStaticMeshEditorSubsystem::SetConvexDecompositionCollisionsWithNotification.  The subsystem removes any existing collision first, so this is idempotent (re-running replaces, never duplicates).  Reuses the duplicate-first logic so a read-only /Engine source is copied into dest_path first.  The subsystem returns false honestly on bad input (no valid mesh description / not on the game thread / under PIE) — surfaced as a CLEAN error, never a fake success.
//   @param static_mesh_path - Path to the source UStaticMesh.
//   @param dest_path - /Game path to duplicate a read-only /Engine source mesh into first (required only when the source is not under /Game).
//   @param hull_count - Maximum number of convex pieces to create (default 4, must be positive).
//   @param max_hull_verts - Maximum vertices per generated hull (default 16).
//   @param hull_precision - Voxel resolution used during decomposition (default 100000, must be positive).
//   @returns { static_mesh_path, hull_count, max_hull_verts, hull_precision, convex_count, element_count, collision_set, duplicated_to_game, saved, save_error? }
TSharedPtr<FJsonObject> HandleSetStaticMeshConvexCollision(const TSharedPtr<FJsonObject>& Params)
{
    bool bDuplicated = false;
    TSharedPtr<FJsonObject> Error;
    UStaticMesh* StaticMesh = ResolveWritableStaticMesh(Params, bDuplicated, Error);
    if (!StaticMesh)
    {
        return Error;
    }

    // V-HACD parameters (sane editor defaults; positive-guarded below).
    int32 HullCount = 4;
    int32 MaxHullVerts = 16;
    int32 HullPrecision = 100000;
    {
        double Num = 0.0;
        if (Params->TryGetNumberField(TEXT("hull_count"), Num))     { HullCount = static_cast<int32>(Num); }
        if (Params->TryGetNumberField(TEXT("max_hull_verts"), Num)) { MaxHullVerts = static_cast<int32>(Num); }
        if (Params->TryGetNumberField(TEXT("hull_precision"), Num)) { HullPrecision = static_cast<int32>(Num); }
    }
    if (HullCount <= 0 || HullPrecision <= 0)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("hull_count and hull_precision must be positive (got hull_count=%d, hull_precision=%d)"),
                HullCount, HullPrecision));
    }

    UStaticMeshEditorSubsystem* SMES = GEditor ? GEditor->GetEditorSubsystem<UStaticMeshEditorSubsystem>() : nullptr;
    if (!SMES)
    {
        return mcp_common::MakeErrorReply(TEXT("UStaticMeshEditorSubsystem unavailable"));
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetStaticMeshConvexCollision", "Set Static Mesh Convex Collision"));
    StaticMesh->Modify();

    // Synchronous: the subsystem runs V-HACD (Async + ParallelFor) then WaitFor before
    // returning, and pre-sets GIsRunningUnattendedScript so no progress dialog blocks headless.
    // Returns false honestly on bad input (invalid MeshDescription / under PIE / off game thread).
    const bool bSet = SMES->SetConvexDecompositionCollisionsWithNotification(
        StaticMesh, HullCount, MaxHullVerts, HullPrecision, /*bApplyChanges=*/true);
    if (!bSet)
    {
        // Honest failure: do NOT report success:true. The subsystem refused the input.
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Convex decomposition failed for %s (subsystem returned false: invalid mesh "
                "description, running under PIE, or not on the game thread)"), *StaticMesh->GetPathName()));
    }

    StaticMesh->MarkPackageDirty();
    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(StaticMesh->GetOutermost(), StaticMesh, SaveError);

    // Read back the authored convex/element counts from the subsystem (negative = query failed).
    const int32 ConvexCount = SMES->GetConvexCollisionCount(StaticMesh);
    int32 ElementCount = 0;
    if (UBodySetup* BodySetup = StaticMesh->GetBodySetup())
    {
        ElementCount = BodySetup->AggGeom.GetElementCount();
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("static_mesh_path"), StaticMesh->GetPathName());
    ResultObj->SetNumberField(TEXT("hull_count"), HullCount);
    ResultObj->SetNumberField(TEXT("max_hull_verts"), MaxHullVerts);
    ResultObj->SetNumberField(TEXT("hull_precision"), HullPrecision);
    ResultObj->SetNumberField(TEXT("convex_count"), ConvexCount);
    ResultObj->SetNumberField(TEXT("element_count"), ElementCount);
    ResultObj->SetBoolField(TEXT("collision_set"), true);
    ResultObj->SetBoolField(TEXT("duplicated_to_game"), bDuplicated);

    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("set_static_mesh_convex_collision"), SaveError);
}

// MCP:set_static_mesh_kdop_collision - Add one K-DOP simple collision hull to a UStaticMesh asset, found by path, as the Static Mesh Editor's Add K-DOP action does, in one undo step, then save it. The hull is a convex element fitted around the LOD0 vertices: NDOP10_X, NDOP10_Y or NDOP10_Z bevels the four edges along that axis, NDOP18 (default, case-insensitive) every edge, NDOP26 every edge and corner. Existing collision is kept, so each call appends another hull (added_index). A mesh outside /Game, such as /Engine content, is first copied to dest_path under /Game, reusing a copy already there. An invalid shape, unknown mesh or running PIE session is an error; a failed save makes success false with saved=false and save_error. Replace all collision with a convex decomposition: set_static_mesh_convex_collision; box, sphere or capsule: add_simple_collision_to_static_mesh; clear: remove_static_mesh_collision.
//   @param static_mesh_path - Path to the source UStaticMesh.
//   @param dest_path - /Game path to duplicate a read-only /Engine source mesh into first (required only when the source is not under /Game).
//   @param shape - K-DOP hull shape: NDOP10_X | NDOP10_Y | NDOP10_Z | NDOP18 | NDOP26 (default NDOP18).
//   @returns { static_mesh_path, shape, added_index, simple_collision_count, element_count, collision_added, duplicated_to_game, saved, save_error? }
//   @hints additive not-idempotent
TSharedPtr<FJsonObject> HandleSetStaticMeshKDopCollision(const TSharedPtr<FJsonObject>& Params)
{
    bool bDuplicated = false;
    TSharedPtr<FJsonObject> Error;
    UStaticMesh* StaticMesh = ResolveWritableStaticMesh(Params, bDuplicated, Error);
    if (!StaticMesh)
    {
        return Error;
    }

    FString ShapeStr = TEXT("NDOP18");
    Params->TryGetStringField(TEXT("shape"), ShapeStr);
    EScriptCollisionShapeType Shape = EScriptCollisionShapeType::NDOP18;
    if (!ParseKDopShape(ShapeStr, Shape))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Invalid shape '%s' (NDOP10_X|NDOP10_Y|NDOP10_Z|NDOP18|NDOP26)"), *ShapeStr));
    }

    UStaticMeshEditorSubsystem* SMES = GEditor ? GEditor->GetEditorSubsystem<UStaticMeshEditorSubsystem>() : nullptr;
    if (!SMES)
    {
        return mcp_common::MakeErrorReply(TEXT("UStaticMeshEditorSubsystem unavailable"));
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetStaticMeshKDopCollision", "Set Static Mesh K-DOP Collision"));
    StaticMesh->Modify();

    // Returns the index of the newly-created collision; a negative value means it failed.
    const int32 AddedIndex = SMES->AddSimpleCollisionsWithNotification(StaticMesh, Shape, /*bApplyChanges=*/true);
    if (AddedIndex < 0)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("K-DOP collision (%s) failed for %s (subsystem returned index %d)"),
                *ShapeStr, *StaticMesh->GetPathName(), AddedIndex));
    }

    StaticMesh->MarkPackageDirty();
    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(StaticMesh->GetOutermost(), StaticMesh, SaveError);

    const int32 SimpleCount = SMES->GetSimpleCollisionCount(StaticMesh);
    int32 ElementCount = 0;
    if (UBodySetup* BodySetup = StaticMesh->GetBodySetup())
    {
        ElementCount = BodySetup->AggGeom.GetElementCount();
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("static_mesh_path"), StaticMesh->GetPathName());
    ResultObj->SetStringField(TEXT("shape"), ShapeStr);
    ResultObj->SetNumberField(TEXT("added_index"), AddedIndex);
    ResultObj->SetNumberField(TEXT("simple_collision_count"), SimpleCount);
    ResultObj->SetNumberField(TEXT("element_count"), ElementCount);
    ResultObj->SetBoolField(TEXT("collision_added"), true);
    ResultObj->SetBoolField(TEXT("duplicated_to_game"), bDuplicated);

    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("set_static_mesh_kdop_collision"), SaveError);
}

// Ragdoll constraints inside the UPhysicsAsset (distinct from the world constraint-actor commands).

// MCP:add_physics_asset_constraint - Author a ragdoll constraint (joint) inside a UPhysicsAsset between two bones.  Creates a UPhysicsConstraintTemplate via FPhysicsAssetUtils::CreateNewConstraint (idempotent by name — re-using a name returns the existing joint), sets the child/parent bones and a Limited swing+twist cone, and snaps the joint frames to the bones' default relative transform when BOTH bones exist in the preview-mesh skeleton (skipped honestly otherwise, since the engine fatal-checks an unknown bone).  Refine motions/limits afterwards with set_physics_asset_constraint_limits.
//   @param physics_asset_path - /Game path to the UPhysicsAsset to add the constraint to.
//   @param child_bone - Name of the child bone/body (ConstraintBone1, the moving end).
//   @param parent_bone - Name of the parent bone/body (ConstraintBone2, the anchor end).
//   @param constraint_name - Optional joint name (defaults to child_bone); re-using an existing name is idempotent (no duplicate).
//   @returns { physics_asset_path: string, constraint_name: string, constraint_index: int, child_bone: string, parent_bone: string, already_existed: bool, child_in_skeleton: bool, parent_in_skeleton: bool, child_body_index: int, parent_body_index: int, frames_snapped: bool, collision_disabled: bool, constraint_count: int, saved: bool, save_error?: string, note?: string, constraint: object }
TSharedPtr<FJsonObject> HandleAddPhysicsAssetConstraint(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("physics_asset_path"), AssetPath)) { return Missing; }
    FString ChildBone;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("child_bone"), ChildBone)) { return Missing; }
    FString ParentBone;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("parent_bone"), ParentBone)) { return Missing; }
    // Optional: defaults to child_bone (engine convention). Plain read so the catalog
    // extractor sees it as an optional string (an "|| IsEmpty()" guard reads as required).
    FString ConstraintName;
    Params->TryGetStringField(TEXT("constraint_name"), ConstraintName);
    if (ConstraintName.IsEmpty())
    {
        ConstraintName = ChildBone;
    }

    UPhysicsAsset* PhysAsset = nullptr;
    if (TSharedPtr<FJsonObject> Error = PhysicsAsset_LoadAsset(AssetPath, PhysAsset))
    {
        return Error;
    }

    const FName ConName(*ConstraintName);
    const FName ChildName(*ChildBone);
    const FName ParentName(*ParentBone);

    const bool bAlreadyExisted = PhysAsset->FindConstraintIndex(ConName) != INDEX_NONE;

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "AddPhysicsAssetConstraint", "Add Physics Asset Constraint"));
    PhysAsset->Modify();

    // CreateNewConstraint is idempotent by name (returns the existing index if present).
    const int32 Idx = FPhysicsAssetUtils::CreateNewConstraint(PhysAsset, ConName);
    if (!PhysAsset->ConstraintSetup.IsValidIndex(Idx))
    {
        return mcp_common::MakeErrorReply(
            TEXT("Failed to create constraint (CanCreateConstraints() returned false)"));
    }

    UPhysicsConstraintTemplate* CS = PhysAsset->ConstraintSetup[Idx];
    CS->Modify();
    FConstraintInstance& CI = CS->DefaultInstance;
    CI.JointName = ConName;
    CI.ConstraintBone1 = ChildName;
    CI.ConstraintBone2 = ParentName;
    // Sensible ragdoll default: a Limited swing+twist cone (PhAT's create-constraint default).
    CI.SetAngularSwing1Motion(ACM_Limited);
    CI.SetAngularSwing2Motion(ACM_Limited);
    CI.SetAngularTwistMotion(ACM_Limited);

    const int32 ChildBodyIdx = PhysAsset->FindBodyIndex(ChildName);
    const int32 ParentBodyIdx = PhysAsset->FindBodyIndex(ParentName);
    bool bChildInSkel = false;
    bool bParentInSkel = false;
    if (const USkeletalMesh* Preview = PhysAsset->GetPreviewMesh())
    {
        const FReferenceSkeleton& RefSkel = Preview->GetRefSkeleton();
        bChildInSkel = RefSkel.FindBoneIndex(ChildName) != INDEX_NONE;
        bParentInSkel = RefSkel.FindBoneIndex(ParentName) != INDEX_NONE;
    }

    bool bFramesSnapped = false;
    // SnapTransformsToDefault -> CalculateRelativeBoneTransform fatal-checks a bone that is not
    // present in the preview skeleton, so only snap when BOTH bones resolve there.
    if (bChildInSkel && bParentInSkel)
    {
        CI.SnapTransformsToDefault(EConstraintTransformComponentFlags::All, PhysAsset);
        bFramesSnapped = true;
    }
    // Load-bearing: the default profile is serialized back into DefaultInstance on save.
    CS->SetDefaultProfile(CI);

    // Ragdoll default: disable collision between the two constrained bodies (when both exist).
    bool bCollisionDisabled = false;
    if (PhysAsset->SkeletalBodySetups.IsValidIndex(ChildBodyIdx) &&
        PhysAsset->SkeletalBodySetups.IsValidIndex(ParentBodyIdx) &&
        ChildBodyIdx != ParentBodyIdx)
    {
        PhysAsset->DisableCollision(ChildBodyIdx, ParentBodyIdx);
        bCollisionDisabled = true;
    }

    PhysAsset->MarkPackageDirty();
    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(PhysAsset->GetOutermost(), PhysAsset, SaveError);

    TSharedPtr<FJsonObject> ResultObj = PhysicsAsset_StartConstraintReply(PhysAsset, ConstraintName, Idx);
    ResultObj->SetStringField(TEXT("child_bone"), ChildBone);
    ResultObj->SetStringField(TEXT("parent_bone"), ParentBone);
    ResultObj->SetBoolField(TEXT("already_existed"), bAlreadyExisted);
    ResultObj->SetBoolField(TEXT("child_in_skeleton"), bChildInSkel);
    ResultObj->SetBoolField(TEXT("parent_in_skeleton"), bParentInSkel);
    ResultObj->SetNumberField(TEXT("child_body_index"), ChildBodyIdx);
    ResultObj->SetNumberField(TEXT("parent_body_index"), ParentBodyIdx);
    ResultObj->SetBoolField(TEXT("frames_snapped"), bFramesSnapped);
    ResultObj->SetBoolField(TEXT("collision_disabled"), bCollisionDisabled);
    ResultObj->SetNumberField(TEXT("constraint_count"), PhysAsset->ConstraintSetup.Num());
    if (!bFramesSnapped)
    {
        ResultObj->SetStringField(TEXT("note"),
            TEXT("Joint frames not snapped: child_bone and parent_bone are not both present in the "
                 "preview-mesh skeleton, so the frames stay at their defaults. The constraint itself "
                 "(bones + limits) is authored and persisted; on a real ragdoll skeleton both bones "
                 "resolve and the frames snap to the bones' relative transform."));
    }
    ResultObj->SetObjectField(TEXT("constraint"), MakeConstraintObject(Idx, CS));
    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("add_physics_asset_constraint"), SaveError);
}

// MCP:remove_physics_asset_constraint - Remove a ragdoll constraint (joint) from a UPhysicsAsset — the inverse of add_physics_asset_constraint.  Resolves the joint by constraint_name (preferred) or constraint_index and deletes it via FPhysicsAssetUtils::DestroyConstraint (the same ConstraintSetup.RemoveAt the PhAT editor runs).  Idempotent: an already-absent constraint returns removed:false/already_absent:true with success (no mutation, no save).
//   @param physics_asset_path - /Game path to the UPhysicsAsset to remove the constraint from.
//   @param constraint_name - Optional joint name (JointName) of the constraint to remove; preferred over constraint_index when both are given (see get_physics_asset_info).
//   @param constraint_index - Optional constraint index to remove (used when constraint_name is absent).
//   @returns { physics_asset_path: string, constraint_name: string, constraint_index: int, child_bone: string, parent_bone: string, removed: bool, already_absent: bool, constraint_count: int, saved: bool, save_error?: string }
TSharedPtr<FJsonObject> HandleRemovePhysicsAssetConstraint(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("physics_asset_path"), AssetPath)) { return Missing; }
    // Optional: constraint_name (preferred) or constraint_index. Plain reads so the catalog
    // extractor sees them as optional (an "|| IsEmpty()" guard reads as required).
    FString ConstraintName;
    Params->TryGetStringField(TEXT("constraint_name"), ConstraintName);
    int32 ConstraintIndex = INDEX_NONE;
    const bool bHasIndex = Params->TryGetNumberField(TEXT("constraint_index"), ConstraintIndex);
    if (ConstraintName.IsEmpty() && !bHasIndex)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Provide at least one of: constraint_name (preferred) or constraint_index"));
    }

    UPhysicsAsset* PhysAsset = nullptr;
    if (TSharedPtr<FJsonObject> Error = PhysicsAsset_LoadAsset(AssetPath, PhysAsset))
    {
        return Error;
    }

    // constraint_name wins when both are supplied.
    int32 Idx = INDEX_NONE;
    if (!ConstraintName.IsEmpty())
    {
        Idx = PhysAsset->FindConstraintIndex(FName(*ConstraintName));
    }
    else
    {
        Idx = ConstraintIndex;
    }

    // Already-absent: honest no-op (no Modify/save). DestroyConstraint is a bare RemoveAt
    // that does not validate the index, so we MUST guard it here or it crashes.
    if (!PhysAsset->ConstraintSetup.IsValidIndex(Idx))
    {
        TSharedPtr<FJsonObject> AbsentObj = MakeShared<FJsonObject>();
        AbsentObj->SetStringField(TEXT("physics_asset_path"), PhysAsset->GetPathName());
        AbsentObj->SetStringField(TEXT("constraint_name"), ConstraintName);
        AbsentObj->SetBoolField(TEXT("removed"), false);
        AbsentObj->SetBoolField(TEXT("already_absent"), true);
        AbsentObj->SetNumberField(TEXT("constraint_count"), PhysAsset->ConstraintSetup.Num());
        AbsentObj->SetBoolField(TEXT("saved"), false);
        return mcp_common::MakeOkReply(AbsentObj);
    }

    // Capture the joint's identity BEFORE deletion (the setup is freed by DestroyConstraint).
    const UPhysicsConstraintTemplate* CS = PhysAsset->ConstraintSetup[Idx];
    const FString ChildBone = CS->DefaultInstance.ConstraintBone1.ToString();
    const FString ParentBone = CS->DefaultInstance.ConstraintBone2.ToString();

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "RemovePhysicsAssetConstraint", "Remove Physics Asset Constraint"));
    PhysAsset->Modify();

    // Bare ConstraintSetup.RemoveAt(Idx); constraints do not participate in the collision-disable
    // table or the body index-maps, so there is nothing else to fix up.
    FPhysicsAssetUtils::DestroyConstraint(PhysAsset, Idx);

    PhysAsset->MarkPackageDirty();
    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(PhysAsset->GetOutermost(), PhysAsset, SaveError);

    TSharedPtr<FJsonObject> ResultObj = PhysicsAsset_StartConstraintReply(PhysAsset, ConstraintName, Idx);
    ResultObj->SetStringField(TEXT("child_bone"), ChildBone);
    ResultObj->SetStringField(TEXT("parent_bone"), ParentBone);
    ResultObj->SetBoolField(TEXT("removed"), true);
    ResultObj->SetBoolField(TEXT("already_absent"), false);
    ResultObj->SetNumberField(TEXT("constraint_count"), PhysAsset->ConstraintSetup.Num());
    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("remove_physics_asset_constraint"), SaveError);
}

// MCP:set_physics_asset_constraint_limits - Set the linear/angular motion types and limits of an existing constraint inside a UPhysicsAsset (read-modify-write: only the axes you provide change).  Angular limits are in degrees; the linear limit is a single value shared across X/Y/Z (cm).  Each *_motion is Free|Limited|Locked; providing a *_limit without its motion implies Limited on that axis.  Idempotent (set-to-fixed-value).
//   @param physics_asset_path - /Game path to the UPhysicsAsset.
//   @param constraint_name - Name (JointName) of the constraint to edit (see get_physics_asset_info).
//   @param swing1_motion - Optional cone swing1 motion: Free|Limited|Locked.
//   @param swing1_limit - Optional cone swing1 limit in degrees.
//   @param swing2_motion - Optional cone swing2 motion: Free|Limited|Locked.
//   @param swing2_limit - Optional cone swing2 limit in degrees.
//   @param twist_motion - Optional twist motion: Free|Limited|Locked.
//   @param twist_limit - Optional twist limit in degrees.
//   @param linear_x_motion - Optional linear X-axis motion: Free|Limited|Locked.
//   @param linear_y_motion - Optional linear Y-axis motion: Free|Limited|Locked.
//   @param linear_z_motion - Optional linear Z-axis motion: Free|Limited|Locked.
//   @param linear_limit - Optional shared linear limit in cm (applied to all 3 axes).
//   @returns { physics_asset_path: string, constraint_name: string, constraint_index: int, applied_count: int, saved: bool, save_error?: string, constraint: object }
TSharedPtr<FJsonObject> HandleSetPhysicsAssetConstraintLimits(const TSharedPtr<FJsonObject>& Params)
{
    UPhysicsAsset* PhysAsset = nullptr;
    FString ConstraintName;
    int32 Idx = INDEX_NONE;
    UPhysicsConstraintTemplate* CS = nullptr;
    if (TSharedPtr<FJsonObject> Error = PhysicsAsset_ReadConstraint(Params, PhysAsset, ConstraintName, Idx, CS))
    {
        return Error;
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetPhysicsAssetConstraintLimits", "Set Physics Asset Constraint Limits"));
    PhysAsset->Modify();
    CS->Modify();
    FConstraintInstance& CI = CS->DefaultInstance;

    int32 Applied = 0;
    FString MotionStr;
    double LimitVal = 0.0;

    // --- Angular swing1 ---
    {
        EAngularConstraintMotion Motion = CI.GetAngularSwing1Motion();
        const bool bMotion = Params->TryGetStringField(TEXT("swing1_motion"), MotionStr) && !MotionStr.IsEmpty();
        if (bMotion && !ParseAngularMotion(MotionStr, Motion))
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Invalid swing1_motion '%s' (Free|Limited|Locked)"), *MotionStr));
        }
        const bool bLimit = Params->TryGetNumberField(TEXT("swing1_limit"), LimitVal);
        float Limit = bLimit ? static_cast<float>(LimitVal) : CI.GetAngularSwing1Limit();
        if (bLimit && !bMotion) { Motion = ACM_Limited; }
        if (bMotion || bLimit) { CI.SetAngularSwing1Limit(Motion, Limit); ++Applied; }
    }
    // --- Angular swing2 ---
    {
        EAngularConstraintMotion Motion = CI.GetAngularSwing2Motion();
        const bool bMotion = Params->TryGetStringField(TEXT("swing2_motion"), MotionStr) && !MotionStr.IsEmpty();
        if (bMotion && !ParseAngularMotion(MotionStr, Motion))
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Invalid swing2_motion '%s' (Free|Limited|Locked)"), *MotionStr));
        }
        const bool bLimit = Params->TryGetNumberField(TEXT("swing2_limit"), LimitVal);
        float Limit = bLimit ? static_cast<float>(LimitVal) : CI.GetAngularSwing2Limit();
        if (bLimit && !bMotion) { Motion = ACM_Limited; }
        if (bMotion || bLimit) { CI.SetAngularSwing2Limit(Motion, Limit); ++Applied; }
    }
    // --- Angular twist ---
    {
        EAngularConstraintMotion Motion = CI.GetAngularTwistMotion();
        const bool bMotion = Params->TryGetStringField(TEXT("twist_motion"), MotionStr) && !MotionStr.IsEmpty();
        if (bMotion && !ParseAngularMotion(MotionStr, Motion))
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Invalid twist_motion '%s' (Free|Limited|Locked)"), *MotionStr));
        }
        const bool bLimit = Params->TryGetNumberField(TEXT("twist_limit"), LimitVal);
        float Limit = bLimit ? static_cast<float>(LimitVal) : CI.GetAngularTwistLimit();
        if (bLimit && !bMotion) { Motion = ACM_Limited; }
        if (bMotion || bLimit) { CI.SetAngularTwistLimit(Motion, Limit); ++Applied; }
    }
    // --- Linear (per-axis motion + a single shared limit size) ---
    {
        ELinearConstraintMotion MX = CI.GetLinearXMotion();
        ELinearConstraintMotion MY = CI.GetLinearYMotion();
        ELinearConstraintMotion MZ = CI.GetLinearZMotion();
        const bool bX = Params->TryGetStringField(TEXT("linear_x_motion"), MotionStr) && !MotionStr.IsEmpty();
        if (bX && !ParseLinearMotion(MotionStr, MX))
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Invalid linear_x_motion '%s' (Free|Limited|Locked)"), *MotionStr));
        }
        const bool bY = Params->TryGetStringField(TEXT("linear_y_motion"), MotionStr) && !MotionStr.IsEmpty();
        if (bY && !ParseLinearMotion(MotionStr, MY))
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Invalid linear_y_motion '%s' (Free|Limited|Locked)"), *MotionStr));
        }
        const bool bZ = Params->TryGetStringField(TEXT("linear_z_motion"), MotionStr) && !MotionStr.IsEmpty();
        if (bZ && !ParseLinearMotion(MotionStr, MZ))
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Invalid linear_z_motion '%s' (Free|Limited|Locked)"), *MotionStr));
        }
        const bool bLin = Params->TryGetNumberField(TEXT("linear_limit"), LimitVal);
        float Size = bLin ? static_cast<float>(LimitVal) : CI.GetLinearLimit();
        if (bX || bY || bZ || bLin) { CI.SetLinearLimits(MX, MY, MZ, Size); ++Applied; }
    }

    // Load-bearing: the default profile is serialized back into DefaultInstance on save.
    CS->SetDefaultProfile(CI);

    PhysAsset->MarkPackageDirty();
    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(PhysAsset->GetOutermost(), PhysAsset, SaveError);

    TSharedPtr<FJsonObject> ResultObj = PhysicsAsset_StartConstraintReply(PhysAsset, ConstraintName, Idx);
    ResultObj->SetNumberField(TEXT("applied_count"), Applied);
    ResultObj->SetObjectField(TEXT("constraint"), MakeConstraintObject(Idx, CS));
    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("set_physics_asset_constraint_limits"), SaveError);
}

// MCP:set_physics_asset_constraint_drives - Set a UPhysicsAsset constraint's drives (linear + angular motors), breakable thresholds, and projection — the "behavior beyond limits" (read-modify-write; only provided fields change; idempotent set-to-fixed-value).  Authoring-only: the values persist into the asset (the live motor effect is observed in PIE/sim).
//   @param physics_asset_path - /Game path to the UPhysicsAsset.
//   @param constraint_name - Name (JointName) of the constraint to edit (see get_physics_asset_info).
//   @param angular_drive_mode - Optional angular drive mode: SLERP|TwistAndSwing.
//   @param angular_orientation_drive - Optional: enable the angular orientation (position) drive.
//   @param angular_velocity_drive - Optional: enable the angular velocity drive.
//   @param angular_drive_spring - Optional angular drive spring/stiffness.
//   @param angular_drive_damping - Optional angular drive damping.
//   @param angular_drive_force_limit - Optional angular drive force/torque limit (0 = unlimited).
//   @param angular_orientation_target - Optional [pitch, yaw, roll] target orientation in degrees.
//   @param linear_position_drive - Optional: enable the linear position drive (applied to all 3 axes).
//   @param linear_velocity_drive - Optional: enable the linear velocity drive (all 3 axes).
//   @param linear_drive_spring - Optional linear drive spring/stiffness.
//   @param linear_drive_damping - Optional linear drive damping.
//   @param linear_drive_force_limit - Optional linear drive force limit (0 = unlimited).
//   @param linear_position_target - Optional [x, y, z] target position.
//   @param linear_breakable - Optional: the joint can break under linear force.
//   @param linear_break_threshold - Optional linear break force threshold.
//   @param angular_breakable - Optional: the joint can break under angular force.
//   @param angular_break_threshold - Optional angular break torque threshold.
//   @param projection_enabled - Optional: enable projection (post-solve position/angle fixup).
//   @param projection_linear_tolerance - Optional projection linear tolerance.
//   @param projection_angular_tolerance - Optional projection angular tolerance in degrees.
//   @param projection_linear_alpha - Optional semi-physical linear projection alpha [0..1].
//   @param projection_angular_alpha - Optional semi-physical angular projection alpha [0..1].
//   @param parent_dominates - Optional: the parent body is unaffected by the child's motion.
//   @returns { physics_asset_path: string, constraint_name: string, constraint_index: int, applied_count: int, angular_drive_spring: float, angular_drive_damping: float, angular_drive_force_limit: float, angular_orientation_target: [float], linear_drive_spring: float, linear_drive_damping: float, linear_drive_force_limit: float, linear_position_target: [float], projection_linear_alpha: float, projection_angular_alpha: float, projection_linear_tolerance: float, projection_angular_tolerance: float, saved: bool, save_error?: string, constraint: object }
TSharedPtr<FJsonObject> HandleSetPhysicsAssetConstraintDrives(const TSharedPtr<FJsonObject>& Params)
{
    UPhysicsAsset* PhysAsset = nullptr;
    FString ConstraintName;
    int32 Idx = INDEX_NONE;
    UPhysicsConstraintTemplate* CS = nullptr;
    if (TSharedPtr<FJsonObject> Error = PhysicsAsset_ReadConstraint(Params, PhysAsset, ConstraintName, Idx, CS))
    {
        return Error;
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetPhysicsAssetConstraintDrives", "Set Physics Asset Constraint Drives"));
    PhysAsset->Modify();
    CS->Modify();
    FConstraintInstance& CI = CS->DefaultInstance;

    PhysicsConstraintUtils::FDriveSettings Drives;
    if (TSharedPtr<FJsonObject> Error = PhysicsAsset_ReadDriveSettings(Params, Drives))
    {
        return Error;
    }
    const int32 Applied = PhysicsConstraintUtils::ApplyDriveSettings(CI, Drives);

    // Load-bearing: serialized back into DefaultInstance on save (same as the limits handler).
    CS->SetDefaultProfile(CI);
    PhysAsset->MarkPackageDirty();
    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(PhysAsset->GetOutermost(), PhysAsset, SaveError);

    TSharedPtr<FJsonObject> ResultObj = PhysicsAsset_StartConstraintReply(PhysAsset, ConstraintName, Idx);
    ResultObj->SetNumberField(TEXT("applied_count"), Applied);
    PhysicsConstraintUtils::AddAngularDriveFields(ResultObj, CI);
    PhysicsConstraintUtils::AddLinearDriveFields(ResultObj, CI);
    PhysicsConstraintUtils::AddProjectionFields(ResultObj, CI);
    ResultObj->SetObjectField(TEXT("constraint"), MakeConstraintObject(Idx, CS));
    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("set_physics_asset_constraint_drives"), SaveError);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_physics_asset_constraint"), &HandleAddPhysicsAssetConstraint },
    { TEXT("add_simple_collision_to_static_mesh"), &HandleAddSimpleCollisionToStaticMesh },
    { TEXT("create_physics_asset"), &HandleCreatePhysicsAsset },
    { TEXT("get_physics_asset_info"), &HandleGetPhysicsAssetInfo },
    { TEXT("remove_physics_asset_constraint"), &HandleRemovePhysicsAssetConstraint },
    { TEXT("remove_static_mesh_collision"), &HandleRemoveStaticMeshCollision },
    { TEXT("set_physics_asset_constraint_drives"), &HandleSetPhysicsAssetConstraintDrives },
    { TEXT("set_physics_asset_constraint_limits"), &HandleSetPhysicsAssetConstraintLimits },
    { TEXT("set_physics_body_advanced_properties"), &HandleSetPhysicsBodyAdvancedProperties },
    { TEXT("set_physics_body_properties"), &HandleSetPhysicsBodyProperties },
    { TEXT("set_static_mesh_convex_collision"), &HandleSetStaticMeshConvexCollision },
    { TEXT("set_static_mesh_kdop_collision"), &HandleSetStaticMeshKDopCollision },
};
} // namespace UEAgentPhysicsAssetCommands
