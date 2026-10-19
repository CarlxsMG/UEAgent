#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Animation/AnimAssetUtils.h"

#include "Editor.h"
#include "ScopedTransaction.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Modules/ModuleManager.h"

#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"

#include "Engine/SkeletalMesh.h"
#include "ReferenceSkeleton.h"

// IK Rig runtime (build_cs_public_deps: IKRig)
#include "Rig/IKRigDefinition.h"                              // UIKRigDefinition, UIKRigEffectorGoal, FBoneChain
#include "Rig/Solvers/IKRigFullBodyIK.h"                      // FIKRigFullBodyIKSolver
#include "Rig/Solvers/IKRigLimbSolver.h"                      // FIKRigLimbSolver
#include "Rig/Solvers/IKRigPoleSolver.h"                      // FIKRigPoleSolver
#include "Rig/Solvers/IKRigBodyMoverSolver.h"                 // FIKRigBodyMoverSolver / Settings
#include "Rig/Solvers/IKRigSetTransform.h"                    // FIKRigSetTransform / Settings
#include "Rig/Solvers/IKRigStretchLimb.h"                     // FIKRigStretchLimbSolver / Settings
#include "Rig/Solvers/IKRigSolverBase.h"                      // FIKRigSolverBase
#include "Retargeter/IKRetargeter.h"                          // UIKRetargeter
#include "Retargeter/IKRetargetSettings.h"                    // ERetargetSourceOrTarget
#include "Retargeter/IKRetargetOps.h"                         // FIKRetargetOpBase / FIKRetargetOpSettingsBase

// IK Rig editor (build_cs_editor_deps: IKRigEditor)
#include "RigEditor/IKRigController.h"                        // UIKRigController
#include "RigEditor/IKRigDefinitionFactory.h"                 // UIKRigDefinitionFactory::CreateNewIKRigAsset (static, non-modal)
#include "RetargetEditor/IKRetargeterController.h"            // UIKRetargeterController
#include "RetargetEditor/IKRetargetFactory.h"                 // UIKRetargetFactory (non-modal FactoryCreateNew)

// InstancedStruct for solver type introspection
#include "StructUtils/InstancedStruct.h"

// Generic (reflection-based) UStruct <-> JSON for non-FBIK solver settings
#include "JsonObjectConverter.h"                              // FJsonObjectConverter::UStructToJsonObject / JsonObjectToUStruct
#include "UObject/UnrealType.h"                               // TFieldIterator / FNumericProperty (generic clamp pass)

/**
 * IK Rig + IK Retargeter authoring commands: UIKRigDefinition and UIKRetargeter assets, edited through the
 * engine's editor controllers (UIKRigController, UIKRetargeterController).
 */
namespace UEAgentIKRigCommands
{

// MCP-DOMAIN: Animation

namespace
{
    // -- Enum conversion helpers --------------------------------------

    /** The name of a TEnum value (EPBIKLimitType, EPBIKRootBehavior), or the number when the enum is not found. */
    template <typename TEnum>
    FString IKRig_EnumValueToStr(int32 Val)
    {
        const UEnum* E = StaticEnum<TEnum>();
        return E ? E->GetNameStringByValue(Val) : FString::FromInt(Val);
    }

    /** The TEnum value a name stands for; 0 (the first value) when the name is unknown. */
    template <typename TEnum>
    int32 IKRig_StrToEnumValue(const FString& S)
    {
        const UEnum* E = StaticEnum<TEnum>();
        if (!E) { return 0; }
        int64 V = E->GetValueByNameString(S);
        return V == INDEX_NONE ? 0 : static_cast<int32>(V);
    }

    // -- FBIK solver settings serialization --------------------------

    /** Serialize FIKRigFullBodyIKSolver (FBIK) solver-level settings to JSON. */
    TSharedPtr<FJsonObject> SerializeFBIKSolver(const FIKRigFullBodyIKSolver& S)
    {
        // Solver-level tunables live on the nested FIKRigFBIKSettings Settings member.
        const FIKRigFBIKSettings& Cfg = S.Settings;
        TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("root_bone"), Cfg.RootBone.ToString());
        O->SetNumberField(TEXT("iterations"), Cfg.Iterations);
        O->SetNumberField(TEXT("sub_iterations"), Cfg.SubIterations);
        O->SetNumberField(TEXT("mass_multiplier"), Cfg.MassMultiplier);
        O->SetBoolField(TEXT("allow_stretch"), Cfg.bAllowStretch);
        O->SetStringField(TEXT("root_behavior"), IKRig_EnumValueToStr<EPBIKRootBehavior>(static_cast<int32>(Cfg.RootBehavior)));
        O->SetNumberField(TEXT("global_pull_chain_alpha"), Cfg.GlobalPullChainAlpha);
        O->SetNumberField(TEXT("max_angle"), Cfg.MaxAngle);
        O->SetNumberField(TEXT("over_relaxation"), Cfg.OverRelaxation);
        return O;
    }

    /** Apply JSON values onto the FIKRigFBIKSettings of a FullBodyIK solver (partial update: only fields present in the JSON are set). */
    void ApplyFBIKSolverJson(FIKRigFBIKSettings& Cfg, const TSharedPtr<FJsonObject>& J)
    {
        FString Str;
        double Num;
        bool Bool;
        if (J->TryGetStringField(TEXT("root_bone"), Str))            Cfg.RootBone = FName(*Str);
        if (J->TryGetNumberField(TEXT("iterations"), Num))           Cfg.Iterations = FMath::Clamp(static_cast<int32>(Num), 0, 1000);
        if (J->TryGetNumberField(TEXT("sub_iterations"), Num))       Cfg.SubIterations = FMath::Clamp(static_cast<int32>(Num), 0, 1000);
        if (J->TryGetNumberField(TEXT("mass_multiplier"), Num))      Cfg.MassMultiplier = FMath::Clamp(static_cast<float>(Num), 0.f, 10.f);
        if (J->TryGetBoolField(TEXT("allow_stretch"), Bool))         Cfg.bAllowStretch = Bool;
        if (J->TryGetStringField(TEXT("root_behavior"), Str))        Cfg.RootBehavior = static_cast<EPBIKRootBehavior>(IKRig_StrToEnumValue<EPBIKRootBehavior>(Str));
        if (J->TryGetNumberField(TEXT("global_pull_chain_alpha"), Num)) Cfg.GlobalPullChainAlpha = FMath::Clamp(static_cast<float>(Num), 0.f, 1.f);
        if (J->TryGetNumberField(TEXT("max_angle"), Num))            Cfg.MaxAngle = FMath::Clamp(static_cast<float>(Num), 0.f, 45.f);
        if (J->TryGetNumberField(TEXT("over_relaxation"), Num))      Cfg.OverRelaxation = FMath::Clamp(static_cast<float>(Num), 1.f, 2.f);
    }

    // -- FBIK goal settings serialization ----------------------------

    TSharedPtr<FJsonObject> SerializeFBIKGoal(const FIKRigFBIKGoalSettings& G)
    {
        TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("bone_name"), G.BoneName.ToString());
        O->SetNumberField(TEXT("chain_depth"), G.ChainDepth);
        O->SetNumberField(TEXT("strength_alpha"), G.StrengthAlpha);
        O->SetNumberField(TEXT("pull_chain_alpha"), G.PullChainAlpha);
        O->SetNumberField(TEXT("pin_rotation"), G.PinRotation);
        return O;
    }

    void ApplyFBIKGoalJson(FIKRigFBIKGoalSettings& G, const TSharedPtr<FJsonObject>& J)
    {
        FString Str;
        double Num;
        if (J->TryGetStringField(TEXT("bone_name"), Str))            G.BoneName = FName(*Str);
        if (J->TryGetNumberField(TEXT("chain_depth"), Num))          G.ChainDepth = FMath::Max(static_cast<int32>(Num), 0);
        if (J->TryGetNumberField(TEXT("strength_alpha"), Num))       G.StrengthAlpha = FMath::Clamp(static_cast<float>(Num), 0.f, 1.f);
        if (J->TryGetNumberField(TEXT("pull_chain_alpha"), Num))     G.PullChainAlpha = FMath::Clamp(static_cast<float>(Num), 0.f, 1.f);
        if (J->TryGetNumberField(TEXT("pin_rotation"), Num))         G.PinRotation = FMath::Clamp(static_cast<float>(Num), 0.f, 1.f);
    }

    // -- FBIK bone settings serialization ----------------------------

    TSharedPtr<FJsonObject> SerializeFBIKBone(const FIKRigFBIKBoneSettings& B)
    {
        TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
        O->SetNumberField(TEXT("rotation_stiffness"), B.RotationStiffness);
        O->SetNumberField(TEXT("position_stiffness"), B.PositionStiffness);
        O->SetStringField(TEXT("limit_x"), IKRig_EnumValueToStr<EPBIKLimitType>(static_cast<int32>(B.X)));
        O->SetStringField(TEXT("limit_y"), IKRig_EnumValueToStr<EPBIKLimitType>(static_cast<int32>(B.Y)));
        O->SetStringField(TEXT("limit_z"), IKRig_EnumValueToStr<EPBIKLimitType>(static_cast<int32>(B.Z)));
        O->SetNumberField(TEXT("min_x"), B.MinX);
        O->SetNumberField(TEXT("max_x"), B.MaxX);
        O->SetNumberField(TEXT("min_y"), B.MinY);
        O->SetNumberField(TEXT("max_y"), B.MaxY);
        O->SetNumberField(TEXT("min_z"), B.MinZ);
        O->SetNumberField(TEXT("max_z"), B.MaxZ);
        O->SetBoolField(TEXT("use_preferred_angles"), B.bUsePreferredAngles);
        O->SetNumberField(TEXT("preferred_x"), B.PreferredAngles.X);
        O->SetNumberField(TEXT("preferred_y"), B.PreferredAngles.Y);
        O->SetNumberField(TEXT("preferred_z"), B.PreferredAngles.Z);
        return O;
    }

    void ApplyFBIKBoneJson(FIKRigFBIKBoneSettings& B, const TSharedPtr<FJsonObject>& J)
    {
        FString Str;
        double Num;
        bool Bool;
        if (J->TryGetNumberField(TEXT("rotation_stiffness"), Num))   B.RotationStiffness = FMath::Clamp(static_cast<float>(Num), 0.f, 1.f);
        if (J->TryGetNumberField(TEXT("position_stiffness"), Num))   B.PositionStiffness = FMath::Clamp(static_cast<float>(Num), 0.f, 1.f);
        if (J->TryGetStringField(TEXT("limit_x"), Str))              B.X = static_cast<EPBIKLimitType>(IKRig_StrToEnumValue<EPBIKLimitType>(Str));
        if (J->TryGetStringField(TEXT("limit_y"), Str))              B.Y = static_cast<EPBIKLimitType>(IKRig_StrToEnumValue<EPBIKLimitType>(Str));
        if (J->TryGetStringField(TEXT("limit_z"), Str))              B.Z = static_cast<EPBIKLimitType>(IKRig_StrToEnumValue<EPBIKLimitType>(Str));
        if (J->TryGetNumberField(TEXT("min_x"), Num))                B.MinX = static_cast<float>(Num);
        if (J->TryGetNumberField(TEXT("max_x"), Num))                B.MaxX = static_cast<float>(Num);
        if (J->TryGetNumberField(TEXT("min_y"), Num))                B.MinY = static_cast<float>(Num);
        if (J->TryGetNumberField(TEXT("max_y"), Num))                B.MaxY = static_cast<float>(Num);
        if (J->TryGetNumberField(TEXT("min_z"), Num))                B.MinZ = static_cast<float>(Num);
        if (J->TryGetNumberField(TEXT("max_z"), Num))                B.MaxZ = static_cast<float>(Num);
        if (J->TryGetBoolField(TEXT("use_preferred_angles"), Bool))  B.bUsePreferredAngles = Bool;
        if (J->HasField(TEXT("preferred_x")) || J->HasField(TEXT("preferred_y")) || J->HasField(TEXT("preferred_z")))
        {
            double PX = B.PreferredAngles.X, PY = B.PreferredAngles.Y, PZ = B.PreferredAngles.Z;
            J->TryGetNumberField(TEXT("preferred_x"), PX);
            J->TryGetNumberField(TEXT("preferred_y"), PY);
            J->TryGetNumberField(TEXT("preferred_z"), PZ);
            B.PreferredAngles = FVector(PX, PY, PZ);
        }
    }

    // -- Generic (reflection-based) settings serialization --------
    // One shared helper trio used for every non-FBIK solver type. The solver
    // exposes its live settings struct via the FIKRigSolverBase virtuals
    // GetSolverSettingsType()/GetSolverSettings() (verified in IKRigSolverBase.h:
    // 101-103), so we serialize/merge it generically — no per-type hand code.

    /** Serialize any live UStruct instance to a JSON object (camelCase keys). */
    TSharedPtr<FJsonObject> SerializeStructGeneric(const UScriptStruct* Type, const void* Ptr)
    {
        if (Type && Ptr)
        {
            if (TSharedPtr<FJsonObject> Out = mcp_common::StructToJson(Type, Ptr, 0, 0))
            {
                return Out;
            }
        }
        // Preserve the never-null contract: empty object when Type/Ptr are null or conversion fails.
        return MakeShared<FJsonObject>();
    }

    /** Enforce ClampMin/ClampMax UPROPERTY metadata on every numeric field.
     *  FJsonObjectConverter::JsonObjectToUStruct ignores Clamp meta, so we apply
     *  it here generically (reads the real meta — Pole Alpha 0-1, SetTransform
     *  alphas 0-1, StretchLimb StretchStartPercent 0.1-1.0, Limb base settings...).
     *  Soft UIMin/UIMax-only fields (e.g. BodyMover alphas) are deliberately left
     *  untouched, matching the engine's own (non-)clamping. */
    void ClampStructNumericMeta(const UScriptStruct* Type, void* Ptr)
    {
        // Delegates to the centralized clamp pass (itself WITH_EDITORONLY_DATA-gated).
        mcp_common::ClampNumericByMeta(Type, Ptr);
    }

    /** Partial JSON merge onto a live UStruct (only fields present in the JSON are
     *  written — idempotent), followed by the clamp post-pass. */
    void MergeJsonIntoStructGeneric(const UScriptStruct* Type, void* Ptr, const TSharedPtr<FJsonObject>& Json)
    {
        if (!Type || !Ptr || !Json.IsValid()) { return; }
        FString JsonErr;
        mcp_common::JsonToStruct(Json, Type, Ptr, JsonErr, 0, 0);
        ClampStructNumericMeta(Type, Ptr);
    }

    /** Detect solver type canonical name from an InstancedStruct. */
    FString DetectSolverTypeCanonical(const FInstancedStruct* SS)
    {
        if (!SS || !SS->IsValid()) { return TEXT("Unknown"); }
        const UScriptStruct* ST = SS->GetScriptStruct();
        if (ST == FIKRigFullBodyIKSolver::StaticStruct()) { return TEXT("FullBodyIK"); }
        if (ST == FIKRigLimbSolver::StaticStruct()) { return TEXT("Limb"); }
        if (ST == FIKRigPoleSolver::StaticStruct()) { return TEXT("Pole"); }
        if (ST == FIKRigBodyMoverSolver::StaticStruct()) { return TEXT("BodyMover"); }
        if (ST == FIKRigSetTransform::StaticStruct()) { return TEXT("SetTransform"); }
        if (ST == FIKRigStretchLimbSolver::StaticStruct())return TEXT("StretchLimb");
        return ST->GetName();
    }

    /**
     * Loads the IK Rig at RigPath into OutRig and its controller into OutController: null, else the reply naming the
     * one missing.
     */
    TSharedPtr<FJsonObject> IKRig_LoadRigController(const FString& RigPath, UIKRigDefinition*& OutRig, UIKRigController*& OutController)
    {
        if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadFlexibleOrError(RigPath, TEXT("IK Rig"), OutRig))
        {
            return Error;
        }
        OutController = UIKRigController::GetController(OutRig);
        if (!OutController)
        {
            return mcp_common::MakeErrorReply(TEXT("Could not get IK Rig controller"));
        }
        return nullptr;
    }

    /**
     * Loads the IK Retargeter at RetargeterPath into OutRetargeter and its controller into OutController: null, else
     * the reply naming the one missing.
     */
    TSharedPtr<FJsonObject> IKRig_LoadRetargeterController(const FString& RetargeterPath, UIKRetargeter*& OutRetargeter,
        UIKRetargeterController*& OutController)
    {
        if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadFlexibleOrError(RetargeterPath, TEXT("IK Retargeter"), OutRetargeter))
        {
            return Error;
        }
        OutController = UIKRetargeterController::GetController(OutRetargeter);
        if (!OutController)
        {
            return mcp_common::MakeErrorReply(TEXT("Could not get IK Retargeter controller"));
        }
        return nullptr;
    }

    /**
     * The "solver_index out of range" reply when SolverIndex is not a solver of Controller (naming the valid range
     * when bShowRange), else null.
     */
    TSharedPtr<FJsonObject> IKRig_CheckSolverIndex(UIKRigController* Controller, int32 SolverIndex, bool bShowRange)
    {
        if (SolverIndex >= 0 && SolverIndex < Controller->GetNumSolvers())
        {
            return nullptr;
        }
        if (bShowRange)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("solver_index %d out of range (0..%d)"), SolverIndex, Controller->GetNumSolvers() - 1));
        }
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("solver_index %d out of range"), SolverIndex));
    }

    /** The optional source_or_target of a retarget-pose command ("Target" when absent) and the side it names. */
    ERetargetSourceOrTarget IKRig_ReadSourceOrTarget(const TSharedPtr<FJsonObject>& Params, FString& OutSourceOrTarget)
    {
        OutSourceOrTarget = TEXT("Target");
        Params->TryGetStringField(TEXT("source_or_target"), OutSourceOrTarget);
        return OutSourceOrTarget.Equals(TEXT("Source"), ESearchCase::IgnoreCase)
            ? ERetargetSourceOrTarget::Source : ERetargetSourceOrTarget::Target;
    }

    /** Map a friendly solver_type string to the concrete IK Rig solver UScriptStruct. */
    UScriptStruct* ResolveSolverStruct(const FString& In, FString& OutCanonical)
    {
        struct FSolverTypeName
        {
            const TCHAR* Canonical;
            UScriptStruct* Struct;
            const TCHAR* Aliases[4];
        };
        const FSolverTypeName SolverTypes[] =
        {
            { TEXT("FullBodyIK"),   FIKRigFullBodyIKSolver::StaticStruct(),  { TEXT("fullbodyik"), TEXT("fbik"), TEXT("full body ik"), TEXT("fikrigfullbodyiksolver") } },
            { TEXT("Limb"),         FIKRigLimbSolver::StaticStruct(),        { TEXT("limb"), TEXT("limbik"), TEXT("limb ik"), TEXT("fikriglimbsolver") } },
            { TEXT("Pole"),         FIKRigPoleSolver::StaticStruct(),        { TEXT("pole"), TEXT("polesolver"), TEXT("pole solver"), TEXT("fikrigpolesolver") } },
            { TEXT("BodyMover"),    FIKRigBodyMoverSolver::StaticStruct(),   { TEXT("bodymover"), TEXT("body mover"), TEXT("bodymoversolver"), TEXT("fikrigbodymoversolver") } },
            { TEXT("SetTransform"), FIKRigSetTransform::StaticStruct(),      { TEXT("settransform"), TEXT("set transform"), TEXT("settransformsolver"), TEXT("fikrigsettransform") } },
            { TEXT("StretchLimb"),  FIKRigStretchLimbSolver::StaticStruct(), { TEXT("stretchlimb"), TEXT("stretch limb"), TEXT("stretchlimbsolver"), TEXT("fikrigstretchlimbsolver") } },
        };
        const FString Lower = In.ToLower();
        for (const FSolverTypeName& SolverType : SolverTypes)
        {
            for (const TCHAR* Alias : SolverType.Aliases)
            {
                // An empty type is the first entry, FullBodyIK.
                if (Lower.IsEmpty() || Lower == Alias)
                {
                    OutCanonical = SolverType.Canonical;
                    return SolverType.Struct;
                }
            }
        }
        // Unknown, non-empty type -> honest failure (no silent FBIK fallback).
        OutCanonical = In;
        return nullptr;
    }

    /** Pick a real leaf bone name from a skeletal mesh's RefSkeleton (a bone with no children).
     *  Falls back to the last bone, then to the root. Returns NAME_None if mesh has no bones. */
    FName PickLeafBone(USkeletalMesh* Mesh)
    {
        if (!Mesh)
        {
            return NAME_None;
        }
        const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();
        const int32 NumBones = Ref.GetNum();
        if (NumBones <= 0)
        {
            return NAME_None;
        }

        // Mark which bones are parents.
        TArray<bool> HasChild;
        HasChild.Init(false, NumBones);
        for (int32 i = 1; i < NumBones; ++i)
        {
            const int32 Parent = Ref.GetParentIndex(i);
            if (Ref.IsValidIndex(Parent))
            {
                HasChild[Parent] = true;
            }
        }
        // First leaf (no child) we find.
        for (int32 i = NumBones - 1; i >= 0; --i)
        {
            if (!HasChild[i])
            {
                return Ref.GetBoneName(i);
            }
        }
        // No leaf found (shouldn't happen) -> last bone, then root.
        return Ref.GetBoneName(NumBones - 1);
    }

    /** Fill solver/goal/chain counts + the skeletal mesh path from a controller. */
    void FillRigInfo(UIKRigController* Controller, UIKRigDefinition* Rig,
                     const TSharedPtr<FJsonObject>& Out, bool bIncludeLists)
    {
        const int32 SolverCount = Controller->GetNumSolvers();
        const TArray<UIKRigEffectorGoal*>& Goals = Controller->GetAllGoals();
        const TArray<FBoneChain>& Chains = Controller->GetRetargetChains();

        Out->SetNumberField(TEXT("solver_count"), SolverCount);
        Out->SetNumberField(TEXT("goal_count"), Goals.Num());
        Out->SetNumberField(TEXT("retarget_chain_count"), Chains.Num());

        USkeletalMesh* Mesh = Controller->GetSkeletalMesh();
        Out->SetStringField(TEXT("skeletal_mesh"), Mesh ? Mesh->GetPathName() : TEXT(""));

        if (bIncludeLists)
        {
            TArray<TSharedPtr<FJsonValue>> GoalArr;
            for (const UIKRigEffectorGoal* Goal : Goals)
            {
                if (!Goal)
                {
                    continue;
                }
                TSharedPtr<FJsonObject> G = MakeShared<FJsonObject>();
                G->SetStringField(TEXT("goal_name"), Goal->GoalName.ToString());
                G->SetStringField(TEXT("bone_name"), Goal->BoneName.ToString());
                GoalArr.Add(MakeShared<FJsonValueObject>(G));
            }
            Out->SetArrayField(TEXT("goals"), GoalArr);

            TArray<TSharedPtr<FJsonValue>> ChainArr;
            for (const FBoneChain& Chain : Chains)
            {
                TSharedPtr<FJsonObject> C = MakeShared<FJsonObject>();
                C->SetStringField(TEXT("chain_name"), Chain.ChainName.ToString());
                C->SetStringField(TEXT("start_bone"), Chain.StartBone.BoneName.ToString());
                C->SetStringField(TEXT("end_bone"), Chain.EndBone.BoneName.ToString());
                C->SetStringField(TEXT("goal_name"), Chain.IKGoalName.ToString());
                ChainArr.Add(MakeShared<FJsonValueObject>(C));
            }
            Out->SetArrayField(TEXT("chains"), ChainArr);
        }
    }

    /**
     * The reply of a valid call that changed nothing: success with saved=false and Note. The asset was neither
     * dirtied nor saved, so a Play-in-Editor session or a read-only root does not turn the no-op into a failure.
     */
    TSharedPtr<FJsonObject> IKRig_NoChangeReply(const TSharedPtr<FJsonObject>& Resp, const FString& Note)
    {
        Resp->SetBoolField(TEXT("saved"), false);
        Resp->SetStringField(TEXT("note"), Note);
        return mcp_common::MakeOkReply(Resp);
    }

    /**
     * Whether Apply, run on a scratch copy of the Type struct at Live, leaves the copy different from Live. Lets a
     * settings write that would change nothing return before Modify() dirties the asset.
     */
    bool IKRig_WouldChangeStruct(const UScriptStruct* Type, const void* Live, TFunctionRef<void(void*)> Apply)
    {
        FInstancedStruct Scratch;
        Scratch.InitializeAs(Type, static_cast<const uint8*>(Live));
        Apply(Scratch.GetMutableMemory());
        return !Type->CompareScriptStruct(Scratch.GetMemory(), Live, PPF_None);
    }

    /** Whether the IK Rig of Controller has a retarget chain named ChainName. */
    bool IKRig_HasRetargetChain(const UIKRigController* Controller, FName ChainName)
    {
        return Controller->GetRetargetChains().ContainsByPredicate(
            [ChainName](const FBoneChain& Chain) { return Chain.ChainName == ChainName; });
    }
}

// ---------------------------------------------------------------------------
// create_ik_rig
// ---------------------------------------------------------------------------
// MCP:create_ik_rig - Create and save a UIKRigDefinition asset and set its source skeletal mesh.  Idempotent: an existing rig is loaded and only given the mesh, then saved, when it has another one; when it already has that mesh, or the mesh does not load or the engine refuses it (skeletal_mesh_set false), nothing changes, nothing is saved and a note says why. The default skeletal_mesh_path is /Engine/EngineMeshes/SkeletalCube. A failed save makes success false, with saved=false and save_error.
//   @param package_path - UE content folder, e.g. /Game/MyGame
//   @param name - Asset name for the new IK Rig
//   @param skeletal_mesh_path - Optional USkeletalMesh asset path; defaults to /Engine/EngineMeshes/SkeletalCube
//   @returns { asset_path: string, name: string, skeletal_mesh: string, skeletal_mesh_set: bool, created: bool, already_existed: bool, saved: bool, save_error?: string, note?: string }

TSharedPtr<FJsonObject> HandleCreateIKRig(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath, Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("package_path"), PackagePath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LongPackagePathError(PackagePath)) { return Error; }

    FString SkeletalMeshPath = TEXT("/Engine/EngineMeshes/SkeletalCube");
    Params->TryGetStringField(TEXT("skeletal_mesh_path"), SkeletalMeshPath);

    FString Folder, FullPath;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::JoinAssetPath(PackagePath, Name, Folder, FullPath)) { return Error; }

    USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *SkeletalMeshPath);

    // Idempotent: if it already exists, load it and (re)apply the mesh, return a stable payload.
    bool bCreated = true;
    UIKRigDefinition* Rig = nullptr;
    if (UEditorAssetLibrary::DoesAssetExist(FullPath))
    {
        Rig = mcp_common::LoadAssetOrSoftPath<UIKRigDefinition>(FullPath);
        bCreated = false;
    }

    if (!Rig)
    {
        // CreateNewIKRigAsset takes (folder, name), creates + registers (no modal), returns the asset.
        Rig = UIKRigDefinitionFactory::CreateNewIKRigAsset(Folder, Name);
        bCreated = true;
    }
    if (!Rig)
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Failed to create IK Rig asset at %s"), *FullPath));

    UIKRigController* Controller = UIKRigController::GetController(Rig);
    if (!Controller)
        return mcp_common::MakeErrorReply(TEXT("Could not get IK Rig controller"));

    // An existing rig is only touched when there is a loaded mesh it does not have yet: SetSkeletalMesh
    // calls Modify() even for the mesh the rig already has, which would dirty the asset for nothing.
    const bool bMeshAlreadySet = Mesh && Controller->GetSkeletalMesh() == Mesh;
    if (!bCreated && (!Mesh || bMeshAlreadySet))
    {
        USkeletalMesh* CurrentMesh = Controller->GetSkeletalMesh();
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetStringField(TEXT("asset_path"), Rig->GetPathName());
        Resp->SetStringField(TEXT("name"), Name);
        Resp->SetStringField(TEXT("skeletal_mesh"), CurrentMesh ? CurrentMesh->GetPathName() : TEXT(""));
        Resp->SetBoolField(TEXT("skeletal_mesh_set"), bMeshAlreadySet);
        Resp->SetBoolField(TEXT("created"), false);
        Resp->SetBoolField(TEXT("already_existed"), true);
        return IKRig_NoChangeReply(Resp, bMeshAlreadySet
            ? TEXT("The IK Rig already exists with that skeletal mesh; nothing changed and nothing was saved.")
            : FString::Printf(TEXT("The IK Rig already exists and skeletal_mesh_path '%s' did not load as a USkeletalMesh; nothing changed and nothing was saved."), *SkeletalMeshPath));
    }

    bool bMeshSet = false;
    if (Mesh)
    {
        // Set the source skeleton; this loads the bone hierarchy into the asset's IKRigSkeleton.
        bMeshSet = Controller->SetSkeletalMesh(Mesh);
    }

    // The engine refused the new mesh of an existing rig (it lacks bones the rig uses): the rig is unchanged.
    if (!bCreated && !bMeshSet)
    {
        USkeletalMesh* CurrentMesh = Controller->GetSkeletalMesh();
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetStringField(TEXT("asset_path"), Rig->GetPathName());
        Resp->SetStringField(TEXT("name"), Name);
        Resp->SetStringField(TEXT("skeletal_mesh"), CurrentMesh ? CurrentMesh->GetPathName() : TEXT(""));
        Resp->SetBoolField(TEXT("skeletal_mesh_set"), false);
        Resp->SetBoolField(TEXT("created"), false);
        Resp->SetBoolField(TEXT("already_existed"), true);
        return IKRig_NoChangeReply(Resp, FString::Printf(
            TEXT("The engine refused skeletal mesh %s for this IK Rig (it lacks bones the rig uses, see the editor log); nothing changed and nothing was saved."),
            *Mesh->GetPathName()));
    }

    if (bCreated)
    {
        FAssetRegistryModule::AssetCreated(Rig);
    }
    FString SaveErr;
    const bool bSaved = mcp_common::SaveAssetPackage(Rig->GetOutermost(), Rig, SaveErr);

    USkeletalMesh* ActualMesh = Controller->GetSkeletalMesh();

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("asset_path"), Rig->GetPathName());
    Resp->SetStringField(TEXT("name"), Name);
    Resp->SetStringField(TEXT("skeletal_mesh"),
        ActualMesh ? ActualMesh->GetPathName() : (Mesh ? Mesh->GetPathName() : TEXT("")));
    Resp->SetBoolField(TEXT("skeletal_mesh_set"), bMeshSet);
    Resp->SetBoolField(TEXT("created"), bCreated);
    Resp->SetBoolField(TEXT("already_existed"), !bCreated);
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("create_ik_rig"), SaveErr);
}

// ---------------------------------------------------------------------------
// add_ik_solver
// ---------------------------------------------------------------------------
// MCP:add_ik_solver - Add an IK solver to the bottom of an IK Rig's solver stack.  Solver type is one of "FullBodyIK", "Limb", "Pole", "BodyMover", "SetTransform" or "StretchLimb" (default "FullBodyIK"). Unknown types are rejected with an honest error. A failed save makes success false, with saved=false and save_error.
//   @param ik_rig_path - Path to the UIKRigDefinition asset
//   @param solver_type - Optional solver type: "FullBodyIK"|"Limb"|"Pole"|"BodyMover"|"SetTransform"|"StretchLimb"; defaults to "FullBodyIK"
//   @returns { asset_path: string, solver_index: int, solver_type: string, solver_struct: string, solver_count: int, saved: bool, save_error?: string }

TSharedPtr<FJsonObject> HandleAddIKSolver(const TSharedPtr<FJsonObject>& Params)
{
    FString RigPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ik_rig_path"), RigPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    FString SolverTypeStr = TEXT("FullBodyIK");
    Params->TryGetStringField(TEXT("solver_type"), SolverTypeStr);

    UIKRigDefinition* Rig = nullptr;
    UIKRigController* Controller = nullptr;
    if (TSharedPtr<FJsonObject> Error = IKRig_LoadRigController(RigPath, Rig, Controller)) { return Error; }

    FString CanonicalType;
    UScriptStruct* SolverStruct = ResolveSolverStruct(SolverTypeStr, CanonicalType);
    if (!SolverStruct)
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Unknown solver_type: %s"), *SolverTypeStr));

    // Use the UScriptStruct* overload (robust; avoids package-path string parsing).
    const int32 SolverIndex = Controller->AddSolver(SolverStruct);
    if (SolverIndex == INDEX_NONE)
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("AddSolver failed for '%s'"), *CanonicalType));

    FString SaveErr;
    const bool bSaved = mcp_common::SaveAssetPackage(Rig->GetOutermost(), Rig, SaveErr);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("asset_path"), Rig->GetPathName());
    Resp->SetNumberField(TEXT("solver_index"), SolverIndex);
    Resp->SetStringField(TEXT("solver_type"), CanonicalType);
    Resp->SetStringField(TEXT("solver_struct"), SolverStruct->GetName());
    Resp->SetNumberField(TEXT("solver_count"), Controller->GetNumSolvers());
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("add_ik_solver"), SaveErr);
}

// ---------------------------------------------------------------------------
// add_ik_goal
// ---------------------------------------------------------------------------
// MCP:add_ik_goal - Add an IK goal to an IK Rig, associated with a bone, through the IK Rig controller (one undo transaction), then save the asset.  If bone_name is omitted, a leaf bone of the rig's skeletal mesh is picked automatically. Idempotent: a goal of that name on the same bone is not re-created but returned with already_exists=true, saved=false and a note, and nothing is saved. A failed save makes success false, with saved=false and save_error.
//   @param ik_rig_path - Path to the UIKRigDefinition asset
//   @param goal_name - Unique name for the new goal
//   @param bone_name - Optional bone name to attach the goal to
//   @returns { asset_path: string, goal_name: string, bone_name: string, goal_count: int, saved: bool, already_exists?: bool, save_error?: string, note?: string }

TSharedPtr<FJsonObject> HandleAddIKGoal(const TSharedPtr<FJsonObject>& Params)
{
    FString RigPath, GoalName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ik_rig_path"), RigPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("goal_name"), GoalName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UIKRigDefinition* Rig = nullptr;
    UIKRigController* Controller = nullptr;
    if (TSharedPtr<FJsonObject> Error = IKRig_LoadRigController(RigPath, Rig, Controller)) { return Error; }

    // Resolve bone name: explicit param, else a real leaf bone from the rig's skeletal mesh.
    FString BoneNameStr;
    Params->TryGetStringField(TEXT("bone_name"), BoneNameStr);
    FName BoneName(*BoneNameStr);
    if (BoneNameStr.IsEmpty())
    {
        USkeletalMesh* Mesh = Controller->GetSkeletalMesh();
        BoneName = PickLeafBone(Mesh);
        if (BoneName.IsNone())
            return mcp_common::MakeErrorReply(
                TEXT("No bone_name given and the IK Rig has no skeletal mesh / bones to pick from"));
    }

    // Idempotent: check if goal already exists with the same bone.
    const FName GoalFName(*GoalName);
    UIKRigEffectorGoal* ExistingGoal = Controller->GetGoal(GoalFName);
    if (ExistingGoal && ExistingGoal->BoneName == BoneName)
    {
        // The goal the caller asked for is already there: nothing changes, so nothing is saved.
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetStringField(TEXT("asset_path"), Rig->GetPathName());
        Resp->SetStringField(TEXT("goal_name"), GoalName);
        Resp->SetStringField(TEXT("bone_name"), BoneName.ToString());
        Resp->SetNumberField(TEXT("goal_count"), Controller->GetAllGoals().Num());
        Resp->SetBoolField(TEXT("already_exists"), true);
        return IKRig_NoChangeReply(Resp, TEXT("A goal of that name already exists on that bone; nothing changed and nothing was saved."));
    }

    const FName ResultGoal = Controller->AddNewGoal(FName(*GoalName), BoneName);
    if (ResultGoal.IsNone())
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("AddNewGoal failed for goal '%s' on bone '%s'"), *GoalName, *BoneName.ToString()));

    FString SaveErr;
    const bool bSaved = mcp_common::SaveAssetPackage(Rig->GetOutermost(), Rig, SaveErr);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("asset_path"), Rig->GetPathName());
    Resp->SetStringField(TEXT("goal_name"), ResultGoal.ToString());
    Resp->SetStringField(TEXT("bone_name"), BoneName.ToString());
    Resp->SetNumberField(TEXT("goal_count"), Controller->GetAllGoals().Num());
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("add_ik_goal"), SaveErr);
}

// ---------------------------------------------------------------------------
// remove_ik_goal
// ---------------------------------------------------------------------------
// MCP:remove_ik_goal - Remove an IK goal from an IK Rig by name through the IK Rig controller (one undo transaction), then save the asset.  Inverse of add_ik_goal: also detaches the goal from every solver's effector and clears it from any retarget chain that referenced it. Idempotent: removing a goal that isn't present is a success with removed:false, already_absent:true, saved:false and a note, and nothing is saved. A failed save makes success false, with saved=false and save_error.
//   @param ik_rig_path - Path to the UIKRigDefinition asset
//   @param goal_name - Name of the goal to remove
//   @returns { asset_path: string, goal_name: string, removed: bool, already_absent: bool, goal_count: int, saved: bool, save_error?: string, note?: string }

TSharedPtr<FJsonObject> HandleRemoveIKGoal(const TSharedPtr<FJsonObject>& Params)
{
    FString RigPath, GoalName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ik_rig_path"), RigPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("goal_name"), GoalName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UIKRigDefinition* Rig = nullptr;
    UIKRigController* Controller = nullptr;
    if (TSharedPtr<FJsonObject> Error = IKRig_LoadRigController(RigPath, Rig, Controller)) { return Error; }

    // Idempotent: RemoveGoal returns false both when the goal is absent AND on any
    // internal failure. Distinguish absence up front (GetGoal == null) so a missing
    // goal is an honest no-op, not a hard error.
    const FName GoalFName(*GoalName);
    const bool bExisted = Controller->GetGoal(GoalFName) != nullptr;

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("asset_path"), Rig->GetPathName());
    Resp->SetStringField(TEXT("goal_name"), GoalName);
    if (!bExisted)
    {
        Resp->SetBoolField(TEXT("removed"), false);
        Resp->SetBoolField(TEXT("already_absent"), true);
        Resp->SetNumberField(TEXT("goal_count"), Controller->GetAllGoals().Num());
        return IKRig_NoChangeReply(Resp, TEXT("The IK Rig has no goal of that name; nothing changed and nothing was saved."));
    }

    if (!Controller->RemoveGoal(GoalFName))
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("RemoveGoal failed for goal '%s'"), *GoalName));

    FString SaveErr;
    const bool bSaved = mcp_common::SaveAssetPackage(Rig->GetOutermost(), Rig, SaveErr);

    Resp->SetBoolField(TEXT("removed"), true);
    Resp->SetBoolField(TEXT("already_absent"), false);
    Resp->SetNumberField(TEXT("goal_count"), Controller->GetAllGoals().Num());
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("remove_ik_goal"), SaveErr);
}

// ---------------------------------------------------------------------------
// connect_ik_goal_to_solver
// ---------------------------------------------------------------------------
// MCP:connect_ik_goal_to_solver - Connect an existing IK Rig goal to a solver, creating that solver's per-goal effector/settings, in one undo transaction, then save the asset.  This is the step that makes get_/set_ik_goal_settings work: a goal added via add_ik_goal is not attached to any solver until connected. Idempotent: re-connecting an already-connected goal is a success with already_connected:true, saved:false and a note, and nothing is saved, avoiding the duplicate the raw engine call would create. An unknown goal or out-of-range solver_index is an error. A failed save makes success false, with saved=false and save_error.
//   @param ik_rig_path - Content path to the IK Rig asset.
//   @param goal_name - Name of an existing goal (created via add_ik_goal).
//   @param solver_index - Index of the solver in the rig's solver stack.
//   @returns { ik_rig_path: string, goal_name: string, solver_index: int, connected: bool, already_connected: bool, saved: bool, save_error?: string, note?: string }

TSharedPtr<FJsonObject> HandleConnectIKGoalToSolver(const TSharedPtr<FJsonObject>& Params)
{
    FString RigPath, GoalName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ik_rig_path"), RigPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("goal_name"), GoalName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    int32 SolverIndex = 0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("solver_index"), SolverIndex)) { return Missing; }

    UIKRigDefinition* Rig = nullptr;
    UIKRigController* Controller = nullptr;
    if (TSharedPtr<FJsonObject> Error = IKRig_LoadRigController(RigPath, Rig, Controller)) { return Error; }

    if (TSharedPtr<FJsonObject> Error = IKRig_CheckSolverIndex(Controller, SolverIndex, /*bShowRange=*/true)) { return Error; }

    const FName GoalFName(*GoalName);
    if (!Controller->GetGoal(GoalFName))
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Goal '%s' does not exist in the IK Rig. Add it first with add_ik_goal."), *GoalName));

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("ik_rig_path"), Rig->GetPathName());
    Resp->SetStringField(TEXT("goal_name"), GoalName);
    Resp->SetNumberField(TEXT("solver_index"), SolverIndex);

    // Idempotent: the raw ConnectGoalToSolver calls AddGoal unconditionally (would
    // duplicate the effector), so short-circuit when already connected.
    if (Controller->IsGoalConnectedToSolver(GoalFName, SolverIndex))
    {
        Resp->SetBoolField(TEXT("connected"), true);
        Resp->SetBoolField(TEXT("already_connected"), true);
        return IKRig_NoChangeReply(Resp, TEXT("The goal is already connected to that solver; nothing changed and nothing was saved."));
    }

    {
        // ConnectGoalToSolver writes the solver stack without calling Modify(), so the rig is recorded here
        // to make the connection undoable, as disconnect_ik_goal_from_solver is.
        const FScopedTransaction Transaction(NSLOCTEXT("UEAgentIKRigCommands", "ConnectIKGoalToSolver", "Connect IK Goal To Solver"));
        Rig->Modify();
        if (!Controller->ConnectGoalToSolver(GoalFName, SolverIndex))
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("ConnectGoalToSolver failed for goal '%s' on solver %d."), *GoalName, SolverIndex));
    }

    FString SaveErr;
    const bool bSaved = mcp_common::SaveAssetPackage(Rig->GetOutermost(), Rig, SaveErr);

    Resp->SetBoolField(TEXT("connected"), true);
    Resp->SetBoolField(TEXT("already_connected"), false);
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("connect_ik_goal_to_solver"), SaveErr);
}

// ---------------------------------------------------------------------------
// disconnect_ik_goal_from_solver
// ---------------------------------------------------------------------------
// MCP:disconnect_ik_goal_from_solver - Disconnect a goal from a solver, removing that solver's per-goal effector/settings, through the IK Rig controller (one undo transaction), then save the asset.  Inverse of connect_ik_goal_to_solver. Idempotent: disconnecting a goal that isn't connected is a success with was_connected:false, saved:false and a note, and nothing is saved. An out-of-range solver_index is an error. A failed save makes success false, with saved=false and save_error.
//   @param ik_rig_path - Content path to the IK Rig asset.
//   @param goal_name - Name of the goal.
//   @param solver_index - Index of the solver in the rig's solver stack.
//   @returns { ik_rig_path: string, goal_name: string, solver_index: int, connected: bool, was_connected: bool, saved: bool, save_error?: string, note?: string }

TSharedPtr<FJsonObject> HandleDisconnectIKGoalFromSolver(const TSharedPtr<FJsonObject>& Params)
{
    FString RigPath, GoalName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ik_rig_path"), RigPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("goal_name"), GoalName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    int32 SolverIndex = 0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("solver_index"), SolverIndex)) { return Missing; }

    UIKRigDefinition* Rig = nullptr;
    UIKRigController* Controller = nullptr;
    if (TSharedPtr<FJsonObject> Error = IKRig_LoadRigController(RigPath, Rig, Controller)) { return Error; }

    if (TSharedPtr<FJsonObject> Error = IKRig_CheckSolverIndex(Controller, SolverIndex, /*bShowRange=*/true)) { return Error; }

    const FName GoalFName(*GoalName);
    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("ik_rig_path"), Rig->GetPathName());
    Resp->SetStringField(TEXT("goal_name"), GoalName);
    Resp->SetNumberField(TEXT("solver_index"), SolverIndex);

    if (!Controller->IsGoalConnectedToSolver(GoalFName, SolverIndex))
    {
        Resp->SetBoolField(TEXT("connected"), false);
        Resp->SetBoolField(TEXT("was_connected"), false);
        return IKRig_NoChangeReply(Resp, TEXT("The goal is not connected to that solver; nothing changed and nothing was saved."));
    }

    if (!Controller->DisconnectGoalFromSolver(GoalFName, SolverIndex))
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("DisconnectGoalFromSolver failed for goal '%s' on solver %d."), *GoalName, SolverIndex));

    const bool bConnectedNow = Controller->IsGoalConnectedToSolver(GoalFName, SolverIndex);

    FString SaveErr;
    const bool bSaved = mcp_common::SaveAssetPackage(Rig->GetOutermost(), Rig, SaveErr);

    Resp->SetBoolField(TEXT("connected"), bConnectedNow);
    Resp->SetBoolField(TEXT("was_connected"), true);
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("disconnect_ik_goal_from_solver"), SaveErr);
}

// ---------------------------------------------------------------------------
// add_ik_retarget_chain
// ---------------------------------------------------------------------------
// MCP:add_ik_retarget_chain - Add a retarget bone chain from start_bone to end_bone, optionally tied to an IK goal, to an IK Rig asset and save it. Both bones must exist in the skeleton of the rig; the chain name is made unique, so read chain_name back. A failed save makes success false, with saved=false and save_error.
//   @param ik_rig_path - IK Rig (UIKRigDefinition) asset path, e.g. "/Game/Retarget/IK_Hero".
//   @param chain_name - Name for the new retarget chain (uniquified by the engine)
//   @param start_bone - First bone of the chain; must exist in the skeleton of the rig.
//   @param end_bone - Last bone of the chain; must exist in the skeleton of the rig.
//   @param goal_name - IK goal to attach the chain to (default none); not checked against the goals of the rig.
//   @returns { asset_path: string, chain_name: string, start_bone: string, end_bone: string, goal_name: string, chain_count: int, saved: bool, save_error?: string }

TSharedPtr<FJsonObject> HandleAddIKRetargetChain(const TSharedPtr<FJsonObject>& Params)
{
    FString RigPath, ChainName, StartBone, EndBone;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ik_rig_path"), RigPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("chain_name"), ChainName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("start_bone"), StartBone, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("end_bone"), EndBone, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    FString GoalName;
    Params->TryGetStringField(TEXT("goal_name"), GoalName);

    UIKRigDefinition* Rig = nullptr;
    UIKRigController* Controller = nullptr;
    if (TSharedPtr<FJsonObject> Error = IKRig_LoadRigController(RigPath, Rig, Controller)) { return Error; }

    const FName ResultChain = Controller->AddRetargetChain(
        FName(*ChainName), FName(*StartBone), FName(*EndBone),
        GoalName.IsEmpty() ? NAME_None : FName(*GoalName));

    if (ResultChain.IsNone())
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("AddRetargetChain failed for chain '%s' (start='%s', end='%s')"),
                *ChainName, *StartBone, *EndBone));

    FString SaveErr;
    const bool bSaved = mcp_common::SaveAssetPackage(Rig->GetOutermost(), Rig, SaveErr);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("asset_path"), Rig->GetPathName());
    Resp->SetStringField(TEXT("chain_name"), ResultChain.ToString());
    Resp->SetStringField(TEXT("start_bone"), StartBone);
    Resp->SetStringField(TEXT("end_bone"), EndBone);
    Resp->SetStringField(TEXT("goal_name"), GoalName);
    Resp->SetNumberField(TEXT("chain_count"), Controller->GetRetargetChains().Num());
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("add_ik_retarget_chain"), SaveErr);
}

// ---------------------------------------------------------------------------
// get_ik_rig_info
// ---------------------------------------------------------------------------
// MCP:get_ik_rig_info - Read an IK Rig asset: its skeletal mesh, the number of solvers, and every goal and retarget chain, or with chain_name just that retarget chain (start_bone, end_bone and the IK goal it drives) plus whether the rig has it. Read-only; a chain_name the rig does not have is not an error but found=false and an empty chains list. Solvers are counted, not listed: get_ik_solver_details reads one by index.
//   @param ik_rig_path - Path to the UIKRigDefinition asset (an IK Rig, not an IK Retargeter)
//   @param chain_name - Optional. Name of one retarget chain, matched without case: chains then lists only that chain and found says whether the rig has it. retarget_chain_count still counts every chain.
//   @returns { asset_path: string, solver_count: int, goal_count: int, retarget_chain_count: int, skeletal_mesh: string, goals: [{ goal_name, bone_name }], chains: [{ chain_name, start_bone, end_bone, goal_name }], chain_name?: string, found?: bool }

TSharedPtr<FJsonObject> HandleGetIKRigInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString RigPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ik_rig_path"), RigPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString ChainFilter;
    const bool bFilterChain = Params->TryGetStringField(TEXT("chain_name"), ChainFilter);

    UIKRigDefinition* Rig = nullptr;
    UIKRigController* Controller = nullptr;
    if (TSharedPtr<FJsonObject> Error = IKRig_LoadRigController(RigPath, Rig, Controller)) { return Error; }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("asset_path"), Rig->GetPathName());
    FillRigInfo(Controller, Rig, Resp, /*bIncludeLists*/ true);

    if (bFilterChain)
    {
        // One chain: FName comparison ignores case, the way the controller looks chain names up.
        const FName WantedChain(*ChainFilter);
        TArray<TSharedPtr<FJsonValue>> ChainArr;
        for (const FBoneChain& Chain : Controller->GetRetargetChains())
        {
            if (Chain.ChainName != WantedChain)
            {
                continue;
            }
            TSharedPtr<FJsonObject> C = MakeShared<FJsonObject>();
            C->SetStringField(TEXT("chain_name"), Chain.ChainName.ToString());
            C->SetStringField(TEXT("start_bone"), Chain.StartBone.BoneName.ToString());
            C->SetStringField(TEXT("end_bone"), Chain.EndBone.BoneName.ToString());
            C->SetStringField(TEXT("goal_name"), Chain.IKGoalName.ToString());
            ChainArr.Add(MakeShared<FJsonValueObject>(C));
        }
        Resp->SetStringField(TEXT("chain_name"), ChainFilter);
        Resp->SetBoolField(TEXT("found"), ChainArr.Num() > 0);
        Resp->SetArrayField(TEXT("chains"), ChainArr);
    }
    return mcp_common::MakeOkReply(Resp);
}

// ---------------------------------------------------------------------------
// create_ik_retargeter
// ---------------------------------------------------------------------------
// MCP:create_ik_retargeter - Create and save a UIKRetargeter asset wiring source + target IK Rigs. An existing retargeter at that path is rewired and saved when either rig differs; when it already uses both rigs nothing changes, nothing is saved (saved=false) and a note says so. A failed save makes success false, with saved=false and save_error.
//   @param package_path - UE content folder, e.g. /Game/MyGame
//   @param name - Asset name for the new IK Retargeter
//   @param source_ik_rig_path - Path to the source UIKRigDefinition (copy FROM)
//   @param target_ik_rig_path - Path to the target UIKRigDefinition (copy TO)
//   @returns { asset_path: string, name: string, source_ik_rig: string, target_ik_rig: string, created: bool, already_existed: bool, saved: bool, save_error?: string, note?: string }

TSharedPtr<FJsonObject> HandleCreateIKRetargeter(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath, Name, SourcePath, TargetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("package_path"), PackagePath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("source_ik_rig_path"), SourcePath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("target_ik_rig_path"), TargetPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LongPackagePathError(PackagePath)) { return Error; }

    UIKRigDefinition* SourceRig = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadFlexibleOrError(SourcePath, TEXT("source IK Rig"), SourceRig)) { return Error; }
    UIKRigDefinition* TargetRig = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadFlexibleOrError(TargetPath, TEXT("target IK Rig"), TargetRig)) { return Error; }

    FString Folder, FullPath;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::JoinAssetPath(PackagePath, Name, Folder, FullPath)) { return Error; }

    bool bCreated = true;
    UIKRetargeter* Retargeter = nullptr;
    if (UEditorAssetLibrary::DoesAssetExist(FullPath))
    {
        Retargeter = LoadObject<UIKRetargeter>(nullptr, *FullPath);
        bCreated = false;
    }

    if (!Retargeter)
    {
        // Create via the non-modal UIKRetargetFactory through IAssetTools::CreateAsset.
        const FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
        UIKRetargetFactory* Factory = NewObject<UIKRetargetFactory>();
        UObject* NewAsset = AssetToolsModule.Get().CreateAsset(Name, Folder, UIKRetargeter::StaticClass(), Factory);
        Retargeter = Cast<UIKRetargeter>(NewAsset);
        bCreated = true;
    }
    if (!Retargeter)
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Failed to create IK Retargeter asset at %s"), *FullPath));

    UIKRetargeterController* Controller = UIKRetargeterController::GetController(Retargeter);
    if (!Controller)
        return mcp_common::MakeErrorReply(TEXT("Could not get IK Retargeter controller"));

    // An existing retargeter already wired to both rigs is left as it is: nothing to change, nothing to save.
    if (!bCreated
        && Controller->GetIKRig(ERetargetSourceOrTarget::Source) == SourceRig
        && Controller->GetIKRig(ERetargetSourceOrTarget::Target) == TargetRig)
    {
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetStringField(TEXT("asset_path"), Retargeter->GetPathName());
        Resp->SetStringField(TEXT("name"), Name);
        Resp->SetStringField(TEXT("source_ik_rig"), SourceRig->GetPathName());
        Resp->SetStringField(TEXT("target_ik_rig"), TargetRig->GetPathName());
        Resp->SetBoolField(TEXT("created"), false);
        Resp->SetBoolField(TEXT("already_existed"), true);
        return IKRig_NoChangeReply(Resp, TEXT("The IK Retargeter already exists with those source and target IK Rigs; nothing changed and nothing was saved."));
    }

    // Wire source + target IK Rigs.
    Controller->SetIKRig(ERetargetSourceOrTarget::Source, SourceRig);
    Controller->SetIKRig(ERetargetSourceOrTarget::Target, TargetRig);

    // Verify by reading back.
    const UIKRigDefinition* ReadSource = Controller->GetIKRig(ERetargetSourceOrTarget::Source);
    const UIKRigDefinition* ReadTarget = Controller->GetIKRig(ERetargetSourceOrTarget::Target);

    if (bCreated)
    {
        FAssetRegistryModule::AssetCreated(Retargeter);
    }
    FString SaveErr;
    const bool bSaved = mcp_common::SaveAssetPackage(Retargeter->GetOutermost(), Retargeter, SaveErr);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("asset_path"), Retargeter->GetPathName());
    Resp->SetStringField(TEXT("name"), Name);
    Resp->SetStringField(TEXT("source_ik_rig"), ReadSource ? ReadSource->GetPathName() : TEXT(""));
    Resp->SetStringField(TEXT("target_ik_rig"), ReadTarget ? ReadTarget->GetPathName() : TEXT(""));
    Resp->SetBoolField(TEXT("created"), bCreated);
    Resp->SetBoolField(TEXT("already_existed"), !bCreated);
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("create_ik_retargeter"), SaveErr);
}

// ===========================================================================
// IK Rig solver operations

// ---------------------------------------------------------------------------
// remove_ik_solver
// ---------------------------------------------------------------------------
// MCP:remove_ik_solver - Remove the solver at a 0-based index from the solver stack of an IK Rig asset through the IK Rig controller (one undo transaction), then save the asset. An out-of-range index is an error. A failed save makes success false, with saved=false and save_error.
//   @param ik_rig_path - Path to the UIKRigDefinition asset
//   @param solver_index - 0-based index of the solver to remove
//   @returns { asset_path: string, removed_solver_index: int, solver_count: int, removed: bool, saved: bool, save_error?: string }

TSharedPtr<FJsonObject> HandleRemoveIKSolver(const TSharedPtr<FJsonObject>& Params)
{
    FString RigPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ik_rig_path"), RigPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    int32 SolverIndex = 0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("solver_index"), SolverIndex)) { return Missing; }

    UIKRigDefinition* Rig = nullptr;
    UIKRigController* Controller = nullptr;
    if (TSharedPtr<FJsonObject> Error = IKRig_LoadRigController(RigPath, Rig, Controller)) { return Error; }

    if (TSharedPtr<FJsonObject> Error = IKRig_CheckSolverIndex(Controller, SolverIndex, /*bShowRange=*/true)) { return Error; }

    const bool bRemoved = Controller->RemoveSolver(SolverIndex);
    if (!bRemoved)
        return mcp_common::MakeErrorReply(TEXT("RemoveSolver returned false"));

    FString SaveErr;
    const bool bSaved = mcp_common::SaveAssetPackage(Rig->GetOutermost(), Rig, SaveErr);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("asset_path"), Rig->GetPathName());
    Resp->SetNumberField(TEXT("removed_solver_index"), SolverIndex);
    Resp->SetNumberField(TEXT("solver_count"), Controller->GetNumSolvers());
    Resp->SetBoolField(TEXT("removed"), true);
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("remove_ik_solver"), SaveErr);
}

// ---------------------------------------------------------------------------
// move_ik_solver_in_stack
// ---------------------------------------------------------------------------
// MCP:move_ik_solver_in_stack - Move the solver at from_index so that it ends up at to_index (both 0-based positions in the solver stack) of an IK Rig asset through the IK Rig controller (one undo transaction), then save the asset. An index outside the stack is an error and nothing moves; from_index equal to to_index is not an error but moved false, saved false and a note, with nothing saved. A failed save makes success false, with saved=false and save_error.
//   @param ik_rig_path - Path to the UIKRigDefinition asset
//   @param from_index - Source 0-based solver index
//   @param to_index - Target 0-based solver index, below the solver count
//   @returns { asset_path: string, from_index: int, to_index: int, moved: bool, solver_count: int, saved: bool, save_error?: string, note?: string }

TSharedPtr<FJsonObject> HandleMoveIKSolverInStack(const TSharedPtr<FJsonObject>& Params)
{
    FString RigPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ik_rig_path"), RigPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    int32 FromIndex = 0, ToIndex = 0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("from_index"), FromIndex)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("to_index"), ToIndex)) { return Missing; }

    UIKRigDefinition* Rig = nullptr;
    UIKRigController* Controller = nullptr;
    if (TSharedPtr<FJsonObject> Error = IKRig_LoadRigController(RigPath, Rig, Controller)) { return Error; }

    // MoveSolverInStack accepts a target one past the last solver, then removes the solver and inserts it
    // at that index of the now shorter stack, past its end: TArray::Insert does not range-check and the
    // relocation corrupts memory. Both indices must name a solver of the stack.
    const int32 NumSolvers = Controller->GetNumSolvers();
    if (FromIndex < 0 || FromIndex >= NumSolvers)
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("from_index %d out of range (0..%d)"), FromIndex, NumSolvers - 1));
    if (ToIndex < 0 || ToIndex >= NumSolvers)
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("to_index %d out of range (0..%d)"), ToIndex, NumSolvers - 1));

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("asset_path"), Rig->GetPathName());
    Resp->SetNumberField(TEXT("from_index"), FromIndex);
    Resp->SetNumberField(TEXT("to_index"), ToIndex);

    if (FromIndex == ToIndex)
    {
        // The solver is already at to_index (the engine refuses this move without touching the rig).
        Resp->SetBoolField(TEXT("moved"), false);
        Resp->SetNumberField(TEXT("solver_count"), NumSolvers);
        return IKRig_NoChangeReply(Resp, TEXT("from_index equals to_index; nothing moved and nothing was saved."));
    }

    if (!Controller->MoveSolverInStack(FromIndex, ToIndex))
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("MoveSolverInStack refused moving solver %d to %d (see the editor log)"), FromIndex, ToIndex));

    FString SaveErr;
    const bool bSaved = mcp_common::SaveAssetPackage(Rig->GetOutermost(), Rig, SaveErr);

    Resp->SetBoolField(TEXT("moved"), true);
    Resp->SetNumberField(TEXT("solver_count"), Controller->GetNumSolvers());
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("move_ik_solver_in_stack"), SaveErr);
}

// ---------------------------------------------------------------------------
// set_ik_solver_enabled
// ---------------------------------------------------------------------------
// MCP:set_ik_solver_enabled - Enable or disable the solver at a 0-based index of an IK Rig asset through the IK Rig controller (one undo transaction), then save the asset; enabled is read back. An out-of-range index is not an error but set_ok false, and a solver already in the requested state gives changed false: in both cases saved is false, a note says why and nothing is saved. A failed save makes success false, with saved=false and save_error.
//   @param ik_rig_path - Path to the UIKRigDefinition asset
//   @param solver_index - 0-based solver index
//   @param enabled - Optional bool; defaults to true
//   @returns { asset_path: string, solver_index: int, enabled: bool, set_ok: bool, changed: bool, saved: bool, save_error?: string, note?: string }

TSharedPtr<FJsonObject> HandleSetIKSolverEnabled(const TSharedPtr<FJsonObject>& Params)
{
    FString RigPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ik_rig_path"), RigPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    int32 SolverIndex = 0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("solver_index"), SolverIndex)) { return Missing; }
    bool bEnabled = true;
    Params->TryGetBoolField(TEXT("enabled"), bEnabled);

    UIKRigDefinition* Rig = nullptr;
    UIKRigController* Controller = nullptr;
    if (TSharedPtr<FJsonObject> Error = IKRig_LoadRigController(RigPath, Rig, Controller)) { return Error; }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("asset_path"), Rig->GetPathName());
    Resp->SetNumberField(TEXT("solver_index"), SolverIndex);

    // Neither an index outside the stack nor the state the solver already has changes anything; calling
    // SetSolverEnabled for the second would still Modify() and dirty the rig.
    if (SolverIndex < 0 || SolverIndex >= Controller->GetNumSolvers())
    {
        Resp->SetBoolField(TEXT("enabled"), false);
        Resp->SetBoolField(TEXT("set_ok"), false);
        Resp->SetBoolField(TEXT("changed"), false);
        return IKRig_NoChangeReply(Resp, FString::Printf(
            TEXT("solver_index %d is out of range (0..%d); nothing changed and nothing was saved."), SolverIndex, Controller->GetNumSolvers() - 1));
    }
    if (Controller->GetSolverEnabled(SolverIndex) == bEnabled)
    {
        Resp->SetBoolField(TEXT("enabled"), bEnabled);
        Resp->SetBoolField(TEXT("set_ok"), true);
        Resp->SetBoolField(TEXT("changed"), false);
        return IKRig_NoChangeReply(Resp, TEXT("The solver already has that enabled state; nothing changed and nothing was saved."));
    }

    const bool bSet = Controller->SetSolverEnabled(SolverIndex, bEnabled);
    // Read back to confirm (some APIs return false but succeed).
    const bool bActual = Controller->GetSolverEnabled(SolverIndex);

    FString SaveErr;
    const bool bSaved = mcp_common::SaveAssetPackage(Rig->GetOutermost(), Rig, SaveErr);

    Resp->SetBoolField(TEXT("enabled"), bActual);
    Resp->SetBoolField(TEXT("set_ok"), bSet);
    Resp->SetBoolField(TEXT("changed"), bActual == bEnabled);
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("set_ik_solver_enabled"), SaveErr);
}

// ---------------------------------------------------------------------------
// set_ik_solver_bones
// ---------------------------------------------------------------------------
// MCP:set_ik_solver_bones - Set the start bone and/or end bone of one solver in an IK Rig asset (0-based stack index) through the IK Rig controller, which records each change as an undo transaction, then save the asset. Pass at least one bone, else an error. An out-of-range index, a bone missing from the rig skeleton, or a start or end bone on a solver type without one is not an error: start_bone_set or end_bone_set comes back false, next to the bones read back, and nothing is stored for it. A bone the solver already has is left alone; when nothing changes, changed is false, saved is false, a note says why and nothing is saved. A failed save makes success false, with saved=false and save_error. get_ik_solver_details reads them back.
//   @param ik_rig_path - Path to the UIKRigDefinition asset
//   @param solver_index - 0-based solver index
//   @param start_bone - Optional start bone name
//   @param end_bone - Optional end bone name
//   @returns { asset_path: string, solver_index: int, start_bone: string, end_bone: string, start_bone_set: bool, end_bone_set: bool, changed: bool, saved: bool, save_error?: string, note?: string }

TSharedPtr<FJsonObject> HandleSetIKSolverBones(const TSharedPtr<FJsonObject>& Params)
{
    FString RigPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ik_rig_path"), RigPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    int32 SolverIndex = 0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("solver_index"), SolverIndex)) { return Missing; }

    FString StartBoneStr, EndBoneStr;
    Params->TryGetStringField(TEXT("start_bone"), StartBoneStr);
    Params->TryGetStringField(TEXT("end_bone"), EndBoneStr);

    if (StartBoneStr.IsEmpty() && EndBoneStr.IsEmpty())
        return mcp_common::MakeErrorReply(TEXT("Provide at least one of: start_bone, end_bone"));

    UIKRigDefinition* Rig = nullptr;
    UIKRigController* Controller = nullptr;
    if (TSharedPtr<FJsonObject> Error = IKRig_LoadRigController(RigPath, Rig, Controller)) { return Error; }

    // A bone the solver already has is not set again: the controller's setters Modify() the rig even then.
    const bool bValidIndex = SolverIndex >= 0 && SolverIndex < Controller->GetNumSolvers();
    const FName PrevStart = bValidIndex ? Controller->GetStartBone(SolverIndex) : NAME_None;
    const FName PrevEnd = bValidIndex ? Controller->GetEndBone(SolverIndex) : NAME_None;
    const FInstancedStruct* SolverStruct = bValidIndex ? Controller->GetSolverStructAtIndex(SolverIndex) : nullptr;
    const FIKRigSolverBase* Solver = (SolverStruct && SolverStruct->IsValid()) ? SolverStruct->GetPtr<FIKRigSolverBase>() : nullptr;

    bool bStartSet = true, bEndSet = true, bChanged = false;
    if (!StartBoneStr.IsEmpty())
    {
        const FName StartFName(*StartBoneStr);
        if (Solver && !Solver->UsesStartBone())
        {
            // SetStartBone calls Modify() and reports success for a solver type that has no start bone, but
            // the solver stores nothing: answered here as not set, with the rig untouched.
            bStartSet = false;
        }
        else if (!bValidIndex || StartFName.IsNone() || StartFName != PrevStart)
        {
            bStartSet = Controller->SetStartBone(StartFName, SolverIndex);
            bChanged |= bStartSet;
        }
    }
    if (!EndBoneStr.IsEmpty())
    {
        const FName EndFName(*EndBoneStr);
        if (!bValidIndex || EndFName.IsNone() || EndFName != PrevEnd)
        {
            bEndSet = Controller->SetEndBone(EndFName, SolverIndex);
            bChanged |= bEndSet;
        }
    }

    // Read back
    const FName ActualStart = Controller->GetStartBone(SolverIndex);
    const FName ActualEnd = Controller->GetEndBone(SolverIndex);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("asset_path"), Rig->GetPathName());
    Resp->SetNumberField(TEXT("solver_index"), SolverIndex);
    Resp->SetStringField(TEXT("start_bone"), ActualStart.ToString());
    Resp->SetStringField(TEXT("end_bone"), ActualEnd.ToString());
    Resp->SetBoolField(TEXT("start_bone_set"), bStartSet);
    Resp->SetBoolField(TEXT("end_bone_set"), bEndSet);
    Resp->SetBoolField(TEXT("changed"), bChanged);
    if (!bChanged)
    {
        return IKRig_NoChangeReply(Resp, TEXT("Nothing changed (the solver already had those bones, or the controller refused them: see start_bone_set and end_bone_set); nothing was saved."));
    }

    FString SaveErr;
    const bool bSaved = mcp_common::SaveAssetPackage(Rig->GetOutermost(), Rig, SaveErr);
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("set_ik_solver_bones"), SaveErr);
}

// ---------------------------------------------------------------------------
// get_ik_solver_details
// ---------------------------------------------------------------------------
// MCP:get_ik_solver_details - Read one solver of an IK Rig asset by its 0-based stack index: solver_type (the solver struct name, e.g. "IKRigFullBodyIKSolver") and solver_type_canonical (FullBodyIK, Limb, ...), unique_name ("<index+1> - <display name>"), enabled, start and end bone (end_bone "None" for a solver type without one), the names of the goals connected to it, and its solver-specific settings -- FullBodyIK with the curated snake_case keys set_ik_solver_settings takes, every other solver type serialized generically from its live settings struct (camelCase property names). Read-only; an out-of-range solver_index or a rig that does not load is an error. get_ik_rig_info reads the goals and chains of the whole rig.
//   @param ik_rig_path - Path to the UIKRigDefinition asset
//   @param solver_index - 0-based solver index
//   @returns { asset_path: string, solver_index: int, solver_type: string, solver_type_canonical: string, unique_name: string, enabled: bool, start_bone: string, end_bone: string, connected_goal_count: int, connected_goals: [string], settings: object }

TSharedPtr<FJsonObject> HandleGetIKSolverDetails(const TSharedPtr<FJsonObject>& Params)
{
    FString RigPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ik_rig_path"), RigPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    int32 SolverIndex = 0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("solver_index"), SolverIndex)) { return Missing; }

    UIKRigDefinition* Rig = nullptr;
    UIKRigController* Controller = nullptr;
    if (TSharedPtr<FJsonObject> Error = IKRig_LoadRigController(RigPath, Rig, Controller)) { return Error; }

    if (TSharedPtr<FJsonObject> Error = IKRig_CheckSolverIndex(Controller, SolverIndex, /*bShowRange=*/false)) { return Error; }

    // Get solver type name from the InstancedStruct
    FInstancedStruct* SolverStruct = Controller->GetSolverStructAtIndex(SolverIndex);
    FString SolverTypeName = TEXT("Unknown");
    if (SolverStruct && SolverStruct->IsValid())
    {
        SolverTypeName = SolverStruct->GetScriptStruct()->GetName();
    }

    const bool bEnabled = Controller->GetSolverEnabled(SolverIndex);
    const FName StartBone = Controller->GetStartBone(SolverIndex);
    const FName EndBone = Controller->GetEndBone(SolverIndex);
    const FString UniqueName = Controller->GetSolverUniqueName(SolverIndex);

    // Count goals connected to this solver
    int32 ConnectedGoals = 0;
    const TArray<UIKRigEffectorGoal*>& AllGoals = Controller->GetAllGoals();
    TArray<FString> ConnectedGoalNames;
    for (const UIKRigEffectorGoal* Goal : AllGoals)
    {
        if (Goal && Controller->IsGoalConnectedToSolver(Goal->GoalName, SolverIndex))
        {
            ++ConnectedGoals;
            ConnectedGoalNames.Add(Goal->GoalName.ToString());
        }
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("asset_path"), Rig->GetPathName());
    Resp->SetNumberField(TEXT("solver_index"), SolverIndex);
    Resp->SetStringField(TEXT("solver_type"), SolverTypeName);
    Resp->SetStringField(TEXT("unique_name"), UniqueName);
    Resp->SetBoolField(TEXT("enabled"), bEnabled);
    Resp->SetStringField(TEXT("start_bone"), StartBone.ToString());
    Resp->SetStringField(TEXT("end_bone"), EndBone.ToString());
    Resp->SetNumberField(TEXT("connected_goal_count"), ConnectedGoals);
    Resp->SetArrayField(TEXT("connected_goals"), mcp_common::StringsToJson(ConnectedGoalNames));

    // The solver's tunables: FBIK through its curated snake_case keys (the ones set_ik_solver_settings
    // takes), every other solver type serialized generically from its live settings struct.
    const FString SolverTypeCanonical = DetectSolverTypeCanonical(SolverStruct);
    TSharedPtr<FJsonObject> SettingsObj;
    if (SolverStruct && SolverStruct->IsValid() &&
        SolverStruct->GetScriptStruct() == FIKRigFullBodyIKSolver::StaticStruct())
    {
        SettingsObj = SerializeFBIKSolver(SolverStruct->GetMutable<FIKRigFullBodyIKSolver>());
    }
    else if (SolverStruct && SolverStruct->IsValid())
    {
        // Gate on the settings type first: the base GetSolverSettings() is checkNoEntry(), but any solver
        // that returns a non-null type also overrides GetSolverSettings().
        FIKRigSolverBase& Base = SolverStruct->GetMutable<FIKRigSolverBase>();
        const UScriptStruct* SettingsType = Base.GetSolverSettingsType();
        if (SettingsType)
        {
            SettingsObj = SerializeStructGeneric(SettingsType, Base.GetSolverSettings());
        }
        else
        {
            SettingsObj = MakeShared<FJsonObject>();
            SettingsObj->SetStringField(TEXT("note"),
                FString::Printf(TEXT("Solver type '%s' exposes no editable settings struct."), *SolverTypeCanonical));
        }
    }
    else
    {
        SettingsObj = MakeShared<FJsonObject>();
        SettingsObj->SetStringField(TEXT("note"), TEXT("Solver struct is invalid."));
    }
    Resp->SetStringField(TEXT("solver_type_canonical"), SolverTypeCanonical);
    Resp->SetObjectField(TEXT("settings"), SettingsObj);

    return mcp_common::MakeOkReply(Resp);
}

// ===========================================================================
// IK Rig retarget chain operations

// ---------------------------------------------------------------------------
// remove_ik_retarget_chain
// ---------------------------------------------------------------------------
// MCP:remove_ik_retarget_chain - Remove the retarget chain with that name from an IK Rig asset through the IK Rig controller (one undo transaction), then save the asset. A name no chain has is not an error but removed false, saved false and a note, with nothing saved. A failed save makes success false, with saved=false and save_error.
//   @param ik_rig_path - Path to the UIKRigDefinition asset
//   @param chain_name - Name of the retarget chain to remove
//   @returns { asset_path: string, chain_name: string, removed: bool, chain_count: int, saved: bool, save_error?: string, note?: string }

TSharedPtr<FJsonObject> HandleRemoveIKRetargetChain(const TSharedPtr<FJsonObject>& Params)
{
    FString RigPath, ChainName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ik_rig_path"), RigPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("chain_name"), ChainName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UIKRigDefinition* Rig = nullptr;
    UIKRigController* Controller = nullptr;
    if (TSharedPtr<FJsonObject> Error = IKRig_LoadRigController(RigPath, Rig, Controller)) { return Error; }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("asset_path"), Rig->GetPathName());
    Resp->SetStringField(TEXT("chain_name"), ChainName);

    // RemoveRetargetChain calls Modify() before it looks for the chain, so an absent name is answered here.
    const FName ChainFName(*ChainName);
    if (!IKRig_HasRetargetChain(Controller, ChainFName))
    {
        Resp->SetBoolField(TEXT("removed"), false);
        Resp->SetNumberField(TEXT("chain_count"), Controller->GetRetargetChains().Num());
        return IKRig_NoChangeReply(Resp, TEXT("The IK Rig has no retarget chain of that name; nothing changed and nothing was saved."));
    }

    const bool bRemoved = Controller->RemoveRetargetChain(ChainFName);

    FString SaveErr;
    const bool bSaved = mcp_common::SaveAssetPackage(Rig->GetOutermost(), Rig, SaveErr);

    Resp->SetBoolField(TEXT("removed"), bRemoved);
    Resp->SetNumberField(TEXT("chain_count"), Controller->GetRetargetChains().Num());
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("remove_ik_retarget_chain"), SaveErr);
}

// ---------------------------------------------------------------------------
// rename_ik_retarget_chain
// ---------------------------------------------------------------------------
// MCP:rename_ik_retarget_chain - Rename a retarget chain of an IK Rig asset through the IK Rig controller (one undo transaction) and save it. The engine makes the new name unique, so read new_name back; an unknown chain_name is not an error but returns renamed false, saved false and a note, with nothing saved. A failed save makes success false, with saved=false and save_error.
//   @param ik_rig_path - IK Rig (UIKRigDefinition) asset path, e.g. "/Game/Retarget/IK_Hero".
//   @param chain_name - Current name of the retarget chain, as get_ik_rig_info lists it.
//   @param new_name - Requested new name; the engine may add a suffix to keep it unique.
//   @returns { asset_path: string, old_name: string, new_name: string, renamed: bool, saved: bool, save_error?: string, note?: string }

TSharedPtr<FJsonObject> HandleRenameIKRetargetChain(const TSharedPtr<FJsonObject>& Params)
{
    FString RigPath, ChainName, NewName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ik_rig_path"), RigPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("chain_name"), ChainName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("new_name"), NewName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UIKRigDefinition* Rig = nullptr;
    UIKRigController* Controller = nullptr;
    if (TSharedPtr<FJsonObject> Error = IKRig_LoadRigController(RigPath, Rig, Controller)) { return Error; }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("asset_path"), Rig->GetPathName());
    Resp->SetStringField(TEXT("old_name"), ChainName);

    // The engine renames nothing for an unknown chain; it is answered here so the rig is not saved for it.
    const FName ChainFName(*ChainName);
    if (!IKRig_HasRetargetChain(Controller, ChainFName))
    {
        Resp->SetStringField(TEXT("new_name"), ChainName);
        Resp->SetBoolField(TEXT("renamed"), false);
        return IKRig_NoChangeReply(Resp, TEXT("The IK Rig has no retarget chain of that name; nothing was renamed and nothing was saved."));
    }

    const FName ResultName = Controller->RenameRetargetChain(ChainFName, FName(*NewName));

    FString SaveErr;
    const bool bSaved = mcp_common::SaveAssetPackage(Rig->GetOutermost(), Rig, SaveErr);

    Resp->SetStringField(TEXT("new_name"), ResultName.ToString());
    Resp->SetBoolField(TEXT("renamed"), !ResultName.IsNone() && ResultName != ChainFName);
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("rename_ik_retarget_chain"), SaveErr);
}

// ---------------------------------------------------------------------------
// set_ik_retarget_chain_bones
// ---------------------------------------------------------------------------
// MCP:set_ik_retarget_chain_bones - Change the start bone, end bone and/or IK goal of an existing retarget chain in an IK Rig asset through the IK Rig controller (each change an undo transaction), then save the asset. Pass at least one of the three, else an error. A missing chain, a bone not in the rig skeleton or a goal_name the rig has no goal for is not an error but start_bone_set, end_bone_set or goal_set false, and that field is left unchanged. A value the chain already has is left alone; when nothing changes, changed is false, saved is false, a note says why and nothing is saved. A failed save makes success false, with saved=false and save_error. Siblings: add_ik_retarget_chain, rename_ik_retarget_chain.
//   @param ik_rig_path - Path to the UIKRigDefinition asset
//   @param chain_name - Name of the retarget chain to mutate
//   @param start_bone - Optional new start bone
//   @param end_bone - Optional new end bone
//   @param goal_name - Optional new associated IK goal, which must be a goal of the rig; None clears the goal of the chain.
//   @returns { asset_path: string, chain_name: string, start_bone: string, end_bone: string, goal_name: string, start_bone_set: bool, end_bone_set: bool, goal_set: bool, changed: bool, saved: bool, save_error?: string, note?: string }

TSharedPtr<FJsonObject> HandleSetIKRetargetChainBones(const TSharedPtr<FJsonObject>& Params)
{
    FString RigPath, ChainName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ik_rig_path"), RigPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("chain_name"), ChainName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    FString StartBone, EndBone, GoalName;
    Params->TryGetStringField(TEXT("start_bone"), StartBone);
    Params->TryGetStringField(TEXT("end_bone"), EndBone);
    Params->TryGetStringField(TEXT("goal_name"), GoalName);

    if (StartBone.IsEmpty() && EndBone.IsEmpty() && GoalName.IsEmpty())
        return mcp_common::MakeErrorReply(TEXT("Provide at least one of: start_bone, end_bone, goal_name"));

    UIKRigDefinition* Rig = nullptr;
    UIKRigController* Controller = nullptr;
    if (TSharedPtr<FJsonObject> Error = IKRig_LoadRigController(RigPath, Rig, Controller)) { return Error; }

    // A value the chain already has is not set again: the controller's setters Modify() the rig even then.
    const FName ChainFName(*ChainName);
    const bool bChainExists = IKRig_HasRetargetChain(Controller, ChainFName);
    const FName PrevStart = bChainExists ? Controller->GetRetargetChainStartBone(ChainFName) : NAME_None;
    const FName PrevEnd = bChainExists ? Controller->GetRetargetChainEndBone(ChainFName) : NAME_None;
    const FName PrevGoal = bChainExists ? Controller->GetRetargetChainGoal(ChainFName) : NAME_None;

    bool bStartOk = true, bEndOk = true, bGoalOk = true, bChanged = false;
    if (!StartBone.IsEmpty())
    {
        const FName StartFName(*StartBone);
        if (!bChainExists || StartFName.IsNone() || StartFName != PrevStart)
        {
            bStartOk = Controller->SetRetargetChainStartBone(ChainFName, StartFName);
            bChanged |= bStartOk;
        }
    }
    if (!EndBone.IsEmpty())
    {
        const FName EndFName(*EndBone);
        if (!bChainExists || EndFName.IsNone() || EndFName != PrevEnd)
        {
            bEndOk = Controller->SetRetargetChainEndBone(ChainFName, EndFName);
            bChanged |= bEndOk;
        }
    }
    if (!GoalName.IsEmpty())
    {
        // SetRetargetChainGoal stores None for a name the rig has no goal for and still returns true, so an
        // unknown goal is refused here; the literal None is the one way to clear the chain's goal on purpose.
        const FName GoalFName(*GoalName);
        bGoalOk = GoalFName.IsNone() || Controller->GetGoal(GoalFName) != nullptr;
        if (bGoalOk && (!bChainExists || GoalFName != PrevGoal))
        {
            bGoalOk = Controller->SetRetargetChainGoal(ChainFName, GoalFName);
            bChanged |= bGoalOk;
        }
    }

    // Read back
    const FName ActualStart = Controller->GetRetargetChainStartBone(ChainFName);
    const FName ActualEnd = Controller->GetRetargetChainEndBone(ChainFName);
    const FName ActualGoal = Controller->GetRetargetChainGoal(ChainFName);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("asset_path"), Rig->GetPathName());
    Resp->SetStringField(TEXT("chain_name"), ChainName);
    Resp->SetStringField(TEXT("start_bone"), ActualStart.ToString());
    Resp->SetStringField(TEXT("end_bone"), ActualEnd.ToString());
    Resp->SetStringField(TEXT("goal_name"), ActualGoal.ToString());
    Resp->SetBoolField(TEXT("start_bone_set"), bStartOk);
    Resp->SetBoolField(TEXT("end_bone_set"), bEndOk);
    Resp->SetBoolField(TEXT("goal_set"), bGoalOk);
    Resp->SetBoolField(TEXT("changed"), bChanged);
    if (!bChanged)
    {
        return IKRig_NoChangeReply(Resp, TEXT("Nothing changed (the chain already had those values, or they were refused: see start_bone_set, end_bone_set and goal_set); nothing was saved."));
    }

    FString SaveErr;
    const bool bSaved = mcp_common::SaveAssetPackage(Rig->GetOutermost(), Rig, SaveErr);
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("set_ik_retarget_chain_bones"), SaveErr);
}

// ===========================================================================
// IK Retargeter operations

// ---------------------------------------------------------------------------
// add_retargeter_op
// ---------------------------------------------------------------------------
// MCP:add_retargeter_op - Add a retarget op to the op stack of an IK Retargeter asset through its controller (one undo transaction), then save the asset; op_index and op_name are where and how the engine placed it. Default type: FK Chains op. An unknown op type, or a singleton op type the stack already has, is an error. A failed save makes success false, with saved=false and save_error.
//   @param ik_retargeter_path - Path to the UIKRetargeter asset
//   @param op_type - Optional op class path (default "/Script/IKRig.IKRetargetFKChainsOp")
//   @returns { asset_path: string, op_index: int, op_name: string, op_type: string, op_count: int, saved: bool, save_error?: string }

TSharedPtr<FJsonObject> HandleAddRetargeterOp(const TSharedPtr<FJsonObject>& Params)
{
    FString RetargeterPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ik_retargeter_path"), RetargeterPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    // Default op type — the most common retarget op
    FString OpType = TEXT("/Script/IKRig.IKRetargetFKChainsOp");
    Params->TryGetStringField(TEXT("op_type"), OpType);

    UIKRetargeter* Retargeter = nullptr;
    UIKRetargeterController* Controller = nullptr;
    if (TSharedPtr<FJsonObject> Error = IKRig_LoadRetargeterController(RetargeterPath, Retargeter, Controller)) { return Error; }

    const int32 OpIndex = Controller->AddRetargetOp(OpType);
    if (OpIndex == INDEX_NONE)
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("AddRetargetOp failed for type '%s'"), *OpType));

    const FName OpName = Controller->GetOpName(OpIndex);

    FString SaveErr;
    const bool bSaved = mcp_common::SaveAssetPackage(Retargeter->GetOutermost(), Retargeter, SaveErr);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("asset_path"), Retargeter->GetPathName());
    Resp->SetNumberField(TEXT("op_index"), OpIndex);
    Resp->SetStringField(TEXT("op_name"), OpName.ToString());
    Resp->SetStringField(TEXT("op_type"), OpType);
    Resp->SetNumberField(TEXT("op_count"), Controller->GetNumRetargetOps());
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("add_retargeter_op"), SaveErr);
}

// ---------------------------------------------------------------------------
// remove_retargeter_op
// ---------------------------------------------------------------------------
// MCP:remove_retargeter_op - Remove the op at a 0-based index, with any child ops, from the op stack of an IK Retargeter asset through its controller (one undo transaction), then save the asset. An out-of-range index is an error. A failed save makes success false, with saved=false and save_error.
//   @param ik_retargeter_path - Path to the UIKRetargeter asset
//   @param op_index - 0-based op index
//   @returns { asset_path: string, removed_op_index: int, removed: bool, op_count: int, saved: bool, save_error?: string }

TSharedPtr<FJsonObject> HandleRemoveRetargeterOp(const TSharedPtr<FJsonObject>& Params)
{
    FString RetargeterPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ik_retargeter_path"), RetargeterPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    int32 OpIndex = 0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("op_index"), OpIndex)) { return Missing; }

    UIKRetargeter* Retargeter = nullptr;
    UIKRetargeterController* Controller = nullptr;
    if (TSharedPtr<FJsonObject> Error = IKRig_LoadRetargeterController(RetargeterPath, Retargeter, Controller)) { return Error; }

    if (OpIndex < 0 || OpIndex >= Controller->GetNumRetargetOps())
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("op_index %d out of range (0..%d)"), OpIndex, Controller->GetNumRetargetOps() - 1));

    const bool bRemoved = Controller->RemoveRetargetOp(OpIndex);

    FString SaveErr;
    const bool bSaved = mcp_common::SaveAssetPackage(Retargeter->GetOutermost(), Retargeter, SaveErr);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("asset_path"), Retargeter->GetPathName());
    Resp->SetNumberField(TEXT("removed_op_index"), OpIndex);
    Resp->SetBoolField(TEXT("removed"), bRemoved);
    Resp->SetNumberField(TEXT("op_count"), Controller->GetNumRetargetOps());
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("remove_retargeter_op"), SaveErr);
}

// ---------------------------------------------------------------------------
// set_retargeter_chain_mapping
// ---------------------------------------------------------------------------
// MCP:set_retargeter_chain_mapping - Make source_chain drive target_chain in an IK Retargeter asset, overwriting the previous mapping, on every op whose chain mapping lists target_chain (or only the op named op_name), in one undo transaction, then save the asset. source_chain is not validated, so a misspelled name is stored as given. set_ok is false when no op lists target_chain; mapped_source is read back from the first matching op. When no op lists target_chain, or every one already maps it to source_chain, changed is false, saved is false, a note says why and nothing is saved. A failed save makes success false, with saved=false and save_error. get_ik_retargeter_info shows the per-op mappings.
//   @param ik_retargeter_path - Path to the UIKRetargeter asset
//   @param source_chain - Name of the source retarget chain
//   @param target_chain - Name of the target retarget chain to map to
//   @param op_name - Optional op name as get_ik_retargeter_info lists it; omitted, every op whose chain mapping lists target_chain is updated
//   @returns { asset_path: string, source_chain: string, target_chain: string, mapped_source: string, set_ok: bool, changed: bool, saved: bool, save_error?: string, note?: string }

TSharedPtr<FJsonObject> HandleSetRetargeterChainMapping(const TSharedPtr<FJsonObject>& Params)
{
    FString RetargeterPath, SourceChain, TargetChain;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ik_retargeter_path"), RetargeterPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("source_chain"), SourceChain, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("target_chain"), TargetChain, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    FString OpName;
    Params->TryGetStringField(TEXT("op_name"), OpName);

    UIKRetargeter* Retargeter = nullptr;
    UIKRetargeterController* Controller = nullptr;
    if (TSharedPtr<FJsonObject> Error = IKRig_LoadRetargeterController(RetargeterPath, Retargeter, Controller)) { return Error; }

    const FName SourceFName(*SourceChain);
    const FName TargetFName(*TargetChain);
    const FName OpFName = OpName.IsEmpty() ? NAME_None : FName(*OpName);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("asset_path"), Retargeter->GetPathName());
    Resp->SetStringField(TEXT("source_chain"), SourceChain);
    Resp->SetStringField(TEXT("target_chain"), TargetChain);

    // SetSourceChain calls Modify() before it looks at the ops, so the ops it would update are found here
    // first, the same way it walks them: no op listing target_chain, or every such op already mapping it to
    // source_chain, changes nothing.
    bool bAnyOpListsTarget = false;
    bool bAllAlreadyMapped = true;
    for (int32 OpIndex = 0; OpIndex < Controller->GetNumRetargetOps(); ++OpIndex)
    {
        const FIKRetargetOpBase* Op = Controller->GetRetargetOpByIndex(OpIndex);
        if (!Op || (!OpFName.IsNone() && Op->GetName() != OpFName))
        {
            continue;
        }
        const FRetargetChainMapping* Mapping = Op->GetChainMapping();
        if (!Mapping || !Mapping->HasChain(TargetFName, ERetargetSourceOrTarget::Target))
        {
            continue;
        }
        bAnyOpListsTarget = true;
        bAllAlreadyMapped &= Mapping->GetChainMappedTo(TargetFName, ERetargetSourceOrTarget::Target) == SourceFName;
    }
    if (!bAnyOpListsTarget || bAllAlreadyMapped)
    {
        Resp->SetStringField(TEXT("mapped_source"), Controller->GetSourceChain(TargetFName, OpFName).ToString());
        Resp->SetBoolField(TEXT("set_ok"), bAnyOpListsTarget);
        Resp->SetBoolField(TEXT("changed"), false);
        return IKRig_NoChangeReply(Resp, bAnyOpListsTarget
            ? TEXT("target_chain is already mapped to source_chain; nothing changed and nothing was saved.")
            : TEXT("No op lists target_chain in its chain mapping; nothing changed and nothing was saved."));
    }

    // SetSourceChain(InSourceChainName, InTargetChainName, InOpName)
    const bool bSet = Controller->SetSourceChain(SourceFName, TargetFName, OpFName);

    // Read back
    const FName ActualSource = Controller->GetSourceChain(TargetFName, OpFName);

    FString SaveErr;
    const bool bSaved = mcp_common::SaveAssetPackage(Retargeter->GetOutermost(), Retargeter, SaveErr);

    Resp->SetStringField(TEXT("mapped_source"), ActualSource.ToString());
    Resp->SetBoolField(TEXT("set_ok"), bSet);
    Resp->SetBoolField(TEXT("changed"), bSet);
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("set_retargeter_chain_mapping"), SaveErr);
}

// ---------------------------------------------------------------------------
// set_retarget_pose_bone_rotation
// ---------------------------------------------------------------------------
// MCP:set_retarget_pose_bone_rotation - Set a bone's rotation offset in the current retarget pose (euler degrees) of an IK Retargeter asset, as one undo transaction, then save the asset; the offset is read back as quat_x..quat_w. Creates a default pose if none is current. bone_name is stored as given, not checked against the skeleton. When the pose to edit is already current and already holds exactly that offset for the bone, changed is false, saved is false, a note says so and nothing is saved. A failed save makes success false, with saved=false and save_error.
//   @param ik_retargeter_path - Path to the UIKRetargeter asset
//   @param bone_name - Name of the bone to update
//   @param source_or_target - Optional "Source" or "Target" scope; defaults to "Target"
//   @param pitch - Euler pitch in degrees (used when no quat is provided)
//   @param yaw - Euler yaw in degrees
//   @param roll - Euler roll in degrees
//   @param quat_x - Optional quaternion X
//   @param quat_y - Optional quaternion Y
//   @param quat_z - Optional quaternion Z
//   @param quat_w - Optional quaternion W
//   @param rotation - Optional nested {x, y, z, w} quaternion object
//   @param pose_name - Optional explicit retarget pose name; created if missing
//   @returns { asset_path: string, bone_name: string, source_or_target: string, retarget_pose: string, quat_x: number, quat_y: number, quat_z: number, quat_w: number, changed: bool, saved: bool, save_error?: string, note?: string }

TSharedPtr<FJsonObject> HandleSetRetargetPoseBoneRotation(const TSharedPtr<FJsonObject>& Params)
{
    FString RetargeterPath, BoneName, SourceOrTargetStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ik_retargeter_path"), RetargeterPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("bone_name"), BoneName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    const ERetargetSourceOrTarget Mode = IKRig_ReadSourceOrTarget(Params, SourceOrTargetStr);

    // Parse rotation as euler angles (degrees) — x=pitch, y=yaw, z=roll
    double Pitch = 0.0, Yaw = 0.0, Roll = 0.0;
    Params->TryGetNumberField(TEXT("pitch"), Pitch);
    Params->TryGetNumberField(TEXT("yaw"), Yaw);
    Params->TryGetNumberField(TEXT("roll"), Roll);

    // Also support a quaternion if provided directly (flat or nested "rotation" object)
    double QX = 0.0, QY = 0.0, QZ = 0.0, QW = 1.0;
    bool bHasQuat = Params->TryGetNumberField(TEXT("quat_x"), QX);
    if (bHasQuat) Params->TryGetNumberField(TEXT("quat_y"), QY);
    if (bHasQuat) Params->TryGetNumberField(TEXT("quat_z"), QZ);
    if (bHasQuat) Params->TryGetNumberField(TEXT("quat_w"), QW);

    // Also accept rotation as a nested object { x, y, z, w }
    if (!bHasQuat)
    {
        const TSharedPtr<FJsonObject>* RotObj = nullptr;
        if (Params->TryGetObjectField(TEXT("rotation"), RotObj) && *RotObj)
        {
            bHasQuat = (*RotObj)->TryGetNumberField(TEXT("x"), QX);
            if (bHasQuat) (*RotObj)->TryGetNumberField(TEXT("y"), QY);
            if (bHasQuat) (*RotObj)->TryGetNumberField(TEXT("z"), QZ);
            if (bHasQuat) (*RotObj)->TryGetNumberField(TEXT("w"), QW);
        }
    }

    FQuat Rotation;
    if (bHasQuat)
    {
        Rotation = FQuat(QX, QY, QZ, QW);
    }
    else
    {
        // Convert euler degrees to quaternion
        const FRotator RotDeg(Pitch, Yaw, Roll);
        Rotation = RotDeg.Quaternion();
    }

    // Optional explicit pose_name
    FString PoseNameStr;
    Params->TryGetStringField(TEXT("pose_name"), PoseNameStr);

    UIKRetargeter* Retargeter = nullptr;
    UIKRetargeterController* Controller = nullptr;
    if (TSharedPtr<FJsonObject> Error = IKRig_LoadRetargeterController(RetargeterPath, Retargeter, Controller)) { return Error; }

    // Nothing changes when the pose to edit is already the current one and already holds exactly this offset
    // for the bone; answered before the transaction below, whose Modify() would dirty the asset.
    {
        const FName CurrentPose = Controller->GetCurrentRetargetPoseName(Mode);
        const FName PoseToEdit = PoseNameStr.IsEmpty() ? CurrentPose : FName(*PoseNameStr);
        const FIKRetargetPose* Pose = (!PoseToEdit.IsNone() && PoseToEdit == CurrentPose)
            ? Controller->GetRetargetPoses(Mode).Find(PoseToEdit) : nullptr;
        const FQuat* Existing = Pose ? Pose->GetAllDeltaRotations().Find(FName(*BoneName)) : nullptr;
        if (Existing && *Existing == Rotation)
        {
            TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
            Resp->SetStringField(TEXT("asset_path"), Retargeter->GetPathName());
            Resp->SetStringField(TEXT("bone_name"), BoneName);
            Resp->SetStringField(TEXT("source_or_target"), SourceOrTargetStr);
            Resp->SetStringField(TEXT("retarget_pose"), CurrentPose.ToString());
            Resp->SetNumberField(TEXT("quat_x"), Existing->X);
            Resp->SetNumberField(TEXT("quat_y"), Existing->Y);
            Resp->SetNumberField(TEXT("quat_z"), Existing->Z);
            Resp->SetNumberField(TEXT("quat_w"), Existing->W);
            Resp->SetBoolField(TEXT("changed"), false);
            return IKRig_NoChangeReply(Resp, TEXT("The bone already has that rotation offset in the current retarget pose; nothing changed and nothing was saved."));
        }
    }

    // One undo step for the whole call, the pose the controller may create included:
    // SetRotationOffsetForRetargetPoseBone neither opens a transaction nor calls Modify.
    const FScopedTransaction Transaction(NSLOCTEXT("UEAgentIKRigCommands", "SetRetargetPoseBoneRotation", "Set Retarget Pose Bone Rotation"));
    Retargeter->Modify();

    // Determine which retarget pose to operate on
    FName CurrentPoseName;
    if (!PoseNameStr.IsEmpty())
    {
        // Use the explicitly named pose; create it if it doesn't exist yet
        CurrentPoseName = FName(*PoseNameStr);
        // Try to set it as current; if it doesn't exist, create it first
        Controller->SetCurrentRetargetPose(CurrentPoseName, Mode);
        FName Verify = Controller->GetCurrentRetargetPoseName(Mode);
        if (Verify.IsNone() || Verify != CurrentPoseName)
        {
            CurrentPoseName = Controller->CreateRetargetPose(CurrentPoseName, Mode);
            if (!CurrentPoseName.IsNone())
            {
                Controller->SetCurrentRetargetPose(CurrentPoseName, Mode);
            }
        }
    }
    else
    {
        // Legacy path: ensure a current retarget pose exists — create one if needed
        CurrentPoseName = Controller->GetCurrentRetargetPoseName(Mode);
        if (CurrentPoseName.IsNone())
        {
            CurrentPoseName = Controller->CreateRetargetPose(FName(TEXT("Default")), Mode);
            if (!CurrentPoseName.IsNone())
            {
                Controller->SetCurrentRetargetPose(CurrentPoseName, Mode);
            }
        }
    }

    // Set the rotation offset
    Controller->SetRotationOffsetForRetargetPoseBone(FName(*BoneName), Rotation, Mode);

    // Read back
    const FQuat ActualRot = Controller->GetRotationOffsetForRetargetPoseBone(FName(*BoneName), Mode);

    FString SaveErr;
    const bool bSaved = mcp_common::SaveAssetPackage(Retargeter->GetOutermost(), Retargeter, SaveErr);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("asset_path"), Retargeter->GetPathName());
    Resp->SetStringField(TEXT("bone_name"), BoneName);
    Resp->SetStringField(TEXT("source_or_target"), SourceOrTargetStr);
    Resp->SetStringField(TEXT("retarget_pose"), CurrentPoseName.ToString());
    Resp->SetNumberField(TEXT("quat_x"), ActualRot.X);
    Resp->SetNumberField(TEXT("quat_y"), ActualRot.Y);
    Resp->SetNumberField(TEXT("quat_z"), ActualRot.Z);
    Resp->SetNumberField(TEXT("quat_w"), ActualRot.W);
    Resp->SetBoolField(TEXT("changed"), true);
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("set_retarget_pose_bone_rotation"), SaveErr);
}

// ===========================================================================
// IK Rig solver settings (detailed per-solver-type get/set)

// ---------------------------------------------------------------------------
// set_ik_solver_settings
// ---------------------------------------------------------------------------
// MCP:set_ik_solver_settings - Set detailed solver-specific settings on a solver in an IK Rig, as one undo transaction, then save the asset.  Partial update: only fields present in ``settings`` are applied (idempotent). FullBodyIK uses snake_case keys: root_bone, iterations (0-1000), sub_iterations (0-1000), mass_multiplier (0-10), allow_stretch (bool), root_behavior (enum string), global_pull_chain_alpha (0-1), max_angle (0-45), over_relaxation (1-2). Other solver types (Limb/Pole/BodyMover/SetTransform/StretchLimb) accept the camelCase names of their live settings struct; ClampMin/ClampMax bounds are enforced server-side. Settings that already have those values give changed false, saved false and a note, with nothing saved. A failed save makes success false, with saved=false and save_error.
//   @param ik_rig_path - Content path to the IK Rig asset.
//   @param solver_index - Index of the solver in the rig's solver stack.
//   @param settings - Object with solver-type-specific fields to set.
//   @returns { ik_rig_path: string, solver_index: int, solver_type: string, success: bool, changed: bool, saved: bool, save_error?: string, note?: string }

TSharedPtr<FJsonObject> HandleSetIKSolverSettings(const TSharedPtr<FJsonObject>& Params)
{
    FString RigPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ik_rig_path"), RigPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    int32 SolverIndex = 0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("solver_index"), SolverIndex)) { return Missing; }

    const TSharedPtr<FJsonObject>* SettingsObj = nullptr;
    if (!Params->TryGetObjectField(TEXT("settings"), SettingsObj) || !*SettingsObj)
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: settings (object)"));

    UIKRigDefinition* Rig = nullptr;
    UIKRigController* Controller = nullptr;
    if (TSharedPtr<FJsonObject> Error = IKRig_LoadRigController(RigPath, Rig, Controller)) { return Error; }

    if (TSharedPtr<FJsonObject> Error = IKRig_CheckSolverIndex(Controller, SolverIndex, /*bShowRange=*/true)) { return Error; }

    FInstancedStruct* SolverStruct = Controller->GetSolverStructAtIndex(SolverIndex);
    if (!SolverStruct || !SolverStruct->IsValid())
        return mcp_common::MakeErrorReply(TEXT("Solver struct is invalid"));

    const FString SolverType = DetectSolverTypeCanonical(SolverStruct);

    // The live settings struct and how the JSON applies to it: FBIK takes its curated snake_case keys,
    // every other solver a partial merge of the JSON onto its settings struct followed by the
    // ClampMin/ClampMax pass. Only fields present in the JSON are written, so re-running is idempotent.
    FIKRigSolverBase& Base = SolverStruct->GetMutable<FIKRigSolverBase>();
    const UScriptStruct* SettingsType = Base.GetSolverSettingsType();
    FIKRigSolverSettingsBase* Live = SettingsType ? Base.GetSolverSettings() : nullptr;
    if (!SettingsType || !Live)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("set_ik_solver_settings: solver type '%s' exposes no editable settings struct."), *SolverType));
    }
    const bool bFBIK = SolverStruct->GetScriptStruct() == FIKRigFullBodyIKSolver::StaticStruct();
    const auto ApplySettings = [&](void* Settings)
    {
        if (bFBIK)
        {
            ApplyFBIKSolverJson(*static_cast<FIKRigFBIKSettings*>(Settings), *SettingsObj);
        }
        else
        {
            MergeJsonIntoStructGeneric(SettingsType, Settings, *SettingsObj);
        }
    };

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("ik_rig_path"), Rig->GetPathName());
    Resp->SetNumberField(TEXT("solver_index"), SolverIndex);
    Resp->SetStringField(TEXT("solver_type"), SolverType);

    if (!IKRig_WouldChangeStruct(SettingsType, Live, ApplySettings))
    {
        Resp->SetBoolField(TEXT("success"), true);
        Resp->SetBoolField(TEXT("changed"), false);
        return IKRig_NoChangeReply(Resp, TEXT("The solver settings already have those values; nothing changed and nothing was saved."));
    }

    {
        // The settings are written straight into the solver stack, so the write runs in a transaction
        // after Modify() to be undoable like the controller's own edits.
        const FScopedTransaction Transaction(NSLOCTEXT("UEAgentIKRigCommands", "SetIKSolverSettings", "Set IK Solver Settings"));
        Rig->Modify();
        ApplySettings(Live);
    }

    FString SaveErr;
    const bool bSaved = mcp_common::SaveAssetPackage(Rig->GetOutermost(), Rig, SaveErr);

    Resp->SetBoolField(TEXT("success"), bSaved);
    Resp->SetBoolField(TEXT("changed"), true);
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("set_ik_solver_settings"), SaveErr);
}

// ---------------------------------------------------------------------------
// get_ik_goal_settings
// ---------------------------------------------------------------------------
// MCP:get_ik_goal_settings - Get goal-specific settings for a goal connected to a solver.  FullBodyIK returns snake_case keys (chain_depth, strength_alpha, pull_chain_alpha, pin_rotation); BodyMover (the only other solver with per-goal settings) is serialized generically (camelCase: influenceMultiplier, boneName). The goal must be connected to the solver first (connect_ik_goal_to_solver), else a `note` explains that. Solvers without per-goal settings return a `note`.
//   @param ik_rig_path - Content path to the IK Rig asset.
//   @param solver_index - Index of the solver.
//   @param goal_name - Name of the goal to query settings for.
//   @returns { ik_rig_path: string, solver_index: int, goal_name: string, settings: object }

TSharedPtr<FJsonObject> HandleGetIKGoalSettings(const TSharedPtr<FJsonObject>& Params)
{
    FString RigPath, GoalName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ik_rig_path"), RigPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("goal_name"), GoalName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    int32 SolverIndex = 0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("solver_index"), SolverIndex)) { return Missing; }

    UIKRigDefinition* Rig = nullptr;
    UIKRigController* Controller = nullptr;
    if (TSharedPtr<FJsonObject> Error = IKRig_LoadRigController(RigPath, Rig, Controller)) { return Error; }

    if (TSharedPtr<FJsonObject> Error = IKRig_CheckSolverIndex(Controller, SolverIndex, /*bShowRange=*/false)) { return Error; }

    FInstancedStruct* SolverStruct = Controller->GetSolverStructAtIndex(SolverIndex);
    const FString SolverType = DetectSolverTypeCanonical(SolverStruct);
    TSharedPtr<FJsonObject> SettingsObj;

    if (SolverStruct && SolverStruct->IsValid())
    {
        FIKRigSolverBase& Base = SolverStruct->GetMutable<FIKRigSolverBase>();
        if (SolverStruct->GetScriptStruct() == FIKRigFullBodyIKSolver::StaticStruct())
        {
            // FBIK: curated snake_case keys.
            if (FIKRigGoalSettingsBase* GoalBase = Base.GetGoalSettings(FName(*GoalName)))
                SettingsObj = SerializeFBIKGoal(*static_cast<FIKRigFBIKGoalSettings*>(GoalBase));
        }
        else if (Base.UsesCustomGoalSettings())
        {
            // Non-FBIK with custom per-goal settings (BodyMover): serialize generically.
            FIKRigGoalSettingsBase* GoalBase = Base.GetGoalSettings(FName(*GoalName));
            const UScriptStruct* GoalType = Base.GetGoalSettingsType();
            if (GoalBase && GoalType)
                SettingsObj = SerializeStructGeneric(GoalType, GoalBase);
        }
        else
        {
            // Honest: this solver type genuinely has no per-goal settings.
            SettingsObj = MakeShared<FJsonObject>();
            SettingsObj->SetStringField(TEXT("note"),
                FString::Printf(TEXT("Solver type '%s' has no per-goal settings."), *SolverType));
        }
    }

    if (!SettingsObj)
    {
        // FBIK/BodyMover solver, but this goal isn't connected to it yet.
        SettingsObj = MakeShared<FJsonObject>();
        SettingsObj->SetStringField(TEXT("note"),
            FString::Printf(TEXT("Goal '%s' is not connected to solver %d. Use connect_ik_goal_to_solver first."),
                            *GoalName, SolverIndex));
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("ik_rig_path"), Rig->GetPathName());
    Resp->SetNumberField(TEXT("solver_index"), SolverIndex);
    Resp->SetStringField(TEXT("goal_name"), GoalName);
    Resp->SetObjectField(TEXT("settings"), SettingsObj);
    return mcp_common::MakeOkReply(Resp);
}

// ---------------------------------------------------------------------------
// set_ik_goal_settings
// ---------------------------------------------------------------------------
// MCP:set_ik_goal_settings - Set goal-specific settings for a goal connected to a solver, as one undo transaction, then save the IK Rig asset.  Partial update (idempotent): only fields present in ``settings`` are applied. FullBodyIK uses snake_case keys: bone_name, chain_depth (0+), strength_alpha (0-1), pull_chain_alpha (0-1), pin_rotation (0-1). BodyMover uses its live goal struct's camelCase keys (influenceMultiplier 0-10, clamped server-side). Connect the goal first with connect_ik_goal_to_solver. Solvers without per-goal settings return an honest error. Settings that already have those values give changed false, saved false and a note, with nothing saved. A failed save makes success false, with saved=false and save_error.
//   @param ik_rig_path - Content path to the IK Rig asset.
//   @param solver_index - Index of the solver.
//   @param goal_name - Name of the goal to modify.
//   @param settings - Object with goal-specific fields to set.
//   @returns { ik_rig_path: string, solver_index: int, goal_name: string, success: bool, changed: bool, saved: bool, save_error?: string, note?: string }

TSharedPtr<FJsonObject> HandleSetIKGoalSettings(const TSharedPtr<FJsonObject>& Params)
{
    FString RigPath, GoalName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ik_rig_path"), RigPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("goal_name"), GoalName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    int32 SolverIndex = 0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("solver_index"), SolverIndex)) { return Missing; }

    const TSharedPtr<FJsonObject>* SettingsObj = nullptr;
    if (!Params->TryGetObjectField(TEXT("settings"), SettingsObj) || !*SettingsObj)
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: settings (object)"));

    UIKRigDefinition* Rig = nullptr;
    UIKRigController* Controller = nullptr;
    if (TSharedPtr<FJsonObject> Error = IKRig_LoadRigController(RigPath, Rig, Controller)) { return Error; }

    if (TSharedPtr<FJsonObject> Error = IKRig_CheckSolverIndex(Controller, SolverIndex, /*bShowRange=*/false)) { return Error; }

    FInstancedStruct* SolverStruct = Controller->GetSolverStructAtIndex(SolverIndex);
    const FString SolverType = DetectSolverTypeCanonical(SolverStruct);
    if (!SolverStruct || !SolverStruct->IsValid())
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("set_ik_goal_settings: could not apply for solver type '%s'."), *SolverType));
    }

    // FBIK takes its curated snake_case keys; the other solver with per-goal settings (BodyMover) a partial
    // merge + clamp of its live goal struct.
    FIKRigSolverBase& Base = SolverStruct->GetMutable<FIKRigSolverBase>();
    const bool bFBIK = SolverStruct->GetScriptStruct() == FIKRigFullBodyIKSolver::StaticStruct();
    if (!bFBIK && !Base.UsesCustomGoalSettings())
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("set_ik_goal_settings: solver type '%s' has no per-goal settings."), *SolverType));
    }
    const FName GoalFName(*GoalName);
    FIKRigGoalSettingsBase* GoalBase = Base.GetGoalSettings(GoalFName);
    const UScriptStruct* GoalType = Base.GetGoalSettingsType();
    if (!GoalBase || !GoalType)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Goal '%s' is not connected to solver %d. Use connect_ik_goal_to_solver first."), *GoalName, SolverIndex));
    }
    const auto ApplySettings = [&](void* Settings)
    {
        if (bFBIK)
        {
            ApplyFBIKGoalJson(*static_cast<FIKRigFBIKGoalSettings*>(Settings), *SettingsObj);
        }
        else
        {
            MergeJsonIntoStructGeneric(GoalType, Settings, *SettingsObj);
        }
    };

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("ik_rig_path"), Rig->GetPathName());
    Resp->SetNumberField(TEXT("solver_index"), SolverIndex);
    Resp->SetStringField(TEXT("goal_name"), GoalName);

    if (!IKRig_WouldChangeStruct(GoalType, GoalBase, ApplySettings))
    {
        Resp->SetBoolField(TEXT("success"), true);
        Resp->SetBoolField(TEXT("changed"), false);
        return IKRig_NoChangeReply(Resp, TEXT("The goal settings already have those values; nothing changed and nothing was saved."));
    }

    {
        // Written straight into the solver stack: Modify() inside a transaction makes it undoable.
        const FScopedTransaction Transaction(NSLOCTEXT("UEAgentIKRigCommands", "SetIKGoalSettings", "Set IK Goal Settings"));
        Rig->Modify();
        ApplySettings(GoalBase);
        if (bFBIK)
        {
            Base.SetGoalSettings(GoalFName, GoalBase);
        }
    }

    FString SaveErr;
    const bool bSaved = mcp_common::SaveAssetPackage(Rig->GetOutermost(), Rig, SaveErr);

    Resp->SetBoolField(TEXT("success"), bSaved);
    Resp->SetBoolField(TEXT("changed"), true);
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("set_ik_goal_settings"), SaveErr);
}

// ---------------------------------------------------------------------------
// get_ik_bone_settings
// ---------------------------------------------------------------------------
// MCP:get_ik_bone_settings - Get bone-specific settings for a bone within a solver.  FullBodyIK returns snake_case keys (rotation/position stiffness, axis limits Free/Limited/Locked, min/max angles, preferred angles); StretchLimb (the only other solver with per-bone settings) is serialized generically (camelCase: squashDirection vector). Only FullBodyIK and StretchLimb support per-bone settings; others return a `note`.
//   @param ik_rig_path - Content path to the IK Rig asset.
//   @param solver_index - Index of the solver.
//   @param bone_name - Name of the bone to query settings for.
//   @returns { ik_rig_path: string, solver_index: int, bone_name: string, settings: object }

TSharedPtr<FJsonObject> HandleGetIKBoneSettings(const TSharedPtr<FJsonObject>& Params)
{
    FString RigPath, BoneName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ik_rig_path"), RigPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("bone_name"), BoneName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    int32 SolverIndex = 0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("solver_index"), SolverIndex)) { return Missing; }

    UIKRigDefinition* Rig = nullptr;
    UIKRigController* Controller = nullptr;
    if (TSharedPtr<FJsonObject> Error = IKRig_LoadRigController(RigPath, Rig, Controller)) { return Error; }

    if (TSharedPtr<FJsonObject> Error = IKRig_CheckSolverIndex(Controller, SolverIndex, /*bShowRange=*/false)) { return Error; }

    FInstancedStruct* SolverStruct = Controller->GetSolverStructAtIndex(SolverIndex);
    const FString SolverType = DetectSolverTypeCanonical(SolverStruct);
    TSharedPtr<FJsonObject> SettingsObj;

    // Bone settings are fetched solver-agnostically via the controller; only the
    // serialization differs by solver type.
    FIKRigBoneSettingsBase* BoneBase = Controller->GetSettingsForBone(FName(*BoneName), SolverIndex);
    if (BoneBase && SolverStruct && SolverStruct->IsValid())
    {
        if (SolverStruct->GetScriptStruct() == FIKRigFullBodyIKSolver::StaticStruct())
        {
            SettingsObj = SerializeFBIKBone(*static_cast<FIKRigFBIKBoneSettings*>(BoneBase));
        }
        else
        {
            // Non-FBIK with custom per-bone settings (StretchLimb): serialize generically.
            FIKRigSolverBase& Base = SolverStruct->GetMutable<FIKRigSolverBase>();
            if (const UScriptStruct* BoneType = Base.GetBoneSettingsType())
                SettingsObj = SerializeStructGeneric(BoneType, BoneBase);
        }
    }

    if (!SettingsObj)
    {
        SettingsObj = MakeShared<FJsonObject>();
        SettingsObj->SetStringField(TEXT("note"), !BoneBase
            ? FString::Printf(TEXT("No bone settings for bone '%s' on solver %d. Create them via set_ik_bone_settings "
                                   "(only FullBodyIK and StretchLimb support per-bone settings)."), *BoneName, SolverIndex)
            : FString::Printf(TEXT("Solver type '%s' exposes no bone-settings struct."), *SolverType));
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("ik_rig_path"), Rig->GetPathName());
    Resp->SetNumberField(TEXT("solver_index"), SolverIndex);
    Resp->SetStringField(TEXT("bone_name"), BoneName);
    Resp->SetObjectField(TEXT("settings"), SettingsObj);
    return mcp_common::MakeOkReply(Resp);
}

// ---------------------------------------------------------------------------
// set_ik_bone_settings
// ---------------------------------------------------------------------------
// MCP:set_ik_bone_settings - Set bone-specific settings for a bone within a solver, as one undo transaction, then save the IK Rig asset; a failed save makes success false, with saved=false and save_error.  Partial update (idempotent): only fields present in ``settings`` are applied; bone settings are auto-created if absent (a bone the rig skeleton lacks is an error, with nothing changed), and existing settings that already have those values give changed false, saved false and a note, with nothing saved. FullBodyIK snake_case keys: rotation_stiffness (0-1), position_stiffness (0-1), limit_x/limit_y/limit_z ("Free"/"Limited"/"Locked"), min_x, max_x, min_y, max_y, min_z, max_z, use_preferred_angles (bool), preferred_x, preferred_y, preferred_z. StretchLimb uses its live bone struct's camelCase keys (squashDirection: {x,y,z}). Only FullBodyIK and StretchLimb support per-bone settings; others return an honest error.
//   @param ik_rig_path - Content path to the IK Rig asset.
//   @param solver_index - Index of the solver.
//   @param bone_name - Name of the bone to modify.
//   @param settings - Object with bone-specific fields to set.
//   @returns { ik_rig_path: string, solver_index: int, bone_name: string, success: bool, changed: bool, saved: bool, save_error?: string, note?: string }

TSharedPtr<FJsonObject> HandleSetIKBoneSettings(const TSharedPtr<FJsonObject>& Params)
{
    FString RigPath, BoneName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ik_rig_path"), RigPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("bone_name"), BoneName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    int32 SolverIndex = 0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("solver_index"), SolverIndex)) { return Missing; }

    const TSharedPtr<FJsonObject>* SettingsObj = nullptr;
    if (!Params->TryGetObjectField(TEXT("settings"), SettingsObj) || !*SettingsObj)
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: settings (object)"));

    UIKRigDefinition* Rig = nullptr;
    UIKRigController* Controller = nullptr;
    if (TSharedPtr<FJsonObject> Error = IKRig_LoadRigController(RigPath, Rig, Controller)) { return Error; }

    if (TSharedPtr<FJsonObject> Error = IKRig_CheckSolverIndex(Controller, SolverIndex, /*bShowRange=*/false)) { return Error; }

    FInstancedStruct* SolverStruct = Controller->GetSolverStructAtIndex(SolverIndex);
    const FString SolverType = DetectSolverTypeCanonical(SolverStruct);
    const FName BoneFName(*BoneName);
    const auto CannotApplyReply = [&]()
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("set_ik_bone_settings: could not add/apply bone settings for bone '%s' on solver %d."), *BoneName, SolverIndex));
    };

    FIKRigSolverBase* Base = (SolverStruct && SolverStruct->IsValid())
        ? &SolverStruct->GetMutable<FIKRigSolverBase>() : nullptr;
    if (!Base)
    {
        return CannotApplyReply();
    }
    if (!Base->UsesCustomBoneSettings())
    {
        // Limb/Pole/BodyMover/SetTransform have no per-bone settings.
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("set_ik_bone_settings: solver type '%s' has no per-bone settings (only FullBodyIK and StretchLimb do)."), *SolverType));
    }
    const UScriptStruct* BoneType = Base->GetBoneSettingsType();
    if (!BoneType)
    {
        return CannotApplyReply();
    }

    // Settings the bone does not have yet are added by the controller; the prerequisites are checked
    // before the transaction below, so a refusal leaves the rig untouched.
    FIKRigBoneSettingsBase* BoneBase = Controller->GetSettingsForBone(BoneFName, SolverIndex);
    if (!BoneBase)
    {
        FText Reason;
        if (!Controller->CanAddBoneSetting(BoneFName, SolverIndex, &Reason))
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("set_ik_bone_settings: could not add bone settings for bone '%s' on solver %d: %s"),
                *BoneName, SolverIndex, *Reason.ToString()));
        }
    }

    // FBIK takes its curated snake_case keys; StretchLimb a partial merge + clamp of its live bone struct.
    const bool bFBIK = SolverStruct->GetScriptStruct() == FIKRigFullBodyIKSolver::StaticStruct();
    const auto ApplySettings = [&](void* Settings)
    {
        if (bFBIK)
        {
            ApplyFBIKBoneJson(*static_cast<FIKRigFBIKBoneSettings*>(Settings), *SettingsObj);
        }
        else
        {
            MergeJsonIntoStructGeneric(BoneType, Settings, *SettingsObj);
        }
    };

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("ik_rig_path"), Rig->GetPathName());
    Resp->SetNumberField(TEXT("solver_index"), SolverIndex);
    Resp->SetStringField(TEXT("bone_name"), BoneName);

    if (BoneBase && !IKRig_WouldChangeStruct(BoneType, BoneBase, ApplySettings))
    {
        Resp->SetBoolField(TEXT("success"), true);
        Resp->SetBoolField(TEXT("changed"), false);
        return IKRig_NoChangeReply(Resp, TEXT("The bone settings already have those values; nothing changed and nothing was saved."));
    }

    {
        // One undo step for the bone settings the controller may add and the fields written straight
        // into them, which need Modify() inside a transaction to be undoable.
        const FScopedTransaction Transaction(NSLOCTEXT("UEAgentIKRigCommands", "SetIKBoneSettings", "Set IK Bone Settings"));
        Rig->Modify();
        if (!BoneBase)
        {
            Controller->AddBoneSetting(BoneFName, SolverIndex);
            BoneBase = Controller->GetSettingsForBone(BoneFName, SolverIndex);
        }
        if (!BoneBase)
        {
            return CannotApplyReply();
        }
        ApplySettings(BoneBase);
    }

    FString SaveErr;
    const bool bSaved = mcp_common::SaveAssetPackage(Rig->GetOutermost(), Rig, SaveErr);

    Resp->SetBoolField(TEXT("success"), bSaved);
    Resp->SetBoolField(TEXT("changed"), true);
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("set_ik_bone_settings"), SaveErr);
}

// ---------------------------------------------------------------------------
// create_retarget_pose
// ---------------------------------------------------------------------------
// MCP:create_retarget_pose - Create a new retarget pose for editing in an IK Retargeter through its controller, then save the asset.  The engine makes the name unique, so read pose_name back. The pose is set as the current pose after creation, ready for bone rotation edits via set_retarget_pose_bone_rotation. A failed save makes success false, with saved=false and save_error.
//   @param ik_retargeter_path - Content path to the IK Retargeter asset.
//   @param pose_name - Name for the new retarget pose
//   @param source_or_target - Optional "Source" or "Target" scope; defaults to "Target"
//   @returns { ik_retargeter_path: string, pose_name: string, source_or_target: string, success: bool, saved: bool, save_error?: string }

TSharedPtr<FJsonObject> HandleCreateRetargetPose(const TSharedPtr<FJsonObject>& Params)
{
    FString RetargeterPath, PoseName, SourceOrTargetStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ik_retargeter_path"), RetargeterPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("pose_name"), PoseName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    const ERetargetSourceOrTarget Mode = IKRig_ReadSourceOrTarget(Params, SourceOrTargetStr);

    UIKRetargeter* Retargeter = nullptr;
    UIKRetargeterController* Controller = nullptr;
    if (TSharedPtr<FJsonObject> Error = IKRig_LoadRetargeterController(RetargeterPath, Retargeter, Controller)) { return Error; }

    const FName ResultPose = Controller->CreateRetargetPose(FName(*PoseName), Mode);
    if (ResultPose.IsNone())
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("CreateRetargetPose failed for '%s'"), *PoseName));

    // Set as current pose
    Controller->SetCurrentRetargetPose(ResultPose, Mode);

    FString SaveErr;
    const bool bSaved = mcp_common::SaveAssetPackage(Retargeter->GetOutermost(), Retargeter, SaveErr);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("ik_retargeter_path"), Retargeter->GetPathName());
    Resp->SetStringField(TEXT("pose_name"), ResultPose.ToString());
    Resp->SetStringField(TEXT("source_or_target"), SourceOrTargetStr);
    Resp->SetBoolField(TEXT("success"), bSaved);
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("create_retarget_pose"), SaveErr);
}

// ---------------------------------------------------------------------------
// remove_retarget_pose
// ---------------------------------------------------------------------------
// MCP:remove_retarget_pose - Remove a retarget pose from an IK Retargeter by name through its controller, then save the asset.  Inverse of create_retarget_pose. If the removed pose was the current one, the retargeter falls back to the default pose. Idempotent: removing a pose that isn't present is a success with removed:false, already_absent:true, saved:false and a note, and nothing is saved. The engine's built-in default pose cannot be removed (honest error). A failed save makes success false, with saved=false and save_error.
//   @param ik_retargeter_path - Content path to the IK Retargeter asset.
//   @param pose_name - Name of the retarget pose to remove
//   @param source_or_target - Optional "Source" or "Target" scope; defaults to "Target"
//   @returns { ik_retargeter_path: string, pose_name: string, source_or_target: string, removed: bool, already_absent: bool, success: bool, saved: bool, save_error?: string, note?: string }

TSharedPtr<FJsonObject> HandleRemoveRetargetPose(const TSharedPtr<FJsonObject>& Params)
{
    FString RetargeterPath, PoseName, SourceOrTargetStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ik_retargeter_path"), RetargeterPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("pose_name"), PoseName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    const ERetargetSourceOrTarget Mode = IKRig_ReadSourceOrTarget(Params, SourceOrTargetStr);

    UIKRetargeter* Retargeter = nullptr;
    UIKRetargeterController* Controller = nullptr;
    if (TSharedPtr<FJsonObject> Error = IKRig_LoadRetargeterController(RetargeterPath, Retargeter, Controller)) { return Error; }

    // RemoveRetargetPose returns false in two distinct cases: the default pose (a hard
    // rejection) and a pose that doesn't exist (idempotent no-op). Disambiguate up front
    // so each maps to an honest response instead of a bare false.
    const FName PoseFName(*PoseName);
    if (PoseFName == UIKRetargeter::GetDefaultPoseName())
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Cannot remove the default retarget pose '%s'"), *PoseName));

    const bool bExisted = Controller->GetRetargetPoses(Mode).Contains(PoseFName);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("ik_retargeter_path"), Retargeter->GetPathName());
    Resp->SetStringField(TEXT("pose_name"), PoseName);
    Resp->SetStringField(TEXT("source_or_target"), SourceOrTargetStr);
    if (!bExisted)
    {
        Resp->SetBoolField(TEXT("removed"), false);
        Resp->SetBoolField(TEXT("already_absent"), true);
        Resp->SetBoolField(TEXT("success"), true);
        return IKRig_NoChangeReply(Resp, TEXT("The IK Retargeter has no retarget pose of that name on that side; nothing changed and nothing was saved."));
    }

    if (!Controller->RemoveRetargetPose(PoseFName, Mode))
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("RemoveRetargetPose failed for '%s'"), *PoseName));

    FString SaveErr;
    const bool bSaved = mcp_common::SaveAssetPackage(Retargeter->GetOutermost(), Retargeter, SaveErr);

    Resp->SetBoolField(TEXT("removed"), true);
    Resp->SetBoolField(TEXT("already_absent"), false);
    Resp->SetBoolField(TEXT("success"), bSaved);
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("remove_retarget_pose"), SaveErr);
}

// ===========================================================================
// IK Retargeter read-only inspectors (global on the UIKRetargeter)

// ---------------------------------------------------------------------------
// get_ik_retargeter_info
// ---------------------------------------------------------------------------
// MCP:get_ik_retargeter_info - Read the global structure of an IK Retargeter via its controller: source/target IK Rigs, the op-stack, per-op chain mappings, and the current poses.  Read-only. Chain mappings are per-op in 5.6+ (there is no global RootSettings on the UIKRetargeter in 5.8; detailed root/global settings live inside the ops themselves).
//   @param ik_retargeter_path - Content path to the UIKRetargeter asset.
//   @returns { asset_path: string, source_ik_rig: string, target_ik_rig: string, op_count: int, ops: [{ op_index, op_name, enabled, parent_op, op_type }], chain_mappings: [{ op_name, target_chain, source_chain }], source: { current_pose, pelvis_bone, root_offset: {x,y,z} }, target: { current_pose, pelvis_bone, root_offset: {x,y,z} } }
TSharedPtr<FJsonObject> HandleGetIKRetargeterInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString RetargeterPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ik_retargeter_path"), RetargeterPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UIKRetargeter* Retargeter = nullptr;
    UIKRetargeterController* Controller = nullptr;
    if (TSharedPtr<FJsonObject> Error = IKRig_LoadRetargeterController(RetargeterPath, Retargeter, Controller)) { return Error; }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("asset_path"), Retargeter->GetPathName());

    // Source/target IK Rigs.
    const UIKRigDefinition* SourceRig = Controller->GetIKRig(ERetargetSourceOrTarget::Source);
    const UIKRigDefinition* TargetRig = Controller->GetIKRig(ERetargetSourceOrTarget::Target);
    Resp->SetStringField(TEXT("source_ik_rig"), SourceRig ? SourceRig->GetPathName() : TEXT(""));
    Resp->SetStringField(TEXT("target_ik_rig"), TargetRig ? TargetRig->GetPathName() : TEXT(""));

    // Op-stack: one entry per retarget op, with the live struct type name.
    const int32 NumOps = Controller->GetNumRetargetOps();
    Resp->SetNumberField(TEXT("op_count"), NumOps);

    TArray<FName> OpNames;
    OpNames.Reserve(NumOps);
    TArray<TSharedPtr<FJsonValue>> OpsArr;
    for (int32 i = 0; i < NumOps; ++i)
    {
        const FName OpName = Controller->GetOpName(i);
        OpNames.Add(OpName);

        TSharedPtr<FJsonObject> Op = MakeShared<FJsonObject>();
        Op->SetNumberField(TEXT("op_index"), i);
        Op->SetStringField(TEXT("op_name"), OpName.ToString());
        Op->SetBoolField(TEXT("enabled"), Controller->GetRetargetOpEnabled(i));
        Op->SetStringField(TEXT("parent_op"), Controller->GetParentOpByName(OpName).ToString());

        // GetRetargetOpStructAtIndex returns a nullable FInstancedStruct*; an empty
        // struct's GetScriptStruct() is also null — null-check both (honest).
        FString OpType = TEXT("");
        if (const FInstancedStruct* OpStruct = Controller->GetRetargetOpStructAtIndex(i))
        {
            if (const UScriptStruct* ScriptStruct = OpStruct->GetScriptStruct())
            {
                OpType = ScriptStruct->GetName();
            }
        }
        Op->SetStringField(TEXT("op_type"), OpType);
        OpsArr.Add(MakeShared<FJsonValueObject>(Op));
    }
    Resp->SetArrayField(TEXT("ops"), OpsArr);

    // Chain mappings (per-op in 5.6+): derive the target chains from the target IK
    // Rig and, for each op, look up the mapped source chain. Only emit pairs that
    // are actually mapped (source chain resolves) — no invented entries.
    TArray<TSharedPtr<FJsonValue>> MappingArr;
    if (TargetRig)
    {
        if (UIKRigController* TargetRigController = UIKRigController::GetController(TargetRig))
        {
            const TArray<FBoneChain>& TargetChains = TargetRigController->GetRetargetChains();
            for (const FName& OpName : OpNames)
            {
                for (const FBoneChain& Chain : TargetChains)
                {
                    const FName SourceChain = Controller->GetSourceChain(Chain.ChainName, OpName);
                    if (SourceChain.IsNone())
                    {
                        continue;
                    }
                    TSharedPtr<FJsonObject> M = MakeShared<FJsonObject>();
                    M->SetStringField(TEXT("op_name"), OpName.ToString());
                    M->SetStringField(TEXT("target_chain"), Chain.ChainName.ToString());
                    M->SetStringField(TEXT("source_chain"), SourceChain.ToString());
                    MappingArr.Add(MakeShared<FJsonValueObject>(M));
                }
            }
        }
    }
    Resp->SetArrayField(TEXT("chain_mappings"), MappingArr);

    // Per-side pose summary: current pose name, pelvis bone, and the root offset of
    // the current retarget pose. (Detailed root/global settings live in the ops.)
    auto BuildSide = [Controller](ERetargetSourceOrTarget Mode) -> TSharedPtr<FJsonObject>
    {
        TSharedPtr<FJsonObject> Side = MakeShared<FJsonObject>();
        Side->SetStringField(TEXT("current_pose"), Controller->GetCurrentRetargetPoseName(Mode).ToString());
        Side->SetStringField(TEXT("pelvis_bone"), Controller->GetPelvisBone(Mode).ToString());

        const FVector RootOffset = Controller->GetRootOffsetInRetargetPose(Mode);
        Side->SetObjectField(TEXT("root_offset"), mcp_common::VectorToJsonObject(RootOffset));
        return Side;
    };
    Resp->SetObjectField(TEXT("source"), BuildSide(ERetargetSourceOrTarget::Source));
    Resp->SetObjectField(TEXT("target"), BuildSide(ERetargetSourceOrTarget::Target));

    return mcp_common::MakeOkReply(Resp);
}

// ---------------------------------------------------------------------------
// get_retarget_poses
// ---------------------------------------------------------------------------
// MCP:get_retarget_poses - List every retarget pose (source and target) of an IK Retargeter with its root translation delta and per-bone rotation deltas.  Read-only. Bone rotation deltas use the same quat keys as set_retarget_pose_bone_rotation (quat_x, quat_y, quat_z, quat_w).
//   @param ik_retargeter_path - Content path to the UIKRetargeter asset.
//   @returns { asset_path: string, source: [{ name, is_current, root_translation_offset: {x,y,z}, bone_rotations: [{ bone, quat_x, quat_y, quat_z, quat_w }] }], target: [ ... same shape ... ] }
TSharedPtr<FJsonObject> HandleGetRetargetPoses(const TSharedPtr<FJsonObject>& Params)
{
    FString RetargeterPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ik_retargeter_path"), RetargeterPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UIKRetargeter* Retargeter = nullptr;
    UIKRetargeterController* Controller = nullptr;
    if (TSharedPtr<FJsonObject> Error = IKRig_LoadRetargeterController(RetargeterPath, Retargeter, Controller)) { return Error; }

    // Serialize all poses of one side. GetRetargetPoses returns the live TMap by ref;
    // we only read it. GetAllDeltaRotations()/GetRootTranslationDelta() are the public
    // read-only getters of FIKRetargetPose (its delta fields are private).
    auto BuildSide = [Controller](ERetargetSourceOrTarget Mode) -> TArray<TSharedPtr<FJsonValue>>
    {
        TArray<TSharedPtr<FJsonValue>> PoseArr;
        const FName CurrentPose = Controller->GetCurrentRetargetPoseName(Mode);
        const TMap<FName, FIKRetargetPose>& Poses = Controller->GetRetargetPoses(Mode);
        for (const TPair<FName, FIKRetargetPose>& Pair : Poses)
        {
            const FName PoseName = Pair.Key;
            const FIKRetargetPose& Pose = Pair.Value;

            TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
            P->SetStringField(TEXT("name"), PoseName.ToString());
            P->SetBoolField(TEXT("is_current"), PoseName == CurrentPose);

            const FVector RootDelta = Pose.GetRootTranslationDelta();
            P->SetObjectField(TEXT("root_translation_offset"), mcp_common::VectorToJsonObject(RootDelta));

            TArray<TSharedPtr<FJsonValue>> RotArr;
            for (const TPair<FName, FQuat>& Delta : Pose.GetAllDeltaRotations())
            {
                TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
                R->SetStringField(TEXT("bone"), Delta.Key.ToString());
                R->SetNumberField(TEXT("quat_x"), Delta.Value.X);
                R->SetNumberField(TEXT("quat_y"), Delta.Value.Y);
                R->SetNumberField(TEXT("quat_z"), Delta.Value.Z);
                R->SetNumberField(TEXT("quat_w"), Delta.Value.W);
                RotArr.Add(MakeShared<FJsonValueObject>(R));
            }
            P->SetArrayField(TEXT("bone_rotations"), RotArr);
            PoseArr.Add(MakeShared<FJsonValueObject>(P));
        }
        return PoseArr;
    };

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("asset_path"), Retargeter->GetPathName());
    Resp->SetArrayField(TEXT("source"), BuildSide(ERetargetSourceOrTarget::Source));
    Resp->SetArrayField(TEXT("target"), BuildSide(ERetargetSourceOrTarget::Target));
    return mcp_common::MakeOkReply(Resp);
}

// ===========================================================================
// IK Retargeter per-op settings (read + partial-merge write)

// ---------------------------------------------------------------------------
// get_retargeter_op_info
// ---------------------------------------------------------------------------
// MCP:get_retargeter_op_info - Read one retarget op's live settings struct from an IK Retargeter by op index.  Read-only (no save). The settings keys are the op settings UPROPERTY names with only the first character lower-cased (FJsonObjectConverter convention), e.g. FK Chains op exposes lODThreshold, bEnabled, bDebugDraw, chainDrawSize, bDrawChainLines. op_type is the settings struct name (e.g. IKRetargetFKChainsOpSettings), which differs from get_ik_retargeter_info.ops[].op_type (the op struct name).
//   @param ik_retargeter_path - Content path to the UIKRetargeter asset.
//   @param op_index [int!] - 0-based index of the op in the retargeter's op stack.
//   @returns { asset_path: string, op_index: int, op_name: string, op_type: string, enabled: bool, settings: object }
TSharedPtr<FJsonObject> HandleGetRetargeterOpInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString RetargeterPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ik_retargeter_path"), RetargeterPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    int32 OpIndex = 0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("op_index"), OpIndex)) { return Missing; }

    UIKRetargeter* Retargeter = nullptr;
    UIKRetargeterController* Controller = nullptr;
    if (TSharedPtr<FJsonObject> Error = IKRig_LoadRetargeterController(RetargeterPath, Retargeter, Controller)) { return Error; }

    if (OpIndex < 0 || OpIndex >= Controller->GetNumRetargetOps())
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("op_index %d out of range (0..%d)"), OpIndex, Controller->GetNumRetargetOps() - 1));

    // GetRetargetOpByIndex returns a mutable pointer to the live op; null on out-of-range
    // (already validated above, but the null-check is honest).
    FIKRetargetOpBase* Op = Controller->GetRetargetOpByIndex(OpIndex);
    if (!Op)
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("op_index %d resolves to a null op"), OpIndex));

    const UScriptStruct* SettingsType = Op->GetSettingsType();
    const void* SettingsPtr = Op->GetSettings();

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("asset_path"), Retargeter->GetPathName());
    Resp->SetNumberField(TEXT("op_index"), OpIndex);
    Resp->SetStringField(TEXT("op_name"), Controller->GetOpName(OpIndex).ToString());
    Resp->SetStringField(TEXT("op_type"), SettingsType ? SettingsType->GetName() : TEXT(""));
    Resp->SetBoolField(TEXT("enabled"), Controller->GetRetargetOpEnabled(OpIndex));
    Resp->SetObjectField(TEXT("settings"), SerializeStructGeneric(SettingsType, SettingsPtr));
    return mcp_common::MakeOkReply(Resp);
}

// ---------------------------------------------------------------------------
// set_retargeter_op_settings
// ---------------------------------------------------------------------------
// MCP:set_retargeter_op_settings - Partial-merge a retarget op's live settings struct (only the JSON fields present are written -> idempotent), and optionally toggle the op enabled flag, as one undo transaction, then save the asset.  Settings keys match get_retargeter_op_info (first char lower-cased, e.g. lODThreshold). Numeric ClampMin/ClampMax metadata is enforced after the merge. The optional `enabled` flag is applied via the controller. The payload echoes the read-back settings. When neither is given, or the op already has those settings and that enabled state, changed is false, saved is false, a note says why and nothing is saved. An out-of-range op_index is an error. A failed save makes success false, with saved=false and save_error.
//   @param ik_retargeter_path - Content path to the UIKRetargeter asset.
//   @param op_index [int!] - 0-based index of the op in the retargeter's op stack.
//   @param settings [object] - Partial settings object; only present fields are written (e.g. { lODThreshold: 5 }).
//   @param enabled [bool] - Optional: enable/disable the op (applied via the controller).
//   @returns { asset_path: string, op_index: int, op_name: string, op_type: string, enabled: bool, settings: object, changed: bool, saved: bool, save_error?: string, note?: string }
TSharedPtr<FJsonObject> HandleSetRetargeterOpSettings(const TSharedPtr<FJsonObject>& Params)
{
    FString RetargeterPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ik_retargeter_path"), RetargeterPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    int32 OpIndex = 0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("op_index"), OpIndex)) { return Missing; }

    // settings is optional here only so a caller may toggle `enabled` alone; if neither
    // settings nor enabled is provided the call is a no-op merge (still idempotent).
    const TSharedPtr<FJsonObject>* SettingsObj = nullptr;
    const bool bHasSettings = Params->TryGetObjectField(TEXT("settings"), SettingsObj) && SettingsObj && SettingsObj->IsValid();

    bool bEnabled = false;
    const bool bHasEnabled = Params->TryGetBoolField(TEXT("enabled"), bEnabled);

    UIKRetargeter* Retargeter = nullptr;
    UIKRetargeterController* Controller = nullptr;
    if (TSharedPtr<FJsonObject> Error = IKRig_LoadRetargeterController(RetargeterPath, Retargeter, Controller)) { return Error; }

    if (OpIndex < 0 || OpIndex >= Controller->GetNumRetargetOps())
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("op_index %d out of range (0..%d)"), OpIndex, Controller->GetNumRetargetOps() - 1));

    FIKRetargetOpBase* Op = Controller->GetRetargetOpByIndex(OpIndex);
    if (!Op)
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("op_index %d resolves to a null op"), OpIndex));

    // Partial-merge the JSON onto the live settings struct (GetSettings() returns a
    // mutable pointer to the instance stored in the op, so writing through it mutates
    // the live op). Only fields present in the JSON are written -> idempotent. The
    // generic merge runs the ClampMin/ClampMax post-pass (generic helper).
    const UScriptStruct* SettingsType = Op->GetSettingsType();
    void* SettingsPtr = Op->GetSettings();
    if (bHasSettings && (!SettingsType || !SettingsPtr))
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Op at index %d exposes no editable settings struct."), OpIndex));
    const auto ApplySettings = [&](void* Settings) { MergeJsonIntoStructGeneric(SettingsType, Settings, *SettingsObj); };

    // Settings that already have the given values and an enabled flag the op already has change nothing:
    // they are answered before Modify() would dirty the asset.
    const bool bSettingsChange = bHasSettings && IKRig_WouldChangeStruct(SettingsType, SettingsPtr, ApplySettings);
    const bool bEnabledChange = bHasEnabled && Controller->GetRetargetOpEnabled(OpIndex) != bEnabled;

    // Read-back the live settings + enabled state for an honest round-trip payload.
    // Re-fetch the op pointer in case SetRetargetOpEnabled's reinit invalidated it.
    const auto BuildResponse = [Retargeter, Controller, OpIndex]()
    {
        FIKRetargetOpBase* ReadOp = Controller->GetRetargetOpByIndex(OpIndex);
        const UScriptStruct* ReadType = ReadOp ? ReadOp->GetSettingsType() : nullptr;
        const void* ReadPtr = ReadOp ? ReadOp->GetSettings() : nullptr;

        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetStringField(TEXT("asset_path"), Retargeter->GetPathName());
        Resp->SetNumberField(TEXT("op_index"), OpIndex);
        Resp->SetStringField(TEXT("op_name"), Controller->GetOpName(OpIndex).ToString());
        Resp->SetStringField(TEXT("op_type"), ReadType ? ReadType->GetName() : TEXT(""));
        Resp->SetBoolField(TEXT("enabled"), Controller->GetRetargetOpEnabled(OpIndex));
        Resp->SetObjectField(TEXT("settings"), SerializeStructGeneric(ReadType, ReadPtr));
        return Resp;
    };

    if (!bSettingsChange && !bEnabledChange)
    {
        TSharedPtr<FJsonObject> Resp = BuildResponse();
        Resp->SetBoolField(TEXT("changed"), false);
        return IKRig_NoChangeReply(Resp, (bHasSettings || bHasEnabled)
            ? TEXT("The op already has those settings and that enabled state; nothing changed and nothing was saved.")
            : TEXT("Neither settings nor enabled was given; nothing changed and nothing was saved."));
    }

    {
        // One undo step for the settings written straight into the op and the enabled toggle, which the
        // controller applies in its own transaction (nested in this one) with Modify + reinit.
        const FScopedTransaction Transaction(NSLOCTEXT("UEAgentIKRigCommands", "SetRetargeterOpSettings", "Set Retargeter Op Settings"));
        Retargeter->Modify();
        if (bSettingsChange)
        {
            ApplySettings(SettingsPtr);
        }
        if (bEnabledChange)
        {
            Controller->SetRetargetOpEnabled(OpIndex, bEnabled);
        }
    }

    FString SaveErr;
    const bool bSaved = mcp_common::SaveAssetPackage(Retargeter->GetOutermost(), Retargeter, SaveErr);

    TSharedPtr<FJsonObject> Resp = BuildResponse();
    Resp->SetBoolField(TEXT("changed"), true);
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("set_retargeter_op_settings"), SaveErr);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_ik_goal"), &HandleAddIKGoal },
    { TEXT("add_ik_retarget_chain"), &HandleAddIKRetargetChain },
    { TEXT("add_ik_solver"), &HandleAddIKSolver },
    { TEXT("add_retargeter_op"), &HandleAddRetargeterOp },
    { TEXT("connect_ik_goal_to_solver"), &HandleConnectIKGoalToSolver },
    { TEXT("create_ik_retargeter"), &HandleCreateIKRetargeter },
    { TEXT("create_ik_rig"), &HandleCreateIKRig },
    { TEXT("create_retarget_pose"), &HandleCreateRetargetPose },
    { TEXT("disconnect_ik_goal_from_solver"), &HandleDisconnectIKGoalFromSolver },
    { TEXT("get_ik_bone_settings"), &HandleGetIKBoneSettings },
    { TEXT("get_ik_goal_settings"), &HandleGetIKGoalSettings },
    { TEXT("get_ik_retargeter_info"), &HandleGetIKRetargeterInfo },
    { TEXT("get_ik_rig_info"), &HandleGetIKRigInfo },
    { TEXT("get_ik_solver_details"), &HandleGetIKSolverDetails },
    { TEXT("get_retarget_poses"), &HandleGetRetargetPoses },
    { TEXT("get_retargeter_op_info"), &HandleGetRetargeterOpInfo },
    { TEXT("move_ik_solver_in_stack"), &HandleMoveIKSolverInStack },
    { TEXT("remove_ik_goal"), &HandleRemoveIKGoal },
    { TEXT("remove_ik_retarget_chain"), &HandleRemoveIKRetargetChain },
    { TEXT("remove_ik_solver"), &HandleRemoveIKSolver },
    { TEXT("remove_retarget_pose"), &HandleRemoveRetargetPose },
    { TEXT("remove_retargeter_op"), &HandleRemoveRetargeterOp },
    { TEXT("rename_ik_retarget_chain"), &HandleRenameIKRetargetChain },
    { TEXT("set_ik_bone_settings"), &HandleSetIKBoneSettings },
    { TEXT("set_ik_goal_settings"), &HandleSetIKGoalSettings },
    { TEXT("set_ik_retarget_chain_bones"), &HandleSetIKRetargetChainBones },
    { TEXT("set_ik_solver_bones"), &HandleSetIKSolverBones },
    { TEXT("set_ik_solver_enabled"), &HandleSetIKSolverEnabled },
    { TEXT("set_ik_solver_settings"), &HandleSetIKSolverSettings },
    { TEXT("set_retarget_pose_bone_rotation"), &HandleSetRetargetPoseBoneRotation },
    { TEXT("set_retargeter_chain_mapping"), &HandleSetRetargeterChainMapping },
    { TEXT("set_retargeter_op_settings"), &HandleSetRetargeterOpSettings },
};
} // namespace UEAgentIKRigCommands
