#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
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
// MCP:create_anim_montage - Create and save an empty-but-valid UAnimMontage asset under /Game
//   @param name - Asset name (e.g. 'AM_Example').
//   @param package_path - Target content package path where the asset is created (default /Game/Animation)
//   @param slot_name - Montage slot name (e.g. DefaultSlot)
//   @param skeleton_path - Asset path of the target Skeleton
//   @returns { name, package_path, asset_path, montage_path, skeleton_path, slot_names, slot_name, slot_count, section_count, created, already_existed, saved, message }
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

    // Idempotent: if it already exists, load + return a stable payload.
    if (UAnimMontage* Existing = AnimAssetUtils::FindExisting<UAnimMontage>(FullPath))
    {
        TSharedPtr<FJsonObject> Obj = BuildPayload(Existing, /*bCreated=*/false);
        Obj->SetBoolField(TEXT("already_existed"), true);
        Obj->SetBoolField(TEXT("saved"), true);
        Obj->SetStringField(TEXT("message"), TEXT("AnimMontage already exists; loaded existing asset"));
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
    // AnimSequence segment (AddSlot appends an FSlotAnimationTrack with the given name).
    if (!Montage->IsValidSlot(FName(*SlotName)))
    {
        Montage->AddSlot(FName(*SlotName));
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
// MCP:create_blend_space - Create and save a UBlendSpace (or UBlendSpace1D) asset under /Game
//   @param name - Asset name (e.g. 'BS_Example').
//   @param package_path - Target content package path where the asset is created (default /Game/Animation)
//   @param is_1d - If true create a 1D BlendSpace, else 2D (default false)
//   @param axis_name - X-axis display name (default 'Speed')
//   @param axis_min - X-axis minimum value (default 0)
//   @param axis_max - X-axis maximum value (default 100)
//   @param skeleton_path - Asset path of the target Skeleton
//   @returns { name, package_path, asset_path, blend_space_path, is_1d, skeleton_path, axis_name, axis_min, axis_max, axis_grid_num, sample_count, created, already_existed, saved, message }
TSharedPtr<FJsonObject> HandleCreateBlendSpace(const TSharedPtr<FJsonObject>& Params)
{
    FString Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name)) { return Missing; }

    const FString PackagePath = AnimAuthoring_ReadPackagePath(Params, TEXT("/Game/Animation"));

    bool bIs1D = false;
    Params->TryGetBoolField(TEXT("is_1d"), bIs1D);

    FString AxisName = TEXT("Speed");
    Params->TryGetStringField(TEXT("axis_name"), AxisName);
    if (AxisName.IsEmpty())
    {
        AxisName = TEXT("Speed");
    }

    double AxisMin = 0.0;
    double AxisMax = 100.0;
    Params->TryGetNumberField(TEXT("axis_min"), AxisMin);
    Params->TryGetNumberField(TEXT("axis_max"), AxisMax);

    USkeleton* Skeleton = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAuthoring_ReadSkeleton(Params, Skeleton)) { return Error; }

    const FString FullPath = FString::Printf(TEXT("%s/%s"), *PackagePath, *Name);

    auto BuildPayload = [&](UBlendSpace* BlendSpace, bool bCreated) -> TSharedPtr<FJsonObject>
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Name);
        Obj->SetStringField(TEXT("package_path"), PackagePath);
        Obj->SetStringField(TEXT("asset_path"), BlendSpace->GetPathName());
        Obj->SetStringField(TEXT("blend_space_path"), FullPath);
        Obj->SetBoolField(TEXT("is_1d"), BlendSpace->IsA<UBlendSpace1D>());
        const USkeleton* BSSkeleton = BlendSpace->GetSkeleton();
        Obj->SetStringField(TEXT("skeleton_path"), BSSkeleton ? BSSkeleton->GetPathName() : FString());

        // Read back the configured X axis via the public const accessor.
        const FBlendParameter& XAxis = BlendSpace->GetBlendParameter(0);
        Obj->SetStringField(TEXT("axis_name"), XAxis.DisplayName);
        Obj->SetNumberField(TEXT("axis_min"), XAxis.Min);
        Obj->SetNumberField(TEXT("axis_max"), XAxis.Max);
        Obj->SetNumberField(TEXT("axis_grid_num"), XAxis.GridNum);
        Obj->SetNumberField(TEXT("sample_count"), BlendSpace->GetBlendSamples().Num());
        Obj->SetBoolField(TEXT("created"), bCreated);
        return Obj;
    };

    // Idempotent: if it already exists, load + return a stable payload.
    if (UBlendSpace* Existing = AnimAssetUtils::FindExisting<UBlendSpace>(FullPath))
    {
        TSharedPtr<FJsonObject> Obj = BuildPayload(Existing, /*bCreated=*/false);
        Obj->SetBoolField(TEXT("already_existed"), true);
        Obj->SetBoolField(TEXT("saved"), true);
        Obj->SetStringField(TEXT("message"), TEXT("BlendSpace already exists; loaded existing asset"));
        return mcp_common::MakeOkReply(Obj);
    }

    UClass* BlendSpaceClass = bIs1D ? UBlendSpace1D::StaticClass() : UBlendSpace::StaticClass();
    // The constructor sets DimensionIndices ({0} for 1D, {0,1} for 2D) automatically.
    FString CreateError;
    UBlendSpace* BlendSpace = Cast<UBlendSpace>(mcp_common::NewAssetInPackage(BlendSpaceClass, FullPath, Name,
        TEXT("Failed to create UBlendSpace"), CreateError));
    if (!BlendSpace)
    {
        return mcp_common::MakeErrorReply(CreateError);
    }

    BlendSpace->SetSkeleton(Skeleton);

    // Configure the X axis (BlendParameters[0]). BlendParameters is a protected
    // fixed-size UPROPERTY array (ArrayDim==3); write it via the reflection system,
    // which is the supported external-write path. Read-back uses GetBlendParameter().
    if (FStructProperty* BlendParamsProp = FindFProperty<FStructProperty>(UBlendSpace::StaticClass(), TEXT("BlendParameters")))
    {
        // Element 0 == X axis.
        FBlendParameter* XAxis = BlendParamsProp->ContainerPtrToValuePtr<FBlendParameter>(BlendSpace, /*ArrayIndex=*/0);
        if (XAxis)
        {
            XAxis->DisplayName = AxisName;
            XAxis->Min = static_cast<float>(AxisMin);
            XAxis->Max = static_cast<float>(AxisMax);
            if (XAxis->GridNum < 1)
            {
                XAxis->GridNum = 4;
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
// MCP:add_anim_notify_name - Register a notify name in the AnimationNotifies list of a USkeleton (no duplicate is added; already_existed tells) inside an undo transaction and save the skeleton package; returns present, notify_count and saved. Without skeleton_path it edits /Engine/EngineMeshes/SkeletalCube_Skeleton.
//   @param notify_name - The notify name to register (e.g. 'Footstep').
//   @param skeleton_path - Asset path of the USkeleton to edit (/Game/...); when omitted or empty the engine asset /Engine/EngineMeshes/SkeletalCube_Skeleton is edited and saved.
//   @returns { skeleton_path, notify_name, present, added, already_existed, notify_count, saved, message }
TSharedPtr<FJsonObject> HandleAddAnimNotifyName(const TSharedPtr<FJsonObject>& Params)
{
    FString NotifyName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("notify_name"), NotifyName)) { return Missing; }

    USkeleton* Skeleton = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAuthoring_ReadSkeleton(Params, Skeleton)) { return Error; }

    const FName NotifyFName(*NotifyName);

    const FScopedTransaction Transaction(LOCTEXT("AddAnimNotifyName", "Add Anim Notify Name"));
    Skeleton->Modify();

    bool bAlreadyExisted = false;
    bool bPresent = false;
    bAlreadyExisted = Skeleton->AnimationNotifies.Contains(NotifyFName);
    // AddNewAnimationNotify uses AddUnique semantics internally.
    Skeleton->AddNewAnimationNotify(NotifyFName);
    bPresent = Skeleton->AnimationNotifies.Contains(NotifyFName);

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Skeleton, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("skeleton_path"), Skeleton->GetPathName());
    Obj->SetStringField(TEXT("notify_name"), NotifyName);
    Obj->SetBoolField(TEXT("present"), bPresent);
    Obj->SetBoolField(TEXT("added"), bPresent);
    Obj->SetBoolField(TEXT("already_existed"), bAlreadyExisted);
    Obj->SetNumberField(TEXT("notify_count"), Skeleton->AnimationNotifies.Num());
    Obj->SetStringField(TEXT("message"), bSaved
        ? TEXT("Anim notify registered on skeleton and saved to disk")
        : FString::Printf(TEXT("Anim notify registered in-memory; save not persisted (%s)"), *SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("add_anim_notify_name"));
}

// ---------------------------------------------------------------------------
// add_anim_curve_name
// ---------------------------------------------------------------------------
// MCP:add_anim_curve_name - Register a curve name as curve metadata on a USkeleton (USkeleton::AddCurveMetaData) in an undo transaction, then save the skeleton package. Idempotent: a name already registered is kept, reported as already_existed=true, and the skeleton is saved anyway. It adds no keys -- to put a keyed float curve on a UAnimSequence use add_anim_float_curve; remove_anim_curve_name is the inverse. An unknown skeleton is an error and a failed save makes success false.
//   @param curve_name - The curve name to add (e.g. 'Curve_Example').
//   @param skeleton_path - Asset path of the USkeleton to edit (/Game/...); when omitted or empty the engine asset /Engine/EngineMeshes/SkeletalCube_Skeleton is edited and saved.
//   @returns { skeleton_path, curve_name, present, added, already_existed, curve_count, saved, message }
TSharedPtr<FJsonObject> HandleAddAnimCurveName(const TSharedPtr<FJsonObject>& Params)
{
    FString CurveName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("curve_name"), CurveName)) { return Missing; }

    USkeleton* Skeleton = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAuthoring_ReadSkeleton(Params, Skeleton)) { return Error; }

    const FName CurveFName(*CurveName);

    const FScopedTransaction Transaction(LOCTEXT("AddAnimCurveName", "Add Anim Curve Name"));
    Skeleton->Modify();

    // USkeleton::AddCurveMetaData(Name, bTransact) returns true if a new entry was added,
    // false if one already existed. Either way the name is present afterwards.
    const bool bAddedNew = Skeleton->AddCurveMetaData(CurveFName, /*bTransact=*/false);

    // Verify present by enumerating curve metadata names.
    TArray<FName> Names;
    Skeleton->GetCurveMetaDataNames(Names);
    const bool bPresent = Names.Contains(CurveFName);

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Skeleton, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("skeleton_path"), Skeleton->GetPathName());
    Obj->SetStringField(TEXT("curve_name"), CurveName);
    Obj->SetBoolField(TEXT("present"), bPresent);
    Obj->SetBoolField(TEXT("added"), bPresent);
    Obj->SetBoolField(TEXT("already_existed"), !bAddedNew);
    Obj->SetNumberField(TEXT("curve_count"), Names.Num());
    Obj->SetStringField(TEXT("message"), bSaved
        ? TEXT("Anim curve metadata added to skeleton and saved to disk")
        : FString::Printf(TEXT("Anim curve metadata added in-memory; save not persisted (%s)"), *SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("add_anim_curve_name"));
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
// MCP:add_montage_section - Add a composite section to a UAnimMontage at an absolute start time, or move the existing section of that name there; it does not link sections (use link_montage_sections). Saves the montage in an undo transaction and returns start_time, section_count, sections and already_existed.
//   @param montage_path - Full asset path of the montage (e.g. '/Game/MyGame/AM_Example').
//   @param section_name - Name of the section to add (e.g. 'Loop').
//   @param start_time - Absolute section start time in seconds (default 0)
//   @returns { montage_path, section_name, start_time, section_count, sections, already_existed, added, saved, message }
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

    const FScopedTransaction Transaction(LOCTEXT("AddMontageSection", "Add Montage Section"));
    Montage->Modify();

    // Idempotent: reuse an existing section of the same name, otherwise append a new one.
    FCompositeSection* Target = AnimAuthoring_FindSection(Montage, SectionFName);
    const bool bAlreadyExisted = Target != nullptr;
    if (!Target)
    {
        FCompositeSection NewSection;
        NewSection.SectionName = SectionFName;
        const int32 NewIndex = Montage->CompositeSections.Add(NewSection);
        Target = &Montage->CompositeSections[NewIndex];
    }
    // SetTime stores the absolute time into LinkValue (Absolute reference frame).
    Target->SetTime(static_cast<float>(StartTime));

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Montage, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("montage_path"), Montage->GetPathName());
    Obj->SetStringField(TEXT("section_name"), SectionName);
    Obj->SetNumberField(TEXT("start_time"), Target->GetTime());
    Obj->SetNumberField(TEXT("section_count"), Montage->CompositeSections.Num());
    Obj->SetArrayField(TEXT("sections"), CollectSectionNames(Montage));
    Obj->SetBoolField(TEXT("already_existed"), bAlreadyExisted);
    Obj->SetBoolField(TEXT("added"), true);
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Montage section added"), bSaved, SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("add_montage_section"));
}

// ---------------------------------------------------------------------------
// add_montage_slot
// ---------------------------------------------------------------------------
// MCP:add_montage_slot - Add an empty slot animation track with the given slot name to a UAnimMontage and save it; an existing slot of that name is left as is (already_existed=true). Returns slot_count, the slots list and present. Fill it with add_montage_segment; inverse: remove_montage_slot.
//   @param montage_path - Full asset path of the montage (e.g. '/Game/MyGame/AM_Example').
//   @param slot_name - Name of the slot track to add (e.g. 'UpperBody').
//   @returns { montage_path, slot_name, slot_count, slots, present, already_existed, added, saved, message }
TSharedPtr<FJsonObject> HandleAddMontageSlot(const TSharedPtr<FJsonObject>& Params)
{
    FString MontagePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("montage_path"), MontagePath)) { return Missing; }

    FString SlotName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("slot_name"), SlotName)) { return Missing; }

    UAnimMontage* Montage = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(MontagePath, TEXT("AnimMontage"), Montage)) { return Error; }

    const FName SlotFName(*SlotName);

    const FScopedTransaction Transaction(LOCTEXT("AddMontageSlot", "Add Montage Slot"));
    Montage->Modify();

    // UAnimMontage::IsValidSlot returns true if a slot track with this name exists.
    const bool bAlreadyExisted = Montage->IsValidSlot(SlotFName);
    if (!bAlreadyExisted)
    {
        // AddSlot appends an FSlotAnimationTrack with the given SlotName.
        Montage->AddSlot(SlotFName);
    }
    const bool bPresent = Montage->IsValidSlot(SlotFName);

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Montage, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("montage_path"), Montage->GetPathName());
    Obj->SetStringField(TEXT("slot_name"), SlotName);
    Obj->SetNumberField(TEXT("slot_count"), Montage->SlotAnimTracks.Num());
    Obj->SetArrayField(TEXT("slots"), CollectSlotNames(Montage));
    Obj->SetBoolField(TEXT("present"), bPresent);
    Obj->SetBoolField(TEXT("already_existed"), bAlreadyExisted);
    Obj->SetBoolField(TEXT("added"), bPresent);
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Montage slot track added"), bSaved, SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("add_montage_slot"));
}

// ---------------------------------------------------------------------------
// add_montage_notify
// ---------------------------------------------------------------------------
// MCP:add_montage_notify - Append a name-only notify event (no UAnimNotify class) to a UAnimMontage at an absolute trigger time on notify track 0 and save the montage; every call appends another event. Returns trigger_time, notify_count and the notifies name list. Remove with remove_montage_notify.
//   @param montage_path - Full asset path of the montage (e.g. '/Game/MyGame/AM_Example').
//   @param notify_name - Name of the notify event (e.g. 'Footstep').
//   @param trigger_time - Absolute notify trigger time in seconds (default 0)
//   @returns { montage_path, notify_name, trigger_time, notify_count, notifies, added, saved, message }
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
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("add_montage_notify"));
}

// ---------------------------------------------------------------------------
// set_montage_blend_times
// ---------------------------------------------------------------------------
// MCP:set_montage_blend_times - Set the blend-in / blend-out times on a UAnimMontage
//   @param montage_path - Full asset path of the montage (e.g. '/Game/MyGame/AM_Example').
//   @param blend_in - Blend-in time in seconds (defaults to current value)
//   @param blend_out - Blend-out time in seconds (defaults to current value)
//   @returns { montage_path, blend_in, blend_out, applied, saved, message }
TSharedPtr<FJsonObject> HandleSetMontageBlendTimes(const TSharedPtr<FJsonObject>& Params)
{
    FString MontagePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("montage_path"), MontagePath)) { return Missing; }

    UAnimMontage* Montage = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(MontagePath, TEXT("AnimMontage"), Montage)) { return Error; }

    // Default to the montage's current blend times if a value is omitted.
    double BlendIn = Montage->BlendIn.GetBlendTime();
    double BlendOut = Montage->BlendOut.GetBlendTime();
    const bool bHasBlendIn = Params->TryGetNumberField(TEXT("blend_in"), BlendIn);
    const bool bHasBlendOut = Params->TryGetNumberField(TEXT("blend_out"), BlendOut);

    const FScopedTransaction Transaction(LOCTEXT("SetMontageBlendTimes", "Set Montage Blend Times"));
    Montage->Modify();

    if (bHasBlendIn)
    {
        Montage->BlendIn.SetBlendTime(static_cast<float>(BlendIn));
    }
    if (bHasBlendOut)
    {
        Montage->BlendOut.SetBlendTime(static_cast<float>(BlendOut));
    }

    // Read back the values that actually landed on the FAlphaBlend members.
    const float AppliedIn = Montage->BlendIn.GetBlendTime();
    const float AppliedOut = Montage->BlendOut.GetBlendTime();

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Montage, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("montage_path"), Montage->GetPathName());
    Obj->SetNumberField(TEXT("blend_in"), AppliedIn);
    Obj->SetNumberField(TEXT("blend_out"), AppliedOut);
    Obj->SetBoolField(TEXT("applied"), true);
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Montage blend times set"), bSaved, SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("set_montage_blend_times"));
}

// ---------------------------------------------------------------------------
// get_montage_info
// ---------------------------------------------------------------------------
// MCP:get_montage_info - Read a UAnimMontage without changing it: skeleton_path, section, slot and notify counts with their name lists, blend_in/blend_out times, blend modes, enable_auto_blend_out, blend_out_trigger_time, sync_group and sync_slot_index. Change them with set_montage_blend_times or set_montage_properties.
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
    // that set_montage_properties writes (set_montage_blend_times only owns the TIMES).
    Obj->SetStringField(TEXT("blend_mode_in"), AnimAuthoring_BlendModeName(Montage->BlendModeIn));
    Obj->SetStringField(TEXT("blend_mode_out"), AnimAuthoring_BlendModeName(Montage->BlendModeOut));
    Obj->SetBoolField(TEXT("enable_auto_blend_out"), Montage->bEnableAutoBlendOut);
    Obj->SetNumberField(TEXT("blend_out_trigger_time"), Montage->BlendOutTriggerTime);
    Obj->SetStringField(TEXT("sync_group"), Montage->SyncGroup.ToString());
    Obj->SetNumberField(TEXT("sync_slot_index"), Montage->SyncSlotIndex);
    return mcp_common::MakeOkReply(Obj);
}

// ---------------------------------------------------------------------------
// delete_montage_section
// Remove a CompositeSection from UAnimMontage::CompositeSections by name.
// Canonical path: UAnimMontage exposes GetSectionIndex(FName) -> int32 (AnimMontage.h)
// and a public TArray<FCompositeSection> CompositeSections (AnimMontage.h). There is
// no AddSection/RemoveSection method — surgical TArray op is the canonical pattern.
// Idempotent: a missing section returns success with already_existed=false and the
// current sections list (no fake removed=true). The caller can branch on the boolean.
// ---------------------------------------------------------------------------
// MCP:delete_montage_section - Delete the first composite section with that name (FName match, case ignored) from a UAnimMontage and save it; a missing name is a success with removed=false and nothing saved. Links from other sections (NextSectionName) are not updated. Returns removed_index, section_count and the remaining sections.
//   @param montage_path - Full asset path of the montage (e.g. '/Game/MyGame/AM_Example').
//   @param section_name - Name of the FCompositeSection to remove; compared as an FName, so case is ignored.
//   @returns { montage_path, section_name, removed_index?, already_existed, removed, section_count, sections[], saved, message }
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
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("delete_montage_section"));
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
// MCP:remove_montage_notify - Remove the notify event at a zero-based index from the Notifies array of a UAnimMontage and save it; an index past the end changes nothing and still returns success with removed=false. Returns removed_notify_name, notify_count_before, notify_count and the remaining notify names.
//   @param montage_path - Full asset path of the montage (e.g. '/Game/MyGame/AM_Example').
//   @param notify_index - Zero-based index into the montage's Notifies array. Use get_montage_info first to list notifies.
//   @returns { montage_path, notify_index, removed_notify_name?, notify_count_before, notify_count, notifies[], removed, saved, message }
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
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("remove_montage_notify"));
}

// ---------------------------------------------------------------------------
// remove_montage_slot
// Remove an FSlotAnimationTrack from UAnimMontage::SlotAnimTracks. Name-keyed
// inverse of add_montage_slot (which appends via UAnimMontage::AddSlot).
// Canonical path: SlotAnimTracks is a public TArray<FSlotAnimationTrack>
// UPROPERTY (AnimMontage.h); FSlotAnimationTrack has an FName SlotName
// (AnimMontage.h). There is no RemoveSlot method — surgical TArray::RemoveAt
// after a SlotName lookup is the canonical pattern (same idiom as
// delete_montage_section's CompositeSections.RemoveAt). Resolution is by
// slot_name (primary; mirrors add_montage_slot) or by slot_index alternative.
// Idempotent: a missing slot returns success with already_existed=false and the
// current slots list (no fake removed=true), mirroring delete_montage_section.
// ---------------------------------------------------------------------------
// MCP:remove_montage_slot - Remove a slot animation track from a UAnimMontage by name or index
//   @param montage_path - Full asset path of the montage (e.g. '/Game/MyGame/AM_Example').
//   @param slot_name - Name of the FSlotAnimationTrack to remove (FName). Either this or slot_index is required.
//   @param slot_index - Zero-based index into SlotAnimTracks (alternative to slot_name). Use get_montage_info first to list slots.
//   @returns { montage_path, slot_name?, slot_index?, removed_index?, removed_slot_name?, slot_count, slots[], already_existed, removed, saved, message }
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
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("remove_montage_slot"));
}

// ===========================================================================
// Composite / pose asset / additive settings

// ---------------------------------------------------------------------------
// create_anim_composite
// ---------------------------------------------------------------------------
// MCP:create_anim_composite - Create and save a UAnimComposite asset bound to a USkeleton, under /Game
//   @param name - Asset name (e.g. 'AnimComp_Example').
//   @param package_path - Target content package path where the asset is created (default /Game/Animation)
//   @param skeleton_path - Asset path of the target Skeleton
//   @returns { name, package_path, asset_path, composite_path, skeleton, num_segments, created, already_existed, saved, message }
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

    // Idempotent: if it already exists, load + return a stable payload.
    if (UAnimComposite* Existing = AnimAssetUtils::FindExisting<UAnimComposite>(FullPath))
    {
        TSharedPtr<FJsonObject> Obj = BuildPayload(Existing, /*bCreated=*/false);
        Obj->SetBoolField(TEXT("already_existed"), true);
        Obj->SetBoolField(TEXT("saved"), true);
        Obj->SetStringField(TEXT("message"), TEXT("AnimComposite already exists; loaded existing asset"));
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
// MCP:create_pose_asset - Create an empty UPoseAsset (no poses) at package_path/name, bound to the USkeleton at skeleton_path (default the engine SkeletalCube_Skeleton), and save it; no undo transaction. Idempotent: a UPoseAsset already at that path is returned untouched with already_existed=true, even when bound to another skeleton. A name taken by another asset class or an unknown skeleton is an error, and a failed save makes success false. Add poses with add_pose_to_pose_asset; to build one from animation frames use create_pose_asset_from_anim_sequence.
//   @param name - Asset name (e.g. 'Pose_Example').
//   @param package_path - Target content package path where the asset is created (default /Game/Animation)
//   @param skeleton_path - Asset path of the target Skeleton
//   @returns { name, package_path, asset_path, pose_asset_path, skeleton, num_poses, created, already_existed, saved, message }
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

    // Idempotent: if it already exists, load + return a stable payload.
    if (UPoseAsset* Existing = AnimAssetUtils::FindExisting<UPoseAsset>(FullPath))
    {
        TSharedPtr<FJsonObject> Obj = BuildPayload(Existing, /*bCreated=*/false);
        Obj->SetBoolField(TEXT("already_existed"), true);
        Obj->SetBoolField(TEXT("saved"), true);
        Obj->SetStringField(TEXT("message"), TEXT("PoseAsset already exists; loaded existing asset"));
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

// ---------------------------------------------------------------------------
// add_pose_to_pose_asset
// ---------------------------------------------------------------------------
// MCP:add_pose_to_pose_asset - Add a pose copied from the skeleton reference pose to a UPoseAsset under the given name (a pose of that name is replaced) and save it; refused when the asset has no skeleton. Returns num_poses, num_tracks and already_existed. For poses from animation frames use create_pose_asset_from_anim_sequence.
//   @param pose_asset_path - Path to the UPoseAsset (e.g. '/Game/MyGame/Pose_Example').
//   @param pose_name - Name of the pose to add (e.g. 'RefPose_A').
//   @returns { pose_asset_path, pose_name, num_poses, num_tracks, present, added, already_existed, saved, message }
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

    const FScopedTransaction Transaction(LOCTEXT("AddPoseToPoseAsset", "Add Pose To Pose Asset"));
    PoseAsset->Modify();

    const bool bAlreadyExisted = PoseAsset->ContainsPose(PoseFName);

    // AddReferencePose builds tracks + a pose purely from the skeleton's reference pose
    // (FReferenceSkeleton::GetRefBonePose) — no source UAnimSequence is required, so this
    // is a fully headless-safe REAL effect. If the named pose already exists it is updated.
    PoseAsset->AddReferencePose(PoseFName, Skeleton->GetReferenceSkeleton());

    const bool bPresent = PoseAsset->ContainsPose(PoseFName);
    const int32 NumPoses = PoseAsset->GetNumPoses();

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(PoseAsset, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("pose_asset_path"), PoseAsset->GetPathName());
    Obj->SetStringField(TEXT("pose_name"), PoseName);
    Obj->SetNumberField(TEXT("num_poses"), NumPoses);
    Obj->SetNumberField(TEXT("num_tracks"), PoseAsset->GetNumTracks());
    Obj->SetBoolField(TEXT("present"), bPresent);
    Obj->SetBoolField(TEXT("added"), bPresent);
    Obj->SetBoolField(TEXT("already_existed"), bAlreadyExisted);
    Obj->SetStringField(TEXT("message"), bSaved
        ? TEXT("Reference pose added to PoseAsset (from skeleton ref pose) and saved to disk")
        : FString::Printf(TEXT("Reference pose added to PoseAsset in-memory; save not persisted (%s)"), *SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("add_pose_to_pose_asset"));
}

// ---------------------------------------------------------------------------
// set_anim_montage_rate_scale
// ---------------------------------------------------------------------------
// MCP:set_anim_montage_rate_scale - Set the global RateScale (and optionally BlendOutTriggerTime) on a UAnimMontage
//   @param montage_path - Path to the UAnimMontage (e.g. '/Game/MyGame/AM_Example').
//   @param name - Alias of montage_path
//   @param rate_scale - Global play rate scale (defaults to current value)
//   @param blend_out_trigger_time - Blend-out trigger time in seconds (defaults to current value)
//   @returns { montage_path, rate_scale, blend_out_trigger_time, applied, saved, message }
TSharedPtr<FJsonObject> HandleSetAnimMontageRateScale(const TSharedPtr<FJsonObject>& Params)
{
    // Accept montage_path, or its alias 'name', for the asset path.
    FString MontagePath;
    if (!Params->TryGetStringField(TEXT("montage_path"), MontagePath) || MontagePath.IsEmpty())
    {
        Params->TryGetStringField(TEXT("name"), MontagePath);
    }
    if (MontagePath.IsEmpty())
    {
        return mcp_common::MissingParameterReply(TEXT("montage_path"));
    }

    UAnimMontage* Montage = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(MontagePath, TEXT("AnimMontage"), Montage)) { return Error; }

    // RateScale lives on UAnimSequenceBase (public float). Default to the current value.
    double RateScale = Montage->RateScale;
    const bool bHasRate = Params->TryGetNumberField(TEXT("rate_scale"), RateScale);

    double BlendOutTrigger = Montage->BlendOutTriggerTime;
    const bool bHasTrigger = Params->TryGetNumberField(TEXT("blend_out_trigger_time"), BlendOutTrigger);

    const FScopedTransaction Transaction(LOCTEXT("SetAnimMontageRateScale", "Set Anim Montage Rate Scale"));
    Montage->Modify();

    if (bHasRate)
    {
        Montage->RateScale = static_cast<float>(RateScale);
    }
    if (bHasTrigger)
    {
        Montage->BlendOutTriggerTime = static_cast<float>(BlendOutTrigger);
    }

    // Read back the values that actually landed on the asset.
    const float AppliedRate = Montage->RateScale;
    const float AppliedTrigger = Montage->BlendOutTriggerTime;

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Montage, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("montage_path"), Montage->GetPathName());
    Obj->SetNumberField(TEXT("rate_scale"), AppliedRate);
    Obj->SetNumberField(TEXT("blend_out_trigger_time"), AppliedTrigger);
    Obj->SetBoolField(TEXT("applied"), true);
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Montage rate scale set"), bSaved, SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("set_anim_montage_rate_scale"));
}

// ---------------------------------------------------------------------------
// get_anim_asset_info
// ---------------------------------------------------------------------------
// MCP:get_anim_asset_info - Load any animation asset and report its class, skeleton and asset_type (montage, composite, pose, sequence, blendspace or animation_asset) plus the counts of that type: sections/slots/notifies, segments, poses/tracks/curves, length, samples. Read-only; get_montage_info, get_anim_sequence_info and get_blendspace_samples give detail.
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

// ---------------------------------------------------------------------------
// add_blendspace_sample
// ---------------------------------------------------------------------------
// MCP:add_blendspace_sample - Add a blend sample (animation + blend position) to a BlendSpace
//   @param blendspace_path - Full asset path to the UBlendSpace (e.g.
//   @param animation_path - Full asset path to the UAnimSequence to use as sample.
//   @param blend_value - Blend position: single number (1D) or array of up to 3 numbers (default 0)
//   @returns { blendspace_path, sample_index, num_samples, success, saved, message }
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

    const FScopedTransaction Transaction(LOCTEXT("AddBlendspaceSample", "Add Blendspace Sample"));
    BlendSpace->Modify();

    const int32 SampleIndex = BlendSpace->AddSample(AnimSeq, BlendValue);
    const int32 NumSamples = BlendSpace->GetBlendSamples().Num();

    // Without this the RUNTIME does not see the blend space.
    RebuildBlendSpaceRuntimeData(BlendSpace);
    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(BlendSpace, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("blendspace_path"), BlendSpace->GetPathName());
    Obj->SetNumberField(TEXT("sample_index"), SampleIndex);
    Obj->SetNumberField(TEXT("num_samples"), NumSamples);
    Obj->SetBoolField(TEXT("success"), SampleIndex != INDEX_NONE);
    Obj->SetBoolField(TEXT("saved"), bSaved);
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Sample added to blendspace"), bSaved, SaveError));
    // Success is BOTH things: the operation succeeded AND it persisted.
    return mcp_common::MakeOutcomeReply(Obj, (SampleIndex != INDEX_NONE) && bSaved,
        ((SampleIndex != INDEX_NONE) && bSaved) ? FString() : mcp_common::SavedOutcomeError(TEXT("add_blendspace_sample")));
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
// remove_blendspace_sample
// ---------------------------------------------------------------------------
// remove_blendspace_sample and edit_blendspace_sample validate the index with IsValidBlendSampleIndex, NEVER
// with GetBlendSample, which checkf-asserts on an invalid index.

// MCP:remove_blendspace_sample - Remove a blend sample (by index) from a BlendSpace. WARNING: the remaining indices REORDER -- the engine removes with a swap, so the LAST sample lands in the deleted slot. Never remove a batch by descending index and assume the rest held still: the full post-op sample list is returned so the caller can re-sync before the next call.
//   @param blendspace_path - Full asset path to the UBlendSpace.
//   @param index - Index of the sample to remove.
//   @returns { blendspace_path, removed, num_samples, samples, saved, message }
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
    UE_LOG(LogTemp, Verbose, TEXT("remove_blendspace_sample: DeleteSample(%d) on %s -> %s"),
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
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("remove_blendspace_sample"));
}

// ---------------------------------------------------------------------------
// edit_blendspace_sample
// ---------------------------------------------------------------------------
// MCP:edit_blendspace_sample - Edit a blend sample (by index): blend value and/or animation. At least one of blend_value / animation_path is required; both are individually optional so you can retime a sample without touching its animation, or swap the animation without moving it.
//   @param blendspace_path - Full asset path to the UBlendSpace.
//   @param index - Index of the sample to edit.
//   @param blend_value - New blend position: single number (1D) or array of up to 3 numbers (optional).
//   @param animation_path - Full asset path to a UAnimSequence to replace the sample's animation (optional).
//   @returns { blendspace_path, index, value_edited, animation_replaced, num_samples, samples, saved, message }
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

    const FScopedTransaction Transaction(LOCTEXT("EditBlendspaceSample", "Edit Blendspace Sample"));
    BlendSpace->Modify();

    bool bValueEdited = false;
    if (bHasBlendValue)
    {
        // EditSampleValue returns false (no mutation) on invalid index or an
        // out-of-bounds / too-close value — propagate as an honest failure, not a crash.
        bValueEdited = BlendSpace->EditSampleValue(Index, BlendValue);
        UE_LOG(LogTemp, Verbose, TEXT("edit_blendspace_sample: EditSampleValue(%d, %s) -> %s"),
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
        UE_LOG(LogTemp, Verbose, TEXT("edit_blendspace_sample: ReplaceSampleAnimation(%d, %s) -> %s"),
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
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("edit_blendspace_sample"));
}

// ---------------------------------------------------------------------------
// get_blendspace_samples
// ---------------------------------------------------------------------------
// MCP:get_blendspace_samples - Read every sample of a UBlendSpace or UBlendSpace1D in index order (animation path, blend_x/blend_y/blend_z, rate_scale) plus num_samples and the per-axis input smoothing under interpolation. Read-only; change samples with add_blendspace_sample, edit_blendspace_sample or remove_blendspace_sample.
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
// MCP:add_anim_notify - Add an instance of a UAnimNotify class at a time on a UAnimSequence notify track (created when missing) and save the sequence; each call adds another event, and an unknown class is an error. Returns time and success. For montages use add_montage_notify; for durations add_anim_notify_state.
//   @param animation_path - Full asset path to the UAnimSequence.
//   @param notify_name - Class name of a loaded UAnimNotify subclass, tried as given and then with the AnimNotify_ prefix (e.g. 'AN_Footstep', or "PlaySound" for AnimNotify_PlaySound); a Blueprint notify goes by its generated name ending in _C.
//   @param time - Notify trigger time in seconds (default 0)
//   @param track_name - Notify track name (default 'Default')
//   @returns { animation_path, notify_name, time, success, saved, message }
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
    Obj->SetBoolField(TEXT("saved"), bSaved);
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
// MCP:remove_anim_notify - Remove all UAnimNotify events matching a name from an AnimSequence
//   @param animation_path - Full asset path to the UAnimSequence.
//   @param notify_name - Name of the notify to remove (matches FAnimNotifyEvent.NotifyName).
//   @returns { animation_path, notify_name, removed, removed_count, already_absent }
TSharedPtr<FJsonObject> HandleRemoveAnimNotify(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimationPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("animation_path"), AnimationPath)) { return Missing; }

    FString NotifyName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("notify_name"), NotifyName)) { return Missing; }

    UAnimSequence* AnimSeq = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(AnimationPath, TEXT("AnimSequence"), AnimSeq)) { return Error; }

    const FScopedTransaction Transaction(LOCTEXT("RemoveAnimNotify", "Remove Anim Notify"));
    AnimSeq->Modify();

    // By-name removal: the library call also fires RefreshCacheGuid / notify-sort.
    const int32 Removed = UAnimationBlueprintLibrary::RemoveAnimationNotifyEventsByName(
        AnimSeq, FName(*NotifyName));
    const bool bRemoved = Removed > 0;

    // Honest headless: only persist when we actually mutated the asset.
    FString SaveError;
    bool bSaved = false;
    if (bRemoved)
    {
        AnimSeq->MarkPackageDirty();
        bSaved = AnimAuthoring_SavePackageOf(AnimSeq, SaveError);
    }

    UE_LOG(LogTemp, Verbose,
        TEXT("[remove_anim_notify] %s notify '%s' -> removed_count=%d (already_absent=%d, saved=%d)"),
        *AnimSeq->GetPathName(), *NotifyName, Removed, bRemoved ? 0 : 1, bSaved ? 1 : 0);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("animation_path"), AnimSeq->GetPathName());
    Obj->SetStringField(TEXT("notify_name"), NotifyName);
    Obj->SetBoolField(TEXT("removed"), bRemoved);
    Obj->SetNumberField(TEXT("removed_count"), Removed);
    Obj->SetBoolField(TEXT("already_absent"), !bRemoved);
    return mcp_common::MakeOkReply(Obj);
}

// ---------------------------------------------------------------------------
// add_anim_notify_state
// ---------------------------------------------------------------------------
// MCP:add_anim_notify_state - Add an instance of a UAnimNotifyState class with a start time and duration to a UAnimSequence notify track (created when missing) and save the sequence; each call adds another event, and unknown classes are refused. Returns start_time, duration and success. For instant notifies use add_anim_notify.
//   @param animation_path - Full asset path to the UAnimSequence.
//   @param notify_state_name - Class name of a loaded UAnimNotifyState subclass, tried as given and then with the AnimNotifyState_ prefix (e.g. "Trail" for AnimNotifyState_Trail); a Blueprint notify state goes by its generated name ending in _C.
//   @param start_time - Notify state start time in seconds (default 0)
//   @param duration - Notify state duration in seconds (default 1.0)
//   @param track_name - Notify track name (default 'Default')
//   @returns { animation_path, notify_state_name, start_time, duration, success, saved, message }
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
    Obj->SetBoolField(TEXT("saved"), bSaved);
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
// MCP:get_anim_notifies - List the notify events of a UAnimSequence without changing it: name, trigger time, duration, track_index and the notify_class or notify_state_class of each, plus num_notifies. Use get_notify_track_names for track names and get_montage_info for the notifies of a montage.
//   @param animation_path - Full asset path to the UAnimSequence.
//   @returns { name, time, duration, track_index, notify_class, notify_state_class, animation_path, num_notifies, notifies }
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
}

// ---------------------------------------------------------------------------
// create_anim_sequence
// ---------------------------------------------------------------------------
// MCP:create_anim_sequence - Create a UAnimSequence at package_path/anim_name bound to the USkeleton at skeleton_path (default the engine SkeletalCube_Skeleton) and save it; its data model gets frame_rate rounded to whole fps and round(length_seconds * fps) frames, but no bone tracks or curves. Idempotent: a UAnimSequence already at that path is returned unchanged (already_existed=true, length and rate not applied); another asset class there or an unknown skeleton is an error. A failed save makes success false, with saved=false and save_error, and the new asset stays in memory only. Fill it with add_anim_bone_track or add_anim_float_curve; inspect it with get_anim_sequence_info.
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

    // Idempotent: if it already exists, load + return a stable payload.
    if (UAnimSequence* Existing = AnimAssetUtils::FindExisting<UAnimSequence>(FullPath))
    {
        TSharedPtr<FJsonObject> Obj = BuildPayload(Existing, /*bSaved=*/true);
        Obj->SetBoolField(TEXT("already_existed"), true);
        Obj->SetStringField(TEXT("message"), TEXT("AnimSequence already exists; loaded existing asset"));
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
// add_anim_bone_track
// ---------------------------------------------------------------------------
// MCP:add_anim_bone_track - Add the animation track of one skeleton bone to a UAnimSequence through its data controller, filled with num_keys identity keys (zero translation, identity rotation, unit scale), as one undo step, then save the package. An existing track stays but its keys are replaced (idempotent). A bone_name the sequence's skeleton does not have is an error and nothing is written; an omitted bone_name targets the root bone. A missing sequence or skeleton is an error and a failed save makes success false. Real poses: set_anim_bone_track_keys; inverse: remove_anim_bone_track.
//   @param anim_sequence_path - Asset path of the UAnimSequence.
//   @param bone_name - Name of a bone of the skeleton of the sequence; omitted or empty means the root bone.
//   @param num_keys - Number of identity keys to write, rounded, minimum 1 (default 2); not checked against the frame count of the sequence.
//   @returns { anim_sequence_path, bone_name, num_bone_tracks, num_keys, success, saved, save_error, message }
TSharedPtr<FJsonObject> HandleAddAnimBoneTrack(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("anim_sequence_path"), AnimPath)) { return Missing; }

    FString RequestedBone;
    Params->TryGetStringField(TEXT("bone_name"), RequestedBone);

    int32 NumKeys = 2;
    {
        double NumKeysD = 2.0;
        if (Params->TryGetNumberField(TEXT("num_keys"), NumKeysD))
        {
            NumKeys = FMath::Max(1, FMath::RoundToInt(NumKeysD));
        }
    }

    UAnimSequence* Seq = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(AnimPath, TEXT("AnimSequence"), Seq)) { return Error; }

    USkeleton* Skeleton = Seq->GetSkeleton();
    if (!Skeleton)
    {
        return mcp_common::MakeErrorReply(TEXT("AnimSequence has no skeleton"));
    }

    // A named bone must exist on the skeleton: PickBoneName falls back to the root bone, and
    // writing identity keys to the root track for a misspelled name would wipe the root motion.
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

    int32 NumBoneTracks = 0;
    bool bSaved = false;
    FString SaveError;

    IAnimationDataController& Controller = Seq->GetController();
    Controller.OpenBracket(NSLOCTEXT("MCP", "AddBoneTrack", "Add Bone Track"));

    // AddBoneCurve adds a bone track (no-op if it already exists).
    Controller.AddBoneCurve(BoneFName);

    // Three identity key arrays of equal, non-zero length.
    TArray<FVector3f> PosKeys;
    TArray<FQuat4f> RotKeys;
    TArray<FVector3f> ScaleKeys;
    PosKeys.Init(FVector3f::ZeroVector, NumKeys);
    RotKeys.Init(FQuat4f::Identity, NumKeys);
    ScaleKeys.Init(FVector3f(1.0f, 1.0f, 1.0f), NumKeys);
    Controller.SetBoneTrackKeys(BoneFName, PosKeys, RotKeys, ScaleKeys, true);

    Controller.CloseBracket();

    if (const IAnimationDataModel* Model = Seq->GetDataModel())
    {
        NumBoneTracks = Model->GetNumBoneTracks();
    }

    bSaved = AnimAuthoring_SavePackageOf(Seq, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("anim_sequence_path"), Seq->GetPathName());
    Obj->SetStringField(TEXT("bone_name"), BoneFName.ToString());
    Obj->SetNumberField(TEXT("num_bone_tracks"), NumBoneTracks);
    Obj->SetNumberField(TEXT("num_keys"), NumKeys);
    Obj->SetBoolField(TEXT("success"), bSaved);
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Bone track added"), bSaved, SaveError, TEXT("")));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("add_anim_bone_track"), SaveError);
}

// ---------------------------------------------------------------------------
// set_anim_bone_track_keys — author REAL per-key bone transforms (translation /
// rotation / scale) on a UAnimSequence bone track, vs add_anim_bone_track which
// only writes identity. Same IAnimationDataController::SetBoneTrackKeys path,
// fed from caller-provided per-key transforms; read back from the data model.
// ---------------------------------------------------------------------------
// MCP:set_anim_bone_track_keys - Set per-key bone transforms (translation/rotation/scale) on a UAnimSequence bone track (real pose data, not identity).
//   @param anim_sequence_path - Asset path of the UAnimSequence.
//   @param bone_name - Skeleton bone whose track to author (resolved/validated against the skeleton; first bone if empty).
//   @param keys - Array of per-key transforms: { translation:[x,y,z], rotation:[pitch,yaw,roll] (deg), scale:[x,y,z] }. translation default [0,0,0], rotation default [0,0,0], scale default [1,1,1].
//   @returns { anim_sequence_path, bone_name, num_keys, num_keys_readback, num_bone_tracks, first_key:{translation,rotation_euler}, success, saved, message }
TSharedPtr<FJsonObject> HandleSetAnimBoneTrackKeys(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("anim_sequence_path"), AnimPath)) { return Missing; }
    FString RequestedBone;
    Params->TryGetStringField(TEXT("bone_name"), RequestedBone);

    const TArray<TSharedPtr<FJsonValue>>* KeysArr = nullptr;
    if (!Params->TryGetArrayField(TEXT("keys"), KeysArr) || !KeysArr || KeysArr->Num() == 0)
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required non-empty 'keys' array parameter"));
    }

    UAnimSequence* Seq = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(AnimPath, TEXT("AnimSequence"), Seq)) { return Error; }
    USkeleton* Skeleton = Seq->GetSkeleton();
    if (!Skeleton)
    {
        return mcp_common::MakeErrorReply(TEXT("AnimSequence has no skeleton"));
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
    for (const TSharedPtr<FJsonValue>& KV : *KeysArr)
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

    bSaved = AnimAuthoring_SavePackageOf(Seq, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("anim_sequence_path"), Seq->GetPathName());
    Obj->SetStringField(TEXT("bone_name"), BoneFName.ToString());
    Obj->SetNumberField(TEXT("num_keys"), NumKeys);
    Obj->SetNumberField(TEXT("num_keys_readback"), NumKeysReadback);
    Obj->SetNumberField(TEXT("num_bone_tracks"), NumBoneTracks);
    {
        TSharedPtr<FJsonObject> First = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> T = mcp_common::VectorToJsonArray(FirstPos);
        TArray<TSharedPtr<FJsonValue>> E = mcp_common::VectorToJsonArray(FirstEuler);
        First->SetArrayField(TEXT("translation"), T);
        First->SetArrayField(TEXT("rotation_euler"), E);
        Obj->SetObjectField(TEXT("first_key"), First);
    }
    Obj->SetBoolField(TEXT("success"), true);
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Bone track keys set"), bSaved, SaveError, TEXT("")));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("set_anim_bone_track_keys"));
}

// ---------------------------------------------------------------------------
// rename_pose_in_pose_asset — rename a named pose in a UPoseAsset, with a read
// back of the post-rename pose-name list.
// ---------------------------------------------------------------------------
// MCP:rename_pose_in_pose_asset - Rename an existing named pose in a UPoseAsset (UPoseAsset::ModifyPoseName).
//   @param pose_asset_path - Path to the UPoseAsset.
//   @param old_pose_name - Current name of the pose to rename.
//   @param new_pose_name - New name for the pose.
//   @returns { pose_asset_path, old_pose_name, new_pose_name, renamed, contains_new, contains_old, num_poses, pose_names, saved, message }
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

    const FScopedTransaction Transaction(NSLOCTEXT("MCP", "RenamePoseInPoseAsset", "Rename Pose In Pose Asset"));
    PoseAsset->Modify();

    const bool bRenamed = PoseAsset->ModifyPoseName(OldFName, NewFName);

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(PoseAsset, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("pose_asset_path"), PoseAsset->GetPathName());
    Obj->SetStringField(TEXT("old_pose_name"), OldName);
    Obj->SetStringField(TEXT("new_pose_name"), NewName);
    Obj->SetBoolField(TEXT("renamed"), bRenamed);
    Obj->SetBoolField(TEXT("contains_new"), PoseAsset->ContainsPose(NewFName));
    Obj->SetBoolField(TEXT("contains_old"), PoseAsset->ContainsPose(OldFName));
    Obj->SetNumberField(TEXT("num_poses"), PoseAsset->GetNumPoses());
    Obj->SetArrayField(TEXT("pose_names"), CollectPoseNames(PoseAsset));
    Obj->SetStringField(TEXT("message"), bRenamed
        ? TEXT("Pose renamed and saved to disk")
        : TEXT("ModifyPoseName returned false (new name may already exist)"));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("rename_pose_in_pose_asset"));
}

// ===========================================================================
// Pose CRUD (remove pose / base pose / remove curve)
//               + float-curve metadata (color / comment / scale)

// ---------------------------------------------------------------------------
// remove_pose_from_pose_asset
// ---------------------------------------------------------------------------
// MCP:remove_pose_from_pose_asset - Delete the named pose from a UPoseAsset (UPoseAsset::DeletePoses, which also fixes the base pose index) in an undo transaction and save the asset, even when the name is absent (then removed=false). Returns was_present, removed_count, contains_after, num_poses and the remaining pose_names.
//   @param pose_asset_path - Path to the UPoseAsset (e.g. '/Game/MyGame/Pose_Example').
//   @param pose_name - Name of the pose to remove.
//   @returns { pose_asset_path, pose_name, was_present, removed_count, removed, contains_after, num_poses, pose_names, saved, message }
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
    // clean no-op (removed=false, removed_count=0), never a fake success.
    const bool bWasPresent = PoseAsset->ContainsPose(PoseFName);

    const FScopedTransaction Transaction(NSLOCTEXT("MCP", "RemovePoseFromPoseAsset", "Remove Pose From Pose Asset"));
    PoseAsset->Modify();

    // DeletePoses fixes BasePoseIndex, calls PostProcessData and broadcasts OnPoseListChanged.
    // It does NOT open its own transaction, so the FScopedTransaction above owns undo.
    // The returned count is the honest number of poses actually removed.
    int32 RemovedCount = 0;
    if (bWasPresent)
    {
        RemovedCount = PoseAsset->DeletePoses({ PoseFName });
    }

    const bool bContainsAfter = PoseAsset->ContainsPose(PoseFName);

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(PoseAsset, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("pose_asset_path"), PoseAsset->GetPathName());
    Obj->SetStringField(TEXT("pose_name"), PoseName);
    Obj->SetBoolField(TEXT("was_present"), bWasPresent);
    Obj->SetNumberField(TEXT("removed_count"), RemovedCount);
    Obj->SetBoolField(TEXT("removed"), RemovedCount > 0);
    Obj->SetBoolField(TEXT("contains_after"), bContainsAfter);
    Obj->SetNumberField(TEXT("num_poses"), PoseAsset->GetNumPoses());
    Obj->SetArrayField(TEXT("pose_names"), CollectPoseNames(PoseAsset));
    Obj->SetStringField(TEXT("message"), bWasPresent
        ? AnimAuthoring_SaveMessage(TEXT("Pose removed from PoseAsset"), bSaved, SaveError)
        : TEXT("Pose not present; no-op (idempotent remove)"));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("remove_pose_from_pose_asset"));
}

// ---------------------------------------------------------------------------
// set_pose_asset_base_pose
// ---------------------------------------------------------------------------
// MCP:set_pose_asset_base_pose - Set which named pose of a UPoseAsset is its base pose (UPoseAsset::SetBasePoseName) in an undo transaction, then save the asset; an empty or None base_pose_name goes back to the skeleton reference pose. A name the asset does not contain is an error before anything changes; check applied, false when the engine refused the name, and a failed save makes success false. The base pose only drives evaluation of an additive pose asset, which additive and note report. Edit poses with add_pose_to_pose_asset, rename_pose_in_pose_asset and remove_pose_from_pose_asset.
//   @param pose_asset_path - Path to the UPoseAsset.
//   @param base_pose_name - Name of the pose to use as the base; empty/None resets to the reference pose.
//   @returns { pose_asset_path, base_pose_name, requested_name, applied, base_pose_index, is_reference_pose, additive, note, saved, message }
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

    const FScopedTransaction Transaction(NSLOCTEXT("MCP", "SetPoseAssetBasePose", "Set Pose Asset Base Pose"));
    PoseAsset->Modify();

    // EXPORT-MACRO GOTCHA: UPoseAsset::SetBasePoseName is a bare UFUNCTION(BlueprintCallable)
    // WITHOUT ENGINE_API on the method line, so a direct cross-module C++ call would LNK2019.
    // Invoke it through the reflection system (ProcessEvent) instead. The param frame mirrors
    // the UFUNCTION signature: input FName followed by the bool return value.
    bool bApplied = false;
    if (UFunction* Func = PoseAsset->FindFunction(TEXT("SetBasePoseName")))
    {
        struct FSetBasePoseNameParms { FName NewBasePoseName; bool ReturnValue; };
        FSetBasePoseNameParms Parms;
        Parms.NewBasePoseName = BasePoseFName;
        Parms.ReturnValue = false;
        PoseAsset->ProcessEvent(Func, &Parms);
        bApplied = Parms.ReturnValue;
    }
    else
    {
        return mcp_common::MakeErrorReply(
            TEXT("UPoseAsset::SetBasePoseName reflection function not found"));
    }

    // Read back via the inline-exported GetBasePoseIndex() + GetPoseNameByIndex() (both are
    // header-bodied / safe cross-module, unlike the un-exported GetBasePoseName()).
    const int32 BaseIndex = PoseAsset->GetBasePoseIndex();
    const FName ResolvedName = PoseAsset->GetPoseNameByIndex(BaseIndex);
    const bool bIsRefPose = (BaseIndex == INDEX_NONE);
    const bool bAdditive = PoseAsset->IsValidAdditive();

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(PoseAsset, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("pose_asset_path"), PoseAsset->GetPathName());
    Obj->SetStringField(TEXT("requested_name"), BasePoseName);
    Obj->SetStringField(TEXT("base_pose_name"), bIsRefPose ? FString() : ResolvedName.ToString());
    Obj->SetBoolField(TEXT("applied"), bApplied);
    Obj->SetNumberField(TEXT("base_pose_index"), BaseIndex);
    Obj->SetBoolField(TEXT("is_reference_pose"), bIsRefPose);
    Obj->SetBoolField(TEXT("additive"), bAdditive);
    // The base pose only drives evaluation when the PoseAsset is additive; it is settable
    // regardless, so we note this honestly rather than rejecting the call.
    Obj->SetStringField(TEXT("note"), bAdditive
        ? TEXT("Base pose set on an additive PoseAsset (drives additive evaluation)")
        : TEXT("Base pose recorded; only meaningful when the PoseAsset is additive (this one is not)"));
    Obj->SetStringField(TEXT("message"), bApplied
        ? AnimAuthoring_SaveMessage(TEXT("Base pose set"), bSaved, SaveError)
        : TEXT("SetBasePoseName returned false"));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("set_pose_asset_base_pose"));
}

// ---------------------------------------------------------------------------
// remove_pose_curve
// ---------------------------------------------------------------------------
// MCP:remove_pose_curve - Delete the named curve from a UPoseAsset (UPoseAsset::DeleteCurves) in an undo transaction and save the asset; an absent curve gives removed=false and removed_count 0 but the asset is saved anyway. Returns removed_count, removed and num_curves.
//   @param pose_asset_path - Path to the UPoseAsset.
//   @param curve_name - Name of the curve to remove.
//   @returns { pose_asset_path, curve_name, removed_count, removed, num_curves, saved, message }
TSharedPtr<FJsonObject> HandleRemovePoseCurve(const TSharedPtr<FJsonObject>& Params)
{
    FString PoseAssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("pose_asset_path"), PoseAssetPath)) { return Missing; }
    FString CurveName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("curve_name"), CurveName)) { return Missing; }

    UPoseAsset* PoseAsset = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(PoseAssetPath, TEXT("PoseAsset"), PoseAsset)) { return Error; }

    const FName CurveFName(*CurveName);

    const FScopedTransaction Transaction(NSLOCTEXT("MCP", "RemovePoseCurve", "Remove Pose Curve"));
    PoseAsset->Modify();

    // DeleteCurves loops PoseContainer.DeleteCurve + broadcasts OnPoseListChanged and returns
    // the honest count removed. No internal transaction -> FScopedTransaction above owns undo.
    // An absent curve yields count 0 (clean no-op), never a fake success.
    const int32 RemovedCount = PoseAsset->DeleteCurves({ CurveFName });

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(PoseAsset, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("pose_asset_path"), PoseAsset->GetPathName());
    Obj->SetStringField(TEXT("curve_name"), CurveName);
    Obj->SetNumberField(TEXT("removed_count"), RemovedCount);
    Obj->SetBoolField(TEXT("removed"), RemovedCount > 0);
    Obj->SetNumberField(TEXT("num_curves"), PoseAsset->GetCurveFNames().Num());
    Obj->SetStringField(TEXT("message"), RemovedCount > 0
        ? AnimAuthoring_SaveMessage(TEXT("Pose curve removed"), bSaved, SaveError)
        : TEXT("Curve not present; no-op (idempotent remove)"));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("remove_pose_curve"));
}

// ---------------------------------------------------------------------------
// set_anim_curve_color
// ---------------------------------------------------------------------------
// MCP:set_anim_curve_color - Set the editor display color of a FLOAT curve on a UAnimSequence (cosmetic: it tints the curve in the editor, it does not affect evaluation). FLOAT-CURVE ONLY: transform curves and any other non-float curve return applied=false instead of an error.
//   @param anim_sequence_path - Asset path of the UAnimSequence.
//   @param curve_name - Name of the float curve.
//   @param color - RGBA color as [r, g, b, a] in 0..1 (default [1,1,1,1]).
//   @returns { anim_sequence_path, curve_name, color, applied, saved, message }
//   NOTE (impl): wraps IAnimationDataController::SetCurveColor.
TSharedPtr<FJsonObject> HandleSetAnimCurveColor(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimPath, CurveName;
    if (TSharedPtr<FJsonObject> Error = AnimAuthoring_ReadCurveTarget(Params, AnimPath, CurveName))
    {
        return Error;
    }

    // Parse color=[r,g,b,a]; default opaque white.
    const TArray<TSharedPtr<FJsonValue>>* ColorArr = nullptr;
    Params->TryGetArrayField(TEXT("color"), ColorArr);
    const FLinearColor Color = mcp_common::LinearColorFromJsonArray(ColorArr, FLinearColor(1.0f, 1.0f, 1.0f, 1.0f));

    UAnimSequence* Seq = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(AnimPath, TEXT("AnimSequence"), Seq)) { return Error; }

    // FLOAT-CURVE ONLY: build the identifier as a float curve and require it to exist.
    const FAnimationCurveIdentifier CurveId(FName(*CurveName), ERawCurveTrackTypes::RCT_Float);
    IAnimationDataController& Controller = Seq->GetController();
    if (!Controller.GetModel() || !Controller.GetModel()->FindFloatCurve(CurveId))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Float curve '%s' does not exist; call add_anim_float_curve first"), *CurveName));
    }

    // SetCurveColor self-manages undo via bShouldTransact; do NOT add an outer FScopedTransaction.
    // It Report(Warning)+returns false for non-float curves — the returned bool is the honest `applied`.
    Controller.OpenBracket(NSLOCTEXT("MCP", "SetAnimCurveColor", "Set Anim Curve Color"));
    const bool bApplied = Controller.SetCurveColor(CurveId, Color, /*bShouldTransact=*/true);
    Controller.CloseBracket();

    FString SaveError;
    const bool bSaved = bApplied && AnimAuthoring_SavePackageOf(Seq, SaveError);

    TArray<TSharedPtr<FJsonValue>> ColorJson = mcp_common::ColorToJsonArray(Color);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("anim_sequence_path"), Seq->GetPathName());
    Obj->SetStringField(TEXT("curve_name"), CurveName);
    Obj->SetArrayField(TEXT("color"), ColorJson);
    Obj->SetBoolField(TEXT("applied"), bApplied);
    Obj->SetBoolField(TEXT("saved"), bSaved);
    Obj->SetStringField(TEXT("message"), bApplied
        ? AnimAuthoring_SaveMessage(TEXT("Curve color set"), bSaved, SaveError, TEXT(""))
        : TEXT("SetCurveColor returned false (only float curves support color)"));
    return mcp_common::MakeOkReply(Obj);
}

// ---------------------------------------------------------------------------
// set_anim_curve_comment
// ---------------------------------------------------------------------------
// MCP:set_anim_curve_comment - Set the author comment of a FLOAT curve on a UAnimSequence (metadata only; it does not affect evaluation). FLOAT-CURVE ONLY: transform curves and any other non-float curve return applied=false instead of an error.
//   @param anim_sequence_path - Asset path of the UAnimSequence.
//   @param curve_name - Name of the float curve.
//   @param comment - Comment string to set (default empty).
//   @returns { anim_sequence_path, curve_name, comment, applied, saved, message }
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
    if (!Controller.GetModel() || !Controller.GetModel()->FindFloatCurve(CurveId))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Float curve '%s' does not exist; call add_anim_float_curve first"), *CurveName));
    }

    // FLOAT-CURVE ONLY: SetCurveComment Report(Warning)+returns false for non-float curves.
    // Self-transacts via bShouldTransact; only bracketed, not wrapped in FScopedTransaction.
    Controller.OpenBracket(NSLOCTEXT("MCP", "SetAnimCurveComment", "Set Anim Curve Comment"));
    const bool bApplied = Controller.SetCurveComment(CurveId, Comment, /*bShouldTransact=*/true);
    Controller.CloseBracket();

    FString SaveError;
    const bool bSaved = bApplied && AnimAuthoring_SavePackageOf(Seq, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("anim_sequence_path"), Seq->GetPathName());
    Obj->SetStringField(TEXT("curve_name"), CurveName);
    Obj->SetStringField(TEXT("comment"), Comment);
    Obj->SetBoolField(TEXT("applied"), bApplied);
    Obj->SetBoolField(TEXT("saved"), bSaved);
    Obj->SetStringField(TEXT("message"), bApplied
        ? AnimAuthoring_SaveMessage(TEXT("Curve comment set"), bSaved, SaveError, TEXT(""))
        : TEXT("SetCurveComment returned false (only float curves support comments)"));
    return mcp_common::MakeOkReply(Obj);
}

// ---------------------------------------------------------------------------
// scale_anim_curve
// ---------------------------------------------------------------------------
// MCP:scale_anim_curve - Retime a FLOAT curve on a UAnimSequence: every key TIME becomes (t - origin) * factor + origin, and the key VALUES are left untouched -- this stretches/compresses a curve in time, it does not scale its amplitude. FLOAT-CURVE ONLY: transform curves and any other non-float curve return applied=false instead of an error.
//   @param anim_sequence_path - Asset path of the UAnimSequence.
//   @param curve_name - Name of the float curve.
//   @param factor - Scale factor applied to each key TIME (default 1.0).
//   @param origin - Time origin the key times are scaled about (default 0.0).
//   @returns { anim_sequence_path, curve_name, origin, factor, num_keys, applied, saved, message }
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
    if (!Controller.GetModel() || !Controller.GetModel()->FindFloatCurve(CurveId))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Float curve '%s' does not exist; call add_anim_float_curve first"), *CurveName));
    }

    // FLOAT-CURVE ONLY: ScaleCurve Report(Warning)+returns false for non-float curves.
    // Self-transacts via bShouldTransact; only bracketed, not wrapped in FScopedTransaction.
    Controller.OpenBracket(NSLOCTEXT("MCP", "ScaleAnimCurve", "Scale Anim Curve"));
    const bool bApplied = Controller.ScaleCurve(CurveId, static_cast<float>(Origin), static_cast<float>(Factor), /*bShouldTransact=*/true);
    Controller.CloseBracket();

    int32 NumKeys = 0;
    if (const FFloatCurve* C = Controller.GetModel()->FindFloatCurve(CurveId))
    {
        NumKeys = C->FloatCurve.Keys.Num();
    }

    FString SaveError;
    const bool bSaved = bApplied && AnimAuthoring_SavePackageOf(Seq, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("anim_sequence_path"), Seq->GetPathName());
    Obj->SetStringField(TEXT("curve_name"), CurveName);
    Obj->SetNumberField(TEXT("origin"), Origin);
    Obj->SetNumberField(TEXT("factor"), Factor);
    Obj->SetNumberField(TEXT("num_keys"), NumKeys);
    Obj->SetBoolField(TEXT("applied"), bApplied);
    Obj->SetBoolField(TEXT("saved"), bSaved);
    Obj->SetStringField(TEXT("message"), bApplied
        ? AnimAuthoring_SaveMessage(TEXT("Curve scaled"), bSaved, SaveError, TEXT(""))
        : TEXT("ScaleCurve returned false (only float curves can be scaled)"));
    return mcp_common::MakeOkReply(Obj);
}

// ---------------------------------------------------------------------------
// add_anim_float_curve
// ---------------------------------------------------------------------------
// MCP:add_anim_float_curve - Add a float curve (Editable flag) to a UAnimSequence through its data controller and set its keys from [time, value] pairs as linear keys with auto tangents, in one undo step, then save the package. An existing float curve of that name is kept and its keys are replaced (idempotent); curve_found confirms the curve and num_keys counts the keys sent. A missing sequence is an error and a failed save makes success false. Rekey: set_anim_curve_keys; transform curves: add_anim_transform_curve; a skeleton-only name: add_anim_curve_name; read back: get_anim_curve_data.
//   @param anim_sequence_path - Asset path of the UAnimSequence.
//   @param curve_name - Float curve name to add.
//   @param keys - JSON array of [time, value] pairs (default [[0,0],[1,1]])
//   @returns { anim_sequence_path, curve_name, num_float_curves, num_keys, curve_found, success, saved, message }
TSharedPtr<FJsonObject> HandleAddAnimFloatCurve(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimPath, CurveName;
    if (TSharedPtr<FJsonObject> Error = AnimAuthoring_ReadCurveTarget(Params, AnimPath, CurveName))
    {
        return Error;
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
    Controller.OpenBracket(NSLOCTEXT("MCP", "AddFloatCurve", "Add Float Curve"));

    // 0x00000004 == AACF_DefaultCurve.
    Controller.AddCurve(CurveId, 0x00000004);
    Controller.SetCurveKeys(CurveId, RichKeys, true);

    Controller.CloseBracket();

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
    Obj->SetBoolField(TEXT("success"), true);
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Float curve added"), bSaved, SaveError, TEXT("")));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("add_anim_float_curve"));
}

// ---------------------------------------------------------------------------
// add_anim_sync_marker
// ---------------------------------------------------------------------------
// MCP:add_anim_sync_marker - Append an authored sync marker (name and time, on marker track 0) to a UAnimSequence in an undo transaction and save it; duplicates are not checked, nor is the time checked against the sequence length. Returns num_sync_markers. Remove with remove_anim_sync_marker.
//   @param anim_sequence_path - Asset path of the UAnimSequence.
//   @param marker_name - Name of the sync marker (e.g. "LeftFootDown"); sequences in one sync group line up markers that share a name.
//   @param time - Sync marker time in seconds (default 0.5)
//   @returns { anim_sequence_path, marker_name, time, num_sync_markers, success, saved, message }
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
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("add_anim_sync_marker"));
}

// ---------------------------------------------------------------------------
// set_anim_root_motion
// ---------------------------------------------------------------------------
// MCP:set_anim_root_motion - Write the three root motion settings of a UAnimSequence (bEnableRootMotion, RootMotionRootLock, bForceRootLock) and save it. All three are written on every call, so an omitted one is reset to its default; an unknown root lock name applies RefPose while the reply echoes the given text.
//   @param anim_sequence_path - Asset path of the UAnimSequence.
//   @param enable_root_motion - Whether to enable root motion extraction (default true)
//   @param root_motion_root_lock - Root lock mode: RefPose|AnimFirstFrame|Zero (default RefPose)
//   @param force_root_lock - Whether to force the root lock (default false)
//   @returns { anim_sequence_path, enable_root_motion, root_motion_root_lock, force_root_lock, success, saved, message }
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

    const FScopedTransaction Transaction(LOCTEXT("SetAnimRootMotion", "Set Anim Root Motion"));
    Seq->Modify();

    Seq->bEnableRootMotion = bEnableRootMotion;
    Seq->RootMotionRootLock = Lock;
    Seq->bForceRootLock = bForceRootLock;

    bSaved = AnimAuthoring_SavePackageOf(Seq, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("anim_sequence_path"), Seq->GetPathName());
    Obj->SetBoolField(TEXT("enable_root_motion"), bEnableRootMotion);
    Obj->SetStringField(TEXT("root_motion_root_lock"), RootLockStr);
    Obj->SetBoolField(TEXT("force_root_lock"), bForceRootLock);
    Obj->SetBoolField(TEXT("success"), true);
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Root motion settings applied"), bSaved, SaveError, TEXT("")));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("set_anim_root_motion"));
}

// ---------------------------------------------------------------------------
// get_anim_sequence_info
// ---------------------------------------------------------------------------
// MCP:get_anim_sequence_info - Read a UAnimSequence without changing it: skeleton, play_length, frame_rate, num_frames, bone track and float curve counts from its data model, num_sync_markers, enable_root_motion, retarget_source and ref_pose_seq. Use get_anim_curve_names for the track and curve names.
//   @param anim_sequence_path - Asset path of the UAnimSequence.
//   @returns { anim_sequence_path, skeleton, play_length, frame_rate, num_frames, num_bone_tracks, num_float_curves, num_sync_markers, enable_root_motion, success }
TSharedPtr<FJsonObject> HandleGetAnimSequenceInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("anim_sequence_path"), AnimPath)) { return Missing; }

    UAnimSequence* Seq = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(AnimPath, TEXT("AnimSequence"), Seq)) { return Error; }

    const USkeleton* Skeleton = Seq->GetSkeleton();

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("anim_sequence_path"), Seq->GetPathName());
    Obj->SetStringField(TEXT("skeleton"), Skeleton ? Skeleton->GetPathName() : FString());

    FillSequenceModelCounts(Seq, Obj);
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
// MCP:add_montage_segment - Append an animation segment referencing a real UAnimSequence to a montage slot
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
    if (!Montage->IsValidSlot(SlotFName))
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

// ---------------------------------------------------------------------------
// add_anim_transform_curve — add a transform curve (RCT_Transform) to a
// UAnimSequence via the data controller.
// ---------------------------------------------------------------------------
// MCP:add_anim_transform_curve - Add a transform curve (RCT_Transform) to a UAnimSequence via the data controller
//   @param anim_sequence_path - Object path of the UAnimSequence.
//   @param curve_name - Name of the transform curve to add.
//   @param keys - JSON array of {time, location:[x,y,z], rotation:[pitch,yaw,roll], scale:[x,y,z]} objects
//   @returns { anim_sequence_path, curve_name, num_transform_curves, num_keys, keys_set, success, saved, save_error }
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

    IAnimationDataController& Controller = Seq->GetController();
    Controller.OpenBracket(LOCTEXT("AddTransformCurve", "MCP: Add Transform Curve"));
    Controller.AddCurve(CurveId, /*AACF_DefaultCurve*/0x00000004);
    const bool bSet = Controller.SetTransformCurveKeys(CurveId, Transforms, Times);
    Controller.NotifyPopulated();
    Controller.CloseBracket();

    int32 NumTransformCurves = 0;
    if (const IAnimationDataModel* Model = Seq->GetDataModel())
    {
        NumTransformCurves = Model->GetNumberOfTransformCurves();
    }

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Seq, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("anim_sequence_path"), Seq->GetPathName());
    Obj->SetStringField(TEXT("curve_name"), CurveName);
    Obj->SetNumberField(TEXT("num_transform_curves"), NumTransformCurves);
    Obj->SetNumberField(TEXT("num_keys"), Times.Num());
    Obj->SetBoolField(TEXT("keys_set"), bSet);
    Obj->SetBoolField(TEXT("success"), true);
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("add_anim_transform_curve"), SaveError);
}

// ---------------------------------------------------------------------------
// set_anim_additive_settings — set the additive animation type + base pose on a UAnimSequence.
// ---------------------------------------------------------------------------
// MCP:set_anim_additive_settings - Set the additive-animation type and base pose on a UAnimSequence
//   @param anim_sequence_path - Object path of the UAnimSequence.
//   @param additive_anim_type - Additive type: LocalSpaceBase|RotationOffsetMeshSpace|None (default LocalSpaceBase)
//   @param ref_pose_type - Base pose type: RefPose|AnimScaled|AnimFrame|LocalAnimFrame|None (default RefPose)
//   @param ref_frame_index - Frame index used for the base pose (default 0)
//   @param ref_pose_anim_path - Object path of the reference UAnimSequence for the base pose (optional)
//   @returns { anim_sequence_path, additive_anim_type, ref_pose_type, ref_frame_index, ref_pose_anim_set, success, saved, save_error }
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

    const FScopedTransaction Transaction(LOCTEXT("SetAdditiveSettings", "MCP: Set Additive Settings"));
    Seq->Modify();
    Seq->AdditiveAnimType = AdditiveType;
    Seq->RefPoseType = BasePoseType;
    Seq->RefFrameIndex = static_cast<int32>(RefFrameD);

    bool bRefPoseSet = false;
    FString RefPosePath;
    if (Params->TryGetStringField(TEXT("ref_pose_anim_path"), RefPosePath) && !RefPosePath.IsEmpty())
    {
        if (UAnimSequence* RefSeq = AnimAssetUtils::LoadAsset<UAnimSequence>(RefPosePath))
        {
            Seq->RefPoseSeq = RefSeq;
            bRefPoseSet = true;
        }
    }

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Seq, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("anim_sequence_path"), Seq->GetPathName());
    Obj->SetStringField(TEXT("additive_anim_type"), AdditiveAnimTypeToString(Seq->AdditiveAnimType.GetValue()));
    Obj->SetStringField(TEXT("ref_pose_type"), BasePoseTypeToString(Seq->RefPoseType.GetValue()));
    Obj->SetNumberField(TEXT("ref_frame_index"), Seq->RefFrameIndex);
    Obj->SetBoolField(TEXT("ref_pose_anim_set"), bRefPoseSet);
    Obj->SetBoolField(TEXT("success"), true);
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("set_anim_additive_settings"), SaveError);
}

// ---------------------------------------------------------------------------
// create_pose_asset_from_anim_sequence — build a UPoseAsset from a real anim.
// ---------------------------------------------------------------------------
// MCP:create_pose_asset_from_anim_sequence - Create a UPoseAsset built from a real UAnimSequence's frames
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

    // Idempotent: load existing.
    if (UPoseAsset* Existing = AnimAssetUtils::FindExisting<UPoseAsset>(FullPath))
    {
        TSharedPtr<FJsonObject> Obj = BuildPayload(Existing, /*bCreated=*/false);
        Obj->SetBoolField(TEXT("already_existed"), true);
        Obj->SetBoolField(TEXT("saved"), true);
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
//   duplicate_anim_sequence      -- StaticDuplicateObject into /Game
//   get_anim_curve_names         -- IAnimationDataModel::GetBoneTrackNames + GetFloatCurves + GetTransformCurves
// ===========================================================================

// MCP:remove_anim_bone_track - Remove the animation track of one bone from a UAnimSequence through its data controller (IAnimationDataController::RemoveBoneTrack) and save the package; removed=false when no track has that bone name, and the save result is not reported. List tracks with get_anim_curve_names.
//   @param sequence_path - Asset path of the UAnimSequence.
//   @param bone_name - Name of the bone track to remove (must match an existing track).
//   @returns { sequence_path, bone_name, removed }
TSharedPtr<FJsonObject> HandleRemoveAnimBoneTrack(const TSharedPtr<FJsonObject>& Params)
{
    FString SequencePath, BoneName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sequence_path"), SequencePath)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("bone_name"), BoneName)) { return Missing; }
    UAnimSequence* Sequence = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(SequencePath, TEXT("AnimSequence"), Sequence)) { return Error; }
    IAnimationDataController& Controller = Sequence->GetController();
    const bool bRemoved = Controller.RemoveBoneTrack(FName(*BoneName));
    mcp_common::SaveAssetPackage(Sequence->GetPackage(), Sequence);
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("sequence_path"), Sequence->GetPathName());
    Data->SetStringField(TEXT("bone_name"), BoneName);
    Data->SetBoolField(TEXT("removed"), bRemoved);
    return mcp_common::MakeOkReply(Data);
}

// MCP:remove_anim_curve - Remove a float or transform curve by name from a UAnimSequence through its data controller and save the package (the save result is not reported); removed=false when no such curve exists. Any curve_type other than transform targets float curves.
//   @param sequence_path - Asset path of the UAnimSequence.
//   @param curve_name - Name of the curve to remove.
//   @param curve_type - Curve type to target: float (default) or transform
//   @returns { sequence_path, curve_name, curve_type, removed }
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
    mcp_common::SaveAssetPackage(Sequence->GetPackage(), Sequence);
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("sequence_path"), Sequence->GetPathName());
    Data->SetStringField(TEXT("curve_name"), CurveName);
    Data->SetStringField(TEXT("curve_type"), CurveType.IsEmpty() ? TEXT("float") : CurveType);
    Data->SetBoolField(TEXT("removed"), bRemoved);
    return mcp_common::MakeOkReply(Data);
}

// MCP:remove_anim_sync_marker - Remove every authored sync marker with the given name from a UAnimSequence and save the package (no undo transaction; the save result is not reported). Returns markers_before, markers_after and removed, which is false when no marker had that name.
//   @param sequence_path - Asset path of the UAnimSequence.
//   @param marker_name - Name of the marker to remove.
//   @returns { sequence_path, marker_name, markers_before, markers_after, removed }
TSharedPtr<FJsonObject> HandleRemoveAnimSyncMarker(const TSharedPtr<FJsonObject>& Params)
{
    FString SequencePath, MarkerName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sequence_path"), SequencePath)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("marker_name"), MarkerName)) { return Missing; }
    UAnimSequence* Sequence = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(SequencePath, TEXT("AnimSequence"), Sequence)) { return Error; }
    const FName MarkerFName(*MarkerName);
    const int32 BeforeCount = Sequence->AuthoredSyncMarkers.Num();
    Sequence->AuthoredSyncMarkers.RemoveAll([&MarkerFName](const FAnimSyncMarker& M) {
        return M.MarkerName == MarkerFName;
    });
    const int32 AfterCount = Sequence->AuthoredSyncMarkers.Num();
    mcp_common::SaveAssetPackage(Sequence->GetPackage(), Sequence);
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("sequence_path"), Sequence->GetPathName());
    Data->SetStringField(TEXT("marker_name"), MarkerName);
    Data->SetNumberField(TEXT("markers_before"), BeforeCount);
    Data->SetNumberField(TEXT("markers_after"), AfterCount);
    Data->SetBoolField(TEXT("removed"), AfterCount < BeforeCount);
    return mcp_common::MakeOkReply(Data);
}

// MCP:set_anim_frame_rate - Set the sampling frame rate of a UAnimSequence (IAnimationDataController::SetFrameRate).
//   @param sequence_path - Asset path of the UAnimSequence.
//   @param fps - New frame rate in frames per second (integer > 0).
//   @returns { sequence_path, fps, numerator, denominator }
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
    UAnimSequence* Sequence = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(SequencePath, TEXT("AnimSequence"), Sequence)) { return Error; }
    const int32 Num = FMath::RoundToInt(Fps);
    const FFrameRate NewRate(Num, 1);
    IAnimationDataController& Controller = Sequence->GetController();
    Controller.SetFrameRate(NewRate);
    mcp_common::SaveAssetPackage(Sequence->GetPackage(), Sequence);
    const FFrameRate ReadBack = Sequence->GetSamplingFrameRate();
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("sequence_path"), Sequence->GetPathName());
    Data->SetNumberField(TEXT("fps"), ReadBack.Numerator);
    Data->SetNumberField(TEXT("numerator"), ReadBack.Numerator);
    Data->SetNumberField(TEXT("denominator"), ReadBack.Denominator);
    return mcp_common::MakeOkReply(Data);
}

// MCP:set_anim_number_of_frames - Set the number of sampled frames on a UAnimSequence (IAnimationDataController::SetNumberOfFrames).
//   @param sequence_path - Asset path of the UAnimSequence.
//   @param frame_count - New number of frames (integer > 0).
//   @returns { sequence_path, frame_count }
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
    IAnimationDataController& Controller = Sequence->GetController();
    Controller.SetNumberOfFrames(FFrameNumber(FrameCount));
    mcp_common::SaveAssetPackage(Sequence->GetPackage(), Sequence);
    const int32 ReadBack = Sequence->GetNumberOfSampledKeys();
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("sequence_path"), Sequence->GetPathName());
    Data->SetNumberField(TEXT("frame_count"), ReadBack);
    return mcp_common::MakeOkReply(Data);
}

// MCP:duplicate_anim_sequence - Copy a UAnimSequence to a new asset package_path/new_name with the same skeleton (StaticDuplicateObject) and save it; the save result is not reported, and a name taken by an asset of another class is refused. Returns duplicate_path and duplicate_class. For other asset types use duplicate_asset.
//   @param source_path - Asset path of the source UAnimSequence.
//   @param new_name - Name of the new duplicate.
//   @param package_path - Target content package path where the asset is created (default /Game/MCP)
//   @returns { source_path, duplicate_path, duplicate_class }
TSharedPtr<FJsonObject> HandleDuplicateAnimSequence(const TSharedPtr<FJsonObject>& Params)
{
    FString SourcePath, NewName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("source_path"), SourcePath)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("new_name"), NewName)) { return Missing; }
    FString PackagePath = mcp_common::DefaultAssetFolder();
    Params->TryGetStringField(TEXT("package_path"), PackagePath);
    PackagePath.RemoveFromEnd(TEXT("/"));

    UAnimSequence* Source = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(SourcePath, TEXT("Source AnimSequence"), Source)) { return Error; }
    const FString FullPackageName = PackagePath / NewName;
    FString PackageError;
    UPackage* Package = mcp_common::CreateAssetPackage(FullPackageName, NewName, Source->GetClass(), PackageError);
    if (!Package)
    {
        return mcp_common::MakeErrorReply(PackageError);
    }
    Package->FullyLoad();

    UObject* Dup = StaticDuplicateObject(Source, Package, FName(*NewName), RF_Public | RF_Standalone);
    UAnimSequence* DupSeq = Cast<UAnimSequence>(Dup);
    if (!DupSeq)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Duplication produced wrong type (%s)"), Dup ? *Dup->GetClass()->GetName() : TEXT("null")));
    }
    DupSeq->SetSkeleton(Source->GetSkeleton());
    FAssetRegistryModule::AssetCreated(DupSeq);
    Package->MarkPackageDirty();
    mcp_common::SaveAssetPackage(DupSeq->GetPackage(), DupSeq);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("source_path"), Source->GetPathName());
    Data->SetStringField(TEXT("duplicate_path"), DupSeq->GetPathName());
    Data->SetStringField(TEXT("duplicate_class"), DupSeq->GetClass()->GetName());
    return mcp_common::MakeOkReply(Data);
}

// MCP:get_anim_curve_names - List the bone tracks, float curves, and transform curves on a UAnimSequence
//   @param sequence_path - Asset path of the UAnimSequence.
//   @returns { sequence_path, bone_track_count, float_curve_count, transform_curve_count, bone_tracks, float_curves, transform_curves }
TSharedPtr<FJsonObject> HandleGetAnimCurveNames(const TSharedPtr<FJsonObject>& Params)
{
    FString SequencePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sequence_path"), SequencePath)) { return Missing; }
    UAnimSequence* Sequence = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(SequencePath, TEXT("AnimSequence"), Sequence)) { return Error; }
    const IAnimationDataModel* Model = Sequence->GetDataModel();

    TArray<TSharedPtr<FJsonValue>> BoneTracks;
    TArray<TSharedPtr<FJsonValue>> FloatCurves;
    TArray<TSharedPtr<FJsonValue>> TransformCurves;
    if (Model)
    {
        TArray<FName> BoneTrackNames;
        Model->GetBoneTrackNames(BoneTrackNames);
        for (const FName& Bone : BoneTrackNames)
        {
            BoneTracks.Add(MakeShared<FJsonValueString>(Bone.ToString()));
        }
        for (const FFloatCurve& C : Model->GetFloatCurves())
        {
            FloatCurves.Add(MakeShared<FJsonValueString>(C.GetName().ToString()));
        }
        for (const FTransformCurve& C : Model->GetTransformCurves())
        {
            TransformCurves.Add(MakeShared<FJsonValueString>(C.GetName().ToString()));
        }
    }
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("sequence_path"), Sequence->GetPathName());
    Data->SetNumberField(TEXT("bone_track_count"), BoneTracks.Num());
    Data->SetNumberField(TEXT("float_curve_count"), FloatCurves.Num());
    Data->SetNumberField(TEXT("transform_curve_count"), TransformCurves.Num());
    Data->SetArrayField(TEXT("bone_tracks"), BoneTracks);
    Data->SetArrayField(TEXT("float_curves"), FloatCurves);
    Data->SetArrayField(TEXT("transform_curves"), TransformCurves);
    return mcp_common::MakeOkReply(Data);
}

// ========================================================================== //
// Anim sequence curves advanced

#define LOCTEXT_NAMESPACE "UEAgentAnimationAuthoringCommands"
// Per-curve keyframe manipulation, curve flags, rename, full readback and
// reimport on an AnimSequence; `add_anim_float_curve` /
// `add_anim_transform_curve` / `remove_anim_curve` cover add+remove at the
// curve level.
//   set_anim_curve_keys                  IAnimationDataController::SetCurveKeys
//                                         (replace keys on an existing curve).
//   set_anim_curve_interp_mode           IAnimationDataController::SetCurveFlags
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

// MCP:set_anim_curve_keys - Replace every key of an existing float curve on a UAnimSequence with the given [time, value] pairs as linear keys with auto tangents, through its data controller in an undo transaction, then save the package; num_keys is read back. The curve must exist (add_anim_float_curve first) and keys needs at least one valid pair. A transform curve is refused, since its keys are transforms, not [time, value] pairs: rekey it with add_anim_transform_curve. Any other curve_type is an error, and so is a rekey the data controller rejects, with nothing saved. A failed save makes success false.
//   @param anim_sequence_path - Full asset path to the UAnimSequence
//   @param curve_name - Name of the float curve (e.g. "MyCurve")
//   @param keys - JSON array of [time, value] pairs, time in seconds, at least one; entries that are not such a pair are skipped, and tangents or interpolation cannot be given.
//   @param curve_type - Curve type to target: float (default) or transform; transform is refused with an error pointing at add_anim_transform_curve.
//   @returns { anim_sequence_path, curve_name, curve_type, num_keys, applied, saved, save_error }
TSharedPtr<FJsonObject> HandleSetAnimCurveKeys(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimPath, CurveName;
    if (TSharedPtr<FJsonObject> Error = AnimAuthoring_ReadCurveTarget(Params, AnimPath, CurveName))
    {
        return Error;
    }
    const FString CurveTypeRaw = AnimAuthoring_ReadCurveType(Params);
    const FString CTLower = CurveTypeRaw.ToLower();
    if (CTLower == TEXT("transform"))
    {
        // A transform curve holds nine channel curves (translation/rotation/scale by X/Y/Z), and the
        // data controller keys one only through an identifier that names its channel and axis; a
        // [time, value] pair addresses none of them, so the call is refused instead of writing nothing.
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("set_anim_curve_keys takes [time, value] keys, which only a float curve has; '%s' asks for a transform curve. ")
            TEXT("Rekey a transform curve with add_anim_transform_curve (its keys are {time, location, rotation, scale})."),
            *CurveTypeRaw));
    }
    if (CTLower != TEXT("float"))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Unsupported curve_type '%s' (set_anim_curve_keys rekeys float curves: use float)"), *CurveTypeRaw));
    }

    // keys=[[time,value],...]; at least one pair.
    const TArray<FRichCurveKey> RichKeys = AnimAuthoring_ReadTimeValueKeys(Params);
    if (RichKeys.Num() == 0)
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: keys (>=1 [time,value] pair)"));
    }

    UAnimSequence* Seq = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(AnimPath, TEXT("AnimSequence"), Seq)) { return Error; }

    const FName CurveFName(*CurveName);
    const FAnimationCurveIdentifier CurveId(CurveFName, ERawCurveTrackTypes::RCT_Float);
    IAnimationDataController& Controller = Seq->GetController();

    if (!Controller.GetModel()->FindFloatCurve(CurveId))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Float curve '%s' does not exist; call add_anim_float_curve first"), *CurveName));
    }

    const FScopedTransaction Transaction(LOCTEXT("SetAnimCurveKeys", "Set Anim Curve Keys"));
    Controller.OpenBracket(NSLOCTEXT("MCP", "SetCurveKeys", "Set Curve Keys"));
    const bool bApplied = Controller.SetCurveKeys(CurveId, RichKeys, true);
    Controller.CloseBracket();

    // SetCurveKeys returns false only when it wrote neither the curve's keys nor its control keys.
    if (!bApplied)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("The data controller rejected the keys for float curve '%s' (see the editor log); nothing was saved"),
            *CurveName));
    }

    int32 NumKeys = 0;
    if (const FFloatCurve* C = Controller.GetModel()->FindFloatCurve(CurveId))
    {
        NumKeys = C->FloatCurve.Keys.Num();
    }

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Seq, SaveError);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("anim_sequence_path"), Seq->GetPathName());
    Result->SetStringField(TEXT("curve_name"), CurveName);
    Result->SetStringField(TEXT("curve_type"), CurveTypeRaw);
    Result->SetNumberField(TEXT("num_keys"), NumKeys);
    Result->SetBoolField(TEXT("applied"), bApplied);
    return mcp_common::MakeSavedReply(Result, bSaved, TEXT("set_anim_curve_keys"), SaveError);
}

// MCP:set_anim_curve_interp_mode - Replace the curve flags (the EAnimAssetCurveFlags bitmask) of an existing float or transform curve on a UAnimSequence with one flag, in an undo transaction, then save the package. Despite the name it changes no key interpolation or tangents: linear and editable both set Editable; metadata, drive_track and disabled set those flags. interp_mode is required; an unknown mode, curve or sequence is an error, and any curve_type other than transform means float. old_flags and new_flags are the bitmasks before and after; a failed save makes success false. get_anim_curve_data reads the flags.
//   @param anim_sequence_path - Full asset path to the UAnimSequence
//   @param curve_name - Name of the curve
//   @param interp_mode [string!] - One of "linear|editable|metadata|drive_track|disabled"
//   @param curve_type - Curve type to target: float (default) or transform
//   @returns { anim_sequence_path, curve_name, interp_mode, old_flags, new_flags, applied, saved, save_error }
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
    return mcp_common::MakeSavedReply(Result, bSaved, TEXT("set_anim_curve_interp_mode"), SaveError);
}

// MCP:rename_anim_curve - Rename a float or transform curve on a UAnimSequence through its data controller (one undo step) and save it; errors when the source curve does not exist or the rename is refused. Any curve_type other than transform means float. Returns renamed and saved.
//   @param anim_sequence_path - Full asset path to the UAnimSequence
//   @param old_curve_name - Name of the existing curve to rename, as listed by get_anim_curve_names.
//   @param new_curve_name - Name the curve gets after the rename.
//   @param curve_type - Curve type to target: float (default) or transform
//   @returns { anim_sequence_path, old_curve_name, new_curve_name, renamed, saved }
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
    return mcp_common::MakeSavedReply(Result, bSaved, TEXT("rename_anim_curve"));
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

// MCP:reimport_anim_sequence - Reimport a UAnimSequence, or any reimportable asset (the class is not checked), from source_file_path or its first recorded source via FReimportManager, automated (no dialogs) and unsaved; a missing source returns reimported=false with a note instead of crashing.
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
    if (EffectiveSource.IsEmpty())
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("anim_sequence_path"), Asset->GetPathName());
        Result->SetStringField(TEXT("source_file_path"), FString());
        Result->SetBoolField(TEXT("reimported"), false);
        Result->SetBoolField(TEXT("source_validated"), false);
        Result->SetStringField(TEXT("note"),
            TEXT("Asset has a registered reimport factory but no source filename is recorded -- "
                 "the sequence was likely created at runtime or its FBX was relocated. "
                 "Provide 'source_file_path' explicitly or assign one in the asset's AssetImportData."));
        return mcp_common::MakeOkReply(Result);
    }

    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    if (!PlatformFile.FileExists(*EffectiveSource))
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("anim_sequence_path"), Asset->GetPathName());
        Result->SetStringField(TEXT("source_file_path"), EffectiveSource);
        Result->SetBoolField(TEXT("reimported"), false);
        Result->SetBoolField(TEXT("source_validated"), false);
        Result->SetStringField(TEXT("note"),
            FString::Printf(TEXT("Source file does not exist on disk: %s -- "
                                 "FReimportManager::Reimport would crash the editor on a missing source. "
                                 "Provide a valid 'source_file_path' or fix the asset's import path."),
                             *EffectiveSource));
        return mcp_common::MakeOkReply(Result);
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
    return mcp_common::MakeOkReply(Result);
}

// ---------------------------------------------------------------------------
// link_montage_sections
// Sets FCompositeSection::NextSectionName so montage sections chain (a field
// add_montage_section does not write). Pass
// next_section_name = "" or "None" to clear the link.
// ---------------------------------------------------------------------------
// MCP:link_montage_sections - Chain one montage section to the next by setting its NextSectionName.  add_montage_section creates sections but never links them; this writes FCompositeSection::NextSectionName so playback flows section -> next. Pass next_section_name "" or "None" to clear the link. Idempotent.
//   @param montage_path - Full asset path of the montage (e.g. '/Game/MyGame/AM_Example').
//   @param section_name - Name of the existing section whose link to set.
//   @param next_section_name - Name of the existing section to play next (or "" / "None" to clear).
//   @returns { montage_path, section_name, next_section_name, linked, section_count, sections, saved, message }
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

    const FScopedTransaction Transaction(LOCTEXT("LinkMontageSections", "Link Montage Sections"));
    Montage->Modify();
    Target->NextSectionName = NextFName;

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Montage, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("montage_path"), Montage->GetPathName());
    Obj->SetStringField(TEXT("section_name"), SectionName);
    Obj->SetStringField(TEXT("next_section_name"), Target->NextSectionName.ToString());
    Obj->SetBoolField(TEXT("linked"), Target->NextSectionName != NAME_None);
    Obj->SetNumberField(TEXT("section_count"), Montage->CompositeSections.Num());
    Obj->SetArrayField(TEXT("sections"), CollectSectionNames(Montage));
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Section link set"), bSaved, SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("link_montage_sections"));
}

// ---------------------------------------------------------------------------
// set_blendspace_properties
// Configures the X (index 0) and, for 2D blendspaces, Y (index 1) axes of an
// existing UBlendSpace — name / min / max / grid. create_blend_space only
// configures the X axis; this is the only way to set the Y axis (or change X
// after creation). Writes BlendParameters via reflection (protected fixed-size
// array, ArrayDim==3 — same path create_blend_space uses), reads back via
// GetBlendParameter().
// ---------------------------------------------------------------------------
// MCP:set_blendspace_properties - Configure an existing blend space's X and (for 2D) Y axis: name, min, max, grid divisions.  create_blend_space only sets X; this sets the Y axis too (and can change X after creation). On a 1D blend space the y_axis_* params are reported as an honest note and ignored. Only the supplied fields change.
//   @param blend_space_path - Full asset path of the UBlendSpace / UBlendSpace1D.
//   @param x_axis_name - Optional X axis display name.
//   @param x_axis_min [number] - Optional X axis minimum value.
//   @param x_axis_max [number] - Optional X axis maximum value.
//   @param x_axis_grid [integer] - Optional X axis grid divisions (>= 1).
//   @param y_axis_name - Optional Y axis display name (2D only).
//   @param y_axis_min [number] - Optional Y axis minimum value (2D only).
//   @param y_axis_max [number] - Optional Y axis maximum value (2D only).
//   @param y_axis_grid [integer] - Optional Y axis grid divisions (2D only, >= 1).
//   @returns { blend_space_path, is_1d, x_axis: {name, min, max, grid}, y_axis: {name, min, max, grid}, applied_count, notes, saved }
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
    TArray<FString> Notes;

    auto ApplyAxis = [&](int32 Index, const TCHAR* Prefix)
    {
        FBlendParameter* Axis = BlendParamsProp->ContainerPtrToValuePtr<FBlendParameter>(BlendSpace, Index);
        if (!Axis)
        {
            return;
        }
        FString S;
        if (Params->TryGetStringField(FString(Prefix) + TEXT("_name"), S)) { Axis->DisplayName = S; ++AppliedCount; }
        double D = 0.0;
        if (Params->TryGetNumberField(FString(Prefix) + TEXT("_min"), D)) { Axis->Min = static_cast<float>(D); ++AppliedCount; }
        if (Params->TryGetNumberField(FString(Prefix) + TEXT("_max"), D)) { Axis->Max = static_cast<float>(D); ++AppliedCount; }
        int32 G = 0;
        if (Params->TryGetNumberField(FString(Prefix) + TEXT("_grid"), G)) { Axis->GridNum = FMath::Max(1, G); ++AppliedCount; }
    };

    ApplyAxis(0, TEXT("x_axis"));

    // Build the field names so the codegen extractor does not see them as literal
    // string inputs (their JSON types are declared on the @param hints instead).
    const FString YPrefix = TEXT("y_axis");
    const bool bYProvided = Params->HasField(YPrefix + TEXT("_name")) || Params->HasField(YPrefix + TEXT("_min"))
        || Params->HasField(YPrefix + TEXT("_max")) || Params->HasField(YPrefix + TEXT("_grid"));
    if (bIs1D)
    {
        if (bYProvided)
        {
            Notes.Add(TEXT("y_axis_* ignored: this is a 1D blend space (only the X axis applies)"));
        }
    }
    else
    {
        ApplyAxis(1, TEXT("y_axis"));
    }

    if (AppliedCount == 0)
    {
        return mcp_common::MakeErrorReply(
            TEXT("No axis properties provided. Supply at least one of: x_axis_name/min/max/grid (and y_axis_* for 2D)"));
    }

    // Without this the RUNTIME does not see the blend space.
    RebuildBlendSpaceRuntimeData(BlendSpace);
    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(BlendSpace, SaveError);

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
    Obj->SetNumberField(TEXT("applied_count"), AppliedCount);
    Obj->SetArrayField(TEXT("notes"), mcp_common::StringsToJson(Notes));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("set_blendspace_properties"), SaveError);
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
// MCP:add_notify_track - Add a named notify TRACK to an AnimSequence (the track itself, not events on it)
//   @param animation_path - Full asset path to the UAnimSequence.
//   @param track_name - Notify track name to add.
//   @param color - Optional track color as an [r,g,b] or [r,g,b,a] float array (default white).
//   @returns { animation_path, track_name, track_names, track_count, already_existed, saved, message }
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
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("add_notify_track"));
}

// ---------------------------------------------------------------------------
// remove_notify_track
// ---------------------------------------------------------------------------
// MCP:remove_notify_track - Remove a named notify TRACK from an AnimSequence (deletes events on it + reindexes the rest)
//   @param animation_path - Full asset path to the UAnimSequence.
//   @param track_name - Notify track name to remove.
//   @returns { animation_path, track_name, track_names, track_count, already_absent, saved, message }
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
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("remove_notify_track"));
}

// ---------------------------------------------------------------------------
// get_notify_track_names
// ---------------------------------------------------------------------------
// MCP:get_notify_track_names - List the notify track names of a UAnimSequence in track order (UAnimationBlueprintLibrary::GetAnimationNotifyTrackNames) without changing it; returns track_names and track_count. Use get_anim_notifies for the events and add_notify_track or remove_notify_track to edit tracks.
//   @param animation_path - Full asset path to the UAnimSequence.
//   @returns { animation_path, track_names, track_count }
TSharedPtr<FJsonObject> HandleGetNotifyTrackNames(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimationPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("animation_path"), AnimationPath)) { return Missing; }

    UAnimSequence* AnimSeq = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(AnimationPath, TEXT("AnimSequence"), AnimSeq)) { return Error; }

    const TArray<TSharedPtr<FJsonValue>> TrackNamesJson = NotifyTrackNamesJson(AnimSeq);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("animation_path"), AnimSeq->GetPathName());
    Obj->SetArrayField(TEXT("track_names"), TrackNamesJson);
    Obj->SetNumberField(TEXT("track_count"), TrackNamesJson.Num());
    return mcp_common::MakeOkReply(Obj);
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

// MCP:add_anim_composite_segment - Append an FAnimSegment referencing a real UAnimSequence to a UAnimComposite's AnimationTrack, then recompute its length
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

// MCP:remove_anim_composite_segment - Remove an FAnimSegment by index from a UAnimComposite's AnimationTrack, then recompute its length
//   @param composite_path - Object path of the UAnimComposite (e.g. "/Game/Animation/AC_Example").
//   @param index - Zero-based index of the segment to remove, in [0, num_segments).
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
// edit_anim_notify edits an existing notify event; get_anim_notifies is its reader
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

// MCP:edit_anim_notify - Edit an existing FAnimNotifyEvent on a UAnimSequence (identified by name + occurrence)
//   @param animation_path - Full asset path to the UAnimSequence.
//   @param notify_name - NotifyName of the event to edit (FName). Names are not unique; see occurrence_index.
//   @param occurrence_index - Zero-based index AMONG events sharing notify_name (default 0)
//   @param time - New trigger time in seconds (FAnimNotifyEvent::SetTime). Optional.
//   @param duration - New duration in seconds (FAnimNotifyEvent::SetDuration; meaningful for notify-state). Optional.
//   @param trigger_chance - New NotifyTriggerChance in [0,1]. Optional.
//   @param trigger_weight_threshold - New TriggerWeightThreshold. Optional.
//   @param montage_tick_type - New MontageTickType: 'Queued' or 'BranchingPoint'. Optional.
//   @param track_index - New TrackIndex (zero-based). Optional.
//   @returns { animation_path, notify_name, occurrence_index, matched_count, notify, applied_count, applied, saved, message }
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

    const FScopedTransaction Transaction(LOCTEXT("EditAnimNotify", "Edit Anim Notify"));
    AnimSeq->Modify();

    FAnimNotifyEvent& Event = AnimSeq->Notifies[TargetArrayIndex];
    int32 AppliedCount = 0;
    bool bTimingChanged = false;

    // Duration first, then time: SetTime recomputes EndLink from the CURRENT duration,
    // so applying duration before time keeps the end anchored to the final values.
    double DurationD = 0.0;
    if (Params->TryGetNumberField(TEXT("duration"), DurationD))
    {
        Event.SetDuration(static_cast<float>(DurationD));
        ++AppliedCount;
        bTimingChanged = true;
    }

    double TimeD = 0.0;
    if (Params->TryGetNumberField(TEXT("time"), TimeD))
    {
        Event.SetTime(static_cast<float>(TimeD), EAnimLinkMethod::Absolute);
        ++AppliedCount;
        bTimingChanged = true;
    }

    double ChanceD = 0.0;
    if (Params->TryGetNumberField(TEXT("trigger_chance"), ChanceD))
    {
        // ClampMin/Max meta only fires through the editor/property path; clamp here.
        Event.NotifyTriggerChance = FMath::Clamp(static_cast<float>(ChanceD), 0.0f, 1.0f);
        ++AppliedCount;
    }

    double WeightD = 0.0;
    if (Params->TryGetNumberField(TEXT("trigger_weight_threshold"), WeightD))
    {
        Event.TriggerWeightThreshold = static_cast<float>(WeightD);
        ++AppliedCount;
    }

    if (bHasTickType)
    {
        Event.MontageTickType = static_cast<EMontageNotifyTickType::Type>(TickTypeValue);
        ++AppliedCount;
    }

    double TrackIndexD = 0.0;
    if (Params->TryGetNumberField(TEXT("track_index"), TrackIndexD))
    {
        Event.TrackIndex = FMath::Max(0, static_cast<int32>(TrackIndexD));
        ++AppliedCount;
    }

    if (AppliedCount == 0)
    {
        return mcp_common::MakeErrorReply(
            TEXT("No editable properties provided. Supply at least one of: time, duration, trigger_chance, "
                 "trigger_weight_threshold, montage_tick_type, track_index"));
    }

    // Keep Notifies time-sorted after a timing edit (the array contract).
    if (bTimingChanged)
    {
        AnimSeq->SortNotifies();
    }

    UE_LOG(LogTemp, Verbose, TEXT("[edit_anim_notify] %s notify '%s'[occ %d] -> %d field(s) applied (timing_changed=%d)"),
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
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("edit_anim_notify"));
}

// MCP:set_montage_properties - Set only the supplied blend modes, auto blend out, trigger time, sync group/slot and deprecated root motion fields of a UAnimMontage in one undo step and save it; bad enum names, or no field, are refused before any change. For blend times use set_montage_blend_times.
//   @param montage_path - Full asset path of the montage (e.g. '/Game/MyGame/AM_Example').
//   @param blend_mode_in - BlendModeIn: 'Standard' or 'Inertialization' (EMontageBlendMode). Optional.
//   @param blend_mode_out - BlendModeOut: 'Standard' or 'Inertialization' (EMontageBlendMode). Optional.
//   @param enable_auto_blend_out - When true the montage blends out by itself near its end (bEnableAutoBlendOut); omitted keeps the current value.
//   @param blend_out_trigger_time - BlendOutTriggerTime in seconds (<0 = auto from BlendOut time). Optional.
//   @param sync_group - Sync group name (SyncGroup) the montage joins for marker-based sync; an empty string sets None, omitted keeps the current value.
//   @param sync_slot_index - Zero-based index of the slot track whose animation drives the sync group (SyncSlotIndex); omitted keeps the current value.
//   @param enable_root_motion_translation - bEnableRootMotionTranslation (bool, DEPRECATED since 4.5). Optional.
//   @param enable_root_motion_rotation - bEnableRootMotionRotation (bool, DEPRECATED since 4.5). Optional.
//   @param root_motion_root_lock - RootMotionRootLock: 'RefPose'|'AnimFirstFrame'|'Zero' (DEPRECATED since 4.5). Optional.
//   @returns { montage_path, blend_mode_in, blend_mode_out, enable_auto_blend_out, blend_out_trigger_time, sync_group, sync_slot_index, root_motion: {...}, applied_count, notes, saved, message }
TSharedPtr<FJsonObject> HandleSetMontageProperties(const TSharedPtr<FJsonObject>& Params)
{
    FString MontagePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("montage_path"), MontagePath)) { return Missing; }

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

    const FScopedTransaction Transaction(LOCTEXT("SetMontageProperties", "Set Montage Properties"));
    Montage->Modify();

    int32 AppliedCount = 0;
    TArray<FString> Notes;

    // BlendModeIn/Out are public 'enum class EMontageBlendMode : uint8' members -> assign
    // directly (type-checked; BlueprintReadOnly restricts BP write only, not C++).
    if (bHasBlendIn)  { Montage->BlendModeIn  = static_cast<EMontageBlendMode>(BlendInValue);  ++AppliedCount; }
    if (bHasBlendOut) { Montage->BlendModeOut = static_cast<EMontageBlendMode>(BlendOutValue); ++AppliedCount; }

    bool bAutoBlendOut = false;
    if (Params->TryGetBoolField(TEXT("enable_auto_blend_out"), bAutoBlendOut))
    {
        Montage->bEnableAutoBlendOut = bAutoBlendOut;
        ++AppliedCount;
    }

    double TriggerTimeD = 0.0;
    if (Params->TryGetNumberField(TEXT("blend_out_trigger_time"), TriggerTimeD))
    {
        Montage->BlendOutTriggerTime = static_cast<float>(TriggerTimeD);
        ++AppliedCount;
    }

    FString SyncGroupStr;
    if (Params->TryGetStringField(TEXT("sync_group"), SyncGroupStr))
    {
        Montage->SyncGroup = FName(*SyncGroupStr);
        ++AppliedCount;
    }

    double SyncSlotD = 0.0;
    if (Params->TryGetNumberField(TEXT("sync_slot_index"), SyncSlotD))
    {
        Montage->SyncSlotIndex = static_cast<int32>(SyncSlotD);
        ++AppliedCount;
    }

    // -------- Deprecated-since-4.5 root-motion members (honest NOTE on use). -------- //
    // Comment-only deprecation (no UE_DEPRECATED macro) so direct assignment is warning-
    // free; still public. RootMotionRootLock is a TEnumAsByte -> set via reflection.
    bool bRootTrans = false;
    if (Params->TryGetBoolField(TEXT("enable_root_motion_translation"), bRootTrans))
    {
        Montage->bEnableRootMotionTranslation = bRootTrans;
        ++AppliedCount;
        Notes.Add(TEXT("enable_root_motion_translation is DEPRECATED since 4.5 (root motion is controlled by anim sequences)"));
    }
    bool bRootRot = false;
    if (Params->TryGetBoolField(TEXT("enable_root_motion_rotation"), bRootRot))
    {
        Montage->bEnableRootMotionRotation = bRootRot;
        ++AppliedCount;
        Notes.Add(TEXT("enable_root_motion_rotation is DEPRECATED since 4.5 (root motion is controlled by anim sequences)"));
    }
    if (bHasRootLock)
    {
        if (FByteProperty* LockProp = FindFProperty<FByteProperty>(UAnimMontage::StaticClass(), TEXT("RootMotionRootLock")))
        {
            void* Addr = LockProp->ContainerPtrToValuePtr<void>(Montage);
            LockProp->SetIntPropertyValue(Addr, static_cast<int64>(RootLockValue));
            ++AppliedCount;
            Notes.Add(TEXT("root_motion_root_lock is DEPRECATED since 4.5 (root motion is controlled by anim sequences)"));
        }
        else
        {
            return mcp_common::MakeErrorReply(TEXT("Could not access RootMotionRootLock via reflection"));
        }
    }

    if (AppliedCount == 0)
    {
        return mcp_common::MakeErrorReply(
            TEXT("No properties provided. Supply at least one of: blend_mode_in, blend_mode_out, enable_auto_blend_out, "
                 "blend_out_trigger_time, sync_group, sync_slot_index, enable_root_motion_translation, "
                 "enable_root_motion_rotation, root_motion_root_lock"));
    }

    UE_LOG(LogTemp, Verbose, TEXT("[set_montage_properties] %s -> %d field(s) applied (mode_in=%s mode_out=%s)"),
        *Montage->GetPathName(), AppliedCount,
        AnimAuthoring_BlendModeName(Montage->BlendModeIn), AnimAuthoring_BlendModeName(Montage->BlendModeOut));

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Montage, SaveError);

    // Read the deprecated lock back through reflection so the round-trip is honest.
    int32 ReadbackRootLock = 0;
    if (FByteProperty* LockProp = FindFProperty<FByteProperty>(UAnimMontage::StaticClass(), TEXT("RootMotionRootLock")))
    {
        const void* Addr = LockProp->ContainerPtrToValuePtr<void>(Montage);
        ReadbackRootLock = static_cast<int32>(LockProp->GetUnsignedIntPropertyValue(Addr));
    }

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("montage_path"), Montage->GetPathName());
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

    Obj->SetNumberField(TEXT("applied_count"), AppliedCount);
    Obj->SetArrayField(TEXT("notes"), mcp_common::StringsToJson(Notes));
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Montage properties set"), bSaved, SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("set_montage_properties"));
}

// ===========================================================================
// Blend space input-smoothing (interpolation) + anim retarget source
// (Helpers ParseFilterInterpType / FilterInterpTypeToString / InterpAxisJson /
//  ReadBlendSpaceInterpolation live in the blendspace-samples anonymous namespace above, since
//  get_blendspace_samples also round-trips the interpolation through them.)

// ---------------------------------------------------------------------------
// set_blendspace_interpolation
// ---------------------------------------------------------------------------
// set_blendspace_properties only writes the BlendParameters axis (name/min/max/
// grid); the INPUT-SMOOTHING (interpolation) of a blend space lives in a SIBLING
// fixed-size array member, UBlendSpace::InterpolationParam[3] (BlendSpace.h,
// EditAnywhere, FInterpolationParameter per axis). We reach it by reflection
// EXACTLY like set_blendspace_properties reaches BlendParameters:
//   FindFProperty<FStructProperty>(UBlendSpace::StaticClass(), "InterpolationParam")
//   -> ContainerPtrToValuePtr<FInterpolationParameter>(BS, AxisIndex).
// 1D blend spaces only smooth axis 0; 2D smooth axes 0 and 1. ClampMin="0" metas
// on InterpolationTime/DampingRatio/MaxSpeed fire only through the editor property
// path, so we clamp to >=0 here.
// ---------------------------------------------------------------------------
// MCP:set_blendspace_interpolation - Set a blend space's INPUT-SMOOTHING (interpolation) per axis: smoothing time, damping ratio, max speed, smoothing type.  Distinct from set_blendspace_properties (which only sets the BlendParameters axis name/min/max/grid). Writes UBlendSpace::InterpolationParam[axis] via reflection. 1D smooths axis 0 only; 2D smooths X (axis 0) and Y (axis 1). Only the supplied fields change.
//   @param blend_space_path - Full asset path of the UBlendSpace / UBlendSpace1D.
//   @param x_interpolation_time [number] - Optional X-axis smoothing time in seconds (>= 0; 0 disables smoothing).
//   @param x_damping_ratio [number] - Optional X-axis damping ratio (>= 0; only used when type is SpringDamper).
//   @param x_max_speed [number] - Optional X-axis maximum speed in real units (>= 0; only used when type is SpringDamper or Exponential).
//   @param x_interpolation_type - Optional X-axis smoothing type: Averaged|Linear|Cubic|EaseInOut|Exponential|SpringDamper.
//   @param y_interpolation_time [number] - Optional Y-axis smoothing time in seconds (2D only, >= 0).
//   @param y_damping_ratio [number] - Optional Y-axis damping ratio (2D only, >= 0).
//   @param y_max_speed [number] - Optional Y-axis maximum speed (2D only, >= 0).
//   @param y_interpolation_type - Optional Y-axis smoothing type (2D only): Averaged|Linear|Cubic|EaseInOut|Exponential|SpringDamper.
//   @returns { blend_space_path, is_1d, x_axis: {interpolation_time, damping_ratio, max_speed, interpolation_type}, y_axis: {...}, applied_count, notes, saved }
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
    TArray<FString> Notes;
    bool bTypeError = false;
    FString TypeErrorMsg;

    auto ApplyAxis = [&](int32 Index, const TCHAR* Prefix)
    {
        FInterpolationParameter* Axis = InterpProp->ContainerPtrToValuePtr<FInterpolationParameter>(BlendSpace, Index);
        if (!Axis)
        {
            return;
        }
        double D = 0.0;
        // ClampMin="0" metas only fire through the editor property path -> clamp here.
        if (Params->TryGetNumberField(FString(Prefix) + TEXT("_interpolation_time"), D)) { Axis->InterpolationTime = FMath::Max(0.0f, static_cast<float>(D)); ++AppliedCount; }
        if (Params->TryGetNumberField(FString(Prefix) + TEXT("_damping_ratio"), D))      { Axis->DampingRatio     = FMath::Max(0.0f, static_cast<float>(D)); ++AppliedCount; }
        if (Params->TryGetNumberField(FString(Prefix) + TEXT("_max_speed"), D))          { Axis->MaxSpeed         = FMath::Max(0.0f, static_cast<float>(D)); ++AppliedCount; }
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
            Axis->InterpolationType = Parsed;
            ++AppliedCount;
        }
    };

    ApplyAxis(0, TEXT("x"));
    if (bTypeError)
    {
        return mcp_common::MakeErrorReply(TypeErrorMsg);
    }

    const FString YPrefix = TEXT("y");
    const bool bYProvided = Params->HasField(YPrefix + TEXT("_interpolation_time")) || Params->HasField(YPrefix + TEXT("_damping_ratio"))
        || Params->HasField(YPrefix + TEXT("_max_speed")) || Params->HasField(YPrefix + TEXT("_interpolation_type"));
    if (bIs1D)
    {
        if (bYProvided)
        {
            Notes.Add(TEXT("y_* ignored: this is a 1D blend space (only the X axis is smoothed)"));
        }
    }
    else
    {
        ApplyAxis(1, TEXT("y"));
        if (bTypeError)
        {
            return mcp_common::MakeErrorReply(TypeErrorMsg);
        }
    }

    if (AppliedCount == 0)
    {
        return mcp_common::MakeErrorReply(
            TEXT("No interpolation properties provided. Supply at least one of: x_interpolation_time/x_damping_ratio/"
                 "x_max_speed/x_interpolation_type (and y_* for 2D)"));
    }

    // Without this the RUNTIME does not see the blend space.
    RebuildBlendSpaceRuntimeData(BlendSpace);
    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(BlendSpace, SaveError);

    // Read the values back through the SAME reflection path so the round-trip is honest.
    auto AxisReadback = [&](int32 Index) -> TSharedPtr<FJsonObject>
    {
        const FInterpolationParameter* Axis = InterpProp->ContainerPtrToValuePtr<FInterpolationParameter>(BlendSpace, Index);
        return Axis ? InterpAxisJson(*Axis) : MakeShared<FJsonObject>();
    };

    UE_LOG(LogTemp, Verbose, TEXT("[set_blendspace_interpolation] %s -> %d field(s) applied (is_1d=%d)"),
        *BlendSpace->GetPathName(), AppliedCount, bIs1D ? 1 : 0);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("blend_space_path"), BlendSpace->GetPathName());
    Obj->SetBoolField(TEXT("is_1d"), bIs1D);
    Obj->SetObjectField(TEXT("x_axis"), AxisReadback(0));
    if (!bIs1D)
    {
        Obj->SetObjectField(TEXT("y_axis"), AxisReadback(1));
    }
    Obj->SetNumberField(TEXT("applied_count"), AppliedCount);
    Obj->SetArrayField(TEXT("notes"), mcp_common::StringsToJson(Notes));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("set_blendspace_interpolation"), SaveError);
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
// MCP:set_anim_retarget_source - Set a UAnimSequence's retarget source (and optionally its retarget reference-pose sequence)
//   @param anim_sequence_path - Object path of the UAnimSequence to mutate.
//   @param retarget_source - Retarget-source name (FName). Must name an entry in the owning skeleton's retarget-source map to RESOLVE at runtime, but the property write persists regardless. Use 'None' (or empty) to clear.
//   @param ref_pose_seq_path - Optional object path of a UAnimSequence used as the additive retarget reference pose (RefPoseSeq). Pass empty/'None' to clear it.
//   @returns { anim_sequence_path, retarget_source, ref_pose_seq, ref_pose_seq_set, ref_pose_seq_cleared, applied_count, notes, success, saved, save_error }
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

    const FScopedTransaction Transaction(LOCTEXT("SetAnimRetargetSource", "MCP: Set Anim Retarget Source"));
    Seq->Modify();

    if (bHasSource)
    {
        FString SourceStr;
        Params->TryGetStringField(TEXT("retarget_source"), SourceStr);
        SourceStr = SourceStr.TrimStartAndEnd();
        // Treat empty / "None" as a clear (NAME_None).
        const FName SourceName = (SourceStr.IsEmpty() || SourceStr.Equals(TEXT("None"), ESearchCase::IgnoreCase))
            ? NAME_None : FName(*SourceStr);
        Seq->RetargetSource = SourceName;
        ++AppliedCount;
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
        FString RefPosePath;
        Params->TryGetStringField(TEXT("ref_pose_seq_path"), RefPosePath);
        RefPosePath = RefPosePath.TrimStartAndEnd();
        if (RefPosePath.IsEmpty() || RefPosePath.Equals(TEXT("None"), ESearchCase::IgnoreCase))
        {
            Seq->RefPoseSeq = nullptr;
            bRefPoseCleared = true;
            ++AppliedCount;
        }
        else if (UAnimSequence* RefSeq = AnimAssetUtils::LoadAsset<UAnimSequence>(RefPosePath))
        {
            Seq->RefPoseSeq = RefSeq;
            bRefPoseSet = true;
            ++AppliedCount;
            Notes.Add(TEXT("ref_pose_seq (RefPoseSeq) is meaningful only when the additive base pose type is AnimScaled or AnimFrame"));
        }
        else
        {
            // Honest failure: a referenced ref-pose asset path that does not load is an error,
            // not a silent skip.
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("ref_pose_seq_path AnimSequence not found: %s"), *RefPosePath));
        }
    }

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Seq, SaveError);

    UE_LOG(LogTemp, Verbose, TEXT("[set_anim_retarget_source] %s -> retarget_source='%s' ref_pose_set=%d cleared=%d (%d field(s))"),
        *Seq->GetPathName(), *Seq->RetargetSource.ToString(), bRefPoseSet ? 1 : 0, bRefPoseCleared ? 1 : 0, AppliedCount);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("anim_sequence_path"), Seq->GetPathName());
    Obj->SetStringField(TEXT("retarget_source"), Seq->RetargetSource.ToString());
    Obj->SetStringField(TEXT("ref_pose_seq"), Seq->RefPoseSeq ? Seq->RefPoseSeq->GetPathName() : FString());
    Obj->SetBoolField(TEXT("ref_pose_seq_set"), bRefPoseSet);
    Obj->SetBoolField(TEXT("ref_pose_seq_cleared"), bRefPoseCleared);
    Obj->SetNumberField(TEXT("applied_count"), AppliedCount);
    Obj->SetArrayField(TEXT("notes"), mcp_common::StringsToJson(Notes));
    Obj->SetBoolField(TEXT("success"), true);
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
// MCP:set_anim_montage_skeleton - Rebind a UAnimMontage to a different USkeleton (clears existing source link + recomputes bone caches)
//   @param montage_path - Full asset path of the montage (e.g. '/Game/MyGame/AM_Example').
//   @param skeleton_path - Asset path of the new Skeleton (e.g. '/Engine/EngineMeshes/SkeletalCube_Skeleton').
//   @returns { montage_path, skeleton_path, prev_skeleton_path, applied, saved, message }
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

    const FScopedTransaction Transaction(LOCTEXT("SetAnimMontageSkeleton", "Set Anim Montage Skeleton"));
    Montage->Modify();
    Montage->SetSkeleton(NewSkeleton);
    Montage->RefreshCacheData();

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Montage, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("montage_path"), Montage->GetPathName());
    Obj->SetStringField(TEXT("skeleton_path"), NewSkeleton->GetPathName());
    Obj->SetStringField(TEXT("prev_skeleton_path"), PrevSkeletonPath);
    Obj->SetBoolField(TEXT("applied"), true);
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Montage skeleton set"), bSaved, SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("set_anim_montage_skeleton"));
}

// ---------------------------------------------------------------------------
// set_anim_montage_preview_mesh — assign the preview USkeletalMesh on a montage.
// UAnimMontage::SetPreviewMesh (AnimationAsset.h, ENGINE_API virtual override)
// writes PreviewSkeletalMesh UPROPERTY; the second arg bMarkAsDirty=true defaults
// to dirty-marking, but we still MarkPackageDirty explicitly so editor save flows
// pick the change up the same way as the rest of this file.
// ---------------------------------------------------------------------------
// MCP:set_anim_montage_preview_mesh - Set the editor preview USkeletalMesh of a UAnimMontage (SetPreviewMesh, one undo step) and save the montage; it only changes what the asset editor shows, and the mesh skeleton is not checked against the montage. Returns mesh_path, prev_mesh_path and applied.
//   @param montage_path - Full asset path of the montage (e.g. '/Game/MyGame/AM_Example').
//   @param mesh_path - Asset path of the preview USkeletalMesh (e.g. '/Engine/EngineMeshes/SkeletalCube').
//   @returns { montage_path, mesh_path, prev_mesh_path, applied, saved, message }
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

    const FString PrevMeshPath = Montage->GetPreviewMesh() ? Montage->GetPreviewMesh()->GetPathName() : FString();

    const FScopedTransaction Transaction(LOCTEXT("SetAnimMontagePreviewMesh", "Set Anim Montage Preview Mesh"));
    Montage->Modify();
    Montage->SetPreviewMesh(NewMesh, /*bMarkAsDirty=*/true);

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Montage, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("montage_path"), Montage->GetPathName());
    Obj->SetStringField(TEXT("mesh_path"), NewMesh->GetPathName());
    Obj->SetStringField(TEXT("prev_mesh_path"), PrevMeshPath);
    Obj->SetBoolField(TEXT("applied"), true);
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Montage preview mesh set"), bSaved, SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("set_anim_montage_preview_mesh"));
}

// ---------------------------------------------------------------------------
// refresh_anim_montage_cache — re-run UAnimSequenceBase::RefreshCacheData().
// ENGINE_API virtual AnimSequenceBase.h; safe + idempotent (recomputes
// NotifyTracks, SequenceLength and bone-cache without mutating authored data).
// ---------------------------------------------------------------------------
// MCP:refresh_anim_montage_cache - Refresh derived cache data on a UAnimMontage (NotifyTracks / SequenceLength / bone-cache). Idempotent.
//   @param montage_path - Full asset path of the montage (e.g. '/Game/MyGame/AM_Example').
//   @returns { montage_path, refreshed, sequence_length, slot_count, section_count, saved, message }
TSharedPtr<FJsonObject> HandleRefreshAnimMontageCache(const TSharedPtr<FJsonObject>& Params)
{
    FString MontagePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("montage_path"), MontagePath)) { return Missing; }

    UAnimMontage* Montage = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadOrError(MontagePath, TEXT("AnimMontage"), Montage)) { return Error; }

    Montage->RefreshCacheData();

    // Persist so the recomputed SequenceLength / NotifyTracks land on disk too.
    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Montage, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("montage_path"), Montage->GetPathName());
    Obj->SetBoolField(TEXT("refreshed"), true);
    Obj->SetNumberField(TEXT("sequence_length"), Montage->GetPlayLength());
    Obj->SetNumberField(TEXT("slot_count"), Montage->SlotAnimTracks.Num());
    Obj->SetNumberField(TEXT("section_count"), Montage->CompositeSections.Num());
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Montage cache refreshed"), bSaved, SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("refresh_anim_montage_cache"));
}

// ---------------------------------------------------------------------------
// rename_montage_section — overwrite SectionName on an existing FCompositeSection by index.
// FCompositeSection::SectionName (AnimMontage.h) is a public UPROPERTY FName;
// surgical write under Modify + RefreshCacheData keeps NotifyTracks in sync.
// ---------------------------------------------------------------------------
// MCP:rename_montage_section - Rename an existing FCompositeSection on a UAnimMontage by zero-based section_index
//   @param montage_path - Full asset path of the montage (e.g. '/Game/MyGame/AM_Example').
//   @param section_index - Zero-based index into CompositeSections (use get_montage_info first to list).
//   @param new_name - New FName for the section.
//   @returns { montage_path, section_index, old_name, new_name, section_count, sections, applied, saved, message }
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

    const FScopedTransaction Transaction(LOCTEXT("RenameMontageSection", "Rename Montage Section"));
    Montage->Modify();
    Montage->CompositeSections[Index].SectionName = NewFName;
    Montage->RefreshCacheData();

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Montage, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("montage_path"), Montage->GetPathName());
    Obj->SetNumberField(TEXT("section_index"), Index);
    Obj->SetStringField(TEXT("old_name"), OldName.ToString());
    Obj->SetStringField(TEXT("new_name"), NewName);
    Obj->SetNumberField(TEXT("section_count"), Montage->CompositeSections.Num());
    Obj->SetArrayField(TEXT("sections"), CollectSectionNames(Montage));
    Obj->SetBoolField(TEXT("applied"), true);
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Montage section renamed"), bSaved, SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("rename_montage_section"));
}

// ---------------------------------------------------------------------------
// set_montage_section_start_time — overwrite StartTime on an FCompositeSection by index.
// FCompositeSection::StartTime is a public UPROPERTY float (AnimMontage.h);
// write + RefreshCacheData keeps the section's LinkValue / derived bookkeeping in sync.
// ---------------------------------------------------------------------------
// MCP:set_montage_section_start_time - Set the StartTime on an existing FCompositeSection by zero-based section_index
//   @param montage_path - Full asset path of the montage (e.g. '/Game/MyGame/AM_Example').
//   @param section_index - Zero-based index into CompositeSections.
//   @param start_time - Absolute section start time in seconds.
//   @returns { montage_path, section_index, start_time, applied, saved, message }
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

    const FScopedTransaction Transaction(LOCTEXT("SetMontageSectionStartTime", "Set Montage Section Start Time"));
    Montage->Modify();
    // FCompositeSection::StartTime is deprecated in favor of FAnimLinkableElement::SetTime/GetTime.
    // Use the inherited setter with EAnimLinkMethod::Absolute (the default; the alternative Relative/Proportional require a valid SegmentBeginTime).
    Montage->CompositeSections[Index].SetTime(static_cast<float>(StartTime), EAnimLinkMethod::Absolute);
    Montage->RefreshCacheData();

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Montage, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("montage_path"), Montage->GetPathName());
    Obj->SetNumberField(TEXT("section_index"), Index);
    Obj->SetNumberField(TEXT("start_time"), static_cast<double>(Montage->CompositeSections[Index].GetTime(EAnimLinkMethod::Absolute)));
    Obj->SetBoolField(TEXT("applied"), true);
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Montage section start time set"), bSaved, SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("set_montage_section_start_time"));
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
// MCP:set_montage_segment_anim_sequence - Rebind an existing FAnimSegment on a slot track to a different UAnimSequence (resets segment start/end times to the new sequence's range)
//   @param montage_path - Full asset path of the montage (e.g. '/Game/MyGame/AM_Example').
//   @param slot_name - Name of the slot track containing the segment (e.g. 'DefaultSlot').
//   @param segment_index - Zero-based index into the slot track's AnimSegments.
//   @param anim_sequence_path - Object path of the UAnimSequence to reference (must share the montage's skeleton).
//   @returns { montage_path, slot_name, segment_index, anim_sequence_path, anim_start_time, anim_end_time, applied, saved, message }
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

    const FScopedTransaction Transaction(LOCTEXT("SetMontageSegmentAnimSequence", "Set Montage Segment Anim Sequence"));
    Montage->Modify();
    // bInitialize=true resets AnimStartTime=0 / AnimEndTime=AnimSequenceLength — the
    // supported way to switch a segment's underlying animation.
    Segment->SetAnimReference(NewAnim, /*bInitialize=*/true);
    Montage->RefreshCacheData();

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Montage, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("montage_path"), Montage->GetPathName());
    Obj->SetStringField(TEXT("slot_name"), SlotName);
    Obj->SetNumberField(TEXT("segment_index"), Index);
    Obj->SetStringField(TEXT("anim_sequence_path"), NewAnim->GetPathName());
    Obj->SetNumberField(TEXT("anim_start_time"), static_cast<double>(Segment->AnimStartTime));
    Obj->SetNumberField(TEXT("anim_end_time"), static_cast<double>(Segment->AnimEndTime));
    Obj->SetBoolField(TEXT("applied"), true);
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Montage segment anim reference set"), bSaved, SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("set_montage_segment_anim_sequence"));
}

// ---------------------------------------------------------------------------
// set_montage_segment_start_time — overwrite AnimStartTime on an existing FAnimSegment.
// FAnimSegment::AnimStartTime (AnimCompositeBase.h) is a BlueprintReadOnly UPROPERTY
// float; raw access is the canonical C++ write path (HandleAddMontageSegment writes the
// same member via FAnimSegment::SetAnimReference with bInitialize=true, which also touches
// AnimEndTime). Here we only move the START of an already-bound segment — the user keeps
// AnimEndTime as-is to preserve the segment's play-length intent.
// ---------------------------------------------------------------------------
// MCP:set_montage_segment_start_time - Set the AnimStartTime on an existing FAnimSegment by (slot_name, segment_index). AnimEndTime is left untouched.
//   @param montage_path - Full asset path of the montage (e.g. '/Game/MyGame/AM_Example').
//   @param slot_name - Name of the slot track containing the segment.
//   @param segment_index - Zero-based index into the slot track's AnimSegments.
//   @param start_time - New AnimStartTime in seconds (clamped to [0, AnimEndTime]).
//   @returns { montage_path, slot_name, segment_index, start_time, anim_end_time, applied, saved, message }
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

    const FScopedTransaction Transaction(LOCTEXT("SetMontageSegmentStartTime", "Set Montage Segment Start Time"));
    Montage->Modify();
    Segment->AnimStartTime = ClampedStart;
    Montage->RefreshCacheData();

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Montage, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("montage_path"), Montage->GetPathName());
    Obj->SetStringField(TEXT("slot_name"), SlotName);
    Obj->SetNumberField(TEXT("segment_index"), Index);
    Obj->SetNumberField(TEXT("start_time"), static_cast<double>(Segment->AnimStartTime));
    Obj->SetNumberField(TEXT("anim_end_time"), static_cast<double>(Segment->AnimEndTime));
    if (!FMath::IsNearlyEqual(NewStart, ClampedStart))
    {
        Obj->SetStringField(TEXT("note"), FString::Printf(
            TEXT("Requested start_time=%f was clamped to AnimEndTime=%f"), NewStart, Segment->AnimEndTime));
    }
    Obj->SetBoolField(TEXT("applied"), true);
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Montage segment start time set"), bSaved, SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("set_montage_segment_start_time"));
}

// ---------------------------------------------------------------------------
// collapse_montage_segments — FAnimTrack::CollapseAnimSegments() on a slot track.
// AnimCompositeBase.h — moves AnimSegments so there are no gaps between one
// finishing and the next starting (preserves order). WITH_EDITOR-only.
// ---------------------------------------------------------------------------
// MCP:collapse_montage_segments - Collapse a montage slot track's segments end-to-end (no gaps, order preserved)
//   @param montage_path - Full asset path of the montage (e.g. '/Game/MyGame/AM_Example').
//   @param slot_name - Name of the slot track to collapse.
//   @returns { montage_path, slot_name, segment_count, applied, saved, message }
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

    const FScopedTransaction Transaction(LOCTEXT("CollapseMontageSegments", "Collapse Montage Segments"));
    Montage->Modify();
    SlotTrack->AnimTrack.CollapseAnimSegments();
    Montage->RefreshCacheData();

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Montage, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("montage_path"), Montage->GetPathName());
    Obj->SetStringField(TEXT("slot_name"), SlotName);
    Obj->SetNumberField(TEXT("segment_count"), SlotTrack->AnimTrack.AnimSegments.Num());
    Obj->SetBoolField(TEXT("applied"), true);
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Montage segments collapsed"), bSaved, SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("collapse_montage_segments"));
}

// ---------------------------------------------------------------------------
// sort_montage_segments — FAnimTrack::SortAnimSegments() on a slot track.
// AnimCompositeBase.h — sorts AnimSegments by StartTime ascending.
// WITH_EDITOR-only.
// ---------------------------------------------------------------------------
// MCP:sort_montage_segments - Sort a montage slot track's segments by start time ascending
//   @param montage_path - Full asset path of the montage (e.g. '/Game/MyGame/AM_Example').
//   @param slot_name - Name of the slot track to sort.
//   @returns { montage_path, slot_name, segment_count, applied, saved, message }
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

    const FScopedTransaction Transaction(LOCTEXT("SortMontageSegments", "Sort Montage Segments"));
    Montage->Modify();
    SlotTrack->AnimTrack.SortAnimSegments();
    Montage->RefreshCacheData();

    FString SaveError;
    const bool bSaved = AnimAuthoring_SavePackageOf(Montage, SaveError);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("montage_path"), Montage->GetPathName());
    Obj->SetStringField(TEXT("slot_name"), SlotName);
    Obj->SetNumberField(TEXT("segment_count"), SlotTrack->AnimTrack.AnimSegments.Num());
    Obj->SetBoolField(TEXT("applied"), true);
    Obj->SetStringField(TEXT("message"), AnimAuthoring_SaveMessage(TEXT("Montage segments sorted"), bSaved, SaveError));
    return mcp_common::MakeSavedReply(Obj, bSaved, TEXT("sort_montage_segments"));
}

#undef LOCTEXT_NAMESPACE

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_anim_bone_track"), &HandleAddAnimBoneTrack },
    { TEXT("add_anim_composite_segment"), &HandleAddAnimCompositeSegment },
    { TEXT("add_anim_curve_name"), &HandleAddAnimCurveName },
    { TEXT("add_anim_float_curve"), &HandleAddAnimFloatCurve },
    { TEXT("add_anim_notify"), &HandleAddAnimNotify },
    { TEXT("add_anim_notify_name"), &HandleAddAnimNotifyName },
    { TEXT("add_anim_notify_state"), &HandleAddAnimNotifyState },
    { TEXT("add_anim_sync_marker"), &HandleAddAnimSyncMarker },
    { TEXT("add_anim_transform_curve"), &HandleAddAnimTransformCurve },
    { TEXT("add_blendspace_sample"), &HandleAddBlendspaceSample },
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
    { TEXT("delete_montage_section"), &HandleDeleteMontageSection },
    { TEXT("duplicate_anim_sequence"), &HandleDuplicateAnimSequence },
    { TEXT("edit_anim_notify"), &HandleEditAnimNotify },
    { TEXT("edit_blendspace_sample"), &HandleEditBlendspaceSample },
    { TEXT("get_anim_asset_info"), &HandleGetAnimAssetInfo },
    { TEXT("get_anim_curve_data"), &HandleGetAnimCurveData },
    { TEXT("get_anim_curve_names"), &HandleGetAnimCurveNames },
    { TEXT("get_anim_notifies"), &HandleGetAnimNotifies },
    { TEXT("get_anim_sequence_info"), &HandleGetAnimSequenceInfo },
    { TEXT("get_blendspace_samples"), &HandleGetBlendspaceSamples },
    { TEXT("get_montage_info"), &HandleGetMontageInfo },
    { TEXT("get_notify_track_names"), &HandleGetNotifyTrackNames },
    { TEXT("link_montage_sections"), &HandleLinkMontageSections },
    { TEXT("refresh_anim_montage_cache"), &HandleRefreshAnimMontageCache },
    { TEXT("reimport_anim_sequence"), &HandleReimportAnimSequence },
    { TEXT("remove_anim_bone_track"), &HandleRemoveAnimBoneTrack },
    { TEXT("remove_anim_composite_segment"), &HandleRemoveAnimCompositeSegment },
    { TEXT("remove_anim_curve"), &HandleRemoveAnimCurve },
    { TEXT("remove_anim_notify"), &HandleRemoveAnimNotify },
    { TEXT("remove_anim_sync_marker"), &HandleRemoveAnimSyncMarker },
    { TEXT("remove_blendspace_sample"), &HandleRemoveBlendspaceSample },
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
    { TEXT("set_anim_curve_interp_mode"), &HandleSetAnimCurveInterpMode },
    { TEXT("set_anim_curve_keys"), &HandleSetAnimCurveKeys },
    { TEXT("set_anim_frame_rate"), &HandleSetAnimFrameRate },
    { TEXT("set_anim_montage_preview_mesh"), &HandleSetAnimMontagePreviewMesh },
    { TEXT("set_anim_montage_rate_scale"), &HandleSetAnimMontageRateScale },
    { TEXT("set_anim_montage_skeleton"), &HandleSetAnimMontageSkeleton },
    { TEXT("set_anim_number_of_frames"), &HandleSetAnimNumberOfFrames },
    { TEXT("set_anim_retarget_source"), &HandleSetAnimRetargetSource },
    { TEXT("set_anim_root_motion"), &HandleSetAnimRootMotion },
    { TEXT("set_blendspace_interpolation"), &HandleSetBlendSpaceInterpolation },
    { TEXT("set_blendspace_properties"), &HandleSetBlendSpaceProperties },
    { TEXT("set_montage_blend_times"), &HandleSetMontageBlendTimes },
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
