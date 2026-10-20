#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Physics/PhysicsConstraintUtils.h"

// Editor / world
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"

// Asset creation / save
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "UObject/SavePackage.h"
#include "UObject/Package.h"
#include "Misc/PackageName.h"

// Physics
#include "PhysicalMaterials/PhysicalMaterial.h"            // UPhysicalMaterial (Friction/StaticFriction/Restitution/Density/combine modes/SurfaceType/sleep)
#include "PhysicsEngine/PhysicsConstraintActor.h"          // APhysicsConstraintActor::GetConstraintComp()
#include "PhysicsEngine/PhysicsConstraintComponent.h"      // UPhysicsConstraintComponent (Set*Limit, ConstraintActor1/2, ConstraintInstance)
#include "PhysicsEngine/ConstraintInstance.h"              // FConstraintInstance (drives/breakable getters used for read-back)
#include "PhysicsEngine/ConstraintDrives.h"                // EAngularDriveMode::{SLERP,TwistAndSwing}
#include "Components/PrimitiveComponent.h"                  // UPrimitiveComponent::SetPhysMaterialOverride
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMeshActor.h"                         // AStaticMeshActor::GetStaticMeshComponent()
#include "PhysicsEngine/BodyInstance.h"                     // FBodyInstance::GetPhysMaterialOverride()
#include "Chaos/ChaosEngineInterface.h"                     // ELinearConstraintMotion / EAngularConstraintMotion / EPhysicalSurface
#include "PhysicsSettingsEnums.h"                           // EFrictionCombineMode::Type {Average,Min,Multiply,Max}
// Chaos Vehicles (ChaosVehiclesPlugin, enabled in UEAgent.uplugin)
#include "ChaosWheeledVehicleMovementComponent.h"           // UChaosWheeledVehicleMovementComponent, FChaosWheelSetup
#include "ChaosVehicleWheel.h"                              // UChaosVehicleWheel

#define LOCTEXT_NAMESPACE "UEAgentPhysicsAuthoring"

/**
 * Physics-authoring commands.
 *
 * AUTHOR persisted /Game physical-material assets, spawn and configure physics-constraint actors in the
 * editor world (limits, drives, read-back), override an actor's physical material, and set up the wheels
 * of a Chaos wheeled vehicle. Ragdoll constraints inside a Physics Asset belong to
 * UEAgentPhysicsAssetCommands.
 *
 * Engine APIs: UPhysicalMaterial (Friction, Restitution, Density, ...); APhysicsConstraintActor::
 * GetConstraintComp(); UPhysicsConstraintComponent (SetLinearX/Y/ZLimit, SetAngularSwing1/Swing2/TwistLimit,
 * ConstraintActor1/2, ConstraintInstance, UpdateConstraintFrames); the FConstraintInstance limit and motion
 * getters for the read-back (ELinearConstraintMotion LCM_*, EAngularConstraintMotion ACM_*);
 * UPrimitiveComponent::SetPhysMaterialOverride and AStaticMeshActor::GetStaticMeshComponent().
 *
 * Modules: Engine + PhysicsCore, UnrealEd (editor world), AssetRegistry (asset registration),
 * ChaosVehicles (wheel setup).
 */
namespace UEAgentPhysicsAuthoringCommands
{

// MCP-DOMAIN: Physics

namespace
{
    // Resolve a primitive component to apply a body physical material / constraint to.
    // Prefer AStaticMeshActor::GetStaticMeshComponent(); else the first UPrimitiveComponent.
    UPrimitiveComponent* PA_GetPrimitiveComponent(AActor* Actor)
    {
        if (!Actor)
        {
            return nullptr;
        }
        if (AStaticMeshActor* SMA = Cast<AStaticMeshActor>(Actor))
        {
            if (UStaticMeshComponent* SMC = SMA->GetStaticMeshComponent())
            {
                return SMC;
            }
        }
        return Actor->FindComponentByClass<UPrimitiveComponent>();
    }

    const TCHAR* PA_FrictionCombineModeName(EFrictionCombineMode::Type Mode)
    {
        switch (Mode)
        {
            case EFrictionCombineMode::Average:  return TEXT("Average");
            case EFrictionCombineMode::Min:      return TEXT("Min");
            case EFrictionCombineMode::Multiply: return TEXT("Multiply");
            case EFrictionCombineMode::Max:      return TEXT("Max");
            default:                             return TEXT("Unknown");
        }
    }

    // Average|Min|Multiply|Max -> enum. Returns false on an unrecognized value.
    bool PA_ParseFrictionCombineMode(const FString& In, EFrictionCombineMode::Type& Out)
    {
        if (In.Equals(TEXT("Average"), ESearchCase::IgnoreCase))  { Out = EFrictionCombineMode::Average;  return true; }
        if (In.Equals(TEXT("Min"), ESearchCase::IgnoreCase))      { Out = EFrictionCombineMode::Min;      return true; }
        if (In.Equals(TEXT("Multiply"), ESearchCase::IgnoreCase)) { Out = EFrictionCombineMode::Multiply; return true; }
        if (In.Equals(TEXT("Max"), ESearchCase::IgnoreCase))      { Out = EFrictionCombineMode::Max;      return true; }
        return false;
    }

    // Resolve an EPhysicalSurface by its reflected enum-entry name (e.g. "SurfaceType_Default",
    // "SurfaceType1"). The numeric value is also accepted. Returns false if the name/value is unknown.
    bool PA_ParsePhysicalSurface(const FString& In, EPhysicalSurface& Out)
    {
        const UEnum* Enum = StaticEnum<EPhysicalSurface>();
        if (!Enum)
        {
            return false;
        }
        // Accept the bare entry name or the fully-qualified "EPhysicalSurface::Name".
        int64 Value = Enum->GetValueByNameString(In, EGetByNameFlags::CheckAuthoredName);
        if (Value == INDEX_NONE)
        {
            // Allow a raw integer index too.
            if (In.IsNumeric())
            {
                const int64 AsInt = FCString::Atoi64(*In);
                if (Enum->IsValidEnumValue(AsInt))
                {
                    Value = AsInt;
                }
            }
        }
        if (Value == INDEX_NONE)
        {
            return false;
        }
        Out = static_cast<EPhysicalSurface>(Value);
        return true;
    }

    // Reflected name of an EPhysicalSurface value (e.g. "SurfaceType_Default"), for read-back.
    FString PA_PhysicalSurfaceName(EPhysicalSurface Surface)
    {
        if (const UEnum* Enum = StaticEnum<EPhysicalSurface>())
        {
            return Enum->GetNameStringByValue(static_cast<int64>(Surface));
        }
        return FString::FromInt(static_cast<int32>(Surface));
    }
}

namespace UEAgentPhysicsAuthoringImpl
{
    /**
     * Editor-world actor lookup shared by the actor-targeted authoring commands. Returns null on
     * failure and puts in OutError the exact message the command reports: the missing editor
     * world is checked first, then the actor name (label or internal name).
     */
    AActor* FindEditorActor(const FString& ActorName, FString& OutError)
    {
        UWorld* EditorWorld = mcp_common::GetEditorWorld();
        if (EditorWorld == nullptr)
        {
            OutError = TEXT("Failed to get editor world");
            return nullptr;
        }
        AActor* Found = mcp_common::FindActorByName(EditorWorld, ActorName);
        if (Found == nullptr)
        {
            OutError = FString::Printf(TEXT("Actor not found: %s"), *ActorName);
        }
        return Found;
    }

    /**
     * Reads the required constraint_actor_name and finds that APhysicsConstraintActor in the editor world, with its
     * constraint component. Null on success, else the error reply; the param is checked first, then the editor world,
     * the actor and its component.
     */
    TSharedPtr<FJsonObject> ReadConstraintActor(const TSharedPtr<FJsonObject>& Params, APhysicsConstraintActor*& OutActor,
        UPhysicsConstraintComponent*& OutComponent)
    {
        FString ActorName;
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("constraint_actor_name"), ActorName)) { return Missing; }

        UWorld* World = nullptr;
        if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World, TEXT("Failed to get editor world"))) { return NoWorld; }

        OutActor = Cast<APhysicsConstraintActor>(mcp_common::FindActorByName(World, ActorName));
        if (!OutActor)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("PhysicsConstraintActor not found: %s"), *ActorName));
        }

        OutComponent = OutActor->GetConstraintComp();
        if (!OutComponent)
        {
            return mcp_common::MakeErrorReply(TEXT("Constraint actor has no constraint component"));
        }
        return nullptr;
    }

    /**
     * Reads the optional drive, breakable and projection params of set_physics_constraint_drives. Null on success, else
     * the error reply for an angular_drive_mode that is neither SLERP nor TwistAndSwing.
     */
    TSharedPtr<FJsonObject> ReadDriveSettings(const TSharedPtr<FJsonObject>& Params, PhysicsConstraintUtils::FDriveSettings& Out)
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

    /** A reply that opens with constraint_actor_name (the actor's label) and constraint_component. */
    TSharedPtr<FJsonObject> StartConstraintReply(APhysicsConstraintActor* ConstraintActor, UPhysicsConstraintComponent* ConstraintComp)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("constraint_actor_name"), ConstraintActor->GetActorLabel());
        Result->SetStringField(TEXT("constraint_component"), ConstraintComp->GetName());
        return Result;
    }

    /** constraint_actor1 and constraint_actor2, the labels of the constrained actors, each only when it is set. */
    void AddConstrainedActorFields(const TSharedPtr<FJsonObject>& Out, UPhysicsConstraintComponent* ConstraintComp)
    {
        if (ConstraintComp->ConstraintActor1)
        {
            Out->SetStringField(TEXT("constraint_actor1"), ConstraintComp->ConstraintActor1->GetActorLabel());
        }
        if (ConstraintComp->ConstraintActor2)
        {
            Out->SetStringField(TEXT("constraint_actor2"), ConstraintComp->ConstraintActor2->GetActorLabel());
        }
    }

    /** linear_limit and the three linear motions, then the swing1, swing2 and twist limits, each with its motion. */
    void AddLimitFields(const TSharedPtr<FJsonObject>& Out, const FConstraintInstance& CI)
    {
        Out->SetNumberField(TEXT("linear_limit"), CI.GetLinearLimit());
        Out->SetStringField(TEXT("linear_x_motion"), PhysicsConstraintUtils::LinearMotionName(CI.GetLinearXMotion()));
        Out->SetStringField(TEXT("linear_y_motion"), PhysicsConstraintUtils::LinearMotionName(CI.GetLinearYMotion()));
        Out->SetStringField(TEXT("linear_z_motion"), PhysicsConstraintUtils::LinearMotionName(CI.GetLinearZMotion()));
        Out->SetNumberField(TEXT("swing1_limit"), CI.GetAngularSwing1Limit());
        Out->SetStringField(TEXT("swing1_motion"), PhysicsConstraintUtils::AngularMotionName(CI.GetAngularSwing1Motion()));
        Out->SetNumberField(TEXT("swing2_limit"), CI.GetAngularSwing2Limit());
        Out->SetStringField(TEXT("swing2_motion"), PhysicsConstraintUtils::AngularMotionName(CI.GetAngularSwing2Motion()));
        Out->SetNumberField(TEXT("twist_limit"), CI.GetAngularTwistLimit());
        Out->SetStringField(TEXT("twist_motion"), PhysicsConstraintUtils::AngularMotionName(CI.GetAngularTwistMotion()));
    }

    /**
     * The drive, breakable, projection, dominance and collision fields that set_physics_constraint_drives and
     * get_physics_constraint_info report (non-const CI: several drive getters are non-const).
     */
    void AddDriveFields(const TSharedPtr<FJsonObject>& Out, FConstraintInstance& CI)
    {
        Out->SetStringField(TEXT("angular_drive_mode"), PhysicsConstraintUtils::AngularDriveModeName(CI.GetAngularDriveMode()));
        Out->SetBoolField(TEXT("angular_orientation_drive"), CI.IsAngularOrientationDriveEnabled());
        Out->SetBoolField(TEXT("angular_velocity_drive"), CI.IsAngularVelocityDriveEnabled());
        PhysicsConstraintUtils::AddAngularDriveFields(Out, CI);
        Out->SetBoolField(TEXT("linear_position_drive"), CI.IsLinearPositionDriveEnabled());
        Out->SetBoolField(TEXT("linear_velocity_drive"), CI.IsLinearVelocityDriveEnabled());
        PhysicsConstraintUtils::AddLinearDriveFields(Out, CI);
        Out->SetBoolField(TEXT("linear_breakable"), CI.IsLinearBreakable());
        Out->SetNumberField(TEXT("linear_break_threshold"), CI.GetLinearBreakThreshold());
        Out->SetBoolField(TEXT("angular_breakable"), CI.IsAngularBreakable());
        Out->SetNumberField(TEXT("angular_break_threshold"), CI.GetAngularBreakThreshold());
        Out->SetBoolField(TEXT("projection_enabled"), CI.IsProjectionEnabled());
        PhysicsConstraintUtils::AddProjectionFields(Out, CI);
        Out->SetBoolField(TEXT("parent_dominates"), CI.IsParentDominatesEnabled());
        Out->SetBoolField(TEXT("disable_collision"), CI.IsCollisionDisabled());
    }
}

// --------------------------------------------------------------------------- //
// create_physical_material
// --------------------------------------------------------------------------- //
// MCP:create_physical_material - Create and save a UPhysicalMaterial asset, optionally setting surface/object properties.  Idempotent: if the asset already exists it is loaded, any provided values are re-applied, and a stable payload is returned.
//   @param name - Asset name (e.g. 'PM_Rubber').
//   @param package_path - Target /Game package path (optional, default "/Game/Physics")
//   @param friction - Optional Friction value to set on creation
//   @param restitution - Optional Restitution value to set on creation
//   @param density - Optional Density value (g/cm^3) to set on creation
//   @returns { name: string, asset_path: string, package_path: string, friction: number, restitution: number, density: number, already_existed: bool, created: bool }

TSharedPtr<FJsonObject> HandleCreatePhysicalMaterial(const TSharedPtr<FJsonObject>& Params)
{
    FString Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name)) { return Missing; }
    FString PackagePath = TEXT("/Game/Physics");
    Params->TryGetStringField(TEXT("package_path"), PackagePath);
    PackagePath = mcp_common::NormalizeFolder(PackagePath, TEXT("/Game/Physics"));
    const FString FullPath = FString::Printf(TEXT("%s/%s"), *PackagePath, *Name);
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(FullPath)) { return Invalid; }

    UPhysicalMaterial* PhysMat = nullptr;
    UPackage* Package = nullptr;
    bool bAlreadyExisted = false;

    if (UEditorAssetLibrary::DoesAssetExist(FullPath))
    {
        // Idempotent reload — return the existing asset (re-applying any provided values below).
        PhysMat = LoadObject<UPhysicalMaterial>(nullptr, *FullPath);
        if (!PhysMat)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Asset exists but failed to load as PhysicalMaterial: %s"), *FullPath));
        }
        Package = PhysMat->GetPackage();
        bAlreadyExisted = true;
    }
    else
    {
        if (const FString AssetConflict = mcp_common::AssetNameConflict(*FullPath, FName(*Name), UPhysicalMaterial::StaticClass()); !AssetConflict.IsEmpty())
        {
            return mcp_common::MakeErrorReply(AssetConflict);
        }
        Package = CreatePackage(*FullPath);
        if (!Package)
        {
            return mcp_common::MakeErrorReply(TEXT("Failed to create package"));
        }
        PhysMat = NewObject<UPhysicalMaterial>(
            Package, UPhysicalMaterial::StaticClass(), FName(*Name),
            RF_Public | RF_Standalone | RF_Transactional);
        if (!PhysMat)
        {
            return mcp_common::MakeErrorReply(TEXT("Failed to create UPhysicalMaterial"));
        }
        FAssetRegistryModule::AssetCreated(PhysMat);
    }

    // Apply provided surface/object properties.
    double Friction = 0.0, Restitution = 0.0, Density = 0.0;
    const bool bHasFriction    = Params->TryGetNumberField(TEXT("friction"), Friction);
    const bool bHasRestitution = Params->TryGetNumberField(TEXT("restitution"), Restitution);
    const bool bHasDensity     = Params->TryGetNumberField(TEXT("density"), Density);

    if (bHasFriction || bHasRestitution || bHasDensity)
    {
        const FScopedTransaction Transaction(LOCTEXT("CreatePhysMat", "MCP: Create Physical Material"));
        PhysMat->Modify();
        if (bHasFriction)    { PhysMat->Friction    = static_cast<float>(Friction); }
        if (bHasRestitution) { PhysMat->Restitution = static_cast<float>(Restitution); }
        if (bHasDensity)     { PhysMat->Density     = static_cast<float>(Density); }
    }

    FString SaveError;
    if (!mcp_common::SaveAssetPackage(Package, PhysMat, SaveError))
    {
        return mcp_common::MakeErrorReply(SaveError);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("name"), Name);
    Result->SetStringField(TEXT("asset_path"), FullPath);
    Result->SetStringField(TEXT("package_path"), PackagePath);
    // Read back the live values from the asset.
    Result->SetNumberField(TEXT("friction"), PhysMat->Friction);
    Result->SetNumberField(TEXT("restitution"), PhysMat->Restitution);
    Result->SetNumberField(TEXT("density"), PhysMat->Density);
    Result->SetBoolField(TEXT("already_existed"), bAlreadyExisted);
    Result->SetBoolField(TEXT("created"), true);
    return mcp_common::MakeOkReply(Result);
}

// --------------------------------------------------------------------------- //
// set_physical_material_properties
// --------------------------------------------------------------------------- //
// MCP:set_physical_material_properties - Set friction, restitution, density, combine modes, surface type, mass power and sleep thresholds of a PhysicalMaterial asset as one undo step, then save it. Over set_properties: it clamps to the engine ranges, turns combine-mode overrides on, and saves; like a Details-panel edit it runs PostEditChange, so bodies using the material in this session see the new values. Bad enum names, or a call with no field, fail before writing. Values the material already has give changed=false and saved=false; a failed save makes success false, with saved=false and save_error, and still returns the values read back.
//   @param physical_material_path - /Game path to the physical material asset.
//   @param friction - Optional new (kinetic) Friction value (>= 0)
//   @param static_friction - Optional new StaticFriction value (>= 0)
//   @param restitution - Optional new Restitution value (clamped to [0,1])
//   @param density - Optional new Density value (g/cm^3, >= 0)
//   @param friction_combine_mode - Optional Average|Min|Multiply|Max (sets bOverrideFrictionCombineMode true)
//   @param restitution_combine_mode - Optional Average|Min|Multiply|Max (sets bOverrideRestitutionCombineMode true)
//   @param surface_type - Optional EPhysicalSurface by reflected name (e.g. 'SurfaceType_Default','SurfaceType1') or numeric value
//   @param raise_mass_to_power - Optional RaiseMassToPower (clamped to [0.1,1])
//   @param sleep_linear_velocity_threshold - Optional SleepLinearVelocityThreshold (>= 0)
//   @param sleep_angular_velocity_threshold - Optional SleepAngularVelocityThreshold (>= 0)
//   @returns { physical_material_path: string, friction: number, static_friction: number, restitution: number, density: number, friction_combine_mode: string, override_friction_combine_mode: bool, restitution_combine_mode: string, override_restitution_combine_mode: bool, surface_type: string, raise_mass_to_power: number, sleep_linear_velocity_threshold: number, sleep_angular_velocity_threshold: number, friction_applied: bool, static_friction_applied: bool, restitution_applied: bool, density_applied: bool, friction_combine_mode_applied: bool, restitution_combine_mode_applied: bool, surface_type_applied: bool, raise_mass_to_power_applied: bool, sleep_linear_velocity_threshold_applied: bool, sleep_angular_velocity_threshold_applied: bool, updated: bool, changed: bool, saved: bool, save_error?: string, note?: string }

TSharedPtr<FJsonObject> HandleSetPhysicalMaterialProperties(const TSharedPtr<FJsonObject>& Params)
{
    FString MatPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("physical_material_path"), MatPath)) { return Missing; }

    UPhysicalMaterial* PhysMat = LoadObject<UPhysicalMaterial>(nullptr, *MatPath);
    if (!PhysMat)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("PhysicalMaterial not found: %s"), *MatPath));
    }

    double Friction = 0.0, StaticFriction = 0.0, Restitution = 0.0, Density = 0.0;
    double RaiseMass = 0.0, SleepLin = 0.0, SleepAng = 0.0;
    const bool bHasFriction       = Params->TryGetNumberField(TEXT("friction"), Friction);
    const bool bHasStaticFriction = Params->TryGetNumberField(TEXT("static_friction"), StaticFriction);
    const bool bHasRestitution    = Params->TryGetNumberField(TEXT("restitution"), Restitution);
    const bool bHasDensity        = Params->TryGetNumberField(TEXT("density"), Density);
    const bool bHasRaiseMass      = Params->TryGetNumberField(TEXT("raise_mass_to_power"), RaiseMass);
    const bool bHasSleepLin       = Params->TryGetNumberField(TEXT("sleep_linear_velocity_threshold"), SleepLin);
    const bool bHasSleepAng       = Params->TryGetNumberField(TEXT("sleep_angular_velocity_threshold"), SleepAng);

    // Combine modes (string enums) — applying one also asserts its override flag so the value
    // actually takes effect instead of deferring to project settings.
    FString FrictionModeStr, RestitutionModeStr;
    EFrictionCombineMode::Type FrictionMode = EFrictionCombineMode::Average;
    EFrictionCombineMode::Type RestitutionMode = EFrictionCombineMode::Average;
    const bool bHasFrictionMode    = Params->TryGetStringField(TEXT("friction_combine_mode"), FrictionModeStr) && !FrictionModeStr.IsEmpty();
    const bool bHasRestitutionMode = Params->TryGetStringField(TEXT("restitution_combine_mode"), RestitutionModeStr) && !RestitutionModeStr.IsEmpty();
    if (bHasFrictionMode && !PA_ParseFrictionCombineMode(FrictionModeStr, FrictionMode))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Invalid friction_combine_mode '%s' (Average|Min|Multiply|Max)"), *FrictionModeStr));
    }
    if (bHasRestitutionMode && !PA_ParseFrictionCombineMode(RestitutionModeStr, RestitutionMode))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Invalid restitution_combine_mode '%s' (Average|Min|Multiply|Max)"), *RestitutionModeStr));
    }

    // Surface type (string enum by reflected name, or numeric).
    FString SurfaceStr;
    EPhysicalSurface Surface = SurfaceType_Default;
    const bool bHasSurface = Params->TryGetStringField(TEXT("surface_type"), SurfaceStr) && !SurfaceStr.IsEmpty();
    if (bHasSurface && !PA_ParsePhysicalSurface(SurfaceStr, Surface))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Invalid surface_type '%s' (use a reflected EPhysicalSurface name e.g. 'SurfaceType_Default' or a numeric value)"), *SurfaceStr));
    }

    const bool bAnyField = bHasFriction || bHasStaticFriction || bHasRestitution || bHasDensity || bHasRaiseMass
        || bHasSleepLin || bHasSleepAng || bHasFrictionMode || bHasRestitutionMode || bHasSurface;
    if (!bAnyField)
    {
        return mcp_common::MakeErrorReply(TEXT("Pass at least one field to set (friction, static_friction, restitution, density, ")
            TEXT("friction_combine_mode, restitution_combine_mode, surface_type, raise_mass_to_power, ")
            TEXT("sleep_linear_velocity_threshold, sleep_angular_velocity_threshold); nothing was changed."));
    }

    // The authored state before the write, to report whether anything changed.
    const auto Snapshot = [PhysMat]()
    {
        return FString::Printf(TEXT("%g|%g|%g|%g|%g|%g|%g|%d|%d|%d|%d|%d"), PhysMat->Friction, PhysMat->StaticFriction,
            PhysMat->Restitution, PhysMat->Density, PhysMat->RaiseMassToPower, PhysMat->SleepLinearVelocityThreshold,
            PhysMat->SleepAngularVelocityThreshold, (int32)PhysMat->bOverrideFrictionCombineMode, (int32)PhysMat->FrictionCombineMode,
            (int32)PhysMat->bOverrideRestitutionCombineMode, (int32)PhysMat->RestitutionCombineMode, (int32)PhysMat->SurfaceType);
    };
    const FString Before = Snapshot();

    {
        const FScopedTransaction Transaction(LOCTEXT("SetPhysMatProps", "MCP: Set Physical Material Properties"));
        PhysMat->Modify();
        // Clamp to the engine's own UPROPERTY meta ranges so authored values are physically valid.
        if (bHasFriction)       { PhysMat->Friction       = FMath::Max(0.0f, static_cast<float>(Friction)); }                 // ClampMin=0
        if (bHasStaticFriction) { PhysMat->StaticFriction = FMath::Max(0.0f, static_cast<float>(StaticFriction)); }           // ClampMin=0
        if (bHasRestitution)    { PhysMat->Restitution    = FMath::Clamp(static_cast<float>(Restitution), 0.0f, 1.0f); }      // [0,1]
        if (bHasDensity)        { PhysMat->Density        = FMath::Max(0.0f, static_cast<float>(Density)); }                  // ClampMin=0
        if (bHasRaiseMass)      { PhysMat->RaiseMassToPower = FMath::Clamp(static_cast<float>(RaiseMass), 0.1f, 1.0f); }       // [0.1,1]
        if (bHasSleepLin)       { PhysMat->SleepLinearVelocityThreshold  = FMath::Max(0.0f, static_cast<float>(SleepLin)); }  // ClampMin=0
        if (bHasSleepAng)       { PhysMat->SleepAngularVelocityThreshold = FMath::Max(0.0f, static_cast<float>(SleepAng)); }  // ClampMin=0
        if (bHasFrictionMode)
        {
            PhysMat->bOverrideFrictionCombineMode = true;   // editcondition gate — must be set for the mode to apply
            PhysMat->FrictionCombineMode = FrictionMode;
        }
        if (bHasRestitutionMode)
        {
            PhysMat->bOverrideRestitutionCombineMode = true; // editcondition gate
            PhysMat->RestitutionCombineMode = RestitutionMode;
        }
        if (bHasSurface) { PhysMat->SurfaceType = Surface; }
        // As a Details-panel edit would: UPhysicalMaterial::PostEditChangeProperty pushes the new values into the
        // physics engine's material, so bodies already using it in this session see them.
        PhysMat->PostEditChange();
    }

    // Values the material already had: nothing to save.
    const bool bChanged = Snapshot() != Before;
    FString SaveError;
    const bool bSaved = bChanged && mcp_common::SaveAssetPackage(PhysMat->GetPackage(), PhysMat, SaveError);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("physical_material_path"), MatPath);
    // Read back live values to confirm the writes landed.
    Result->SetNumberField(TEXT("friction"), PhysMat->Friction);
    Result->SetNumberField(TEXT("static_friction"), PhysMat->StaticFriction);
    Result->SetNumberField(TEXT("restitution"), PhysMat->Restitution);
    Result->SetNumberField(TEXT("density"), PhysMat->Density);
    Result->SetStringField(TEXT("friction_combine_mode"), PA_FrictionCombineModeName(PhysMat->FrictionCombineMode));
    Result->SetBoolField(TEXT("override_friction_combine_mode"), PhysMat->bOverrideFrictionCombineMode);
    Result->SetStringField(TEXT("restitution_combine_mode"), PA_FrictionCombineModeName(PhysMat->RestitutionCombineMode));
    Result->SetBoolField(TEXT("override_restitution_combine_mode"), PhysMat->bOverrideRestitutionCombineMode);
    Result->SetStringField(TEXT("surface_type"), PA_PhysicalSurfaceName(PhysMat->SurfaceType));
    Result->SetNumberField(TEXT("raise_mass_to_power"), PhysMat->RaiseMassToPower);
    Result->SetNumberField(TEXT("sleep_linear_velocity_threshold"), PhysMat->SleepLinearVelocityThreshold);
    Result->SetNumberField(TEXT("sleep_angular_velocity_threshold"), PhysMat->SleepAngularVelocityThreshold);
    Result->SetBoolField(TEXT("friction_applied"), bHasFriction);
    Result->SetBoolField(TEXT("static_friction_applied"), bHasStaticFriction);
    Result->SetBoolField(TEXT("restitution_applied"), bHasRestitution);
    Result->SetBoolField(TEXT("density_applied"), bHasDensity);
    Result->SetBoolField(TEXT("friction_combine_mode_applied"), bHasFrictionMode);
    Result->SetBoolField(TEXT("restitution_combine_mode_applied"), bHasRestitutionMode);
    Result->SetBoolField(TEXT("surface_type_applied"), bHasSurface);
    Result->SetBoolField(TEXT("raise_mass_to_power_applied"), bHasRaiseMass);
    Result->SetBoolField(TEXT("sleep_linear_velocity_threshold_applied"), bHasSleepLin);
    Result->SetBoolField(TEXT("sleep_angular_velocity_threshold_applied"), bHasSleepAng);
    Result->SetBoolField(TEXT("updated"), bChanged);
    Result->SetBoolField(TEXT("changed"), bChanged);
    if (!bChanged)
    {
        Result->SetBoolField(TEXT("saved"), false);
        Result->SetStringField(TEXT("note"), TEXT("The material already had these values; nothing was saved."));
        return mcp_common::MakeOkReply(Result);
    }
    return mcp_common::MakeSavedReply(Result, bSaved, TEXT("set_physical_material_properties"), SaveError);
}

// --------------------------------------------------------------------------- //
// spawn_physics_constraint
// --------------------------------------------------------------------------- //
// MCP:spawn_physics_constraint - Spawns an APhysicsConstraintActor in the editor world at location (zero rotation) in an undo transaction and optionally sets the two actors it constrains, found by object name or label (first match), then recomputes the joint frames; spawn_actor has no constraint type. A name that matches no actor is skipped, not an error: check actor1_wired and actor2_wired. Each call adds a new actor; the level is not saved. Tune it with set_physics_constraint_limits and set_physics_constraint_drives, read it with get_physics_constraint_info; for joints inside a Physics Asset use add_physics_asset_constraint.
//   @param location - World [x, y, z] for the constraint actor (defaults to origin)
//   @param actor1_name - Optional name/label of the first actor to constrain
//   @param actor2_name - Optional name/label of the second actor to constrain
//   @returns { actor_name: string, actor_internal_name: string, actor_class: string, constraint_component: string, actor1_wired: bool, actor2_wired: bool, constraint_actor1?: string, constraint_actor2?: string, spawned: bool }

TSharedPtr<FJsonObject> HandleSpawnPhysicsConstraint(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World, TEXT("Failed to get editor world"))) { return NoWorld; }

    const FVector Location = mcp_common::ReadJsonVector(Params, TEXT("location"));

    const FScopedTransaction Transaction(LOCTEXT("SpawnConstraint", "MCP: Spawn Physics Constraint"));
    APhysicsConstraintActor* ConstraintActor = World->SpawnActor<APhysicsConstraintActor>(Location, FRotator::ZeroRotator);
    if (!ConstraintActor)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to spawn PhysicsConstraintActor"));
    }

    UPhysicsConstraintComponent* ConstraintComp = ConstraintActor->GetConstraintComp();
    if (!ConstraintComp)
    {
        return mcp_common::MakeErrorReply(TEXT("Spawned constraint actor has no constraint component"));
    }

    // Optionally wire the two constrained actors by label/name.
    FString Actor1Name, Actor2Name;
    bool bWired1 = false, bWired2 = false;
    if (Params->TryGetStringField(TEXT("actor1_name"), Actor1Name) && !Actor1Name.IsEmpty())
    {
        if (AActor* A1 = mcp_common::FindActorByName(World, Actor1Name))
        {
            ConstraintComp->Modify();
            ConstraintComp->ConstraintActor1 = A1;
            bWired1 = true;
        }
    }
    if (Params->TryGetStringField(TEXT("actor2_name"), Actor2Name) && !Actor2Name.IsEmpty())
    {
        if (AActor* A2 = mcp_common::FindActorByName(World, Actor2Name))
        {
            ConstraintComp->Modify();
            ConstraintComp->ConstraintActor2 = A2;
            bWired2 = true;
        }
    }

    if (bWired1 || bWired2)
    {
        // Re-evaluate the joint reference frames from the connected bodies.
        ConstraintComp->UpdateConstraintFrames();
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actor_name"), ConstraintActor->GetActorLabel());
    Result->SetStringField(TEXT("actor_internal_name"), ConstraintActor->GetName());
    Result->SetStringField(TEXT("actor_class"), ConstraintActor->GetClass()->GetName());
    Result->SetStringField(TEXT("constraint_component"), ConstraintComp->GetName());
    Result->SetBoolField(TEXT("actor1_wired"), bWired1);
    Result->SetBoolField(TEXT("actor2_wired"), bWired2);
    UEAgentPhysicsAuthoringImpl::AddConstrainedActorFields(Result, ConstraintComp);
    Result->SetBoolField(TEXT("spawned"), true);
    return mcp_common::MakeOkReply(Result);
}

// --------------------------------------------------------------------------- //
// set_physics_constraint_limits
// --------------------------------------------------------------------------- //
// MCP:set_physics_constraint_limits - Set linear/angular limits on a physics-constraint actor's UPhysicsConstraintComponent and read back.  Providing a limit sets that axis to a Limited motion type with the given size (linear is shared across X/Y/Z; swing/twist are in degrees).
//   @param constraint_actor_name - Label/name of the APhysicsConstraintActor.
//   @param linear_limit - Optional shared linear limit applied to all 3 axes (cm)
//   @param swing1_limit - Optional swing1 angular limit (degrees)
//   @param swing2_limit - Optional swing2 angular limit (degrees)
//   @param twist_limit - Optional twist angular limit (degrees)
//   @returns { constraint_actor_name: string, constraint_component: string, linear_limit: number, linear_x_motion: string, linear_y_motion: string, linear_z_motion: string, swing1_limit: number, swing1_motion: string, swing2_limit: number, swing2_motion: string, twist_limit: number, twist_motion: string, linear_applied: bool, swing1_applied: bool, swing2_applied: bool, twist_applied: bool, updated: bool }

TSharedPtr<FJsonObject> HandleSetConstraintLimits(const TSharedPtr<FJsonObject>& Params)
{
    APhysicsConstraintActor* ConstraintActor = nullptr;
    UPhysicsConstraintComponent* ConstraintComp = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentPhysicsAuthoringImpl::ReadConstraintActor(Params, ConstraintActor, ConstraintComp))
    {
        return Error;
    }

    double LinearLimit = 0.0, Swing1 = 0.0, Swing2 = 0.0, Twist = 0.0;
    const bool bHasLinear = Params->TryGetNumberField(TEXT("linear_limit"), LinearLimit);
    const bool bHasSwing1 = Params->TryGetNumberField(TEXT("swing1_limit"), Swing1);
    const bool bHasSwing2 = Params->TryGetNumberField(TEXT("swing2_limit"), Swing2);
    const bool bHasTwist  = Params->TryGetNumberField(TEXT("twist_limit"), Twist);

    {
        const FScopedTransaction Transaction(LOCTEXT("SetConstraintLimits", "MCP: Set Constraint Limits"));
        ConstraintComp->Modify();
        // A finite limit implies a Limited motion type on that axis; the component
        // setters take (MotionType, LimitSize) and push through ConstraintInstance.
        if (bHasLinear)
        {
            // Linear limit is shared across X/Y/Z in FConstraintInstance — set all three.
            ConstraintComp->SetLinearXLimit(LCM_Limited, static_cast<float>(LinearLimit));
            ConstraintComp->SetLinearYLimit(LCM_Limited, static_cast<float>(LinearLimit));
            ConstraintComp->SetLinearZLimit(LCM_Limited, static_cast<float>(LinearLimit));
        }
        if (bHasSwing1) { ConstraintComp->SetAngularSwing1Limit(ACM_Limited, static_cast<float>(Swing1)); }
        if (bHasSwing2) { ConstraintComp->SetAngularSwing2Limit(ACM_Limited, static_cast<float>(Swing2)); }
        if (bHasTwist)  { ConstraintComp->SetAngularTwistLimit(ACM_Limited, static_cast<float>(Twist)); }
    }

    // Read back from the live ConstraintInstance.
    TSharedPtr<FJsonObject> Result = UEAgentPhysicsAuthoringImpl::StartConstraintReply(ConstraintActor, ConstraintComp);
    UEAgentPhysicsAuthoringImpl::AddLimitFields(Result, ConstraintComp->ConstraintInstance);

    Result->SetBoolField(TEXT("linear_applied"), bHasLinear);
    Result->SetBoolField(TEXT("swing1_applied"), bHasSwing1);
    Result->SetBoolField(TEXT("swing2_applied"), bHasSwing2);
    Result->SetBoolField(TEXT("twist_applied"), bHasTwist);
    Result->SetBoolField(TEXT("updated"), true);
    return mcp_common::MakeOkReply(Result);
}

// --------------------------------------------------------------------------- //
// set_physics_constraint_drives
// --------------------------------------------------------------------------- //
// MCP:set_physics_constraint_drives - Set linear/angular drives (motors), breakable thresholds, projection, and collision/dominance on a world APhysicsConstraintActor's UPhysicsConstraintComponent — the "behavior beyond limits" (read-modify-write; only provided fields change; idempotent set-to-fixed-value).  Authoring-only: the values are written into the component's serialized ConstraintInstance template and persist; the live motor torque is only applied while simulating (PIE).  Distinct from the UPhysicsAsset constraint drives (set_physics_asset_constraint_drives), which author a ragdoll joint inside an asset.
//   @param constraint_actor_name - Label/name of the APhysicsConstraintActor (as spawned by spawn_physics_constraint).
//   @param angular_drive_mode - Optional angular drive mode: SLERP|TwistAndSwing (dispatches the orientation/velocity enable path).
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
//   @param disable_collision - Optional: disable collision between the two constrained bodies.
//   @param break_constraint - Optional: if true, immediately break the (live) constraint via BreakConstraint().
//   @returns { constraint_actor_name: string, constraint_component: string, applied_count: int, angular_drive_mode: string, angular_orientation_drive: bool, angular_velocity_drive: bool, angular_drive_spring: number, angular_drive_damping: number, angular_drive_force_limit: number, angular_orientation_target: [number], linear_position_drive: bool, linear_velocity_drive: bool, linear_drive_spring: number, linear_drive_damping: number, linear_drive_force_limit: number, linear_position_target: [number], linear_breakable: bool, linear_break_threshold: number, angular_breakable: bool, angular_break_threshold: number, projection_enabled: bool, projection_linear_alpha: number, projection_angular_alpha: number, projection_linear_tolerance: number, projection_angular_tolerance: number, parent_dominates: bool, disable_collision: bool, constraint_broken: bool, note: string, updated: bool }

TSharedPtr<FJsonObject> HandleSetPhysicsConstraintDrives(const TSharedPtr<FJsonObject>& Params)
{
    APhysicsConstraintActor* ConstraintActor = nullptr;
    UPhysicsConstraintComponent* ConstraintComp = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentPhysicsAuthoringImpl::ReadConstraintActor(Params, ConstraintActor, ConstraintComp))
    {
        return Error;
    }

    // The settings are read, and a bad angular_drive_mode refused, before anything is mutated.
    PhysicsConstraintUtils::FDriveSettings Drives;
    if (TSharedPtr<FJsonObject> Error = UEAgentPhysicsAuthoringImpl::ReadDriveSettings(Params, Drives))
    {
        return Error;
    }

    int32 Applied = 0;
    bool bBrokeConstraint = false;

    {
        const FScopedTransaction Transaction(LOCTEXT("SetConstraintDrives", "MCP: Set Physics Constraint Drives"));
        ConstraintComp->Modify();

        // The component setters delegate into this same instance, so it is written directly (authoring the
        // serialized template), the way set_physics_constraint_limits writes the limits.
        FConstraintInstance& CI = ConstraintComp->ConstraintInstance;
        Applied = PhysicsConstraintUtils::ApplyDriveSettings(CI, Drives);

        bool Flag = false;
        if (Params->TryGetBoolField(TEXT("disable_collision"), Flag))
        {
            CI.SetDisableCollision(Flag);
            ++Applied;
        }

        // An explicit break of the live constraint (a component-level operation).
        if (Params->TryGetBoolField(TEXT("break_constraint"), Flag) && Flag)
        {
            ConstraintComp->BreakConstraint();
            bBrokeConstraint = true;
            ++Applied;
        }
    }

    TSharedPtr<FJsonObject> Result = UEAgentPhysicsAuthoringImpl::StartConstraintReply(ConstraintActor, ConstraintComp);
    Result->SetNumberField(TEXT("applied_count"), Applied);
    UEAgentPhysicsAuthoringImpl::AddDriveFields(Result, ConstraintComp->ConstraintInstance);
    Result->SetBoolField(TEXT("constraint_broken"), bBrokeConstraint);
    // HONEST: drives author the serialized template and persist headless; the actual motor torque
    // is only produced while the physics sim runs (PIE). break_constraint also only affects a live joint.
    Result->SetStringField(TEXT("note"),
        TEXT("Drive/breakable/projection values are authored into the constraint template and persist headless; "
             "the live motor torque (and an effective BreakConstraint) only occur while simulating (PIE)."));
    Result->SetBoolField(TEXT("updated"), true);
    return mcp_common::MakeOkReply(Result);
}

// --------------------------------------------------------------------------- //
// set_actor_physical_material
// --------------------------------------------------------------------------- //
// MCP:set_actor_physical_material - Set the body physical-material override on an actor's primitive (static-mesh) component.  Calls UPrimitiveComponent::SetPhysMaterialOverride and reads the override back.
//   @param actor_name - Label/name of the target actor.
//   @param physical_material_path - /Game path to the UPhysicalMaterial asset.
//   @returns { actor_name: string, component_name: string, physical_material_path: string, physical_material_name: string, applied_material_name?: string, verified: bool, applied: bool }

TSharedPtr<FJsonObject> HandleSetActorPhysicalMaterial(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName)) { return Missing; }
    FString MatPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("physical_material_path"), MatPath)) { return Missing; }

    FString LookupError;
    AActor* Actor = UEAgentPhysicsAuthoringImpl::FindEditorActor(ActorName, LookupError);
    if (!Actor)
    {
        return mcp_common::MakeErrorReply(LookupError);
    }

    // The component that receives the override: the static-mesh component of a static-mesh
    // actor, otherwise the actor's first primitive (PA_GetPrimitiveComponent).
    UPrimitiveComponent* Prim = PA_GetPrimitiveComponent(Actor);
    if (!Prim)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor has no primitive component: %s"), *ActorName));
    }
    UPhysicalMaterial* PhysMat = LoadObject<UPhysicalMaterial>(nullptr, *MatPath);
    if (!PhysMat)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("PhysicalMaterial not found: %s"), *MatPath));
    }

    {
        const FScopedTransaction Transaction(LOCTEXT("SetActorPhysMat", "MCP: Set Actor Physical Material"));
        Prim->Modify();
        Prim->SetPhysMaterialOverride(PhysMat);
    }

    // Read back the override directly from the body instance (reflects exactly what
    // SetPhysMaterialOverride stored, independent of whether a physics body is built).
    UPhysicalMaterial* Applied = Prim->BodyInstance.GetPhysMaterialOverride();
    const bool bVerified = (Applied == PhysMat);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actor_name"), Actor->GetActorLabel());
    Result->SetStringField(TEXT("component_name"), Prim->GetName());
    Result->SetStringField(TEXT("physical_material_path"), MatPath);
    Result->SetStringField(TEXT("physical_material_name"), PhysMat->GetName());
    if (Applied)
    {
        Result->SetStringField(TEXT("applied_material_name"), Applied->GetName());
    }
    Result->SetBoolField(TEXT("verified"), bVerified);
    Result->SetBoolField(TEXT("applied"), true);
    return mcp_common::MakeOkReply(Result);
}

// --------------------------------------------------------------------------- //
// get_physics_constraint_info
// --------------------------------------------------------------------------- //
// MCP:get_physics_constraint_info - Read the full authored configuration of a world APhysicsConstraintActor's UPhysicsConstraintComponent: identity + wired bodies/bones, linear/angular limits (with motion types), linear/angular drives (motors) with targets, breakable thresholds, projection, and collision/dominance flags.  Read-only complement of spawn_physics_constraint / set_physics_constraint_limits / set_physics_constraint_drives; returns the serialized template, not live simulation state (no motor torque/broken/force values, which are 0 outside PIE).
//   @param constraint_actor_name - Label/name of the APhysicsConstraintActor.
//   @returns { constraint_actor_name: string, constraint_component: string, constraint_actor1?: string, constraint_actor2?: string, constraint_bone1: string, constraint_bone2: string, linear_limit: number, linear_x_motion: string, linear_y_motion: string, linear_z_motion: string, swing1_limit: number, swing1_motion: string, swing2_limit: number, swing2_motion: string, twist_limit: number, twist_motion: string, angular_drive_mode: string, angular_orientation_drive: bool, angular_velocity_drive: bool, angular_drive_spring: number, angular_drive_damping: number, angular_drive_force_limit: number, angular_orientation_target: [number], linear_position_drive: bool, linear_velocity_drive: bool, linear_drive_spring: number, linear_drive_damping: number, linear_drive_force_limit: number, linear_position_target: [number], linear_breakable: bool, linear_break_threshold: number, angular_breakable: bool, angular_break_threshold: number, projection_enabled: bool, projection_linear_alpha: number, projection_angular_alpha: number, projection_linear_tolerance: number, projection_angular_tolerance: number, parent_dominates: bool, disable_collision: bool, found: bool }

TSharedPtr<FJsonObject> HandleGetPhysicsConstraintInfo(const TSharedPtr<FJsonObject>& Params)
{
    APhysicsConstraintActor* ConstraintActor = nullptr;
    UPhysicsConstraintComponent* ConstraintComp = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentPhysicsAuthoringImpl::ReadConstraintActor(Params, ConstraintActor, ConstraintComp))
    {
        return Error;
    }

    // Read-only: no FScopedTransaction / Modify().
    FConstraintInstance& CI = ConstraintComp->ConstraintInstance;

    // Identity, constrained actors and bones, then limits, drives, breakable, projection and flags.
    TSharedPtr<FJsonObject> Result = UEAgentPhysicsAuthoringImpl::StartConstraintReply(ConstraintActor, ConstraintComp);
    UEAgentPhysicsAuthoringImpl::AddConstrainedActorFields(Result, ConstraintComp);
    Result->SetStringField(TEXT("constraint_bone1"), CI.GetChildBoneName().ToString());
    Result->SetStringField(TEXT("constraint_bone2"), CI.GetParentBoneName().ToString());
    UEAgentPhysicsAuthoringImpl::AddLimitFields(Result, CI);
    UEAgentPhysicsAuthoringImpl::AddDriveFields(Result, CI);

    Result->SetBoolField(TEXT("found"), true);
    return mcp_common::MakeOkReply(Result);
}

// MCP:set_vehicle_wheel_setup - Configure ONE entry of a UChaosWheeledVehicleMovementComponent's WheelSetups array (wheel class, bone, offset) by index. USE THIS RATHER THAN the generic set_properties, which cannot reliably write the wheel class: it is a TSubclassOf nested in a struct nested in an array, and the generic path only FINDS an already-loaded class instead of loading it. This tool resolves the class explicitly and writes the whole struct element, growing the array to fit if wheel_index is past the end. READING them back needs no special tool -- the generic get_properties already resolves nested object refs correctly.
//   @param actor_name [string!] - Editor-world actor with a UChaosWheeledVehicleMovementComponent
//   @param wheel_index [number!] - Index into WheelSetups (>= 0). The array grows to fit if this is past the current end.
//   @param wheel_class [string!] - Full class path of the UChaosVehicleWheel subclass, e.g. "/Script/ChaosVehicles.ChaosVehicleWheel"
//   @param bone_name [string] - Skeletal bone this wheel is attached to. Optional.
//   @param additional_offset [array] - [x, y, z] additional offset applied to the wheel. Optional.
//   @returns { actor_name: string, wheel_index: int, wheel_class: string, bone_name: string, additional_offset: {x,y,z}, num_wheels: int }
//   NOTE (impl): not a thin wrapper. The generic reflection layer (set_properties,
//   mcp_common::SetObjectProperty) cannot write the wheel_class field reliably -- it's a
//   TSubclassOf<UChaosVehicleWheel> nested inside a struct nested inside an array (FArrayProperty ->
//   FStructProperty -> FClassProperty), and the generic JsonValueToUProperty path for object/class refs is
//   find-only-first (StaticFindObjectSafe) with StaticLoadObject only as a fallback -- unreliable
//   for this exact shape. This command resolves
//   wheel_class explicitly via LoadClass and writes the whole struct element directly, growing the array via
//   FScriptArrayHelper if wheel_index is past the end. Reading WheelSetups back does NOT need this treatment:
//   PropertyValueToJson's CustomExportCallback handles FObjectPropertyBase, including FClassProperty, at any
//   nesting depth.
TSharedPtr<FJsonObject> HandleSetVehicleWheelSetup(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName)) { return Missing; }
    double WheelIndexD = -1.0;
    if (!Params->TryGetNumberField(TEXT("wheel_index"), WheelIndexD) || WheelIndexD < 0.0)
    {
        return mcp_common::MakeErrorReply(TEXT("Missing or invalid required parameter: wheel_index (must be >= 0)"));
    }
    const int32 WheelIndex = static_cast<int32>(WheelIndexD);
    FString WheelClassPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("wheel_class"), WheelClassPath)) { return Missing; }

    // The wheel class is validated before any world lookup: a bad class is reported even when
    // the actor does not exist.
    UClass* WheelClass = LoadClass<UChaosVehicleWheel>(nullptr, *WheelClassPath);
    if (!WheelClass)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Could not load wheel_class as a UChaosVehicleWheel subclass: %s"), *WheelClassPath));
    }
    if (WheelClass->HasAnyClassFlags(CLASS_Abstract))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("wheel_class is Abstract, cannot be used as a concrete wheel: %s"), *WheelClassPath));
    }

    FString LookupError;
    AActor* Actor = UEAgentPhysicsAuthoringImpl::FindEditorActor(ActorName, LookupError);
    if (!Actor)
    {
        return mcp_common::MakeErrorReply(LookupError);
    }
    UChaosWheeledVehicleMovementComponent* MovementComp = Actor->FindComponentByClass<UChaosWheeledVehicleMovementComponent>();
    if (!MovementComp)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor has no UChaosWheeledVehicleMovementComponent: %s"), *ActorName));
    }

    FArrayProperty* ArrProp = CastField<FArrayProperty>(
        UChaosWheeledVehicleMovementComponent::StaticClass()->FindPropertyByName(TEXT("WheelSetups")));
    if (!ArrProp)
    {
        return mcp_common::MakeErrorReply(TEXT("Could not reflect UChaosWheeledVehicleMovementComponent::WheelSetups"));
    }
    void* ArrPtr = ArrProp->ContainerPtrToValuePtr<void>(MovementComp);
    FScriptArrayHelper Helper(ArrProp, ArrPtr);

    if (WheelIndex >= Helper.Num())
    {
        Helper.Resize(WheelIndex + 1);
    }

    FChaosWheelSetup* Setup = reinterpret_cast<FChaosWheelSetup*>(Helper.GetRawPtr(WheelIndex));
    Setup->WheelClass = WheelClass;

    FString BoneName;
    if (Params->TryGetStringField(TEXT("bone_name"), BoneName) && !BoneName.IsEmpty())
    {
        Setup->BoneName = FName(*BoneName);
    }
    if (Params->HasField(TEXT("additional_offset")))
    {
        Setup->AdditionalOffset = mcp_common::ReadJsonVector(Params, TEXT("additional_offset"));
    }

    Actor->Modify();
    MovementComp->MarkPackageDirty();

    TSharedPtr<FJsonObject> OffsetObj = mcp_common::VectorToJsonObject(Setup->AdditionalOffset);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actor_name"), ActorName);
    Result->SetNumberField(TEXT("wheel_index"), WheelIndex);
    Result->SetStringField(TEXT("wheel_class"), WheelClass->GetPathName());
    Result->SetStringField(TEXT("bone_name"), Setup->BoneName.ToString());
    Result->SetObjectField(TEXT("additional_offset"), OffsetObj);
    Result->SetNumberField(TEXT("num_wheels"), Helper.Num());
    return mcp_common::MakeOkReply(Result);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("create_physical_material"), &HandleCreatePhysicalMaterial },
    { TEXT("get_physics_constraint_info"), &HandleGetPhysicsConstraintInfo },
    { TEXT("set_actor_physical_material"), &HandleSetActorPhysicalMaterial },
    { TEXT("set_physics_constraint_limits"), &HandleSetConstraintLimits },
    { TEXT("set_physical_material_properties"), &HandleSetPhysicalMaterialProperties },
    { TEXT("set_physics_constraint_drives"), &HandleSetPhysicsConstraintDrives },
    { TEXT("set_vehicle_wheel_setup"), &HandleSetVehicleWheelSetup },
    { TEXT("spawn_physics_constraint"), &HandleSpawnPhysicsConstraint },
};
} // namespace UEAgentPhysicsAuthoringCommands

#undef LOCTEXT_NAMESPACE
