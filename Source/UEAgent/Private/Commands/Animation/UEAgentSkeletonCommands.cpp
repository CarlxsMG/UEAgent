#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Animation/AnimAssetUtils.h"

#include "Editor.h"
#include "EditorAssetLibrary.h"

#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"

#include "Animation/Skeleton.h"                 // Engine: USkeleton, FVirtualBone, FAnimSlotGroup
#include "Animation/BlendProfile.h"             // Engine: UBlendProfile (SetBoneBlendScale)
#include "Engine/SkeletalMesh.h"                // Engine: USkeletalMesh (for is_skeleton_shared)
#include "Engine/SkeletalMeshSocket.h"          // Engine: USkeletalMeshSocket
#include "ReferenceSkeleton.h"                  // Engine: FReferenceSkeleton (GetNum / GetBoneName) + FReferenceSkeletonModifier
#include "AssetRegistry/IAssetRegistry.h"       // IAssetRegistry::GetAssets(FARFilter)

/**
 * Skeleton editing commands.
 *
 * They are meant to run on a /Game duplicate of an engine skeleton made by create_editable_skeleton
 * (UEditorAssetLibrary::DuplicateAsset; default source /Engine/EngineMeshes/SkeletalCube_Skeleton), so engine
 * content is never mutated in place. Every command is headless-safe and goes through the engine's own API:
 *   - virtual bones, slot groups, curve names and blend profiles: USkeleton's own API, plus
 *     UBlendProfile::SetBoneBlendScale for per-bone blend scales;
 *   - sockets: NewObject<USkeletalMeshSocket>(Skeleton) + Skeleton->Sockets, the editor's own path;
 *   - the bone tree: FReferenceSkeletonModifier (Add, Remove, Rename, UpdateRefPoseTransform, SetParent), the
 *     path USkeleton itself uses; its destructor rebuilds the reference skeleton and broadcasts the change
 *     without traversing assets;
 *   - is_skeleton_shared: an Asset Registry FARFilter for the skeletal meshes bound to the skeleton.
 *
 * Each mutation calls Modify() and MarkPackageDirty, then saves the package.
 */
namespace UEAgentSkeletonCommands
{

// MCP-DOMAIN: Animation

namespace
{
    const TCHAR* kDefaultSourceSkeleton = TEXT("/Engine/EngineMeshes/SkeletalCube_Skeleton");

    /** A reply payload that starts with the skeleton's asset_path. */
    TSharedPtr<FJsonObject> Skeleton_NewPayload(const USkeleton* Skeleton)
    {
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetStringField(TEXT("asset_path"), Skeleton->GetPathName());
        return Resp;
    }

    /** The payload of a removal: the entry named under NameField, removed, and already_absent as its opposite. */
    TSharedPtr<FJsonObject> Skeleton_RemovalPayload(const USkeleton* Skeleton, const TCHAR* NameField, const FString& Name, bool bRemoved)
    {
        TSharedPtr<FJsonObject> Resp = Skeleton_NewPayload(Skeleton);
        Resp->SetStringField(NameField, Name);
        Resp->SetBoolField(TEXT("removed"), bRemoved);
        Resp->SetBoolField(TEXT("already_absent"), !bRemoved);
        return Resp;
    }

    /** Marks the skeleton's package dirty and saves it; false, with OutError, when the save fails. */
    bool Skeleton_MarkDirtyAndSave(USkeleton* Skeleton, FString& OutError)
    {
        Skeleton->MarkPackageDirty();
        return mcp_common::SaveAssetPackage(Skeleton->GetOutermost(), Skeleton, OutError);
    }
}

// ---------------------------------------------------------------------------
// create_editable_skeleton
// ---------------------------------------------------------------------------
// MCP:create_editable_skeleton - Create an editable /Game skeleton by duplicating an engine skeleton
//   @param package_path - UE content folder, e.g. /Game/MyGame
//   @param name - Asset name for the new skeleton
//   @param source_skeleton - Engine skeleton to duplicate, defaults to SkeletalCube_Skeleton
//   @returns { asset_path: string, name: string, source: string, bone_count: int, created: bool, saved: bool, save_error: string }
TSharedPtr<FJsonObject> HandleCreateEditableSkeleton(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath, Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("package_path"), PackagePath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LongPackagePathError(PackagePath)) { return Error; }

    FString SourceSkeleton = kDefaultSourceSkeleton;
    Params->TryGetStringField(TEXT("source_skeleton"), SourceSkeleton);

    FString Folder, FullPath;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::JoinAssetPath(PackagePath, Name, Folder, FullPath)) { return Error; }

    // Idempotent: if it already exists, load it and return a stable payload
    // (same shape as a fresh create, created=false).
    auto BuildPayload = [&](USkeleton* Skeleton, bool bCreated) -> TSharedPtr<FJsonObject>
    {
        TSharedPtr<FJsonObject> Resp = Skeleton_NewPayload(Skeleton);
        Resp->SetStringField(TEXT("name"), Name);
        Resp->SetStringField(TEXT("source"), SourceSkeleton);
        Resp->SetNumberField(TEXT("bone_count"), Skeleton->GetReferenceSkeleton().GetNum());
        Resp->SetBoolField(TEXT("created"), bCreated);
        return Resp;
    };

    if (UEditorAssetLibrary::DoesAssetExist(FullPath))
    {
        USkeleton* Existing = mcp_common::LoadAssetOrSoftPath<USkeleton>(FullPath);
        if (Existing)
        {
            return mcp_common::MakeOkReply(BuildPayload(Existing, false));
        }
    }

    if (!UEditorAssetLibrary::DoesAssetExist(SourceSkeleton))
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Source skeleton does not exist: %s"), *SourceSkeleton));

    UObject* Duplicated = UEditorAssetLibrary::DuplicateAsset(SourceSkeleton, FullPath);
    USkeleton* NewSkeleton = Cast<USkeleton>(Duplicated);
    if (!NewSkeleton)
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("DuplicateAsset failed for %s -> %s (or source is not a USkeleton)"),
                *SourceSkeleton, *FullPath));

    FString SaveErr;
    const bool bSaved = mcp_common::SaveAssetPackage(NewSkeleton->GetOutermost(), NewSkeleton, SaveErr);

    TSharedPtr<FJsonObject> Resp = BuildPayload(NewSkeleton, true);
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("create_editable_skeleton"), SaveErr);
}

// ---------------------------------------------------------------------------
// add_virtual_bone
// ---------------------------------------------------------------------------
// MCP:add_virtual_bone - Add a virtual bone from a source bone to a target bone of a USkeleton asset, named "VB <source>_<target>" by the engine, then save. Bone names are not checked against the skeleton; an existing pair returns that bone with already_exists true. Returns virtual_bone_name and virtual_bone_count.
//   @param skeleton_path - USkeleton asset path, e.g. "/Game/Characters/SK_Hero_Skeleton".
//   @param source_bone - Bone the virtual bone starts from (default the first bone, usually the root); not validated.
//   @param target_bone - Bone the virtual bone points to (default the first bone that differs from source_bone); not validated.
//   @returns { asset_path: string, virtual_bone_name: string, source_bone: string, target_bone: string, virtual_bone_count: int, saved: bool, already_exists: bool }
TSharedPtr<FJsonObject> HandleAddVirtualBone(const TSharedPtr<FJsonObject>& Params)
{
    FString SkeletonPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("skeleton_path"), SkeletonPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    USkeleton* Skeleton = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadFlexibleOrError(SkeletonPath, TEXT("skeleton"), Skeleton)) { return Error; }

    const FReferenceSkeleton& RefSkel = Skeleton->GetReferenceSkeleton();
    const int32 NumBones = RefSkel.GetNum();
    if (NumBones < 2)
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Skeleton has %d bones; need at least 2 for a virtual bone"), NumBones));

    // Source/target: use the caller's bones, otherwise auto-pick the first two
    // distinct real bones from the reference skeleton.
    FString SourceStr, TargetStr;
    FName SourceBone, TargetBone;
    if (Params->TryGetStringField(TEXT("source_bone"), SourceStr) && !SourceStr.IsEmpty())
    {
        SourceBone = FName(*SourceStr);
    }
    else
    {
        SourceBone = RefSkel.GetBoneName(0);
    }
    if (Params->TryGetStringField(TEXT("target_bone"), TargetStr) && !TargetStr.IsEmpty())
    {
        TargetBone = FName(*TargetStr);
    }
    else
    {
        // Pick the first bone whose name differs from the source.
        TargetBone = RefSkel.GetBoneName(1);
        for (int32 i = 0; i < NumBones; ++i)
        {
            const FName Candidate = RefSkel.GetBoneName(i);
            if (Candidate != SourceBone)
            {
                TargetBone = Candidate;
                break;
            }
        }
    }

    if (SourceBone == TargetBone)
        return mcp_common::MakeErrorReply(
            TEXT("source_bone and target_bone must be different"));

    // Both outcomes report the virtual bone between SourceBone and TargetBone.
    auto BuildPayload = [&](FName VirtualBoneName) -> TSharedPtr<FJsonObject>
    {
        TSharedPtr<FJsonObject> Resp = Skeleton_NewPayload(Skeleton);
        Resp->SetStringField(TEXT("virtual_bone_name"), VirtualBoneName.ToString());
        Resp->SetStringField(TEXT("source_bone"), SourceBone.ToString());
        Resp->SetStringField(TEXT("target_bone"), TargetBone.ToString());
        Resp->SetNumberField(TEXT("virtual_bone_count"), Skeleton->GetVirtualBones().Num());
        return Resp;
    };

    // Idempotent: check if virtual bone already exists between these bones.
    for (const FVirtualBone& VB : Skeleton->GetVirtualBones())
    {
        if (VB.SourceBoneName == SourceBone && VB.TargetBoneName == TargetBone)
        {
            // Virtual bone already exists — return success (idempotent)
            TSharedPtr<FJsonObject> Resp = BuildPayload(VB.VirtualBoneName);
            Resp->SetBoolField(TEXT("saved"), true);
            Resp->SetBoolField(TEXT("already_exists"), true);
            return mcp_common::MakeOkReply(Resp);
        }
    }

    Skeleton->Modify();

    FName OutName;
    const bool bAdded = Skeleton->AddNewVirtualBone(SourceBone, TargetBone, OutName);
    if (!bAdded)
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("AddNewVirtualBone failed for %s -> %s (already exists?)"),
                *SourceBone.ToString(), *TargetBone.ToString()));

    FString SaveErr;
    const bool bSaved = Skeleton_MarkDirtyAndSave(Skeleton, SaveErr);
    return mcp_common::MakeSavedReply(BuildPayload(OutName), bSaved, TEXT("add_virtual_bone"), SaveErr);
}

// ---------------------------------------------------------------------------
// add_anim_slot_group
// ---------------------------------------------------------------------------
// MCP:add_anim_slot_group - Add an animation slot group (the groups montage slots belong to) to a USkeleton asset and save it. Idempotent: an existing group returns added false and changes nothing, but the skeleton is still saved; a failed save returns success false. Returns added and slot_group_count.
//   @param skeleton_path - USkeleton asset path, e.g. "/Game/Characters/SK_Hero_Skeleton".
//   @param group_name - Name of the slot group to add, e.g. "WeaponGroup".
//   @returns { asset_path: string, group_name: string, added: bool, slot_group_count: int, saved: bool }
TSharedPtr<FJsonObject> HandleAddAnimSlotGroup(const TSharedPtr<FJsonObject>& Params)
{
    FString SkeletonPath, GroupName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("skeleton_path"), SkeletonPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("group_name"), GroupName)) { return Missing; }

    USkeleton* Skeleton = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadFlexibleOrError(SkeletonPath, TEXT("skeleton"), Skeleton)) { return Error; }

    Skeleton->Modify();

    const FName Group(*GroupName);
    // Returns true if added, false if it already existed (idempotent re-run).
    const bool bAdded = Skeleton->AddSlotGroupName(Group);

    FString SaveErr;
    const bool bSaved = Skeleton_MarkDirtyAndSave(Skeleton, SaveErr);

    TSharedPtr<FJsonObject> Resp = Skeleton_NewPayload(Skeleton);
    Resp->SetStringField(TEXT("group_name"), GroupName);
    Resp->SetBoolField(TEXT("added"), bAdded);
    Resp->SetNumberField(TEXT("slot_group_count"), Skeleton->GetSlotGroups().Num());
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("add_anim_slot_group"), SaveErr);
}

// ---------------------------------------------------------------------------
// add_socket_to_skeleton_asset
// ---------------------------------------------------------------------------
// MCP:add_socket_to_skeleton_asset - Add a socket to a USkeleton asset on a bone (the root by default) at a bone-relative location and rotation, then save; an existing socket of that name is updated in place (already_existed true). A failed save still returns success, so check saved. Mesh-only sockets: add_skeletal_mesh_socket.
//   @param skeleton_path - USkeleton asset path, e.g. "/Game/Characters/SK_Hero_Skeleton".
//   @param socket_name - Name of the socket; an existing socket with this name is updated instead of duplicated.
//   @param bone_name - Bone to attach the socket to; must exist in the skeleton (default the root bone).
//   @param location - Socket location relative to its bone as [x, y, z] (default 0,0,0)
//   @param rotation - Socket rotation relative to its bone as [pitch, yaw, roll] (default 0,0,0)
//   @returns { asset_path: string, socket_name: string, bone_name: string, socket_count: int, added: bool, already_existed: bool, saved: bool }
TSharedPtr<FJsonObject> HandleAddSkeletonSocket(const TSharedPtr<FJsonObject>& Params)
{
    FString SkeletonPath, SocketName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("skeleton_path"), SkeletonPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("socket_name"), SocketName)) { return Missing; }

    USkeleton* Skeleton = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadFlexibleOrError(SkeletonPath, TEXT("skeleton"), Skeleton)) { return Error; }

    const FReferenceSkeleton& RefSkel = Skeleton->GetReferenceSkeleton();
    const int32 NumBones = RefSkel.GetNum();
    if (NumBones < 1)
        return mcp_common::MakeErrorReply(TEXT("Skeleton has no bones to attach a socket to"));

    // Bone: use the caller's bone (validated against the ref skeleton),
    // otherwise default to the root bone.
    FString BoneStr;
    FName BoneName;
    if (Params->TryGetStringField(TEXT("bone_name"), BoneStr) && !BoneStr.IsEmpty())
    {
        BoneName = FName(*BoneStr);
        if (RefSkel.FindBoneIndex(BoneName) == INDEX_NONE)
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Bone '%s' not found in skeleton"), *BoneStr));
    }
    else
    {
        BoneName = RefSkel.GetBoneName(0);
    }

    // Optional socket transform relative to its bone (default identity).
    const FVector RelLocation = mcp_common::ReadJsonVector(Params, TEXT("location"));
    const FRotator RelRotation = mcp_common::ReadJsonRotator(Params, TEXT("rotation"));

    // Both outcomes report the socket, the save result and, when the save failed, its error.
    auto BuildPayload = [&](const USkeletalMeshSocket* Socket, bool bAdded, bool bSaved, const FString& SaveErr) -> TSharedPtr<FJsonObject>
    {
        TSharedPtr<FJsonObject> Resp = Skeleton_NewPayload(Skeleton);
        Resp->SetStringField(TEXT("socket_name"), Socket->SocketName.ToString());
        Resp->SetStringField(TEXT("bone_name"), Socket->BoneName.ToString());
        Resp->SetNumberField(TEXT("socket_count"), Skeleton->Sockets.Num());
        Resp->SetBoolField(TEXT("added"), bAdded);
        Resp->SetBoolField(TEXT("already_existed"), !bAdded);
        mcp_common::AddSaveOutcome(Resp, bSaved, SaveErr);
        return Resp;
    };

    const FName SocketFName(*SocketName);
    // Idempotent: if a socket with this name already exists, update its bone and
    // transform in place and return its data.
    if (USkeletalMeshSocket* Existing = Skeleton->FindSocket(SocketFName))
    {
        Skeleton->Modify();
        Existing->BoneName = BoneName;
        Existing->RelativeLocation = RelLocation;
        Existing->RelativeRotation = RelRotation;

        FString SaveErr;
        const bool bSaved = Skeleton_MarkDirtyAndSave(Skeleton, SaveErr);
        return mcp_common::MakeOkReply(BuildPayload(Existing, /*bAdded=*/false, bSaved, SaveErr));
    }

    Skeleton->Modify();

    // Mirrors FEditableSkeleton::HandleAddSocket: NewObject outered to the
    // skeleton, set SocketName/BoneName, then push into Skeleton->Sockets.
    USkeletalMeshSocket* NewSocket = NewObject<USkeletalMeshSocket>(Skeleton);
    if (!NewSocket)
        return mcp_common::MakeErrorReply(TEXT("Failed to create USkeletalMeshSocket"));

    NewSocket->SocketName = SocketFName;
    NewSocket->BoneName = BoneName;
    NewSocket->RelativeLocation = RelLocation;
    NewSocket->RelativeRotation = RelRotation;
    Skeleton->Sockets.Add(NewSocket);

    FString SaveErr;
    const bool bSaved = Skeleton_MarkDirtyAndSave(Skeleton, SaveErr);
    return mcp_common::MakeOkReply(BuildPayload(NewSocket, /*bAdded=*/true, bSaved, SaveErr));
}

// ---------------------------------------------------------------------------
// get_skeleton_info
// ---------------------------------------------------------------------------
// MCP:get_skeleton_asset_info - Read a summary of a USkeleton asset: bone_count, the first 10 bone names only, every virtual bone with its source and target, slot_group_count and socket_count. Read-only; for up to 100 bone names use get_skeleton_info with load_full_data true.
//   @param skeleton_path - USkeleton asset path, e.g. "/Game/Characters/SK_Hero_Skeleton".
//   @returns { asset_path: string, bone_count: int, bones: [string], virtual_bone_count: int, virtual_bone_names: [string], virtual_bones: [{ name: string, source_bone: string, target_bone: string }], slot_group_count: int, socket_count: int }
TSharedPtr<FJsonObject> HandleGetSkeletonInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString SkeletonPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("skeleton_path"), SkeletonPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    USkeleton* Skeleton = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadFlexibleOrError(SkeletonPath, TEXT("skeleton"), Skeleton)) { return Error; }

    const FReferenceSkeleton& RefSkel = Skeleton->GetReferenceSkeleton();
    const int32 NumBones = RefSkel.GetNum();

    // First ~10 bone names.
    TArray<TSharedPtr<FJsonValue>> Bones;
    const int32 Limit = FMath::Min(NumBones, 10);
    for (int32 i = 0; i < Limit; ++i)
    {
        Bones.Add(MakeShared<FJsonValueString>(RefSkel.GetBoneName(i).ToString()));
    }

    // Virtual bones, read-only: walk Skeleton->GetVirtualBones() once and populate TWO fields,
    // deliberately kept separate
    // instead of overloading one name with two shapes (a field named the same but returning a
    // different type depending on the tool is a silent trap the plugin avoids everywhere):
    //   - virtual_bone_names: [string] -- flat names, SAME shape as remove_virtual_bone's field
    //     of the same name, kept for parity across the plugin (a caller that already does
    //     `for n in resp["virtual_bone_names"]` against remove_virtual_bone gets the same shape
    //     here).
    //   - virtual_bones: [{name, source_bone, target_bone}] -- the per-bone detail.
    // get_skeleton_info (in UEAgentAnimationCommands.cpp) intentionally does NOT surface
    // virtual bones -- this is the one tool that does, avoiding a second overlapping surface.
    TArray<TSharedPtr<FJsonValue>> VirtualBoneNames;
    TArray<TSharedPtr<FJsonValue>> VirtualBones;
    for (const FVirtualBone& VB : Skeleton->GetVirtualBones())
    {
        VirtualBoneNames.Add(MakeShared<FJsonValueString>(VB.VirtualBoneName.ToString()));

        TSharedPtr<FJsonObject> VBObj = MakeShared<FJsonObject>();
        VBObj->SetStringField(TEXT("name"), VB.VirtualBoneName.ToString());
        VBObj->SetStringField(TEXT("source_bone"), VB.SourceBoneName.ToString());
        VBObj->SetStringField(TEXT("target_bone"), VB.TargetBoneName.ToString());
        VirtualBones.Add(MakeShared<FJsonValueObject>(VBObj));
    }

    TSharedPtr<FJsonObject> Resp = Skeleton_NewPayload(Skeleton);
    Resp->SetNumberField(TEXT("bone_count"), NumBones);
    Resp->SetArrayField(TEXT("bones"), Bones);
    Resp->SetNumberField(TEXT("virtual_bone_count"), Skeleton->GetVirtualBones().Num());
    Resp->SetArrayField(TEXT("virtual_bone_names"), VirtualBoneNames);
    Resp->SetArrayField(TEXT("virtual_bones"), VirtualBones);
    Resp->SetNumberField(TEXT("slot_group_count"), Skeleton->GetSlotGroups().Num());
    Resp->SetNumberField(TEXT("socket_count"), Skeleton->Sockets.Num());
    return mcp_common::MakeOkReply(Resp);
}

// ---------------------------------------------------------------------------
// remove_virtual_bone
// ---------------------------------------------------------------------------
// MCP:remove_virtual_bone - Remove a virtual bone from a USkeleton asset, named directly or found by its source and target bones, then save. Idempotent: no match returns removed false and already_absent true without saving. Returns the remaining virtual_bone_names; a failed save returns success false.
//   @param skeleton_path - USkeleton asset path, e.g. "/Game/Characters/SK_Hero_Skeleton".
//   @param virtual_bone_name - Full virtual bone name with its "VB " prefix, e.g. "VB pelvis_hand_r"; when given, source_bone and target_bone are ignored.
//   @param source_bone - Source bone name; used with target_bone to resolve the virtual bone when virtual_bone_name is omitted.
//   @param target_bone - Target bone name; used with source_bone to resolve the virtual bone when virtual_bone_name is omitted.
//   @returns { asset_path: string, virtual_bone_name: string, removed: bool, already_absent: bool, virtual_bone_count: int, virtual_bone_names: [string], saved: bool }
TSharedPtr<FJsonObject> HandleRemoveVirtualBone(const TSharedPtr<FJsonObject>& Params)
{
    FString SkeletonPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("skeleton_path"), SkeletonPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    USkeleton* Skeleton = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadFlexibleOrError(SkeletonPath, TEXT("skeleton"), Skeleton)) { return Error; }

    // Resolve the target virtual bone name. Accept the VB-prefixed name directly,
    // or resolve from source_bone/target_bone the same way add_virtual_bone does.
    FName TargetName;
    FString VBName;
    if (Params->TryGetStringField(TEXT("virtual_bone_name"), VBName) && !VBName.IsEmpty())
    {
        TargetName = FName(*VBName);
    }
    else
    {
        FString SourceStr, TargetStr;
        const bool bHasSource = Params->TryGetStringField(TEXT("source_bone"), SourceStr) && !SourceStr.IsEmpty();
        const bool bHasTarget = Params->TryGetStringField(TEXT("target_bone"), TargetStr) && !TargetStr.IsEmpty();
        if (!bHasSource || !bHasTarget)
            return mcp_common::MakeErrorReply(
                TEXT("Provide virtual_bone_name, or both source_bone and target_bone"));

        const FName SourceBone(*SourceStr);
        const FName TargetBone(*TargetStr);
        for (const FVirtualBone& VB : Skeleton->GetVirtualBones())
        {
            if (VB.SourceBoneName == SourceBone && VB.TargetBoneName == TargetBone)
            {
                TargetName = VB.VirtualBoneName;
                break;
            }
        }
    }

    // Scan for an existing virtual bone matching the resolved name.
    bool bFound = false;
    if (!TargetName.IsNone())
    {
        for (const FVirtualBone& VB : Skeleton->GetVirtualBones())
        {
            if (VB.VirtualBoneName == TargetName)
            {
                bFound = true;
                break;
            }
        }
    }

    // Both outcomes report the virtual bone and the virtual bones the skeleton has left.
    auto BuildPayload = [&](bool bRemoved) -> TSharedPtr<FJsonObject>
    {
        TArray<TSharedPtr<FJsonValue>> Names;
        for (const FVirtualBone& VB : Skeleton->GetVirtualBones())
        {
            Names.Add(MakeShared<FJsonValueString>(VB.VirtualBoneName.ToString()));
        }
        TSharedPtr<FJsonObject> Resp = Skeleton_RemovalPayload(Skeleton, TEXT("virtual_bone_name"),
            TargetName.IsNone() ? VBName : TargetName.ToString(), bRemoved);
        Resp->SetNumberField(TEXT("virtual_bone_count"), Skeleton->GetVirtualBones().Num());
        Resp->SetArrayField(TEXT("virtual_bone_names"), Names);
        return Resp;
    };

    if (!bFound)
    {
        // Idempotent: nothing to remove.
        TSharedPtr<FJsonObject> Resp = BuildPayload(/*bRemoved=*/false);
        Resp->SetBoolField(TEXT("saved"), true);
        return mcp_common::MakeOkReply(Resp);
    }

    Skeleton->Modify();
    // USkeleton::RemoveVirtualBones(const TArray<FName>&) is ENGINE_API and self-guards.
    Skeleton->RemoveVirtualBones({ TargetName });

    FString SaveErr;
    const bool bSaved = Skeleton_MarkDirtyAndSave(Skeleton, SaveErr);
    return mcp_common::MakeSavedReply(BuildPayload(/*bRemoved=*/true), bSaved, TEXT("remove_virtual_bone"), SaveErr);
}

// ---------------------------------------------------------------------------
// remove_anim_slot_group
// ---------------------------------------------------------------------------
// MCP:remove_anim_slot_group - Remove an animation slot group from a skeleton (idempotent; guarded against missing group).
//   @param skeleton_path - Path to the USkeleton asset.
//   @param group_name - Name of the slot group to remove.
//   @returns { asset_path: string, group_name: string, removed: bool, already_absent: bool, slot_group_count: int, slot_group_names: [string], saved: bool }
TSharedPtr<FJsonObject> HandleRemoveAnimSlotGroup(const TSharedPtr<FJsonObject>& Params)
{
    FString SkeletonPath, GroupName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("skeleton_path"), SkeletonPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("group_name"), GroupName)) { return Missing; }

    USkeleton* Skeleton = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadFlexibleOrError(SkeletonPath, TEXT("skeleton"), Skeleton)) { return Error; }

    // Both outcomes report the group and the slot groups the skeleton has left.
    auto BuildPayload = [&](bool bRemoved) -> TSharedPtr<FJsonObject>
    {
        TArray<TSharedPtr<FJsonValue>> Names;
        for (const FAnimSlotGroup& Grp : Skeleton->GetSlotGroups())
        {
            Names.Add(MakeShared<FJsonValueString>(Grp.GroupName.ToString()));
        }
        TSharedPtr<FJsonObject> Resp = Skeleton_RemovalPayload(Skeleton, TEXT("group_name"), GroupName, bRemoved);
        Resp->SetNumberField(TEXT("slot_group_count"), Skeleton->GetSlotGroups().Num());
        Resp->SetArrayField(TEXT("slot_group_names"), Names);
        return Resp;
    };

    const FName Group(*GroupName);

    // CRITICAL crash guard: USkeleton::RemoveSlotGroup derefs null if the group is
    // absent. FindAnimSlotGroup returns nullptr when absent -> must NOT call Remove.
    if (Skeleton->FindAnimSlotGroup(Group) == nullptr)
    {
        TSharedPtr<FJsonObject> Resp = BuildPayload(/*bRemoved=*/false);
        Resp->SetBoolField(TEXT("saved"), true);
        return mcp_common::MakeOkReply(Resp);
    }

    Skeleton->Modify();
    Skeleton->RemoveSlotGroup(Group);

    FString SaveErr;
    const bool bSaved = Skeleton_MarkDirtyAndSave(Skeleton, SaveErr);
    return mcp_common::MakeSavedReply(BuildPayload(/*bRemoved=*/true), bSaved, TEXT("remove_anim_slot_group"), SaveErr);
}

// ---------------------------------------------------------------------------
// remove_skeleton_socket
// ---------------------------------------------------------------------------
// MCP:remove_socket_from_skeleton_asset - Remove a socket from the socket list of a USkeleton asset and save it. Idempotent: a missing socket returns removed false and already_absent true without saving. Returns socket_count and the remaining socket_names; sockets that exist only on a skeletal mesh are untouched (use remove_skeletal_mesh_socket).
//   @param skeleton_path - USkeleton asset path, e.g. "/Game/Characters/SK_Hero_Skeleton".
//   @param socket_name - Name of the socket to remove (case-insensitive).
//   @returns { asset_path: string, socket_name: string, removed: bool, already_absent: bool, socket_count: int, socket_names: [string], saved: bool }
TSharedPtr<FJsonObject> HandleRemoveSkeletonSocket(const TSharedPtr<FJsonObject>& Params)
{
    FString SkeletonPath, SocketName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("skeleton_path"), SkeletonPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("socket_name"), SocketName)) { return Missing; }

    USkeleton* Skeleton = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadFlexibleOrError(SkeletonPath, TEXT("skeleton"), Skeleton)) { return Error; }

    // Both outcomes report the socket and the sockets the skeleton has left.
    auto BuildPayload = [&](bool bRemoved) -> TSharedPtr<FJsonObject>
    {
        TArray<TSharedPtr<FJsonValue>> Names;
        for (const USkeletalMeshSocket* Sock : Skeleton->Sockets)
        {
            if (Sock)
            {
                Names.Add(MakeShared<FJsonValueString>(Sock->SocketName.ToString()));
            }
        }
        TSharedPtr<FJsonObject> Resp = Skeleton_RemovalPayload(Skeleton, TEXT("socket_name"), SocketName, bRemoved);
        Resp->SetNumberField(TEXT("socket_count"), Skeleton->Sockets.Num());
        Resp->SetArrayField(TEXT("socket_names"), Names);
        return Resp;
    };

    int32 Idx = INDEX_NONE;
    USkeletalMeshSocket* Socket = Skeleton->FindSocketAndIndex(FName(*SocketName), Idx);

    if (!Socket || Idx == INDEX_NONE)
    {
        // Idempotent: nothing to remove.
        TSharedPtr<FJsonObject> Resp = BuildPayload(/*bRemoved=*/false);
        Resp->SetBoolField(TEXT("saved"), true);
        return mcp_common::MakeOkReply(Resp);
    }

    Skeleton->Modify();
    Skeleton->Sockets.RemoveAt(Idx);

    FString SaveErr;
    const bool bSaved = Skeleton_MarkDirtyAndSave(Skeleton, SaveErr);
    return mcp_common::MakeSavedReply(BuildPayload(/*bRemoved=*/true), bSaved, TEXT("remove_socket_from_skeleton_asset"), SaveErr);
}

// ===========================================================================
// Bone tree CRUD via FReferenceSkeletonModifier.
//
// ReferenceSkeleton.h (FReferenceSkeletonModifier) is the engine's own
// public API for USkeleton bone tree mutation. Its destructor triggers
// RefSkeleton.RebuildRefSkeleton (ReferenceSkeleton.cpp) which only calls
// HandleReferenceSkeletonChange (Skeleton.cpp) — a delegate broadcast,
// no asset traversal, no SkeletalMesh::Build, no PIE required. Headless-safe.
//
// The modifier requires friend access to USkeleton::ReferenceSkeleton
// (Skeleton.h) which is granted for it.
// ===========================================================================

namespace
{
    /** Read a {location:[x,y,z], rotation:[p,y,r], scale:[x,y,z]} JSON object into FTransform.
     *  Tolerates missing fields (defaults to identity). Returns true on success.
     */
    bool ParseBoneTransformJson(const TSharedPtr<FJsonObject>& Json, FTransform& Out)
    {
        Out = FTransform::Identity;
        if (!Json.IsValid())
        {
            return true;
        }
        const FVector Loc = mcp_common::ReadJsonVector(Json, TEXT("location"));
        const FRotator Rot = mcp_common::ReadJsonRotator(Json, TEXT("rotation"));
        const FVector Scl = mcp_common::ReadJsonVector(Json, TEXT("scale"));
        // Treat scale 0,0,0 as a request for identity scale (helps when caller
        // forgets to provide scale at all — UE's default is 1,1,1 not 0,0,0).
        Out.SetLocation(Loc);
        Out.SetRotation(Rot.Quaternion());
        if (Scl.IsNearlyZero())
        {
            Out.SetScale3D(FVector::OneVector);
        }
        else
        {
            Out.SetScale3D(Scl);
        }
        return true;
    }

    /** Build the standard response payload for bone mutation commands. */
    TSharedPtr<FJsonObject> MakeBoneResponse(
        USkeleton* Skeleton,
        FReferenceSkeletonModifier& Mod,
        const FName& BoneName,
        int32 BoneIndex,
        int32 ParentIndex,
        const TCHAR* Op)
    {
        TSharedPtr<FJsonObject> Resp = Skeleton_NewPayload(Skeleton);
        Resp->SetStringField(TEXT("bone_name"), BoneName.ToString());
        Resp->SetNumberField(TEXT("bone_index"), BoneIndex);
        Resp->SetNumberField(TEXT("parent_index"), ParentIndex);
        Resp->SetNumberField(TEXT("bone_count"), Mod.GetReferenceSkeleton().GetNum());
        Resp->SetStringField(TEXT("op"), Op);
        return Resp;
    }
}

// ---------------------------------------------------------------------------
// add_bone
// ---------------------------------------------------------------------------
// MCP:add_bone - Add a bone named bone_name to the reference skeleton of a USkeleton asset under parent_name (omitted or empty: an extra root bone), with local_transform relative to the parent (a zero or missing scale becomes 1), then save the skeleton. Idempotent by name: an existing bone is returned (added false, already_existed true) without checking its parent or saving. An unknown parent is an error, and so is a failed save. Only the skeleton changes: skeletal meshes using it get no new bone, and there is no undo entry. No commit_bone_changes is needed; set_bone_parent and set_bone_transform edit bones, add_virtual_bone adds virtual ones.
//   @param skeleton_path - Path to the USkeleton asset
//   @param bone_name - Name for the new bone; when a bone of that name already exists nothing changes (added false)
//   @param parent_name - Parent bone name, or empty/null for a new root bone
//   @param local_transform - Optional {location:[x,y,z], rotation:[p,y,r], scale:[x,y,z]}, defaults to identity
//   @returns { asset_path, bone_name, bone_index, parent_index, bone_count, added, already_existed, saved, op: "add_bone" }
TSharedPtr<FJsonObject> HandleAddBone(const TSharedPtr<FJsonObject>& Params)
{
    FString SkeletonPath, BoneName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("skeleton_path"), SkeletonPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("bone_name"), BoneName)) { return Missing; }

    USkeleton* Skeleton = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadFlexibleOrError(SkeletonPath, TEXT("skeleton"), Skeleton)) { return Error; }

    FString ParentNameStr;
    Params->TryGetStringField(TEXT("parent_name"), ParentNameStr);
    const FName BoneFName(*BoneName);
    const FName ParentFName(ParentNameStr.IsEmpty() ? TEXT("") : *ParentNameStr);

    FTransform LocalTransform;
    const TSharedPtr<FJsonObject>* LocalXformObj = nullptr;
    if (Params->TryGetObjectField(TEXT("local_transform"), LocalXformObj) && LocalXformObj && LocalXformObj->IsValid())
    {
        ParseBoneTransformJson(*LocalXformObj, LocalTransform);
    }

    {
        // Pre-check for idempotency BEFORE calling Modify() — avoids spurious
        // dirty-flag/transaction noise on re-runs.
        FReferenceSkeletonModifier Mod(Skeleton);
        if (Mod.FindBoneIndex(BoneFName) != INDEX_NONE)
        {
            const int32 ExistingIdx = Mod.FindBoneIndex(BoneFName);
            TSharedPtr<FJsonObject> Resp = Skeleton_NewPayload(Skeleton);
            Resp->SetStringField(TEXT("bone_name"), BoneName);
            Resp->SetNumberField(TEXT("bone_index"), ExistingIdx);
            Resp->SetNumberField(TEXT("bone_count"), Mod.GetReferenceSkeleton().GetNum());
            Resp->SetBoolField(TEXT("added"), false);
            Resp->SetBoolField(TEXT("already_existed"), true);
            Resp->SetStringField(TEXT("op"), TEXT("add_bone"));
            return mcp_common::MakeOkReply(Resp);
        }

        const int32 ParentIdx = ParentNameStr.IsEmpty() ? INDEX_NONE : Mod.FindBoneIndex(ParentFName);
        if (!ParentNameStr.IsEmpty() && ParentIdx == INDEX_NONE)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Parent bone '%s' not found in skeleton"), *ParentNameStr));
        }

        Skeleton->Modify();

        // bAllowMultipleRoots=true when caller explicitly wants a root bone
        // (no parent). Skeletons have bOnlyOneRootAllowed=true by default, so
        // we must opt in for the root-bone case.
        const FMeshBoneInfo Info(BoneFName, BoneName, ParentIdx);
        Mod.Add(Info, LocalTransform, /*bAllowMultipleRoots=*/ParentNameStr.IsEmpty());
        // dtor -> RebuildRefSkeleton + HandleReferenceSkeletonChange broadcast.
    }

    FString SaveErr;
    const bool bSaved = Skeleton_MarkDirtyAndSave(Skeleton, SaveErr);

    // Re-open modifier to report the post-add state cleanly.
    FReferenceSkeletonModifier Mod2(Skeleton);
    const int32 AddedIdx = Mod2.FindBoneIndex(BoneFName);
    const int32 PostParentIdx = (AddedIdx != INDEX_NONE)
        ? Mod2.GetRefBoneInfo()[AddedIdx].ParentIndex
        : INDEX_NONE;
    TSharedPtr<FJsonObject> Resp = MakeBoneResponse(Skeleton, Mod2, BoneFName, AddedIdx, PostParentIdx, TEXT("add_bone"));
    Resp->SetBoolField(TEXT("added"), AddedIdx != INDEX_NONE);
    Resp->SetBoolField(TEXT("already_existed"), false);
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("add_bone"), SaveErr);
}

// ---------------------------------------------------------------------------
// remove_bone
// ---------------------------------------------------------------------------
// MCP:remove_bone - Remove a bone from a skeleton's reference skeleton (idempotent).
//   @param skeleton_path - Path to the USkeleton asset
//   @param bone_name - Name of the bone to remove
//   @param remove_children - If true, recursively remove all descendant bones (default false)
//   @returns { asset_path, bone_name, removed, already_absent, remove_children, bone_count, saved, op: "remove_bone" }
TSharedPtr<FJsonObject> HandleRemoveBone(const TSharedPtr<FJsonObject>& Params)
{
    FString SkeletonPath, BoneName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("skeleton_path"), SkeletonPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("bone_name"), BoneName)) { return Missing; }

    bool bRemoveChildren = false;
    Params->TryGetBoolField(TEXT("remove_children"), bRemoveChildren);

    USkeleton* Skeleton = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadFlexibleOrError(SkeletonPath, TEXT("skeleton"), Skeleton)) { return Error; }

    // Both outcomes report the bone and the bone count the skeleton has left.
    auto BuildPayload = [&](bool bRemoved) -> TSharedPtr<FJsonObject>
    {
        TSharedPtr<FJsonObject> Resp = Skeleton_RemovalPayload(Skeleton, TEXT("bone_name"), BoneName, bRemoved);
        Resp->SetBoolField(TEXT("remove_children"), bRemoveChildren);
        Resp->SetNumberField(TEXT("bone_count"), Skeleton->GetReferenceSkeleton().GetNum());
        Resp->SetStringField(TEXT("op"), TEXT("remove_bone"));
        return Resp;
    };

    const FName BoneFName(*BoneName);
    const int32 ExistingIdx = Skeleton->GetReferenceSkeleton().FindBoneIndex(BoneFName);

    if (ExistingIdx == INDEX_NONE)
    {
        // Idempotent: nothing to remove.
        return mcp_common::MakeOkReply(BuildPayload(/*bRemoved=*/false));
    }

    Skeleton->Modify();
    {
        FReferenceSkeletonModifier Mod(Skeleton);
        Mod.Remove(BoneFName, bRemoveChildren);
    }

    FString SaveErr;
    const bool bSaved = Skeleton_MarkDirtyAndSave(Skeleton, SaveErr);
    return mcp_common::MakeSavedReply(BuildPayload(/*bRemoved=*/true), bSaved, TEXT("remove_bone"), SaveErr);
}

// ---------------------------------------------------------------------------
// rename_bone
// ---------------------------------------------------------------------------
// MCP:rename_bone - Rename a bone in the reference skeleton of a USkeleton asset and save it. Fails with an error when old_name is not a bone or new_name is already taken; skeletal meshes and animations that use the old name are not updated. A failed save returns success false.
//   @param skeleton_path - USkeleton asset path, e.g. "/Game/Characters/SK_Hero_Skeleton".
//   @param old_name - Current bone name; must exist in the reference skeleton.
//   @param new_name - Desired new bone name (must not already exist)
//   @returns { asset_path, old_name, new_name, renamed: true, not_found: false, already_used: false, bone_count, saved, save_error?, op: "rename_bone" }
TSharedPtr<FJsonObject> HandleRenameBone(const TSharedPtr<FJsonObject>& Params)
{
    FString SkeletonPath, OldName, NewName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("skeleton_path"), SkeletonPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("old_name"), OldName)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("new_name"), NewName)) { return Missing; }

    USkeleton* Skeleton = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadFlexibleOrError(SkeletonPath, TEXT("skeleton"), Skeleton)) { return Error; }

    const FName OldFName(*OldName);
    const FName NewFName(*NewName);
    const FReferenceSkeleton& RefSkel = Skeleton->GetReferenceSkeleton();

    if (RefSkel.FindBoneIndex(OldFName) == INDEX_NONE)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("rename_bone: bone '%s' not found in skeleton '%s'"),
                *OldName, *Skeleton->GetPathName()));
    }

    if (RefSkel.FindBoneIndex(NewFName) != INDEX_NONE)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("rename_bone: bone name '%s' already in use in skeleton '%s'"),
                *NewName, *Skeleton->GetPathName()));
    }

    Skeleton->Modify();
    {
        FReferenceSkeletonModifier Mod(Skeleton);
        Mod.Rename(OldFName, NewFName);
    }

    FString SaveErr;
    const bool bSaved = Skeleton_MarkDirtyAndSave(Skeleton, SaveErr);

    TSharedPtr<FJsonObject> Resp = Skeleton_NewPayload(Skeleton);
    Resp->SetStringField(TEXT("old_name"), OldName);
    Resp->SetStringField(TEXT("new_name"), NewName);
    Resp->SetBoolField(TEXT("renamed"), true);
    Resp->SetBoolField(TEXT("not_found"), false);
    Resp->SetBoolField(TEXT("already_used"), false);
    Resp->SetNumberField(TEXT("bone_count"), Skeleton->GetReferenceSkeleton().GetNum());
    Resp->SetStringField(TEXT("op"), TEXT("rename_bone"));
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("rename_bone"), SaveErr);
}

// ---------------------------------------------------------------------------
// set_bone_transform
// ---------------------------------------------------------------------------
// MCP:set_bone_transform - Update the local reference-pose transform of an existing bone
//   @param skeleton_path - Path to the USkeleton asset
//   @param bone_name - Bone to update
//   @param local_transform - {location:[x,y,z], rotation:[p,y,r], scale:[x,y,z]} — all three required
//   @returns { asset_path, bone_name, bone_index, updated, not_found, bone_count, saved, op: "set_bone_transform" }
TSharedPtr<FJsonObject> HandleSetBoneTransform(const TSharedPtr<FJsonObject>& Params)
{
    FString SkeletonPath, BoneName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("skeleton_path"), SkeletonPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("bone_name"), BoneName)) { return Missing; }

    const TSharedPtr<FJsonObject>* LocalXformObj = nullptr;
    if (!Params->TryGetObjectField(TEXT("local_transform"), LocalXformObj) || !LocalXformObj || !LocalXformObj->IsValid())
    {
        return mcp_common::MissingParameterReply(TEXT("local_transform"));
    }

    FTransform LocalTransform;
    ParseBoneTransformJson(*LocalXformObj, LocalTransform);

    USkeleton* Skeleton = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadFlexibleOrError(SkeletonPath, TEXT("skeleton"), Skeleton)) { return Error; }

    const FName BoneFName(*BoneName);
    const int32 BoneIdx = Skeleton->GetReferenceSkeleton().FindBoneIndex(BoneFName);

    if (BoneIdx == INDEX_NONE)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("set_bone_transform: bone '%s' not found in skeleton '%s'"),
                *BoneName, *Skeleton->GetPathName()));
    }

    Skeleton->Modify();
    {
        FReferenceSkeletonModifier Mod(Skeleton);
        Mod.UpdateRefPoseTransform(BoneIdx, LocalTransform);
    }

    FString SaveErr;
    const bool bSaved = Skeleton_MarkDirtyAndSave(Skeleton, SaveErr);

    TSharedPtr<FJsonObject> Resp = Skeleton_NewPayload(Skeleton);
    Resp->SetStringField(TEXT("bone_name"), BoneName);
    Resp->SetNumberField(TEXT("bone_index"), BoneIdx);
    Resp->SetBoolField(TEXT("updated"), true);
    Resp->SetBoolField(TEXT("not_found"), false);
    Resp->SetNumberField(TEXT("bone_count"), Skeleton->GetReferenceSkeleton().GetNum());
    Resp->SetStringField(TEXT("op"), TEXT("set_bone_transform"));
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("set_bone_transform"), SaveErr);
}

// ---------------------------------------------------------------------------
// set_bone_parent
// ---------------------------------------------------------------------------
// MCP:set_bone_parent - Re-parent a bone (pass empty parent_name to make it a new root).
//   @param skeleton_path - Path to the USkeleton asset
//   @param bone_name - Bone to re-parent
//   @param parent_name - New parent bone name, or empty/null to detach to root
//   @returns { asset_path, bone_name, parent_name, bone_index, parent_index, updated, not_found, bone_count, saved, op: "set_bone_parent" }
TSharedPtr<FJsonObject> HandleSetBoneParent(const TSharedPtr<FJsonObject>& Params)
{
    FString SkeletonPath, BoneName, ParentNameStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("skeleton_path"), SkeletonPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("bone_name"), BoneName)) { return Missing; }
    Params->TryGetStringField(TEXT("parent_name"), ParentNameStr);

    USkeleton* Skeleton = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadFlexibleOrError(SkeletonPath, TEXT("skeleton"), Skeleton)) { return Error; }

    const FName BoneFName(*BoneName);
    const FName ParentFName(ParentNameStr.IsEmpty() ? TEXT("") : *ParentNameStr);
    const FReferenceSkeleton& RefSkel = Skeleton->GetReferenceSkeleton();

    if (RefSkel.FindBoneIndex(BoneFName) == INDEX_NONE)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("set_bone_parent: bone '%s' not found in skeleton '%s'"),
                *BoneName, *Skeleton->GetPathName()));
    }
    if (!ParentNameStr.IsEmpty() && RefSkel.FindBoneIndex(ParentFName) == INDEX_NONE)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("set_bone_parent: parent bone '%s' not found in skeleton '%s'"),
                *ParentNameStr, *Skeleton->GetPathName()));
    }

    Skeleton->Modify();
    int32 NewIndex = INDEX_NONE;
    {
        FReferenceSkeletonModifier Mod(Skeleton);
        NewIndex = Mod.SetParent(BoneFName, ParentFName, /*bAllowMultipleRoots=*/ParentNameStr.IsEmpty());
    }

    FString SaveErr;
    const bool bSaved = Skeleton_MarkDirtyAndSave(Skeleton, SaveErr);

    TSharedPtr<FJsonObject> Resp = Skeleton_NewPayload(Skeleton);
    Resp->SetStringField(TEXT("bone_name"), BoneName);
    Resp->SetStringField(TEXT("parent_name"), ParentNameStr);
    Resp->SetNumberField(TEXT("bone_index"), NewIndex);
    Resp->SetBoolField(TEXT("updated"), NewIndex != INDEX_NONE);
    Resp->SetBoolField(TEXT("not_found"), false);
    Resp->SetNumberField(TEXT("bone_count"), Skeleton->GetReferenceSkeleton().GetNum());
    Resp->SetStringField(TEXT("op"), TEXT("set_bone_parent"));
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("set_bone_parent"), SaveErr);
}

// ---------------------------------------------------------------------------
// is_skeleton_shared
// ---------------------------------------------------------------------------
// MCP:is_skeleton_shared - Find the skeletal mesh assets bound to a USkeleton asset through an Asset Registry query on their Skeleton tag, returning mesh_count, mesh_paths and shared (true when more than one mesh uses it). Read-only; only the skeleton itself is loaded.
//   @param skeleton_path - USkeleton asset path, e.g. "/Game/Characters/SK_Hero_Skeleton".
//   @returns { asset_path, mesh_count, shared, mesh_paths }
TSharedPtr<FJsonObject> HandleIsSkeletonShared(const TSharedPtr<FJsonObject>& Params)
{
    FString SkeletonPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("skeleton_path"), SkeletonPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    USkeleton* Skeleton = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadFlexibleOrError(SkeletonPath, TEXT("skeleton"), Skeleton)) { return Error; }

    // AssetRegistry FARFilter on SkeletalMesh tag "Skeleton" == <export text of this skeleton>.
    // Pattern mirrored from USkeleton::FindCompatibleMesh (Skeleton.cpp) —
    // the engine's own canonical "which meshes use this skeleton" lookup.
    IAssetRegistry& AR = mcp_common::AssetRegistry();

    FARFilter Filter;
    Filter.bRecursivePaths = true;
    Filter.bRecursiveClasses = true;
    Filter.ClassPaths.Add(USkeletalMesh::StaticClass()->GetClassPathName());
    Filter.TagsAndValues.Add(USkeletalMesh::GetSkeletonMemberName(),
                             FAssetData(Skeleton).GetExportTextName());

    TArray<FAssetData> AssetList;
    AR.GetAssets(Filter, AssetList);

    TArray<TSharedPtr<FJsonValue>> Paths;
    Paths.Reserve(AssetList.Num());
    for (const FAssetData& AD : AssetList)
    {
        Paths.Add(MakeShared<FJsonValueString>(AD.GetObjectPathString()));
    }

    TSharedPtr<FJsonObject> Resp = Skeleton_NewPayload(Skeleton);
    Resp->SetNumberField(TEXT("mesh_count"), AssetList.Num());
    Resp->SetBoolField(TEXT("shared"), AssetList.Num() > 1);
    Resp->SetArrayField(TEXT("mesh_paths"), Paths);
    return mcp_common::MakeOkReply(Resp);
}

// ---------------------------------------------------------------------------
// set_blend_profile_bone
// ---------------------------------------------------------------------------
// MCP:set_blend_profile_bone - Set a bone's blend scale on a UBlendProfile (creates profile if missing).
//   @param skeleton_path - Path to the USkeleton asset
//   @param profile_name - Name of the UBlendProfile (created if absent when create_if_missing=true)
//   @param bone_name - Bone to scale
//   @param scale - Blend scale value (clamped to [0, 1] by UBlendProfile)
//   @param create_if_missing - If true, create the profile when not present (default true)
//   @returns { asset_path, profile_name, bone_name, scale, updated, created_profile, profile_count, saved, op: "set_blend_profile_bone" }
TSharedPtr<FJsonObject> HandleSetBlendProfileBone(const TSharedPtr<FJsonObject>& Params)
{
    FString SkeletonPath, ProfileName, BoneName;
    double ScaleDouble = 1.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("skeleton_path"), SkeletonPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("profile_name"), ProfileName)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("bone_name"), BoneName)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("scale"), ScaleDouble)) { return Missing; }

    bool bCreateIfMissing = true;
    Params->TryGetBoolField(TEXT("create_if_missing"), bCreateIfMissing);

    USkeleton* Skeleton = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadFlexibleOrError(SkeletonPath, TEXT("skeleton"), Skeleton)) { return Error; }

    const FName ProfileFName(*ProfileName);
    const FName BoneFName(*BoneName);
    const float Scale = (float)ScaleDouble;

    if (Skeleton->GetReferenceSkeleton().FindBoneIndex(BoneFName) == INDEX_NONE)
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Bone '%s' not found in skeleton"), *BoneName));

    bool bCreatedProfile = false;
    UBlendProfile* Profile = Skeleton->GetBlendProfile(ProfileFName);
    if (!Profile)
    {
        if (!bCreateIfMissing)
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Blend profile '%s' not found (and create_if_missing=false)"), *ProfileName));
        Profile = Skeleton->CreateNewBlendProfile(ProfileFName);
        bCreatedProfile = (Profile != nullptr);
        if (!Profile)
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Failed to create blend profile '%s'"), *ProfileName));
    }

    Skeleton->Modify();
    Profile->Modify();
    // SetBoneBlendScale(name, scale, bRecurse, bCreate) — the bCreate=true lets
    // us write a scale to a bone that has no entry yet without manual bookkeeping.
    Profile->SetBoneBlendScale(BoneFName, Scale, /*bRecurse=*/false, /*bCreate=*/true);

    FString SaveErr;
    const bool bSaved = Skeleton_MarkDirtyAndSave(Skeleton, SaveErr);

    TSharedPtr<FJsonObject> Resp = Skeleton_NewPayload(Skeleton);
    Resp->SetStringField(TEXT("profile_name"), ProfileName);
    Resp->SetStringField(TEXT("bone_name"), BoneName);
    Resp->SetNumberField(TEXT("scale"), Scale);
    Resp->SetBoolField(TEXT("updated"), true);
    Resp->SetBoolField(TEXT("created_profile"), bCreatedProfile);
    Resp->SetNumberField(TEXT("stored_scale"), Profile->GetBoneBlendScale(BoneFName));
    Resp->SetStringField(TEXT("op"), TEXT("set_blend_profile_bone"));
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("set_blend_profile_bone"), SaveErr);
}

// ---------------------------------------------------------------------------
// commit_bone_changes
// ---------------------------------------------------------------------------
// MCP:commit_bone_changes - Mark a skeleton dirty + save it. YOU DO NOT NEED THIS after add_bone / remove_bone / rename_bone / set_bone_transform: each of those already notifies the skeleton's listeners on its own. It exists for the cases those do not cover -- batched mutations made outside the modifier, or an asset import that never went through one (interop with other tools).
//   @param skeleton_path - Path to the USkeleton asset
//   @param force - If true, mark dirty and save even when no-op (default true)
//   @returns { asset_path, committed, force, bone_count, saved, op: "commit_bone_changes" }
//   NOTE (impl): listeners are auto-refreshed by the FReferenceSkeletonModifier destructor (RebuildRefSkeleton
//   broadcasts OnReferenceSkeletonChanged via USkeleton::HandleReferenceSkeletonChange).
TSharedPtr<FJsonObject> HandleCommitBoneChanges(const TSharedPtr<FJsonObject>& Params)
{
    FString SkeletonPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("skeleton_path"), SkeletonPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    bool bForce = true;
    Params->TryGetBoolField(TEXT("force"), bForce);

    USkeleton* Skeleton = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadFlexibleOrError(SkeletonPath, TEXT("skeleton"), Skeleton)) { return Error; }

    // The FReferenceSkeletonModifier destructor already triggers
    // RebuildRefSkeleton + HandleReferenceSkeletonChange on every bone mutation,
    // so after add_bone/remove_bone/rename_bone/set_bone_transform the delegate
    // OnReferenceSkeletonChanged has already fired. This command is the
    // "after-batch-save" hook: it just marks dirty + saves when requested.
    if (bForce)
    {
        Skeleton->Modify();
        Skeleton->MarkPackageDirty();
    }

    FString SaveErr;
    const bool bSaved = bForce
        ? mcp_common::SaveAssetPackage(Skeleton->GetOutermost(), Skeleton, SaveErr)
        : true;

    TSharedPtr<FJsonObject> Resp = Skeleton_NewPayload(Skeleton);
    Resp->SetBoolField(TEXT("committed"), true);
    Resp->SetBoolField(TEXT("force"), bForce);
    Resp->SetNumberField(TEXT("bone_count"), Skeleton->GetReferenceSkeleton().GetNum());
    Resp->SetStringField(TEXT("op"), TEXT("commit_bone_changes"));
    if (bForce && !bSaved)
    {
        Resp->SetStringField(TEXT("save_error"), SaveErr);
    }
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("commit_bone_changes"));
}

// ===========================================================================
// CRUD inverses — remove_anim_curve_name (of add_anim_curve_name), remove_blend_profile
// (of set_blend_profile_bone) and get_bone_transform (of set_bone_transform).

// ---------------------------------------------------------------------------
// remove_anim_curve_name
// ---------------------------------------------------------------------------
// MCP:remove_anim_curve_name - Remove a curve metadata entry (curve name) from a USkeleton (idempotent).
//   @param skeleton_path - Path to the USkeleton asset
//   @param curve_name - The curve name to remove (e.g. 'Curve_Example').
//   @returns { asset_path, curve_name, removed, already_absent, present, curve_count, curve_names, saved }
TSharedPtr<FJsonObject> HandleRemoveAnimCurveName(const TSharedPtr<FJsonObject>& Params)
{
    FString SkeletonPath, CurveName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("skeleton_path"), SkeletonPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("curve_name"), CurveName)) { return Missing; }

    USkeleton* Skeleton = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadFlexibleOrError(SkeletonPath, TEXT("skeleton"), Skeleton)) { return Error; }

    const FName CurveFName(*CurveName);

    // Enumerate current curve metadata names (mirrors add_anim_curve_name's
    // GetCurveMetaDataNames verification path).
    auto BuildNames = [&]() -> TArray<TSharedPtr<FJsonValue>>
    {
        TArray<FName> Names;
        Skeleton->GetCurveMetaDataNames(Names);
        TArray<TSharedPtr<FJsonValue>> Out;
        Out.Reserve(Names.Num());
        for (const FName& N : Names)
        {
            Out.Add(MakeShared<FJsonValueString>(N.ToString()));
        }
        return Out;
    };

    // Presence check up front so removal is idempotent (RemoveCurveMetaData
    // already returns false when absent, but we skip the transaction/save churn).
    TArray<FName> PreNames;
    Skeleton->GetCurveMetaDataNames(PreNames);
    const bool bPresentBefore = PreNames.Contains(CurveFName);

    if (!bPresentBefore)
    {
        // Idempotent: nothing to remove.
        TSharedPtr<FJsonObject> Resp = Skeleton_RemovalPayload(Skeleton, TEXT("curve_name"), CurveName, /*bRemoved=*/false);
        Resp->SetBoolField(TEXT("present"), false);
        Resp->SetNumberField(TEXT("curve_count"), PreNames.Num());
        Resp->SetArrayField(TEXT("curve_names"), BuildNames());
        Resp->SetBoolField(TEXT("saved"), true);
        return mcp_common::MakeOkReply(Resp);
    }

    Skeleton->Modify();
    // USkeleton::RemoveCurveMetaData(FName) is ENGINE_API but #if WITH_EDITOR only
    // (Skeleton.h, inside the 413-464 editor block) — its twin AddCurveMetaData
    // is NOT gated, so the mirror is asymmetric. The symbol does not exist in a
    // non-editor build, so the call must be guarded. Honest gate outside the editor.
    // Returns true if the entry existed and was removed.
    const bool bRemoved = Skeleton->RemoveCurveMetaData(CurveFName);

    // Verify absent by re-enumerating (symmetric with add_anim_curve_name).
    TArray<FName> PostNames;
    Skeleton->GetCurveMetaDataNames(PostNames);
    const bool bPresentAfter = PostNames.Contains(CurveFName);

    FString SaveErr;
    const bool bSaved = Skeleton_MarkDirtyAndSave(Skeleton, SaveErr);

    TSharedPtr<FJsonObject> Resp = Skeleton_NewPayload(Skeleton);
    Resp->SetStringField(TEXT("curve_name"), CurveName);
    Resp->SetBoolField(TEXT("removed"), bRemoved);
    Resp->SetBoolField(TEXT("already_absent"), false);
    Resp->SetBoolField(TEXT("present"), bPresentAfter);
    Resp->SetNumberField(TEXT("curve_count"), PostNames.Num());
    Resp->SetArrayField(TEXT("curve_names"), BuildNames());
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("remove_anim_curve_name"), SaveErr);
}

// ---------------------------------------------------------------------------
// remove_blend_profile
// ---------------------------------------------------------------------------
// MCP:remove_blend_profile - Remove a WHOLE UBlendProfile from a skeleton (idempotent). Inverse of set_blend_profile_bone, which is what implicitly CREATES a profile -- there is no separate create_blend_profile to undo.
//   @param skeleton_path - Path to the USkeleton asset
//   @param profile_name - Name of the UBlendProfile to remove
//   @returns { asset_path, profile_name, removed, already_absent, profile_count, profile_names, saved }
TSharedPtr<FJsonObject> HandleRemoveBlendProfile(const TSharedPtr<FJsonObject>& Params)
{
    FString SkeletonPath, ProfileName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("skeleton_path"), SkeletonPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("profile_name"), ProfileName)) { return Missing; }

    USkeleton* Skeleton = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadFlexibleOrError(SkeletonPath, TEXT("skeleton"), Skeleton)) { return Error; }

    // Skeleton->BlendProfiles is a public UPROPERTY(Instanced)
    // TArray<TObjectPtr<UBlendProfile>> (Skeleton.h). There is no
    // RemoveBlendProfile accessor, so remove Engine-only: resolve via the
    // canonical GetBlendProfile lookup (the same accessor set_blend_profile_bone
    // uses), drop it from the array, and mark it garbage.

    // Both outcomes report the profile and the blend profiles the skeleton has left.
    auto BuildPayload = [&](bool bRemoved) -> TSharedPtr<FJsonObject>
    {
        TArray<TSharedPtr<FJsonValue>> Names;
        for (const UBlendProfile* BP : Skeleton->BlendProfiles)
        {
            if (BP)
            {
                Names.Add(MakeShared<FJsonValueString>(BP->GetName()));
            }
        }
        TSharedPtr<FJsonObject> Resp = Skeleton_RemovalPayload(Skeleton, TEXT("profile_name"), ProfileName, bRemoved);
        Resp->SetNumberField(TEXT("profile_count"), Skeleton->BlendProfiles.Num());
        Resp->SetArrayField(TEXT("profile_names"), Names);
        return Resp;
    };

    const FName ProfileFName(*ProfileName);
    UBlendProfile* Profile = Skeleton->GetBlendProfile(ProfileFName);

    if (!Profile)
    {
        // Idempotent: nothing to remove.
        TSharedPtr<FJsonObject> Resp = BuildPayload(/*bRemoved=*/false);
        Resp->SetBoolField(TEXT("saved"), true);
        return mcp_common::MakeOkReply(Resp);
    }

    Skeleton->Modify();
    Skeleton->BlendProfiles.Remove(Profile);
    // The profile is a UPROPERTY(Instanced) subobject outered to the skeleton;
    // drop the last reference so it is not resaved into the package.
    Profile->MarkAsGarbage();

    FString SaveErr;
    const bool bSaved = Skeleton_MarkDirtyAndSave(Skeleton, SaveErr);
    return mcp_common::MakeSavedReply(BuildPayload(/*bRemoved=*/true), bSaved, TEXT("remove_blend_profile"), SaveErr);
}

// ---------------------------------------------------------------------------
// get_bone_transform
// ---------------------------------------------------------------------------
// MCP:get_bone_transform - Read the LOCAL reference-pose transform of a bone (parent-relative, not component space). Exact inverse of set_bone_transform: it reads the same raw ref-pose array that one writes, so a read-modify-write round trip is lossless.
//   @param skeleton_path - Path to the USkeleton asset
//   @param bone_name - Bone to read
//   @returns { asset_path, bone_name, bone_index, found, location:[x,y,z], rotation:[p,y,r], scale:[x,y,z], bone_count, op: "get_bone_transform" }
//   NOTE (impl): the array is the one FReferenceSkeletonModifier::UpdateRefPoseTransform writes.
TSharedPtr<FJsonObject> HandleGetBoneTransform(const TSharedPtr<FJsonObject>& Params)
{
    FString SkeletonPath, BoneName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("skeleton_path"), SkeletonPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("bone_name"), BoneName)) { return Missing; }

    USkeleton* Skeleton = nullptr;
    if (TSharedPtr<FJsonObject> Error = AnimAssetUtils::LoadFlexibleOrError(SkeletonPath, TEXT("skeleton"), Skeleton)) { return Error; }

    const FReferenceSkeleton& RefSkel = Skeleton->GetReferenceSkeleton();
    const FName BoneFName(*BoneName);
    // Resolve against the RAW name->index map (ReferenceSkeleton.h), which is
    // the correct basis for GetRawRefBonePose (the RAW array). This is the pose
    // set_bone_transform mutates via UpdateRefPoseTransform (RawRefBonePose[idx],
    // ReferenceSkeleton.h). Virtual bones have no raw entry -> INDEX_NONE here,
    // which is honest: they carry no reference-pose transform to read.
    const int32 BoneIdx = RefSkel.FindRawBoneIndex(BoneFName);

    if (BoneIdx == INDEX_NONE)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("get_bone_transform: bone '%s' not found (or is a virtual bone with no raw ref-pose) in skeleton '%s'"),
                *BoneName, *Skeleton->GetPathName()));
    }

    // Read the symmetric array (ReferenceSkeleton.h).
    const TArray<FTransform>& RawPose = RefSkel.GetRawRefBonePose();
    if (!RawPose.IsValidIndex(BoneIdx))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("get_bone_transform: bone '%s' has no raw ref-pose entry (index %d of %d)"),
                *BoneName, BoneIdx, RawPose.Num()));
    }

    const FTransform& Xform = RawPose[BoneIdx];
    const FVector Loc = Xform.GetLocation();
    const FRotator Rot = Xform.GetRotation().Rotator();
    const FVector Scl = Xform.GetScale3D();

    // Emit the same array layout ParseBoneTransformJson consumes so the payload
    // round-trips straight back into set_bone_transform: location [x,y,z],
    // rotation [pitch,yaw,roll], scale [x,y,z].
    TSharedPtr<FJsonObject> Resp = Skeleton_NewPayload(Skeleton);
    Resp->SetStringField(TEXT("bone_name"), BoneName);
    Resp->SetNumberField(TEXT("bone_index"), BoneIdx);
    Resp->SetBoolField(TEXT("found"), true);
    Resp->SetArrayField(TEXT("location"), mcp_common::VectorToJsonArray(Loc));
    Resp->SetArrayField(TEXT("rotation"), mcp_common::RotatorToJsonArray(Rot));
    Resp->SetArrayField(TEXT("scale"), mcp_common::VectorToJsonArray(Scl));
    Resp->SetNumberField(TEXT("bone_count"), RefSkel.GetNum());
    Resp->SetStringField(TEXT("op"), TEXT("get_bone_transform"));
    return mcp_common::MakeOkReply(Resp);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_anim_slot_group"), &HandleAddAnimSlotGroup },
    { TEXT("add_bone"), &HandleAddBone },
    { TEXT("add_socket_to_skeleton_asset"), &HandleAddSkeletonSocket },
    { TEXT("add_virtual_bone"), &HandleAddVirtualBone },
    { TEXT("commit_bone_changes"), &HandleCommitBoneChanges },
    { TEXT("create_editable_skeleton"), &HandleCreateEditableSkeleton },
    { TEXT("get_bone_transform"), &HandleGetBoneTransform },
    { TEXT("get_skeleton_asset_info"), &HandleGetSkeletonInfo },
    { TEXT("is_skeleton_shared"), &HandleIsSkeletonShared },
    { TEXT("remove_anim_curve_name"), &HandleRemoveAnimCurveName },
    { TEXT("remove_anim_slot_group"), &HandleRemoveAnimSlotGroup },
    { TEXT("remove_blend_profile"), &HandleRemoveBlendProfile },
    { TEXT("remove_bone"), &HandleRemoveBone },
    { TEXT("remove_socket_from_skeleton_asset"), &HandleRemoveSkeletonSocket },
    { TEXT("remove_virtual_bone"), &HandleRemoveVirtualBone },
    { TEXT("rename_bone"), &HandleRenameBone },
    { TEXT("set_blend_profile_bone"), &HandleSetBlendProfileBone },
    { TEXT("set_bone_parent"), &HandleSetBoneParent },
    { TEXT("set_bone_transform"), &HandleSetBoneTransform },
};
} // namespace UEAgentSkeletonCommands
