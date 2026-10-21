// Copyright (c) 2026 CarlxsMG.

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

    // What a UStaticMeshEditorSubsystem collision call can change, read before it runs (and before the mesh's
    // Modify): the package's dirty flag, and the body setup with its GUID, which every collision edit renews
    // (UBodySetup::InvalidatePhysicsData) and which a new body setup replaces.
    struct FMeshCollisionProbe
    {
        bool bPackageWasDirty = false;
        const UBodySetup* BodySetup = nullptr;
        FGuid BodySetupGuid;

        explicit FMeshCollisionProbe(const UStaticMesh* StaticMesh)
            : bPackageWasDirty(StaticMesh->GetOutermost()->IsDirty())
            , BodySetup(StaticMesh->GetBodySetup())
            , BodySetupGuid(BodySetup ? BodySetup->BodySetupGuid : FGuid())
        {
        }

        // Whether StaticMesh still has the same body setup, with the same GUID: then the subsystem changed no collision.
        bool CollisionUnchanged(const UStaticMesh* StaticMesh) const
        {
            const UBodySetup* Now = StaticMesh->GetBodySetup();
            return Now == BodySetup && (Now == nullptr || Now->BodySetupGuid == BodySetupGuid);
        }
    };

    // The reply of a collision call whose UStaticMeshEditorSubsystem step failed after StaticMesh->Modify(). When
    // the probe shows the collision untouched, nothing stored changed: the package's dirty flag is put back and the
    // call's undo step, which would undo nothing, is dropped. Otherwise the subsystem changed the collision before it
    // failed (the convex decomposition clears the old collision first), and the error says so: the change is left
    // unsaved and one undo restores it. Either way the payload says whether this call created the /Game copy at
    // dest_path and whether that copy was saved, as remove_static_mesh_collision reports it.
    TSharedPtr<FJsonObject> MeshCollision_FailureReply(UStaticMesh* StaticMesh, const FMeshCollisionProbe& Probe,
        bool bDuplicated, const FWritableMeshCopy& Copy, const FString& Failure)
    {
        const bool bUnchanged = Probe.CollisionUnchanged(StaticMesh);
        FString Message = Failure;
        if (bUnchanged)
        {
            StaticMesh->GetOutermost()->SetDirtyFlag(Probe.bPackageWasDirty);
            mcp_common::DiscardUndoStepOfThisCall();
            Message += TEXT("; the mesh's collision was not changed");
        }
        else
        {
            Message += TEXT("; the subsystem had already changed the mesh's collision, which is left in memory, unsaved: undo restores it");
        }

        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("static_mesh_path"), StaticMesh->GetPathName());
        Data->SetBoolField(TEXT("collision_changed"), !bUnchanged);
        Data->SetBoolField(TEXT("duplicated_to_game"), bDuplicated);
        Data->SetBoolField(TEXT("duplicated"), Copy.bCreated);
        if (Copy.bCreated)
        {
            mcp_common::AddSaveOutcome(Data, Copy.bSaved, Copy.SaveError);
            Message += Copy.bSaved
                ? FString::Printf(TEXT("; this call created and saved the /Game copy %s"), *StaticMesh->GetPathName())
                : FString::Printf(TEXT("; this call created the /Game copy %s but could not save it: %s"),
                    *StaticMesh->GetPathName(), *Copy.SaveError);
        }
        else
        {
            Data->SetBoolField(TEXT("saved"), false);
        }
        return mcp_common::MakeOutcomeReply(Data, false, Message);
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

    // Reads the optional [x,y,z] param Key into Out; bOutPresent says whether it was given. Null on success
    // (absent included), else the error reply for a value that is not an array of 3 numbers.
    TSharedPtr<FJsonObject> PhysicsAsset_ReadOptionalVector3(const TSharedPtr<FJsonObject>& Params, const TCHAR* Key,
        FVector& Out, bool& bOutPresent)
    {
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        bOutPresent = Params->TryGetArrayField(Key, Arr);
        if (!bOutPresent && !Params->HasField(Key))
        {
            return nullptr;
        }
        double X = 0.0;
        double Y = 0.0;
        double Z = 0.0;
        if (!bOutPresent || !Arr || Arr->Num() != 3
            || !(*Arr)[0]->TryGetNumber(X) || !(*Arr)[1]->TryGetNumber(Y) || !(*Arr)[2]->TryGetNumber(Z))
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("%s must be an array of 3 numbers [x,y,z]; nothing was changed"), Key));
        }
        Out = FVector(X, Y, Z);
        return nullptr;
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

// MCP:get_physics_asset_info - Read a UPhysicsAsset without changing it: body_count and constraint_count; each body's index, bone_name and primitive_count; every constraint with its name, child and parent bones, swing, twist and linear motions and limits, and its drive, breakable and projection settings; skeletal_mesh_path, the asset's preview mesh (empty when none, and not necessarily a mesh that uses the asset); and skeleton_bones, that mesh's bone names, the names body and constraint tools accept. A path that does not load as a physics asset is an error. Edit bodies with set_physics_body_properties, joints with add_physics_asset_constraint, set_physics_asset_constraint_limits or set_physics_asset_constraint_drives; a world constraint actor is get_physics_constraint_info.
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

// MCP:set_physics_body_properties - Set the physics type, a mass override and/or the advanced FBodyInstance fields (damping, mass scale, inertia scale, centre-of-mass nudge, gravity, mass update on scale) of one body of a UPhysicsAsset, chosen by body_index or else bone_name, then save the asset. Use it rather than set_properties, which needs the body setup's object path and the nested DefaultInstance struct: this picks the body by bone, goes through the engine setters for mass (which also switches the override on), mass scale and gravity, and saves. Only the fields you pass change; passing none is an error and nothing is written. An asset with no bodies, no body found, an unknown physics_type, a mass_scale below 0.001 or a vector that is not 3 numbers is an error, checked before any write. Every field is read back from the authored body (not the live physics actor, which is absent headless). success is false, with save_error, when the save fails (during PIE, for one).
//   @param physics_asset_path - /Game path to the UPhysicsAsset.
//   @param body_index - 0-based index of the body in the asset; wins over bone_name when both are given
//   @param bone_name - Bone whose body to configure, used when body_index is absent
//   @param physics_type - Body physics type (Default|Kinematic|Simulated), case-insensitive; omit to keep the current one
//   @param mass - Mass override in kilograms; also switches the mass override on. Omit to keep the current mass
//   @param linear_damping [number] - Optional. Drag that slows linear movement (FBodyInstance LinearDamping, unitless, engine default 0.01); omit to keep the current value
//   @param angular_damping [number] - Optional. Drag that slows rotation (FBodyInstance AngularDamping, unitless, engine default 0); omit to keep the current value
//   @param mass_scale [number] - Optional. Per-instance mass multiplier, >= 0.001
//   @param inertia_tensor_scale [array] - Optional. [x,y,z] inertia scale
//   @param com_nudge [array] - Optional. [x,y,z] centre-of-mass offset
//   @param enable_gravity [bool] - Optional. Whether the body is affected by gravity
//   @param update_mass_when_scale_changes [bool] - Optional. Recompute mass on scale change
//   @returns { physics_asset_path, body_index, bone_name, physics_type, physics_type_applied, mass_override_enabled, mass, mass_applied, linear_damping, angular_damping, mass_scale, inertia_tensor_scale: [x,y,z], com_nudge: [x,y,z], enable_gravity, update_mass_when_scale_changes, applied_count (fields written by this call), updated, saved, save_error }
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

    // The advanced FBodyInstance fields, each optional, read and validated here too.
    double LinearDamping = 0.0;
    const bool bLinearDamping = Params->TryGetNumberField(TEXT("linear_damping"), LinearDamping);
    double AngularDamping = 0.0;
    const bool bAngularDamping = Params->TryGetNumberField(TEXT("angular_damping"), AngularDamping);
    double MassScale = 1.0;
    const bool bMassScale = Params->TryGetNumberField(TEXT("mass_scale"), MassScale);
    if (bMassScale && MassScale < 0.001)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("mass_scale must be >= 0.001 (got %g); nothing was changed"), MassScale));
    }
    FVector InertiaTensorScale = FVector::OneVector;
    bool bInertiaTensorScale = false;
    if (TSharedPtr<FJsonObject> Invalid = PhysicsAsset_ReadOptionalVector3(Params, TEXT("inertia_tensor_scale"), InertiaTensorScale, bInertiaTensorScale))
    {
        return Invalid;
    }
    FVector ComNudge = FVector::ZeroVector;
    bool bComNudge = false;
    if (TSharedPtr<FJsonObject> Invalid = PhysicsAsset_ReadOptionalVector3(Params, TEXT("com_nudge"), ComNudge, bComNudge))
    {
        return Invalid;
    }
    bool bEnableGravity = false;
    const bool bEnableGravityGiven = Params->TryGetBoolField(TEXT("enable_gravity"), bEnableGravity);
    bool bUpdateMassOnScale = false;
    const bool bUpdateMassOnScaleGiven = Params->TryGetBoolField(TEXT("update_mass_when_scale_changes"), bUpdateMassOnScale);

    const int32 Applied = int32(bPhysicsTypeApplied) + int32(bMassApplied) + int32(bLinearDamping) + int32(bAngularDamping)
        + int32(bMassScale) + int32(bInertiaTensorScale) + int32(bComNudge) + int32(bEnableGravityGiven) + int32(bUpdateMassOnScaleGiven);
    if (Applied == 0)
    {
        return mcp_common::MakeErrorReply(
            TEXT("No properties provided (physics_type|mass|linear_damping|angular_damping|mass_scale|inertia_tensor_scale|com_nudge|enable_gravity|update_mass_when_scale_changes); nothing was changed"));
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetPhysicsBodyProperties", "Set Physics Body Properties"));
    PhysAsset->Modify();
    BodySetup->Modify();

    FBodyInstance& BI = BodySetup->DefaultInstance;
    if (bPhysicsTypeApplied)
    {
        BodySetup->PhysicsType = NewType;
    }
    if (bMassApplied)
    {
        // FBodyInstance::SetMassOverride(MassInKG, bNewOverrideMass=true)  [ENGINE_API]
        BI.SetMassOverride((float)Mass, /*bNewOverrideMass=*/true);
    }
    if (bLinearDamping)
    {
        BI.LinearDamping = static_cast<float>(LinearDamping);
    }
    if (bAngularDamping)
    {
        BI.AngularDamping = static_cast<float>(AngularDamping);
    }
    if (bMassScale)
    {
        BI.SetMassScale(static_cast<float>(MassScale));   // ENGINE_API setter
    }
    if (bInertiaTensorScale)
    {
        BI.InertiaTensorScale = InertiaTensorScale;
    }
    if (bComNudge)
    {
        BI.COMNudge = ComNudge;
    }
    if (bEnableGravityGiven)
    {
        BI.SetEnableGravity(bEnableGravity);   // ENGINE_API setter
    }
    if (bUpdateMassOnScaleGiven)
    {
        // Bitfield from FBodyInstanceCore base; direct write.
        BI.bUpdateMassWhenScaleChanges = bUpdateMassOnScale ? 1 : 0;
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
    ResultObj->SetNumberField(TEXT("mass"), BI.GetMassOverride());
    ResultObj->SetBoolField(TEXT("mass_applied"), bMassApplied);
    // READ-BACK of the advanced fields: the AUTHORED struct fields off BI. Never GetBodyMass()/
    // GetBodyInertiaTensor() -- those read the live physics actor, which is 0/absent headless.
    ResultObj->SetNumberField(TEXT("linear_damping"), BI.LinearDamping);
    ResultObj->SetNumberField(TEXT("angular_damping"), BI.AngularDamping);
    ResultObj->SetNumberField(TEXT("mass_scale"), BI.MassScale);
    ResultObj->SetArrayField(TEXT("inertia_tensor_scale"), mcp_common::VectorToJsonArray(BI.InertiaTensorScale));
    ResultObj->SetArrayField(TEXT("com_nudge"), mcp_common::VectorToJsonArray(BI.COMNudge));
    ResultObj->SetBoolField(TEXT("enable_gravity"), BI.bEnableGravity);
    ResultObj->SetBoolField(TEXT("update_mass_when_scale_changes"), BI.bUpdateMassWhenScaleChanges);
    ResultObj->SetNumberField(TEXT("applied_count"), Applied);
    ResultObj->SetBoolField(TEXT("updated"), true);

    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("set_physics_body_properties"), SaveError);
}

// MCP:add_static_mesh_simple_collision - Appends one simple collision element to a UStaticMesh and saves the mesh: a primitive (Box, Sphere or Capsule) sized to the mesh bounds, or a K-DOP hull (NDOP10_X / NDOP10_Y / NDOP10_Z bevel the four edges along that axis, NDOP18 every edge, NDOP26 every edge and corner) fitted around the LOD0 vertices as the Static Mesh Editor's Add K-DOP action does. A mesh outside /Game is first copied to dest_path under /Game, reusing a copy already there (duplicated: this call created it). An unknown shape is an error naming the accepted ones, checked before any copy. Not idempotent: each call adds one more element, existing collision is kept (clear it with remove_static_mesh_collision; convex decomposition is set_static_mesh_convex_collision). A K-DOP subsystem failure, as under PIE, makes success false with collision_changed and the new copy's saved; a failed save makes success false with save_error.
//   @param static_mesh_path - Path to the source UStaticMesh.
//   @param dest_path - Path under /Game to copy a mesh that is not under /Game into before editing (required then; an existing mesh there is reused and edited)
//   @param shape - Box (default), Sphere, Capsule, NDOP10_X, NDOP10_Y, NDOP10_Z, NDOP18 or NDOP26, case-insensitive. Any other value is refused.
//   @returns { static_mesh_path, shape, box_count, sphere_count, capsule_count, element_count, collision_added, duplicated_to_game, duplicated, added_index (K-DOP shapes), simple_collision_count (K-DOP shapes), saved, save_error?, collision_changed? }
//   @hints additive not-idempotent
TSharedPtr<FJsonObject> HandleAddSimpleCollisionToStaticMesh(const TSharedPtr<FJsonObject>& Params)
{
    // The shape (and, for a K-DOP, the subsystem) is checked before ResolveWritableStaticMesh, so a refused
    // call copies nothing into /Game.
    FString ShapeStr = TEXT("Box");
    Params->TryGetStringField(TEXT("shape"), ShapeStr);
    const bool bSphere = ShapeStr.Equals(TEXT("Sphere"), ESearchCase::IgnoreCase);
    const bool bCapsule = ShapeStr.Equals(TEXT("Capsule"), ESearchCase::IgnoreCase);
    const bool bBox = ShapeStr.Equals(TEXT("Box"), ESearchCase::IgnoreCase);
    EScriptCollisionShapeType KDopShape = EScriptCollisionShapeType::NDOP18;
    const bool bKDop = !bSphere && !bCapsule && !bBox && ParseKDopShape(ShapeStr, KDopShape);
    if (!bSphere && !bCapsule && !bBox && !bKDop)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Invalid shape '%s' (Box|Sphere|Capsule|NDOP10_X|NDOP10_Y|NDOP10_Z|NDOP18|NDOP26)"), *ShapeStr));
    }

    UStaticMeshEditorSubsystem* SMES = nullptr;
    if (bKDop)
    {
        SMES = GEditor ? GEditor->GetEditorSubsystem<UStaticMeshEditorSubsystem>() : nullptr;
        if (!SMES)
        {
            return mcp_common::MakeErrorReply(TEXT("UStaticMeshEditorSubsystem unavailable"));
        }
    }

    bool bDuplicated = false;
    TSharedPtr<FJsonObject> Error;
    FWritableMeshCopy Copy;
    UStaticMesh* StaticMesh = ResolveWritableStaticMesh(Params, bDuplicated, Error, &Copy);
    if (!StaticMesh)
    {
        return Error;
    }

    if (bKDop)
    {
        const FMeshCollisionProbe Probe(StaticMesh);
        FScopedTransaction KDopTransaction(NSLOCTEXT("UEAgent", "AddStaticMeshKDopCollision", "Add Static Mesh K-DOP Collision"));
        StaticMesh->Modify();

        // Returns the index of the newly-created collision; a negative value means it failed: under PIE, before it
        // touches the mesh, or when the mesh has neither render data nor a LOD 0 mesh description to fit the hull to,
        // after it has given a mesh without a body setup an empty one.
        const int32 AddedIndex = SMES->AddSimpleCollisionsWithNotification(StaticMesh, KDopShape, /*bApplyChanges=*/true);
        if (AddedIndex < 0)
        {
            return MeshCollision_FailureReply(StaticMesh, Probe, bDuplicated, Copy,
                FString::Printf(TEXT("K-DOP collision (%s) failed for %s (subsystem returned index %d)"),
                    *ShapeStr, *StaticMesh->GetPathName(), AddedIndex));
        }

        StaticMesh->MarkPackageDirty();
        FString KDopSaveError;
        const bool bKDopSaved = mcp_common::SaveAssetPackage(StaticMesh->GetOutermost(), StaticMesh, KDopSaveError);

        const UBodySetup* KDopBodySetup = StaticMesh->GetBodySetup();
        TSharedPtr<FJsonObject> KDopResult = MakeShared<FJsonObject>();
        KDopResult->SetStringField(TEXT("static_mesh_path"), StaticMesh->GetPathName());
        KDopResult->SetStringField(TEXT("shape"), ShapeStr.ToUpper());
        KDopResult->SetNumberField(TEXT("added_index"), AddedIndex);
        KDopResult->SetNumberField(TEXT("simple_collision_count"), SMES->GetSimpleCollisionCount(StaticMesh));
        KDopResult->SetNumberField(TEXT("box_count"), KDopBodySetup ? KDopBodySetup->AggGeom.BoxElems.Num() : 0);
        KDopResult->SetNumberField(TEXT("sphere_count"), KDopBodySetup ? KDopBodySetup->AggGeom.SphereElems.Num() : 0);
        KDopResult->SetNumberField(TEXT("capsule_count"), KDopBodySetup ? KDopBodySetup->AggGeom.SphylElems.Num() : 0);
        KDopResult->SetNumberField(TEXT("element_count"), KDopBodySetup ? KDopBodySetup->AggGeom.GetElementCount() : 0);
        KDopResult->SetBoolField(TEXT("collision_added"), true);
        KDopResult->SetBoolField(TEXT("duplicated_to_game"), bDuplicated);
        KDopResult->SetBoolField(TEXT("duplicated"), Copy.bCreated);

        // This save also writes the /Game copy, so it is the one the reply reports.
        return mcp_common::MakeSavedReply(KDopResult, bKDopSaved, TEXT("add_static_mesh_simple_collision"), KDopSaveError);
    }

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
    if (bSphere)
    {
        FKSphereElem Elem;
        Elem.Center = Center;
        Elem.Radius = FMath::Max3(Extent.X, Extent.Y, Extent.Z);
        BodySetup->AggGeom.SphereElems.Add(Elem);
        AppliedShape = TEXT("Sphere");
    }
    else if (bCapsule)
    {
        FKSphylElem Elem;
        Elem.Center = Center;
        Elem.Radius = FMath::Max(Extent.X, Extent.Y);
        // Length is the distance between the two hemisphere centers (cylinder part).
        Elem.Length = FMath::Max(0.0f, (float)(2.0 * Extent.Z - 2.0 * Elem.Radius));
        BodySetup->AggGeom.SphylElems.Add(Elem);
        AppliedShape = TEXT("Capsule");
    }
    else // Box
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
    ResultObj->SetBoolField(TEXT("duplicated"), Copy.bCreated);

    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("add_static_mesh_simple_collision"), SaveError);
}

// Static-mesh collision CRUD: clear (RemoveSimpleCollision) and convex decomposition (primitives and K-DOP hulls
// are add_static_mesh_simple_collision above).

// MCP:remove_static_mesh_collision - Clear ALL simple/convex collision primitives from a UStaticMesh's BodySetup (the 'clear' half of the collision CRUD).  Calls UBodySetup::RemoveSimpleCollision (InvalidatePhysicsData + AggGeom.EmptyElements), rebuilds the physics meshes and saves; a failed save makes success false with save_error.  Reuses the same duplicate-first logic as add_static_mesh_simple_collision so a read-only /Engine source is copied into dest_path first (an existing copy there is reused); duplicated is true when this call created that copy.  Idempotent: clearing an already-empty BodySetup (or a mesh with no BodySetup) is a no-op reported via already_empty:true, where saved reports only the save of a copy this call created and is false, with nothing saved, when there is none.
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

// MCP:set_static_mesh_convex_collision - Replace the simple collision of a UStaticMesh asset, found by path, with a V-HACD convex decomposition, as the Static Mesh Editor's Auto Convex Collision does, in one undo step, then save it; re-running replaces the hulls, never adds to them. hull_count and hull_precision must be positive, checked before anything is copied. A mesh outside /Game is first copied to dest_path under /Game, reusing a copy already there; duplicated says this call created it. When the subsystem fails (no mesh description, no LOD 0 geometry with collision enabled, a PIE session), success is false: collision_changed says whether it had already cleared the old collision, left unsaved and undoable, and saved whether a new copy was saved. A failed save makes success false with save_error. Box, sphere, capsule or K-DOP hull: add_static_mesh_simple_collision; clear: remove_static_mesh_collision.
//   @param static_mesh_path - Path to the source UStaticMesh.
//   @param dest_path - /Game path to duplicate a read-only /Engine source mesh into first (required only when the source is not under /Game).
//   @param hull_count - Maximum number of convex pieces to create (default 4, must be positive).
//   @param max_hull_verts - Maximum vertices per generated hull (default 16).
//   @param hull_precision - Voxel resolution used during decomposition (default 100000, must be positive).
//   @returns { static_mesh_path, hull_count, max_hull_verts, hull_precision, convex_count, element_count, collision_set, duplicated_to_game, duplicated, saved, save_error?, collision_changed? }
TSharedPtr<FJsonObject> HandleSetStaticMeshConvexCollision(const TSharedPtr<FJsonObject>& Params)
{
    // V-HACD parameters (sane editor defaults; positive-guarded below). They and the subsystem are checked before
    // ResolveWritableStaticMesh, so a refused call copies nothing into /Game.
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

    bool bDuplicated = false;
    TSharedPtr<FJsonObject> Error;
    FWritableMeshCopy Copy;
    UStaticMesh* StaticMesh = ResolveWritableStaticMesh(Params, bDuplicated, Error, &Copy);
    if (!StaticMesh)
    {
        return Error;
    }

    const FMeshCollisionProbe Probe(StaticMesh);
    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetStaticMeshConvexCollision", "Set Static Mesh Convex Collision"));
    StaticMesh->Modify();

    // Synchronous: the subsystem runs V-HACD (Async + ParallelFor) then WaitFor before
    // returning, and pre-sets GIsRunningUnattendedScript so no progress dialog blocks headless.
    // Returns false on an invalid mesh description, under PIE or off the game thread, before it touches the
    // mesh, and also when the LOD 0 sections with collision enabled hold fewer than 3 vertices or indices,
    // which it finds only after it has cleared the old collision.
    const bool bSet = SMES->SetConvexDecompositionCollisionsWithNotification(
        StaticMesh, HullCount, MaxHullVerts, HullPrecision, /*bApplyChanges=*/true);
    if (!bSet)
    {
        // Honest failure: do NOT report success:true.
        return MeshCollision_FailureReply(StaticMesh, Probe, bDuplicated, Copy,
            FString::Printf(TEXT("Convex decomposition failed for %s (subsystem returned false: invalid mesh "
                "description, no LOD 0 geometry with collision enabled, running under PIE, or not on the game thread)"),
                *StaticMesh->GetPathName()));
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
    ResultObj->SetBoolField(TEXT("duplicated"), Copy.bCreated);

    // This save also writes the /Game copy, so it is the one the reply reports.
    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("set_static_mesh_convex_collision"), SaveError);
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
    { TEXT("add_static_mesh_simple_collision"), &HandleAddSimpleCollisionToStaticMesh },
    { TEXT("create_physics_asset"), &HandleCreatePhysicsAsset },
    { TEXT("get_physics_asset_info"), &HandleGetPhysicsAssetInfo },
    { TEXT("remove_physics_asset_constraint"), &HandleRemovePhysicsAssetConstraint },
    { TEXT("remove_static_mesh_collision"), &HandleRemoveStaticMeshCollision },
    { TEXT("set_physics_asset_constraint_drives"), &HandleSetPhysicsAssetConstraintDrives },
    { TEXT("set_physics_asset_constraint_limits"), &HandleSetPhysicsAssetConstraintLimits },
    { TEXT("set_physics_body_properties"), &HandleSetPhysicsBodyProperties },
    { TEXT("set_static_mesh_convex_collision"), &HandleSetStaticMeshConvexCollision },
};
} // namespace UEAgentPhysicsAssetCommands
