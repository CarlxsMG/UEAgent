// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Misc/EngineVersionComparison.h"
#include "Commands/Animation/AnimAssetUtils.h"

#include "Animation/Skeleton.h"
#include "Animation/AnimationAsset.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimTypes.h"
#include "Animation/AnimLinkableElement.h"
#include "Animation/AnimCompositeBase.h"
#include "Animation/AnimComposite.h"
#include "Animation/AnimSequence.h"
#include "Animation/PoseAsset.h"
#include "Animation/BlendSpace.h"
#include "Animation/BlendSpace1D.h"
#include "Animation/AimOffsetBlendSpace.h"
#include "Animation/AimOffsetBlendSpace1D.h"
#include "AnimationBlueprintLibrary.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Engine/SkeletalMesh.h"  // USkeletalMesh -- set_anim_montage_preview_mesh
#include "ReferenceSkeleton.h"

// Animation Data Model authoring.
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimData/CurveIdentifier.h"
#include "Misc/FrameRate.h"
#include "Misc/FrameNumber.h"
#include "Curves/RichCurve.h"
#include "Math/Color.h"

#include "UObject/UObjectGlobals.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"
#include "UObject/PerPlatformProperties.h"  // FPerPlatformFrameRate -- the per-platform sampling rates of a sequence
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"

// Anim sequence reimport
#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "EditorReimportHandler.h"

#define LOCTEXT_NAMESPACE "UEAgentAnimationAuthoringCommands"

/**
 * Animation asset authoring commands.
 *
 * Distinct from the runtime/query commands of UEAgentAnimationCommands: these create and mutate persisted
 * animation assets -- montages, blend spaces, composites, pose assets and sequences, plus the notify and curve
 * names registered on a USkeleton. The creation commands bind the new asset to a real USkeleton: the one
 * skeleton_path names, /Engine/EngineMeshes/SkeletalCube_Skeleton by default. Sequence tracks and curves are
 * authored through the Animation Data Model (IAnimationDataController).
 */
namespace UEAgentAnimationAuthoringCommands
{

// MCP-DOMAIN: Animation

namespace
{
    static const TCHAR* kDefaultSkeletonPath = TEXT("/Engine/EngineMeshes/SkeletalCube_Skeleton");

    // Loads the skeleton the optional "skeleton_path" names (default: the engine SkeletalCube_Skeleton) into
    // OutSkeleton: null when it loads, else the "Skeleton not found" reply.
    TSharedPtr<FJsonObject> AnimAuthoring_ReadSkeleton(const TSharedPtr<FJsonObject>& Params, USkeleton*& OutSkeleton)
    {
        FString SkeletonPath = kDefaultSkeletonPath;
        FString Provided;
        if (Params->TryGetStringField(TEXT("skeleton_path"), Provided) && !Provided.IsEmpty())
        {
            SkeletonPath = Provided;
        }
        return AnimAssetUtils::LoadOrError(SkeletonPath, TEXT("Skeleton"), OutSkeleton);
    }

    // Pick a valid bone name on the skeleton. If BoneName is provided and exists, use it;
    // otherwise fall back to the root bone (index 0). Returns NAME_None if no bones.
    FName PickBoneName(const USkeleton* Skeleton, const FString& RequestedBone)
    {
        const FReferenceSkeleton& Ref = Skeleton->GetReferenceSkeleton();
        if (Ref.GetNum() <= 0)
        {
            return NAME_None;
        }
        if (!RequestedBone.IsEmpty())
        {
            const FName Requested(*RequestedBone);
            if (Ref.FindBoneIndex(Requested) != INDEX_NONE)
            {
                return Requested;
            }
        }
        return Ref.GetBoneName(0);
    }

    // Saves the package Asset lives in. False when the save fails (OutError says why) or Asset has no package.
    bool AnimAuthoring_SavePackageOf(UObject* Asset, FString& OutError)
    {
        UPackage* Package = Asset->GetPackage();
        return Package && mcp_common::SaveAssetPackage(Package, Asset, OutError);
    }

    // The "message" of a mutation that saves its asset: "<Done> and saved to disk", or, when the save failed,
    // "<Done><UnsavedNote>; save not persisted (<SaveError>)".
    FString AnimAuthoring_SaveMessage(const TCHAR* Done, bool bSaved, const FString& SaveError,
        const TCHAR* UnsavedNote = TEXT(" in-memory"))
    {
        return bSaved
            ? FString::Printf(TEXT("%s and saved to disk"), Done)
            : FString::Printf(TEXT("%s%s; save not persisted (%s)"), Done, UnsavedNote, *SaveError);
    }

    // The reply of a valid call that changed nothing: success with saved=false and Note under NoteField. The
    // asset was neither modified, dirtied nor saved, so a Play-in-Editor session or a read-only root cannot
    // turn the no-op into a failure.
    TSharedPtr<FJsonObject> AnimAuthoring_NoChangeReply(const TSharedPtr<FJsonObject>& Obj, const FString& Note,
        const TCHAR* NoteField = TEXT("message"))
    {
        Obj->SetBoolField(TEXT("saved"), false);
        Obj->SetStringField(NoteField, Note);
        return mcp_common::MakeOkReply(Obj);
    }

    // The optional "package_path" param without trailing slashes; DefaultFolder when absent or empty.
    FString AnimAuthoring_ReadPackagePath(const TSharedPtr<FJsonObject>& Params, const TCHAR* DefaultFolder)
    {
        FString PackagePath;
        if (!Params->TryGetStringField(TEXT("package_path"), PackagePath) || PackagePath.IsEmpty())
        {
            PackagePath = DefaultFolder;
        }
        return AnimAssetUtils::StripTrailingSlashes(PackagePath);
    }

    // The "<Name> out of range" reply when Value is negative or does not fit in int32, else null.
    TSharedPtr<FJsonObject> AnimAuthoring_CheckIndex(double Value, const TCHAR* Name)
    {
        if (Value < 0.0 || Value > static_cast<double>(TNumericLimits<int32>::Max()))
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("%s out of range (must be >= 0 and fit in int32): %f"), Name, Value));
        }
        return nullptr;
    }
}

// ---------------------------------------------------------------------------
// create_anim_montage
// ---------------------------------------------------------------------------
// MCP:create_anim_montage - Create and save an empty-but-valid UAnimMontage asset under /Game. Idempotent: a UAnimMontage already at that path is returned unchanged with already_existed=true and saved=false, and nothing is saved. A failed save makes success false, with saved=false and save_error.
//   @param name - Asset name (e.g. 'AM_Example').
//   @param package_path - Target content package path where the asset is created (default /Game/Animation)
//   @param slot_name - Montage slot name (e.g. DefaultSlot)
//   @param skeleton_path - Asset path of the target Skeleton
//   @returns { name, package_path, asset_path, montage_path, skeleton_path, slot_names, slot_name, slot_count, section_count, created, already_existed, saved, save_error, message }
TSharedPtr<FJsonObject> HandleCreateAnimMontage(const TSharedPtr<FJsonObject>& Params)
{
    FString Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name)) { return Missing; }

    const FString PackagePath = AnimAuthoring_ReadPackagePath(Params, TEXT("/Game/Animation"));

    FString SlotName = TEXT("DefaultSlot");
    Params->TryGetStringField(TEXT("slot_name"), SlotName);
    if (SlotName.IsEmpty())
    {
        SlotName = TEXT("DefaultSlot");
    }

    USkeleton* Skeleton = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAuthoring_ReadSkeleton(Params, Skeleton)) { return Error; }

    const FString FullPath = FString::Printf(TEXT("%s/%s"), *PackagePath, *Name);

    auto BuildPayload = [&](UAnimMontage* Montage, bool bCreated) -> TSharedPtr<FJsonObject>
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Name);
        Obj->SetStringField(TEXT("package_path"), PackagePath);
        Obj->SetStringField(TEXT("asset_path"), Montage->GetPathName());
        Obj->SetStringField(TEXT("montage_path"), FullPath);
        const USkeleton* MontageSkeleton = Montage->GetSkeleton();
        Obj->SetStringField(TEXT("skeleton_path"), MontageSkeleton ? MontageSkeleton->GetPathName() : FString());

        // Read back the slot track names that actually landed on the montage.
        TArray<TSharedPtr<FJsonValue>> SlotNames;
        FString FirstSlot;
        for (const FSlotAnimationTrack& Track : Montage->SlotAnimTracks)
        {
            SlotNames.Add(MakeShared<FJsonValueString>(Track.SlotName.ToString()));
            if (FirstSlot.IsEmpty())
            {
                FirstSlot = Track.SlotName.ToString();
            }
        }
        Obj->SetArrayField(TEXT("slot_names"), SlotNames);
        Obj->SetStringField(TEXT("slot_name"), FirstSlot);
        Obj->SetNumberField(TEXT("slot_count"), Montage->SlotAnimTracks.Num());
        Obj->SetNumberField(TEXT("section_count"), Montage->CompositeSections.Num());
        Obj->SetBoolField(TEXT("created"), bCreated);
        return Obj;
    };

    // Idempotent: if it already exists, load + return a stable payload. Nothing changed, so nothing is saved.
    if (UAnimMontage* Existing = AnimAssetUtils::FindExisting<UAnimMontage>(FullPath))
    {
        TSharedPtr<FJsonObject> Obj = BuildPayload(Existing, /*bCreated=*/false);
        Obj->SetBoolField(TEXT("already_existed"), true);
        Obj->SetBoolField(TEXT("saved"), false);
        Obj->SetStringField(TEXT("message"), TEXT("AnimMontage already exists; loaded the existing asset unchanged, nothing saved"));
        return mcp_common::MakeOkReply(Obj);
    }

    // Mirrors UAnimMontageFactory::FactoryCreateNew's mutation sequence.
    FString CreateError;
    UAnimMontage* Montage = mcp_common::NewAssetInPackage<UAnimMontage>(FullPath, Name,
        TEXT("Failed to create UAnimMontage"), CreateError);
    if (!Montage)
    {
        return mcp_common::MakeErrorReply(CreateError);
    }

    Montage->SetSkeleton(Skeleton);

    // Add a slot animation track so the montage is structurally valid even without an
    // AnimSequence segment (AddSlot appends an FSlotAnimationTrack with the given name). The
    // constructor already adds DefaultSlot, so look the name up: UAnimMontage::IsValidSlot is
    // false for a track with no segment, and testing it would add a second, empty DefaultSlot.
    const FName SlotFName(*SlotName);
    if (!Montage->SlotAnimTracks.ContainsByPredicate([SlotFName](const FSlotAnimationTrack& Track) { return Track.SlotName == SlotFName; }))
    {
        Montage->AddSlot(SlotFName);
    }

    // Ensure a starting composite section at t=0 (mirrors UAnimMontageFactory::EnsureStartingSection).
    if (Montage->CompositeSections.Num() <= 0)
    {
        FCompositeSection NewSection;
        NewSection.SetTime(0.0f);
        NewSection.SectionName = FName(TEXT("Default"));
        Montage->CompositeSections.Add(NewSection);
    }

    FAssetRegistryModule::AssetCreated(Montage);

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Montage->GetPackage(), Montage, SaveError);

    TSharedPtr<FJsonObject> Obj = BuildPayload(Montage, /*bCreated=*/true);
    Obj->SetBoolField(TEXT("already_existed"), false);
    Obj->SetStringField(TEXT("message"), bSaved
        ? TEXT("AnimMontage created and saved to disk")
        : FString::Printf(TEXT("AnimMontage created but save failed: %s"), *SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("create_anim_montage"), SaveError);
}

// ---------------------------------------------------------------------------
// Rebuild the RUNTIME DATA of a blend space.
//
// UBlendSpace::AddSample / DeleteSample / the BlendParameters setters touch ONLY
// SampleData. What the runtime actually evaluates is FBlendSpaceData, built by
// ResampleData() -- which the engine calls from SBlendSpaceEditor on EVERY edit and
// from PostLoad, but NOT from PostEditChangeProperty.
//
// Without this, a blend space authored through MCP reads back perfectly, compiles with
// 0 errors and evaluates to the REFERENCE POSE (its .uasset carries no "BlendSpaceData",
// unlike one authored in the editor).
//
// Mirrors SAnimationBlendSpace.cpp (ValidateSampleData, then ResampleData).
// ---------------------------------------------------------------------------
namespace
{
    void RebuildBlendSpaceRuntimeData(UBlendSpace* BlendSpace)
    {
        if (BlendSpace)
        {
            BlendSpace->ValidateSampleData();
            BlendSpace->ResampleData();
            BlendSpace->MarkPackageDirty();
        }
    }
}
// ---------------------------------------------------------------------------
// create_blend_space
// ---------------------------------------------------------------------------
// MCP:create_blend_space - Create and save a blend space asset under /Game -- a 2D UBlendSpace (default), a UBlendSpace1D, or an aim offset (UAimOffsetBlendSpace or UAimOffsetBlendSpace1D) picked by blend_space_type -- and write every axis setting it is given (name, range and grid divisions of the X axis, and of the Y axis on a 2D type) before rebuilding its runtime data. Idempotent: a blend space already at that path is returned unchanged with already_existed=true and saved=false, and nothing is written or saved (blend_space_type then reports the class it already has). An unknown blend_space_type, one that contradicts is_1d, an axis minimum not below its maximum, a grid below 1 or a Y-axis param on a 1D type is refused before anything is created. A failed save makes success false, with saved=false and save_error.
//   @param name - Asset name (e.g. 'BS_Example').
//   @param asset_name - Alias of name.
//   @param package_path - Target content package path where the asset is created (default /Game/Animation)
//   @param blend_space_type - Optional class to create: BlendSpace (2D, default) | BlendSpace1D | AimOffset (2D UAimOffsetBlendSpace) | AimOffset1D (UAimOffsetBlendSpace1D), any case. Omitted, is_1d picks BlendSpace1D or BlendSpace.
//   @param is_1d - If true create a 1D BlendSpace, else 2D (default false); when blend_space_type is also given they must agree.
//   @param axis_name - X-axis display name (default 'Speed'; 'Yaw' for an aim offset)
//   @param axis_min - X-axis minimum value (default 0; -90 for an aim offset); must be below axis_max.
//   @param min_value - Alias of axis_min.
//   @param axis_max - X-axis maximum value (default 100; 90 for an aim offset)
//   @param max_value - Alias of axis_max.
//   @param axis_grid - Optional X-axis grid divisions, an integer >= 1 (default: the engine's 4).
//   @param num_grid_points - Alias of axis_grid.
//   @param y_axis_name - Optional Y-axis display name, 2D types only (default 'Pitch' for an aim offset; otherwise the engine's 'None' is kept).
//   @param y_axis_min - Optional Y-axis minimum, 2D types only (default 0; -90 for an aim offset); must be below y_axis_max.
//   @param y_axis_max - Optional Y-axis maximum, 2D types only (default 100; 90 for an aim offset).
//   @param y_axis_grid - Optional Y-axis grid divisions, 2D types only, an integer >= 1 (default: the engine's 4).
//   @param skeleton_path - Asset path of the target Skeleton (default /Engine/EngineMeshes/SkeletalCube_Skeleton)
//   @returns { name, package_path, asset_path, blend_space_path, blend_space_type, is_1d, skeleton_path, axis_name, axis_min, axis_max, axis_grid_num, y_axis_name?, y_axis_min?, y_axis_max?, y_axis_grid_num?, sample_count, created, already_existed, saved, save_error, message }
TSharedPtr<FJsonObject> HandleCreateBlendSpace(const TSharedPtr<FJsonObject>& Params)
{
    FString Name;
    if (!Params->TryGetStringField(TEXT("name"), Name) && !Params->TryGetStringField(TEXT("asset_name"), Name))
    {
        return mcp_common::MissingParameterReply(TEXT("name"));
    }
    if (Name.IsEmpty())
    {
        return mcp_common::MissingParameterReply(TEXT("name"));
    }

    const FString PackagePath = AnimAuthoring_ReadPackagePath(Params, TEXT("/Game/Animation"));

    // The class: blend_space_type when given, else is_1d (the original switch). Both given must agree.
    bool bIs1D = false;
    const bool bHasIs1D = Params->TryGetBoolField(TEXT("is_1d"), bIs1D);
    bool bAimOffset = false;
    FString TypeText;
    if (Params->TryGetStringField(TEXT("blend_space_type"), TypeText) && !TypeText.TrimStartAndEnd().IsEmpty())
    {
        const FString Type = TypeText.TrimStartAndEnd();
        bool bTypeIs1D = false;
        if (Type.Equals(TEXT("BlendSpace"), ESearchCase::IgnoreCase) || Type.Equals(TEXT("BlendSpace2D"), ESearchCase::IgnoreCase))
        {
            bTypeIs1D = false;
        }
        else if (Type.Equals(TEXT("BlendSpace1D"), ESearchCase::IgnoreCase))
        {
            bTypeIs1D = true;
        }
        else if (Type.Equals(TEXT("AimOffset"), ESearchCase::IgnoreCase) || Type.Equals(TEXT("AimOffsetBlendSpace"), ESearchCase::IgnoreCase))
        {
            bAimOffset = true;
        }
        else if (Type.Equals(TEXT("AimOffset1D"), ESearchCase::IgnoreCase) || Type.Equals(TEXT("AimOffsetBlendSpace1D"), ESearchCase::IgnoreCase))
        {
            bAimOffset = true;
            bTypeIs1D = true;
        }
        else
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Unknown blend_space_type '%s': expected BlendSpace, BlendSpace1D, AimOffset or AimOffset1D. Nothing was created."), *Type));
        }
        if (bHasIs1D && bIs1D != bTypeIs1D)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("is_1d=%s contradicts blend_space_type '%s'; pass one of them. Nothing was created."),
                bIs1D ? TEXT("true") : TEXT("false"), *Type));
        }
        bIs1D = bTypeIs1D;
    }

    // X axis (BlendParameters[0]). An aim offset defaults to a Yaw axis of -90..90.
    FString AxisName;
    Params->TryGetStringField(TEXT("axis_name"), AxisName);
    if (AxisName.IsEmpty())
    {
        AxisName = bAimOffset ? TEXT("Yaw") : TEXT("Speed");
    }
    double AxisMin = bAimOffset ? -90.0 : 0.0;
    double AxisMax = bAimOffset ? 90.0 : 100.0;
    if (!Params->TryGetNumberField(TEXT("axis_min"), AxisMin))
    {
        Params->TryGetNumberField(TEXT("min_value"), AxisMin);
    }
    if (!Params->TryGetNumberField(TEXT("axis_max"), AxisMax))
    {
        Params->TryGetNumberField(TEXT("max_value"), AxisMax);
    }
    double AxisGridD = 0.0;
    if (!Params->TryGetNumberField(TEXT("axis_grid"), AxisGridD))
    {
        Params->TryGetNumberField(TEXT("num_grid_points"), AxisGridD);
    }
    const bool bHasAxisGrid = Params->HasField(TEXT("axis_grid")) || Params->HasField(TEXT("num_grid_points"));

    // Y axis (BlendParameters[1]), 2D types only. An aim offset defaults to a Pitch axis of -90..90; a plain
    // 2D blend space keeps the engine's unnamed 0..100 axis unless the call names one.
    FString YAxisName;
    Params->TryGetStringField(TEXT("y_axis_name"), YAxisName);
    double YAxisMin = bAimOffset ? -90.0 : 0.0;
    double YAxisMax = bAimOffset ? 90.0 : 100.0;
    const bool bHasYMin = Params->TryGetNumberField(TEXT("y_axis_min"), YAxisMin);
    const bool bHasYMax = Params->TryGetNumberField(TEXT("y_axis_max"), YAxisMax);
    double YAxisGridD = 0.0;
    const bool bHasYGrid = Params->TryGetNumberField(TEXT("y_axis_grid"), YAxisGridD);
    if (bIs1D && (!YAxisName.IsEmpty() || bHasYMin || bHasYMax || bHasYGrid))
    {
        return mcp_common::MakeErrorReply(TEXT(
            "y_axis_name, y_axis_min, y_axis_max and y_axis_grid apply to a 2D blend space only, and this call creates a 1D one. Nothing was created."));
    }
    if (YAxisName.IsEmpty() && bAimOffset)
    {
        YAxisName = TEXT("Pitch");
    }

    // Every axis value is checked before anything is created: the engine divides by the range and the grid.
    if (AxisMin >= AxisMax)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("axis_min (%g) must be below axis_max (%g). Nothing was created."), AxisMin, AxisMax));
    }
    if (!bIs1D && YAxisMin >= YAxisMax)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("y_axis_min (%g) must be below y_axis_max (%g). Nothing was created."), YAxisMin, YAxisMax));
    }
    const auto CheckGrid = [](bool bGiven, double Value, const TCHAR* Label) -> TSharedPtr<FJsonObject>
    {
        if (bGiven && (Value < 1.0 || Value > static_cast<double>(TNumericLimits<int32>::Max()) || FMath::Frac(Value) != 0.0))
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("%s must be an integer >= 1 (grid divisions), got %g. Nothing was created."), Label, Value));
        }
        return nullptr;
    };
    if (TSharedPtr<FJsonObject> Error = CheckGrid(bHasAxisGrid, AxisGridD, TEXT("axis_grid"))) { return Error; }
    if (TSharedPtr<FJsonObject> Error = CheckGrid(bHasYGrid, YAxisGridD, TEXT("y_axis_grid"))) { return Error; }

    USkeleton* Skeleton = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAuthoring_ReadSkeleton(Params, Skeleton)) { return Error; }

    const FString FullPath = FString::Printf(TEXT("%s/%s"), *PackagePath, *Name);

    const auto TypeNameOf = [](const UBlendSpace* BlendSpace) -> const TCHAR*
    {
        // UAimOffsetBlendSpace1D derives from UBlendSpace1D, so the aim offsets are tested first.
        if (BlendSpace->IsA<UAimOffsetBlendSpace1D>()) { return TEXT("AimOffset1D"); }
        if (BlendSpace->IsA<UAimOffsetBlendSpace>())   { return TEXT("AimOffset"); }
        if (BlendSpace->IsA<UBlendSpace1D>())          { return TEXT("BlendSpace1D"); }
        return TEXT("BlendSpace");
    };

    auto BuildPayload = [&](UBlendSpace* BlendSpace, bool bCreated) -> TSharedPtr<FJsonObject>
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Name);
        Obj->SetStringField(TEXT("package_path"), PackagePath);
        Obj->SetStringField(TEXT("asset_path"), BlendSpace->GetPathName());
        Obj->SetStringField(TEXT("blend_space_path"), FullPath);
        Obj->SetStringField(TEXT("blend_space_type"), TypeNameOf(BlendSpace));
        const bool bAsset1D = BlendSpace->IsA<UBlendSpace1D>();
        Obj->SetBoolField(TEXT("is_1d"), bAsset1D);
        const USkeleton* BSSkeleton = BlendSpace->GetSkeleton();
        Obj->SetStringField(TEXT("skeleton_path"), BSSkeleton ? BSSkeleton->GetPathName() : FString());

        // Read back the configured axes via the public const accessor.
        const FBlendParameter& XAxis = BlendSpace->GetBlendParameter(0);
        Obj->SetStringField(TEXT("axis_name"), XAxis.DisplayName);
        Obj->SetNumberField(TEXT("axis_min"), XAxis.Min);
        Obj->SetNumberField(TEXT("axis_max"), XAxis.Max);
        Obj->SetNumberField(TEXT("axis_grid_num"), XAxis.GridNum);
        if (!bAsset1D)
        {
            const FBlendParameter& YAxis = BlendSpace->GetBlendParameter(1);
            Obj->SetStringField(TEXT("y_axis_name"), YAxis.DisplayName);
            Obj->SetNumberField(TEXT("y_axis_min"), YAxis.Min);
            Obj->SetNumberField(TEXT("y_axis_max"), YAxis.Max);
            Obj->SetNumberField(TEXT("y_axis_grid_num"), YAxis.GridNum);
        }
        Obj->SetNumberField(TEXT("sample_count"), BlendSpace->GetBlendSamples().Num());
        Obj->SetBoolField(TEXT("created"), bCreated);
        return Obj;
    };

    // Idempotent: if it already exists, load + return a stable payload. Nothing changed, so nothing is saved.
    if (UBlendSpace* Existing = AnimAssetUtils::FindExisting<UBlendSpace>(FullPath))
    {
        TSharedPtr<FJsonObject> Obj = BuildPayload(Existing, /*bCreated=*/false);
        Obj->SetBoolField(TEXT("already_existed"), true);
        Obj->SetBoolField(TEXT("saved"), false);
        Obj->SetStringField(TEXT("message"), TEXT("BlendSpace already exists; loaded the existing asset unchanged, nothing saved"));
        return mcp_common::MakeOkReply(Obj);
    }

    UClass* BlendSpaceClass = bAimOffset
        ? (bIs1D ? UAimOffsetBlendSpace1D::StaticClass() : UAimOffsetBlendSpace::StaticClass())
        : (bIs1D ? UBlendSpace1D::StaticClass() : UBlendSpace::StaticClass());
    // The constructor sets DimensionIndices ({0} for 1D, {0,1} for 2D) automatically.
    FString CreateError;
    UBlendSpace* BlendSpace = Cast<UBlendSpace>(mcp_common::NewAssetInPackage(BlendSpaceClass, FullPath, Name,
        TEXT("Failed to create UBlendSpace"), CreateError));
    if (!BlendSpace)
    {
        return mcp_common::MakeErrorReply(CreateError);
    }

    BlendSpace->SetSkeleton(Skeleton);

    // Configure the axes (BlendParameters[0] = X, [1] = Y). BlendParameters is a protected fixed-size
    // UPROPERTY array (ArrayDim==3); write it via the reflection system, which is the supported external-write
    // path. Read-back uses GetBlendParameter().
    if (FStructProperty* BlendParamsProp = FindFProperty<FStructProperty>(UBlendSpace::StaticClass(), TEXT("BlendParameters")))
    {
        if (FBlendParameter* XAxis = BlendParamsProp->ContainerPtrToValuePtr<FBlendParameter>(BlendSpace, /*ArrayIndex=*/0))
        {
            XAxis->DisplayName = AxisName;
            XAxis->Min = static_cast<float>(AxisMin);
            XAxis->Max = static_cast<float>(AxisMax);
            if (bHasAxisGrid)
            {
                XAxis->GridNum = static_cast<int32>(AxisGridD);
            }
            else if (XAxis->GridNum < 1)
            {
                XAxis->GridNum = 4;
            }
        }
        if (!bIs1D)
        {
            if (FBlendParameter* YAxis = BlendParamsProp->ContainerPtrToValuePtr<FBlendParameter>(BlendSpace, /*ArrayIndex=*/1))
            {
                if (!YAxisName.IsEmpty())
                {
                    YAxis->DisplayName = YAxisName;
                }
                YAxis->Min = static_cast<float>(YAxisMin);
                YAxis->Max = static_cast<float>(YAxisMax);
                if (bHasYGrid)
                {
                    YAxis->GridNum = static_cast<int32>(YAxisGridD);
                }
                else if (YAxis->GridNum < 1)
                {
                    YAxis->GridNum = 4;
                }
            }
        }
    }

    FAssetRegistryModule::AssetCreated(BlendSpace);

    // Without this the RUNTIME does not see the blend space.
    RebuildBlendSpaceRuntimeData(BlendSpace);
    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(BlendSpace->GetPackage(), BlendSpace, SaveError);

    TSharedPtr<FJsonObject> Obj = BuildPayload(BlendSpace, /*bCreated=*/true);
    Obj->SetBoolField(TEXT("already_existed"), false);
    Obj->SetStringField(TEXT("message"), bSaved
        ? TEXT("BlendSpace created and saved to disk")
        : FString::Printf(TEXT("BlendSpace created but save failed: %s"), *SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("create_blend_space"), SaveError);
}

// ---------------------------------------------------------------------------
// add_anim_notify_name
// ---------------------------------------------------------------------------
// MCP:add_anim_notify_name - Register a notify name in the AnimationNotifies list of a USkeleton inside an undo transaction and save the skeleton package; returns present, notify_count and saved. A name already registered is a success with added=false, already_existed=true and saved=false, and the skeleton is neither modified nor saved. Without skeleton_path it edits /Engine/EngineMeshes/SkeletalCube_Skeleton. A failed save makes success false, with saved=false and save_error. Inverse: remove_anim_notify_name.
//   @param notify_name - The notify name to register (e.g. 'Footstep').
//   @param skeleton_path - Asset path of the USkeleton to edit (/Game/...); when omitted or empty the engine asset /Engine/EngineMeshes/SkeletalCube_Skeleton is edited and saved.
//   @returns { skeleton_path, notify_name, present, added, already_existed, notify_count, saved, save_error, message }
TSharedPtr<FJsonObject> HandleAddAnimNotifyName(const TSharedPtr<FJsonObject>& Params)
{
    FString NotifyName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("notify_name"), NotifyName)) { return Missing; }

    USkeleton* Skeleton = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAuthoring_ReadSkeleton(Params, Skeleton)) { return Error; }

    const FName NotifyFName(*NotifyName);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("skeleton_path"), Skeleton->GetPathName());
    Obj->SetStringField(TEXT("notify_name"), NotifyName);

    // A name already registered changes nothing: answered before Modify() would dirty the skeleton.
    if (Skeleton->AnimationNotifies.Contains(NotifyFName))
    {
        Obj->SetBoolField(TEXT("present"), true);
        Obj->SetBoolField(TEXT("added"), false);
        Obj->SetBoolField(TEXT("already_existed"), true);
        Obj->SetNumberField(TEXT("notify_count"), Skeleton->AnimationNotifies.Num());
        return AnimAuthoring_NoChangeReply(Obj, TEXT("Anim notify name already registered on the skeleton; nothing changed, nothing saved"));
    }

    {
        const FScopedTransaction Transaction(LOCTEXT("AddAnimNotifyName", "Add Anim Notify Name"));
        // A skeleton duplicated from engine content can lack RF_Transactional, and then Modify() records nothing: the
        // edit would be no undo step at all, whatever the transaction says.
        Skeleton->SetFlags(RF_Transactional);
        Skeleton->Modify();
        Skeleton->AddNewAnimationNotify(NotifyFName);
    }
    const bool bPresent = Skeleton->AnimationNotifies.Contains(NotifyFName);

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Skeleton, SaveError);

    Obj->SetBoolField(TEXT("present"), bPresent);
    Obj->SetBoolField(TEXT("added"), bPresent);
    Obj->SetBoolField(TEXT("already_existed"), false);
    Obj->SetNumberField(TEXT("notify_count"), Skeleton->AnimationNotifies.Num());
    Obj->SetStringField(TEXT("message"), bSaved
        ? TEXT("Anim notify registered on skeleton and saved to disk")
        : FString::Printf(TEXT("Anim notify registered in-memory; save not persisted (%s)"), *SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("add_anim_notify_name"), SaveError);
}

// MCP:remove_anim_notify_name - Unregister a notify name from the AnimationNotifies list of a USkeleton (USkeleton::RemoveAnimationNotify) inside an undo transaction and save the skeleton package. Inverse of add_anim_notify_name. It only drops the name from the skeleton's list: notify events already placed on animations keep their name (remove_anim_notify takes those off). A name that is not registered is a success with removed=false, already_absent=true and saved=false, and the skeleton is neither modified nor saved. Without skeleton_path it edits /Engine/EngineMeshes/SkeletalCube_Skeleton, as add_anim_notify_name does. The reply re-reads the list (notify_names). A failed save makes success false, with saved=false and save_error.
//   @param notify_name - The notify name to unregister (e.g. 'Footstep').
//   @param skeleton_path - Asset path of the USkeleton to edit (/Game/...); when omitted or empty the engine asset /Engine/EngineMeshes/SkeletalCube_Skeleton is edited and saved.
//   @returns { skeleton_path, notify_name, present, removed, already_absent, notify_count, notify_names: [string], saved, save_error, message }
TSharedPtr<FJsonObject> HandleRemoveAnimNotifyName(const TSharedPtr<FJsonObject>& Params)
{
    FString NotifyName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("notify_name"), NotifyName)) { return Missing; }

    USkeleton* Skeleton = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAuthoring_ReadSkeleton(Params, Skeleton)) { return Error; }

    const FName NotifyFName(*NotifyName);
    const auto NotifyNames = [Skeleton]()
    {
        TArray<FString> Names;
        for (const FName& Name : Skeleton->AnimationNotifies)
        {
            Names.Add(Name.ToString());
        }
        return Names;
    };

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("skeleton_path"), Skeleton->GetPathName());
    Obj->SetStringField(TEXT("notify_name"), NotifyName);

    // A name that is not registered changes nothing: answered before Modify() would dirty the skeleton.
    if (!Skeleton->AnimationNotifies.Contains(NotifyFName))
    {
        Obj->SetBoolField(TEXT("present"), false);
        Obj->SetBoolField(TEXT("removed"), false);
        Obj->SetBoolField(TEXT("already_absent"), true);
        Obj->SetNumberField(TEXT("notify_count"), Skeleton->AnimationNotifies.Num());
        Obj->SetArrayField(TEXT("notify_names"), mcp_common::StringsToJson(NotifyNames()));
        return AnimAuthoring_NoChangeReply(Obj, TEXT("Anim notify name is not registered on the skeleton; nothing changed, nothing saved"));
    }

    {
        const FScopedTransaction Transaction(LOCTEXT("RemoveAnimNotifyName", "Remove Anim Notify Name"));
        // A skeleton duplicated from engine content can lack RF_Transactional, and then Modify() records nothing: the
        // edit would be no undo step at all, whatever the transaction says.
        Skeleton->SetFlags(RF_Transactional);
        Skeleton->Modify();
        Skeleton->RemoveAnimationNotify(NotifyFName);
    }
    if (Skeleton->AnimationNotifies.Contains(NotifyFName))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("'%s' is still registered after RemoveAnimationNotify; nothing was saved"), *NotifyName));
    }

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Skeleton, SaveError);

    Obj->SetBoolField(TEXT("present"), false);
    Obj->SetBoolField(TEXT("removed"), true);
    Obj->SetBoolField(TEXT("already_absent"), false);
    Obj->SetNumberField(TEXT("notify_count"), Skeleton->AnimationNotifies.Num());
    Obj->SetArrayField(TEXT("notify_names"), mcp_common::StringsToJson(NotifyNames()));
    Obj->SetStringField(TEXT("message"), bSaved
        ? TEXT("Anim notify name unregistered from the skeleton and saved to disk")
        : FString::Printf(TEXT("Anim notify name unregistered in memory; save not persisted (%s)"), *SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("remove_anim_notify_name"), SaveError);
}

// ---------------------------------------------------------------------------
// add_anim_curve_name
// ---------------------------------------------------------------------------
// MCP:add_anim_curve_name - Register a curve name as curve metadata on a USkeleton (USkeleton::AddCurveMetaData) in an undo transaction, then save the skeleton package. Idempotent: a name already registered is a success with added=false, already_existed=true and saved=false, and the skeleton is neither modified nor saved. It adds no keys -- to put a keyed float curve on a UAnimSequence use add_anim_float_curve; remove_anim_curve_name is the inverse. An unknown skeleton is an error and a failed save makes success false, with saved=false and save_error.
//   @param curve_name - The curve name to add (e.g. 'Curve_Example').
//   @param skeleton_path - Asset path of the USkeleton to edit (/Game/...); when omitted or empty the engine asset /Engine/EngineMeshes/SkeletalCube_Skeleton is edited and saved.
//   @returns { skeleton_path, curve_name, present, added, already_existed, curve_count, saved, save_error, message }
TSharedPtr<FJsonObject> HandleAddAnimCurveName(const TSharedPtr<FJsonObject>& Params)
{
    FString CurveName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("curve_name"), CurveName)) { return Missing; }

    USkeleton* Skeleton = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAuthoring_ReadSkeleton(Params, Skeleton)) { return Error; }

    const FName CurveFName(*CurveName);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("skeleton_path"), Skeleton->GetPathName());
    Obj->SetStringField(TEXT("curve_name"), CurveName);

    // A name already registered changes nothing: answered before Modify() would dirty the skeleton.
    TArray<FName> Names;
    Skeleton->GetCurveMetaDataNames(Names);
    if (Names.Contains(CurveFName))
    {
        Obj->SetBoolField(TEXT("present"), true);
        Obj->SetBoolField(TEXT("added"), false);
        Obj->SetBoolField(TEXT("already_existed"), true);
        Obj->SetNumberField(TEXT("curve_count"), Names.Num());
        return AnimAuthoring_NoChangeReply(Obj, TEXT("Anim curve metadata already registered on the skeleton; nothing changed, nothing saved"));
    }

    bool bAddedNew = false;
    {
        const FScopedTransaction Transaction(LOCTEXT("AddAnimCurveName", "Add Anim Curve Name"));
        Skeleton->Modify();
        // USkeleton::AddCurveMetaData(Name, bTransact) returns true if a new entry was added.
        bAddedNew = Skeleton->AddCurveMetaData(CurveFName, /*bTransact=*/false);
    }

    // Verify present by enumerating curve metadata names.
    Names.Reset();
    Skeleton->GetCurveMetaDataNames(Names);
    const bool bPresent = Names.Contains(CurveFName);

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Skeleton, SaveError);

    Obj->SetBoolField(TEXT("present"), bPresent);
    Obj->SetBoolField(TEXT("added"), bAddedNew && bPresent);
    Obj->SetBoolField(TEXT("already_existed"), false);
    Obj->SetNumberField(TEXT("curve_count"), Names.Num());
    Obj->SetStringField(TEXT("message"), bSaved
        ? TEXT("Anim curve metadata added to skeleton and saved to disk")
        : FString::Printf(TEXT("Anim curve metadata added in-memory; save not persisted (%s)"), *SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("add_anim_curve_name"), SaveError);
}

// ===========================================================================
// Montage depth: sections / slots / notifies / blend times / info
// ===========================================================================

namespace
{
    // Append section names to a JSON array, preserving CompositeSections order.
    TArray<TSharedPtr<FJsonValue>> CollectSectionNames(const UAnimMontage* Montage)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        for (const FCompositeSection& Section : Montage->CompositeSections)
        {
            Out.Add(MakeShared<FJsonValueString>(Section.SectionName.ToString()));
        }
        return Out;
    }

    TArray<TSharedPtr<FJsonValue>> CollectSlotNames(const UAnimMontage* Montage)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        for (const FSlotAnimationTrack& Track : Montage->SlotAnimTracks)
        {
            Out.Add(MakeShared<FJsonValueString>(Track.SlotName.ToString()));
        }
        return Out;
    }

    TArray<TSharedPtr<FJsonValue>> CollectNotifyNames(const UAnimMontage* Montage)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        for (const FAnimNotifyEvent& Notify : Montage->Notifies)
        {
            Out.Add(MakeShared<FJsonValueString>(Notify.NotifyName.ToString()));
        }
        return Out;
    }

    // Collect the current pose names of a UPoseAsset into a JSON array.
    TArray<TSharedPtr<FJsonValue>> CollectPoseNames(const UPoseAsset* PoseAsset)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        for (const FName& PN : PoseAsset->GetPoseFNames())
        {
            Out.Add(MakeShared<FJsonValueString>(PN.ToString()));
        }
        return Out;
    }

    // The first composite section of Montage named SectionName, or null.
    FCompositeSection* AnimAuthoring_FindSection(UAnimMontage* Montage, FName SectionName)
    {
        for (FCompositeSection& Section : Montage->CompositeSections)
        {
            if (Section.SectionName == SectionName)
            {
                return &Section;
            }
        }
        return nullptr;
    }

    // The first slot track of Montage named SlotName, or null.
    FSlotAnimationTrack* AnimAuthoring_FindSlotTrack(UAnimMontage* Montage, FName SlotName)
    {
        for (FSlotAnimationTrack& Track : Montage->SlotAnimTracks)
        {
            if (Track.SlotName == SlotName)
            {
                return &Track;
            }
        }
        return nullptr;
    }

    const TCHAR* AnimAuthoring_BlendModeName(EMontageBlendMode Mode)
    {
        return (Mode == EMontageBlendMode::Inertialization) ? TEXT("Inertialization") : TEXT("Standard");
    }
}

// ---------------------------------------------------------------------------
// add_montage_section
// ---------------------------------------------------------------------------
// MCP:add_montage_section - Add a composite section to a UAnimMontage at an absolute start time, or move the existing section of that name there; it does not link sections (use link_montage_sections). Saves the montage in an undo transaction and returns start_time, section_count, sections, already_existed and added (true only for a new section). A section of that name already at that start time is a success with changed=false and saved=false, and the montage is neither modified nor saved. A failed save makes success false, with saved=false and save_error.
//   @param montage_path - Full asset path of the montage (e.g. '/Game/MyGame/AM_Example').
//   @param section_name - Name of the section to add (e.g. 'Loop').
//   @param start_time - Absolute section start time in seconds (default 0)
//   @returns { montage_path, section_name, start_time, section_count, sections, already_existed, added, changed, saved, save_error, message }
TSharedPtr<FJsonObject> HandleAddMontageSection(const TSharedPtr<FJsonObject>& Params)
{
    FString MontagePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("montage_path"), MontagePath)) { return Missing; }

    FString SectionName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("section_name"), SectionName)) { return Missing; }

    double StartTime = 0.0;
    Params->TryGetNumberField(TEXT("start_time"), StartTime);

    UAnimMontage* Montage = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(MontagePath, TEXT("AnimMontage"), Montage)) { return Error; }

    const FName SectionFName(*SectionName);

    // Idempotent: reuse an existing section of the same name, otherwise append a new one. An existing section
    // already at that time changes nothing: answered before Modify() would dirty the montage.
    FCompositeSection* Target = AnimAuthoring_FindSection(Montage, SectionFName);
    const bool bAlreadyExisted = Target != nullptr;

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("montage_path"), Montage->GetPathName());
    Obj->SetStringField(TEXT("section_name"), SectionName);
    Obj->SetBoolField(TEXT("already_existed"), bAlreadyExisted);
    Obj->SetBoolField(TEXT("added"), !bAlreadyExisted);

    if (Target && Target->GetTime() == static_cast<float>(StartTime))
    {
        Obj->SetNumberField(TEXT("start_time"), Target->GetTime());
        Obj->SetNumberField(TEXT("section_count"), Montage->CompositeSections.Num());
        Obj->SetArrayField(TEXT("sections"), CollectSectionNames(Montage));
        Obj->SetBoolField(TEXT("changed"), false);
        return AnimAuthoring_NoChangeReply(Obj, TEXT("Montage section already exists at that start time; nothing changed, nothing saved"));
    }

    {
        const FScopedTransaction Transaction(LOCTEXT("AddMontageSection", "Add Montage Section"));
        Montage->Modify();
        if (!Target)
        {
            FCompositeSection NewSection;
            NewSection.SectionName = SectionFName;
            const int32 NewIndex = Montage->CompositeSections.Add(NewSection);
            Target = &Montage->CompositeSections[NewIndex];
        }
        // SetTime stores the absolute time into LinkValue (Absolute reference frame).
        Target->SetTime(static_cast<float>(StartTime));
    }

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Montage, SaveError);

    Obj->SetNumberField(TEXT("start_time"), Target->GetTime());
    Obj->SetNumberField(TEXT("section_count"), Montage->CompositeSections.Num());
    Obj->SetArrayField(TEXT("sections"), CollectSectionNames(Montage));
    Obj->SetBoolField(TEXT("changed"), true);
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(bAlreadyExisted ? TEXT("Montage section moved") : TEXT("Montage section added"), bSaved, SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("add_montage_section"), SaveError);
}

// ---------------------------------------------------------------------------
// add_montage_slot
// ---------------------------------------------------------------------------
// MCP:add_montage_slot - Add an empty slot animation track with the given slot name to a UAnimMontage in an undo transaction and save it; an existing slot of that name is a success with added=false, already_existed=true and saved=false, and the montage is neither modified nor saved. Returns slot_count, the slots list and present. Fill it with add_montage_segment; inverse: remove_montage_slot. A failed save makes success false, with saved=false and save_error.
//   @param montage_path - Full asset path of the montage (e.g. '/Game/MyGame/AM_Example').
//   @param slot_name - Name of the slot track to add (e.g. 'UpperBody').
//   @returns { montage_path, slot_name, slot_count, slots, present, already_existed, added, saved, save_error, message }
TSharedPtr<FJsonObject> HandleAddMontageSlot(const TSharedPtr<FJsonObject>& Params)
{
    FString MontagePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("montage_path"), MontagePath)) { return Missing; }

    FString SlotName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("slot_name"), SlotName)) { return Missing; }

    UAnimMontage* Montage = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(MontagePath, TEXT("AnimMontage"), Montage)) { return Error; }

    const FName SlotFName(*SlotName);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("montage_path"), Montage->GetPathName());
    Obj->SetStringField(TEXT("slot_name"), SlotName);

    // A slot track with this name already exists: nothing changes, answered before Modify() would dirty the
    // montage. Not UAnimMontage::IsValidSlot, which is false for a track that has no segment yet.
    if (AnimAuthoring_FindSlotTrack(Montage, SlotFName))
    {
        Obj->SetNumberField(TEXT("slot_count"), Montage->SlotAnimTracks.Num());
        Obj->SetArrayField(TEXT("slots"), CollectSlotNames(Montage));
        Obj->SetBoolField(TEXT("present"), true);
        Obj->SetBoolField(TEXT("already_existed"), true);
        Obj->SetBoolField(TEXT("added"), false);
        return AnimAuthoring_NoChangeReply(Obj, TEXT("Montage slot track already exists; nothing changed, nothing saved"));
    }

    {
        const FScopedTransaction Transaction(LOCTEXT("AddMontageSlot", "Add Montage Slot"));
        Montage->Modify();
        // AddSlot appends an FSlotAnimationTrack with the given SlotName.
        Montage->AddSlot(SlotFName);
    }
    const bool bPresent = AnimAuthoring_FindSlotTrack(Montage, SlotFName) != nullptr;

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Montage, SaveError);

    Obj->SetNumberField(TEXT("slot_count"), Montage->SlotAnimTracks.Num());
    Obj->SetArrayField(TEXT("slots"), CollectSlotNames(Montage));
    Obj->SetBoolField(TEXT("present"), bPresent);
    Obj->SetBoolField(TEXT("already_existed"), false);
    Obj->SetBoolField(TEXT("added"), bPresent);
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Montage slot track added"), bSaved, SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("add_montage_slot"), SaveError);
}

// ---------------------------------------------------------------------------
// add_montage_notify
// ---------------------------------------------------------------------------
// MCP:add_montage_notify - Append a name-only notify event (no UAnimNotify class) to a UAnimMontage at an absolute trigger time on notify track 0 and save the montage; every call appends another event. Returns trigger_time, notify_count and the notifies name list. Remove with remove_montage_notify. A failed save makes success false, with saved=false and save_error.
//   @param montage_path - Full asset path of the montage (e.g. '/Game/MyGame/AM_Example').
//   @param notify_name - Name of the notify event (e.g. 'Footstep').
//   @param trigger_time - Absolute notify trigger time in seconds (default 0)
//   @returns { montage_path, notify_name, trigger_time, notify_count, notifies, added, saved, save_error, message }
TSharedPtr<FJsonObject> HandleAddMontageNotify(const TSharedPtr<FJsonObject>& Params)
{
    FString MontagePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("montage_path"), MontagePath)) { return Missing; }

    FString NotifyName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("notify_name"), NotifyName)) { return Missing; }

    double TriggerTime = 0.0;
    Params->TryGetNumberField(TEXT("trigger_time"), TriggerTime);

    UAnimMontage* Montage = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(MontagePath, TEXT("AnimMontage"), Montage)) { return Error; }

    const FName NotifyFName(*NotifyName);

    const FScopedTransaction Transaction(LOCTEXT("AddMontageNotify", "Add Montage Notify"));
    Montage->Modify();

    // Append a new FAnimNotifyEvent (UAnimMontage is a UAnimSequenceBase: Notifies array).
    FAnimNotifyEvent NewNotify;
    NewNotify.NotifyName = NotifyFName;
    NewNotify.TrackIndex = 0;
    const int32 NewIndex = Montage->Notifies.Add(NewNotify);
    FAnimNotifyEvent& Added = Montage->Notifies[NewIndex];
    // Link the notify to the montage at the trigger time so its LinkValue/segment data is
    // consistent; SetTime then writes the absolute trigger time into LinkValue.
    Added.Link(Montage, static_cast<float>(TriggerTime), /*InSlotIndex=*/0);
    Added.SetTime(static_cast<float>(TriggerTime));

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Montage, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("montage_path"), Montage->GetPathName());
    Obj->SetStringField(TEXT("notify_name"), NotifyName);
    Obj->SetNumberField(TEXT("trigger_time"), Added.GetTriggerTime());
    Obj->SetNumberField(TEXT("notify_count"), Montage->Notifies.Num());
    Obj->SetArrayField(TEXT("notifies"), CollectNotifyNames(Montage));
    Obj->SetBoolField(TEXT("added"), true);
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Montage notify added"), bSaved, SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("add_montage_notify"), SaveError);
}

// ---------------------------------------------------------------------------
// get_montage_info
// ---------------------------------------------------------------------------
// MCP:get_montage_info - Read a UAnimMontage without changing it: skeleton_path, section, slot and notify counts with their name lists, blend_in/blend_out times, blend modes, enable_auto_blend_out, blend_out_trigger_time, sync_group and sync_slot_index. Change them with set_properties.
//   @param montage_path - Full asset path of the montage (e.g. '/Game/MyGame/AM_Example').
//   @returns { montage_path, skeleton_path, section_count, slot_count, notify_count, sections, slots, notifies, blend_in, blend_out, blend_mode_in, blend_mode_out, enable_auto_blend_out, blend_out_trigger_time, sync_group, sync_slot_index }
TSharedPtr<FJsonObject> HandleGetMontageInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString MontagePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("montage_path"), MontagePath)) { return Missing; }

    UAnimMontage* Montage = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(MontagePath, TEXT("AnimMontage"), Montage)) { return Error; }

    const USkeleton* Skeleton = Montage->GetSkeleton();

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("montage_path"), Montage->GetPathName());
    Obj->SetStringField(TEXT("skeleton_path"), Skeleton ? Skeleton->GetPathName() : FString());
    Obj->SetNumberField(TEXT("section_count"), Montage->CompositeSections.Num());
    Obj->SetNumberField(TEXT("slot_count"), Montage->SlotAnimTracks.Num());
    Obj->SetNumberField(TEXT("notify_count"), Montage->Notifies.Num());
    Obj->SetArrayField(TEXT("sections"), CollectSectionNames(Montage));
    Obj->SetArrayField(TEXT("slots"), CollectSlotNames(Montage));
    Obj->SetArrayField(TEXT("notifies"), CollectNotifyNames(Montage));
    Obj->SetNumberField(TEXT("blend_in"), Montage->BlendIn.GetBlendTime());
    Obj->SetNumberField(TEXT("blend_out"), Montage->BlendOut.GetBlendTime());
    // Round-trip the blend MODE + sync / auto-blend-out properties
    // that set_montage_properties writes (it also writes the blend TIMES above).
    Obj->SetStringField(TEXT("blend_mode_in"), AnimAuthoring_BlendModeName(Montage->BlendModeIn));
    Obj->SetStringField(TEXT("blend_mode_out"), AnimAuthoring_BlendModeName(Montage->BlendModeOut));
    Obj->SetBoolField(TEXT("enable_auto_blend_out"), Montage->bEnableAutoBlendOut);
    Obj->SetNumberField(TEXT("blend_out_trigger_time"), Montage->BlendOutTriggerTime);
    Obj->SetStringField(TEXT("sync_group"), Montage->SyncGroup.ToString());
    Obj->SetNumberField(TEXT("sync_slot_index"), Montage->SyncSlotIndex);
    return mcp_common::MakeOkReply(Obj);
}

// ---------------------------------------------------------------------------
// remove_montage_section
// Remove a CompositeSection from UAnimMontage::CompositeSections by name.
// Canonical path: UAnimMontage exposes GetSectionIndex(FName) -> int32 (AnimMontage.h)
// and a public TArray<FCompositeSection> CompositeSections (AnimMontage.h). There is
// no AddSection/RemoveSection method — surgical TArray op is the canonical pattern.
// Idempotent: a missing section returns success with already_existed=false and the
// current sections list (no fake removed=true). The caller can branch on the boolean.
// ---------------------------------------------------------------------------
// MCP:remove_montage_section - Delete the first composite section with that name (FName match, case ignored) from a UAnimMontage and save it; a missing name is a success with removed=false and nothing saved. Links from other sections (NextSectionName) are not updated. Returns removed_index, section_count and the remaining sections. A failed save makes success false, with saved=false and save_error.
//   @param montage_path - Full asset path of the montage (e.g. '/Game/MyGame/AM_Example').
//   @param section_name - Name of the FCompositeSection to remove; compared as an FName, so case is ignored.
//   @returns { montage_path, section_name, removed_index?, already_existed, removed, section_count, sections[], saved, save_error, message }
TSharedPtr<FJsonObject> HandleDeleteMontageSection(const TSharedPtr<FJsonObject>& Params)
{
    FString MontagePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("montage_path"), MontagePath)) { return Missing; }

    FString SectionName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("section_name"), SectionName)) { return Missing; }

    UAnimMontage* Montage = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(MontagePath, TEXT("AnimMontage"), Montage)) { return Error; }

    const FName SectionFName(*SectionName);
    const int32 SectionIndex = Montage->GetSectionIndex(SectionFName);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("montage_path"), Montage->GetPathName());
    Obj->SetStringField(TEXT("section_name"), SectionName);

    if (SectionIndex == INDEX_NONE)
    {
        // Idempotent path: caller asked to delete a section that doesn't exist.
        // No transaction, no save — we did not mutate anything.
        Obj->SetBoolField(TEXT("already_existed"), false);
        Obj->SetBoolField(TEXT("removed"), false);
        Obj->SetNumberField(TEXT("section_count"), Montage->CompositeSections.Num());
        Obj->SetArrayField(TEXT("sections"), CollectSectionNames(Montage));
        Obj->SetStringField(TEXT("message"), TEXT("Section not present; nothing to remove"));
        return mcp_common::MakeOkReply(Obj);
    }

    const FScopedTransaction Transaction(LOCTEXT("DeleteMontageSection", "Delete Montage Section"));
    Montage->Modify();
    Montage->CompositeSections.RemoveAt(SectionIndex);

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Montage, SaveError);

    Obj->SetNumberField(TEXT("removed_index"), SectionIndex);
    Obj->SetBoolField(TEXT("already_existed"), true);
    Obj->SetBoolField(TEXT("removed"), true);
    Obj->SetNumberField(TEXT("section_count"), Montage->CompositeSections.Num());
    Obj->SetArrayField(TEXT("sections"), CollectSectionNames(Montage));
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Montage section removed"), bSaved, SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("remove_montage_section"), SaveError);
}

// ---------------------------------------------------------------------------
// remove_montage_notify
// Remove an FAnimNotifyEvent from UAnimSequenceBase::Notifies by index.
// Canonical path: UAnimSequenceBase exposes a public TArray<FAnimNotifyEvent> Notifies
// (AnimSequenceBase.h) plus RemoveNotifies(TArray<FName>) by-name and RemoveNotifies()
// for a full clear. By-index removal has no engine method — surgical TArray::RemoveAt is
// the canonical path. Notifies are global to the montage (FAnimNotifyEvent has
// TrackIndex, not SectionName), so there is no per-section grouping.
// Idempotent: out-of-range index returns an error with notify_count for diagnostics
// (caller can recover by re-listing). We do not fabricate a removed=true result.
// ---------------------------------------------------------------------------
// MCP:remove_montage_notify - Remove the notify event at a zero-based index from the Notifies array of a UAnimMontage and save it; an index past the end changes nothing and still returns success with removed=false. Returns removed_notify_name, notify_count_before, notify_count and the remaining notify names. A failed save makes success false, with saved=false and save_error.
//   @param montage_path - Full asset path of the montage (e.g. '/Game/MyGame/AM_Example').
//   @param notify_index - Zero-based index into the montage's Notifies array. Use get_montage_info first to list notifies.
//   @returns { montage_path, notify_index, removed_notify_name?, notify_count_before, notify_count, notifies[], removed, saved, save_error, message }
TSharedPtr<FJsonObject> HandleRemoveMontageNotify(const TSharedPtr<FJsonObject>& Params)
{
    FString MontagePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("montage_path"), MontagePath)) { return Missing; }

    double NotifyIndex = -1.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("notify_index"), NotifyIndex)) { return Missing; }
    if (TSharedPtr<FJsonObject> Error = AnimAuthoring_CheckIndex(NotifyIndex, TEXT("notify_index"))) { return Error; }
    const int32 Index = static_cast<int32>(NotifyIndex);

    UAnimMontage* Montage = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(MontagePath, TEXT("AnimMontage"), Montage)) { return Error; }

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("montage_path"), Montage->GetPathName());
    Obj->SetNumberField(TEXT("notify_index"), Index);
    Obj->SetNumberField(TEXT("notify_count_before"), Montage->Notifies.Num());

    if (Index >= Montage->Notifies.Num())
    {
        // Out-of-range path: honest error with diagnostic state. No mutation, no save.
        Obj->SetBoolField(TEXT("removed"), false);
        Obj->SetNumberField(TEXT("notify_count"), Montage->Notifies.Num());
        Obj->SetArrayField(TEXT("notifies"), CollectNotifyNames(Montage));
        Obj->SetStringField(TEXT("message"), FString::Printf(
            TEXT("notify_index %d out of range (notify_count=%d)"), Index, Montage->Notifies.Num()));
        return mcp_common::MakeOkReply(Obj);
    }

    // Capture the name of the notify being removed for the readback payload.
    const FString RemovedNotifyName = Montage->Notifies[Index].NotifyName.ToString();

    const FScopedTransaction Transaction(LOCTEXT("RemoveMontageNotify", "Remove Montage Notify"));
    Montage->Modify();
    Montage->Notifies.RemoveAt(Index);

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Montage, SaveError);

    Obj->SetStringField(TEXT("removed_notify_name"), RemovedNotifyName);
    Obj->SetBoolField(TEXT("removed"), true);
    Obj->SetNumberField(TEXT("notify_count"), Montage->Notifies.Num());
    Obj->SetArrayField(TEXT("notifies"), CollectNotifyNames(Montage));
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Montage notify removed"), bSaved, SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("remove_montage_notify"), SaveError);
}

// ---------------------------------------------------------------------------
// remove_montage_slot
// Remove an FSlotAnimationTrack from UAnimMontage::SlotAnimTracks. Name-keyed
// inverse of add_montage_slot (which appends via UAnimMontage::AddSlot).
// Canonical path: SlotAnimTracks is a public TArray<FSlotAnimationTrack>
// UPROPERTY (AnimMontage.h); FSlotAnimationTrack has an FName SlotName
// (AnimMontage.h). There is no RemoveSlot method — surgical TArray::RemoveAt
// after a SlotName lookup is the canonical pattern (same idiom as
// remove_montage_section's CompositeSections.RemoveAt). Resolution is by
// slot_name (primary; mirrors add_montage_slot) or by slot_index alternative.
// Idempotent: a missing slot returns success with already_existed=false and the
// current slots list (no fake removed=true), mirroring remove_montage_section.
// ---------------------------------------------------------------------------
// MCP:remove_montage_slot - Remove one slot animation track (the FSlotAnimationTrack and every segment on it) from a UAnimMontage, picked by slot_name or slot_index, in one undo step, and save the montage. List the slots first with get_montage_info; add_montage_slot is the inverse, and remove_montage_segment takes out a single segment instead of the whole track. A slot that is not there is a success with removed=false and already_existed=false, and nothing is changed or saved, so a repeat by slot_name is safe; a repeat by slot_index takes out whichever track moved into that index. A failed save makes success false, with saved=false and save_error.
//   @param montage_path - Object path of the UAnimMontage, e.g. '/Game/MyGame/AM_Example'.
//   @param slot_name - Slot name of the track to remove, as get_montage_info lists it, e.g. 'DefaultSlot'; the first track with that name goes. Wins over slot_index when both are given; one of the two is required.
//   @param slot_index - Zero-based index into the montage's slot tracks, in get_montage_info order (used only without slot_name); an index past the last track is a no-op success with removed=false.
//   @returns { montage_path, slot_name?, slot_index?, removed_index?, removed_slot_name?, slot_count, slots[], already_existed, removed, saved, save_error, message }
TSharedPtr<FJsonObject> HandleRemoveMontageSlot(const TSharedPtr<FJsonObject>& Params)
{
    FString MontagePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("montage_path"), MontagePath)) { return Missing; }

    // Resolve the target either by slot_name (primary, mirrors add_montage_slot)
    // or by an explicit slot_index. Exactly one path must be usable.
    FString SlotName;
    const bool bHasSlotName = Params->TryGetStringField(TEXT("slot_name"), SlotName) && !SlotName.IsEmpty();

    double SlotIndexRaw = -1.0;
    const bool bHasSlotIndex = Params->TryGetNumberField(TEXT("slot_index"), SlotIndexRaw);
    if (bHasSlotIndex && (SlotIndexRaw < 0.0 || SlotIndexRaw > static_cast<double>(TNumericLimits<int32>::Max())))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("slot_index out of range (must be >= 0 and fit in int32): %f"), SlotIndexRaw));
    }

    if (!bHasSlotName && !bHasSlotIndex)
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: slot_name or slot_index"));
    }

    UAnimMontage* Montage = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(MontagePath, TEXT("AnimMontage"), Montage)) { return Error; }

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("montage_path"), Montage->GetPathName());
    if (bHasSlotName)
    {
        Obj->SetStringField(TEXT("slot_name"), SlotName);
    }

    // Resolve the index of the slot to remove: by name (first match) or explicit index.
    int32 SlotIndex = INDEX_NONE;
    if (bHasSlotName)
    {
        const FName SlotFName(*SlotName);
        for (int32 i = 0; i < Montage->SlotAnimTracks.Num(); ++i)
        {
            if (Montage->SlotAnimTracks[i].SlotName == SlotFName)
            {
                SlotIndex = i;
                break;
            }
        }
    }
    else
    {
        const int32 RequestedIndex = static_cast<int32>(SlotIndexRaw);
        Obj->SetNumberField(TEXT("slot_index"), RequestedIndex);
        if (RequestedIndex < Montage->SlotAnimTracks.Num())
        {
            SlotIndex = RequestedIndex;
        }
    }

    if (SlotIndex == INDEX_NONE)
    {
        // Idempotent path: caller asked to remove a slot that doesn't exist.
        // No transaction, no save — we did not mutate anything.
        Obj->SetBoolField(TEXT("already_existed"), false);
        Obj->SetBoolField(TEXT("removed"), false);
        Obj->SetNumberField(TEXT("slot_count"), Montage->SlotAnimTracks.Num());
        Obj->SetArrayField(TEXT("slots"), CollectSlotNames(Montage));
        Obj->SetStringField(TEXT("message"), TEXT("Slot track not present; nothing to remove"));
        return mcp_common::MakeOkReply(Obj);
    }

    // Capture the resolved slot name (esp. useful when removing by index) for the readback.
    const FString RemovedSlotName = Montage->SlotAnimTracks[SlotIndex].SlotName.ToString();

    const FScopedTransaction Transaction(LOCTEXT("RemoveMontageSlot", "Remove Montage Slot"));
    Montage->Modify();
    Montage->SlotAnimTracks.RemoveAt(SlotIndex);

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Montage, SaveError);

    Obj->SetNumberField(TEXT("removed_index"), SlotIndex);
    Obj->SetStringField(TEXT("removed_slot_name"), RemovedSlotName);
    Obj->SetBoolField(TEXT("already_existed"), true);
    Obj->SetBoolField(TEXT("removed"), true);
    Obj->SetNumberField(TEXT("slot_count"), Montage->SlotAnimTracks.Num());
    Obj->SetArrayField(TEXT("slots"), CollectSlotNames(Montage));
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Montage slot track removed"), bSaved, SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("remove_montage_slot"), SaveError);
}

// ===========================================================================
// Composite / pose asset / additive settings

// ---------------------------------------------------------------------------
// create_anim_composite
// ---------------------------------------------------------------------------
// MCP:create_anim_composite - Create and save a UAnimComposite asset bound to a USkeleton, under /Game. Idempotent: a UAnimComposite already at that path is returned unchanged with already_existed=true and saved=false, and nothing is saved. A failed save makes success false, with saved=false and save_error.
//   @param name - Asset name (e.g. 'AnimComp_Example').
//   @param package_path - Target content package path where the asset is created (default /Game/Animation)
//   @param skeleton_path - Asset path of the target Skeleton
//   @returns { name, package_path, asset_path, composite_path, skeleton, num_segments, created, already_existed, saved, save_error, message }
TSharedPtr<FJsonObject> HandleCreateAnimComposite(const TSharedPtr<FJsonObject>& Params)
{
    FString Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name)) { return Missing; }

    const FString PackagePath = AnimAuthoring_ReadPackagePath(Params, TEXT("/Game/Animation"));

    USkeleton* Skeleton = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAuthoring_ReadSkeleton(Params, Skeleton)) { return Error; }

    const FString FullPath = FString::Printf(TEXT("%s/%s"), *PackagePath, *Name);

    auto BuildPayload = [&](UAnimComposite* Composite, bool bCreated) -> TSharedPtr<FJsonObject>
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Name);
        Obj->SetStringField(TEXT("package_path"), PackagePath);
        Obj->SetStringField(TEXT("asset_path"), Composite->GetPathName());
        Obj->SetStringField(TEXT("composite_path"), FullPath);
        const USkeleton* CompSkeleton = Composite->GetSkeleton();
        Obj->SetStringField(TEXT("skeleton"), CompSkeleton ? CompSkeleton->GetPathName() : FString());
        Obj->SetNumberField(TEXT("num_segments"), Composite->AnimationTrack.AnimSegments.Num());
        Obj->SetBoolField(TEXT("created"), bCreated);
        return Obj;
    };

    // Idempotent: if it already exists, load + return a stable payload. Nothing changed, so nothing is saved.
    if (UAnimComposite* Existing = AnimAssetUtils::FindExisting<UAnimComposite>(FullPath))
    {
        TSharedPtr<FJsonObject> Obj = BuildPayload(Existing, /*bCreated=*/false);
        Obj->SetBoolField(TEXT("already_existed"), true);
        Obj->SetBoolField(TEXT("saved"), false);
        Obj->SetStringField(TEXT("message"), TEXT("AnimComposite already exists; loaded the existing asset unchanged, nothing saved"));
        return mcp_common::MakeOkReply(Obj);
    }

    // The AnimationTrack starts empty (adding FAnimSegment entries needs a source UAnimSequence,
    // which cannot be authored headless).
    FString CreateError;
    UAnimComposite* Composite = mcp_common::NewAssetInPackage<UAnimComposite>(FullPath, Name,
        TEXT("Failed to create UAnimComposite"), CreateError);
    if (!Composite)
    {
        return mcp_common::MakeErrorReply(CreateError);
    }

    Composite->SetSkeleton(Skeleton);

    FAssetRegistryModule::AssetCreated(Composite);

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Composite->GetPackage(), Composite, SaveError);

    TSharedPtr<FJsonObject> Obj = BuildPayload(Composite, /*bCreated=*/true);
    Obj->SetBoolField(TEXT("already_existed"), false);
    Obj->SetStringField(TEXT("message"), bSaved
        ? TEXT("AnimComposite created and saved to disk (AnimationTrack empty; segments need a source AnimSequence)")
        : FString::Printf(TEXT("AnimComposite created but save failed: %s"), *SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("create_anim_composite"), SaveError);
}

// ---------------------------------------------------------------------------
// create_pose_asset
// ---------------------------------------------------------------------------
// MCP:create_pose_asset - Create an empty UPoseAsset (no poses) at package_path/name, bound to the USkeleton at skeleton_path (default the engine SkeletalCube_Skeleton), and save it; no undo transaction. Idempotent: a UPoseAsset already at that path is returned untouched with already_existed=true and saved=false, even when bound to another skeleton, and nothing is saved. A name taken by another asset class or an unknown skeleton is an error, and a failed save makes success false, with saved=false and save_error. Add poses with add_pose_to_pose_asset; to build one from animation frames use create_pose_asset_from_anim_sequence.
//   @param name - Asset name (e.g. 'Pose_Example').
//   @param package_path - Target content package path where the asset is created (default /Game/Animation)
//   @param skeleton_path - Asset path of the target Skeleton
//   @returns { name, package_path, asset_path, pose_asset_path, skeleton, num_poses, created, already_existed, saved, save_error, message }
TSharedPtr<FJsonObject> HandleCreatePoseAsset(const TSharedPtr<FJsonObject>& Params)
{
    FString Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name)) { return Missing; }

    const FString PackagePath = AnimAuthoring_ReadPackagePath(Params, TEXT("/Game/Animation"));

    USkeleton* Skeleton = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAuthoring_ReadSkeleton(Params, Skeleton)) { return Error; }

    const FString FullPath = FString::Printf(TEXT("%s/%s"), *PackagePath, *Name);

    auto BuildPayload = [&](UPoseAsset* PoseAsset, bool bCreated) -> TSharedPtr<FJsonObject>
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Name);
        Obj->SetStringField(TEXT("package_path"), PackagePath);
        Obj->SetStringField(TEXT("asset_path"), PoseAsset->GetPathName());
        Obj->SetStringField(TEXT("pose_asset_path"), FullPath);
        const USkeleton* PoseSkeleton = PoseAsset->GetSkeleton();
        Obj->SetStringField(TEXT("skeleton"), PoseSkeleton ? PoseSkeleton->GetPathName() : FString());
        Obj->SetNumberField(TEXT("num_poses"), PoseAsset->GetNumPoses());
        Obj->SetBoolField(TEXT("created"), bCreated);
        return Obj;
    };

    // Idempotent: if it already exists, load + return a stable payload. Nothing changed, so nothing is saved.
    if (UPoseAsset* Existing = AnimAssetUtils::FindExisting<UPoseAsset>(FullPath))
    {
        TSharedPtr<FJsonObject> Obj = BuildPayload(Existing, /*bCreated=*/false);
        Obj->SetBoolField(TEXT("already_existed"), true);
        Obj->SetBoolField(TEXT("saved"), false);
        Obj->SetStringField(TEXT("message"), TEXT("PoseAsset already exists; loaded the existing asset unchanged, nothing saved"));
        return mcp_common::MakeOkReply(Obj);
    }

    // The pose container starts empty; poses can be added later via add_pose_to_pose_asset
    // (UPoseAsset::AddReferencePose).
    FString CreateError;
    UPoseAsset* PoseAsset = mcp_common::NewAssetInPackage<UPoseAsset>(FullPath, Name,
        TEXT("Failed to create UPoseAsset"), CreateError);
    if (!PoseAsset)
    {
        return mcp_common::MakeErrorReply(CreateError);
    }

    PoseAsset->SetSkeleton(Skeleton);

    FAssetRegistryModule::AssetCreated(PoseAsset);

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(PoseAsset->GetPackage(), PoseAsset, SaveError);

    TSharedPtr<FJsonObject> Obj = BuildPayload(PoseAsset, /*bCreated=*/true);
    Obj->SetBoolField(TEXT("already_existed"), false);
    Obj->SetStringField(TEXT("message"), bSaved
        ? TEXT("PoseAsset created and saved to disk (no poses yet; add reference poses via add_pose_to_pose_asset)")
        : FString::Printf(TEXT("PoseAsset created but save failed: %s"), *SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("create_pose_asset"), SaveError);
}

namespace
{
    // Whether two transforms hold the same values, component for component, with no tolerance: the pose asset
    // stores what it is given, so only an exact match is a pose the add would leave unchanged.
    bool AnimAuthoring_SameTransform(const FTransform& A, const FTransform& B)
    {
        return A.GetTranslation() == B.GetTranslation() && A.GetRotation() == B.GetRotation()
            && A.GetScale3D() == B.GetScale3D();
    }

#if UE_VERSION_OLDER_THAN(5, 8, 0)
    // UE 5.7 has no UPoseAsset::GetFullCurveValues. What it returns in 5.8, the pose's FPoseData::SourceCurveData (its
    // full, non-additive curve values), read through the reflected private PoseContainer.Poses; empty for an index out
    // of range, as in 5.8.
    TArray<float> AnimAuthoring_FullCurveValues(const UPoseAsset* PoseAsset, int32 PoseIndex)
    {
        const FStructProperty* ContainerProp = FindFProperty<FStructProperty>(UPoseAsset::StaticClass(), TEXT("PoseContainer"));
        const FArrayProperty* PosesProp = ContainerProp ? FindFProperty<FArrayProperty>(ContainerProp->Struct, TEXT("Poses")) : nullptr;
        if (!PosesProp)
        {
            return TArray<float>();
        }
        const TArray<FPoseData>& Poses = *PosesProp->ContainerPtrToValuePtr<TArray<FPoseData>>(
            ContainerProp->ContainerPtrToValuePtr<void>(PoseAsset));
        return Poses.IsValidIndex(PoseIndex) ? Poses[PoseIndex].SourceCurveData : TArray<float>();
    }
#endif

    // Whether UPoseAsset::AddReferencePose(PoseName, RefSkeleton) would leave the asset as it is (PoseAsset.cpp:
    // AddReferencePose, AddOrUpdatePose, CombineTracks and PostProcessData): the pose exists; its tracks are
    // exactly the bones of the skeleton, so no track would be inserted and none dropped as invalid; each track
    // already holds its bone's reference pose; every curve value is zero; and there is no source-animation GUID
    // for the add to invalidate.
    bool AnimAuthoring_PoseIsReferencePose(const UPoseAsset* PoseAsset, FName PoseName, const FReferenceSkeleton& RefSkeleton)
    {
        const int32 PoseIndex = PoseAsset->GetPoseIndexByName(PoseName);
        if (PoseIndex == INDEX_NONE || PoseAsset->SourceAnimationRawDataGUID.IsValid())
        {
            return false;
        }
        const TArray<FName>& Tracks = PoseAsset->GetTrackNames();
#if UE_VERSION_OLDER_THAN(5, 8, 0)
        // 5.7 has only the copying overload; both return the pose's FPoseData::SourceLocalSpacePose.
        TArray<FTransform> SourcePose;
        PoseAsset->GetFullPose(PoseIndex, SourcePose);
#else
        const TArray<FTransform>& SourcePose = PoseAsset->GetFullPose(PoseIndex);
#endif
        const TArray<FTransform>& RefPose = RefSkeleton.GetRefBonePose();
        if (Tracks.Num() != RefSkeleton.GetNum() || SourcePose.Num() != Tracks.Num())
        {
            return false;
        }
        for (int32 BoneIndex = 0; BoneIndex < RefSkeleton.GetNum(); ++BoneIndex)
        {
            if (!Tracks.Contains(RefSkeleton.GetBoneName(BoneIndex)))
            {
                return false;
            }
        }
        for (int32 TrackIndex = 0; TrackIndex < Tracks.Num(); ++TrackIndex)
        {
            const int32 BoneIndex = RefSkeleton.FindBoneIndex(Tracks[TrackIndex]);
            if (!RefPose.IsValidIndex(BoneIndex) || !AnimAuthoring_SameTransform(SourcePose[TrackIndex], RefPose[BoneIndex]))
            {
                return false;
            }
        }
#if UE_VERSION_OLDER_THAN(5, 8, 0)
        const TArray<float> CurveValues = AnimAuthoring_FullCurveValues(PoseAsset, PoseIndex);
#else
        const TArray<float>& CurveValues = PoseAsset->GetFullCurveValues(PoseIndex);
#endif
        if (CurveValues.Num() != PoseAsset->GetNumCurves())
        {
            return false;
        }
        for (const float CurveValue : CurveValues)
        {
            if (CurveValue != 0.0f)
            {
                return false;
            }
        }
        return true;
    }
}

// ---------------------------------------------------------------------------
// add_pose_to_pose_asset
// ---------------------------------------------------------------------------
// MCP:add_pose_to_pose_asset - Add a pose copied from the skeleton reference pose to a UPoseAsset under the given name (a pose of that name is replaced) in an undo transaction and save it; refused when the asset has no skeleton. A pose of that name that already holds exactly that reference pose (every bone a track, zero curve values, no source animation GUID) is a success with added=false, already_existed=true and saved=false, and the asset is neither modified nor saved. Returns num_poses, num_tracks and already_existed. For poses from animation frames use create_pose_asset_from_anim_sequence. A failed save makes success false, with saved=false and save_error.
//   @param pose_asset_path - Path to the UPoseAsset (e.g. '/Game/MyGame/Pose_Example').
//   @param pose_name - Name of the pose to add (e.g. 'RefPose_A').
//   @returns { pose_asset_path, pose_name, num_poses, num_tracks, present, added, already_existed, saved, save_error, message }
TSharedPtr<FJsonObject> HandleAddPoseToPoseAsset(const TSharedPtr<FJsonObject>& Params)
{
    FString PoseAssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("pose_asset_path"), PoseAssetPath)) { return Missing; }

    FString PoseName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("pose_name"), PoseName)) { return Missing; }

    UPoseAsset* PoseAsset = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(PoseAssetPath, TEXT("PoseAsset"), PoseAsset)) { return Error; }

    const USkeleton* Skeleton = PoseAsset->GetSkeleton();
    if (!Skeleton)
    {
        return mcp_common::MakeErrorReply(TEXT("PoseAsset has no skeleton; cannot add a reference pose"));
    }

    const FName PoseFName(*PoseName);
    const FReferenceSkeleton& RefSkeleton = Skeleton->GetReferenceSkeleton();

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("pose_asset_path"), PoseAsset->GetPathName());
    Obj->SetStringField(TEXT("pose_name"), PoseName);

    // A pose that already holds the reference pose it would copy changes nothing: answered before Modify()
    // would dirty the asset.
    if (AnimAuthoring_PoseIsReferencePose(PoseAsset, PoseFName, RefSkeleton))
    {
        Obj->SetNumberField(TEXT("num_poses"), PoseAsset->GetNumPoses());
        Obj->SetNumberField(TEXT("num_tracks"), PoseAsset->GetNumTracks());
        Obj->SetBoolField(TEXT("present"), true);
        Obj->SetBoolField(TEXT("added"), false);
        Obj->SetBoolField(TEXT("already_existed"), true);
        return AnimAuthoring_NoChangeReply(Obj, TEXT("The pose already holds the skeleton reference pose; nothing changed, nothing saved"));
    }

    const FScopedTransaction Transaction(LOCTEXT("AddPoseToPoseAsset", "Add Pose To Pose Asset"));
    PoseAsset->Modify();

    const bool bAlreadyExisted = PoseAsset->ContainsPose(PoseFName);

    // AddReferencePose builds tracks + a pose purely from the skeleton's reference pose
    // (FReferenceSkeleton::GetRefBonePose) — no source UAnimSequence is required, so this
    // is a fully headless-safe REAL effect. If the named pose already exists it is updated.
    PoseAsset->AddReferencePose(PoseFName, RefSkeleton);

    const bool bPresent = PoseAsset->ContainsPose(PoseFName);
    const int32 NumPoses = PoseAsset->GetNumPoses();

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(PoseAsset, SaveError);

    Obj->SetNumberField(TEXT("num_poses"), NumPoses);
    Obj->SetNumberField(TEXT("num_tracks"), PoseAsset->GetNumTracks());
    Obj->SetBoolField(TEXT("present"), bPresent);
    Obj->SetBoolField(TEXT("added"), bPresent);
    Obj->SetBoolField(TEXT("already_existed"), bAlreadyExisted);
    Obj->SetStringField(TEXT("message"), bSaved
        ? TEXT("Reference pose added to PoseAsset (from skeleton ref pose) and saved to disk")
        : FString::Printf(TEXT("Reference pose added to PoseAsset in-memory; save not persisted (%s)"), *SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("add_pose_to_pose_asset"), SaveError);
}

// ---------------------------------------------------------------------------
// get_anim_asset_info
// ---------------------------------------------------------------------------
// MCP:get_anim_asset_info - Load any animation asset and report its class, skeleton and asset_type (montage, composite, pose, sequence, blendspace or animation_asset) plus the counts of that type: sections/slots/notifies, segments, poses/tracks/curves, length, samples. Read-only; get_montage_info, get_anim_sequence_info and get_blend_space_samples give detail.
//   @param asset_path - Path to the animation asset (UAnimMontage, UAnimComposite, UPoseAsset, UAnimSequence, or UBlendSpace)
//   @returns { asset_path, class, skeleton, num_segments, num_poses, section_count, slot_count, notify_count, sequence_length, rate_scale, num_tracks, num_curves, sample_count, asset_type }
TSharedPtr<FJsonObject> HandleGetAnimAssetInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_path"), AssetPath)) { return Missing; }

    UAnimationAsset* Asset = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(AssetPath, TEXT("Animation asset"), Asset)) { return Error; }

    const USkeleton* Skeleton = Asset->GetSkeleton();

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("asset_path"), Asset->GetPathName());
    Obj->SetStringField(TEXT("class"), Asset->GetClass()->GetName());
    Obj->SetStringField(TEXT("skeleton"), Skeleton ? Skeleton->GetPathName() : FString());

    // Classify (order matters: UAnimMontage and UAnimComposite both derive from
    // UAnimSequenceBase / UAnimCompositeBase, so check the leaf types first).
    FString AssetType = TEXT("animation_asset");

    if (UAnimMontage* Montage = Cast<UAnimMontage>(Asset))
    {
        AssetType = TEXT("montage");
        Obj->SetNumberField(TEXT("num_segments"), 0);
        Obj->SetNumberField(TEXT("num_poses"), 0);
        Obj->SetNumberField(TEXT("section_count"), Montage->CompositeSections.Num());
        Obj->SetNumberField(TEXT("slot_count"), Montage->SlotAnimTracks.Num());
        Obj->SetNumberField(TEXT("notify_count"), Montage->Notifies.Num());
        Obj->SetNumberField(TEXT("sequence_length"), Montage->GetPlayLength());
        Obj->SetNumberField(TEXT("rate_scale"), Montage->RateScale);
    }
    else if (UAnimComposite* Composite = Cast<UAnimComposite>(Asset))
    {
        AssetType = TEXT("composite");
        Obj->SetNumberField(TEXT("num_segments"), Composite->AnimationTrack.AnimSegments.Num());
        Obj->SetNumberField(TEXT("num_poses"), 0);
        Obj->SetNumberField(TEXT("sequence_length"), Composite->AnimationTrack.GetLength());
    }
    else if (UPoseAsset* PoseAsset = Cast<UPoseAsset>(Asset))
    {
        AssetType = TEXT("pose");
        Obj->SetNumberField(TEXT("num_segments"), 0);
        Obj->SetNumberField(TEXT("num_poses"), PoseAsset->GetNumPoses());
        Obj->SetNumberField(TEXT("num_tracks"), PoseAsset->GetNumTracks());
        Obj->SetNumberField(TEXT("num_curves"), PoseAsset->GetNumCurves());
    }
    else if (UAnimSequence* Sequence = Cast<UAnimSequence>(Asset))
    {
        AssetType = TEXT("sequence");
        Obj->SetNumberField(TEXT("num_segments"), 0);
        Obj->SetNumberField(TEXT("num_poses"), 0);
        Obj->SetNumberField(TEXT("sequence_length"), Sequence->GetPlayLength());
    }
    else if (UBlendSpace* BlendSpace = Cast<UBlendSpace>(Asset))
    {
        AssetType = TEXT("blendspace");
        Obj->SetNumberField(TEXT("num_segments"), 0);
        Obj->SetNumberField(TEXT("num_poses"), 0);
        Obj->SetNumberField(TEXT("sample_count"), BlendSpace->GetBlendSamples().Num());
    }
    else
    {
        Obj->SetNumberField(TEXT("num_segments"), 0);
        Obj->SetNumberField(TEXT("num_poses"), 0);
    }

    Obj->SetStringField(TEXT("asset_type"), AssetType);
    return mcp_common::MakeOkReply(Obj);
}

// ===========================================================================
// Blendspace samples / anim notifies
// ===========================================================================

namespace
{
    // ---- Blend space input-smoothing (interpolation) helpers ----
    // EFilterInterpolationType (EngineTypes.h) <-> string. Names match the
    // UMETA DisplayNames where sensible, plus the raw BSIT_ identifier so callers
    // can use either spelling.
    EFilterInterpolationType ParseFilterInterpType(const FString& In, bool& bOutValid)
    {
        bOutValid = true;
        const FString V = In.TrimStartAndEnd();
        if (V.Equals(TEXT("Averaged"), ESearchCase::IgnoreCase)       || V.Equals(TEXT("Average"), ESearchCase::IgnoreCase)          || V.Equals(TEXT("BSIT_Average"), ESearchCase::IgnoreCase))            { return BSIT_Average; }
        if (V.Equals(TEXT("Linear"), ESearchCase::IgnoreCase)         || V.Equals(TEXT("BSIT_Linear"), ESearchCase::IgnoreCase))                                                                       { return BSIT_Linear; }
        if (V.Equals(TEXT("Cubic"), ESearchCase::IgnoreCase)          || V.Equals(TEXT("BSIT_Cubic"), ESearchCase::IgnoreCase))                                                                        { return BSIT_Cubic; }
        if (V.Equals(TEXT("EaseInOut"), ESearchCase::IgnoreCase)      || V.Equals(TEXT("Ease In/Out"), ESearchCase::IgnoreCase)      || V.Equals(TEXT("BSIT_EaseInOut"), ESearchCase::IgnoreCase))          { return BSIT_EaseInOut; }
        if (V.Equals(TEXT("Exponential"), ESearchCase::IgnoreCase)    || V.Equals(TEXT("ExponentialDecay"), ESearchCase::IgnoreCase) || V.Equals(TEXT("BSIT_ExponentialDecay"), ESearchCase::IgnoreCase))  { return BSIT_ExponentialDecay; }
        if (V.Equals(TEXT("SpringDamper"), ESearchCase::IgnoreCase)   || V.Equals(TEXT("Spring Damper"), ESearchCase::IgnoreCase)     || V.Equals(TEXT("BSIT_SpringDamper"), ESearchCase::IgnoreCase))       { return BSIT_SpringDamper; }
        bOutValid = false;
        return BSIT_SpringDamper;
    }
    const TCHAR* FilterInterpTypeToString(EFilterInterpolationType T)
    {
        switch (T)
        {
        case BSIT_Average:          return TEXT("Averaged");
        case BSIT_Linear:           return TEXT("Linear");
        case BSIT_Cubic:            return TEXT("Cubic");
        case BSIT_EaseInOut:        return TEXT("EaseInOut");
        case BSIT_ExponentialDecay: return TEXT("Exponential");
        case BSIT_SpringDamper:     return TEXT("SpringDamper");
        default:                    return TEXT("SpringDamper");
        }
    }
    // Serialize one FInterpolationParameter axis to JSON for round-trip reads.
    TSharedPtr<FJsonObject> InterpAxisJson(const FInterpolationParameter& P)
    {
        TSharedPtr<FJsonObject> J = MakeShared<FJsonObject>();
        J->SetNumberField(TEXT("interpolation_time"), P.InterpolationTime);
        J->SetNumberField(TEXT("damping_ratio"), P.DampingRatio);
        J->SetNumberField(TEXT("max_speed"), P.MaxSpeed);
        J->SetStringField(TEXT("interpolation_type"), FilterInterpTypeToString(P.InterpolationType.GetValue()));
        return J;
    }
    // Read UBlendSpace::InterpolationParam[axis] (BlendSpace.h) via reflection
    // and attach x_axis / (for 2D) y_axis interpolation JSON under `interpolation`.
    void ReadBlendSpaceInterpolation(UBlendSpace* BS, TSharedPtr<FJsonObject>& OutObj)
    {
        if (!BS) { return; }
        FStructProperty* InterpProp = FindFProperty<FStructProperty>(UBlendSpace::StaticClass(), TEXT("InterpolationParam"));
        if (!InterpProp) { return; }
        TSharedPtr<FJsonObject> Interp = MakeShared<FJsonObject>();
        if (const FInterpolationParameter* X = InterpProp->ContainerPtrToValuePtr<FInterpolationParameter>(BS, 0))
        {
            Interp->SetObjectField(TEXT("x_axis"), InterpAxisJson(*X));
        }
        if (!BS->IsA<UBlendSpace1D>())
        {
            if (const FInterpolationParameter* Y = InterpProp->ContainerPtrToValuePtr<FInterpolationParameter>(BS, 1))
            {
                Interp->SetObjectField(TEXT("y_axis"), InterpAxisJson(*Y));
            }
        }
        OutObj->SetObjectField(TEXT("interpolation"), Interp);
    }

    // Ensure a notify track exists on an AnimSequenceBase. If it already exists, this
    // is a no-op. Returns true if the track is valid after the call.
    bool EnsureNotifyTrack(UAnimSequenceBase* SeqBase, const FName& TrackName)
    {
        if (!SeqBase)
        {
            return false;
        }
        // Check if the track already exists.
        const bool bExists = SeqBase->AnimNotifyTracks.IndexOfByPredicate(
            [&](const FAnimNotifyTrack& Track) { return Track.TrackName == TrackName; }) != INDEX_NONE;
        if (!bExists)
        {
            UAnimationBlueprintLibrary::AddAnimationNotifyTrack(SeqBase, TrackName, FLinearColor::White);
        }
        // Verify it exists now.
        return SeqBase->AnimNotifyTracks.IndexOfByPredicate(
            [&](const FAnimNotifyTrack& Track) { return Track.TrackName == TrackName; }) != INDEX_NONE;
    }

    // Reads the optional "blend_value": a single number (1D) or an array of up to 3 numbers (2D/3D) into
    // OutValue. False when it is absent.
    bool AnimAuthoring_ReadBlendValue(const TSharedPtr<FJsonObject>& Params, FVector& OutValue)
    {
        double SingleVal = 0.0;
        if (Params->TryGetNumberField(TEXT("blend_value"), SingleVal))
        {
            OutValue.X = static_cast<float>(SingleVal);
            return true;
        }
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (Params->TryGetArrayField(TEXT("blend_value"), Arr) && Arr && Arr->Num() > 0)
        {
            OutValue.X = static_cast<float>((*Arr)[0]->AsNumber());
            if (Arr->Num() > 1)
            {
                OutValue.Y = static_cast<float>((*Arr)[1]->AsNumber());
            }
            if (Arr->Num() > 2)
            {
                OutValue.Z = static_cast<float>((*Arr)[2]->AsNumber());
            }
            return true;
        }
        return false;
    }

    // The loaded UClass derived from BaseClass named Name, else the one named Prefix + Name (C++ and Blueprint
    // classes alike); null when neither exists.
    UClass* AnimAuthoring_FindNotifyClass(const FString& Name, const TCHAR* Prefix, const UClass* BaseClass)
    {
        UClass* Class = FindFirstObjectSafe<UClass>(*Name);
        if (!Class || !Class->IsChildOf(BaseClass))
        {
            const FString PrefixedName = FString::Printf(TEXT("%s%s"), Prefix, *Name);
            Class = FindFirstObjectSafe<UClass>(*PrefixedName);
            if (!Class || !Class->IsChildOf(BaseClass))
            {
                return nullptr;
            }
        }
        return Class;
    }
}

namespace
{
    // Defined with the additive settings commands further down.
    const TCHAR* AdditiveAnimTypeToString(EAdditiveAnimationType T);

    // The most grid steps add_blend_space_sample lets UBlendSpace::ExpandRangeForSample add to one axis.
    constexpr int32 AnimAuthoring_MaxAxisWidenSteps = 1000;

    // UBlendSpace::ExpandRangeForSample (BlendSpace.cpp) run on a copy of one axis, with the engine's float math:
    // true when Value fits the axis after at most MaxSteps steps of widening. False when the engine's loop would
    // never end (a step that is not positive, or one a float can no longer add) or would add more steps than that.
    bool AnimAuthoring_AxisCanWiden(const FBlendParameter& Axis, double Value, int32 MaxSteps)
    {
        const float Delta = Axis.GridNum != 0 ? (Axis.Max - Axis.Min) / Axis.GridNum : 0.0f;
        float Max = Axis.Max;
        float Min = Axis.Min;
        int32 Steps = 0;
        while (Value > Max)
        {
            const float Widened = Max + Delta;
            if (!FMath::IsFinite(Widened) || !(Widened > Max) || ++Steps > MaxSteps)
            {
                return false;
            }
            Max = Widened;
        }
        while (Value < Min)
        {
            const float Widened = Min - Delta;
            if (!FMath::IsFinite(Widened) || !(Widened < Min) || ++Steps > MaxSteps)
            {
                return false;
            }
            Min = Widened;
        }
        return true;
    }

    // Why UBlendSpace::AddSample would reject Animation at Position, asked with the engine's own checks
    // (ValidateAnimationSequence, ValidateSampleValue) BEFORE anything changes; empty when it would take it.
    // AddSample widens an axis before it validates, so a sample it rejects would still have changed the asset.
    FString AnimAuthoring_BlendSampleRefusal(const UBlendSpace* BlendSpace, const UAnimSequence* Animation, const FVector& Position)
    {
        if (!BlendSpace->IsAnimationCompatibleWithSkeleton(Animation))
        {
            const USkeleton* BlendSpaceSkeleton = BlendSpace->GetSkeleton();
            const USkeleton* AnimationSkeleton = Animation->GetSkeleton();
            return FString::Printf(TEXT("the animation's skeleton (%s) is neither the blend space's skeleton (%s) nor compatible with it"),
                AnimationSkeleton ? *AnimationSkeleton->GetPathName() : TEXT("none"),
                BlendSpaceSkeleton ? *BlendSpaceSkeleton->GetPathName() : TEXT("none"));
        }
        const EAdditiveAnimationType AdditiveType = Animation->AdditiveAnimType.GetValue();
        if (!BlendSpace->IsAnimationCompatible(Animation))
        {
            return FString::Printf(TEXT("a %s takes no animation of additive type %s"),
                *BlendSpace->GetClass()->GetName(), AdditiveAnimTypeToString(AdditiveType));
        }
        if (BlendSpace->GetNumberOfBlendSamples() > 0 && !BlendSpace->DoesAnimationMatchExistingSamples(Animation))
        {
            return FString::Printf(TEXT("the animation's additive type %s differs from the existing samples'"),
                AdditiveAnimTypeToString(AdditiveType));
        }
        if (BlendSpace->IsTooCloseToExistingSamplePoint(Position, INDEX_NONE))
        {
            const int32 TakenBy = BlendSpace->GetBlendSamples().IndexOfByPredicate([&Position](const FBlendSample& Sample)
                { return Sample.SampleValue.Equals(Position, UE_KINDA_SMALL_NUMBER); });
            return FString::Printf(TEXT("position (%g, %g, %g) is already taken by sample %d"), Position.X, Position.Y, Position.Z, TakenBy);
        }
        static const TCHAR* const AxisNames[3] = { TEXT("x"), TEXT("y"), TEXT("z") };
        for (int32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
        {
            const FBlendParameter& Axis = BlendSpace->GetBlendParameter(AxisIndex);
            if (!AnimAuthoring_AxisCanWiden(Axis, Position[AxisIndex], AnimAuthoring_MaxAxisWidenSteps))
            {
                return FString::Printf(TEXT("blend_%s %g is outside the %s axis range [%g, %g] (grid %d), which cannot widen to it in %d grid steps or fewer"),
                    AxisNames[AxisIndex], Position[AxisIndex], AxisNames[AxisIndex], Axis.Min, Axis.Max, Axis.GridNum, AnimAuthoring_MaxAxisWidenSteps);
            }
        }
        return FString();
    }
}

// ---------------------------------------------------------------------------
// add_blend_space_sample
// ---------------------------------------------------------------------------
// MCP:add_blend_space_sample - Add one sample (an animation placed at a blend position) to a UBlendSpace or UBlendSpace1D in one undo step, rebuild its runtime data and save the asset. Every call appends a new sample, so it is not a retry-safe call. A sample the engine would reject is refused before anything changes, with success=false, added=false, sample_index -1, saved=false and the reason in the error: an animation whose skeleton is neither the blend space's nor compatible with it, an additive type the blend space does not take or that differs from the existing samples', a position already taken by a sample, or a position an axis cannot widen to in 1000 grid steps or fewer. Otherwise a position past an axis range widens that axis. The sample is read back after the rebuild: animation and blend_x, blend_y, blend_z are what the asset holds at sample_index. Create the asset with create_blend_space and list its samples with get_blend_space_samples; move or swap an existing sample with set_blend_space_sample, and take one out with remove_blend_space_sample. A failed save makes success false, with saved=false and save_error.
//   @param blendspace_path - Object path of the UBlendSpace or UBlendSpace1D asset, e.g. '/Game/Animation/BS_Locomotion' (create_blend_space makes one).
//   @param animation_path - Object path of the UAnimSequence played at this position, on the blend space's skeleton or one compatible with it, e.g. '/Game/Animation/Run_Fwd'.
//   @param blend_value - Blend position in the axis units: one number for a 1D blend space, or [x, y] (up to 3 numbers) for a 2D one, e.g. [0, 300]; default 0 on every axis.
//   @returns { blendspace_path: string, sample_index: int, num_samples: int, added: bool, animation?: string, blend_x?: number, blend_y?: number, blend_z?: number, saved: bool, save_error?: string, message?: string }
TSharedPtr<FJsonObject> HandleAddBlendspaceSample(const TSharedPtr<FJsonObject>& Params)
{
    FString BlendSpacePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blendspace_path"), BlendSpacePath)) { return Missing; }

    FString AnimationPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("animation_path"), AnimationPath)) { return Missing; }

    UBlendSpace* BlendSpace = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(BlendSpacePath, TEXT("BlendSpace"), BlendSpace)) { return Error; }

    UAnimSequence* AnimSeq = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(AnimationPath, TEXT("AnimSequence"), AnimSeq)) { return Error; }

    FVector BlendValue(0.0f);
    AnimAuthoring_ReadBlendValue(Params, BlendValue);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("blendspace_path"), BlendSpace->GetPathName());

    // A sample the engine would reject is refused here, before anything is added, widened, dirtied or saved.
    const FString Refusal = AnimAuthoring_BlendSampleRefusal(BlendSpace, AnimSeq, BlendValue);
    if (!Refusal.IsEmpty())
    {
        Obj->SetNumberField(TEXT("sample_index"), INDEX_NONE);
        Obj->SetNumberField(TEXT("num_samples"), BlendSpace->GetBlendSamples().Num());
        Obj->SetBoolField(TEXT("added"), false);
        Obj->SetBoolField(TEXT("saved"), false);
        return mcp_common::MakeOutcomeReply(Obj, false, FString::Printf(
            TEXT("add_blend_space_sample: the engine rejects this sample: %s; nothing was added or saved"), *Refusal));
    }

    // AddSample widens the axes before it validates: kept to put them back if it still rejects the sample.
    FBlendParameter AxesBefore[3];
    for (int32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
    {
        AxesBefore[AxisIndex] = BlendSpace->GetBlendParameter(AxisIndex);
    }

    const FScopedTransaction Transaction(LOCTEXT("AddBlendspaceSample", "Add Blendspace Sample"));
    BlendSpace->Modify();

    const int32 SampleIndex = BlendSpace->AddSample(AnimSeq, BlendValue);
    if (SampleIndex == INDEX_NONE)
    {
        // A rejection the checks above did not foresee: the axes go back, the call leaves no undo step, nothing is saved.
        if (FStructProperty* BlendParamsProp = FindFProperty<FStructProperty>(UBlendSpace::StaticClass(), TEXT("BlendParameters")))
        {
            for (int32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
            {
                if (FBlendParameter* Axis = BlendParamsProp->ContainerPtrToValuePtr<FBlendParameter>(BlendSpace, AxisIndex))
                {
                    *Axis = AxesBefore[AxisIndex];
                }
            }
        }
        mcp_common::DiscardUndoStepOfThisCall();
        Obj->SetNumberField(TEXT("sample_index"), INDEX_NONE);
        Obj->SetNumberField(TEXT("num_samples"), BlendSpace->GetBlendSamples().Num());
        Obj->SetBoolField(TEXT("added"), false);
        Obj->SetBoolField(TEXT("saved"), false);
        return mcp_common::MakeOutcomeReply(Obj, false,
            TEXT("add_blend_space_sample: the engine rejected the sample (UBlendSpace::AddSample returned -1); nothing was added or saved"));
    }

    // Without this the RUNTIME does not see the blend space.
    RebuildBlendSpaceRuntimeData(BlendSpace);

    // Read back after the rebuild, which snaps samples on an axis that snaps to its grid and drops duplicates: the
    // sample is added only if sample_index still holds this animation.
    const TArray<FBlendSample>& Samples = BlendSpace->GetBlendSamples();
    const bool bAdded = Samples.IsValidIndex(SampleIndex) && Samples[SampleIndex].Animation == AnimSeq;
    Obj->SetNumberField(TEXT("sample_index"), bAdded ? SampleIndex : INDEX_NONE);
    Obj->SetNumberField(TEXT("num_samples"), Samples.Num());
    Obj->SetBoolField(TEXT("added"), bAdded);
    if (!bAdded)
    {
        Obj->SetBoolField(TEXT("saved"), false);
        return mcp_common::MakeOutcomeReply(Obj, false, FString::Printf(
            TEXT("add_blend_space_sample: after the runtime data rebuild, sample %d does not hold the animation (a grid-snapping axis can merge it into an existing sample); nothing was saved, and undo reverts the call"),
            SampleIndex));
    }
    const FBlendSample& Added = Samples[SampleIndex];
    Obj->SetStringField(TEXT("animation"), Added.Animation ? Added.Animation->GetPathName() : FString());
    Obj->SetNumberField(TEXT("blend_x"), Added.SampleValue.X);
    Obj->SetNumberField(TEXT("blend_y"), Added.SampleValue.Y);
    Obj->SetNumberField(TEXT("blend_z"), Added.SampleValue.Z);

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(BlendSpace, SaveError);
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Sample added to blendspace"), bSaved, SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("add_blend_space_sample"), SaveError);
}

namespace
{
    // Build a JSON array of the blendspace's CURRENT samples (post-op snapshot), each with its "index" when
    // bWithIndex: remove/edit return it so callers can re-sync after RemoveAtSwap reorders indices.
    TArray<TSharedPtr<FJsonValue>> BuildBlendSampleArray(const UBlendSpace* BlendSpace, bool bWithIndex = true)
    {
        TArray<TSharedPtr<FJsonValue>> SamplesArray;
        const TArray<FBlendSample>& Samples = BlendSpace->GetBlendSamples();
        for (int32 Index = 0; Index < Samples.Num(); ++Index)
        {
            const FBlendSample& Sample = Samples[Index];
            TSharedPtr<FJsonObject> SampleObj = MakeShared<FJsonObject>();
            if (bWithIndex)
            {
                SampleObj->SetNumberField(TEXT("index"), Index);
            }
            SampleObj->SetStringField(TEXT("animation"), Sample.Animation ? Sample.Animation->GetPathName() : FString());
            SampleObj->SetNumberField(TEXT("blend_x"), Sample.SampleValue.X);
            SampleObj->SetNumberField(TEXT("blend_y"), Sample.SampleValue.Y);
            SampleObj->SetNumberField(TEXT("blend_z"), Sample.SampleValue.Z);
            SampleObj->SetNumberField(TEXT("rate_scale"), Sample.RateScale);
            SamplesArray.Add(MakeShared<FJsonValueObject>(SampleObj));
        }
        return SamplesArray;
    }
}

// ---------------------------------------------------------------------------
// remove_blend_space_sample
// ---------------------------------------------------------------------------
// remove_blend_space_sample and set_blend_space_sample validate the index with IsValidBlendSampleIndex, NEVER
// with GetBlendSample, which checkf-asserts on an invalid index.

// MCP:remove_blend_space_sample - Remove a blend sample (by index) from a BlendSpace. WARNING: the remaining indices REORDER -- the engine removes with a swap, so the LAST sample lands in the deleted slot. Never remove a batch by descending index and assume the rest held still: the full post-op sample list is returned so the caller can re-sync before the next call. A failed save makes success false, with saved=false and save_error.
//   @param blendspace_path - Full asset path to the UBlendSpace.
//   @param index - Index of the sample to remove.
//   @returns { blendspace_path, removed, num_samples, samples, saved, save_error, message }
//   NOTE (impl): the reorder comes from UBlendSpace::DeleteSample using RemoveAtSwap.
TSharedPtr<FJsonObject> HandleRemoveBlendspaceSample(const TSharedPtr<FJsonObject>& Params)
{
    FString BlendSpacePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blendspace_path"), BlendSpacePath)) { return Missing; }

    int32 Index = INDEX_NONE;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("index"), Index)) { return Missing; }

    UBlendSpace* BlendSpace = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(BlendSpacePath, TEXT("BlendSpace"), BlendSpace)) { return Error; }

    if (!BlendSpace->IsValidBlendSampleIndex(Index))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Invalid blend sample index %d (num_samples=%d)"), Index, BlendSpace->GetNumberOfBlendSamples()));
    }

    const FScopedTransaction Transaction(LOCTEXT("RemoveBlendspaceSample", "Remove Blendspace Sample"));
    BlendSpace->Modify();

    const bool bRemoved = BlendSpace->DeleteSample(Index);
    UE_LOG(LogTemp, Verbose, TEXT("remove_blend_space_sample: DeleteSample(%d) on %s -> %s"),
           Index, *BlendSpacePath, bRemoved ? TEXT("true") : TEXT("false"));

    const int32 NumSamples = BlendSpace->GetNumberOfBlendSamples();

    // Without this the RUNTIME does not see the blend space.
    RebuildBlendSpaceRuntimeData(BlendSpace);
    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(BlendSpace, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("blendspace_path"), BlendSpace->GetPathName());
    Obj->SetBoolField(TEXT("removed"), bRemoved);
    Obj->SetNumberField(TEXT("num_samples"), NumSamples);
    Obj->SetArrayField(TEXT("samples"), BuildBlendSampleArray(BlendSpace));
    Obj->SetStringField(TEXT("message"), bSaved
        ? TEXT("Sample removed (RemoveAtSwap reordered remaining indices); saved to disk")
        : FString::Printf(TEXT("Sample removed in-memory; save not persisted (%s)"), *SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("remove_blend_space_sample"), SaveError);
}

// ---------------------------------------------------------------------------
// set_blend_space_sample
// ---------------------------------------------------------------------------
// MCP:set_blend_space_sample - Edit a blend sample (by index): blend value and/or animation. At least one of blend_value / animation_path is required; both are individually optional so you can retime a sample without touching its animation, or swap the animation without moving it. A sample that already has those values is a success with value_edited=false, animation_replaced=false and saved=false, and the blend space is neither modified nor saved. A failed save makes success false, with saved=false and save_error.
//   @renamed-from edit_blend_space_sample
//   @param blendspace_path - Full asset path to the UBlendSpace.
//   @param index - Index of the sample to edit.
//   @param blend_value - New blend position: single number (1D) or array of up to 3 numbers (optional).
//   @param animation_path - Full asset path to a UAnimSequence to replace the sample's animation (optional).
//   @returns { blendspace_path, index, value_edited, animation_replaced, num_samples, samples, saved, save_error, message }
TSharedPtr<FJsonObject> HandleEditBlendspaceSample(const TSharedPtr<FJsonObject>& Params)
{
    FString BlendSpacePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blendspace_path"), BlendSpacePath)) { return Missing; }

    int32 Index = INDEX_NONE;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("index"), Index)) { return Missing; }

    FVector BlendValue(0.0f);
    const bool bHasBlendValue = AnimAuthoring_ReadBlendValue(Params, BlendValue);

    FString AnimationPath;
    const bool bHasAnimation = Params->TryGetStringField(TEXT("animation_path"), AnimationPath) && !AnimationPath.IsEmpty();

    if (!bHasBlendValue && !bHasAnimation)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Nothing to edit: provide at least one of blend_value or animation_path"));
    }

    UBlendSpace* BlendSpace = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(BlendSpacePath, TEXT("BlendSpace"), BlendSpace)) { return Error; }

    if (!BlendSpace->IsValidBlendSampleIndex(Index))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Invalid blend sample index %d (num_samples=%d)"), Index, BlendSpace->GetNumberOfBlendSamples()));
    }

    // Resolve + compatibility pre-check the new animation BEFORE mutating, since
    // ReplaceSampleAnimation does NOT validate the skeleton.
    UAnimSequence* NewAnim = nullptr;
    if (bHasAnimation)
    {
        if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(AnimationPath, TEXT("AnimSequence"), NewAnim)) { return Error; }
        if (!BlendSpace->IsAnimationCompatibleWithSkeleton(NewAnim))
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Animation '%s' is not compatible with the blendspace skeleton"), *AnimationPath));
        }
    }

    // A sample that already has the given value (and is valid) and animation changes nothing: answered before
    // Modify() would dirty the blend space.
    {
        const FBlendSample& Current = BlendSpace->GetBlendSample(Index);
        const bool bValueChanges = bHasBlendValue && (Current.SampleValue != BlendValue || !Current.bIsValid);
        const bool bAnimationChanges = bHasAnimation && Current.Animation != NewAnim;
        if (!bValueChanges && !bAnimationChanges)
        {
            TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
            Obj->SetStringField(TEXT("blendspace_path"), BlendSpace->GetPathName());
            Obj->SetNumberField(TEXT("index"), Index);
            Obj->SetBoolField(TEXT("value_edited"), false);
            Obj->SetBoolField(TEXT("animation_replaced"), false);
            Obj->SetNumberField(TEXT("num_samples"), BlendSpace->GetNumberOfBlendSamples());
            Obj->SetArrayField(TEXT("samples"), BuildBlendSampleArray(BlendSpace));
            return AnimAuthoring_NoChangeReply(Obj, TEXT("The sample already has that blend value and animation; nothing changed, nothing saved"));
        }
    }

    const FScopedTransaction Transaction(LOCTEXT("EditBlendspaceSample", "Edit Blendspace Sample"));
    BlendSpace->Modify();

    bool bValueEdited = false;
    if (bHasBlendValue)
    {
        // EditSampleValue returns false (no mutation) on invalid index or an
        // out-of-bounds / too-close value — propagate as an honest failure, not a crash.
        bValueEdited = BlendSpace->EditSampleValue(Index, BlendValue);
        UE_LOG(LogTemp, Verbose, TEXT("set_blend_space_sample: EditSampleValue(%d, %s) -> %s"),
               Index, *BlendValue.ToString(), bValueEdited ? TEXT("true") : TEXT("false"));
        if (!bValueEdited)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("EditSampleValue rejected blend value %s at index %d (out-of-bounds or too close to an existing sample)"),
                *BlendValue.ToString(), Index));
        }
    }

    bool bAnimationReplaced = false;
    if (bHasAnimation)
    {
        bAnimationReplaced = BlendSpace->ReplaceSampleAnimation(Index, NewAnim);
        UE_LOG(LogTemp, Verbose, TEXT("set_blend_space_sample: ReplaceSampleAnimation(%d, %s) -> %s"),
               Index, *AnimationPath, bAnimationReplaced ? TEXT("true") : TEXT("false"));
        if (!bAnimationReplaced)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("ReplaceSampleAnimation failed at index %d for '%s'"), Index, *AnimationPath));
        }
    }

    const int32 NumSamples = BlendSpace->GetNumberOfBlendSamples();

    // Without this the RUNTIME does not see the blend space.
    RebuildBlendSpaceRuntimeData(BlendSpace);
    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(BlendSpace, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("blendspace_path"), BlendSpace->GetPathName());
    Obj->SetNumberField(TEXT("index"), Index);
    Obj->SetBoolField(TEXT("value_edited"), bValueEdited);
    Obj->SetBoolField(TEXT("animation_replaced"), bAnimationReplaced);
    Obj->SetNumberField(TEXT("num_samples"), NumSamples);
    Obj->SetArrayField(TEXT("samples"), BuildBlendSampleArray(BlendSpace));
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Sample edited"), bSaved, SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("set_blend_space_sample"), SaveError);
}

// ---------------------------------------------------------------------------
// get_blend_space_samples
// ---------------------------------------------------------------------------
// MCP:get_blend_space_samples - Read every sample of a UBlendSpace or UBlendSpace1D in index order (animation path, blend_x/blend_y/blend_z, rate_scale) plus num_samples and the per-axis input smoothing under interpolation. Read-only; change samples with add_blend_space_sample, set_blend_space_sample or remove_blend_space_sample.
//   @param blendspace_path - Full asset path to the UBlendSpace.
//   @returns { animation, blend_x, blend_y, blend_z, rate_scale, blendspace_path, num_samples, samples }
TSharedPtr<FJsonObject> HandleGetBlendspaceSamples(const TSharedPtr<FJsonObject>& Params)
{
    FString BlendSpacePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blendspace_path"), BlendSpacePath)) { return Missing; }

    UBlendSpace* BlendSpace = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(BlendSpacePath, TEXT("BlendSpace"), BlendSpace)) { return Error; }

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("blendspace_path"), BlendSpace->GetPathName());
    Obj->SetNumberField(TEXT("num_samples"), BlendSpace->GetBlendSamples().Num());
    Obj->SetArrayField(TEXT("samples"), BuildBlendSampleArray(BlendSpace, /*bWithIndex=*/false));
    // Round-trip: the per-axis input-smoothing (InterpolationParam).
    ReadBlendSpaceInterpolation(BlendSpace, Obj);
    return mcp_common::MakeOkReply(Obj);
}

// ---------------------------------------------------------------------------
// add_anim_notify
// ---------------------------------------------------------------------------
// MCP:add_anim_notify - Add an instance of a UAnimNotify class at a time on a UAnimSequence notify track (created when missing) and save the sequence; each call adds another event, and an unknown class is an error. Returns time and success. For montages use add_montage_notify; for durations add_anim_notify_state. A failed save makes success false, with saved=false and save_error.
//   @param animation_path - Full asset path to the UAnimSequence.
//   @param notify_name - Class name of a loaded UAnimNotify subclass, tried as given and then with the AnimNotify_ prefix (e.g. 'AN_Footstep', or "PlaySound" for AnimNotify_PlaySound); a Blueprint notify goes by its generated name ending in _C.
//   @param time - Notify trigger time in seconds (default 0)
//   @param track_name - Notify track name (default 'Default')
//   @returns { animation_path, notify_name, time, success, saved, save_error, message }
TSharedPtr<FJsonObject> HandleAddAnimNotify(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimationPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("animation_path"), AnimationPath)) { return Missing; }

    FString NotifyName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("notify_name"), NotifyName)) { return Missing; }

    UAnimSequence* AnimSeq = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(AnimationPath, TEXT("AnimSequence"), AnimSeq)) { return Error; }

    double Time = 0.0;
    Params->TryGetNumberField(TEXT("time"), Time);

    FString TrackNameStr = TEXT("Default");
    Params->TryGetStringField(TEXT("track_name"), TrackNameStr);
    if (TrackNameStr.IsEmpty())
    {
        TrackNameStr = TEXT("Default");
    }

    // Resolve the notify class by name, falling back to the "AnimNotify_" prefix (editor convention).
    const FName NotifyFName(*NotifyName);
    UClass* NotifyClass = AnimAuthoring_FindNotifyClass(NotifyName, TEXT("AnimNotify_"), UAnimNotify::StaticClass());
    if (!NotifyClass)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("AnimNotify class not found: %s"), *NotifyName));
    }

    const FName TrackFName(*TrackNameStr);

    const FScopedTransaction Transaction(LOCTEXT("AddAnimNotify", "Add Anim Notify"));
    AnimSeq->Modify();

    // Ensure the notify track exists before adding the event.
    EnsureNotifyTrack(AnimSeq, TrackFName);

    UAnimNotify* AddedNotify = UAnimationBlueprintLibrary::AddAnimationNotifyEvent(
        AnimSeq, TrackFName, static_cast<float>(Time),
        TSubclassOf<UAnimNotify>(NotifyClass));

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(AnimSeq, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("animation_path"), AnimSeq->GetPathName());
    Obj->SetStringField(TEXT("notify_name"), NotifyName);
    Obj->SetNumberField(TEXT("time"), Time);
    Obj->SetBoolField(TEXT("success"), AddedNotify != nullptr);
    mcp_common::AddSaveOutcome(Obj, bSaved, SaveError);
    Obj->SetStringField(TEXT("message"), AddedNotify
        ? AnimAuthoring_SaveMessage(TEXT("Anim notify added"), bSaved, SaveError)
        : TEXT("Failed to add anim notify (invalid track or time)"));
    // Success is BOTH things: the operation succeeded AND it persisted.
    return mcp_common::MakeOutcomeReply(Obj, (AddedNotify != nullptr) && bSaved,
        ((AddedNotify != nullptr) && bSaved) ? FString() : mcp_common::SavedOutcomeError(TEXT("add_anim_notify")));
}

// ---------------------------------------------------------------------------
// remove_anim_notify — by-NAME symmetric inverse of add_anim_notify.
// ---------------------------------------------------------------------------
// UAnimationBlueprintLibrary::RemoveAnimationNotifyEventsByName removes every
// notify event whose NotifyName matches (fires RefreshCacheGuid + notify-sort
// internally), and returns the count removed. Idempotent: removing a name that
// is absent removes 0 and reports already_absent=true (no fabricated success,
// no save). Addressing mirrors add_anim_notify (by animation path + notify name,
// via the AnimationBlueprintLibrary — NOT remove_montage_notify's by-index path).
// MCP:remove_anim_notify - Remove every notify event whose NotifyName matches notify_name from a UAnimSequence (UAnimationBlueprintLibrary::RemoveAnimationNotifyEventsByName) in an undo transaction and save it. A name no event has is a success with removed=false, already_absent=true and saved=false, and the sequence is neither dirtied nor saved; a failed save makes success false, with saved=false and save_error.
//   @param animation_path - Full asset path to the UAnimSequence.
//   @param notify_name - Name of the notify to remove (matches FAnimNotifyEvent.NotifyName).
//   @returns { animation_path, notify_name, removed, removed_count, already_absent, saved, save_error }
TSharedPtr<FJsonObject> HandleRemoveAnimNotify(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimationPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("animation_path"), AnimationPath)) { return Missing; }

    FString NotifyName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("notify_name"), NotifyName)) { return Missing; }

    UAnimSequence* AnimSeq = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(AnimationPath, TEXT("AnimSequence"), AnimSeq)) { return Error; }

    // The library removes the events whose NotifyName matches; look for one first, so a name no event has
    // neither opens a transaction nor calls Modify(), which would leave the package dirty for nothing.
    const FName NotifyFName(*NotifyName);
    const bool bAnyMatch = AnimSeq->Notifies.ContainsByPredicate(
        [&NotifyFName](const FAnimNotifyEvent& Event) { return Event.NotifyName == NotifyFName; });

    int32 Removed = 0;
    FString SaveError;
    bool bSaved = false;
    if (bAnyMatch)
    {
        {
            const FScopedTransaction Transaction(LOCTEXT("RemoveAnimNotify", "Remove Anim Notify"));
            AnimSeq->Modify();
            // By-name removal: the library call also fires RefreshCacheGuid / notify-sort.
            Removed = UAnimationBlueprintLibrary::RemoveAnimationNotifyEventsByName(AnimSeq, NotifyFName);
        }
        AnimSeq->MarkPackageDirty();
        bSaved = AnimAuthoring_SavePackageOf(AnimSeq, SaveError);
    }
    const bool bRemoved = Removed > 0;

    UE_LOG(LogTemp, Verbose,
        TEXT("[remove_anim_notify] %s notify '%s' -> removed_count=%d (already_absent=%d, saved=%d)"),
        *AnimSeq->GetPathName(), *NotifyName, Removed, bRemoved ? 0 : 1, bSaved ? 1 : 0);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("animation_path"), AnimSeq->GetPathName());
    Obj->SetStringField(TEXT("notify_name"), NotifyName);
    Obj->SetBoolField(TEXT("removed"), bRemoved);
    Obj->SetNumberField(TEXT("removed_count"), Removed);
    Obj->SetBoolField(TEXT("already_absent"), !bRemoved);
    if (!bRemoved)
    {
        // Nothing matched: nothing changed, so nothing was saved.
        Obj->SetBoolField(TEXT("saved"), false);
        return mcp_common::MakeOkReply(Obj);
    }
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("remove_anim_notify"), SaveError);
}

// ---------------------------------------------------------------------------
// add_anim_notify_state
// ---------------------------------------------------------------------------
// MCP:add_anim_notify_state - Add an instance of a UAnimNotifyState class with a start time and duration to a UAnimSequence notify track (created when missing) and save the sequence; each call adds another event, and unknown classes are refused. Returns start_time, duration and success. For instant notifies use add_anim_notify. A failed save makes success false, with saved=false and save_error.
//   @param animation_path - Full asset path to the UAnimSequence.
//   @param notify_state_name - Class name of a loaded UAnimNotifyState subclass, tried as given and then with the AnimNotifyState_ prefix (e.g. "Trail" for AnimNotifyState_Trail); a Blueprint notify state goes by its generated name ending in _C.
//   @param start_time - Notify state start time in seconds (default 0)
//   @param duration - Notify state duration in seconds (default 1.0)
//   @param track_name - Notify track name (default 'Default')
//   @returns { animation_path, notify_state_name, start_time, duration, success, saved, save_error, message }
TSharedPtr<FJsonObject> HandleAddAnimNotifyState(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimationPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("animation_path"), AnimationPath)) { return Missing; }

    FString NotifyStateName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("notify_state_name"), NotifyStateName)) { return Missing; }

    UAnimSequence* AnimSeq = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(AnimationPath, TEXT("AnimSequence"), AnimSeq)) { return Error; }

    double StartTime = 0.0;
    double Duration = 1.0;
    Params->TryGetNumberField(TEXT("start_time"), StartTime);
    Params->TryGetNumberField(TEXT("duration"), Duration);

    FString TrackNameStr = TEXT("Default");
    Params->TryGetStringField(TEXT("track_name"), TrackNameStr);
    if (TrackNameStr.IsEmpty())
    {
        TrackNameStr = TEXT("Default");
    }

    // Resolve the notify state class by name, falling back to the "AnimNotifyState_" prefix.
    UClass* NotifyStateClass = AnimAuthoring_FindNotifyClass(NotifyStateName, TEXT("AnimNotifyState_"),
        UAnimNotifyState::StaticClass());
    if (!NotifyStateClass)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("AnimNotifyState class not found: %s"), *NotifyStateName));
    }

    const FName TrackFName(*TrackNameStr);

    const FScopedTransaction Transaction(LOCTEXT("AddAnimNotifyState", "Add Anim Notify State"));
    AnimSeq->Modify();

    EnsureNotifyTrack(AnimSeq, TrackFName);

    UAnimNotifyState* AddedNotifyState = UAnimationBlueprintLibrary::AddAnimationNotifyStateEvent(
        AnimSeq, TrackFName, static_cast<float>(StartTime), static_cast<float>(Duration),
        TSubclassOf<UAnimNotifyState>(NotifyStateClass));

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(AnimSeq, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("animation_path"), AnimSeq->GetPathName());
    Obj->SetStringField(TEXT("notify_state_name"), NotifyStateName);
    Obj->SetNumberField(TEXT("start_time"), StartTime);
    Obj->SetNumberField(TEXT("duration"), Duration);
    Obj->SetBoolField(TEXT("success"), AddedNotifyState != nullptr);
    mcp_common::AddSaveOutcome(Obj, bSaved, SaveError);
    Obj->SetStringField(TEXT("message"), AddedNotifyState
        ? AnimAuthoring_SaveMessage(TEXT("Anim notify state added"), bSaved, SaveError)
        : TEXT("Failed to add anim notify state (invalid track or time)"));
    // Success is BOTH things: the operation succeeded AND it persisted.
    return mcp_common::MakeOutcomeReply(Obj, (AddedNotifyState != nullptr) && bSaved,
        ((AddedNotifyState != nullptr) && bSaved) ? FString() : mcp_common::SavedOutcomeError(TEXT("add_anim_notify_state")));
}

// ---------------------------------------------------------------------------
// get_anim_notifies
// ---------------------------------------------------------------------------
// MCP:get_anim_notifies - List the notify events of a UAnimSequence without changing it: name, trigger time, duration, track_index and the notify_class or notify_state_class of each, plus num_notifies, and the names of its notify tracks in track order (track_names, track_count). Use get_montage_info for the notifies of a montage.
//   @param animation_path - Full asset path to the UAnimSequence.
//   @returns { animation_path, num_notifies, notifies: [{ name, time, duration, track_index, notify_class?, notify_state_class? }], track_names: [string], track_count }
TSharedPtr<FJsonObject> HandleGetAnimNotifies(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimationPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("animation_path"), AnimationPath)) { return Missing; }

    UAnimSequence* AnimSeq = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(AnimationPath, TEXT("AnimSequence"), AnimSeq)) { return Error; }

    TArray<FAnimNotifyEvent> NotifyEvents;
    UAnimationBlueprintLibrary::GetAnimationNotifyEvents(AnimSeq, NotifyEvents);

    TArray<TSharedPtr<FJsonValue>> NotifiesArray;
    for (const FAnimNotifyEvent& Event : NotifyEvents)
    {
        TSharedPtr<FJsonObject> NotifyObj = MakeShared<FJsonObject>();
        NotifyObj->SetStringField(TEXT("name"), Event.NotifyName.ToString());
        NotifyObj->SetNumberField(TEXT("time"), Event.GetTriggerTime());
        NotifyObj->SetNumberField(TEXT("duration"), Event.Duration);
        NotifyObj->SetNumberField(TEXT("track_index"), Event.TrackIndex);
        // Include the notify class name if an instance is attached.
        if (Event.Notify)
        {
            NotifyObj->SetStringField(TEXT("notify_class"), Event.Notify->GetClass()->GetName());
        }
        if (Event.NotifyStateClass)
        {
            NotifyObj->SetStringField(TEXT("notify_state_class"), Event.NotifyStateClass->GetClass()->GetName());
        }
        NotifiesArray.Add(MakeShared<FJsonValueObject>(NotifyObj));
    }

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("animation_path"), AnimSeq->GetPathName());
    Obj->SetNumberField(TEXT("num_notifies"), NotifyEvents.Num());
    Obj->SetArrayField(TEXT("notifies"), NotifiesArray);

    // The notify track names, in track order (GetAnimationNotifyTrackNames clears and refills the array).
    TArray<FName> TrackNames;
    UAnimationBlueprintLibrary::GetAnimationNotifyTrackNames(AnimSeq, TrackNames);
    TArray<TSharedPtr<FJsonValue>> TrackNamesJson;
    for (const FName& TrackName : TrackNames)
    {
        TrackNamesJson.Add(MakeShared<FJsonValueString>(TrackName.ToString()));
    }
    Obj->SetArrayField(TEXT("track_names"), TrackNamesJson);
    Obj->SetNumberField(TEXT("track_count"), TrackNamesJson.Num());
    return mcp_common::MakeOkReply(Obj);
}

// ===========================================================================
// Animation Data Model authoring (UAnimSequence)
// ===========================================================================

namespace
{
    // Read-back counts off the data model for a sequence (model-driven, not deprecated).
    void FillSequenceModelCounts(const UAnimSequence* Seq, TSharedPtr<FJsonObject>& Obj)
    {
        const IAnimationDataModel* Model = Seq->GetDataModel();
        if (Model)
        {
            Obj->SetNumberField(TEXT("play_length"), Model->GetPlayLength());
            Obj->SetNumberField(TEXT("num_frames"), Model->GetNumberOfFrames());
            const FFrameRate FR = Model->GetFrameRate();
            Obj->SetNumberField(TEXT("frame_rate"), FR.AsDecimal());
            Obj->SetNumberField(TEXT("num_bone_tracks"), Model->GetNumBoneTracks());
            Obj->SetNumberField(TEXT("num_float_curves"), Model->GetNumberOfFloatCurves());
        }
        else
        {
            Obj->SetNumberField(TEXT("play_length"), 0.0);
            Obj->SetNumberField(TEXT("num_frames"), 0);
            Obj->SetNumberField(TEXT("frame_rate"), 0.0);
            Obj->SetNumberField(TEXT("num_bone_tracks"), 0);
            Obj->SetNumberField(TEXT("num_float_curves"), 0);
        }
    }

    // Reads the required "anim_sequence_path" and "curve_name" params; the error reply when one is missing.
    TSharedPtr<FJsonObject> AnimAuthoring_ReadCurveTarget(const TSharedPtr<FJsonObject>& Params, FString& OutAnimPath,
        FString& OutCurveName)
    {
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("anim_sequence_path"), OutAnimPath)) { return Missing; }
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("curve_name"), OutCurveName)) { return Missing; }
        return nullptr;
    }

    // The optional "curve_type" param; "float" when absent or empty.
    FString AnimAuthoring_ReadCurveType(const TSharedPtr<FJsonObject>& Params)
    {
        FString CurveType;
        if (!Params->TryGetStringField(TEXT("curve_type"), CurveType) || CurveType.IsEmpty())
        {
            CurveType = TEXT("float");
        }
        return CurveType;
    }

    // RCT_Transform for the "transform" curve type, RCT_Float for any other.
    ERawCurveTrackTypes AnimAuthoring_CurveTrackType(const FString& CurveType)
    {
        return (CurveType.ToLower() == TEXT("transform"))
            ? ERawCurveTrackTypes::RCT_Transform
            : ERawCurveTrackTypes::RCT_Float;
    }

    // The [time, value] pairs of the "keys" param as curve keys; an entry that is not such a pair is skipped.
    TArray<FRichCurveKey> AnimAuthoring_ReadTimeValueKeys(const TSharedPtr<FJsonObject>& Params)
    {
        TArray<FRichCurveKey> Keys;
        const TArray<TSharedPtr<FJsonValue>>* KeysArray = nullptr;
        if (Params->TryGetArrayField(TEXT("keys"), KeysArray) && KeysArray)
        {
            for (const TSharedPtr<FJsonValue>& Pair : *KeysArray)
            {
                const TArray<TSharedPtr<FJsonValue>>* PairArr = nullptr;
                if (Pair.IsValid() && Pair->TryGetArray(PairArr) && PairArr && PairArr->Num() >= 2)
                {
                    const float T = static_cast<float>((*PairArr)[0]->AsNumber());
                    const float V = static_cast<float>((*PairArr)[1]->AsNumber());
                    Keys.Add(FRichCurveKey(T, V));
                }
            }
        }
        return Keys;
    }

    // Whether the float curve already holds Keys as SetCurveKeys would store them: the same number of keys,
    // each with the same time, value, interpolation and tangent modes (its tangents then derive the same way).
    bool AnimAuthoring_FloatCurveHasKeys(const FFloatCurve* Curve, const TArray<FRichCurveKey>& Keys)
    {
        if (!Curve || Curve->FloatCurve.Keys.Num() != Keys.Num())
        {
            return false;
        }
        for (int32 KeyIndex = 0; KeyIndex < Keys.Num(); ++KeyIndex)
        {
            const FRichCurveKey& Held = Curve->FloatCurve.Keys[KeyIndex];
            const FRichCurveKey& Wanted = Keys[KeyIndex];
            if (Held.Time != Wanted.Time || Held.Value != Wanted.Value || Held.InterpMode != Wanted.InterpMode
                || Held.TangentMode != Wanted.TangentMode || Held.TangentWeightMode != Wanted.TangentWeightMode)
            {
                return false;
            }
        }
        return true;
    }

    // Whether two key arrays are the same, field for field: time, value, modes, tangents and their weights.
    bool AnimAuthoring_SameRichCurveKeys(const TArray<FRichCurveKey>& A, const TArray<FRichCurveKey>& B)
    {
        if (A.Num() != B.Num())
        {
            return false;
        }
        for (int32 KeyIndex = 0; KeyIndex < A.Num(); ++KeyIndex)
        {
            const FRichCurveKey& KeyA = A[KeyIndex];
            const FRichCurveKey& KeyB = B[KeyIndex];
            if (KeyA.Time != KeyB.Time || KeyA.Value != KeyB.Value || KeyA.InterpMode != KeyB.InterpMode
                || KeyA.TangentMode != KeyB.TangentMode || KeyA.TangentWeightMode != KeyB.TangentWeightMode
                || KeyA.ArriveTangent != KeyB.ArriveTangent || KeyA.ArriveTangentWeight != KeyB.ArriveTangentWeight
                || KeyA.LeaveTangent != KeyB.LeaveTangent || KeyA.LeaveTangentWeight != KeyB.LeaveTangentWeight)
            {
                return false;
            }
        }
        return true;
    }

    // Whether the transform curve already holds what IAnimationDataController::SetTransformCurveKeys would write
    // for Transforms at Times (AnimSequencerController.cpp): per key, the location, the rotation as Euler angles
    // and the scale, one linear key per axis, set on each of the nine channels with FRichCurve::SetKeys. SetKeys
    // runs on a copy of each channel, so the tangents it derives are compared too.
    bool AnimAuthoring_TransformCurveHasKeys(const FTransformCurve* Curve, const TArray<FTransform>& Transforms,
        const TArray<float>& Times)
    {
        if (!Curve || Transforms.Num() != Times.Num())
        {
            return false;
        }
        const FVectorCurve* const Channels[3] = { &Curve->TranslationCurve, &Curve->RotationCurve, &Curve->ScaleCurve };
        for (int32 Channel = 0; Channel < 3; ++Channel)
        {
            for (int32 Axis = 0; Axis < 3; ++Axis)
            {
                TArray<FRichCurveKey> Keys;
                Keys.Reserve(Transforms.Num());
                for (int32 KeyIndex = 0; KeyIndex < Transforms.Num(); ++KeyIndex)
                {
                    const FTransform& Value = Transforms[KeyIndex];
                    const FVector Vector = Channel == 0 ? Value.GetLocation()
                        : (Channel == 1 ? Value.GetRotation().Euler() : Value.GetScale3D());
                    Keys.Add(FRichCurveKey(Times[KeyIndex], static_cast<float>(Vector[Axis])));
                }
                const FRichCurve& Held = Channels[Channel]->FloatCurves[Axis];
                FRichCurve Written = Held;
                Written.SetKeys(Keys);
                if (!AnimAuthoring_SameRichCurveKeys(Held.GetConstRefOfKeys(), Written.GetConstRefOfKeys()))
                {
                    return false;
                }
            }
        }
        return true;
    }

    // Tells whether a data-controller write changed what a sequence stores, from the content GUID of its data
    // model (IAnimationDataModel::GenerateGuid, the raw-data key of its compressed data) taken before the write.
    // UAnimationSequencerDataModel hashes the frame count and the times, values and defaults of every bone and
    // curve channel (key counts included), the attributes and the transform curves, so an equal GUID after the
    // write means it left them as they were. When the GUID does not come from that hash (a legacy GUID cached
    // until the first edit of an upgraded asset, or a.AnimDataModel.UseSignatureStateHash=1) the write
    // invalidates it, so it always differs and the write is taken as a change.
    class FAnimAuthoring_SequenceWriteProbe
    {
    public:
        explicit FAnimAuthoring_SequenceWriteProbe(UAnimSequence* InSequence)
            : Sequence(InSequence)
            , Package(InSequence->GetPackage())
            , bWasDirty(Package && Package->IsDirty())
        {
            if (const IAnimationDataModel* Model = Sequence->GetDataModel())
            {
                GuidBefore = Model->GenerateGuid();
            }
        }

        // True when the write left the stored data as it was; the package then goes back to the dirty state it
        // had before, which the sequence changes after any bracketed write of its data model.
        bool RestoreIfUnchanged() const
        {
            const IAnimationDataModel* Model = Sequence->GetDataModel();
            if (!Model || !GuidBefore.IsValid() || Model->GenerateGuid() != GuidBefore)
            {
                return false;
            }
            if (Package)
            {
                Package->SetDirtyFlag(bWasDirty);
            }
            return true;
        }

    private:
        UAnimSequence* Sequence;
        UPackage* Package;
        bool bWasDirty;
        FGuid GuidBefore;
    };
}

// ---------------------------------------------------------------------------
// create_anim_sequence
// ---------------------------------------------------------------------------
// MCP:create_anim_sequence - Create a UAnimSequence at package_path/anim_name bound to the USkeleton at skeleton_path (default the engine SkeletalCube_Skeleton) and save it; its data model gets frame_rate rounded to whole fps and round(length_seconds * fps) frames, but no bone tracks or curves. Idempotent: a UAnimSequence already at that path is returned unchanged (already_existed=true, saved=false, length and rate not applied) and nothing is saved; another asset class there or an unknown skeleton is an error. A failed save makes success false, with saved=false and save_error, and the new asset stays in memory only. Fill it with set_anim_bone_track_keys (identity keys when keys is omitted) or add_anim_float_curve; inspect it with get_anim_sequence_info.
//   @param anim_name - Asset name (no path).
//   @param package_path - Target content package path where the asset is created (default /Game/MCP)
//   @param length_seconds - Sequence length in seconds (default 1.0)
//   @param frame_rate - Sampling frame rate in fps (default 30)
//   @param skeleton_path - Asset path of the target Skeleton
//   @returns { anim_sequence_path, anim_name, skeleton, play_length, frame_rate, num_frames, success, saved, save_error, already_existed, message }
TSharedPtr<FJsonObject> HandleCreateAnimSequence(const TSharedPtr<FJsonObject>& Params)
{
    FString Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("anim_name"), Name)) { return Missing; }

    const FString PackagePath = AnimAuthoring_ReadPackagePath(Params, mcp_common::DefaultAssetFolder());

    double LengthSeconds = 1.0;
    Params->TryGetNumberField(TEXT("length_seconds"), LengthSeconds);
    if (LengthSeconds <= 0.0)
    {
        LengthSeconds = 1.0;
    }

    double FrameRateD = 30.0;
    Params->TryGetNumberField(TEXT("frame_rate"), FrameRateD);
    int32 FrameRateInt = FMath::Max(1, FMath::RoundToInt(FrameRateD));

    int32 NumFrames = FMath::Max(1, FMath::RoundToInt(LengthSeconds * static_cast<double>(FrameRateInt)));

    USkeleton* Skeleton = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAuthoring_ReadSkeleton(Params, Skeleton)) { return Error; }

    const FString FullPath = FString::Printf(TEXT("%s/%s"), *PackagePath, *Name);

    auto BuildPayload = [&](UAnimSequence* Seq, bool bSaved) -> TSharedPtr<FJsonObject>
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("anim_sequence_path"), Seq->GetPathName());
        Obj->SetStringField(TEXT("anim_name"), Name);
        const USkeleton* SeqSkeleton = Seq->GetSkeleton();
        Obj->SetStringField(TEXT("skeleton"), SeqSkeleton ? SeqSkeleton->GetPathName() : FString());
        FillSequenceModelCounts(Seq, Obj);
        Obj->SetBoolField(TEXT("success"), bSaved);
        Obj->SetBoolField(TEXT("saved"), bSaved);
        return Obj;
    };

    // Idempotent: if it already exists, load + return a stable payload. Nothing changed, so nothing is saved.
    if (UAnimSequence* Existing = AnimAssetUtils::FindExisting<UAnimSequence>(FullPath))
    {
        TSharedPtr<FJsonObject> Obj = BuildPayload(Existing, /*bSaved=*/false);
        Obj->SetBoolField(TEXT("success"), true);
        Obj->SetBoolField(TEXT("already_existed"), true);
        Obj->SetStringField(TEXT("message"), TEXT("AnimSequence already exists; loaded the existing asset unchanged, nothing saved"));
        return mcp_common::MakeOkReply(Obj);
    }

    FString CreateError;
    UAnimSequence* Seq = mcp_common::NewAssetInPackage<UAnimSequence>(FullPath, Name,
        TEXT("Failed to create UAnimSequence"), CreateError);
    if (!Seq)
    {
        return mcp_common::MakeErrorReply(CreateError);
    }

    // SetSkeleton MUST precede InitializeModel (controller validates bones vs skeleton).
    Seq->SetSkeleton(Skeleton);

    IAnimationDataController& Controller = Seq->GetController();
    Controller.OpenBracket(NSLOCTEXT("MCP", "CreateSeq", "Create Anim Sequence"));
    Controller.InitializeModel();
    Controller.SetFrameRate(FFrameRate(FrameRateInt, 1));
    Controller.SetNumberOfFrames(FFrameNumber(NumFrames));
    Controller.NotifyPopulated();
    Controller.CloseBracket();

    FAssetRegistryModule::AssetCreated(Seq);

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Seq->GetPackage(), Seq, SaveError);

    TSharedPtr<FJsonObject> Obj = BuildPayload(Seq, bSaved);
    Obj->SetBoolField(TEXT("already_existed"), false);
    Obj->SetStringField(TEXT("message"), bSaved
        ? TEXT("AnimSequence created and saved to disk")
        : FString::Printf(TEXT("AnimSequence created but save failed: %s"), *SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("create_anim_sequence"), SaveError);
}

// ---------------------------------------------------------------------------
// set_anim_bone_track_keys — author per-key bone transforms (translation /
// rotation / scale) on a UAnimSequence bone track, or num_keys identity keys
// when no keys are given. IAnimationDataController::SetBoneTrackKeys, read back
// from the data model.
// ---------------------------------------------------------------------------
// MCP:set_anim_bone_track_keys - Replace the keys of one bone track of a UAnimSequence with the given per-key transforms (translation, rotation, scale), or, without keys, with num_keys identity keys (zero translation, identity rotation, unit scale), adding the track when it is missing, through its data controller as one undo step, then save the package; num_keys_readback and first_key are read back from the data model. A track that already holds exactly these keys (the stored track data comes out unchanged, measured with the data model GUID; the engine stores a constant track as one value, whatever num_keys is) is a success with changed=false and saved=false, the package keeps the dirty state it had and nothing is saved. A bone_name the sequence's skeleton does not have is an error and nothing is written; an omitted bone_name targets the root bone. keys and num_keys together, an empty keys array or a num_keys below 1 are refused. A failed save makes success false, with saved=false and save_error.
//   @param anim_sequence_path - Asset path of the UAnimSequence.
//   @param bone_name - Name of a bone of the skeleton of the sequence; omitted or empty means the root bone.
//   @param keys - Optional array of per-key transforms: { translation:[x,y,z], rotation:[pitch,yaw,roll] (deg), scale:[x,y,z] }. translation default [0,0,0], rotation default [0,0,0], scale default [1,1,1]. Omitted: num_keys identity keys are written.
//   @param num_keys - Optional number of identity keys to write when keys is omitted, rounded, >= 1 (default 2); not checked against the frame count of the sequence.
//   @returns { anim_sequence_path, bone_name, num_keys, num_keys_readback, num_bone_tracks, first_key:{translation,rotation_euler}, identity_keys, changed, success, saved, save_error, message }
TSharedPtr<FJsonObject> HandleSetAnimBoneTrackKeys(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("anim_sequence_path"), AnimPath)) { return Missing; }
    FString RequestedBone;
    Params->TryGetStringField(TEXT("bone_name"), RequestedBone);

    // keys (real transforms) or, without them, num_keys identity keys (default 2).
    const TArray<TSharedPtr<FJsonValue>>* KeysArr = nullptr;
    const bool bHasKeys = Params->TryGetArrayField(TEXT("keys"), KeysArr) && KeysArr;
    double NumKeysD = 2.0;
    const bool bHasNumKeys = Params->TryGetNumberField(TEXT("num_keys"), NumKeysD);
    if (bHasKeys && bHasNumKeys)
    {
        return mcp_common::MakeErrorReply(TEXT("Pass keys (per-key transforms) or num_keys (identity keys), not both; nothing was written."));
    }
    if (bHasKeys && KeysArr->Num() == 0)
    {
        return mcp_common::MakeErrorReply(TEXT("'keys' is empty: pass at least one key, or omit it to write num_keys identity keys"));
    }
    if (!bHasKeys && (NumKeysD < 0.5 || NumKeysD > static_cast<double>(TNumericLimits<int32>::Max())))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("num_keys must be at least 1 (got %g); nothing was written."), NumKeysD));
    }
    const int32 IdentityKeyCount = bHasKeys ? 0 : FMath::Max(1, FMath::RoundToInt(NumKeysD));

    UAnimSequence* Seq = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(AnimPath, TEXT("AnimSequence"), Seq)) { return Error; }
    USkeleton* Skeleton = Seq->GetSkeleton();
    if (!Skeleton)
    {
        return mcp_common::MakeErrorReply(TEXT("AnimSequence has no skeleton"));
    }

    // A named bone must exist on the skeleton: PickBoneName falls back to the root bone, and
    // writing these keys to the root track for a misspelled name would replace the root motion.
    if (!RequestedBone.IsEmpty()
        && Skeleton->GetReferenceSkeleton().FindBoneIndex(FName(*RequestedBone)) == INDEX_NONE)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Bone '%s' is not in skeleton %s; nothing was written. List its bones with get_skeleton_info (load_full_data=true)."),
            *RequestedBone, *Skeleton->GetPathName()));
    }

    const FName BoneFName = PickBoneName(Skeleton, RequestedBone);
    if (BoneFName == NAME_None)
    {
        return mcp_common::MakeErrorReply(TEXT("Skeleton has no bones"));
    }

    // Read a [x,y,z] (or [pitch,yaw,roll]) field off a key object, with a default.
    auto ReadVec3 = [](const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, const FVector3f& Default) -> FVector3f
    {
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (Obj->TryGetArrayField(Field, Arr) && Arr && Arr->Num() >= 3)
        {
            return FVector3f(
                static_cast<float>((*Arr)[0]->AsNumber()),
                static_cast<float>((*Arr)[1]->AsNumber()),
                static_cast<float>((*Arr)[2]->AsNumber()));
        }
        return Default;
    };

    TArray<FVector3f> PosKeys;
    TArray<FQuat4f> RotKeys;
    TArray<FVector3f> ScaleKeys;
    if (!bHasKeys)
    {
        // Three identity key arrays of equal, non-zero length.
        PosKeys.Init(FVector3f::ZeroVector, IdentityKeyCount);
        RotKeys.Init(FQuat4f::Identity, IdentityKeyCount);
        ScaleKeys.Init(FVector3f(1.0f, 1.0f, 1.0f), IdentityKeyCount);
    }
    static const TArray<TSharedPtr<FJsonValue>> NoKeys;
    const TArray<TSharedPtr<FJsonValue>>& KeySource = bHasKeys ? *KeysArr : NoKeys;
    for (const TSharedPtr<FJsonValue>& KV : KeySource)
    {
        const TSharedPtr<FJsonObject>* KeyObjPtr = nullptr;
        if (!KV.IsValid() || !KV->TryGetObject(KeyObjPtr) || !KeyObjPtr) { continue; }
        const TSharedPtr<FJsonObject>& KeyObj = *KeyObjPtr;
        const FVector3f Pos = ReadVec3(KeyObj, TEXT("translation"), FVector3f::ZeroVector);
        const FVector3f Euler = ReadVec3(KeyObj, TEXT("rotation"), FVector3f::ZeroVector);
        const FVector3f Scale = ReadVec3(KeyObj, TEXT("scale"), FVector3f(1.0f, 1.0f, 1.0f));
        const FQuat Quat = FRotator(Euler.X, Euler.Y, Euler.Z).Quaternion();  // (pitch, yaw, roll)
        PosKeys.Add(Pos);
        RotKeys.Add(FQuat4f(static_cast<float>(Quat.X), static_cast<float>(Quat.Y),
                            static_cast<float>(Quat.Z), static_cast<float>(Quat.W)));
        ScaleKeys.Add(Scale);
    }
    if (PosKeys.Num() == 0)
    {
        return mcp_common::MakeErrorReply(TEXT("No valid key objects parsed from 'keys'"));
    }

    const int32 NumKeys = PosKeys.Num();
    int32 NumBoneTracks = 0;
    int32 NumKeysReadback = 0;
    FVector3f FirstPos = FVector3f::ZeroVector;
    FVector3f FirstEuler = FVector3f::ZeroVector;
    bool bSaved = false;
    FString SaveError;

    // The public data model has no raw key count (GetBoneTrackTransforms samples every frame), so whether the
    // write changes anything is measured on the stored data, before and after it.
    const FAnimAuthoring_SequenceWriteProbe Probe(Seq);

    IAnimationDataController& Controller = Seq->GetController();
    Controller.OpenBracket(NSLOCTEXT("MCP", "SetBoneTrackKeys", "Set Bone Track Keys"));
    Controller.AddBoneCurve(BoneFName);  // idempotent (no-op if track exists)
    Controller.SetBoneTrackKeys(BoneFName, PosKeys, RotKeys, ScaleKeys, true);
    Controller.CloseBracket();

    if (const IAnimationDataModel* Model = Seq->GetDataModel())
    {
        NumBoneTracks = Model->GetNumBoneTracks();
        TArray<FTransform> OutTransforms;
        Model->GetBoneTrackTransforms(BoneFName, OutTransforms);
        NumKeysReadback = OutTransforms.Num();
        if (OutTransforms.Num() > 0)
        {
            FirstPos = static_cast<FVector3f>(OutTransforms[0].GetTranslation());
            const FRotator R = OutTransforms[0].GetRotation().Rotator();
            FirstEuler = FVector3f(static_cast<float>(R.Pitch), static_cast<float>(R.Yaw), static_cast<float>(R.Roll));
        }
    }

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("anim_sequence_path"), Seq->GetPathName());
    Obj->SetStringField(TEXT("bone_name"), BoneFName.ToString());
    Obj->SetNumberField(TEXT("num_keys"), NumKeys);
    Obj->SetNumberField(TEXT("num_keys_readback"), NumKeysReadback);
    Obj->SetNumberField(TEXT("num_bone_tracks"), NumBoneTracks);
    Obj->SetBoolField(TEXT("identity_keys"), !bHasKeys);
    {
        TSharedPtr<FJsonObject> First = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> T = mcp_common::VectorToJsonArray(FirstPos);
        TArray<TSharedPtr<FJsonValue>> E = mcp_common::VectorToJsonArray(FirstEuler);
        First->SetArrayField(TEXT("translation"), T);
        First->SetArrayField(TEXT("rotation_euler"), E);
        Obj->SetObjectField(TEXT("first_key"), First);
    }

    // A track that already held these keys is left as it was: nothing to save.
    if (Probe.RestoreIfUnchanged())
    {
        // The controller's bracket still recorded a change, which would undo nothing.
        mcp_common::DiscardUndoStepOfThisCall();
        Obj->SetBoolField(TEXT("changed"), false);
        Obj->SetBoolField(TEXT("success"), true);
        return AnimAuthoring_NoChangeReply(Obj, TEXT("The bone track already holds exactly these keys; nothing changed, nothing saved"));
    }

    bSaved = AnimAuthoring_SavePackageOf(Seq, SaveError);

    Obj->SetBoolField(TEXT("changed"), true);
    Obj->SetBoolField(TEXT("success"), bSaved);
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Bone track keys set"), bSaved, SaveError, TEXT("")));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("set_anim_bone_track_keys"), SaveError);
}

// ---------------------------------------------------------------------------
// rename_pose_in_pose_asset — rename a named pose in a UPoseAsset, with a read
// back of the post-rename pose-name list.
// ---------------------------------------------------------------------------
// MCP:rename_pose_in_pose_asset - Rename an existing named pose in a UPoseAsset (UPoseAsset::ModifyPoseName) in an undo transaction and save it; renamed is measured by reading the pose names back. An unknown old_pose_name, a new_pose_name another pose already has, or a rename the engine does not take is an error with nothing saved; the same name for both is a success with renamed=false and saved=false, and nothing is modified or saved. A failed save makes success false, with saved=false and save_error.
//   @param pose_asset_path - Path to the UPoseAsset.
//   @param old_pose_name - Current name of the pose to rename.
//   @param new_pose_name - New name for the pose.
//   @returns { pose_asset_path, old_pose_name, new_pose_name, renamed, contains_new, contains_old, num_poses, pose_names, saved, save_error, message }
TSharedPtr<FJsonObject> HandleRenamePoseInPoseAsset(const TSharedPtr<FJsonObject>& Params)
{
    FString PoseAssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("pose_asset_path"), PoseAssetPath)) { return Missing; }
    FString OldName, NewName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("old_pose_name"), OldName)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("new_pose_name"), NewName)) { return Missing; }

    UPoseAsset* PoseAsset = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(PoseAssetPath, TEXT("PoseAsset"), PoseAsset)) { return Error; }

    const FName OldFName(*OldName);
    const FName NewFName(*NewName);

    if (!PoseAsset->ContainsPose(OldFName))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Pose '%s' not found in PoseAsset"), *OldName));
    }

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("pose_asset_path"), PoseAsset->GetPathName());
    Obj->SetStringField(TEXT("old_pose_name"), OldName);
    Obj->SetStringField(TEXT("new_pose_name"), NewName);
    const auto AddPoseState = [&]()
    {
        Obj->SetBoolField(TEXT("contains_new"), PoseAsset->ContainsPose(NewFName));
        Obj->SetBoolField(TEXT("contains_old"), PoseAsset->ContainsPose(OldFName));
        Obj->SetNumberField(TEXT("num_poses"), PoseAsset->GetNumPoses());
        Obj->SetArrayField(TEXT("pose_names"), CollectPoseNames(PoseAsset));
    };

    // Renaming a pose to the name it already has changes nothing.
    if (OldFName == NewFName)
    {
        Obj->SetBoolField(TEXT("renamed"), false);
        AddPoseState();
        return AnimAuthoring_NoChangeReply(Obj, TEXT("The pose already has that name; nothing changed, nothing saved"));
    }
    // ModifyPoseName refuses a name another pose already has; refused here before Modify() would dirty the asset.
    if (PoseAsset->ContainsPose(NewFName))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("PoseAsset already has a pose named '%s'; nothing was renamed"), *NewName));
    }

    bool bRenamed = false;
    {
        FScopedTransaction Transaction(NSLOCTEXT("MCP", "RenamePoseInPoseAsset", "Rename Pose In Pose Asset"));
        PoseAsset->Modify();
        // Measured, not trusted: the rename took when the new name is there and the old one is gone.
        bRenamed = PoseAsset->ModifyPoseName(OldFName, NewFName)
            && PoseAsset->ContainsPose(NewFName) && !PoseAsset->ContainsPose(OldFName);
        if (!bRenamed)
        {
            Transaction.Cancel();
        }
    }
    if (!bRenamed)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("The engine did not rename pose '%s' to '%s' (UPoseAsset::ModifyPoseName); nothing was saved"), *OldName, *NewName));
    }

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(PoseAsset, SaveError);

    Obj->SetBoolField(TEXT("renamed"), true);
    AddPoseState();
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Pose renamed"), bSaved, SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("rename_pose_in_pose_asset"), SaveError);
}

// ===========================================================================
// Pose CRUD (remove pose / base pose / remove curve)
//               + float-curve metadata (color / comment / scale)

// ---------------------------------------------------------------------------
// remove_pose_from_pose_asset
// ---------------------------------------------------------------------------
// MCP:remove_pose_from_pose_asset - Delete the named pose from a UPoseAsset (UPoseAsset::DeletePoses, which also fixes the base pose index) in an undo transaction and save the asset. An absent name is a success with removed=false and saved=false, and the asset is neither modified nor saved. Returns was_present, removed_count, contains_after, num_poses and the remaining pose_names. A failed save makes success false, with saved=false and save_error.
//   @param pose_asset_path - Path to the UPoseAsset (e.g. '/Game/MyGame/Pose_Example').
//   @param pose_name - Name of the pose to remove.
//   @returns { pose_asset_path, pose_name, was_present, removed_count, removed, contains_after, num_poses, pose_names, saved, save_error, message }
TSharedPtr<FJsonObject> HandleRemovePoseFromPoseAsset(const TSharedPtr<FJsonObject>& Params)
{
    FString PoseAssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("pose_asset_path"), PoseAssetPath)) { return Missing; }
    FString PoseName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("pose_name"), PoseName)) { return Missing; }

    UPoseAsset* PoseAsset = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(PoseAssetPath, TEXT("PoseAsset"), PoseAsset)) { return Error; }

    const FName PoseFName(*PoseName);

    // Idempotency guard: only the present case actually deletes; an absent name is a
    // clean no-op (removed=false, removed_count=0), answered before Modify() would dirty the asset.
    const bool bWasPresent = PoseAsset->ContainsPose(PoseFName);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("pose_asset_path"), PoseAsset->GetPathName());
    Obj->SetStringField(TEXT("pose_name"), PoseName);
    Obj->SetBoolField(TEXT("was_present"), bWasPresent);
    if (!bWasPresent)
    {
        Obj->SetNumberField(TEXT("removed_count"), 0);
        Obj->SetBoolField(TEXT("removed"), false);
        Obj->SetBoolField(TEXT("contains_after"), false);
        Obj->SetNumberField(TEXT("num_poses"), PoseAsset->GetNumPoses());
        Obj->SetArrayField(TEXT("pose_names"), CollectPoseNames(PoseAsset));
        return AnimAuthoring_NoChangeReply(Obj, TEXT("Pose not present; nothing changed, nothing saved"));
    }

    // DeletePoses fixes BasePoseIndex, calls PostProcessData and broadcasts OnPoseListChanged.
    // It does NOT open its own transaction, so the FScopedTransaction below owns undo.
    // The returned count is the honest number of poses actually removed.
    int32 RemovedCount = 0;
    {
        const FScopedTransaction Transaction(NSLOCTEXT("MCP", "RemovePoseFromPoseAsset", "Remove Pose From Pose Asset"));
        PoseAsset->Modify();
        RemovedCount = PoseAsset->DeletePoses({ PoseFName });
    }

    const bool bContainsAfter = PoseAsset->ContainsPose(PoseFName);

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(PoseAsset, SaveError);

    Obj->SetNumberField(TEXT("removed_count"), RemovedCount);
    Obj->SetBoolField(TEXT("removed"), RemovedCount > 0);
    Obj->SetBoolField(TEXT("contains_after"), bContainsAfter);
    Obj->SetNumberField(TEXT("num_poses"), PoseAsset->GetNumPoses());
    Obj->SetArrayField(TEXT("pose_names"), CollectPoseNames(PoseAsset));
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Pose removed from PoseAsset"), bSaved, SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("remove_pose_from_pose_asset"), SaveError);
}

// ---------------------------------------------------------------------------
// set_pose_asset_base_pose
// ---------------------------------------------------------------------------
// MCP:set_pose_asset_base_pose - Set which named pose of a UPoseAsset is its base pose (UPoseAsset::SetBasePoseName) in an undo transaction, then save the asset; an empty or None base_pose_name goes back to the skeleton reference pose. A name the asset does not contain is an error before anything changes, and so is a base pose the engine does not take (read back from the asset), with nothing saved. The base pose the asset already has is a success with applied=false and saved=false, and nothing is modified or saved. A failed save makes success false, with saved=false and save_error. The base pose only drives evaluation of an additive pose asset, which additive and note report. Edit poses with add_pose_to_pose_asset, rename_pose_in_pose_asset and remove_pose_from_pose_asset.
//   @param pose_asset_path - Path to the UPoseAsset.
//   @param base_pose_name - Name of the pose to use as the base; empty/None resets to the reference pose.
//   @returns { pose_asset_path, base_pose_name, requested_name, applied, base_pose_index, is_reference_pose, additive, note, saved, save_error, message }
TSharedPtr<FJsonObject> HandleSetPoseAssetBasePose(const TSharedPtr<FJsonObject>& Params)
{
    FString PoseAssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("pose_asset_path"), PoseAssetPath)) { return Missing; }
    // Optional: empty/absent base_pose_name means NAME_None -> reset to reference pose.
    FString BasePoseName;
    Params->TryGetStringField(TEXT("base_pose_name"), BasePoseName);

    UPoseAsset* PoseAsset = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(PoseAssetPath, TEXT("PoseAsset"), PoseAsset)) { return Error; }

    const FName BasePoseFName = BasePoseName.IsEmpty() ? NAME_None : FName(*BasePoseName);

    // Honest guard: a non-None name that does not exist would make SetBasePoseName return
    // false; surface that as a clean error rather than a silent no-op.
    if (BasePoseFName != NAME_None && !PoseAsset->ContainsPose(BasePoseFName))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Pose '%s' not found in PoseAsset; cannot set as base pose"), *BasePoseName));
    }

    // EXPORT-MACRO GOTCHA: UPoseAsset::SetBasePoseName is a bare UFUNCTION(BlueprintCallable)
    // WITHOUT ENGINE_API on the method line, so a direct cross-module C++ call would LNK2019.
    // Invoke it through the reflection system (ProcessEvent) instead. Looked up before anything is touched.
    UFunction* Func = PoseAsset->FindFunction(TEXT("SetBasePoseName"));
    if (!Func)
    {
        return mcp_common::MakeErrorReply(
            TEXT("UPoseAsset::SetBasePoseName reflection function not found"));
    }

    // The index SetBasePoseName gives the name (INDEX_NONE for the reference pose), read through the
    // inline-exported GetNumPoses() + GetPoseNameByIndex(), as the result below is.
    int32 WantedIndex = INDEX_NONE;
    if (BasePoseFName != NAME_None)
    {
        for (int32 PoseIndex = 0; PoseIndex < PoseAsset->GetNumPoses(); ++PoseIndex)
        {
            if (PoseAsset->GetPoseNameByIndex(PoseIndex) == BasePoseFName)
            {
                WantedIndex = PoseIndex;
                break;
            }
        }
    }

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("pose_asset_path"), PoseAsset->GetPathName());
    Obj->SetStringField(TEXT("requested_name"), BasePoseName);
    // Read back via the inline-exported GetBasePoseIndex() + GetPoseNameByIndex() (both are
    // header-bodied / safe cross-module, unlike the un-exported GetBasePoseName()).
    const auto AddBaseState = [&]()
    {
        const int32 BaseIndex = PoseAsset->GetBasePoseIndex();
        const bool bIsRefPose = (BaseIndex == INDEX_NONE);
        const bool bAdditive = PoseAsset->IsValidAdditive();
        Obj->SetStringField(TEXT("base_pose_name"), bIsRefPose ? FString() : PoseAsset->GetPoseNameByIndex(BaseIndex).ToString());
        Obj->SetNumberField(TEXT("base_pose_index"), BaseIndex);
        Obj->SetBoolField(TEXT("is_reference_pose"), bIsRefPose);
        Obj->SetBoolField(TEXT("additive"), bAdditive);
        // The base pose only drives evaluation when the PoseAsset is additive; it is settable
        // regardless, so we note this honestly rather than rejecting the call.
        Obj->SetStringField(TEXT("note"), bAdditive
            ? TEXT("Base pose set on an additive PoseAsset (drives additive evaluation)")
            : TEXT("Base pose recorded; only meaningful when the PoseAsset is additive (this one is not)"));
    };

    // The base pose the asset already has changes nothing: answered before Modify() would dirty it.
    if (PoseAsset->GetBasePoseIndex() == WantedIndex)
    {
        Obj->SetBoolField(TEXT("applied"), false);
        AddBaseState();
        return AnimAuthoring_NoChangeReply(Obj, TEXT("The PoseAsset already has that base pose; nothing changed, nothing saved"));
    }

    bool bApplied = false;
    {
        FScopedTransaction Transaction(NSLOCTEXT("MCP", "SetPoseAssetBasePose", "Set Pose Asset Base Pose"));
        PoseAsset->Modify();
        // The param frame mirrors the UFUNCTION signature: input FName followed by the bool return value.
        struct FSetBasePoseNameParms { FName NewBasePoseName; bool ReturnValue; };
        FSetBasePoseNameParms Parms;
        Parms.NewBasePoseName = BasePoseFName;
        Parms.ReturnValue = false;
        PoseAsset->ProcessEvent(Func, &Parms);
        // Measured, not trusted: the base pose took when the asset now reports the wanted index.
        bApplied = Parms.ReturnValue && PoseAsset->GetBasePoseIndex() == WantedIndex;
        if (!bApplied)
        {
            Transaction.Cancel();
        }
    }
    if (!bApplied)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("The engine did not set '%s' as the base pose (UPoseAsset::SetBasePoseName); nothing was saved"),
            *BasePoseFName.ToString()));
    }

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(PoseAsset, SaveError);

    Obj->SetBoolField(TEXT("applied"), true);
    AddBaseState();
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Base pose set"), bSaved, SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("set_pose_asset_base_pose"), SaveError);
}

// ---------------------------------------------------------------------------
// remove_pose_curve
// ---------------------------------------------------------------------------
// MCP:remove_pose_curve - Delete the named curve from a UPoseAsset (UPoseAsset::DeleteCurves) in an undo transaction and save the asset; an absent curve is a success with removed=false, removed_count 0 and saved=false, and the asset is neither modified nor saved. Returns removed_count, removed and num_curves. A failed save makes success false, with saved=false and save_error.
//   @param pose_asset_path - Path to the UPoseAsset.
//   @param curve_name - Name of the curve to remove.
//   @returns { pose_asset_path, curve_name, removed_count, removed, num_curves, saved, save_error, message }
TSharedPtr<FJsonObject> HandleRemovePoseCurve(const TSharedPtr<FJsonObject>& Params)
{
    FString PoseAssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("pose_asset_path"), PoseAssetPath)) { return Missing; }
    FString CurveName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("curve_name"), CurveName)) { return Missing; }

    UPoseAsset* PoseAsset = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(PoseAssetPath, TEXT("PoseAsset"), PoseAsset)) { return Error; }

    const FName CurveFName(*CurveName);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("pose_asset_path"), PoseAsset->GetPathName());
    Obj->SetStringField(TEXT("curve_name"), CurveName);

    // DeleteCurves removes the PoseContainer curves of that name, the ones GetCurveFNames lists: an absent
    // curve changes nothing and is answered before Modify() would dirty the asset.
    if (!PoseAsset->GetCurveFNames().Contains(CurveFName))
    {
        Obj->SetNumberField(TEXT("removed_count"), 0);
        Obj->SetBoolField(TEXT("removed"), false);
        Obj->SetNumberField(TEXT("num_curves"), PoseAsset->GetCurveFNames().Num());
        return AnimAuthoring_NoChangeReply(Obj, TEXT("Curve not present; nothing changed, nothing saved"));
    }

    // DeleteCurves loops PoseContainer.DeleteCurve + broadcasts OnPoseListChanged and returns
    // the honest count removed. No internal transaction -> FScopedTransaction below owns undo.
    int32 RemovedCount = 0;
    {
        const FScopedTransaction Transaction(NSLOCTEXT("MCP", "RemovePoseCurve", "Remove Pose Curve"));
        PoseAsset->Modify();
        RemovedCount = PoseAsset->DeleteCurves({ CurveFName });
    }

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(PoseAsset, SaveError);

    Obj->SetNumberField(TEXT("removed_count"), RemovedCount);
    Obj->SetBoolField(TEXT("removed"), RemovedCount > 0);
    Obj->SetNumberField(TEXT("num_curves"), PoseAsset->GetCurveFNames().Num());
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Pose curve removed"), bSaved, SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("remove_pose_curve"), SaveError);
}

// ---------------------------------------------------------------------------
// set_anim_curve_color
// ---------------------------------------------------------------------------
// MCP:set_anim_curve_color - Set the editor display color of a FLOAT curve on a UAnimSequence through its data controller (one undo step), then save the package (cosmetic: it tints the curve in the editor, it does not affect evaluation). FLOAT-CURVE ONLY: a name that is not a float curve of the sequence, a transform curve included, is an error and nothing is written; so is a color that is not 3 or 4 numbers. The color the curve already has is a success with applied=false and saved=false, and nothing is written or saved. A failed save makes success false, with saved=false and save_error. Create the curve with add_anim_float_curve; its comment: set_anim_curve_comment.
//   @param anim_sequence_path - Asset path of the UAnimSequence.
//   @param curve_name - Name of the float curve.
//   @param color - Color as [r, g, b] or [r, g, b, a], usually 0..1 (numbers within the float range); a is 1 when omitted, and an omitted color is white [1,1,1,1]. Any other value is an error.
//   @returns { anim_sequence_path, curve_name, color, applied, saved, save_error, message }
//   NOTE (impl): wraps IAnimationDataController::SetCurveColor.
TSharedPtr<FJsonObject> HandleSetAnimCurveColor(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimPath, CurveName;
    if (TSharedPtr<FJsonObject> Error = AnimAuthoring_ReadCurveTarget(Params, AnimPath, CurveName))
    {
        return Error;
    }

    // color=[r, g, b] or [r, g, b, a], default opaque white; any other value is refused before the asset is touched.
    FLinearColor Color = FLinearColor::White;
    if (TSharedPtr<FJsonObject> Malformed = mcp_common::ReadJsonLinearColor(Params, TEXT("color"), FLinearColor::White, Color))
    {
        return Malformed;
    }

    UAnimSequence* Seq = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(AnimPath, TEXT("AnimSequence"), Seq)) { return Error; }

    // FLOAT-CURVE ONLY: build the identifier as a float curve and require it to exist.
    const FAnimationCurveIdentifier CurveId(FName(*CurveName), ERawCurveTrackTypes::RCT_Float);
    IAnimationDataController& Controller = Seq->GetController();
    const FFloatCurve* ExistingCurve = Controller.GetModel() ? Controller.GetModel()->FindFloatCurve(CurveId) : nullptr;
    if (!ExistingCurve)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Float curve '%s' does not exist; call add_anim_float_curve first"), *CurveName));
    }

    // The color the curve already has changes nothing: answered before the data controller transacts.
    if (ExistingCurve->Color == Color)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("anim_sequence_path"), Seq->GetPathName());
        Obj->SetStringField(TEXT("curve_name"), CurveName);
        Obj->SetArrayField(TEXT("color"), mcp_common::ColorToJsonArray(Color));
        Obj->SetBoolField(TEXT("applied"), false);
        return AnimAuthoring_NoChangeReply(Obj, TEXT("The curve already has that color; nothing changed, nothing saved"));
    }

    // SetCurveColor self-manages undo via bShouldTransact; do NOT add an outer FScopedTransaction.
    // It Report(Warning)+returns false for non-float curves — the returned bool is the honest `applied`.
    Controller.OpenBracket(NSLOCTEXT("MCP", "SetAnimCurveColor", "Set Anim Curve Color"));
    const bool bApplied = Controller.SetCurveColor(CurveId, Color, /*bShouldTransact=*/true);
    Controller.CloseBracket();

    // The float curve exists (checked above), so false is the data controller refusing the write.
    if (!bApplied)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("The data controller rejected the color for float curve '%s' (see the editor log); nothing was saved"), *CurveName));
    }

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Seq, SaveError);

    TArray<TSharedPtr<FJsonValue>> ColorJson = mcp_common::ColorToJsonArray(Color);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("anim_sequence_path"), Seq->GetPathName());
    Obj->SetStringField(TEXT("curve_name"), CurveName);
    Obj->SetArrayField(TEXT("color"), ColorJson);
    Obj->SetBoolField(TEXT("applied"), bApplied);
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Curve color set"), bSaved, SaveError, TEXT("")));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("set_anim_curve_color"), SaveError);
}

// ---------------------------------------------------------------------------
// set_anim_curve_comment
// ---------------------------------------------------------------------------
// MCP:set_anim_curve_comment - Set the author comment of a FLOAT curve on a UAnimSequence through its data controller (one undo step), then save the package (metadata only; it does not affect evaluation). FLOAT-CURVE ONLY: a name that is not a float curve of the sequence, a transform curve included, is an error and nothing is written. The comment the curve already has (case-sensitive) is a success with applied=false and saved=false, and nothing is written or saved. A failed save makes success false, with saved=false and save_error.
//   @param anim_sequence_path - Asset path of the UAnimSequence.
//   @param curve_name - Name of the float curve.
//   @param comment - Comment string to set (default empty).
//   @returns { anim_sequence_path, curve_name, comment, applied, saved, save_error, message }
//   NOTE (impl): wraps IAnimationDataController::SetCurveComment.
TSharedPtr<FJsonObject> HandleSetAnimCurveComment(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimPath, CurveName;
    if (TSharedPtr<FJsonObject> Error = AnimAuthoring_ReadCurveTarget(Params, AnimPath, CurveName))
    {
        return Error;
    }
    // Comment is optional (empty string is a legitimate "clear comment").
    FString Comment;
    Params->TryGetStringField(TEXT("comment"), Comment);

    UAnimSequence* Seq = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(AnimPath, TEXT("AnimSequence"), Seq)) { return Error; }

    const FAnimationCurveIdentifier CurveId(FName(*CurveName), ERawCurveTrackTypes::RCT_Float);
    IAnimationDataController& Controller = Seq->GetController();
    const FFloatCurve* ExistingCurve = Controller.GetModel() ? Controller.GetModel()->FindFloatCurve(CurveId) : nullptr;
    if (!ExistingCurve)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Float curve '%s' does not exist; call add_anim_float_curve first"), *CurveName));
    }

    // The comment the curve already has (compared case-sensitively) changes nothing: answered before the
    // data controller transacts.
    if (ExistingCurve->Comment.Equals(Comment, ESearchCase::CaseSensitive))
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("anim_sequence_path"), Seq->GetPathName());
        Obj->SetStringField(TEXT("curve_name"), CurveName);
        Obj->SetStringField(TEXT("comment"), Comment);
        Obj->SetBoolField(TEXT("applied"), false);
        return AnimAuthoring_NoChangeReply(Obj, TEXT("The curve already has that comment; nothing changed, nothing saved"));
    }

    // FLOAT-CURVE ONLY: SetCurveComment Report(Warning)+returns false for non-float curves.
    // Self-transacts via bShouldTransact; only bracketed, not wrapped in FScopedTransaction.
    Controller.OpenBracket(NSLOCTEXT("MCP", "SetAnimCurveComment", "Set Anim Curve Comment"));
    const bool bApplied = Controller.SetCurveComment(CurveId, Comment, /*bShouldTransact=*/true);
    Controller.CloseBracket();

    // The float curve exists (checked above), so false is the data controller refusing the write.
    if (!bApplied)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("The data controller rejected the comment for float curve '%s' (see the editor log); nothing was saved"), *CurveName));
    }

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Seq, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("anim_sequence_path"), Seq->GetPathName());
    Obj->SetStringField(TEXT("curve_name"), CurveName);
    Obj->SetStringField(TEXT("comment"), Comment);
    Obj->SetBoolField(TEXT("applied"), bApplied);
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Curve comment set"), bSaved, SaveError, TEXT("")));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("set_anim_curve_comment"), SaveError);
}

// ---------------------------------------------------------------------------
// scale_anim_curve
// ---------------------------------------------------------------------------
// MCP:scale_anim_curve - Retime a FLOAT curve on a UAnimSequence through its data controller (one undo step), then save the package: every key TIME becomes (t - origin) * factor + origin, and the key VALUES are left untouched -- this stretches/compresses a curve in time, it does not scale its amplitude. FLOAT-CURVE ONLY: a name that is not a float curve of the sequence, a transform curve included, is an error and nothing is written. A retime that moves no key (factor 1, no keys, or every key at the origin) is a success with applied=false and saved=false, and nothing is written or saved. A failed save makes success false, with saved=false and save_error.
//   @param anim_sequence_path - Asset path of the UAnimSequence.
//   @param curve_name - Name of the float curve.
//   @param factor - Scale factor applied to each key TIME (default 1.0).
//   @param origin - Time origin the key times are scaled about (default 0.0).
//   @returns { anim_sequence_path, curve_name, origin, factor, num_keys, applied, saved, save_error, message }
//   NOTE (impl): wraps IAnimationDataController::ScaleCurve -> FIndexedCurve::ScaleCurve (SetKeyTime((t-origin)*factor+origin)).
TSharedPtr<FJsonObject> HandleScaleAnimCurve(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimPath, CurveName;
    if (TSharedPtr<FJsonObject> Error = AnimAuthoring_ReadCurveTarget(Params, AnimPath, CurveName))
    {
        return Error;
    }

    double Factor = 1.0;
    double Origin = 0.0;
    Params->TryGetNumberField(TEXT("factor"), Factor);
    Params->TryGetNumberField(TEXT("origin"), Origin);

    UAnimSequence* Seq = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(AnimPath, TEXT("AnimSequence"), Seq)) { return Error; }

    const FAnimationCurveIdentifier CurveId(FName(*CurveName), ERawCurveTrackTypes::RCT_Float);
    IAnimationDataController& Controller = Seq->GetController();
    const FFloatCurve* ExistingCurve = Controller.GetModel() ? Controller.GetModel()->FindFloatCurve(CurveId) : nullptr;
    if (!ExistingCurve)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Float curve '%s' does not exist; call add_anim_float_curve first"), *CurveName));
    }

    // A retime that leaves every key where it is (factor 1, no keys, or every key at the origin) changes
    // nothing: answered before the data controller transacts. Same float arithmetic as
    // FIndexedCurve::ScaleCurve.
    {
        const float OriginF = static_cast<float>(Origin);
        const float FactorF = static_cast<float>(Factor);
        const bool bAnyKeyMoves = ExistingCurve->FloatCurve.Keys.ContainsByPredicate([OriginF, FactorF](const FRichCurveKey& Key)
        {
            return (Key.Time - OriginF) * FactorF + OriginF != Key.Time;
        });
        if (!bAnyKeyMoves)
        {
            TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
            Obj->SetStringField(TEXT("anim_sequence_path"), Seq->GetPathName());
            Obj->SetStringField(TEXT("curve_name"), CurveName);
            Obj->SetNumberField(TEXT("origin"), Origin);
            Obj->SetNumberField(TEXT("factor"), Factor);
            Obj->SetNumberField(TEXT("num_keys"), ExistingCurve->FloatCurve.Keys.Num());
            Obj->SetBoolField(TEXT("applied"), false);
            return AnimAuthoring_NoChangeReply(Obj, TEXT("The retime moves no key of the curve; nothing changed, nothing saved"));
        }
    }

    // FLOAT-CURVE ONLY: ScaleCurve Report(Warning)+returns false for non-float curves.
    // Self-transacts via bShouldTransact; only bracketed, not wrapped in FScopedTransaction.
    Controller.OpenBracket(NSLOCTEXT("MCP", "ScaleAnimCurve", "Scale Anim Curve"));
    const bool bApplied = Controller.ScaleCurve(CurveId, static_cast<float>(Origin), static_cast<float>(Factor), /*bShouldTransact=*/true);
    Controller.CloseBracket();

    // The float curve exists (checked above), so false is the data controller refusing the write.
    if (!bApplied)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("The data controller rejected the scale of float curve '%s' (see the editor log); nothing was saved"), *CurveName));
    }

    int32 NumKeys = 0;
    if (const FFloatCurve* C = Controller.GetModel()->FindFloatCurve(CurveId))
    {
        NumKeys = C->FloatCurve.Keys.Num();
    }

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Seq, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("anim_sequence_path"), Seq->GetPathName());
    Obj->SetStringField(TEXT("curve_name"), CurveName);
    Obj->SetNumberField(TEXT("origin"), Origin);
    Obj->SetNumberField(TEXT("factor"), Factor);
    Obj->SetNumberField(TEXT("num_keys"), NumKeys);
    Obj->SetBoolField(TEXT("applied"), bApplied);
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Curve scaled"), bSaved, SaveError, TEXT("")));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("scale_anim_curve"), SaveError);
}

// ---------------------------------------------------------------------------
// add_anim_float_curve
// ---------------------------------------------------------------------------
// MCP:add_anim_float_curve - Add a float curve (Editable flag) to a UAnimSequence, or rekey the one already there, through its data controller: its keys become the [time, value] pairs as linear keys with auto tangents, in one undo step, then the package is saved. An existing float curve of that name is kept and its keys are replaced (idempotent); one that already holds exactly those keys is a success with changed=false, applied=false and saved=false, and nothing is written or saved. curve_found confirms the curve and num_keys counts the keys sent. A missing sequence is an error, and so are keys the data controller rejects, with nothing saved; a failed save makes success false, with saved=false and save_error. curve_type=transform is refused before anything is loaded, since [time, value] keys only fit a float curve. Transform curves: add_anim_transform_curve; a skeleton-only name: add_anim_curve_name; read back: get_anim_curve_data.
//   @param anim_sequence_path - Asset path of the UAnimSequence.
//   @param curve_name - Float curve name to add or rekey.
//   @param keys - JSON array of [time, value] pairs, time in seconds (default [[0,0],[1,1]]); entries that are not such a pair are skipped.
//   @param curve_type - Optional curve type the keys are meant for: float (the default, the only one written). transform is refused with an error pointing at add_anim_transform_curve; any other value is an error.
//   @returns { anim_sequence_path, curve_name, num_float_curves, num_keys, curve_found, changed, applied, success, saved, save_error, message }
TSharedPtr<FJsonObject> HandleAddAnimFloatCurve(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimPath, CurveName;
    if (TSharedPtr<FJsonObject> Error = AnimAuthoring_ReadCurveTarget(Params, AnimPath, CurveName))
    {
        return Error;
    }

    // curve_type is optional and only float is written. A transform curve holds nine channel curves
    // (translation/rotation/scale by X/Y/Z) that the data controller keys only through an identifier
    // naming the channel and axis; a [time, value] pair addresses none of them, so a caller that meant
    // a transform curve is refused instead of getting a float curve of that name.
    const FString CurveTypeRaw = AnimAuthoring_ReadCurveType(Params);
    const FString CurveTypeLower = CurveTypeRaw.ToLower();
    if (CurveTypeLower == TEXT("transform"))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("add_anim_float_curve takes [time, value] keys, which only a float curve has; '%s' asks for a transform curve. ")
            TEXT("Key a transform curve with add_anim_transform_curve (its keys are {time, location, rotation, scale})."),
            *CurveTypeRaw));
    }
    if (CurveTypeLower != TEXT("float"))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Unsupported curve_type '%s' (add_anim_float_curve writes float curves: use float, or omit it)"), *CurveTypeRaw));
    }

    // keys=[[time,value],...]; default [[0,0],[1,1]].
    TArray<FRichCurveKey> RichKeys = AnimAuthoring_ReadTimeValueKeys(Params);
    if (RichKeys.Num() == 0)
    {
        RichKeys.Add(FRichCurveKey(0.0f, 0.0f));
        RichKeys.Add(FRichCurveKey(1.0f, 1.0f));
    }

    UAnimSequence* Seq = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(AnimPath, TEXT("AnimSequence"), Seq)) { return Error; }

    int32 NumFloatCurves = 0;
    bool bSaved = false;
    bool bCurveFound = false;
    FString SaveError;

    const FName CurveFName(*CurveName);
    const FAnimationCurveIdentifier CurveId(CurveFName, ERawCurveTrackTypes::RCT_Float);

    IAnimationDataController& Controller = Seq->GetController();

    // An existing float curve that already holds these keys changes nothing: answered before the data
    // controller transacts (AddCurve leaves an existing curve and its flags alone).
    if (const IAnimationDataModel* Model = Seq->GetDataModel())
    {
        if (AnimAuthoring_FloatCurveHasKeys(Model->FindFloatCurve(CurveId), RichKeys))
        {
            TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
            Obj->SetStringField(TEXT("anim_sequence_path"), Seq->GetPathName());
            Obj->SetStringField(TEXT("curve_name"), CurveName);
            Obj->SetNumberField(TEXT("num_float_curves"), Model->GetNumberOfFloatCurves());
            Obj->SetNumberField(TEXT("num_keys"), RichKeys.Num());
            Obj->SetBoolField(TEXT("curve_found"), true);
            Obj->SetBoolField(TEXT("changed"), false);
            Obj->SetBoolField(TEXT("applied"), false);
            Obj->SetBoolField(TEXT("success"), true);
            return AnimAuthoring_NoChangeReply(Obj, TEXT("The float curve already exists with those keys; nothing changed, nothing saved"));
        }
    }

    Controller.OpenBracket(NSLOCTEXT("MCP", "AddFloatCurve", "Add Float Curve"));

    // 0x00000004 == AACF_DefaultCurve.
    Controller.AddCurve(CurveId, 0x00000004);
    // SetCurveKeys returns false only when it wrote neither the curve's keys nor its control keys.
    const bool bKeysSet = Controller.SetCurveKeys(CurveId, RichKeys, true);

    Controller.CloseBracket();

    if (!bKeysSet)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("The data controller rejected the keys for float curve '%s' (see the editor log); nothing was saved"),
            *CurveName));
    }

    if (const IAnimationDataModel* Model = Seq->GetDataModel())
    {
        NumFloatCurves = Model->GetNumberOfFloatCurves();
        bCurveFound = (Model->FindFloatCurve(CurveId) != nullptr);
    }

    bSaved = AnimAuthoring_SavePackageOf(Seq, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("anim_sequence_path"), Seq->GetPathName());
    Obj->SetStringField(TEXT("curve_name"), CurveName);
    Obj->SetNumberField(TEXT("num_float_curves"), NumFloatCurves);
    Obj->SetNumberField(TEXT("num_keys"), RichKeys.Num());
    Obj->SetBoolField(TEXT("curve_found"), bCurveFound);
    Obj->SetBoolField(TEXT("changed"), true);
    Obj->SetBoolField(TEXT("applied"), true);
    Obj->SetBoolField(TEXT("success"), true);
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Float curve added"), bSaved, SaveError, TEXT("")));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("add_anim_float_curve"), SaveError);
}

// ---------------------------------------------------------------------------
// add_anim_sync_marker
// ---------------------------------------------------------------------------
// MCP:add_anim_sync_marker - Append an authored sync marker (name and time, on marker track 0) to a UAnimSequence in an undo transaction and save it; duplicates are not checked, nor is the time checked against the sequence length. Returns num_sync_markers. Remove with remove_anim_sync_marker. A failed save makes success false, with saved=false and save_error.
//   @param anim_sequence_path - Asset path of the UAnimSequence.
//   @param marker_name - Name of the sync marker (e.g. "LeftFootDown"); sequences in one sync group line up markers that share a name.
//   @param time - Sync marker time in seconds (default 0.5)
//   @returns { anim_sequence_path, marker_name, time, num_sync_markers, success, saved, save_error, message }
TSharedPtr<FJsonObject> HandleAddAnimSyncMarker(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("anim_sequence_path"), AnimPath)) { return Missing; }

    FString MarkerName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("marker_name"), MarkerName)) { return Missing; }

    double Time = 0.5;
    Params->TryGetNumberField(TEXT("time"), Time);

    UAnimSequence* Seq = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(AnimPath, TEXT("AnimSequence"), Seq)) { return Error; }

    int32 NumSyncMarkers = 0;
    bool bSaved = false;
    FString SaveError;

    const FScopedTransaction Transaction(LOCTEXT("AddAnimSyncMarker", "Add Anim Sync Marker"));
    Seq->Modify();

    FAnimSyncMarker Marker;
    Marker.MarkerName = FName(*MarkerName);
    Marker.Time = static_cast<float>(Time);
    Marker.TrackIndex = 0;
    Seq->AuthoredSyncMarkers.Add(Marker);
    NumSyncMarkers = Seq->AuthoredSyncMarkers.Num();

    bSaved = AnimAuthoring_SavePackageOf(Seq, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("anim_sequence_path"), Seq->GetPathName());
    Obj->SetStringField(TEXT("marker_name"), MarkerName);
    Obj->SetNumberField(TEXT("time"), Time);
    Obj->SetNumberField(TEXT("num_sync_markers"), NumSyncMarkers);
    Obj->SetBoolField(TEXT("success"), true);
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Sync marker added"), bSaved, SaveError, TEXT("")));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("add_anim_sync_marker"), SaveError);
}

// ---------------------------------------------------------------------------
// set_anim_root_motion
// ---------------------------------------------------------------------------
// MCP:set_anim_root_motion - Write the three root motion settings of a UAnimSequence (bEnableRootMotion, RootMotionRootLock, bForceRootLock) and save it. All three are written on every call, so an omitted one is reset to its default; an unknown root lock name applies RefPose while the reply echoes the given text. Settings the sequence already has are a success with changed=false and saved=false, and the sequence is neither modified nor saved. A failed save makes success false, with saved=false and save_error.
//   @param anim_sequence_path - Asset path of the UAnimSequence.
//   @param enable_root_motion - Whether to enable root motion extraction (default true)
//   @param root_motion_root_lock - Root lock mode: RefPose|AnimFirstFrame|Zero (default RefPose)
//   @param force_root_lock - Whether to force the root lock (default false)
//   @returns { anim_sequence_path, enable_root_motion, root_motion_root_lock, force_root_lock, changed, success, saved, save_error, message }
TSharedPtr<FJsonObject> HandleSetAnimRootMotion(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("anim_sequence_path"), AnimPath)) { return Missing; }

    bool bEnableRootMotion = true;
    Params->TryGetBoolField(TEXT("enable_root_motion"), bEnableRootMotion);

    FString RootLockStr = TEXT("RefPose");
    Params->TryGetStringField(TEXT("root_motion_root_lock"), RootLockStr);
    if (RootLockStr.IsEmpty())
    {
        RootLockStr = TEXT("RefPose");
    }

    bool bForceRootLock = false;
    Params->TryGetBoolField(TEXT("force_root_lock"), bForceRootLock);

    UAnimSequence* Seq = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(AnimPath, TEXT("AnimSequence"), Seq)) { return Error; }

    // Map string -> ERootMotionRootLock.
    ERootMotionRootLock::Type Lock = ERootMotionRootLock::RefPose;
    if (RootLockStr.Equals(TEXT("AnimFirstFrame"), ESearchCase::IgnoreCase))
    {
        Lock = ERootMotionRootLock::AnimFirstFrame;
    }
    else if (RootLockStr.Equals(TEXT("Zero"), ESearchCase::IgnoreCase))
    {
        Lock = ERootMotionRootLock::Zero;
    }
    else
    {
        Lock = ERootMotionRootLock::RefPose;
    }

    bool bSaved = false;
    FString SaveError;

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("anim_sequence_path"), Seq->GetPathName());
    Obj->SetBoolField(TEXT("enable_root_motion"), bEnableRootMotion);
    Obj->SetStringField(TEXT("root_motion_root_lock"), RootLockStr);
    Obj->SetBoolField(TEXT("force_root_lock"), bForceRootLock);
    Obj->SetBoolField(TEXT("success"), true);

    // The three settings the sequence already has change nothing: answered before Modify() would dirty it.
    if (Seq->bEnableRootMotion == bEnableRootMotion && Seq->RootMotionRootLock == Lock && Seq->bForceRootLock == bForceRootLock)
    {
        Obj->SetBoolField(TEXT("changed"), false);
        return AnimAuthoring_NoChangeReply(Obj, TEXT("The sequence already has those root motion settings; nothing changed, nothing saved"));
    }

    {
        const FScopedTransaction Transaction(LOCTEXT("SetAnimRootMotion", "Set Anim Root Motion"));
        Seq->Modify();
        Seq->bEnableRootMotion = bEnableRootMotion;
        Seq->RootMotionRootLock = Lock;
        Seq->bForceRootLock = bForceRootLock;
    }

    bSaved = AnimAuthoring_SavePackageOf(Seq, SaveError);

    Obj->SetBoolField(TEXT("changed"), true);
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Root motion settings applied"), bSaved, SaveError, TEXT("")));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("set_anim_root_motion"), SaveError);
}

// ---------------------------------------------------------------------------
// get_anim_sequence_info
// ---------------------------------------------------------------------------
// MCP:get_anim_sequence_info - Summarise one animation sequence (a UAnimSequence asset) from its data model: the skeleton it plays on, its length, frame rate and frame count, the names of its bone tracks, float curves and transform curves with their counts, how many sync markers it has, whether root motion is enabled, and its retarget source and reference-pose sequence. Read-only: it changes nothing and needs no PIE. A path that does not load as an AnimSequence is refused, a montage or blend space included -- get_anim_asset_info reads any animation asset. The keys of one curve come from get_anim_curve_data and the notify events from get_anim_notifies.
//   @param anim_sequence_path - Asset path of the sequence, e.g. "/Game/Anims/Run_Fwd" (package or object path).
//   @param sequence_path - Alias of anim_sequence_path; read only when anim_sequence_path is absent.
//   @returns { anim_sequence_path, skeleton, play_length, frame_rate, num_frames, num_bone_tracks, num_float_curves, transform_curve_count, bone_tracks: [string], float_curves: [string], transform_curves: [string], num_sync_markers, enable_root_motion, retarget_source, ref_pose_seq, success }
TSharedPtr<FJsonObject> HandleGetAnimSequenceInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimPath;
    if (!Params->TryGetStringField(TEXT("anim_sequence_path"), AnimPath) && !Params->TryGetStringField(TEXT("sequence_path"), AnimPath))
    {
        return mcp_common::MissingParameterReply(TEXT("anim_sequence_path"));
    }
    if (AnimPath.IsEmpty())
    {
        return mcp_common::MissingParameterReply(TEXT("anim_sequence_path"));
    }

    UAnimSequence* Seq = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(AnimPath, TEXT("AnimSequence"), Seq)) { return Error; }

    const USkeleton* Skeleton = Seq->GetSkeleton();

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("anim_sequence_path"), Seq->GetPathName());
    Obj->SetStringField(TEXT("skeleton"), Skeleton ? Skeleton->GetPathName() : FString());

    FillSequenceModelCounts(Seq, Obj);

    // The names of the bone tracks, float curves and transform curves of the data model.
    TArray<TSharedPtr<FJsonValue>> BoneTracks;
    TArray<TSharedPtr<FJsonValue>> FloatCurves;
    TArray<TSharedPtr<FJsonValue>> TransformCurves;
    if (const IAnimationDataModel* Model = Seq->GetDataModel())
    {
        TArray<FName> BoneTrackNames;
        Model->GetBoneTrackNames(BoneTrackNames);
        for (const FName& Bone : BoneTrackNames)
        {
            BoneTracks.Add(MakeShared<FJsonValueString>(Bone.ToString()));
        }
        for (const FFloatCurve& Curve : Model->GetFloatCurves())
        {
            FloatCurves.Add(MakeShared<FJsonValueString>(Curve.GetName().ToString()));
        }
        for (const FTransformCurve& Curve : Model->GetTransformCurves())
        {
            TransformCurves.Add(MakeShared<FJsonValueString>(Curve.GetName().ToString()));
        }
    }
    Obj->SetNumberField(TEXT("transform_curve_count"), TransformCurves.Num());
    Obj->SetArrayField(TEXT("bone_tracks"), BoneTracks);
    Obj->SetArrayField(TEXT("float_curves"), FloatCurves);
    Obj->SetArrayField(TEXT("transform_curves"), TransformCurves);

    int32 NumSyncMarkers = Seq->AuthoredSyncMarkers.Num();
    Obj->SetNumberField(TEXT("num_sync_markers"), NumSyncMarkers);
    Obj->SetBoolField(TEXT("enable_root_motion"), Seq->bEnableRootMotion);
    // Round-trip: retarget source (FName) + retarget ref-pose sequence.
    Obj->SetStringField(TEXT("retarget_source"), Seq->RetargetSource.ToString());
    Obj->SetStringField(TEXT("ref_pose_seq"), Seq->RefPoseSeq ? Seq->RefPoseSeq->GetPathName() : FString());
    Obj->SetBoolField(TEXT("success"), true);
    return mcp_common::MakeOkReply(Obj);
}

// ===========================================================================
// Animation real-content (montage segment / transform curve /
//              additive settings / pose-from-animation)
// ===========================================================================

namespace
{
    EAdditiveAnimationType ParseAdditiveAnimType(const FString& S)
    {
        if (S.Equals(TEXT("LocalSpaceBase"), ESearchCase::IgnoreCase) || S.Equals(TEXT("LocalSpace"), ESearchCase::IgnoreCase)) { return AAT_LocalSpaceBase; }
        if (S.Equals(TEXT("RotationOffsetMeshSpace"), ESearchCase::IgnoreCase) || S.Equals(TEXT("MeshSpace"), ESearchCase::IgnoreCase)) { return AAT_RotationOffsetMeshSpace; }
        return AAT_None;
    }
    const TCHAR* AdditiveAnimTypeToString(EAdditiveAnimationType T)
    {
        switch (T)
        {
        case AAT_LocalSpaceBase:           return TEXT("LocalSpaceBase");
        case AAT_RotationOffsetMeshSpace:  return TEXT("RotationOffsetMeshSpace");
        default:                           return TEXT("None");
        }
    }
    EAdditiveBasePoseType ParseBasePoseType(const FString& S)
    {
        if (S.Equals(TEXT("RefPose"), ESearchCase::IgnoreCase) || S.Equals(TEXT("SkeletonReferencePose"), ESearchCase::IgnoreCase)) { return ABPT_RefPose; }
        if (S.Equals(TEXT("AnimScaled"), ESearchCase::IgnoreCase)) { return ABPT_AnimScaled; }
        if (S.Equals(TEXT("AnimFrame"), ESearchCase::IgnoreCase)) { return ABPT_AnimFrame; }
        if (S.Equals(TEXT("LocalAnimFrame"), ESearchCase::IgnoreCase)) { return ABPT_LocalAnimFrame; }
        return ABPT_None;
    }
    const TCHAR* BasePoseTypeToString(EAdditiveBasePoseType T)
    {
        switch (T)
        {
        case ABPT_RefPose:        return TEXT("RefPose");
        case ABPT_AnimScaled:     return TEXT("AnimScaled");
        case ABPT_AnimFrame:      return TEXT("AnimFrame");
        case ABPT_LocalAnimFrame: return TEXT("LocalAnimFrame");
        default:                  return TEXT("None");
        }
    }
}

// ---------------------------------------------------------------------------
// add_montage_segment — append an FAnimSegment referencing a real UAnimSequence
// to a montage slot track, then recompute the montage length.
// ---------------------------------------------------------------------------
// MCP:add_montage_segment - Append an animation segment referencing a real UAnimSequence to a montage slot. A failed save makes success false, with saved=false and save_error. Inverse: remove_montage_segment; read: get_montage_info.
//   @param montage_path - Object path of the UAnimMontage (e.g. "/Game/MyGame/AM_Example").
//   @param anim_sequence_path - Object path of the UAnimSequence to reference.
//   @param slot_name - Montage slot name (e.g. DefaultSlot)
//   @param play_rate - Segment play rate (default 1.0)
//   @param loop_count - Segment loop count, minimum 1 (default 1)
//   @returns { montage_path, anim_sequence_path, slot_name, segment_count, montage_length, play_rate, loop_count, success, saved, save_error }
TSharedPtr<FJsonObject> HandleAddMontageSegment(const TSharedPtr<FJsonObject>& Params)
{
    FString MontagePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("montage_path"), MontagePath)) { return Missing; }
    FString AnimPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("anim_sequence_path"), AnimPath)) { return Missing; }
    FString SlotName = TEXT("DefaultSlot");
    Params->TryGetStringField(TEXT("slot_name"), SlotName);
    if (SlotName.IsEmpty()) { SlotName = TEXT("DefaultSlot"); }

    double PlayRate = 1.0; Params->TryGetNumberField(TEXT("play_rate"), PlayRate);
    double LoopCountD = 1.0; Params->TryGetNumberField(TEXT("loop_count"), LoopCountD);

    UAnimMontage* Montage = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(MontagePath, TEXT("AnimMontage"), Montage)) { return Error; }
    UAnimSequence* AnimSeq = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(AnimPath, TEXT("AnimSequence"), AnimSeq)) { return Error; }
    if (Montage->GetSkeleton() && AnimSeq->GetSkeleton() && Montage->GetSkeleton() != AnimSeq->GetSkeleton())
    {
        return mcp_common::MakeErrorReply(TEXT("Montage and AnimSequence use different skeletons"));
    }

    const FScopedTransaction Transaction(LOCTEXT("AddMontageSegment", "MCP: Add Montage Segment"));
    Montage->Modify();

    const FName SlotFName(*SlotName);
    // By name, not UAnimMontage::IsValidSlot: that is false for an empty track (the one create_anim_montage
    // adds), and adding another would leave a duplicate empty track behind.
    if (!AnimAuthoring_FindSlotTrack(Montage, SlotFName))
    {
        Montage->AddSlot(SlotFName);
    }
    // Find the (now-guaranteed) slot track by name.
    FSlotAnimationTrack* SlotTrack = AnimAuthoring_FindSlotTrack(Montage, SlotFName);
    if (!SlotTrack)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Failed to resolve slot track: %s"), *SlotName));
    }

    FAnimSegment Segment;
    Segment.SetAnimReference(AnimSeq, /*bInitialize=*/true);   // sets AnimStartTime=0, AnimEndTime=anim length
    Segment.AnimPlayRate = static_cast<float>(PlayRate);
    Segment.LoopingCount = FMath::Max(1, static_cast<int32>(LoopCountD));
    SlotTrack->AnimTrack.AnimSegments.Add(Segment);
    SlotTrack->AnimTrack.SortAnimSegments();

    // Recompute the montage length from the segments (ENGINE_API; updates SequenceLength).
    const float NewLength = Montage->CalculateSequenceLength();

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Montage, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("montage_path"), Montage->GetPathName());
    Obj->SetStringField(TEXT("anim_sequence_path"), AnimSeq->GetPathName());
    Obj->SetStringField(TEXT("slot_name"), SlotName);
    Obj->SetNumberField(TEXT("segment_count"), SlotTrack->AnimTrack.AnimSegments.Num());
    Obj->SetNumberField(TEXT("montage_length"), NewLength);
    Obj->SetNumberField(TEXT("play_rate"), PlayRate);
    Obj->SetNumberField(TEXT("loop_count"), Segment.LoopingCount);
    Obj->SetBoolField(TEXT("success"), true);
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("add_montage_segment"), SaveError);
}

// MCP:remove_montage_segment - Remove the segment at segment_index from a slot track of a UAnimMontage, in one undo step, recompute the montage length and save the montage. Inverse of add_montage_segment; the segments after it move down one index, and the gap it leaves on the track stays until collapse_montage_segments closes it. A montage that does not load, a slot it does not have, or an index out of range is an error and nothing changes. The reply re-reads the slot (segment_count, montage_length). A failed save makes success false, with saved=false and save_error. Read the montage: get_montage_info.
//   @param montage_path - Object path of the UAnimMontage (e.g. "/Game/MyGame/AM_Example").
//   @param slot_name - Montage slot name (e.g. DefaultSlot).
//   @param segment_index [int] - Zero-based index of the segment in that slot's track.
//   @returns { montage_path, slot_name, segment_index, removed_anim_sequence_path, removed, segment_count, montage_length, success, saved, save_error }
TSharedPtr<FJsonObject> HandleRemoveMontageSegment(const TSharedPtr<FJsonObject>& Params)
{
    FString MontagePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("montage_path"), MontagePath)) { return Missing; }
    FString SlotName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("slot_name"), SlotName)) { return Missing; }
    int32 SegmentIndex = 0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("segment_index"), SegmentIndex)) { return Missing; }

    UAnimMontage* Montage = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(MontagePath, TEXT("AnimMontage"), Montage)) { return Error; }
    FSlotAnimationTrack* SlotTrack = AnimAuthoring_FindSlotTrack(Montage, FName(*SlotName));
    if (!SlotTrack)
    {
        TArray<FString> Slots;
        for (const FSlotAnimationTrack& Track : Montage->SlotAnimTracks)
        {
            Slots.Add(Track.SlotName.ToString());
        }
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Montage has no slot '%s' (its slots: %s); nothing was changed"),
            *SlotName, *FString::Join(Slots, TEXT(", "))));
    }
    if (!SlotTrack->AnimTrack.AnimSegments.IsValidIndex(SegmentIndex))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("segment_index %d is out of range: slot '%s' has %d segment(s); nothing was changed"),
            SegmentIndex, *SlotName, SlotTrack->AnimTrack.AnimSegments.Num()));
    }

    const UAnimSequenceBase* RemovedAnim = SlotTrack->AnimTrack.AnimSegments[SegmentIndex].GetAnimReference();
    const FString RemovedAnimPath = RemovedAnim ? RemovedAnim->GetPathName() : FString();

    const FScopedTransaction Transaction(LOCTEXT("RemoveMontageSegment", "MCP: Remove Montage Segment"));
    Montage->Modify();
    SlotTrack->AnimTrack.AnimSegments.RemoveAt(SegmentIndex);
    const float NewLength = Montage->CalculateSequenceLength();

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Montage, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("montage_path"), Montage->GetPathName());
    Obj->SetStringField(TEXT("slot_name"), SlotName);
    Obj->SetNumberField(TEXT("segment_index"), SegmentIndex);
    Obj->SetStringField(TEXT("removed_anim_sequence_path"), RemovedAnimPath);
    Obj->SetBoolField(TEXT("removed"), true);
    Obj->SetNumberField(TEXT("segment_count"), SlotTrack->AnimTrack.AnimSegments.Num());
    Obj->SetNumberField(TEXT("montage_length"), NewLength);
    Obj->SetBoolField(TEXT("success"), true);
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("remove_montage_segment"), SaveError);
}

// ---------------------------------------------------------------------------
// add_anim_transform_curve — add a transform curve (RCT_Transform) to a
// UAnimSequence via the data controller.
// ---------------------------------------------------------------------------
// MCP:add_anim_transform_curve - Add a transform curve (RCT_Transform) to a UAnimSequence via the data controller and set its keys as linear keys per axis, in one undo step, then save the package; the sampling rate of the sequence is left as it is. An existing transform curve of that name is kept and its keys are replaced; one that already holds exactly those keys is a success with changed=false, keys_set=false and saved=false, and nothing is written or saved. A failed save makes success false, with saved=false and save_error.
//   @param anim_sequence_path - Object path of the UAnimSequence.
//   @param curve_name - Name of the transform curve to add.
//   @param keys - JSON array of {time, location:[x,y,z], rotation:[pitch,yaw,roll], scale:[x,y,z]} objects
//   @returns { anim_sequence_path, curve_name, num_transform_curves, num_keys, keys_set, changed, success, saved, save_error, message? }
TSharedPtr<FJsonObject> HandleAddAnimTransformCurve(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimPath, CurveName;
    if (TSharedPtr<FJsonObject> Error = AnimAuthoring_ReadCurveTarget(Params, AnimPath, CurveName))
    {
        return Error;
    }

    UAnimSequence* Seq = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(AnimPath, TEXT("AnimSequence"), Seq)) { return Error; }

    // Build keys. Accept an optional "keys":[{time, location:[x,y,z], rotation:[pitch,yaw,roll], scale:[x,y,z]}].
    // Default: two keys — identity at t=0 and a +100 X translation at t=1.
    TArray<FTransform> Transforms;
    TArray<float> Times;
    const TArray<TSharedPtr<FJsonValue>>* KeysArr = nullptr;
    if (Params->TryGetArrayField(TEXT("keys"), KeysArr) && KeysArr && KeysArr->Num() > 0)
    {
        for (const TSharedPtr<FJsonValue>& KV : *KeysArr)
        {
            const TSharedPtr<FJsonObject>* KObj = nullptr;
            if (!KV.IsValid() || !KV->TryGetObject(KObj) || !KObj || !KObj->IsValid()) { continue; }
            double T = 0.0; (*KObj)->TryGetNumberField(TEXT("time"), T);
            FVector Loc(0, 0, 0), Scale(1, 1, 1); FRotator Rot(0, 0, 0);
            const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
            if ((*KObj)->TryGetArrayField(TEXT("location"), Arr) && Arr && Arr->Num() >= 3)
            { Loc = FVector((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber()); }
            if ((*KObj)->TryGetArrayField(TEXT("rotation"), Arr) && Arr && Arr->Num() >= 3)
            { Rot = FRotator((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber()); }
            if ((*KObj)->TryGetArrayField(TEXT("scale"), Arr) && Arr && Arr->Num() >= 3)
            { Scale = FVector((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber()); }
            Times.Add(static_cast<float>(T));
            Transforms.Add(FTransform(Rot, Loc, Scale));
        }
    }
    if (Transforms.Num() == 0)
    {
        Times.Add(0.0f);   Transforms.Add(FTransform::Identity);
        Times.Add(1.0f);   Transforms.Add(FTransform(FRotator::ZeroRotator, FVector(100, 0, 0), FVector(1, 1, 1)));
    }

    const FName CurveFName(*CurveName);
    const FAnimationCurveIdentifier CurveId(CurveFName, ERawCurveTrackTypes::RCT_Transform);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("anim_sequence_path"), Seq->GetPathName());
    Obj->SetStringField(TEXT("curve_name"), CurveName);
    Obj->SetNumberField(TEXT("num_keys"), Times.Num());

    // A transform curve that already holds these keys changes nothing: answered before the data controller
    // transacts (AddCurve leaves an existing curve and its flags alone).
    if (const IAnimationDataModel* Model = Seq->GetDataModel())
    {
        if (AnimAuthoring_TransformCurveHasKeys(Model->FindTransformCurve(CurveId), Transforms, Times))
        {
            Obj->SetNumberField(TEXT("num_transform_curves"), Model->GetNumberOfTransformCurves());
            Obj->SetBoolField(TEXT("keys_set"), false);
            Obj->SetBoolField(TEXT("changed"), false);
            Obj->SetBoolField(TEXT("success"), true);
            return AnimAuthoring_NoChangeReply(Obj, TEXT("The transform curve already exists with those keys; nothing changed, nothing saved"));
        }
    }

    // No NotifyPopulated here: a Populated notify makes the sequence reset its sampling rate to the data model
    // rate and drop its per-platform overrides (UAnimSequence::OnModelModified). The curve notifies of the
    // bracket already regenerate what depends on the curve.
    const bool bCurveExisted = Seq->GetDataModel() && Seq->GetDataModel()->FindTransformCurve(CurveId) != nullptr;
    const FAnimAuthoring_SequenceWriteProbe Probe(Seq);
    IAnimationDataController& Controller = Seq->GetController();
    Controller.OpenBracket(LOCTEXT("AddTransformCurve", "MCP: Add Transform Curve"));
    Controller.AddCurve(CurveId, /*AACF_DefaultCurve*/0x00000004);
    const bool bSet = Controller.SetTransformCurveKeys(CurveId, Transforms, Times);
    if (!bSet && !bCurveExisted)
    {
        Controller.RemoveCurve(CurveId); // the curve this call added goes with the keys it could not set
    }
    Controller.CloseBracket();

    // The controller refuses keys only when their count differs from the times' or the curve is missing, which
    // the code above rules out; should it refuse anyway, nothing is saved and the sequence is put back.
    if (!bSet)
    {
        if (Probe.RestoreIfUnchanged())
        {
            mcp_common::DiscardUndoStepOfThisCall();
        }
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("add_anim_transform_curve: the animation data controller refused the %d keys of '%s'; nothing was saved%s"),
            Times.Num(), *CurveName, bCurveExisted ? TEXT("") : TEXT(" and the curve it had added was removed")));
    }

    int32 NumTransformCurves = 0;
    if (const IAnimationDataModel* Model = Seq->GetDataModel())
    {
        NumTransformCurves = Model->GetNumberOfTransformCurves();
    }

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Seq, SaveError);

    Obj->SetNumberField(TEXT("num_transform_curves"), NumTransformCurves);
    Obj->SetBoolField(TEXT("keys_set"), bSet);
    Obj->SetBoolField(TEXT("changed"), true);
    Obj->SetBoolField(TEXT("success"), true);
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("add_anim_transform_curve"), SaveError);
}

// ---------------------------------------------------------------------------
// set_anim_additive_settings — set the additive animation type + base pose on a UAnimSequence.
// ---------------------------------------------------------------------------
// MCP:set_anim_additive_settings - Set the additive-animation type and base pose on a UAnimSequence in an undo transaction and save it. Settings the sequence already has are a success with changed=false and saved=false, and the sequence is neither modified nor saved. A failed save makes success false, with saved=false and save_error.
//   @param anim_sequence_path - Object path of the UAnimSequence.
//   @param additive_anim_type - Additive type: LocalSpaceBase|RotationOffsetMeshSpace|None (default LocalSpaceBase)
//   @param ref_pose_type - Base pose type: RefPose|AnimScaled|AnimFrame|LocalAnimFrame|None (default RefPose)
//   @param ref_frame_index - Frame index used for the base pose (default 0)
//   @param ref_pose_anim_path - Object path of the reference UAnimSequence for the base pose (optional)
//   @returns { anim_sequence_path, additive_anim_type, ref_pose_type, ref_frame_index, ref_pose_anim_set, changed, success, saved, save_error, message? }
TSharedPtr<FJsonObject> HandleSetAnimAdditiveSettings(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("anim_sequence_path"), AnimPath)) { return Missing; }
    UAnimSequence* Seq = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(AnimPath, TEXT("AnimSequence"), Seq)) { return Error; }

    FString AdditiveTypeStr = TEXT("LocalSpaceBase");
    Params->TryGetStringField(TEXT("additive_anim_type"), AdditiveTypeStr);
    FString BasePoseStr = TEXT("RefPose");
    Params->TryGetStringField(TEXT("ref_pose_type"), BasePoseStr);
    double RefFrameD = 0.0; Params->TryGetNumberField(TEXT("ref_frame_index"), RefFrameD);

    const EAdditiveAnimationType AdditiveType = ParseAdditiveAnimType(AdditiveTypeStr);
    const EAdditiveBasePoseType BasePoseType = ParseBasePoseType(BasePoseStr);

    const int32 RefFrameIndex = static_cast<int32>(RefFrameD);
    UAnimSequence* RefSeq = nullptr;
    FString RefPosePath;
    if (Params->TryGetStringField(TEXT("ref_pose_anim_path"), RefPosePath) && !RefPosePath.IsEmpty())
    {
        RefSeq = AnimAssetUtils::LoadAsset<UAnimSequence>(RefPosePath);
    }

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("anim_sequence_path"), Seq->GetPathName());
    const auto AddSettings = [&]()
    {
        Obj->SetStringField(TEXT("additive_anim_type"), AdditiveAnimTypeToString(Seq->AdditiveAnimType.GetValue()));
        Obj->SetStringField(TEXT("ref_pose_type"), BasePoseTypeToString(Seq->RefPoseType.GetValue()));
        Obj->SetNumberField(TEXT("ref_frame_index"), Seq->RefFrameIndex);
    };

    // Settings the sequence already has change nothing: answered before Modify() would dirty it.
    if (Seq->AdditiveAnimType == AdditiveType && Seq->RefPoseType == BasePoseType && Seq->RefFrameIndex == RefFrameIndex
        && (!RefSeq || Seq->RefPoseSeq == RefSeq))
    {
        AddSettings();
        Obj->SetBoolField(TEXT("ref_pose_anim_set"), false);
        Obj->SetBoolField(TEXT("changed"), false);
        Obj->SetBoolField(TEXT("success"), true);
        return AnimAuthoring_NoChangeReply(Obj, TEXT("The sequence already has those additive settings; nothing changed, nothing saved"));
    }

    {
        const FScopedTransaction Transaction(LOCTEXT("SetAdditiveSettings", "MCP: Set Additive Settings"));
        Seq->Modify();
        Seq->AdditiveAnimType = AdditiveType;
        Seq->RefPoseType = BasePoseType;
        Seq->RefFrameIndex = RefFrameIndex;
        if (RefSeq)
        {
            Seq->RefPoseSeq = RefSeq;
        }
    }
    const bool bRefPoseSet = RefSeq != nullptr;

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Seq, SaveError);

    AddSettings();
    Obj->SetBoolField(TEXT("ref_pose_anim_set"), bRefPoseSet);
    Obj->SetBoolField(TEXT("changed"), true);
    Obj->SetBoolField(TEXT("success"), true);
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("set_anim_additive_settings"), SaveError);
}

// ---------------------------------------------------------------------------
// create_pose_asset_from_anim_sequence — build a UPoseAsset from a real anim.
// ---------------------------------------------------------------------------
// MCP:create_pose_asset_from_anim_sequence - Create a UPoseAsset built from a real UAnimSequence's frames and save it. Idempotent: a UPoseAsset already at that path is returned unchanged with already_existed=true and saved=false, and nothing is saved. A failed save makes success false, with saved=false and save_error.
//   @param pose_asset_name - Name for the new UPoseAsset.
//   @param anim_sequence_path - Object path of the source UAnimSequence.
//   @param package_path - Target content package path where the asset is created (default /Game/Animation)
//   @returns { pose_asset_path, pose_asset_name, source_anim, skeleton_path, num_poses, created, success, already_existed, saved, save_error }
TSharedPtr<FJsonObject> HandleCreatePoseAssetFromAnimSequence(const TSharedPtr<FJsonObject>& Params)
{
    FString Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("pose_asset_name"), Name)) { return Missing; }
    FString AnimPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("anim_sequence_path"), AnimPath)) { return Missing; }
    FString PackagePath = TEXT("/Game/Animation");
    Params->TryGetStringField(TEXT("package_path"), PackagePath);
    PackagePath = AnimAssetUtils::StripTrailingSlashes(PackagePath);

    UAnimSequence* AnimSeq = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(AnimPath, TEXT("AnimSequence"), AnimSeq)) { return Error; }
    USkeleton* Skeleton = AnimSeq->GetSkeleton();
    if (!Skeleton)
    {
        return mcp_common::MakeErrorReply(TEXT("Source AnimSequence has no skeleton"));
    }

    const FString FullPath = FString::Printf(TEXT("%s/%s"), *PackagePath, *Name);

    auto BuildPayload = [&](UPoseAsset* Pose, bool bCreated) -> TSharedPtr<FJsonObject>
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("pose_asset_path"), Pose->GetPathName());
        Obj->SetStringField(TEXT("pose_asset_name"), Name);
        Obj->SetStringField(TEXT("source_anim"), AnimSeq->GetPathName());
        const USkeleton* PoseSkeleton = Pose->GetSkeleton();
        Obj->SetStringField(TEXT("skeleton_path"), PoseSkeleton ? PoseSkeleton->GetPathName() : FString());
        Obj->SetNumberField(TEXT("num_poses"), Pose->GetNumPoses());
        Obj->SetBoolField(TEXT("created"), bCreated);
        Obj->SetBoolField(TEXT("success"), true);
        return Obj;
    };

    // Idempotent: load existing. Nothing changed, so nothing is saved.
    if (UPoseAsset* Existing = AnimAssetUtils::FindExisting<UPoseAsset>(FullPath))
    {
        TSharedPtr<FJsonObject> Obj = BuildPayload(Existing, /*bCreated=*/false);
        Obj->SetBoolField(TEXT("already_existed"), true);
        Obj->SetBoolField(TEXT("saved"), false);
        return mcp_common::MakeOkReply(Obj);
    }

    FString CreateError;
    UPoseAsset* Pose = mcp_common::NewAssetInPackage<UPoseAsset>(FullPath, Name,
        TEXT("Failed to create UPoseAsset"), CreateError);
    if (!Pose)
    {
        return mcp_common::MakeErrorReply(CreateError);
    }
    Pose->SetSkeleton(Skeleton);

    Pose->CreatePoseFromAnimation(AnimSeq);   // ENGINE_API; builds poses from the anim frames

    FAssetRegistryModule::AssetCreated(Pose);
    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Pose->GetPackage(), Pose, SaveError);

    TSharedPtr<FJsonObject> Obj = BuildPayload(Pose, /*bCreated=*/true);
    Obj->SetBoolField(TEXT("already_existed"), false);
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("create_pose_asset_from_anim_sequence"), SaveError);
}

// ===========================================================================
// More anim sequence commands, all headless:
//   remove_anim_bone_track       -- IAnimationDataController::RemoveBoneTrack
//   remove_anim_curve            -- IAnimationDataController::RemoveCurve (float or transform)
//   remove_anim_sync_marker      -- direct UAnimSequence::AuthoredSyncMarkers.RemoveAll
//   set_anim_frame_rate          -- IAnimationDataController::SetFrameRate
//   set_anim_number_of_frames    -- IAnimationDataController::SetNumberOfFrames
// ===========================================================================

// MCP:remove_anim_bone_track - Remove the animation track of one bone from a UAnimSequence through its data controller (IAnimationDataController::RemoveBoneTrack, one undo step) and save the package. No track with that bone name is a success with removed=false and nothing saved; a failed save makes success false, with saved=false and save_error. List tracks with get_anim_sequence_info (bone_tracks).
//   @param sequence_path - Asset path of the UAnimSequence.
//   @param bone_name - Name of the bone track to remove; a name no track has removes nothing.
//   @returns { sequence_path, bone_name, removed, saved, save_error }
TSharedPtr<FJsonObject> HandleRemoveAnimBoneTrack(const TSharedPtr<FJsonObject>& Params)
{
    FString SequencePath, BoneName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sequence_path"), SequencePath)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("bone_name"), BoneName)) { return Missing; }
    UAnimSequence* Sequence = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(SequencePath, TEXT("AnimSequence"), Sequence)) { return Error; }
    IAnimationDataController& Controller = Sequence->GetController();
    const bool bRemoved = Controller.RemoveBoneTrack(FName(*BoneName));
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("sequence_path"), Sequence->GetPathName());
    Data->SetStringField(TEXT("bone_name"), BoneName);
    Data->SetBoolField(TEXT("removed"), bRemoved);
    if (!bRemoved)
    {
        // No track had that name: nothing changed, so nothing is saved.
        Data->SetBoolField(TEXT("saved"), false);
        return mcp_common::MakeOkReply(Data);
    }
    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Sequence->GetPackage(), Sequence, SaveError);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("remove_anim_bone_track"), SaveError);
}

// MCP:remove_anim_curve - Remove a float or transform curve by name from a UAnimSequence through its data controller (one undo step) and save the package. No such curve is a success with removed=false and nothing saved; a failed save makes success false, with saved=false and save_error. Any curve_type other than transform targets float curves.
//   @param sequence_path - Asset path of the UAnimSequence.
//   @param curve_name - Name of the curve to remove.
//   @param curve_type - Curve type to target: float (default) or transform
//   @returns { sequence_path, curve_name, curve_type, removed, saved, save_error }
TSharedPtr<FJsonObject> HandleRemoveAnimCurve(const TSharedPtr<FJsonObject>& Params)
{
    FString SequencePath, CurveName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sequence_path"), SequencePath)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("curve_name"), CurveName)) { return Missing; }
    FString CurveType;
    Params->TryGetStringField(TEXT("curve_type"), CurveType);
    const ERawCurveTrackTypes TypeEnum = AnimAuthoring_CurveTrackType(CurveType);

    UAnimSequence* Sequence = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(SequencePath, TEXT("AnimSequence"), Sequence)) { return Error; }
    const FAnimationCurveIdentifier Id(FName(*CurveName), TypeEnum);
    IAnimationDataController& Controller = Sequence->GetController();
    const bool bRemoved = Controller.RemoveCurve(Id);
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("sequence_path"), Sequence->GetPathName());
    Data->SetStringField(TEXT("curve_name"), CurveName);
    Data->SetStringField(TEXT("curve_type"), CurveType.IsEmpty() ? TEXT("float") : CurveType);
    Data->SetBoolField(TEXT("removed"), bRemoved);
    if (!bRemoved)
    {
        // No curve of that name and type: nothing changed, so nothing is saved.
        Data->SetBoolField(TEXT("saved"), false);
        return mcp_common::MakeOkReply(Data);
    }
    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Sequence->GetPackage(), Sequence, SaveError);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("remove_anim_curve"), SaveError);
}

// MCP:remove_anim_sync_marker - Remove every authored sync marker with the given name from a UAnimSequence in an undo transaction and save the package. Returns markers_before, markers_after and removed; no marker with that name is a success with removed=false and nothing saved, and a failed save makes success false, with saved=false and save_error.
//   @param sequence_path - Asset path of the UAnimSequence.
//   @param marker_name - Name of the marker to remove.
//   @returns { sequence_path, marker_name, markers_before, markers_after, removed, saved, save_error }
TSharedPtr<FJsonObject> HandleRemoveAnimSyncMarker(const TSharedPtr<FJsonObject>& Params)
{
    FString SequencePath, MarkerName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sequence_path"), SequencePath)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("marker_name"), MarkerName)) { return Missing; }
    UAnimSequence* Sequence = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(SequencePath, TEXT("AnimSequence"), Sequence)) { return Error; }
    const FName MarkerFName(*MarkerName);
    const auto HasMarkerName = [&MarkerFName](const FAnimSyncMarker& M) { return M.MarkerName == MarkerFName; };
    const int32 BeforeCount = Sequence->AuthoredSyncMarkers.Num();
    const bool bFound = Sequence->AuthoredSyncMarkers.ContainsByPredicate(HasMarkerName);
    if (bFound)
    {
        // Modify before the write so the removal is one undo step, as add_anim_sync_marker's add is.
        const FScopedTransaction Transaction(LOCTEXT("RemoveAnimSyncMarker", "Remove Anim Sync Marker"));
        Sequence->Modify();
        Sequence->AuthoredSyncMarkers.RemoveAll(HasMarkerName);
    }
    const int32 AfterCount = Sequence->AuthoredSyncMarkers.Num();
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("sequence_path"), Sequence->GetPathName());
    Data->SetStringField(TEXT("marker_name"), MarkerName);
    Data->SetNumberField(TEXT("markers_before"), BeforeCount);
    Data->SetNumberField(TEXT("markers_after"), AfterCount);
    Data->SetBoolField(TEXT("removed"), AfterCount < BeforeCount);
    if (!bFound)
    {
        // No marker had that name: nothing changed, so nothing is saved.
        Data->SetBoolField(TEXT("saved"), false);
        return mcp_common::MakeOkReply(Data);
    }
    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Sequence->GetPackage(), Sequence, SaveError);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("remove_anim_sync_marker"), SaveError);
}

// MCP:set_anim_frame_rate - Set the frame rate of the data model of a UAnimSequence (IAnimationDataController::SetFrameRate, one undo step) to fps rounded to whole frames per second, then save the package; fps, numerator and denominator are read back from the data model. The data controller only takes a rate that is a multiple or a factor of the current one, and the frame count must stay whole at the new rate: any other rate is an error and nothing changes. The rate the data model already has is a success with changed=false, saved=false and a note, and nothing is written or saved. A failed save makes success false, with saved=false and save_error. get_anim_sequence_info reads the rate.
//   @param sequence_path - Asset path of the UAnimSequence.
//   @param fps - New frame rate in frames per second, rounded to a whole number that must be at least 1.
//   @returns { sequence_path, fps, numerator, denominator, changed, saved, save_error, note? }
TSharedPtr<FJsonObject> HandleSetAnimFrameRate(const TSharedPtr<FJsonObject>& Params)
{
    FString SequencePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sequence_path"), SequencePath)) { return Missing; }
    double Fps = 0.0;
    if (!Params->TryGetNumberField(TEXT("fps"), Fps) || Fps <= 0.0)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Missing or invalid 'fps' parameter (must be > 0)"));
    }
    // A rate that rounds to 0 fps passes the data controller's interval check and then reaches
    // FFrameRate::IsMultipleOf, which divides by its numerator: refuse it before the controller sees it.
    const double RoundedFps = FMath::RoundToDouble(Fps);
    if (RoundedFps < 1.0 || RoundedFps > static_cast<double>(TNumericLimits<int32>::Max()))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("fps %g rounds to %.0f; the frame rate must be a whole number of at least 1 fps"), Fps, RoundedFps));
    }
    UAnimSequence* Sequence = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(SequencePath, TEXT("AnimSequence"), Sequence)) { return Error; }
    const IAnimationDataModel* Model = Sequence->GetDataModel();
    if (!Model)
    {
        return mcp_common::MakeErrorReply(TEXT("AnimSequence has no data model"));
    }
    const FFrameRate NewRate(static_cast<int32>(RoundedFps), 1);
    const FFrameRate OldRate = Model->GetFrameRate();
    // Converting from a rate with a zero numerator fails a check() (and the controller's own
    // compatibility test divides by it), so such a model is refused untouched.
    if (!OldRate.IsValid() || OldRate.Numerator <= 0)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("The data model of this sequence has an invalid frame rate (%d/%d); nothing was changed"),
            OldRate.Numerator, OldRate.Denominator));
    }

    // The rate the data model already has changes nothing: answered before the data controller transacts.
    if (NewRate == OldRate)
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("sequence_path"), Sequence->GetPathName());
        Data->SetNumberField(TEXT("fps"), OldRate.AsDecimal());
        Data->SetNumberField(TEXT("numerator"), OldRate.Numerator);
        Data->SetNumberField(TEXT("denominator"), OldRate.Denominator);
        Data->SetBoolField(TEXT("changed"), false);
        return AnimAuthoring_NoChangeReply(Data, TEXT("The sequence already has that frame rate; nothing changed and nothing was saved."), TEXT("note"));
    }

    // The data controller truncates a frame count that is not whole at the new rate (after an ensure),
    // which silently shortens the sequence: refuse that rate instead.
    const int32 NumFrames = Model->GetNumberOfFrames();
    const FFrameTime ConvertedFrames = FFrameRate::TransformTime(FFrameTime(FFrameNumber(NumFrames)), OldRate, NewRate);
    if (!FMath::IsNearlyZero(ConvertedFrames.GetSubFrame()))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("The %d frames of this sequence at %s are not a whole number of frames at %s; nothing was changed"),
            NumFrames, *OldRate.ToPrettyText().ToString(), *NewRate.ToPrettyText().ToString()));
    }

    IAnimationDataController& Controller = Sequence->GetController();
    Controller.SetFrameRate(NewRate);
    // The source rate lives in the data model; GetSamplingFrameRate is the target (resampling) rate,
    // which SetFrameRate does not change.
    const FFrameRate ReadBack = Model->GetFrameRate();
    if (ReadBack != NewRate)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("The data controller rejected %s for this sequence (current rate %s; it takes only a multiple or a factor of it, see the editor log); nothing was saved"),
            *NewRate.ToPrettyText().ToString(), *ReadBack.ToPrettyText().ToString()));
    }
    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Sequence->GetPackage(), Sequence, SaveError);
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("sequence_path"), Sequence->GetPathName());
    Data->SetNumberField(TEXT("fps"), ReadBack.AsDecimal());
    Data->SetNumberField(TEXT("numerator"), ReadBack.Numerator);
    Data->SetNumberField(TEXT("denominator"), ReadBack.Denominator);
    Data->SetBoolField(TEXT("changed"), true);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_anim_frame_rate"), SaveError);
}

namespace
{
    // The smallest frame count at From that is a whole number of frames at To: frame_count * (To / From) is
    // whole exactly when frame_count is a multiple of the reduced denominator of that ratio. Both rates have a
    // positive numerator and denominator.
    long long AnimAuthoring_WholeFrameStep(const FFrameRate& From, const FFrameRate& To)
    {
        const int64 RatioNum = static_cast<int64>(To.Numerator) * From.Denominator;
        const int64 RatioDen = static_cast<int64>(To.Denominator) * From.Numerator;
        int64 Gcd = RatioNum;
        for (int64 Rest = RatioDen; Rest != 0;)
        {
            const int64 Next = Gcd % Rest;
            Gcd = Rest;
            Rest = Next;
        }
        return Gcd > 0 ? static_cast<long long>(RatioDen / Gcd) : 1LL;
    }
}

// MCP:set_anim_number_of_frames - Set the number of frames of the data model of a UAnimSequence (IAnimationDataController::SetNumberOfFrames, one undo step), then save the package; frame_count is read back from the data model. When the data model rate differs from the sequence sampling rate (as after set_anim_frame_rate), a count that is not a whole number of frames at the sampling rate is an error naming the multiple to use, with nothing changed. Each per-platform override of the sampling rate that the engine compresses at (a valid rate other than the default that is a multiple or a factor of it) is checked the same way, and the error names the platform; an override with a non-positive numerator is an error too. The current count gives saved=false and a note, with nothing saved. A count the data controller rejects is an error with nothing saved, and a failed save makes success false, with saved=false and save_error.
//   @param sequence_path - Asset path of the UAnimSequence.
//   @param frame_count - New number of frames (integer > 0).
//   @returns { sequence_path, frame_count, saved, save_error, note? }
TSharedPtr<FJsonObject> HandleSetAnimNumberOfFrames(const TSharedPtr<FJsonObject>& Params)
{
    FString SequencePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sequence_path"), SequencePath)) { return Missing; }
    int32 FrameCount = 0;
    if (!Params->TryGetNumberField(TEXT("frame_count"), FrameCount) || FrameCount <= 0)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Missing or invalid 'frame_count' parameter (must be > 0)"));
    }
    UAnimSequence* Sequence = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(SequencePath, TEXT("AnimSequence"), Sequence)) { return Error; }
    const IAnimationDataModel* Model = Sequence->GetDataModel();
    if (!Model)
    {
        return mcp_common::MakeErrorReply(TEXT("AnimSequence has no data model"));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("sequence_path"), Sequence->GetPathName());
    if (Model->GetNumberOfFrames() == FrameCount)
    {
        // The data controller leaves an equal length alone: nothing changed, so nothing is saved.
        Data->SetNumberField(TEXT("frame_count"), FrameCount);
        Data->SetBoolField(TEXT("saved"), false);
        Data->SetStringField(TEXT("note"), TEXT("The sequence already has that number of frames; nothing changed and nothing was saved."));
        return mcp_common::MakeOkReply(Data);
    }

    // After a length change the sequence recounts its frames at its sampling (target) rate, which
    // set_anim_frame_rate leaves alone, and ensures the count is whole there (UAnimSequence::
    // CalculateNumberOfSampledKeys, then truncates it). A count that is not whole at that rate is refused
    // before the data controller sees it. Converting from a rate with a zero numerator fails a check(),
    // and to one with a zero denominator divides by zero, so such rates are refused first.
    const FFrameRate ModelRate = Model->GetFrameRate();
    const FFrameRate SamplingRate = Sequence->GetSamplingFrameRate();
    if (!ModelRate.IsValid() || ModelRate.Numerator <= 0)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("The data model of this sequence has an invalid frame rate (%d/%d); nothing was changed"),
            ModelRate.Numerator, ModelRate.Denominator));
    }
    if (SamplingRate != ModelRate)
    {
        if (!SamplingRate.IsValid())
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("This sequence has an invalid sampling frame rate (%d/%d); nothing was changed"),
                SamplingRate.Numerator, SamplingRate.Denominator));
        }
        const FFrameTime Resampled = FFrameRate::TransformTime(FFrameTime(FFrameNumber(FrameCount)), ModelRate, SamplingRate);
        if (!FMath::IsNearlyZero(Resampled.GetSubFrame()))
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("%d frames at the data model rate %s are not a whole number of frames at the sampling rate %s of this sequence; nothing was changed. Use a frame count that is a multiple of %lld."),
                FrameCount, *ModelRate.ToPrettyText().ToString(), *SamplingRate.ToPrettyText().ToString(),
                AnimAuthoring_WholeFrameStep(ModelRate, SamplingRate)));
        }
    }

    // A per-platform override of the sampling rate is the rate the engine compresses at for that platform, in
    // the editor too when it names the running platform: UE::Anim::Compression::GetCompressionFrameRate takes a
    // valid override that differs from the default rate and is a multiple or a factor of it, and
    // FCompressibleAnimData::FetchData then check()s that the length is a whole number of frames at that rate.
    // Such an override is checked the way the default rate is. One with a non-positive numerator (or a default
    // rate with one) would make that choice divide by zero, so it is refused.
    if (const FStructProperty* TargetRateProperty = FindFProperty<FStructProperty>(UAnimSequence::StaticClass(), TEXT("PlatformTargetFrameRate")))
    {
        if (TargetRateProperty->Struct == FPerPlatformFrameRate::StaticStruct())
        {
            const FPerPlatformFrameRate* TargetRates = TargetRateProperty->ContainerPtrToValuePtr<FPerPlatformFrameRate>(Sequence);
            for (const TPair<FName, FFrameRate>& Override : TargetRates->PerPlatform)
            {
                const FFrameRate PlatformRate = Override.Value;
                if (!PlatformRate.IsValid() || PlatformRate == SamplingRate)
                {
                    continue;  // GetCompressionFrameRate samples at the default rate for this platform.
                }
                if (PlatformRate.Numerator <= 0 || SamplingRate.Numerator <= 0)
                {
                    return mcp_common::MakeErrorReply(FString::Printf(
                        TEXT("The sampling frame rate %d/%d this sequence has for platform %s cannot be resolved against its default sampling rate %d/%d (a non-positive numerator); nothing was changed"),
                        PlatformRate.Numerator, PlatformRate.Denominator, *Override.Key.ToString(),
                        SamplingRate.Numerator, SamplingRate.Denominator));
                }
                if (!PlatformRate.IsMultipleOf(SamplingRate) && !PlatformRate.IsFactorOf(SamplingRate))
                {
                    continue;  // Neither a multiple nor a factor: the engine samples at the default rate.
                }
                // The same arithmetic as FetchData: the play length in seconds at the data model rate, as frames
                // at the platform rate.
                const FFrameTime AtPlatformRate = PlatformRate.AsFrameTime(ModelRate.AsSeconds(FFrameTime(FFrameNumber(FrameCount))));
                if (!FMath::IsNearlyZero(AtPlatformRate.GetSubFrame()))
                {
                    return mcp_common::MakeErrorReply(FString::Printf(
                        TEXT("%d frames at the data model rate %s are not a whole number of frames at the sampling rate %s this sequence has for platform %s; nothing was changed. Use a frame count that is a multiple of %lld."),
                        FrameCount, *ModelRate.ToPrettyText().ToString(), *PlatformRate.ToPrettyText().ToString(),
                        *Override.Key.ToString(), AnimAuthoring_WholeFrameStep(ModelRate, PlatformRate)));
                }
            }
        }
    }

    IAnimationDataController& Controller = Sequence->GetController();
    Controller.SetNumberOfFrames(FFrameNumber(FrameCount));
    // Read the data model: GetNumberOfSampledKeys counts keys (frames + 1) at the target sampling rate.
    const int32 ReadBack = Model->GetNumberOfFrames();
    if (ReadBack != FrameCount)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("The data controller rejected %d frames for this sequence (it has %d; see the editor log); nothing was saved"),
            FrameCount, ReadBack));
    }
    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Sequence->GetPackage(), Sequence, SaveError);
    Data->SetNumberField(TEXT("frame_count"), ReadBack);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_anim_number_of_frames"), SaveError);
}

// ========================================================================== //
// Anim sequence curves advanced

#define LOCTEXT_NAMESPACE "UEAgentAnimationAuthoringCommands"
// Curve flags, rename, full readback and reimport on an AnimSequence;
// `add_anim_float_curve` (which also rekeys an existing float curve) /
// `add_anim_transform_curve` / `remove_anim_curve` cover add+remove at the
// curve level.
//   set_anim_curve_flags           IAnimationDataController::SetCurveFlags
//                                         (AACF_DefaultCurve / Editable / Metadata /
//                                         DriveTrack / Disabled).
//   rename_anim_curve                    IAnimationDataController::RenameCurve.
//   get_anim_curve_data                  read keys + flag bits by curve name.
//   reimport_anim_sequence               FReimportManager::Reimport (the engine
//                                         legacy reimport path; the sequence's
//                                         existing factory reads the source file).
//   (real sampling/timing changes go through `set_anim_frame_rate`, which
//   calls IAnimationDataController::SetFrameRate — see above.)
// ========================================================================== //

// MCP:set_anim_curve_flags - Replace the curve flags (the EAnimAssetCurveFlags bitmask) of an existing float or transform curve on a UAnimSequence with one flag, in an undo transaction, then save the package. Despite the name it changes no key interpolation or tangents: linear and editable both set Editable; metadata, drive_track and disabled set those flags. interp_mode is required; an unknown mode, curve or sequence is an error, and any curve_type other than transform means float. old_flags and new_flags are the bitmasks before and after; flags the curve already has are a success with applied=false, saved=false and a note, and nothing is written or saved; a failed save makes success false, with saved=false and save_error. get_anim_curve_data reads the flags.
//   @param anim_sequence_path - Full asset path to the UAnimSequence
//   @param curve_name - Name of the curve
//   @param interp_mode [string!] - One of "linear|editable|metadata|drive_track|disabled"
//   @param curve_type - Curve type to target: float (default) or transform
//   @returns { anim_sequence_path, curve_name, interp_mode, old_flags, new_flags, applied, saved, save_error, note? }
TSharedPtr<FJsonObject> HandleSetAnimCurveInterpMode(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimPath, CurveName;
    if (TSharedPtr<FJsonObject> Error = AnimAuthoring_ReadCurveTarget(Params, AnimPath, CurveName))
    {
        return Error;
    }
    const FString CurveTypeRaw = AnimAuthoring_ReadCurveType(Params);
    FString InterpModeRaw;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("interp_mode"), InterpModeRaw)) { return Missing; }
    const FString InterpMode = InterpModeRaw.ToLower();

// Map common interp_mode names onto the AACF_ (Animation Asset Curve Flag)
    // bitmask on the FFloatCurve / FTransformCurve flags. The enum has no
    // AACF_Constant / AACF_Discrete / AACF_Tangent*; its flags are
    // AACF_Editable (default), AACF_Metadata, AACF_DriveTrack,
    // AACF_Disabled. Tangent handling is per-key via FRichCurveKey, not
    // per-curve. We honour that by only accepting the flags that actually
    // exist — anything else is rejected with an honest error.
    int32 Flags = 0;
    if      (InterpMode == TEXT("linear"))       { Flags = AACF_DefaultCurve; }
    else if (InterpMode == TEXT("editable"))     { Flags = AACF_Editable; }
    else if (InterpMode == TEXT("metadata"))     { Flags = AACF_Metadata; }
    else if (InterpMode == TEXT("drive_track"))  { Flags = AACF_DriveTrack; }
    else if (InterpMode == TEXT("disabled"))     { Flags = AACF_Disabled; }
    else
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Unsupported interp_mode '%s' (use linear|editable|metadata|drive_track|disabled). "
                                 "AACF_Constant/AACF_Discrete/AACF_TangentAuto/AACF_TangentUser/AACF_TangentBreak were "
                                 "removed from EAnimAssetCurveFlags in UE 5.7; tangent handling is per-key via FRichCurveKey now."),
                            *InterpMode));
    }

    UAnimSequence* Seq = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(AnimPath, TEXT("AnimSequence"), Seq)) { return Error; }

    const FName CurveFName(*CurveName);
    const ERawCurveTrackTypes CurveType = AnimAuthoring_CurveTrackType(CurveTypeRaw);
    const FAnimationCurveIdentifier CurveId(CurveFName, CurveType);
    IAnimationDataModel* Model = Seq->GetDataModel();
    if (!Model)
    {
        return mcp_common::MakeErrorReply(TEXT("AnimSequence has no data model"));
    }

    // Look the curve up with the finder of its own type: the data model ensures when
    // FindFloatCurve gets a transform identifier (or FindTransformCurve a float one).
    const FAnimCurveBase* Curve = (CurveType == ERawCurveTrackTypes::RCT_Transform)
        ? static_cast<const FAnimCurveBase*>(Model->FindTransformCurve(CurveId))
        : static_cast<const FAnimCurveBase*>(Model->FindFloatCurve(CurveId));
    if (!Curve)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Curve '%s' (%s) does not exist on this AnimSequence"), *CurveName,
                CurveType == ERawCurveTrackTypes::RCT_Transform ? TEXT("transform") : TEXT("float")));
    }
    const int32 OldFlags = Curve->GetCurveTypeFlags();

    // Flags the curve already has change nothing: answered before the transaction.
    if (OldFlags == Flags)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("anim_sequence_path"), Seq->GetPathName());
        Result->SetStringField(TEXT("curve_name"), CurveName);
        Result->SetStringField(TEXT("interp_mode"), InterpMode);
        Result->SetNumberField(TEXT("old_flags"), OldFlags);
        Result->SetNumberField(TEXT("new_flags"), OldFlags);
        Result->SetBoolField(TEXT("applied"), false);
        return AnimAuthoring_NoChangeReply(Result, TEXT("The curve already has exactly those flags; nothing changed and nothing was saved."), TEXT("note"));
    }

    const FScopedTransaction Transaction(LOCTEXT("SetAnimCurveInterpMode", "Set Anim Curve Interp Mode"));
    Seq->GetController().OpenBracket(NSLOCTEXT("MCP", "SetCurveInterpMode", "Set Curve Interp Mode"));
    const bool bApplied = Seq->GetController().SetCurveFlags(CurveId, Flags, /*bShouldTransact=*/true);
    Seq->GetController().CloseBracket();
    if (!bApplied)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("The data controller rejected the flags for curve '%s' (see the editor log); nothing was saved"), *CurveName));
    }

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Seq, SaveError);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("anim_sequence_path"), Seq->GetPathName());
    Result->SetStringField(TEXT("curve_name"), CurveName);
    Result->SetStringField(TEXT("interp_mode"), InterpMode);
    Result->SetNumberField(TEXT("old_flags"), OldFlags);
    Result->SetNumberField(TEXT("new_flags"), Flags);
    Result->SetBoolField(TEXT("applied"), bApplied);
    return mcp_common::MakeSavedReply(Result, bSaved, TEXT("set_anim_curve_flags"), SaveError);
}

// MCP:rename_anim_curve - Rename a float or transform curve on a UAnimSequence through its data controller (one undo step) and save it; errors when the source curve does not exist or the rename is refused. The same name for both is a success with renamed=false, saved=false and a note, and nothing is written or saved. Any curve_type other than transform means float. Returns renamed and saved. A failed save makes success false, with saved=false and save_error.
//   @param anim_sequence_path - Full asset path to the UAnimSequence
//   @param old_curve_name - Name of the existing curve to rename, as listed by get_anim_sequence_info (float_curves, transform_curves).
//   @param new_curve_name - Name the curve gets after the rename.
//   @param curve_type - Curve type to target: float (default) or transform
//   @returns { anim_sequence_path, old_curve_name, new_curve_name, renamed, saved, save_error, note? }
TSharedPtr<FJsonObject> HandleRenameAnimCurve(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimPath, OldCurveName, NewCurveName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("anim_sequence_path"), AnimPath)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("old_curve_name"), OldCurveName)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("new_curve_name"), NewCurveName)) { return Missing; }
    const FString CurveTypeRaw = AnimAuthoring_ReadCurveType(Params);

    UAnimSequence* Seq = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(AnimPath, TEXT("AnimSequence"), Seq)) { return Error; }

    const ERawCurveTrackTypes CurveType = AnimAuthoring_CurveTrackType(CurveTypeRaw);
    const FAnimationCurveIdentifier OldId(FName(*OldCurveName), CurveType);
    const FAnimationCurveIdentifier NewId(FName(*NewCurveName), CurveType);
    IAnimationDataModel* Model = Seq->GetDataModel();
    if (!Model)
    {
        return mcp_common::MakeErrorReply(TEXT("AnimSequence has no data model"));
    }
    const FAnimCurveBase* Existing = Model->FindFloatCurve(OldId);
    if (!Existing && CurveType == ERawCurveTrackTypes::RCT_Transform)
    {
        Existing = Model->FindTransformCurve(OldId);
    }
    if (!Existing)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Source curve '%s' (%s) does not exist"), *OldCurveName, *CurveTypeRaw));
    }

    // Renaming a curve to the name it already has changes nothing (the data controller would refuse it).
    if (FName(*OldCurveName) == FName(*NewCurveName))
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("anim_sequence_path"), Seq->GetPathName());
        Result->SetStringField(TEXT("old_curve_name"), OldCurveName);
        Result->SetStringField(TEXT("new_curve_name"), NewCurveName);
        Result->SetBoolField(TEXT("renamed"), false);
        return AnimAuthoring_NoChangeReply(Result, TEXT("The curve already has that name; nothing changed and nothing was saved."), TEXT("note"));
    }

    const FScopedTransaction Transaction(LOCTEXT("RenameAnimCurve", "Rename Anim Curve"));
    Seq->GetController().OpenBracket(NSLOCTEXT("MCP", "RenameAnimCurve", "Rename Anim Curve"));
    const bool bRenamed = Seq->GetController().RenameCurve(OldId, NewId, true);
    Seq->GetController().CloseBracket();
    if (!bRenamed)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("RenameCurve('%s' -> '%s') returned false"), *OldCurveName, *NewCurveName));
    }

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Seq, SaveError);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("anim_sequence_path"), Seq->GetPathName());
    Result->SetStringField(TEXT("old_curve_name"), OldCurveName);
    Result->SetStringField(TEXT("new_curve_name"), NewCurveName);
    Result->SetBoolField(TEXT("renamed"), true);
    return mcp_common::MakeSavedReply(Result, bSaved, TEXT("rename_anim_curve"), SaveError);
}

// MCP:get_anim_curve_data - Read one curve of a UAnimSequence without changing it: for a float curve its flags, num_keys and keys as time/value pairs; for a transform curve only flags and a summed num_keys. found=false (still a success) when the curve does not exist.
//   @param anim_sequence_path - Full asset path to the UAnimSequence
//   @param curve_name - Name of the curve to read
//   @param curve_type - Curve type to target: float (default) or transform
//   @returns { anim_sequence_path, curve_name, curve_type, found, num_keys, flags, time, value, keys }
TSharedPtr<FJsonObject> HandleGetAnimCurveData(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimPath, CurveName;
    if (TSharedPtr<FJsonObject> Error = AnimAuthoring_ReadCurveTarget(Params, AnimPath, CurveName))
    {
        return Error;
    }
    const FString CurveTypeRaw = AnimAuthoring_ReadCurveType(Params);

    UAnimSequence* Seq = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(AnimPath, TEXT("AnimSequence"), Seq)) { return Error; }

    IAnimationDataModel* Model = Seq->GetDataModel();
    if (!Model)
    {
        return mcp_common::MakeErrorReply(TEXT("AnimSequence has no data model"));
    }
    const ERawCurveTrackTypes CurveType = AnimAuthoring_CurveTrackType(CurveTypeRaw);
    const FAnimationCurveIdentifier CurveId(FName(*CurveName), CurveType);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("anim_sequence_path"), Seq->GetPathName());
    Result->SetStringField(TEXT("curve_name"), CurveName);
    Result->SetStringField(TEXT("curve_type"), CurveTypeRaw);
    Result->SetBoolField(TEXT("found"), false);

if (CurveType == ERawCurveTrackTypes::RCT_Float)
        {
            if (const FFloatCurve* C = Model->FindFloatCurve(CurveId))
            {
                Result->SetBoolField(TEXT("found"), true);
                Result->SetNumberField(TEXT("num_keys"), C->FloatCurve.Keys.Num());
                Result->SetNumberField(TEXT("flags"), C->GetCurveTypeFlags());
                TArray<TSharedPtr<FJsonValue>> Keys;
                for (const FRichCurveKey& K : C->FloatCurve.Keys)
                {
                    TSharedPtr<FJsonObject> KObj = MakeShared<FJsonObject>();
                    KObj->SetNumberField(TEXT("time"), K.Time);
                    KObj->SetNumberField(TEXT("value"), K.Value);
                    Keys.Add(MakeShared<FJsonValueObject>(KObj));
                }
                Result->SetArrayField(TEXT("keys"), Keys);
            }
        }
else if (const FTransformCurve* C = Model->FindTransformCurve(CurveId))
    {
        Result->SetBoolField(TEXT("found"), true);
        // FTransformCurve has 3 FVectorCurves (Translation/Rotation/Scale), each
        // with 3 sub-FRichCurves (X/Y/Z). Sum the key counts.
        const int32 NumKeys = C->TranslationCurve.GetNumKeys()
                            + C->RotationCurve.GetNumKeys()
                            + C->ScaleCurve.GetNumKeys();
        Result->SetNumberField(TEXT("num_keys"), NumKeys);
        Result->SetNumberField(TEXT("flags"), C->GetCurveTypeFlags());
    }
    return mcp_common::MakeOkReply(Result);
}

// MCP:reimport_anim_sequence - Reimport a UAnimSequence, or any reimportable asset (the class is not checked), from source_file_path or its first recorded source via FReimportManager, automated (no dialogs) and unsaved. A missing source is refused before the reimport, which would crash the editor on it, and a reimport the handler reports as failed is a failure too: both make success false with reimported=false, and the missing source a note.
//   @param anim_sequence_path - Full asset path to the UAnimSequence
//   @param source_file_path - Absolute path of the file to reimport from on disk (e.g. "D:/Art/Walk.fbx"); defaults to the first source file recorded in the asset import data.
//   @returns { anim_sequence_path, source_file_path, reimported, source_validated, note? }
//
// FReimportManager::Reimport() crashes the editor
// when called against an asset whose registered factory exists (CanReimport
// returns true) but whose source file is missing or invalid — typical case is
// a runtime-created or copy-pasted asset whose FBX path does not resolve on
// disk.
//
// So the effective source path is checked BEFORE invoking Reimport. If the
// asset has no factory, no source filenames, or the resolved source file does
// not exist on disk, return a clean error payload with `reimported:false` +
// `source_validated:false` + a descriptive `note` instead of crashing the editor
// (honest headless behavior). The actual `FReimportManager::Reimport()`
// call is only reached when all three preconditions hold.
TSharedPtr<FJsonObject> HandleReimportAnimSequence(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("anim_sequence_path"), AnimPath)) { return Missing; }
    FString SourceFilePath;
    Params->TryGetStringField(TEXT("source_file_path"), SourceFilePath);

    UObject* Asset = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(AnimPath, TEXT("AnimSequence"), Asset)) { return Error; }

    // FReimportManager::Reimport runs the asset's registered reimport handler
    // (typically UAnimSequenceFactory) against its recorded source file (or the
    // caller-provided PreferredReimportFile). bAskForNewFileIfMissing=false +
    // bShowNotification=false + bAutomated=true so no dialogs appear under
    // -unattended. Same flow as the existing import cmd `reimport_asset` but
    // tightened to UAnimSequence paths.
    FReimportManager* ReimportManager = FReimportManager::Instance();
    if (!ReimportManager)
    {
        return mcp_common::MakeErrorReply(TEXT("FReimportManager unavailable"));
    }

    TArray<FString> SourceFilenames;
    const bool bCanReimport = ReimportManager->CanReimport(Asset, &SourceFilenames);
    if (!bCanReimport)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Asset %s has no reimport source (factory not registered)"), *AnimPath));
    }

    const FString EffectiveSource = SourceFilePath.IsEmpty()
        ? (SourceFilenames.Num() > 0 ? SourceFilenames[0] : TEXT(""))
        : SourceFilePath;

    // ----- Validate the source path on disk BEFORE
    // calling FReimportManager::Reimport. Reimport() crashes the editor when
    // the asset's recorded source is missing/invalid (CanReimport is true
    // because the factory is registered, but the file path is stale). This
    // pre-validation is the difference between "no reimport possible" (an
    // honest error) and "editor crash".
    // A reimport that cannot run, or that the handler reports as failed, is a failure: the payload says which
    // step stopped it.
    if (EffectiveSource.IsEmpty())
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("anim_sequence_path"), Asset->GetPathName());
        Result->SetStringField(TEXT("source_file_path"), FString());
        Result->SetBoolField(TEXT("reimported"), false);
        Result->SetBoolField(TEXT("source_validated"), false);
        const FString Note =
            TEXT("Asset has a registered reimport factory but no source filename is recorded -- "
                 "the sequence was likely created at runtime or its FBX was relocated. "
                 "Provide 'source_file_path' explicitly or assign one in the asset's AssetImportData.");
        Result->SetStringField(TEXT("note"), Note);
        return mcp_common::MakeOutcomeReply(Result, false, TEXT("reimport_anim_sequence: not reimported. ") + Note);
    }

    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    if (!PlatformFile.FileExists(*EffectiveSource))
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("anim_sequence_path"), Asset->GetPathName());
        Result->SetStringField(TEXT("source_file_path"), EffectiveSource);
        Result->SetBoolField(TEXT("reimported"), false);
        Result->SetBoolField(TEXT("source_validated"), false);
        const FString Note = FString::Printf(TEXT("Source file does not exist on disk: %s -- "
                                 "FReimportManager::Reimport would crash the editor on a missing source. "
                                 "Provide a valid 'source_file_path' or fix the asset's import path."),
                             *EffectiveSource);
        Result->SetStringField(TEXT("note"), Note);
        return mcp_common::MakeOutcomeReply(Result, false, TEXT("reimport_anim_sequence: not reimported. ") + Note);
    }

    const bool bReimported = ReimportManager->Reimport(
        Asset,
        /*bAskForNewFileIfMissing=*/false,
        /*bShowNotification=*/false,
        /*PreferredReimportFile=*/EffectiveSource,
        /*SpecifiedReimportHandler=*/nullptr,
        /*SourceFileIndex=*/INDEX_NONE,
        /*bForceNewFile=*/false,
        /*bAutomated=*/true);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("anim_sequence_path"), Asset->GetPathName());
    Result->SetStringField(TEXT("source_file_path"), EffectiveSource);
    Result->SetBoolField(TEXT("reimported"), bReimported);
    Result->SetBoolField(TEXT("source_validated"), true);
    return mcp_common::MakeOutcomeReply(Result, bReimported, bReimported ? FString() : FString::Printf(
        TEXT("reimport_anim_sequence: FReimportManager::Reimport failed for %s from %s (see the editor log for the reimport handler's reason)."),
        *Asset->GetPathName(), *EffectiveSource));
}

// ---------------------------------------------------------------------------
// link_montage_sections
// Sets FCompositeSection::NextSectionName so montage sections chain (a field
// add_montage_section does not write). Pass
// next_section_name = "" or "None" to clear the link.
// ---------------------------------------------------------------------------
// MCP:link_montage_sections - Chain one montage section to the next by setting its NextSectionName.  add_montage_section creates sections but never links them; this writes FCompositeSection::NextSectionName so playback flows section -> next. Pass next_section_name "" or "None" to clear the link. Done in an undo transaction, then saved. Idempotent: the link the section already has is a success with changed=false and saved=false, and the montage is neither modified nor saved. A failed save makes success false, with saved=false and save_error.
//   @param montage_path - Full asset path of the montage (e.g. '/Game/MyGame/AM_Example').
//   @param section_name - Name of the existing section whose link to set.
//   @param next_section_name - Name of the existing section to play next (or "" / "None" to clear).
//   @returns { montage_path, section_name, next_section_name, linked, changed, section_count, sections, saved, save_error, message }
TSharedPtr<FJsonObject> HandleLinkMontageSections(const TSharedPtr<FJsonObject>& Params)
{
    FString MontagePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("montage_path"), MontagePath)) { return Missing; }
    FString SectionName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("section_name"), SectionName)) { return Missing; }
    FString NextSectionName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("next_section_name"), NextSectionName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UAnimMontage* Montage = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(MontagePath, TEXT("AnimMontage"), Montage)) { return Error; }

    FCompositeSection* Target = AnimAuthoring_FindSection(Montage, FName(*SectionName));
    if (!Target)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Section '%s' not found in montage"), *SectionName));
    }

    // Resolve the next section: empty / "None" clears the link, otherwise it must
    // reference an existing section (so the chain can't dangle).
    FName NextFName = NAME_None;
    const bool bClearing = NextSectionName.IsEmpty() || NextSectionName.Equals(TEXT("None"), ESearchCase::IgnoreCase);
    if (!bClearing)
    {
        const FName CandidateNext(*NextSectionName);
        if (!AnimAuthoring_FindSection(Montage, CandidateNext))
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("next_section_name '%s' is not an existing section in this montage"), *NextSectionName));
        }
        NextFName = CandidateNext;
    }

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("montage_path"), Montage->GetPathName());
    Obj->SetStringField(TEXT("section_name"), SectionName);

    // The link the section already has changes nothing: answered before Modify() would dirty the montage.
    if (Target->NextSectionName == NextFName)
    {
        Obj->SetStringField(TEXT("next_section_name"), Target->NextSectionName.ToString());
        Obj->SetBoolField(TEXT("linked"), Target->NextSectionName != NAME_None);
        Obj->SetBoolField(TEXT("changed"), false);
        Obj->SetNumberField(TEXT("section_count"), Montage->CompositeSections.Num());
        Obj->SetArrayField(TEXT("sections"), CollectSectionNames(Montage));
        return AnimAuthoring_NoChangeReply(Obj, TEXT("The section already has that link; nothing changed, nothing saved"));
    }

    {
        const FScopedTransaction Transaction(LOCTEXT("LinkMontageSections", "Link Montage Sections"));
        Montage->Modify();
        Target->NextSectionName = NextFName;
    }

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Montage, SaveError);

    Obj->SetStringField(TEXT("next_section_name"), Target->NextSectionName.ToString());
    Obj->SetBoolField(TEXT("linked"), Target->NextSectionName != NAME_None);
    Obj->SetBoolField(TEXT("changed"), true);
    Obj->SetNumberField(TEXT("section_count"), Montage->CompositeSections.Num());
    Obj->SetArrayField(TEXT("sections"), CollectSectionNames(Montage));
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Section link set"), bSaved, SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("link_montage_sections"), SaveError);
}

// ---------------------------------------------------------------------------
// set_blend_space_properties
// Configures the X (index 0) and, for 2D blendspaces, Y (index 1) axes of an
// existing UBlendSpace — name / min / max / grid. create_blend_space only
// configures the X axis; this is the only way to set the Y axis (or change X
// after creation). Writes BlendParameters via reflection (protected fixed-size
// array, ArrayDim==3 — same path create_blend_space uses), reads back via
// GetBlendParameter().
// ---------------------------------------------------------------------------
// MCP:set_blend_space_properties - Configure an existing blend space's X and (2D only) Y axis: name, min, max, grid divisions. Over set_properties it addresses each axis field by name, clamps grid to >= 1, rebuilds the runtime sample data (ValidateSampleData/ResampleData) and saves. On a 1D blend space the y_axis_* params are ignored with a note. Only the supplied fields change, in one undo step; min below max is not checked. Values the axes already have are a success with changed=false, applied_count 0, saved=false and a note, and nothing is modified or saved. A failed save makes success false, with saved=false and save_error. New blend space: create_blend_space.
//   @retired set_properties - object_path=<blend space>, properties={"BlendParameters":[{"DisplayName":"Speed","Min":0,"Max":600,"GridNum":4},{}]}, clamp=true, save=true (the blend space is resampled)
//   @param blend_space_path - Full asset path of the UBlendSpace / UBlendSpace1D.
//   @param x_axis_name - Optional X axis display name.
//   @param x_axis_min [number] - Optional X axis minimum value.
//   @param x_axis_max [number] - Optional X axis maximum value.
//   @param x_axis_grid [integer] - Optional X axis grid divisions (>= 1).
//   @param y_axis_name - Optional Y axis display name (2D only).
//   @param y_axis_min [number] - Optional Y axis minimum value (2D only).
//   @param y_axis_max [number] - Optional Y axis maximum value (2D only).
//   @param y_axis_grid [integer] - Optional Y axis grid divisions (2D only, >= 1).
//   @returns { blend_space_path, is_1d, x_axis: {name, min, max, grid}, y_axis: {name, min, max, grid}, applied_count, notes, changed, saved, save_error, note? }
TSharedPtr<FJsonObject> HandleSetBlendSpaceProperties(const TSharedPtr<FJsonObject>& Params)
{
    FString BlendSpacePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blend_space_path"), BlendSpacePath)) { return Missing; }

    UBlendSpace* BlendSpace = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(BlendSpacePath, TEXT("BlendSpace"), BlendSpace)) { return Error; }

    FStructProperty* BlendParamsProp = FindFProperty<FStructProperty>(UBlendSpace::StaticClass(), TEXT("BlendParameters"));
    if (!BlendParamsProp)
    {
        return mcp_common::MakeErrorReply(TEXT("Could not access BlendParameters via reflection"));
    }

    const bool bIs1D = BlendSpace->IsA<UBlendSpace1D>();
    int32 AppliedCount = 0;
    bool bAnyDiffers = false;
    TArray<FString> Notes;

    // Counts the supplied fields of one axis and whether any differs from the axis, and writes them when bWrite.
    auto VisitAxis = [&](int32 Index, const TCHAR* Prefix, bool bWrite)
    {
        FBlendParameter* Axis = BlendParamsProp->ContainerPtrToValuePtr<FBlendParameter>(BlendSpace, Index);
        if (!Axis)
        {
            return;
        }
        FString S;
        if (Params->TryGetStringField(FString(Prefix) + TEXT("_name"), S))
        {
            ++AppliedCount;
            bAnyDiffers |= !Axis->DisplayName.Equals(S, ESearchCase::CaseSensitive);
            if (bWrite) { Axis->DisplayName = S; }
        }
        double D = 0.0;
        if (Params->TryGetNumberField(FString(Prefix) + TEXT("_min"), D))
        {
            ++AppliedCount;
            bAnyDiffers |= Axis->Min != static_cast<float>(D);
            if (bWrite) { Axis->Min = static_cast<float>(D); }
        }
        if (Params->TryGetNumberField(FString(Prefix) + TEXT("_max"), D))
        {
            ++AppliedCount;
            bAnyDiffers |= Axis->Max != static_cast<float>(D);
            if (bWrite) { Axis->Max = static_cast<float>(D); }
        }
        int32 G = 0;
        if (Params->TryGetNumberField(FString(Prefix) + TEXT("_grid"), G))
        {
            ++AppliedCount;
            bAnyDiffers |= Axis->GridNum != FMath::Max(1, G);
            if (bWrite) { Axis->GridNum = FMath::Max(1, G); }
        }
    };

    // Build the field names so the codegen extractor does not see them as literal
    // string inputs (their JSON types are declared on the @param hints instead).
    const FString YPrefix = TEXT("y_axis");
    const bool bYProvided = Params->HasField(YPrefix + TEXT("_name")) || Params->HasField(YPrefix + TEXT("_min"))
        || Params->HasField(YPrefix + TEXT("_max")) || Params->HasField(YPrefix + TEXT("_grid"));
    if (bIs1D && bYProvided)
    {
        Notes.Add(TEXT("y_axis_* ignored: this is a 1D blend space (only the X axis applies)"));
    }

    // First pass: count and compare only, so values the axes already have leave the asset untouched.
    VisitAxis(0, TEXT("x_axis"), /*bWrite=*/false);
    if (!bIs1D)
    {
        VisitAxis(1, TEXT("y_axis"), /*bWrite=*/false);
    }

    if (AppliedCount == 0)
    {
        return mcp_common::MakeErrorReply(
            TEXT("No axis properties provided. Supply at least one of: x_axis_name/min/max/grid (and y_axis_* for 2D)"));
    }

    bool bSaved = false;
    FString SaveError;
    if (bAnyDiffers)
    {
        {
            // Written straight into the protected BlendParameters: Modify() inside a transaction makes it undoable.
            const FScopedTransaction Transaction(LOCTEXT("SetBlendSpaceProperties", "Set Blend Space Properties"));
            BlendSpace->Modify();
            AppliedCount = 0;
            VisitAxis(0, TEXT("x_axis"), /*bWrite=*/true);
            if (!bIs1D)
            {
                VisitAxis(1, TEXT("y_axis"), /*bWrite=*/true);
            }
            // Without this the RUNTIME does not see the blend space.
            RebuildBlendSpaceRuntimeData(BlendSpace);
        }
        bSaved = AnimAuthoring_SavePackageOf(BlendSpace, SaveError);
    }

    auto AxisJson = [](const FBlendParameter& P)
    {
        TSharedPtr<FJsonObject> J = MakeShared<FJsonObject>();
        J->SetStringField(TEXT("name"), P.DisplayName);
        J->SetNumberField(TEXT("min"), P.Min);
        J->SetNumberField(TEXT("max"), P.Max);
        J->SetNumberField(TEXT("grid"), P.GridNum);
        return J;
    };

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("blend_space_path"), BlendSpace->GetPathName());
    Obj->SetBoolField(TEXT("is_1d"), bIs1D);
    Obj->SetObjectField(TEXT("x_axis"), AxisJson(BlendSpace->GetBlendParameter(0)));
    if (!bIs1D)
    {
        Obj->SetObjectField(TEXT("y_axis"), AxisJson(BlendSpace->GetBlendParameter(1)));
    }
    Obj->SetNumberField(TEXT("applied_count"), bAnyDiffers ? AppliedCount : 0);
    Obj->SetArrayField(TEXT("notes"), mcp_common::StringsToJson(Notes));
    Obj->SetBoolField(TEXT("changed"), bAnyDiffers);
    if (!bAnyDiffers)
    {
        return AnimAuthoring_NoChangeReply(Obj, TEXT("The blend space axes already have those values; nothing changed and nothing was saved."), TEXT("note"));
    }
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("set_blend_space_properties"), SaveError);
}

// ===========================================================================
// Notify TRACK management on a UAnimSequence
// ===========================================================================

namespace
{
    // Fill OutNames with the unique notify track names on a sequence
    // (GetAnimationNotifyTrackNames clears + repopulates OutNames itself).
    void GetNotifyTrackNamesList(const UAnimSequence* Seq, TArray<FName>& OutNames)
    {
        UAnimationBlueprintLibrary::GetAnimationNotifyTrackNames(Seq, OutNames);
    }

    // Build a JSON string array of the current notify track names on a sequence.
    TArray<TSharedPtr<FJsonValue>> NotifyTrackNamesJson(const UAnimSequence* Seq)
    {
        TArray<FName> Names;
        GetNotifyTrackNamesList(Seq, Names);
        TArray<TSharedPtr<FJsonValue>> Arr;
        for (const FName& N : Names)
        {
            Arr.Add(MakeShared<FJsonValueString>(N.ToString()));
        }
        return Arr;
    }

    // A reply payload naming the sequence and the track, with the sequence's current notify track names.
    TSharedPtr<FJsonObject> AnimAuthoring_NotifyTrackPayload(const UAnimSequence* Seq, const FString& TrackName)
    {
        const TArray<TSharedPtr<FJsonValue>> TrackNamesJson = NotifyTrackNamesJson(Seq);
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("animation_path"), Seq->GetPathName());
        Obj->SetStringField(TEXT("track_name"), TrackName);
        Obj->SetArrayField(TEXT("track_names"), TrackNamesJson);
        Obj->SetNumberField(TEXT("track_count"), TrackNamesJson.Num());
        return Obj;
    }
}

// ---------------------------------------------------------------------------
// add_notify_track
// ---------------------------------------------------------------------------
// MCP:add_notify_track - Add a named notify TRACK to a UAnimSequence (the track itself, not events on it) in an undo transaction and save it. A track of that name already there is a success with already_existed=true and nothing saved; a failed save makes success false, with saved=false and save_error.
//   @param animation_path - Full asset path to the UAnimSequence.
//   @param track_name - Notify track name to add.
//   @param color - Optional track color as an [r,g,b] or [r,g,b,a] float array (default white).
//   @returns { animation_path, track_name, track_names, track_count, already_existed, saved, save_error, message }
TSharedPtr<FJsonObject> HandleAddNotifyTrack(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimationPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("animation_path"), AnimationPath)) { return Missing; }

    FString TrackNameStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("track_name"), TrackNameStr)) { return Missing; }

    UAnimSequence* AnimSeq = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(AnimationPath, TEXT("AnimSequence"), AnimSeq)) { return Error; }

    const FName TrackFName(*TrackNameStr);

    // Optional color as an [r,g,b(,a)] float array; default white.
    FLinearColor TrackColor = FLinearColor::White;
    TArray<float> ColorArr;
    mcp_common::ReadJsonFloatArray(Params, TEXT("color"), ColorArr);
    if (ColorArr.Num() >= 3)
    {
        TrackColor = FLinearColor(ColorArr[0], ColorArr[1], ColorArr[2],
            ColorArr.Num() >= 4 ? ColorArr[3] : 1.0f);
    }

    // Idempotency probe: the lib swallows dup-add silently, so we cannot infer
    // the result after the call. Pre-check whether the track already exists.
    const bool bAlreadyExisted = UAnimationBlueprintLibrary::IsValidAnimNotifyTrackName(AnimSeq, TrackFName);

    bool bSaved = false;
    FString SaveError;
    if (!bAlreadyExisted)
    {
        const FScopedTransaction Transaction(LOCTEXT("AddNotifyTrack", "Add Notify Track"));
        AnimSeq->Modify();
        UAnimationBlueprintLibrary::AddAnimationNotifyTrack(AnimSeq, TrackFName, TrackColor);
        bSaved = AnimAuthoring_SavePackageOf(AnimSeq, SaveError);
    }

    TSharedPtr<FJsonObject> Obj = AnimAuthoring_NotifyTrackPayload(AnimSeq, TrackNameStr);
    Obj->SetBoolField(TEXT("already_existed"), bAlreadyExisted);
    Obj->SetStringField(TEXT("message"), bAlreadyExisted
        ? TEXT("Notify track already existed; no change")
        : AnimAuthoring_SaveMessage(TEXT("Notify track added"), bSaved, SaveError));
    if (bAlreadyExisted)
    {
        // The track the caller asked for is there: nothing changed, so nothing was saved.
        Obj->SetBoolField(TEXT("saved"), false);
        return mcp_common::MakeOkReply(Obj);
    }
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("add_notify_track"), SaveError);
}

// ---------------------------------------------------------------------------
// remove_notify_track
// ---------------------------------------------------------------------------
// MCP:remove_notify_track - Remove one named notify track from a UAnimSequence in one undo step and save it: the notify events and sync markers on that track go with it and the tracks after it shift down one index. Idempotent by name: a track that is not there is a success with already_absent=true, and nothing is changed or saved. Removing the sequence's last remaining track is refused with an error and nothing changes. List the track names with get_anim_notifies; add_notify_track is the inverse, and remove_anim_notify takes out a single event instead of a whole track. A failed save makes success false, with saved=false and save_error.
//   @param animation_path - Object path of the UAnimSequence, e.g. '/Game/Animation/Run_Fwd'.
//   @param track_name - Name of the notify track, as get_anim_notifies lists it in track_names (matched case-insensitively), e.g. '1' or 'Footsteps'.
//   @returns { animation_path, track_name, track_names, track_count, already_absent, saved, save_error, message }
TSharedPtr<FJsonObject> HandleRemoveNotifyTrack(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimationPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("animation_path"), AnimationPath)) { return Missing; }

    FString TrackNameStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("track_name"), TrackNameStr)) { return Missing; }

    UAnimSequence* AnimSeq = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(AnimationPath, TEXT("AnimSequence"), AnimSeq)) { return Error; }

    const FName TrackFName(*TrackNameStr);

    // Idempotency probe: RemoveAnimationNotifyTrack is a silent no-op when the
    // name isn't found, so pre-check presence.
    const bool bPresent = UAnimationBlueprintLibrary::IsValidAnimNotifyTrackName(AnimSeq, TrackFName);
    if (!bPresent)
    {
        TSharedPtr<FJsonObject> Obj = AnimAuthoring_NotifyTrackPayload(AnimSeq, TrackNameStr);
        Obj->SetBoolField(TEXT("already_absent"), true);
        Obj->SetBoolField(TEXT("saved"), false);
        Obj->SetStringField(TEXT("message"), TEXT("Notify track was already absent; no change"));
        return mcp_common::MakeOkReply(Obj);
    }

    // Empty-track guard: RemoveAnimationNotifyTrack does NOT floor at 1 and does
    // NOT auto-recreate a default track. Removing the only remaining track would
    // empty AnimNotifyTracks — refuse with an honest error instead.
    TArray<FName> CurrentNames;
    GetNotifyTrackNamesList(AnimSeq, CurrentNames);
    if (CurrentNames.Num() <= 1)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Cannot remove the last notify track ('%s'): an AnimSequence must keep at least one notify track"), *TrackNameStr));
    }

    const FScopedTransaction Transaction(LOCTEXT("RemoveNotifyTrack", "Remove Notify Track"));
    AnimSeq->Modify();
    // The lib deletes the notifies ON this track and reindexes trailing
    // FAnimNotifyEvent::TrackIndex itself — do NOT hand-roll the reindex.
    UAnimationBlueprintLibrary::RemoveAnimationNotifyTrack(AnimSeq, TrackFName);

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(AnimSeq, SaveError);

    TSharedPtr<FJsonObject> Obj = AnimAuthoring_NotifyTrackPayload(AnimSeq, TrackNameStr);
    Obj->SetBoolField(TEXT("already_absent"), false);
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Notify track removed"), bSaved, SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("remove_notify_track"), SaveError);
}

// ---------------------------------------------------------------------------
// Composite segment authoring
// ---------------------------------------------------------------------------
namespace
{
    // Read back the composite's segments as JSON. SortAnimSegments() auto-stacks
    // StartPos contiguously, so we report the ACTUAL post-sort StartPos, never the
    // requested value.
    TArray<TSharedPtr<FJsonValue>> CompositeSegmentsJson(const UAnimComposite* Composite)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        if (!Composite) { return Out; }
        for (const FAnimSegment& Seg : Composite->AnimationTrack.AnimSegments)
        {
            TSharedPtr<FJsonObject> SegObj = MakeShared<FJsonObject>();
            const UAnimSequenceBase* Anim = Seg.GetAnimReference();
            SegObj->SetStringField(TEXT("anim"), Anim ? Anim->GetPathName() : FString());
            SegObj->SetNumberField(TEXT("start_pos"), Seg.StartPos);
            SegObj->SetNumberField(TEXT("end_pos"), Seg.StartPos + Seg.GetLength());
            SegObj->SetNumberField(TEXT("play_rate"), Seg.AnimPlayRate);
            SegObj->SetNumberField(TEXT("loop_count"), Seg.LoopingCount);
            Out.Add(MakeShared<FJsonValueObject>(SegObj));
        }
        return Out;
    }
}

// MCP:add_anim_composite_segment - Append an FAnimSegment referencing a real UAnimSequence to a UAnimComposite's AnimationTrack, then recompute its length. A failed save makes success false, with saved=false and save_error.
//   @param composite_path - Object path of the UAnimComposite (e.g. "/Game/Animation/AC_Example").
//   @param animation_path - Object path of the UAnimSequence to reference.
//   @param start_pos - Segment start position; ignored after auto-stacking (default 0)
//   @param play_rate - Segment play rate (default 1.0)
//   @param loop_count - Segment loop count, minimum 1 (default 1)
//   @returns { composite_path, animation_path, num_segments, composite_length, segments, success, saved, save_error }
TSharedPtr<FJsonObject> HandleAddAnimCompositeSegment(const TSharedPtr<FJsonObject>& Params)
{
    FString CompositePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("composite_path"), CompositePath)) { return Missing; }
    FString AnimPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("animation_path"), AnimPath)) { return Missing; }

    double StartPos = 0.0; Params->TryGetNumberField(TEXT("start_pos"), StartPos);
    double PlayRate = 1.0; Params->TryGetNumberField(TEXT("play_rate"), PlayRate);
    double LoopCountD = 1.0; Params->TryGetNumberField(TEXT("loop_count"), LoopCountD);

    UAnimComposite* Composite = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(CompositePath, TEXT("AnimComposite"), Composite)) { return Error; }
    UAnimSequence* Seq = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(AnimPath, TEXT("AnimSequence"), Seq)) { return Error; }
    if (Composite->GetSkeleton() && Seq->GetSkeleton() && Composite->GetSkeleton() != Seq->GetSkeleton())
    {
        return mcp_common::MakeErrorReply(TEXT("Composite and AnimSequence use different skeletons"));
    }

    const FScopedTransaction Transaction(LOCTEXT("AddAnimCompositeSegment", "MCP: Add Anim Composite Segment"));
    Composite->Modify();

    FAnimSegment Seg;
    Seg.SetAnimReference(Seq, /*bInitialize=*/true);   // sets AnimStartTime=0, AnimEndTime=anim length
    Seg.StartPos = static_cast<float>(StartPos);
    Seg.AnimPlayRate = static_cast<float>(PlayRate);
    Seg.LoopingCount = FMath::Max(1, static_cast<int32>(LoopCountD));
    Composite->AnimationTrack.AnimSegments.Add(Seg);

    Composite->AnimationTrack.SortAnimSegments();   // re-stacks StartPos contiguously + validates times

    Composite->SetCompositeLength(Composite->AnimationTrack.GetLength());

    UE_LOG(LogTemp, Verbose, TEXT("[add_anim_composite_segment] %s += %s -> %d segs, length %.4f"),
        *Composite->GetPathName(), *Seq->GetPathName(),
        Composite->AnimationTrack.AnimSegments.Num(), Composite->AnimationTrack.GetLength());

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Composite, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("composite_path"), Composite->GetPathName());
    Obj->SetStringField(TEXT("animation_path"), Seq->GetPathName());
    Obj->SetNumberField(TEXT("num_segments"), Composite->AnimationTrack.AnimSegments.Num());
    Obj->SetNumberField(TEXT("composite_length"), Composite->AnimationTrack.GetLength());
    Obj->SetArrayField(TEXT("segments"), CompositeSegmentsJson(Composite));
    Obj->SetBoolField(TEXT("success"), true);
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("add_anim_composite_segment"), SaveError);
}

// MCP:remove_anim_composite_segment - Remove one segment (an FAnimSegment) by zero-based index from a UAnimComposite's track in one undo step, re-stack the remaining segments end to end, recompute the composite length and save the asset. An index outside [0, num_segments) is an error and nothing changes. The later segments shift down one index, so a repeat with the same index removes a different segment: re-read segments from the reply before the next call. Count the segments with get_anim_asset_info; add_anim_composite_segment is the inverse. A failed save makes success false, with saved=false and save_error.
//   @param composite_path - Object path of the UAnimComposite, e.g. '/Game/Animation/AC_Example' (create_anim_composite makes one).
//   @param index - Zero-based index of the segment to remove, in [0, num_segments), in the order of the segments list that add_anim_composite_segment and this tool return.
//   @returns { composite_path, index, num_segments, composite_length, segments, success, saved, save_error }
TSharedPtr<FJsonObject> HandleRemoveAnimCompositeSegment(const TSharedPtr<FJsonObject>& Params)
{
    FString CompositePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("composite_path"), CompositePath)) { return Missing; }
    double IndexD = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("index"), IndexD)) { return Missing; }
    const int32 Index = static_cast<int32>(IndexD);

    UAnimComposite* Composite = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(CompositePath, TEXT("AnimComposite"), Composite)) { return Error; }

    const int32 NumBefore = Composite->AnimationTrack.AnimSegments.Num();
    if (Index < 0 || Index >= NumBefore)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Segment index %d out of range [0, %d)"), Index, NumBefore));
    }

    const FScopedTransaction Transaction(LOCTEXT("RemoveAnimCompositeSegment", "MCP: Remove Anim Composite Segment"));
    Composite->Modify();

    Composite->AnimationTrack.AnimSegments.RemoveAt(Index);

    Composite->AnimationTrack.SortAnimSegments();   // re-stacks StartPos contiguously + validates times

    Composite->SetCompositeLength(Composite->AnimationTrack.GetLength());

    UE_LOG(LogTemp, Verbose, TEXT("[remove_anim_composite_segment] %s -[%d] -> %d segs, length %.4f"),
        *Composite->GetPathName(), Index,
        Composite->AnimationTrack.AnimSegments.Num(), Composite->AnimationTrack.GetLength());

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Composite, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("composite_path"), Composite->GetPathName());
    Obj->SetNumberField(TEXT("index"), Index);
    Obj->SetNumberField(TEXT("num_segments"), Composite->AnimationTrack.AnimSegments.Num());
    Obj->SetNumberField(TEXT("composite_length"), Composite->AnimationTrack.GetLength());
    Obj->SetArrayField(TEXT("segments"), CompositeSegmentsJson(Composite));
    Obj->SetBoolField(TEXT("success"), true);
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("remove_anim_composite_segment"), SaveError);
}

// ===========================================================================
// Edit an existing notify + set montage blend/sync properties
// ===========================================================================
//
// set_anim_notify edits an existing notify event; get_anim_notifies is its reader
// (name/time/duration/track_index). The event lives in UAnimSequenceBase::Notifies
// (TArray<FAnimNotifyEvent>, AnimSequenceBase.h — always present, NOT editor-only),
// the SAME array add_anim_notify/get_anim_notifies use; we mutate the element in place
// under Modify(). Time/duration go through the ENGINE_API setters
// FAnimNotifyEvent::SetTime(float, EAnimLinkMethod::Type=Absolute) (AnimTypes.h) and
// SetDuration(float) (AnimTypes.h) — they recompute the link end/offset, so we never
// poke LinkValue/TriggerTimeOffset raw. NotifyTriggerChance (float, AnimTypes.h),
// TriggerWeightThreshold (float), MontageTickType (TEnumAsByte<EMontageNotifyTickType::Type>)
// and TrackIndex (int32) are public direct members written under Modify().
// After mutating times we SortNotifies() so the array stays time-sorted (its contract).
//
// Because NotifyName is not unique, the target is identified by (notify_name,
// occurrence_index): the Nth (0-based) event whose NotifyName matches. Re-running with
// the same inputs writes the same values -> idempotent. A missing (name, occurrence)
// target is an HONEST error (never a fake success), echoing how many matched.

namespace
{
    // Map a friendly montage-tick-type string to the enum byte. Returns INDEX_NONE
    // for an unrecognized value so the caller can surface an honest error.
    int32 ParseMontageTickType(const FString& In)
    {
        const FString V = In.TrimStartAndEnd();
        if (V.Equals(TEXT("Queued"), ESearchCase::IgnoreCase))         { return static_cast<int32>(EMontageNotifyTickType::Queued); }
        if (V.Equals(TEXT("BranchingPoint"), ESearchCase::IgnoreCase)) { return static_cast<int32>(EMontageNotifyTickType::BranchingPoint); }
        return INDEX_NONE;
    }

    const TCHAR* MontageTickTypeName(EMontageNotifyTickType::Type T)
    {
        return (T == EMontageNotifyTickType::BranchingPoint) ? TEXT("BranchingPoint") : TEXT("Queued");
    }

    // Serialize one FAnimNotifyEvent into the SAME shape get_anim_notifies uses, plus
    // the editable fields this command can mutate (so the caller can round-trip them).
    TSharedPtr<FJsonObject> AnimNotifyEventJson(const FAnimNotifyEvent& Event)
    {
        TSharedPtr<FJsonObject> J = MakeShared<FJsonObject>();
        J->SetStringField(TEXT("name"), Event.NotifyName.ToString());
        J->SetNumberField(TEXT("time"), Event.GetTriggerTime());
        J->SetNumberField(TEXT("duration"), Event.GetDuration());
        J->SetNumberField(TEXT("track_index"), Event.TrackIndex);
        J->SetNumberField(TEXT("trigger_chance"), Event.NotifyTriggerChance);
        J->SetNumberField(TEXT("trigger_weight_threshold"), Event.TriggerWeightThreshold);
        J->SetStringField(TEXT("montage_tick_type"), MontageTickTypeName(Event.MontageTickType.GetValue()));
        return J;
    }
}

// MCP:set_anim_notify - Edit an existing FAnimNotifyEvent on a UAnimSequence (identified by name + occurrence) in an undo transaction and save it. No editable field is an error before anything changes; edits that leave the event as it is are a success with applied=false, applied_count 0 and saved=false, and the sequence is neither modified nor saved. A failed save makes success false, with saved=false and save_error.
//   @renamed-from edit_anim_notify
//   @param animation_path - Full asset path to the UAnimSequence.
//   @param notify_name - NotifyName of the event to edit (FName). Names are not unique; see occurrence_index.
//   @param occurrence_index - Zero-based index AMONG events sharing notify_name (default 0)
//   @param time - New trigger time in seconds (FAnimNotifyEvent::SetTime). Optional.
//   @param duration - New duration in seconds (FAnimNotifyEvent::SetDuration; meaningful for notify-state). Optional.
//   @param trigger_chance - New NotifyTriggerChance in [0,1]. Optional.
//   @param trigger_weight_threshold - New TriggerWeightThreshold. Optional.
//   @param montage_tick_type - New MontageTickType: 'Queued' or 'BranchingPoint'. Optional.
//   @param track_index - New TrackIndex (zero-based). Optional.
//   @returns { animation_path, notify_name, occurrence_index, matched_count, notify, applied_count, applied, saved, save_error, message }
TSharedPtr<FJsonObject> HandleEditAnimNotify(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimationPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("animation_path"), AnimationPath)) { return Missing; }

    FString NotifyName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("notify_name"), NotifyName)) { return Missing; }

    UAnimSequence* AnimSeq = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(AnimationPath, TEXT("AnimSequence"), AnimSeq)) { return Error; }

    double OccurrenceD = 0.0;
    Params->TryGetNumberField(TEXT("occurrence_index"), OccurrenceD);
    const int32 OccurrenceIndex = FMath::Max(0, static_cast<int32>(OccurrenceD));

    // Locate the Nth event whose NotifyName matches. Match manually on NotifyName
    // (operator== keys off the Notify/NotifyStateClass pointer first) so named notifies
    // are matched unambiguously.
    const FName TargetName(*NotifyName);
    int32 MatchedCount = 0;
    int32 TargetArrayIndex = INDEX_NONE;
    for (int32 i = 0; i < AnimSeq->Notifies.Num(); ++i)
    {
        if (AnimSeq->Notifies[i].NotifyName == TargetName)
        {
            if (MatchedCount == OccurrenceIndex)
            {
                TargetArrayIndex = i;
            }
            ++MatchedCount;
        }
    }

    if (TargetArrayIndex == INDEX_NONE)
    {
        // Honest error: no such (name, occurrence). Report how many matched the name so
        // the caller can recover (e.g. re-list with get_anim_notifies).
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("No notify '%s' at occurrence_index %d (matched %d event(s) with that name) in %s"),
            *NotifyName, OccurrenceIndex, MatchedCount, *AnimSeq->GetPathName()));
    }

    // Validate the montage_tick_type string (if provided) BEFORE mutating anything.
    int32 TickTypeValue = INDEX_NONE;
    FString TickTypeStr;
    const bool bHasTickType = Params->TryGetStringField(TEXT("montage_tick_type"), TickTypeStr) && !TickTypeStr.IsEmpty();
    if (bHasTickType)
    {
        TickTypeValue = ParseMontageTickType(TickTypeStr);
        if (TickTypeValue == INDEX_NONE)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Invalid montage_tick_type '%s' (expected 'Queued' or 'BranchingPoint')"), *TickTypeStr));
        }
    }

    double DurationD = 0.0;
    const bool bHasDuration = Params->TryGetNumberField(TEXT("duration"), DurationD);
    double TimeD = 0.0;
    const bool bHasTime = Params->TryGetNumberField(TEXT("time"), TimeD);
    double ChanceD = 0.0;
    const bool bHasChance = Params->TryGetNumberField(TEXT("trigger_chance"), ChanceD);
    double WeightD = 0.0;
    const bool bHasWeight = Params->TryGetNumberField(TEXT("trigger_weight_threshold"), WeightD);
    double TrackIndexD = 0.0;
    const bool bHasTrackIndex = Params->TryGetNumberField(TEXT("track_index"), TrackIndexD);

    const int32 AppliedCount = (bHasDuration ? 1 : 0) + (bHasTime ? 1 : 0) + (bHasChance ? 1 : 0)
        + (bHasWeight ? 1 : 0) + (bHasTickType ? 1 : 0) + (bHasTrackIndex ? 1 : 0);
    const bool bTimingChanged = bHasDuration || bHasTime;
    if (AppliedCount == 0)
    {
        return mcp_common::MakeErrorReply(
            TEXT("No editable properties provided. Supply at least one of: time, duration, trigger_chance, "
                 "trigger_weight_threshold, montage_tick_type, track_index"));
    }

    // Writes the supplied fields onto an event.
    const auto ApplyEdits = [&](FAnimNotifyEvent& Event)
    {
        // Duration first, then time: SetTime recomputes EndLink from the CURRENT duration,
        // so applying duration before time keeps the end anchored to the final values.
        if (bHasDuration) { Event.SetDuration(static_cast<float>(DurationD)); }
        if (bHasTime) { Event.SetTime(static_cast<float>(TimeD), EAnimLinkMethod::Absolute); }
        // ClampMin/Max meta only fires through the editor/property path; clamp here.
        if (bHasChance) { Event.NotifyTriggerChance = FMath::Clamp(static_cast<float>(ChanceD), 0.0f, 1.0f); }
        if (bHasWeight) { Event.TriggerWeightThreshold = static_cast<float>(WeightD); }
        if (bHasTickType) { Event.MontageTickType = static_cast<EMontageNotifyTickType::Type>(TickTypeValue); }
        if (bHasTrackIndex) { Event.TrackIndex = FMath::Max(0, static_cast<int32>(TrackIndexD)); }
    };

    // Edits that leave every property of the event as it is change nothing: measured on a copy, before
    // Modify() would dirty the sequence.
    {
        FAnimNotifyEvent Edited = AnimSeq->Notifies[TargetArrayIndex];
        ApplyEdits(Edited);
        if (FAnimNotifyEvent::StaticStruct()->CompareScriptStruct(&Edited, &AnimSeq->Notifies[TargetArrayIndex], PPF_None))
        {
            TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
            Obj->SetStringField(TEXT("animation_path"), AnimSeq->GetPathName());
            Obj->SetStringField(TEXT("notify_name"), NotifyName);
            Obj->SetNumberField(TEXT("occurrence_index"), OccurrenceIndex);
            Obj->SetNumberField(TEXT("matched_count"), MatchedCount);
            Obj->SetNumberField(TEXT("applied_count"), 0);
            Obj->SetObjectField(TEXT("notify"), AnimNotifyEventJson(AnimSeq->Notifies[TargetArrayIndex]));
            Obj->SetBoolField(TEXT("applied"), false);
            return AnimAuthoring_NoChangeReply(Obj, TEXT("The notify already has those values; nothing changed, nothing saved"));
        }
    }

    {
        const FScopedTransaction Transaction(LOCTEXT("EditAnimNotify", "Edit Anim Notify"));
        AnimSeq->Modify();
        ApplyEdits(AnimSeq->Notifies[TargetArrayIndex]);
        // Keep Notifies time-sorted after a timing edit (the array contract).
        if (bTimingChanged)
        {
            AnimSeq->SortNotifies();
        }
    }

    UE_LOG(LogTemp, Verbose, TEXT("[set_anim_notify] %s notify '%s'[occ %d] -> %d field(s) applied (timing_changed=%d)"),
        *AnimSeq->GetPathName(), *NotifyName, OccurrenceIndex, AppliedCount, bTimingChanged ? 1 : 0);

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(AnimSeq, SaveError);

    // Re-find the (possibly re-sorted) event so the read-back reflects the real array.
    const FAnimNotifyEvent* Final = nullptr;
    {
        int32 Seen = 0;
        for (const FAnimNotifyEvent& E : AnimSeq->Notifies)
        {
            if (E.NotifyName == TargetName)
            {
                if (Seen == OccurrenceIndex) { Final = &E; break; }
                ++Seen;
            }
        }
    }

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("animation_path"), AnimSeq->GetPathName());
    Obj->SetStringField(TEXT("notify_name"), NotifyName);
    Obj->SetNumberField(TEXT("occurrence_index"), OccurrenceIndex);
    Obj->SetNumberField(TEXT("matched_count"), MatchedCount);
    Obj->SetNumberField(TEXT("applied_count"), AppliedCount);
    if (Final)
    {
        Obj->SetObjectField(TEXT("notify"), AnimNotifyEventJson(*Final));
    }
    Obj->SetBoolField(TEXT("applied"), true);
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Anim notify edited"), bSaved, SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("set_anim_notify"), SaveError);
}

// MCP:set_montage_properties - Set only the supplied blend-in/blend-out times, global rate scale, blend modes, auto blend out, trigger time, sync group/slot and deprecated root motion fields of a UAnimMontage in one undo step and save it. Over set_properties it sets the blend times through FAlphaBlend::SetBlendTime (clamped to >= 0), validates enum names before any write and saves in the same call; other montage properties: set_properties. Bad enum names, or no field, are refused before any change; values the montage already has give changed=false, applied_count 0 and saved=false, nothing modified or saved. A failed save makes success false, with saved=false and save_error.
//   @retired set_properties - object_path=<montage>, properties={"BlendIn":{"BlendTime":..},"BlendOut":{"BlendTime":..},...}, clamp=true, save=true
//   @param montage_path - Full asset path of the montage (e.g. '/Game/MyGame/AM_Example').
//   @param name - Alias of montage_path.
//   @param blend_in - Blend-in time in seconds (BlendIn), clamped to >= 0. Optional.
//   @param blend_out - Blend-out time in seconds (BlendOut), clamped to >= 0. Optional.
//   @param rate_scale - Global play rate scale of the montage (RateScale). Optional.
//   @param blend_mode_in - BlendModeIn: 'Standard' or 'Inertialization' (EMontageBlendMode). Optional.
//   @param blend_mode_out - BlendModeOut: 'Standard' or 'Inertialization' (EMontageBlendMode). Optional.
//   @param enable_auto_blend_out - When true the montage blends out by itself near its end (bEnableAutoBlendOut); omitted keeps the current value.
//   @param blend_out_trigger_time - BlendOutTriggerTime in seconds (<0 = auto from BlendOut time). Optional.
//   @param sync_group - Sync group name (SyncGroup) the montage joins for marker-based sync; an empty string sets None, omitted keeps the current value.
//   @param sync_slot_index - Zero-based index of the slot track whose animation drives the sync group (SyncSlotIndex); omitted keeps the current value.
//   @param enable_root_motion_translation - bEnableRootMotionTranslation (bool, DEPRECATED since 4.5). Optional.
//   @param enable_root_motion_rotation - bEnableRootMotionRotation (bool, DEPRECATED since 4.5). Optional.
//   @param root_motion_root_lock - RootMotionRootLock: 'RefPose'|'AnimFirstFrame'|'Zero' (DEPRECATED since 4.5). Optional.
//   @returns { montage_path, blend_in, blend_out, rate_scale, blend_mode_in, blend_mode_out, enable_auto_blend_out, blend_out_trigger_time, sync_group, sync_slot_index, root_motion: {...}, applied_count, notes, changed, saved, save_error, message }
TSharedPtr<FJsonObject> HandleSetMontageProperties(const TSharedPtr<FJsonObject>& Params)
{
    // Accept montage_path, or its alias 'name', for the asset path.
    FString MontagePath;
    if (!Params->TryGetStringField(TEXT("montage_path"), MontagePath) && !Params->TryGetStringField(TEXT("name"), MontagePath))
    {
        return mcp_common::MissingParameterReply(TEXT("montage_path"));
    }
    if (MontagePath.IsEmpty())
    {
        return mcp_common::MissingParameterReply(TEXT("montage_path"));
    }

    UAnimMontage* Montage = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(MontagePath, TEXT("AnimMontage"), Montage)) { return Error; }

    // Map a blend-mode string to EMontageBlendMode. INDEX_NONE on an unknown value.
    auto ParseBlendMode = [](const FString& In) -> int32
    {
        const FString V = In.TrimStartAndEnd();
        if (V.Equals(TEXT("Standard"), ESearchCase::IgnoreCase))        { return static_cast<int32>(EMontageBlendMode::Standard); }
        if (V.Equals(TEXT("Inertialization"), ESearchCase::IgnoreCase)) { return static_cast<int32>(EMontageBlendMode::Inertialization); }
        return INDEX_NONE;
    };

    // ERootMotionRootLock::Type values (AnimEnums.h): RefPose=0, AnimFirstFrame=1, Zero=2.
    // Written via reflection (TEnumAsByte -> FByteProperty) so we don't pull in the enum
    // header; validated by name here, surfaced with an honest deprecation note.
    auto ParseRootLock = [](const FString& In) -> int32
    {
        const FString V = In.TrimStartAndEnd();
        if (V.Equals(TEXT("RefPose"), ESearchCase::IgnoreCase))        { return 0; }
        if (V.Equals(TEXT("AnimFirstFrame"), ESearchCase::IgnoreCase)) { return 1; }
        if (V.Equals(TEXT("Zero"), ESearchCase::IgnoreCase))           { return 2; }
        return INDEX_NONE;
    };
    auto RootLockName = [](int32 V) -> const TCHAR*
    {
        switch (V) { case 1: return TEXT("AnimFirstFrame"); case 2: return TEXT("Zero"); default: return TEXT("RefPose"); }
    };

    // -------- Validate ALL string-typed enum inputs BEFORE mutating anything. -------- //
    int32 BlendInValue = INDEX_NONE;
    FString BlendInStr;
    const bool bHasBlendIn = Params->TryGetStringField(TEXT("blend_mode_in"), BlendInStr) && !BlendInStr.IsEmpty();
    if (bHasBlendIn)
    {
        BlendInValue = ParseBlendMode(BlendInStr);
        if (BlendInValue == INDEX_NONE)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Invalid blend_mode_in '%s' (expected 'Standard' or 'Inertialization')"), *BlendInStr));
        }
    }

    int32 BlendOutValue = INDEX_NONE;
    FString BlendOutStr;
    const bool bHasBlendOut = Params->TryGetStringField(TEXT("blend_mode_out"), BlendOutStr) && !BlendOutStr.IsEmpty();
    if (bHasBlendOut)
    {
        BlendOutValue = ParseBlendMode(BlendOutStr);
        if (BlendOutValue == INDEX_NONE)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Invalid blend_mode_out '%s' (expected 'Standard' or 'Inertialization')"), *BlendOutStr));
        }
    }

    int32 RootLockValue = INDEX_NONE;
    FString RootLockStr;
    const bool bHasRootLock = Params->TryGetStringField(TEXT("root_motion_root_lock"), RootLockStr) && !RootLockStr.IsEmpty();
    if (bHasRootLock)
    {
        RootLockValue = ParseRootLock(RootLockStr);
        if (RootLockValue == INDEX_NONE)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Invalid root_motion_root_lock '%s' (expected 'RefPose', 'AnimFirstFrame' or 'Zero')"), *RootLockStr));
        }
    }

    // RootMotionRootLock is a TEnumAsByte written through reflection: looked up before anything is touched.
    FByteProperty* LockProp = FindFProperty<FByteProperty>(UAnimMontage::StaticClass(), TEXT("RootMotionRootLock"));
    if (bHasRootLock && !LockProp)
    {
        return mcp_common::MakeErrorReply(TEXT("Could not access RootMotionRootLock via reflection"));
    }

    // Reads every supplied field once: counts it, records whether it differs from the montage, and writes it
    // when bWrite. The first pass only compares, so supplied values the montage already has leave it untouched.
    int32 AppliedCount = 0;
    bool bAnyDiffers = false;
    TArray<FString> Notes;
    const auto VisitFields = [&](bool bWrite)
    {
        AppliedCount = 0;
        bAnyDiffers = false;
        Notes.Reset();

        // Blend times: FAlphaBlend::SetBlendTime clamps to >= 0, so the clamped value is what is compared.
        double BlendInTimeD = 0.0;
        if (Params->TryGetNumberField(TEXT("blend_in"), BlendInTimeD))
        {
            const float NewIn = FMath::Max(static_cast<float>(BlendInTimeD), 0.0f);
            ++AppliedCount;
            bAnyDiffers |= Montage->BlendIn.GetBlendTime() != NewIn;
            if (bWrite) { Montage->BlendIn.SetBlendTime(NewIn); }
        }
        double BlendOutTimeD = 0.0;
        if (Params->TryGetNumberField(TEXT("blend_out"), BlendOutTimeD))
        {
            const float NewOut = FMath::Max(static_cast<float>(BlendOutTimeD), 0.0f);
            ++AppliedCount;
            bAnyDiffers |= Montage->BlendOut.GetBlendTime() != NewOut;
            if (bWrite) { Montage->BlendOut.SetBlendTime(NewOut); }
        }

        // RateScale lives on UAnimSequenceBase (public float).
        double RateScaleD = 1.0;
        if (Params->TryGetNumberField(TEXT("rate_scale"), RateScaleD))
        {
            ++AppliedCount;
            bAnyDiffers |= Montage->RateScale != static_cast<float>(RateScaleD);
            if (bWrite) { Montage->RateScale = static_cast<float>(RateScaleD); }
        }

        // BlendModeIn/Out are public 'enum class EMontageBlendMode : uint8' members -> assign
        // directly (type-checked; BlueprintReadOnly restricts BP write only, not C++).
        if (bHasBlendIn)
        {
            const EMontageBlendMode Mode = static_cast<EMontageBlendMode>(BlendInValue);
            ++AppliedCount;
            bAnyDiffers |= Montage->BlendModeIn != Mode;
            if (bWrite) { Montage->BlendModeIn = Mode; }
        }
        if (bHasBlendOut)
        {
            const EMontageBlendMode Mode = static_cast<EMontageBlendMode>(BlendOutValue);
            ++AppliedCount;
            bAnyDiffers |= Montage->BlendModeOut != Mode;
            if (bWrite) { Montage->BlendModeOut = Mode; }
        }

        bool bAutoBlendOut = false;
        if (Params->TryGetBoolField(TEXT("enable_auto_blend_out"), bAutoBlendOut))
        {
            ++AppliedCount;
            bAnyDiffers |= Montage->bEnableAutoBlendOut != bAutoBlendOut;
            if (bWrite) { Montage->bEnableAutoBlendOut = bAutoBlendOut; }
        }

        double TriggerTimeD = 0.0;
        if (Params->TryGetNumberField(TEXT("blend_out_trigger_time"), TriggerTimeD))
        {
            ++AppliedCount;
            bAnyDiffers |= Montage->BlendOutTriggerTime != static_cast<float>(TriggerTimeD);
            if (bWrite) { Montage->BlendOutTriggerTime = static_cast<float>(TriggerTimeD); }
        }

        FString SyncGroupStr;
        if (Params->TryGetStringField(TEXT("sync_group"), SyncGroupStr))
        {
            ++AppliedCount;
            bAnyDiffers |= Montage->SyncGroup != FName(*SyncGroupStr);
            if (bWrite) { Montage->SyncGroup = FName(*SyncGroupStr); }
        }

        double SyncSlotD = 0.0;
        if (Params->TryGetNumberField(TEXT("sync_slot_index"), SyncSlotD))
        {
            ++AppliedCount;
            bAnyDiffers |= Montage->SyncSlotIndex != static_cast<int32>(SyncSlotD);
            if (bWrite) { Montage->SyncSlotIndex = static_cast<int32>(SyncSlotD); }
        }

        // -------- Deprecated-since-4.5 root-motion members (honest NOTE on use). -------- //
        // Comment-only deprecation (no UE_DEPRECATED macro) so direct assignment is warning-
        // free; still public. RootMotionRootLock is a TEnumAsByte -> set via reflection.
        bool bRootTrans = false;
        if (Params->TryGetBoolField(TEXT("enable_root_motion_translation"), bRootTrans))
        {
            ++AppliedCount;
            bAnyDiffers |= Montage->bEnableRootMotionTranslation != bRootTrans;
            if (bWrite) { Montage->bEnableRootMotionTranslation = bRootTrans; }
            Notes.Add(TEXT("enable_root_motion_translation is DEPRECATED since 4.5 (root motion is controlled by anim sequences)"));
        }
        bool bRootRot = false;
        if (Params->TryGetBoolField(TEXT("enable_root_motion_rotation"), bRootRot))
        {
            ++AppliedCount;
            bAnyDiffers |= Montage->bEnableRootMotionRotation != bRootRot;
            if (bWrite) { Montage->bEnableRootMotionRotation = bRootRot; }
            Notes.Add(TEXT("enable_root_motion_rotation is DEPRECATED since 4.5 (root motion is controlled by anim sequences)"));
        }
        if (bHasRootLock)
        {
            void* Addr = LockProp->ContainerPtrToValuePtr<void>(Montage);
            ++AppliedCount;
            bAnyDiffers |= LockProp->GetUnsignedIntPropertyValue(Addr) != static_cast<uint64>(RootLockValue);
            if (bWrite) { LockProp->SetIntPropertyValue(Addr, static_cast<int64>(RootLockValue)); }
            Notes.Add(TEXT("root_motion_root_lock is DEPRECATED since 4.5 (root motion is controlled by anim sequences)"));
        }
    };

    VisitFields(/*bWrite=*/false);
    if (AppliedCount == 0)
    {
        return mcp_common::MakeErrorReply(
            TEXT("No properties provided. Supply at least one of: blend_in, blend_out, rate_scale, blend_mode_in, blend_mode_out, enable_auto_blend_out, "
                 "blend_out_trigger_time, sync_group, sync_slot_index, enable_root_motion_translation, "
                 "enable_root_motion_rotation, root_motion_root_lock"));
    }

    const bool bChanged = bAnyDiffers;
    bool bSaved = false;
    FString SaveError;
    if (bChanged)
    {
        {
            const FScopedTransaction Transaction(LOCTEXT("SetMontageProperties", "Set Montage Properties"));
            Montage->Modify();
            VisitFields(/*bWrite=*/true);
        }

        UE_LOG(LogTemp, Verbose, TEXT("[set_montage_properties] %s -> %d field(s) applied (mode_in=%s mode_out=%s)"),
            *Montage->GetPathName(), AppliedCount,
            AnimAuthoring_BlendModeName(Montage->BlendModeIn), AnimAuthoring_BlendModeName(Montage->BlendModeOut));

        bSaved = AnimAuthoring_SavePackageOf(Montage, SaveError);
    }

    // Read the deprecated lock back through reflection so the round-trip is honest.
    int32 ReadbackRootLock = 0;
    if (LockProp)
    {
        const void* Addr = LockProp->ContainerPtrToValuePtr<void>(Montage);
        ReadbackRootLock = static_cast<int32>(LockProp->GetUnsignedIntPropertyValue(Addr));
    }

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("montage_path"), Montage->GetPathName());
    Obj->SetNumberField(TEXT("blend_in"), Montage->BlendIn.GetBlendTime());
    Obj->SetNumberField(TEXT("blend_out"), Montage->BlendOut.GetBlendTime());
    Obj->SetNumberField(TEXT("rate_scale"), Montage->RateScale);
    Obj->SetStringField(TEXT("blend_mode_in"), AnimAuthoring_BlendModeName(Montage->BlendModeIn));
    Obj->SetStringField(TEXT("blend_mode_out"), AnimAuthoring_BlendModeName(Montage->BlendModeOut));
    Obj->SetBoolField(TEXT("enable_auto_blend_out"), Montage->bEnableAutoBlendOut);
    Obj->SetNumberField(TEXT("blend_out_trigger_time"), Montage->BlendOutTriggerTime);
    Obj->SetStringField(TEXT("sync_group"), Montage->SyncGroup.ToString());
    Obj->SetNumberField(TEXT("sync_slot_index"), Montage->SyncSlotIndex);

    TSharedPtr<FJsonObject> RootMotion = MakeShared<FJsonObject>();
    RootMotion->SetBoolField(TEXT("enable_translation"), Montage->bEnableRootMotionTranslation);
    RootMotion->SetBoolField(TEXT("enable_rotation"), Montage->bEnableRootMotionRotation);
    RootMotion->SetStringField(TEXT("root_lock"), RootLockName(ReadbackRootLock));
    RootMotion->SetBoolField(TEXT("deprecated"), true);
    Obj->SetObjectField(TEXT("root_motion"), RootMotion);

    Obj->SetNumberField(TEXT("applied_count"), bChanged ? AppliedCount : 0);
    Obj->SetArrayField(TEXT("notes"), mcp_common::StringsToJson(Notes));
    Obj->SetBoolField(TEXT("changed"), bChanged);
    if (!bChanged)
    {
        return AnimAuthoring_NoChangeReply(Obj, TEXT("The montage already has those properties; nothing changed, nothing saved"));
    }
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Montage properties set"), bSaved, SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("set_montage_properties"), SaveError);
}

// ===========================================================================
// Blend space input-smoothing (interpolation) + anim retarget source
// (Helpers ParseFilterInterpType / FilterInterpTypeToString / InterpAxisJson /
//  ReadBlendSpaceInterpolation live in the blendspace-samples anonymous namespace above, since
//  get_blend_space_samples also round-trips the interpolation through them.)

// ---------------------------------------------------------------------------
// set_blend_space_interpolation
// ---------------------------------------------------------------------------
// set_blend_space_properties only writes the BlendParameters axis (name/min/max/
// grid); the INPUT-SMOOTHING (interpolation) of a blend space lives in a SIBLING
// fixed-size array member, UBlendSpace::InterpolationParam[3] (BlendSpace.h,
// EditAnywhere, FInterpolationParameter per axis). We reach it by reflection
// EXACTLY like set_blend_space_properties reaches BlendParameters:
//   FindFProperty<FStructProperty>(UBlendSpace::StaticClass(), "InterpolationParam")
//   -> ContainerPtrToValuePtr<FInterpolationParameter>(BS, AxisIndex).
// 1D blend spaces only smooth axis 0; 2D smooth axes 0 and 1. ClampMin="0" metas
// on InterpolationTime/DampingRatio/MaxSpeed fire only through the editor property
// path, so we clamp to >=0 here.
// ---------------------------------------------------------------------------
// MCP:set_blend_space_interpolation - Set a blend space's INPUT-SMOOTHING (interpolation) per axis: smoothing time, damping ratio, max speed, smoothing type.  Distinct from set_properties (which only sets the BlendParameters axis name/min/max/grid). Writes UBlendSpace::InterpolationParam[axis] via reflection. 1D smooths axis 0 only; 2D smooths X (axis 0) and Y (axis 1). Only the supplied fields change, in one undo transaction, then the blend space is saved; an unknown smoothing type is an error before anything changes, and values the axes already have are a success with changed=false, applied_count 0, saved=false and a note, with nothing modified or saved. A failed save makes success false, with saved=false and save_error.
//   @param blend_space_path - Full asset path of the UBlendSpace / UBlendSpace1D.
//   @param x_interpolation_time [number] - Optional X-axis smoothing time in seconds (>= 0; 0 disables smoothing).
//   @param x_damping_ratio [number] - Optional X-axis damping ratio (>= 0; only used when type is SpringDamper).
//   @param x_max_speed [number] - Optional X-axis maximum speed in real units (>= 0; only used when type is SpringDamper or Exponential).
//   @param x_interpolation_type - Optional X-axis smoothing type: Averaged|Linear|Cubic|EaseInOut|Exponential|SpringDamper.
//   @param y_interpolation_time [number] - Optional Y-axis smoothing time in seconds (2D only, >= 0).
//   @param y_damping_ratio [number] - Optional Y-axis damping ratio (2D only, >= 0).
//   @param y_max_speed [number] - Optional Y-axis maximum speed (2D only, >= 0).
//   @param y_interpolation_type - Optional Y-axis smoothing type (2D only): Averaged|Linear|Cubic|EaseInOut|Exponential|SpringDamper.
//   @returns { blend_space_path, is_1d, x_axis: {interpolation_time, damping_ratio, max_speed, interpolation_type}, y_axis: {...}, applied_count, notes, changed, saved, save_error, note? }
TSharedPtr<FJsonObject> HandleSetBlendSpaceInterpolation(const TSharedPtr<FJsonObject>& Params)
{
    FString BlendSpacePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blend_space_path"), BlendSpacePath)) { return Missing; }

    UBlendSpace* BlendSpace = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(BlendSpacePath, TEXT("BlendSpace"), BlendSpace)) { return Error; }

    // InterpolationParam is a SIBLING of BlendParameters on UBlendSpace — same
    // reflection access pattern (fixed-size array, ArrayDim==3).
    FStructProperty* InterpProp = FindFProperty<FStructProperty>(UBlendSpace::StaticClass(), TEXT("InterpolationParam"));
    if (!InterpProp)
    {
        return mcp_common::MakeErrorReply(TEXT("Could not access InterpolationParam via reflection"));
    }

    const bool bIs1D = BlendSpace->IsA<UBlendSpace1D>();
    int32 AppliedCount = 0;
    bool bAnyDiffers = false;
    TArray<FString> Notes;
    bool bTypeError = false;
    FString TypeErrorMsg;

    // Counts the supplied fields of one axis and whether any differs from the axis, and writes them when bWrite.
    auto VisitAxis = [&](int32 Index, const TCHAR* Prefix, bool bWrite)
    {
        FInterpolationParameter* Axis = InterpProp->ContainerPtrToValuePtr<FInterpolationParameter>(BlendSpace, Index);
        if (!Axis)
        {
            return;
        }
        double D = 0.0;
        // ClampMin="0" metas only fire through the editor property path -> clamp here.
        if (Params->TryGetNumberField(FString(Prefix) + TEXT("_interpolation_time"), D))
        {
            const float V = FMath::Max(0.0f, static_cast<float>(D));
            ++AppliedCount;
            bAnyDiffers |= Axis->InterpolationTime != V;
            if (bWrite) { Axis->InterpolationTime = V; }
        }
        if (Params->TryGetNumberField(FString(Prefix) + TEXT("_damping_ratio"), D))
        {
            const float V = FMath::Max(0.0f, static_cast<float>(D));
            ++AppliedCount;
            bAnyDiffers |= Axis->DampingRatio != V;
            if (bWrite) { Axis->DampingRatio = V; }
        }
        if (Params->TryGetNumberField(FString(Prefix) + TEXT("_max_speed"), D))
        {
            const float V = FMath::Max(0.0f, static_cast<float>(D));
            ++AppliedCount;
            bAnyDiffers |= Axis->MaxSpeed != V;
            if (bWrite) { Axis->MaxSpeed = V; }
        }
        FString TypeStr;
        if (Params->TryGetStringField(FString(Prefix) + TEXT("_interpolation_type"), TypeStr) && !TypeStr.IsEmpty())
        {
            bool bValid = false;
            const EFilterInterpolationType Parsed = ParseFilterInterpType(TypeStr, bValid);
            if (!bValid)
            {
                bTypeError = true;
                TypeErrorMsg = FString::Printf(
                    TEXT("Invalid %s_interpolation_type '%s' (expected Averaged|Linear|Cubic|EaseInOut|Exponential|SpringDamper)"),
                    Prefix, *TypeStr);
                return;
            }
            ++AppliedCount;
            bAnyDiffers |= !(Axis->InterpolationType == Parsed);
            if (bWrite) { Axis->InterpolationType = Parsed; }
        }
    };

    const FString YPrefix = TEXT("y");
    const bool bYProvided = Params->HasField(YPrefix + TEXT("_interpolation_time")) || Params->HasField(YPrefix + TEXT("_damping_ratio"))
        || Params->HasField(YPrefix + TEXT("_max_speed")) || Params->HasField(YPrefix + TEXT("_interpolation_type"));
    if (bIs1D && bYProvided)
    {
        Notes.Add(TEXT("y_* ignored: this is a 1D blend space (only the X axis is smoothed)"));
    }

    // First pass: validate, count and compare only, so a bad type or values the axes already have leave the
    // asset untouched.
    VisitAxis(0, TEXT("x"), /*bWrite=*/false);
    if (!bTypeError && !bIs1D)
    {
        VisitAxis(1, TEXT("y"), /*bWrite=*/false);
    }
    if (bTypeError)
    {
        return mcp_common::MakeErrorReply(TypeErrorMsg);
    }

    if (AppliedCount == 0)
    {
        return mcp_common::MakeErrorReply(
            TEXT("No interpolation properties provided. Supply at least one of: x_interpolation_time/x_damping_ratio/"
                 "x_max_speed/x_interpolation_type (and y_* for 2D)"));
    }

    bool bSaved = false;
    FString SaveError;
    if (bAnyDiffers)
    {
        {
            // Written straight into InterpolationParam: Modify() inside a transaction makes it undoable.
            const FScopedTransaction Transaction(LOCTEXT("SetBlendSpaceInterpolation", "Set Blend Space Interpolation"));
            BlendSpace->Modify();
            AppliedCount = 0;
            VisitAxis(0, TEXT("x"), /*bWrite=*/true);
            if (!bIs1D)
            {
                VisitAxis(1, TEXT("y"), /*bWrite=*/true);
            }
            // Without this the RUNTIME does not see the blend space.
            RebuildBlendSpaceRuntimeData(BlendSpace);
        }
        bSaved = AnimAuthoring_SavePackageOf(BlendSpace, SaveError);
    }

    // Read the values back through the SAME reflection path so the round-trip is honest.
    auto AxisReadback = [&](int32 Index) -> TSharedPtr<FJsonObject>
    {
        const FInterpolationParameter* Axis = InterpProp->ContainerPtrToValuePtr<FInterpolationParameter>(BlendSpace, Index);
        return Axis ? InterpAxisJson(*Axis) : MakeShared<FJsonObject>();
    };

    UE_LOG(LogTemp, Verbose, TEXT("[set_blend_space_interpolation] %s -> %d field(s) applied (is_1d=%d)"),
        *BlendSpace->GetPathName(), AppliedCount, bIs1D ? 1 : 0);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("blend_space_path"), BlendSpace->GetPathName());
    Obj->SetBoolField(TEXT("is_1d"), bIs1D);
    Obj->SetObjectField(TEXT("x_axis"), AxisReadback(0));
    if (!bIs1D)
    {
        Obj->SetObjectField(TEXT("y_axis"), AxisReadback(1));
    }
    Obj->SetNumberField(TEXT("applied_count"), bAnyDiffers ? AppliedCount : 0);
    Obj->SetArrayField(TEXT("notes"), mcp_common::StringsToJson(Notes));
    Obj->SetBoolField(TEXT("changed"), bAnyDiffers);
    if (!bAnyDiffers)
    {
        return AnimAuthoring_NoChangeReply(Obj, TEXT("The blend space smoothing already has those values; nothing changed and nothing was saved."), TEXT("note"));
    }
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("set_blend_space_interpolation"), SaveError);
}

// ---------------------------------------------------------------------------
// set_anim_retarget_source
// ---------------------------------------------------------------------------
// set_anim_additive_settings sets the additive TYPE + base pose; the RETARGET
// SOURCE is a separate, unset property. UAnimSequence::RetargetSource (FName,
// AnimSequence.h, EditAnywhere) names an entry in the OWNING skeleton's
// retarget-source map; the property write persists regardless, but the entry must
// exist on the skeleton to RESOLVE at runtime (honest note). RefPoseSeq
// (TObjectPtr<UAnimSequence>, AnimSequence.h) is the additive base-pose
// sequence used when the retarget source is AnimScaled/AnimFrame — we load it by
// path and assign. Direct member assignment under Modify() (both are public).
// Distinct from set_anim_additive_settings (additive TYPE vs retarget SOURCE).
// ---------------------------------------------------------------------------
// MCP:set_anim_retarget_source - Set a UAnimSequence's retarget source and/or its retarget reference-pose sequence (RefPoseSeq) in one undo step and save it; pass at least one of the two, and an empty or 'None' value clears that field. Read the current values first with get_anim_sequence_info. A ref_pose_seq_path that does not load is an error before anything changes; a retarget_source the skeleton does not define is still written, with a warning in notes. Values the sequence already has are a success with changed=false, applied_count 0, saved=false and a note, and nothing is modified or saved. A failed save makes success false, with saved=false and save_error.
//   @param anim_sequence_path - Object path of the UAnimSequence to change, e.g. '/Game/Animation/Run_Fwd'.
//   @param retarget_source - Retarget-source name (FName). Must name an entry in the owning skeleton's retarget-source map to RESOLVE at runtime, but the property write persists regardless. Use 'None' (or empty) to clear.
//   @param ref_pose_seq_path - Optional object path of a UAnimSequence used as the additive retarget reference pose (RefPoseSeq). Pass empty/'None' to clear it.
//   @returns { anim_sequence_path, retarget_source, ref_pose_seq, ref_pose_seq_set, ref_pose_seq_cleared, applied_count, notes, changed, success, saved, save_error, note? }
TSharedPtr<FJsonObject> HandleSetAnimRetargetSource(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("anim_sequence_path"), AnimPath)) { return Missing; }

    UAnimSequence* Seq = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(AnimPath, TEXT("AnimSequence"), Seq)) { return Error; }

    const bool bHasSource = Params->HasField(TEXT("retarget_source"));
    const bool bHasRefPose = Params->HasField(TEXT("ref_pose_seq_path"));
    if (!bHasSource && !bHasRefPose)
    {
        return mcp_common::MakeErrorReply(
            TEXT("No properties provided. Supply retarget_source and/or ref_pose_seq_path"));
    }

    int32 AppliedCount = 0;
    TArray<FString> Notes;
    bool bRefPoseSet = false;
    bool bRefPoseCleared = false;

    // Resolve every supplied value before anything is touched: an unknown ref-pose sequence is refused, and
    // values the sequence already has change nothing.
    FName SourceName = NAME_None;
    if (bHasSource)
    {
        FString SourceStr;
        Params->TryGetStringField(TEXT("retarget_source"), SourceStr);
        SourceStr = SourceStr.TrimStartAndEnd();
        // Treat empty / "None" as a clear (NAME_None).
        SourceName = (SourceStr.IsEmpty() || SourceStr.Equals(TEXT("None"), ESearchCase::IgnoreCase))
            ? NAME_None : FName(*SourceStr);
    }
    UAnimSequence* NewRefSeq = nullptr;
    bool bClearRefPose = false;
    if (bHasRefPose)
    {
        FString RefPosePath;
        Params->TryGetStringField(TEXT("ref_pose_seq_path"), RefPosePath);
        RefPosePath = RefPosePath.TrimStartAndEnd();
        bClearRefPose = RefPosePath.IsEmpty() || RefPosePath.Equals(TEXT("None"), ESearchCase::IgnoreCase);
        if (!bClearRefPose)
        {
            NewRefSeq = AnimAssetUtils::LoadAsset<UAnimSequence>(RefPosePath);
            if (!NewRefSeq)
            {
                // Honest failure: a referenced ref-pose asset path that does not load is an error,
                // not a silent skip.
                return mcp_common::MakeErrorReply(
                    FString::Printf(TEXT("ref_pose_seq_path AnimSequence not found: %s"), *RefPosePath));
            }
        }
    }
    const bool bSourceChanges = bHasSource && Seq->RetargetSource != SourceName;
    const bool bRefPoseChanges = bHasRefPose && Seq->RefPoseSeq != NewRefSeq;

    TOptional<FScopedTransaction> Transaction;
    if (bSourceChanges || bRefPoseChanges)
    {
        Transaction.Emplace(LOCTEXT("SetAnimRetargetSource", "MCP: Set Anim Retarget Source"));
        Seq->Modify();
    }

    if (bHasSource)
    {
        if (bSourceChanges)
        {
            Seq->RetargetSource = SourceName;
            ++AppliedCount;
        }
        // Honest note: the write persists, but resolution depends on the skeleton's map.
        if (SourceName != NAME_None)
        {
            bool bResolves = false;
            if (const USkeleton* Skeleton = Seq->GetSkeleton())
            {
                // GetRetargetSources is the public ENGINE_API accessor for the
                // skeleton's retarget-source map (Skeleton.h).
                TArray<FName> Sources;
                Skeleton->GetRetargetSources(Sources);
                bResolves = Sources.Contains(SourceName);
            }
            if (!bResolves)
            {
                Notes.Add(FString::Printf(
                    TEXT("retarget_source '%s' is not an entry in the owning skeleton's retarget-source map; "
                         "the property persists but will not resolve at runtime until the skeleton defines it"),
                    *SourceName.ToString()));
            }
        }
    }

    if (bHasRefPose)
    {
        if (bRefPoseChanges)
        {
            Seq->RefPoseSeq = NewRefSeq;
            bRefPoseCleared = bClearRefPose;
            bRefPoseSet = !bClearRefPose;
            ++AppliedCount;
        }
        if (!bClearRefPose)
        {
            Notes.Add(TEXT("ref_pose_seq (RefPoseSeq) is meaningful only when the additive base pose type is AnimScaled or AnimFrame"));
        }
    }
    Transaction.Reset();

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("anim_sequence_path"), Seq->GetPathName());
    Obj->SetStringField(TEXT("retarget_source"), Seq->RetargetSource.ToString());
    Obj->SetStringField(TEXT("ref_pose_seq"), Seq->RefPoseSeq ? Seq->RefPoseSeq->GetPathName() : FString());
    Obj->SetBoolField(TEXT("ref_pose_seq_set"), bRefPoseSet);
    Obj->SetBoolField(TEXT("ref_pose_seq_cleared"), bRefPoseCleared);
    Obj->SetNumberField(TEXT("applied_count"), AppliedCount);
    Obj->SetArrayField(TEXT("notes"), mcp_common::StringsToJson(Notes));
    Obj->SetBoolField(TEXT("changed"), AppliedCount > 0);
    Obj->SetBoolField(TEXT("success"), true);
    if (AppliedCount == 0)
    {
        return AnimAuthoring_NoChangeReply(Obj, TEXT("The sequence already has that retarget source and reference-pose sequence; nothing changed and nothing was saved."), TEXT("note"));
    }

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Seq, SaveError);

    UE_LOG(LogTemp, Verbose, TEXT("[set_anim_retarget_source] %s -> retarget_source='%s' ref_pose_set=%d cleared=%d (%d field(s))"),
        *Seq->GetPathName(), *Seq->RetargetSource.ToString(), bRefPoseSet ? 1 : 0, bRefPoseCleared ? 1 : 0, AppliedCount);

    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("set_anim_retarget_source"), SaveError);
}

// ===========================================================================
// Montage section / segment authoring (deep)
//
// Operations on a persisted /Game UAnimMontage that complete the section +
// segment editing surface area of HandleAddMontageSection,
// HandleAddMontageSegment, HandleSetMontageBlendTimes, HandleSetMontageProperties,
// HandleLinkMontageSections and HandleDeleteMontageSection.
//
// Engine APIs used (Engine/Source/Runtime/Engine):
//   UAnimMontage::SetSkeleton(USkeleton*)              AnimationAsset.h  ENGINE_API
//   UAnimMontage::SetPreviewMesh(USkeletalMesh*, bool) AnimationAsset.h  ENGINE_API
//   UAnimSequenceBase::RefreshCacheData()              AnimSequenceBase.h  ENGINE_API
//   FCompositeSection::SectionName / StartTime         AnimMontage.h  public UPROPERTY
//   FAnimSegment::SetAnimReference(UAnimSequenceBase*) AnimCompositeBase.h  ENGINE_API
//   FAnimSegment::AnimStartTime                        AnimCompositeBase.h  BlueprintReadOnly UPROPERTY
//   FAnimTrack::CollapseAnimSegments()                 AnimCompositeBase.h  ENGINE_API  (WITH_EDITOR)
//   FAnimTrack::SortAnimSegments()                     AnimCompositeBase.h  ENGINE_API  (WITH_EDITOR)
//
// FAnimSegment.AnimSegments is BlueprintReadOnly; the canonical C++ write path is
// raw access + MarkPackageDirty + PostEditChange on the montage (the same recipe
// HandleAddMontageSegment uses for the Add path). Collapse/Sort are editor-only engine
// APIs, which this Editor module always has.
//
// All setters are idempotent: they overwrite the requested value. RefreshCacheData is
// also idempotent. SectionIndex / SegmentIndex are bounds-checked before any mutation.

// ---------------------------------------------------------------------------
// set_anim_montage_skeleton — rebind a UAnimMontage to a different USkeleton.
// UAnimMontage::SetSkeleton (AnimationAsset.h, ENGINE_API) clears the
// existing source link, sets the new Skeleton UPROPERTY and recomputes the
// VirtualBone Guid / RequiredBones cache; it does NOT auto-save. We follow with
// Modify + RefreshCacheData + SaveAssetPackage so the change persists.
// ---------------------------------------------------------------------------
// MCP:set_anim_montage_skeleton - Rebind a UAnimMontage to a different USkeleton (clears existing source link + recomputes bone caches) in an undo transaction and save it. The skeleton the montage already uses is a success with applied=false and saved=false, and the montage is neither modified nor saved. A failed save makes success false, with saved=false and save_error.
//   @param montage_path - Full asset path of the montage (e.g. '/Game/MyGame/AM_Example').
//   @param skeleton_path - Asset path of the new Skeleton (e.g. '/Engine/EngineMeshes/SkeletalCube_Skeleton').
//   @returns { montage_path, skeleton_path, prev_skeleton_path, applied, saved, save_error, message }
TSharedPtr<FJsonObject> HandleSetAnimMontageSkeleton(const TSharedPtr<FJsonObject>& Params)
{
    FString MontagePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("montage_path"), MontagePath)) { return Missing; }
    FString SkeletonPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("skeleton_path"), SkeletonPath)) { return Missing; }

    UAnimMontage* Montage = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(MontagePath, TEXT("AnimMontage"), Montage)) { return Error; }
    USkeleton* NewSkeleton = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(SkeletonPath, TEXT("Skeleton"), NewSkeleton)) { return Error; }

    const FString PrevSkeletonPath = Montage->GetSkeleton() ? Montage->GetSkeleton()->GetPathName() : FString();

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("montage_path"), Montage->GetPathName());
    Obj->SetStringField(TEXT("skeleton_path"), NewSkeleton->GetPathName());
    Obj->SetStringField(TEXT("prev_skeleton_path"), PrevSkeletonPath);

    // The skeleton the montage already has changes nothing: answered before Modify() would dirty it.
    if (Montage->GetSkeleton() == NewSkeleton)
    {
        Obj->SetBoolField(TEXT("applied"), false);
        return AnimAuthoring_NoChangeReply(Obj, TEXT("The montage already uses that skeleton; nothing changed, nothing saved"));
    }

    {
        const FScopedTransaction Transaction(LOCTEXT("SetAnimMontageSkeleton", "Set Anim Montage Skeleton"));
        Montage->Modify();
        Montage->SetSkeleton(NewSkeleton);
        Montage->RefreshCacheData();
    }

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Montage, SaveError);

    Obj->SetBoolField(TEXT("applied"), true);
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Montage skeleton set"), bSaved, SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("set_anim_montage_skeleton"), SaveError);
}

// ---------------------------------------------------------------------------
// set_anim_montage_preview_mesh — assign the preview USkeletalMesh on a montage.
// UAnimMontage::SetPreviewMesh (AnimationAsset.h, ENGINE_API virtual override)
// writes PreviewSkeletalMesh UPROPERTY; the second arg bMarkAsDirty=true defaults
// to dirty-marking, but we still MarkPackageDirty explicitly so editor save flows
// pick the change up the same way as the rest of this file.
// ---------------------------------------------------------------------------
// MCP:set_anim_montage_preview_mesh - Set the editor preview USkeletalMesh of a UAnimMontage (SetPreviewMesh, one undo step) and save the montage; it only changes what the asset editor shows, and the mesh skeleton is not checked against the montage. Returns mesh_path, prev_mesh_path and applied; the preview mesh the montage already has is a success with applied=false and saved=false, and the montage is neither modified nor saved. A failed save makes success false, with saved=false and save_error.
//   @param montage_path - Full asset path of the montage (e.g. '/Game/MyGame/AM_Example').
//   @param mesh_path - Asset path of the preview USkeletalMesh (e.g. '/Engine/EngineMeshes/SkeletalCube').
//   @returns { montage_path, mesh_path, prev_mesh_path, applied, saved, save_error, message }
TSharedPtr<FJsonObject> HandleSetAnimMontagePreviewMesh(const TSharedPtr<FJsonObject>& Params)
{
    FString MontagePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("montage_path"), MontagePath)) { return Missing; }
    FString MeshPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("mesh_path"), MeshPath)) { return Missing; }

    UAnimMontage* Montage = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(MontagePath, TEXT("AnimMontage"), Montage)) { return Error; }
    USkeletalMesh* NewMesh = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(MeshPath, TEXT("SkeletalMesh"), NewMesh)) { return Error; }

    USkeletalMesh* PrevMesh = Montage->GetPreviewMesh();
    const FString PrevMeshPath = PrevMesh ? PrevMesh->GetPathName() : FString();

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("montage_path"), Montage->GetPathName());
    Obj->SetStringField(TEXT("mesh_path"), NewMesh->GetPathName());
    Obj->SetStringField(TEXT("prev_mesh_path"), PrevMeshPath);

    // The preview mesh the montage already has changes nothing: answered before Modify() would dirty it.
    if (PrevMesh == NewMesh)
    {
        Obj->SetBoolField(TEXT("applied"), false);
        return AnimAuthoring_NoChangeReply(Obj, TEXT("The montage already has that preview mesh; nothing changed, nothing saved"));
    }

    {
        const FScopedTransaction Transaction(LOCTEXT("SetAnimMontagePreviewMesh", "Set Anim Montage Preview Mesh"));
        Montage->Modify();
        Montage->SetPreviewMesh(NewMesh, /*bMarkAsDirty=*/true);
    }

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Montage, SaveError);

    Obj->SetBoolField(TEXT("applied"), true);
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Montage preview mesh set"), bSaved, SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("set_anim_montage_preview_mesh"), SaveError);
}

namespace
{
    // The values of the serialized montage fields UAnimMontage::RefreshCacheData can rewrite, copied so a refresh
    // can be compared with them. In the engine source, UAnimSequenceBase::RefreshCacheData sorts Notifies, moves a
    // notify to another track index and adds AnimNotifyTracks, and UAnimMontage::RefreshBranchingPointMarkers
    // rebuilds BranchingPointMarkers and BranchingPointStateNotifyIndices. They are read through reflection (one
    // is private); a field that cannot be found makes every comparison come out as a change.
    class FAnimAuthoring_MontageCacheSnapshot
    {
    public:
        explicit FAnimAuthoring_MontageCacheSnapshot(const UAnimMontage* Montage)
        {
            static const TCHAR* const FieldNames[] = { TEXT("Notifies"), TEXT("AnimNotifyTracks"),
                TEXT("BranchingPointMarkers"), TEXT("BranchingPointStateNotifyIndices") };
            for (const TCHAR* FieldName : FieldNames)
            {
                const FProperty* Property = FindFProperty<FProperty>(UAnimMontage::StaticClass(), FieldName);
                if (!Property)
                {
                    bComplete = false;
                    continue;
                }
                FDefaultConstructedPropertyElement Copy(Property);
                Property->CopyCompleteValue(Copy.GetObjAddress(), Property->ContainerPtrToValuePtr<void>(Montage));
                Fields.Emplace(Property, MoveTemp(Copy));
            }
        }

        // Whether the montage still holds the copied values: FProperty::Identical, every element and field in order.
        bool Matches(const UAnimMontage* Montage) const
        {
            if (!bComplete)
            {
                return false;
            }
            for (const TPair<const FProperty*, FDefaultConstructedPropertyElement>& Field : Fields)
            {
                if (!Field.Key->Identical(Field.Value.GetObjAddress(), Field.Key->ContainerPtrToValuePtr<void>(Montage), PPF_None))
                {
                    return false;
                }
            }
            return true;
        }

    private:
        TArray<TPair<const FProperty*, FDefaultConstructedPropertyElement>> Fields;
        bool bComplete = true;
    };
}

// ---------------------------------------------------------------------------
// refresh_anim_montage_cache — re-run UAnimMontage::RefreshCacheData().
// ---------------------------------------------------------------------------
// MCP:refresh_anim_montage_cache - Re-run UAnimMontage::RefreshCacheData on a UAnimMontage in an undo transaction (it sorts the notifies, moves a notify off a missing or overlapping notify track, adding tracks as needed, and rebuilds the branching point markers) and save it. Idempotent: when the refresh rewrites none of those fields the reply is a success with refreshed=false and saved=false, the montage keeps the dirty state it had and nothing is saved. A failed save makes success false, with saved=false and save_error.
//   @param montage_path - Full asset path of the montage (e.g. '/Game/MyGame/AM_Example').
//   @returns { montage_path, refreshed, sequence_length, slot_count, section_count, saved, save_error, message }
TSharedPtr<FJsonObject> HandleRefreshAnimMontageCache(const TSharedPtr<FJsonObject>& Params)
{
    FString MontagePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("montage_path"), MontagePath)) { return Missing; }

    UAnimMontage* Montage = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(MontagePath, TEXT("AnimMontage"), Montage)) { return Error; }

    // What the refresh rewrites is compared with a copy taken before it; when it rewrote nothing the montage goes
    // back to the dirty state it had (Modify() marked it), and a transaction whose records hold no change leaves
    // no undo step (FTransaction::IsTransient).
    const FAnimAuthoring_MontageCacheSnapshot Before(Montage);
    UPackage* const Package = Montage->GetPackage();
    const bool bWasDirty = Package && Package->IsDirty();
    {
        const FScopedTransaction Transaction(LOCTEXT("RefreshAnimMontageCache", "Refresh Anim Montage Cache"));
        Montage->Modify();
        Montage->RefreshCacheData();
    }

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("montage_path"), Montage->GetPathName());
    Obj->SetNumberField(TEXT("sequence_length"), Montage->GetPlayLength());
    Obj->SetNumberField(TEXT("slot_count"), Montage->SlotAnimTracks.Num());
    Obj->SetNumberField(TEXT("section_count"), Montage->CompositeSections.Num());

    if (Before.Matches(Montage))
    {
        if (Package)
        {
            Package->SetDirtyFlag(bWasDirty);
        }
        Obj->SetBoolField(TEXT("refreshed"), false);
        return AnimAuthoring_NoChangeReply(Obj, TEXT("The montage cache was already current; nothing changed, nothing saved"));
    }

    // Persist so the sorted notifies, their tracks and the branching point markers land on disk too.
    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Montage, SaveError);

    Obj->SetBoolField(TEXT("refreshed"), true);
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Montage cache refreshed"), bSaved, SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("refresh_anim_montage_cache"), SaveError);
}

// ---------------------------------------------------------------------------
// rename_montage_section — overwrite SectionName on an existing FCompositeSection by index.
// FCompositeSection::SectionName (AnimMontage.h) is a public UPROPERTY FName;
// surgical write under Modify + RefreshCacheData keeps NotifyTracks in sync.
// ---------------------------------------------------------------------------
// MCP:rename_montage_section - Rename an existing FCompositeSection on a UAnimMontage by zero-based section_index, in an undo transaction, and save it. The name the section already has (FName match, case ignored) is a success with applied=false and saved=false, and the montage is neither modified nor saved. A failed save makes success false, with saved=false and save_error.
//   @param montage_path - Full asset path of the montage (e.g. '/Game/MyGame/AM_Example').
//   @param section_index - Zero-based index into CompositeSections (use get_montage_info first to list).
//   @param new_name - New FName for the section.
//   @returns { montage_path, section_index, old_name, new_name, section_count, sections, applied, saved, save_error, message }
TSharedPtr<FJsonObject> HandleRenameMontageSection(const TSharedPtr<FJsonObject>& Params)
{
    FString MontagePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("montage_path"), MontagePath)) { return Missing; }
    double SectionIndex = -1.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("section_index"), SectionIndex)) { return Missing; }
    FString NewName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("new_name"), NewName)) { return Missing; }

    if (TSharedPtr<FJsonObject> Error = AnimAuthoring_CheckIndex(SectionIndex, TEXT("section_index"))) { return Error; }
    const int32 Index = static_cast<int32>(SectionIndex);

    UAnimMontage* Montage = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(MontagePath, TEXT("AnimMontage"), Montage)) { return Error; }
    if (Index >= Montage->CompositeSections.Num())
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("section_index %d out of range (section_count=%d)"), Index, Montage->CompositeSections.Num()));
    }

    const FName OldName = Montage->CompositeSections[Index].SectionName;
    const FName NewFName(*NewName);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("montage_path"), Montage->GetPathName());
    Obj->SetNumberField(TEXT("section_index"), Index);
    Obj->SetStringField(TEXT("old_name"), OldName.ToString());
    Obj->SetStringField(TEXT("new_name"), NewName);

    // The name the section already has (FName match) changes nothing: answered before Modify() would dirty it.
    if (OldName == NewFName)
    {
        Obj->SetNumberField(TEXT("section_count"), Montage->CompositeSections.Num());
        Obj->SetArrayField(TEXT("sections"), CollectSectionNames(Montage));
        Obj->SetBoolField(TEXT("applied"), false);
        return AnimAuthoring_NoChangeReply(Obj, TEXT("The section already has that name; nothing changed, nothing saved"));
    }

    {
        const FScopedTransaction Transaction(LOCTEXT("RenameMontageSection", "Rename Montage Section"));
        Montage->Modify();
        Montage->CompositeSections[Index].SectionName = NewFName;
        Montage->RefreshCacheData();
    }

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Montage, SaveError);

    Obj->SetNumberField(TEXT("section_count"), Montage->CompositeSections.Num());
    Obj->SetArrayField(TEXT("sections"), CollectSectionNames(Montage));
    Obj->SetBoolField(TEXT("applied"), true);
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Montage section renamed"), bSaved, SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("rename_montage_section"), SaveError);
}

// ---------------------------------------------------------------------------
// set_montage_section_start_time — overwrite StartTime on an FCompositeSection by index.
// FCompositeSection::StartTime is a public UPROPERTY float (AnimMontage.h);
// write + RefreshCacheData keeps the section's LinkValue / derived bookkeeping in sync.
// ---------------------------------------------------------------------------
// MCP:set_montage_section_start_time - Set the StartTime on an existing FCompositeSection by zero-based section_index, in an undo transaction, and save it. The start time the section already has is a success with applied=false and saved=false, and the montage is neither modified nor saved. A failed save makes success false, with saved=false and save_error.
//   @param montage_path - Full asset path of the montage (e.g. '/Game/MyGame/AM_Example').
//   @param section_index - Zero-based index into CompositeSections.
//   @param start_time - Absolute section start time in seconds.
//   @returns { montage_path, section_index, start_time, applied, saved, save_error, message }
TSharedPtr<FJsonObject> HandleSetMontageSectionStartTime(const TSharedPtr<FJsonObject>& Params)
{
    FString MontagePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("montage_path"), MontagePath)) { return Missing; }
    double SectionIndex = -1.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("section_index"), SectionIndex)) { return Missing; }
    double StartTime = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("start_time"), StartTime)) { return Missing; }

    if (TSharedPtr<FJsonObject> Error = AnimAuthoring_CheckIndex(SectionIndex, TEXT("section_index"))) { return Error; }
    const int32 Index = static_cast<int32>(SectionIndex);

    UAnimMontage* Montage = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(MontagePath, TEXT("AnimMontage"), Montage)) { return Error; }
    if (Index >= Montage->CompositeSections.Num())
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("section_index %d out of range (section_count=%d)"), Index, Montage->CompositeSections.Num()));
    }

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("montage_path"), Montage->GetPathName());
    Obj->SetNumberField(TEXT("section_index"), Index);

    // The start time the section already has changes nothing: answered before Modify() would dirty the montage.
    if (Montage->CompositeSections[Index].GetTime(EAnimLinkMethod::Absolute) == static_cast<float>(StartTime))
    {
        Obj->SetNumberField(TEXT("start_time"), static_cast<double>(Montage->CompositeSections[Index].GetTime(EAnimLinkMethod::Absolute)));
        Obj->SetBoolField(TEXT("applied"), false);
        return AnimAuthoring_NoChangeReply(Obj, TEXT("The section already starts at that time; nothing changed, nothing saved"));
    }

    {
        const FScopedTransaction Transaction(LOCTEXT("SetMontageSectionStartTime", "Set Montage Section Start Time"));
        Montage->Modify();
        // FCompositeSection::StartTime is deprecated in favor of FAnimLinkableElement::SetTime/GetTime.
        // Use the inherited setter with EAnimLinkMethod::Absolute (the default; the alternative Relative/Proportional require a valid SegmentBeginTime).
        Montage->CompositeSections[Index].SetTime(static_cast<float>(StartTime), EAnimLinkMethod::Absolute);
        Montage->RefreshCacheData();
    }

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Montage, SaveError);

    Obj->SetNumberField(TEXT("start_time"), static_cast<double>(Montage->CompositeSections[Index].GetTime(EAnimLinkMethod::Absolute)));
    Obj->SetBoolField(TEXT("applied"), true);
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Montage section start time set"), bSaved, SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("set_montage_section_start_time"), SaveError);
}

// ---------------------------------------------------------------------------
// Helpers for segment-by-(slot,index) editing.
// Resolve a FSlotAnimationTrack by slot name (FName) and a FAnimSegment inside
// its AnimTrack by zero-based index. Returns nullptr on miss. The caller can
// detect "not found" via the Out* params without an extra string scan.
// ---------------------------------------------------------------------------
namespace
{
    bool FindMontageSlotAndSegment(UAnimMontage* Montage, const FString& SlotName, int32 SegmentIndex,
                                  FSlotAnimationTrack*& OutSlot, FAnimSegment*& OutSegment,
                                  FString& OutError)
    {
        OutSlot = nullptr;
        OutSegment = nullptr;
        if (!Montage)
        {
            OutError = TEXT("null montage");
            return false;
        }
        if (SegmentIndex < 0)
        {
            OutError = FString::Printf(TEXT("segment_index %d out of range (must be >= 0)"), SegmentIndex);
            return false;
        }
        const FName SlotFName(*SlotName);
        for (FSlotAnimationTrack& Track : Montage->SlotAnimTracks)
        {
            if (Track.SlotName == SlotFName)
            {
                if (SegmentIndex >= Track.AnimTrack.AnimSegments.Num())
                {
                    OutError = FString::Printf(TEXT("segment_index %d out of range (segment_count=%d)"),
                        SegmentIndex, Track.AnimTrack.AnimSegments.Num());
                    return false;
                }
                OutSlot = &Track;
                OutSegment = &Track.AnimTrack.AnimSegments[SegmentIndex];
                return true;
            }
        }
        OutError = FString::Printf(TEXT("slot '%s' not found on montage"), *SlotName);
        return false;
    }
}

// ---------------------------------------------------------------------------
// set_montage_segment_anim_sequence — rebind the AnimReference on an existing FAnimSegment.
// FAnimSegment::SetAnimReference (AnimCompositeBase.h, ENGINE_API) writes AnimReference
// and (when bInitialize=true) sets AnimStartTime=0 + AnimEndTime=AnimSequenceLength.
// The raw AnimReference field is BlueprintReadOnly (AnimCompositeBase.h) — we go
// through the public setter, which is the supported C++ write path.
// ---------------------------------------------------------------------------
// MCP:set_montage_segment_anim_sequence - Rebind an existing FAnimSegment on a slot track to a different UAnimSequence (resets segment start/end times to the new sequence's range, play rate and loop count to 1 and its start position to 0) in an undo transaction and save the montage. A segment that already references that sequence with those values is a success with applied=false and saved=false, and the montage is neither modified nor saved. A failed save makes success false, with saved=false and save_error.
//   @param montage_path - Full asset path of the montage (e.g. '/Game/MyGame/AM_Example').
//   @param slot_name - Name of the slot track containing the segment (e.g. 'DefaultSlot').
//   @param segment_index - Zero-based index into the slot track's AnimSegments.
//   @param anim_sequence_path - Object path of the UAnimSequence to reference (must share the montage's skeleton).
//   @returns { montage_path, slot_name, segment_index, anim_sequence_path, anim_start_time, anim_end_time, applied, saved, save_error, message }
TSharedPtr<FJsonObject> HandleSetMontageSegmentAnimSequence(const TSharedPtr<FJsonObject>& Params)
{
    FString MontagePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("montage_path"), MontagePath)) { return Missing; }
    FString SlotName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("slot_name"), SlotName)) { return Missing; }
    double SegmentIndex = -1.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("segment_index"), SegmentIndex)) { return Missing; }
    FString AnimPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("anim_sequence_path"), AnimPath)) { return Missing; }

    if (TSharedPtr<FJsonObject> Error = AnimAuthoring_CheckIndex(SegmentIndex, TEXT("segment_index"))) { return Error; }
    const int32 Index = static_cast<int32>(SegmentIndex);

    UAnimMontage* Montage = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(MontagePath, TEXT("AnimMontage"), Montage)) { return Error; }
    UAnimSequenceBase* NewAnim = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(AnimPath, TEXT("AnimSequence"), NewAnim)) { return Error; }
    if (Montage->GetSkeleton() && NewAnim->GetSkeleton() && Montage->GetSkeleton() != NewAnim->GetSkeleton())
    {
        return mcp_common::MakeErrorReply(TEXT("Montage and AnimSequence use different skeletons"));
    }

    FSlotAnimationTrack* SlotTrack = nullptr;
    FAnimSegment* Segment = nullptr;
    FString ResolveError;
    if (!FindMontageSlotAndSegment(Montage, SlotName, Index, SlotTrack, Segment, ResolveError))
    {
        return mcp_common::MakeErrorReply(ResolveError);
    }

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("montage_path"), Montage->GetPathName());
    Obj->SetStringField(TEXT("slot_name"), SlotName);
    Obj->SetNumberField(TEXT("segment_index"), Index);
    Obj->SetStringField(TEXT("anim_sequence_path"), NewAnim->GetPathName());

    // SetAnimReference(bInitialize=true) writes the reference, its cached play length, AnimStartTime 0,
    // AnimEndTime the play length, AnimPlayRate 1, LoopingCount 1 and StartPos 0: a segment that already
    // holds all of them changes nothing and is answered before Modify() would dirty the montage.
    if (Segment->GetAnimReference() == NewAnim && !Segment->IsPlayLengthOutOfDate() && Segment->AnimStartTime == 0.f
        && Segment->AnimEndTime == static_cast<float>(NewAnim->GetPlayLength()) && Segment->AnimPlayRate == 1.f
        && Segment->LoopingCount == 1 && Segment->StartPos == 0.f)
    {
        Obj->SetNumberField(TEXT("anim_start_time"), static_cast<double>(Segment->AnimStartTime));
        Obj->SetNumberField(TEXT("anim_end_time"), static_cast<double>(Segment->AnimEndTime));
        Obj->SetBoolField(TEXT("applied"), false);
        return AnimAuthoring_NoChangeReply(Obj, TEXT("The segment already references that sequence over its full range; nothing changed, nothing saved"));
    }

    {
        const FScopedTransaction Transaction(LOCTEXT("SetMontageSegmentAnimSequence", "Set Montage Segment Anim Sequence"));
        Montage->Modify();
        // bInitialize=true resets AnimStartTime=0 / AnimEndTime=AnimSequenceLength -- the
        // supported way to switch a segment's underlying animation.
        Segment->SetAnimReference(NewAnim, /*bInitialize=*/true);
        Montage->RefreshCacheData();
    }

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Montage, SaveError);

    Obj->SetNumberField(TEXT("anim_start_time"), static_cast<double>(Segment->AnimStartTime));
    Obj->SetNumberField(TEXT("anim_end_time"), static_cast<double>(Segment->AnimEndTime));
    Obj->SetBoolField(TEXT("applied"), true);
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Montage segment anim reference set"), bSaved, SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("set_montage_segment_anim_sequence"), SaveError);
}

// ---------------------------------------------------------------------------
// set_montage_segment_start_time — overwrite AnimStartTime on an existing FAnimSegment.
// FAnimSegment::AnimStartTime (AnimCompositeBase.h) is a BlueprintReadOnly UPROPERTY
// float; raw access is the canonical C++ write path (HandleAddMontageSegment writes the
// same member via FAnimSegment::SetAnimReference with bInitialize=true, which also touches
// AnimEndTime). Here we only move the START of an already-bound segment — the user keeps
// AnimEndTime as-is to preserve the segment's play-length intent.
// ---------------------------------------------------------------------------
// MCP:set_montage_segment_start_time - Set the AnimStartTime on an existing FAnimSegment by (slot_name, segment_index), clamped to [0, AnimEndTime], in an undo transaction, and save the montage. AnimEndTime is left untouched. The (clamped) start time the segment already has is a success with applied=false and saved=false, and the montage is neither modified nor saved. A failed save makes success false, with saved=false and save_error.
//   @param montage_path - Full asset path of the montage (e.g. '/Game/MyGame/AM_Example').
//   @param slot_name - Name of the slot track containing the segment.
//   @param segment_index - Zero-based index into the slot track's AnimSegments.
//   @param start_time - New AnimStartTime in seconds (clamped to [0, AnimEndTime]).
//   @returns { montage_path, slot_name, segment_index, start_time, anim_end_time, applied, saved, save_error, message, note? }
TSharedPtr<FJsonObject> HandleSetMontageSegmentStartTime(const TSharedPtr<FJsonObject>& Params)
{
    FString MontagePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("montage_path"), MontagePath)) { return Missing; }
    FString SlotName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("slot_name"), SlotName)) { return Missing; }
    double SegmentIndex = -1.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("segment_index"), SegmentIndex)) { return Missing; }
    double StartTime = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("start_time"), StartTime)) { return Missing; }

    if (TSharedPtr<FJsonObject> Error = AnimAuthoring_CheckIndex(SegmentIndex, TEXT("segment_index"))) { return Error; }
    const int32 Index = static_cast<int32>(SegmentIndex);

    UAnimMontage* Montage = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(MontagePath, TEXT("AnimMontage"), Montage)) { return Error; }

    FSlotAnimationTrack* SlotTrack = nullptr;
    FAnimSegment* Segment = nullptr;
    FString ResolveError;
    if (!FindMontageSlotAndSegment(Montage, SlotName, Index, SlotTrack, Segment, ResolveError))
    {
        return mcp_common::MakeErrorReply(ResolveError);
    }

    // Clamp to [0, AnimEndTime] so the segment cannot invert (end < start). The same
    // self-check happens inside the engine on PostEditChange, but clamping here keeps
    // the response honest about what was actually written.
    const float NewStart = static_cast<float>(StartTime);
    const float ClampedStart = FMath::Clamp(NewStart, 0.0f, Segment->AnimEndTime);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("montage_path"), Montage->GetPathName());
    Obj->SetStringField(TEXT("slot_name"), SlotName);
    Obj->SetNumberField(TEXT("segment_index"), Index);
    if (!FMath::IsNearlyEqual(NewStart, ClampedStart))
    {
        Obj->SetStringField(TEXT("note"), FString::Printf(
            TEXT("Requested start_time=%f was clamped to AnimEndTime=%f"), NewStart, Segment->AnimEndTime));
    }

    // The (clamped) start time the segment already has changes nothing: answered before Modify() would dirty
    // the montage.
    if (Segment->AnimStartTime == ClampedStart)
    {
        Obj->SetNumberField(TEXT("start_time"), static_cast<double>(Segment->AnimStartTime));
        Obj->SetNumberField(TEXT("anim_end_time"), static_cast<double>(Segment->AnimEndTime));
        Obj->SetBoolField(TEXT("applied"), false);
        return AnimAuthoring_NoChangeReply(Obj, TEXT("The segment already starts at that time; nothing changed, nothing saved"));
    }

    {
        const FScopedTransaction Transaction(LOCTEXT("SetMontageSegmentStartTime", "Set Montage Segment Start Time"));
        Montage->Modify();
        Segment->AnimStartTime = ClampedStart;
        Montage->RefreshCacheData();
    }

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Montage, SaveError);

    Obj->SetNumberField(TEXT("start_time"), static_cast<double>(Segment->AnimStartTime));
    Obj->SetNumberField(TEXT("anim_end_time"), static_cast<double>(Segment->AnimEndTime));
    Obj->SetBoolField(TEXT("applied"), true);
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Montage segment start time set"), bSaved, SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("set_montage_segment_start_time"), SaveError);
}

// ---------------------------------------------------------------------------
// collapse_montage_segments — FAnimTrack::CollapseAnimSegments() on a slot track.
// AnimCompositeBase.h — moves AnimSegments so there are no gaps between one
// finishing and the next starting (preserves order). WITH_EDITOR-only.
// ---------------------------------------------------------------------------
// MCP:collapse_montage_segments - Collapse a montage slot track's segments end-to-end (FAnimTrack::CollapseAnimSegments: no gaps, order preserved) in an undo transaction and save the montage. Segments already end-to-end are a success with applied=false and saved=false, and the montage is neither modified nor saved. An unknown slot is an error. A failed save makes success false, with saved=false and save_error.
//   @param montage_path - Full asset path of the montage (e.g. '/Game/MyGame/AM_Example').
//   @param slot_name - Name of the slot track to collapse.
//   @returns { montage_path, slot_name, segment_count, applied, saved, save_error, message }
TSharedPtr<FJsonObject> HandleCollapseMontageSegments(const TSharedPtr<FJsonObject>& Params)
{
    FString MontagePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("montage_path"), MontagePath)) { return Missing; }
    FString SlotName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("slot_name"), SlotName)) { return Missing; }

    UAnimMontage* Montage = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(MontagePath, TEXT("AnimMontage"), Montage)) { return Error; }

    FSlotAnimationTrack* SlotTrack = AnimAuthoring_FindSlotTrack(Montage, FName(*SlotName));
    if (!SlotTrack)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("slot '%s' not found on montage"), *SlotName));
    }

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("montage_path"), Montage->GetPathName());
    Obj->SetStringField(TEXT("slot_name"), SlotName);
    Obj->SetNumberField(TEXT("segment_count"), SlotTrack->AnimTrack.AnimSegments.Num());

    // Segments already end-to-end change nothing: the engine's own collapse runs on a copy of the track, and a
    // result equal to it (every serialized field of every segment) is answered before Modify() would dirty it.
    FAnimTrack Collapsed = SlotTrack->AnimTrack;
    Collapsed.CollapseAnimSegments();
    if (FAnimTrack::StaticStruct()->CompareScriptStruct(&Collapsed, &SlotTrack->AnimTrack, PPF_None))
    {
        Obj->SetBoolField(TEXT("applied"), false);
        return AnimAuthoring_NoChangeReply(Obj, TEXT("The segments are already end-to-end; nothing changed, nothing saved"));
    }

    {
        const FScopedTransaction Transaction(LOCTEXT("CollapseMontageSegments", "Collapse Montage Segments"));
        Montage->Modify();
        SlotTrack->AnimTrack.CollapseAnimSegments();
        Montage->RefreshCacheData();
    }

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Montage, SaveError);

    Obj->SetBoolField(TEXT("applied"), true);
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Montage segments collapsed"), bSaved, SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("collapse_montage_segments"), SaveError);
}

// ---------------------------------------------------------------------------
// sort_montage_segments — FAnimTrack::SortAnimSegments() on a slot track.
// AnimCompositeBase.h — sorts AnimSegments by StartTime ascending.
// WITH_EDITOR-only.
// ---------------------------------------------------------------------------
// MCP:sort_montage_segments - Sort the segments of one slot track of a UAnimMontage asset by their position on the track, ascending (FAnimTrack::SortAnimSegments), which then lays them end-to-end from 0 and clamps each segment's end time to its animation's length, in one undo step, and save the montage. Segment indices follow the new order, and the sort key is the track position, not the AnimStartTime that set_montage_segment_start_time edits. Idempotent: a track the sort leaves unchanged (already in order, end-to-end and within its animations) is a success with applied=false and saved=false, and nothing is modified or saved. An unknown montage or slot is an error; a failed save makes success false with saved=false and save_error. To close gaps but keep the array order: collapse_montage_segments; slot names: get_montage_info.
//   @param montage_path - Full asset path of the montage (e.g. '/Game/MyGame/AM_Example').
//   @param slot_name - Name of the slot track to sort, e.g. "DefaultSlot" (case-insensitive); an unknown slot is an error.
//   @returns { montage_path, slot_name, segment_count, applied, saved, save_error, message }
TSharedPtr<FJsonObject> HandleSortMontageSegments(const TSharedPtr<FJsonObject>& Params)
{
    FString MontagePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("montage_path"), MontagePath)) { return Missing; }
    FString SlotName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("slot_name"), SlotName)) { return Missing; }

    UAnimMontage* Montage = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(MontagePath, TEXT("AnimMontage"), Montage)) { return Error; }

    FSlotAnimationTrack* SlotTrack = AnimAuthoring_FindSlotTrack(Montage, FName(*SlotName));
    if (!SlotTrack)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("slot '%s' not found on montage"), *SlotName));
    }

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("montage_path"), Montage->GetPathName());
    Obj->SetStringField(TEXT("slot_name"), SlotName);
    Obj->SetNumberField(TEXT("segment_count"), SlotTrack->AnimTrack.AnimSegments.Num());

    // Segments the sort would leave as they are change nothing: the engine's own sort (with the segment-time
    // validation it runs) works on a copy of the track, and a result equal to it is answered before Modify()
    // would dirty the montage.
    FAnimTrack Sorted = SlotTrack->AnimTrack;
    Sorted.SortAnimSegments();
    if (FAnimTrack::StaticStruct()->CompareScriptStruct(&Sorted, &SlotTrack->AnimTrack, PPF_None))
    {
        Obj->SetBoolField(TEXT("applied"), false);
        return AnimAuthoring_NoChangeReply(Obj, TEXT("The segments are already sorted and end-to-end; nothing changed, nothing saved"));
    }

    {
        const FScopedTransaction Transaction(LOCTEXT("SortMontageSegments", "Sort Montage Segments"));
        Montage->Modify();
        SlotTrack->AnimTrack.SortAnimSegments();
        Montage->RefreshCacheData();
    }

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Montage, SaveError);

    Obj->SetBoolField(TEXT("applied"), true);
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Montage segments sorted"), bSaved, SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("sort_montage_segments"), SaveError);
}

#undef LOCTEXT_NAMESPACE

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_anim_composite_segment"), &HandleAddAnimCompositeSegment },
    { TEXT("add_anim_curve_name"), &HandleAddAnimCurveName },
    { TEXT("remove_anim_notify_name"), &HandleRemoveAnimNotifyName },
    { TEXT("remove_montage_segment"), &HandleRemoveMontageSegment },
    { TEXT("add_anim_float_curve"), &HandleAddAnimFloatCurve },
    { TEXT("add_anim_notify"), &HandleAddAnimNotify },
    { TEXT("add_anim_notify_name"), &HandleAddAnimNotifyName },
    { TEXT("add_anim_notify_state"), &HandleAddAnimNotifyState },
    { TEXT("add_anim_sync_marker"), &HandleAddAnimSyncMarker },
    { TEXT("add_anim_transform_curve"), &HandleAddAnimTransformCurve },
    { TEXT("add_blend_space_sample"), &HandleAddBlendspaceSample },
    { TEXT("add_montage_notify"), &HandleAddMontageNotify },
    { TEXT("add_montage_section"), &HandleAddMontageSection },
    { TEXT("add_montage_segment"), &HandleAddMontageSegment },
    { TEXT("add_montage_slot"), &HandleAddMontageSlot },
    { TEXT("add_notify_track"), &HandleAddNotifyTrack },
    { TEXT("add_pose_to_pose_asset"), &HandleAddPoseToPoseAsset },
    { TEXT("create_anim_composite"), &HandleCreateAnimComposite },
    { TEXT("create_anim_montage"), &HandleCreateAnimMontage },
    { TEXT("create_anim_sequence"), &HandleCreateAnimSequence },
    { TEXT("create_blend_space"), &HandleCreateBlendSpace },
    { TEXT("create_pose_asset"), &HandleCreatePoseAsset },
    { TEXT("create_pose_asset_from_anim_sequence"), &HandleCreatePoseAssetFromAnimSequence },
    { TEXT("remove_montage_section"), &HandleDeleteMontageSection },
    { TEXT("set_anim_notify"), &HandleEditAnimNotify },
    { TEXT("set_blend_space_sample"), &HandleEditBlendspaceSample },
    { TEXT("get_anim_asset_info"), &HandleGetAnimAssetInfo },
    { TEXT("get_anim_curve_data"), &HandleGetAnimCurveData },
    { TEXT("get_anim_notifies"), &HandleGetAnimNotifies },
    { TEXT("get_anim_sequence_info"), &HandleGetAnimSequenceInfo },
    { TEXT("get_blend_space_samples"), &HandleGetBlendspaceSamples },
    { TEXT("get_montage_info"), &HandleGetMontageInfo },
    { TEXT("link_montage_sections"), &HandleLinkMontageSections },
    { TEXT("refresh_anim_montage_cache"), &HandleRefreshAnimMontageCache },
    { TEXT("reimport_anim_sequence"), &HandleReimportAnimSequence },
    { TEXT("remove_anim_bone_track"), &HandleRemoveAnimBoneTrack },
    { TEXT("remove_anim_composite_segment"), &HandleRemoveAnimCompositeSegment },
    { TEXT("remove_anim_curve"), &HandleRemoveAnimCurve },
    { TEXT("remove_anim_notify"), &HandleRemoveAnimNotify },
    { TEXT("remove_anim_sync_marker"), &HandleRemoveAnimSyncMarker },
    { TEXT("remove_blend_space_sample"), &HandleRemoveBlendspaceSample },
    { TEXT("remove_montage_notify"), &HandleRemoveMontageNotify },
    { TEXT("remove_montage_slot"), &HandleRemoveMontageSlot },
    { TEXT("remove_notify_track"), &HandleRemoveNotifyTrack },
    { TEXT("remove_pose_curve"), &HandleRemovePoseCurve },
    { TEXT("remove_pose_from_pose_asset"), &HandleRemovePoseFromPoseAsset },
    { TEXT("rename_anim_curve"), &HandleRenameAnimCurve },
    { TEXT("rename_montage_section"), &HandleRenameMontageSection },
    { TEXT("rename_pose_in_pose_asset"), &HandleRenamePoseInPoseAsset },
    { TEXT("scale_anim_curve"), &HandleScaleAnimCurve },
    { TEXT("set_anim_additive_settings"), &HandleSetAnimAdditiveSettings },
    { TEXT("set_anim_bone_track_keys"), &HandleSetAnimBoneTrackKeys },
    { TEXT("set_anim_curve_color"), &HandleSetAnimCurveColor },
    { TEXT("set_anim_curve_comment"), &HandleSetAnimCurveComment },
    { TEXT("set_anim_curve_flags"), &HandleSetAnimCurveInterpMode },
    { TEXT("set_anim_frame_rate"), &HandleSetAnimFrameRate },
    { TEXT("set_anim_montage_preview_mesh"), &HandleSetAnimMontagePreviewMesh },
    { TEXT("set_anim_montage_skeleton"), &HandleSetAnimMontageSkeleton },
    { TEXT("set_anim_number_of_frames"), &HandleSetAnimNumberOfFrames },
    { TEXT("set_anim_retarget_source"), &HandleSetAnimRetargetSource },
    { TEXT("set_anim_root_motion"), &HandleSetAnimRootMotion },
    { TEXT("set_blend_space_interpolation"), &HandleSetBlendSpaceInterpolation },
    { TEXT("set_blend_space_properties"), &HandleSetBlendSpaceProperties },
    { TEXT("set_montage_properties"), &HandleSetMontageProperties },
    { TEXT("set_montage_section_start_time"), &HandleSetMontageSectionStartTime },
    { TEXT("set_montage_segment_anim_sequence"), &HandleSetMontageSegmentAnimSequence },
    { TEXT("set_montage_segment_start_time"), &HandleSetMontageSegmentStartTime },
    { TEXT("set_pose_asset_base_pose"), &HandleSetPoseAssetBasePose },
    { TEXT("collapse_montage_segments"), &HandleCollapseMontageSegments },
    { TEXT("sort_montage_segments"), &HandleSortMontageSegments },
};
} // namespace UEAgentAnimationAuthoringCommands

#undef LOCTEXT_NAMESPACE
