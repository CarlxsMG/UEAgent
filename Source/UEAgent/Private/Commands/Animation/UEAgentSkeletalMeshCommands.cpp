// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Misc/EngineVersionComparison.h"

#include "Editor.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Modules/ModuleManager.h"

#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"

// Skeletal mesh runtime (Engine module — already a dep)
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Animation/Skeleton.h"
#include "Animation/MorphTarget.h"
#include "ReferenceSkeleton.h"

// Skeletal mesh editor model (WITH_EDITOR — FSkeletalMeshModel, FSkeletalMeshLODModel, FSkelMeshSection)
#include "Rendering/SkeletalMeshModel.h"

// Material slot struct (FSkeletalMaterial)
#include "Engine/SkinnedAssetCommon.h"
// Material slot assignment (LoadObject<UMaterialInterface>, GetPathName) — set_skeletal_mesh_material_slot
#include "Materials/MaterialInterface.h"
#include "SkeletalMeshReductionSettings.h"           // FSkeletalMeshOptimizationSettings + SMTC_/SMOT_/SMOI_ enums

// Skeletal mesh editor subsystem (SkeletalMeshEditor module — editor dep)
#include "SkeletalMeshEditorSubsystem.h"
#include "ScopedTransaction.h"        // FScopedTransaction -- set_morph_target_deltas, and the 5.7 body of RemoveSocket
#if UE_VERSION_OLDER_THAN(5, 8, 0)
#include "EditorScriptingHelpers.h"   // CheckIfInEditorAndPIE -- the 5.7 body of SetAllowCPUAccess
#endif

// PhysicsAsset (Engine module — needed for assign_skeletal_mesh_physics_asset)
#include "PhysicsEngine/PhysicsAsset.h"

/**
 * Skeletal mesh editing commands.
 *
 * Headless-safe inspection and editing of USkeletalMesh assets: sockets, morph targets, sections, material
 * slots, LODs and their build and reduction settings. Uses USkeletalMesh (Engine module) for direct mesh
 * access, and USkeletalMeshEditorSubsystem (SkeletalMeshEditor module) for LOD vertex/section counts and the
 * edits the editor routes through it, such as section flags, socket rename/remove and LOD regeneration.
 */
namespace UEAgentSkeletalMeshCommands
{

// MCP-DOMAIN: Animation

namespace
{
	/** Parse a string to a UENUM value by name. Accepts the full value name (e.g.
	 *  "SMTC_NumOfTriangles") or, when Prefix is given, the short form ("NumOfTriangles").
	 *  Returns false if unknown. */
	template<typename TEnum>
	bool ParseEnumByName(const FString& S, const TCHAR* Prefix, TEnum& Out)
	{
		const UEnum* E = StaticEnum<TEnum>();
		if (!E) { return false; }
		int64 V = E->GetValueByNameString(S);
		if (V == INDEX_NONE && Prefix) { V = E->GetValueByNameString(FString(Prefix) + S); }
		if (V == INDEX_NONE) { return false; }
		Out = static_cast<TEnum>(V);
		return true;
	}

	/** Serialize a UENUM value to its name, stripping any "EnumType::" namespace. */
	template<typename TEnum>
	FString EnumToName(TEnum Value)
	{
		const UEnum* E = StaticEnum<TEnum>();
		if (!E) { return FString::FromInt(static_cast<int32>(Value)); }
		FString Name = E->GetNameStringByValue(static_cast<int64>(Value));
		int32 Idx;
		if (Name.FindLastChar(TEXT(':'), Idx)) { Name = Name.RightChop(Idx + 1); }
		return Name;
	}

	/** Check if a bone name exists in the skeleton's reference skeleton. */
	bool BoneExists(const USkeletalMesh* Mesh, const FName& BoneName)
	{
		if (!Mesh)
		{
			return false;
		}
		const FReferenceSkeleton& RefSkel = Mesh->GetRefSkeleton();
		for (int32 i = 0; i < RefSkel.GetNum(); ++i)
		{
			if (RefSkel.GetBoneName(i) == BoneName)
			{
				return true;
			}
		}
		return false;
	}

	/** The skeletal mesh at MeshPath; the error reply when it does not load. */
	TSharedPtr<FJsonObject> SkMesh_Load(const FString& MeshPath, USkeletalMesh*& OutMesh)
	{
		OutMesh = mcp_common::LoadAssetOrSoftPath<USkeletalMesh>(MeshPath);
		if (!OutMesh)
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("Could not load skeletal mesh: %s"), *MeshPath));
		}
		return nullptr;
	}

	/** A reply payload that starts with skeletal_mesh_path. */
	TSharedPtr<FJsonObject> SkMesh_NewPayload(const FString& MeshPath)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("skeletal_mesh_path"), MeshPath);
		return Data;
	}

	/** Saves the mesh's package: null when it saved, else the error reply carrying the save error. */
	TSharedPtr<FJsonObject> SkMesh_SaveOrReply(USkeletalMesh* Mesh)
	{
		FString SaveError;
		if (!mcp_common::SaveAssetPackage(Mesh->GetOutermost(), Mesh, SaveError))
		{
			return mcp_common::MakeErrorReply(SaveError);
		}
		return nullptr;
	}

#if UE_VERSION_OLDER_THAN(5, 8, 0)
	/** UE 5.7 has no USkeletalMeshEditorSubsystem::SetAllowCPUAccess: the body it has in 5.8
	 *  (SkeletalMeshEditorSubsystem.cpp), bAllowCPUAccess set on every LOD and the mesh rebuilt. */
	void SkMesh_SetAllowCPUAccess(USkeletalMesh* TargetMesh, bool bAllowCPUAccess)
	{
		TGuardValue<bool> UnattendedScriptGuard(GIsRunningUnattendedScript, true);

		if (!EditorScriptingHelpers::CheckIfInEditorAndPIE())
		{
			return;
		}

		if (TargetMesh == nullptr)
		{
			UE_LOG(LogTemp, Error, TEXT("SetAllowCPUAccess: The SkeletalMesh is null."));
			return;
		}

		TargetMesh->Modify();

		for (int32 i = 0; i < TargetMesh->GetLODNum(); ++i)
		{
			TargetMesh->GetLODInfo(i)->bAllowCPUAccess = bAllowCPUAccess;
		}

		TargetMesh->PostEditChange();
	}

	/** UE 5.7 has no USkeletalMeshEditorSubsystem::RemoveSocket: the body it has in 5.8 (SkeletalMeshEditorSubsystem.cpp),
	 *  in one undo step. The socket FindSocket resolves leaves the mesh's list (or the skeleton's, when only the
	 *  skeleton has it), a skeleton socket of the same name goes too, and the preview assets attached to that socket
	 *  name are dropped from the mesh and the skeleton. */
	bool SkMesh_RemoveSocket(USkeletalMesh* SkeletalMesh, FName SocketName)
	{
		if (SkeletalMesh == nullptr)
		{
			UE_LOG(LogTemp, Error, TEXT("RemoveSocket: The SkeletalMesh is null."));
			return false;
		}

		if (SkeletalMesh->GetSkeleton() == nullptr)
		{
			UE_LOG(LogTemp, Error, TEXT("RemoveSocket: The SkeletalMesh's Skeleton is null."));
			return false;
		}

		if (SocketName == NAME_None)
		{
			UE_LOG(LogTemp, Error, TEXT("RemoveSocket: The SocketName is None."));
			return false;
		}

		USkeletalMeshSocket* FoundSocket = SkeletalMesh->FindSocket(SocketName);
		if (FoundSocket == nullptr)
		{
			UE_LOG(LogTemp, Error, TEXT("RemoveSocket: The socket named '%s' does not exist on the SkeletalMesh."), *SocketName.ToString());
			return false;
		}

		USkeletalMeshSocket* SkeletonSocket = SkeletalMesh->GetSkeleton()->FindSocket(SocketName);
		// If they're the same pointer, FindSocket fell back to the skeleton: there is no mesh-specific socket.
		if (SkeletonSocket == FoundSocket)
		{
			SkeletonSocket = nullptr;
		}

		FScopedTransaction Transaction(NSLOCTEXT("SkeletalMeshEditorSubsystem", "RemoveSocket", "Remove Socket"));

		TArray<TObjectPtr<USkeletalMeshSocket>>& MeshSocketList = SkeletalMesh->GetMeshOnlySocketList();
		if (MeshSocketList.Contains(FoundSocket))
		{
			FoundSocket->SetFlags(RF_Transactional);
			FoundSocket->Modify();
			SkeletalMesh->Modify();
			MeshSocketList.Remove(FoundSocket);
		}
		else
		{
			// Socket exists only on the skeleton (FindSocket fell through to it).
			FoundSocket->SetFlags(RF_Transactional);
			FoundSocket->Modify();
			SkeletalMesh->GetSkeleton()->Modify();
			SkeletalMesh->GetSkeleton()->Sockets.Remove(FoundSocket);
		}

		if (SkeletonSocket != nullptr)
		{
			SkeletonSocket->SetFlags(RF_Transactional);
			SkeletonSocket->Modify();
			SkeletalMesh->GetSkeleton()->Modify();
			SkeletalMesh->GetSkeleton()->Sockets.Remove(SkeletonSocket);
		}

		// Clean up any preview-attached assets referencing the removed socket on the mesh.
		bool bMeshModified = false;
		FPreviewAssetAttachContainer& MeshPreviewContainer = SkeletalMesh->GetPreviewAttachedAssetContainer();
		for (int32 Index = MeshPreviewContainer.Num() - 1; Index >= 0; --Index)
		{
			if (MeshPreviewContainer[Index].AttachedTo == SocketName)
			{
				if (!bMeshModified)
				{
					SkeletalMesh->Modify();
					bMeshModified = true;
				}
				MeshPreviewContainer.RemoveAtSwap(Index);
			}
		}

		// Clean up any preview-attached assets referencing the removed socket on the skeleton.
		bool bSkeletonModified = false;
		FPreviewAssetAttachContainer& SkeletonPreviewContainer = SkeletalMesh->GetSkeleton()->PreviewAttachedAssetContainer;
		for (int32 Index = SkeletonPreviewContainer.Num() - 1; Index >= 0; --Index)
		{
			if (SkeletonPreviewContainer[Index].AttachedTo == SocketName)
			{
				if (!bSkeletonModified)
				{
					SkeletalMesh->GetSkeleton()->Modify();
					bSkeletonModified = true;
				}
				SkeletonPreviewContainer.RemoveAtSwap(Index);
			}
		}

		return true;
	}
#endif

	/** Why these commands must not write the package PackageName, or empty when they may: the save guard refuses a
	 *  package under an enforced read-only root, a name under no mounted content root has no file, and a package
	 *  whose file is outside the project directory is engine or engine-plugin content, which the editable-copy
	 *  commands exist to keep untouched. */
	FString SkMesh_PackageWriteBlocker(const FString& PackageName)
	{
		if (mcp_common::IsUnderReadOnlyRoot(PackageName) && mcp_common::IsReadOnlyRootSaveEnforcementEnabled())
		{
			return FString::Printf(TEXT("%s is under a read-only root"), *PackageName);
		}
		FString FileName;
		if (!FPackageName::TryConvertLongPackageNameToFilename(PackageName, FileName, FPackageName::GetAssetPackageExtension()))
		{
			return FString::Printf(TEXT("%s is not under a mounted content root"), *PackageName);
		}
		if (!FPaths::IsUnderDirectory(FileName, FPaths::ProjectDir()))
		{
			return FString::Printf(TEXT("%s is engine content outside the project, which these commands do not write"), *PackageName);
		}
		return FString();
	}

	/** Whether a preview asset of Container is attached to AttachName (a bone or socket name). */
	bool SkMesh_HasPreviewAttachedTo(const FPreviewAssetAttachContainer& Container, FName AttachName)
	{
		for (int32 Index = 0; Index < Container.Num(); ++Index)
		{
			if (Container[Index].AttachedTo == AttachName)
			{
				return true;
			}
		}
		return false;
	}

	/**
	 * The assets a rename or removal of the socket SocketName writes, the way USkeletalMeshEditorSubsystem::RenameSocket
	 * and RemoveSocket change them: the mesh when it has its own socket of that name or a preview asset attached to it,
	 * its skeleton when the skeleton has a socket of that name or a preview asset attached to it. A socket lives in
	 * the package of the asset that owns it, so that is the package to save.
	 */
	struct FSkMeshSocketOwners
	{
		USkeleton* Skeleton = nullptr;
		bool bMeshSocket = false;
		bool bSkeletonSocket = false;
		bool bMeshChanges = false;
		bool bSkeletonChanges = false;
	};

	FSkMeshSocketOwners SkMesh_SocketOwners(USkeletalMesh* Mesh, FName SocketName)
	{
		FSkMeshSocketOwners Owners;
		for (const TObjectPtr<USkeletalMeshSocket>& Socket : Mesh->GetMeshOnlySocketList())
		{
			if (Socket && Socket->SocketName == SocketName)
			{
				Owners.bMeshSocket = true;
				break;
			}
		}
		Owners.bMeshChanges = Owners.bMeshSocket || SkMesh_HasPreviewAttachedTo(Mesh->GetPreviewAttachedAssetContainer(), SocketName);
		Owners.Skeleton = Mesh->GetSkeleton();
		if (Owners.Skeleton)
		{
			Owners.bSkeletonSocket = Owners.Skeleton->FindSocket(SocketName) != nullptr;
			Owners.bSkeletonChanges = Owners.bSkeletonSocket
				|| SkMesh_HasPreviewAttachedTo(Owners.Skeleton->PreviewAttachedAssetContainer, SocketName);
		}
		return Owners;
	}

	/** Why the assets Owners says will change cannot be saved, or empty when both can: checked before the edit, so a
	 *  refusal changes nothing. */
	FString SkMesh_SocketOwnersWriteBlocker(USkeletalMesh* Mesh, const FSkMeshSocketOwners& Owners)
	{
		if (Owners.bMeshChanges)
		{
			const FString Blocker = SkMesh_PackageWriteBlocker(Mesh->GetOutermost()->GetName());
			if (!Blocker.IsEmpty())
			{
				return FString::Printf(TEXT("the mesh %s cannot be saved: %s"), *Mesh->GetPathName(), *Blocker);
			}
		}
		if (Owners.bSkeletonChanges && Owners.Skeleton)
		{
			const FString Blocker = SkMesh_PackageWriteBlocker(Owners.Skeleton->GetOutermost()->GetName());
			if (!Blocker.IsEmpty())
			{
				return FString::Printf(TEXT("its skeleton %s cannot be saved: %s"), *Owners.Skeleton->GetPathName(), *Blocker);
			}
		}
		return FString();
	}

	/** Saves the packages of the assets Owners says changed, listing each in Data's saved_assets; false with OutError
	 *  naming every asset whose save failed. */
	bool SkMesh_SaveSocketOwners(USkeletalMesh* Mesh, const FSkMeshSocketOwners& Owners, const TSharedPtr<FJsonObject>& Data,
		FString& OutError)
	{
		TArray<TSharedPtr<FJsonValue>> SavedAssets;
		TArray<FString> Failures;
		auto SaveOne = [&SavedAssets, &Failures](UObject* Asset)
		{
			FString SaveError;
			if (mcp_common::SaveAssetPackage(Asset->GetOutermost(), Asset, SaveError))
			{
				SavedAssets.Add(MakeShared<FJsonValueString>(Asset->GetPathName()));
			}
			else
			{
				Failures.Add(FString::Printf(TEXT("%s: %s"), *Asset->GetPathName(), *SaveError));
			}
		};
		if (Owners.bMeshChanges)
		{
			SaveOne(Mesh);
		}
		if (Owners.bSkeletonChanges && Owners.Skeleton)
		{
			SaveOne(Owners.Skeleton);
		}
		Data->SetArrayField(TEXT("saved_assets"), SavedAssets);
		OutError = FString::Join(Failures, TEXT("; "));
		return Failures.Num() == 0;
	}

	/** The info of LOD LodIndex into OutInfo; the error reply when the index (IndexLabel in the message) is out of
	 *  range or the LOD has no info. */
	TSharedPtr<FJsonObject> SkMesh_FindLODInfo(USkeletalMesh* Mesh, int32 LodIndex, const TCHAR* IndexLabel, FSkeletalMeshLODInfo*& OutInfo)
	{
		const int32 NumLODs = Mesh->GetLODNum();
		if (LodIndex < 0 || LodIndex >= NumLODs)
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("%s %d out of range (mesh has %d LODs)"), IndexLabel, LodIndex, NumLODs));
		}
		OutInfo = Mesh->GetLODInfo(LodIndex);
		if (!OutInfo)
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("Could not get LOD info for index %d"), LodIndex));
		}
		return nullptr;
	}

	/** Out = the {x, y, z} object Obj holds in Field (a missing component reads 0); Out is untouched when Field is absent
	 *  or not an object. */
	void SkMesh_ReadDeltaVector(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, FVector3f& Out)
	{
		const TSharedPtr<FJsonValue> Value = Obj->TryGetField(Field);
		if (!Value.IsValid())
		{
			return;
		}
		const TSharedPtr<FJsonObject> VectorObj = Value->AsObject();
		if (VectorObj.IsValid())
		{
			float X = 0, Y = 0, Z = 0;
			VectorObj->TryGetNumberField(TEXT("x"), X);
			VectorObj->TryGetNumberField(TEXT("y"), Y);
			VectorObj->TryGetNumberField(TEXT("z"), Z);
			Out = FVector3f(X, Y, Z);
		}
	}
}

// ---------------------------------------------------------------------------
// get_skeletal_mesh_info
// ---------------------------------------------------------------------------
// MCP:get_skeletal_mesh_info - Summarise a skeletal mesh asset in one call: its LOD, material slot, morph target, socket and bone counts, the names of its first 20 bones in reference-skeleton order (bone_names), and each LOD's vertex and section count. Read-only: it changes nothing; a path that does not load as a USkeletalMesh is an error. Start here, then drill into one area: get_skeletal_mesh_lod_info for one LOD's settings, get_skeletal_mesh_section_info for its sections, get_skeletal_mesh_material_slots for slot names, get_skeletal_mesh_morph_targets for morph names, and get_skeleton_info with load_full_data for bones past the first 20.
//   @param skeletal_mesh_path - Content path of the USkeletalMesh asset (object or package path), e.g. '/Game/Characters/SKM_Hero'.
//   @returns { skeletal_mesh_path: string, num_lods: int, num_materials: int, num_morph_targets: int, num_sockets: int, num_bones: int, bone_names: [string], lod_info: [{lod_index, num_verts, num_sections}] }

TSharedPtr<FJsonObject> HandleGetSkeletalMeshInfo(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MeshPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("skeletal_mesh_path"), MeshPath)) { return Missing; }

	USkeletalMesh* Mesh = nullptr;
	if (TSharedPtr<FJsonObject> Error = SkMesh_Load(MeshPath, Mesh)) { return Error; }

	const int32 NumLODs = Mesh->GetLODNum();
	const int32 NumMaterials = Mesh->GetNumMaterials();
	const int32 NumMorphTargets = Mesh->GetMorphTargets().Num();
	const int32 NumSockets = Mesh->NumSockets();
	const FReferenceSkeleton& RefSkel = Mesh->GetRefSkeleton();
	const int32 NumBones = RefSkel.GetNum();

	// Per-LOD info via editor subsystem
	USkeletalMeshEditorSubsystem* Subsys = GEditor->GetEditorSubsystem<USkeletalMeshEditorSubsystem>();

	TSharedPtr<FJsonObject> Data = SkMesh_NewPayload(MeshPath);
	Data->SetNumberField(TEXT("num_lods"), NumLODs);
	Data->SetNumberField(TEXT("num_materials"), NumMaterials);
	Data->SetNumberField(TEXT("num_morph_targets"), NumMorphTargets);
	Data->SetNumberField(TEXT("num_sockets"), NumSockets);
	Data->SetNumberField(TEXT("num_bones"), NumBones);

	// Return first ~20 bone names so callers can reference them
	TArray<TSharedPtr<FJsonValue>> BoneNamesArray;
	const int32 MaxBonesToList = FMath::Min(NumBones, 20);
	for (int32 i = 0; i < MaxBonesToList; ++i)
	{
		BoneNamesArray.Add(MakeShared<FJsonValueString>(RefSkel.GetBoneName(i).ToString()));
	}
	Data->SetArrayField(TEXT("bone_names"), BoneNamesArray);

	TArray<TSharedPtr<FJsonValue>> LodInfoArray;
	for (int32 i = 0; i < NumLODs; ++i)
	{
		TSharedPtr<FJsonObject> LodObj = MakeShared<FJsonObject>();
		LodObj->SetNumberField(TEXT("lod_index"), i);

		int32 VertCount = 0;
		int32 SectionCount = 0;
		if (Subsys)
		{
			VertCount = Subsys->GetNumVerts(Mesh, i);
			SectionCount = Subsys->GetNumSections(Mesh, i);
		}
		LodObj->SetNumberField(TEXT("num_verts"), VertCount);
		LodObj->SetNumberField(TEXT("num_sections"), SectionCount);

		LodInfoArray.Add(MakeShared<FJsonValueObject>(LodObj));
	}
	Data->SetArrayField(TEXT("lod_info"), LodInfoArray);

	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// add_skeletal_mesh_socket
// ---------------------------------------------------------------------------
// MCP:add_skeletal_mesh_socket - Add a socket on a named bone of a skeletal mesh asset (undoable) and save the mesh. A copy also goes to the mesh's skeleton, which is then saved too; when the skeleton already has a socket of that name, the mesh has no skeleton, or the skeleton is engine content or under a read-only root, no copy is made (added_to_skeleton false) and skeleton_note says why. Errors, writing nothing, when the mesh does not load, the mesh already has a socket of that name, or the bone is missing from the mesh or its skeleton. success is false when a changed package fails to save. Edit existing sockets with set_skeletal_mesh_socket_transform or rename_skeletal_mesh_socket; skeleton-only sockets with add_skeleton_socket.
//   @param skeletal_mesh_path - Content path to the skeletal mesh asset
//   @param socket_name - Name for the new socket; leading and trailing spaces are trimmed, and a name the mesh already has is an error.
//   @param bone_name - Name of the bone to attach the socket to; it must exist in the mesh and, when the copy goes to the skeleton, in the skeleton too.
//   @param location - Optional [x, y, z] relative location
//   @param rotation - Optional [pitch, yaw, roll] relative rotation
//   @returns { socket_name: string, bone_name: string, socket_count: int, location_x: float, location_y: float, location_z: float, added_to_skeleton: bool, skeleton_path: string, skeleton_note: string?, saved: bool, save_error: string?, skeleton_saved: bool?, skeleton_save_error: string? }

TSharedPtr<FJsonObject> HandleAddSkeletalMeshSocket(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MeshPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("skeletal_mesh_path"), MeshPath)) { return Missing; }
	FString SocketNameStr;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("socket_name"), SocketNameStr)) { return Missing; }
	FString BoneNameStr;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("bone_name"), BoneNameStr)) { return Missing; }

	// USkeletalMesh::AddSocket stores the trimmed name, so every check below uses it too.
	SocketNameStr.TrimStartAndEndInline();
	if (SocketNameStr.IsEmpty())
	{
		return mcp_common::MakeErrorReply(TEXT("socket_name is blank"));
	}
	const FName SocketName(*SocketNameStr);

	USkeletalMesh* Mesh = nullptr;
	if (TSharedPtr<FJsonObject> Error = SkMesh_Load(MeshPath, Mesh)) { return Error; }

	// AddSocket only logs and returns on a duplicate name, so refuse it here instead of reporting a socket that
	// was never added.
	for (const USkeletalMeshSocket* Existing : Mesh->GetMeshOnlySocketList())
	{
		if (Existing && Existing->SocketName == SocketName)
		{
			return mcp_common::MakeErrorReply(FString::Printf(
				TEXT("Skeletal mesh %s already has a socket named '%s'; nothing was written. Edit it with ")
				TEXT("set_skeletal_mesh_socket_transform or rename_skeletal_mesh_socket."),
				*Mesh->GetPathName(), *SocketNameStr));
		}
	}

	const FName BoneName(*BoneNameStr);
	if (!BoneExists(Mesh, BoneName))
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Bone '%s' not found in the reference skeleton of %s"), *BoneNameStr, *Mesh->GetPathName()));
	}

	// The copy for the skeleton. AddSocket(.., true) dereferences GetSkeleton() unchecked and validates the bone
	// against the SKELETON's reference skeleton, so it is only requested once both hold, and only for a skeleton
	// this command may save.
	USkeleton* Skeleton = Mesh->GetSkeleton();
	FString SkeletonNote;
	bool bAddToSkeleton = false;
	if (!Skeleton)
	{
		SkeletonNote = TEXT("the mesh has no skeleton, so the socket is mesh-only");
	}
	else if (Skeleton->FindSocket(SocketName))
	{
		SkeletonNote = TEXT("the skeleton already has a socket of that name, so the copy was not added");
	}
	else
	{
		const FString Blocker = SkMesh_PackageWriteBlocker(Skeleton->GetOutermost()->GetName());
		if (!Blocker.IsEmpty())
		{
			SkeletonNote = TEXT("skeleton ") + Blocker + TEXT(", so the socket is mesh-only");
		}
		else if (Skeleton->GetReferenceSkeleton().FindBoneIndex(BoneName) == INDEX_NONE)
		{
			return mcp_common::MakeErrorReply(FString::Printf(
				TEXT("Bone '%s' is in mesh %s but not in its skeleton %s, which gets a copy of the socket; nothing was written."),
				*BoneNameStr, *Mesh->GetPathName(), *Skeleton->GetPathName()));
		}
		else
		{
			bAddToSkeleton = true;
		}
	}

	USkeletalMeshSocket* Socket = NewObject<USkeletalMeshSocket>(Mesh);
	Socket->SocketName = SocketName;
	Socket->BoneName = BoneName;

	// Optional location
	if (Params->HasField(TEXT("location")))
	{
		Socket->RelativeLocation = mcp_common::ReadJsonVector(Params, TEXT("location"));
	}
	// Optional rotation
	if (Params->HasField(TEXT("rotation")))
	{
		Socket->RelativeRotation = mcp_common::ReadJsonRotator(Params, TEXT("rotation"));
	}

	// AddSocket calls Modify() on the mesh (and the skeleton) inside its own transaction before writing.
	Mesh->AddSocket(Socket, bAddToSkeleton);

	// Read back: AddSocket returns nothing and only logs when it refuses.
	if (!Mesh->GetMeshOnlySocketList().Contains(Socket))
	{
		return mcp_common::MakeErrorReply(FString::Printf(
			TEXT("The engine did not add socket '%s' to %s (see the editor log); nothing was saved."),
			*SocketNameStr, *Mesh->GetPathName()));
	}
	const bool bCopiedToSkeleton = bAddToSkeleton && Skeleton->FindSocket(SocketName) != nullptr;

	FString MeshSaveError;
	const bool bMeshSaved = mcp_common::SaveAssetPackage(Mesh->GetOutermost(), Mesh, MeshSaveError);
	FString SkeletonSaveError;
	const bool bSkeletonSaved = !bCopiedToSkeleton
		|| mcp_common::SaveAssetPackage(Skeleton->GetOutermost(), Skeleton, SkeletonSaveError);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("socket_name"), SocketNameStr);
	Data->SetStringField(TEXT("bone_name"), BoneNameStr);
	Data->SetNumberField(TEXT("socket_count"), Mesh->NumSockets());
	Data->SetNumberField(TEXT("location_x"), Socket->RelativeLocation.X);
	Data->SetNumberField(TEXT("location_y"), Socket->RelativeLocation.Y);
	Data->SetNumberField(TEXT("location_z"), Socket->RelativeLocation.Z);
	Data->SetBoolField(TEXT("added_to_skeleton"), bCopiedToSkeleton);
	Data->SetStringField(TEXT("skeleton_path"), Skeleton ? Skeleton->GetPathName() : FString());
	if (!SkeletonNote.IsEmpty())
	{
		Data->SetStringField(TEXT("skeleton_note"), SkeletonNote);
	}
	mcp_common::AddSaveOutcome(Data, bMeshSaved, MeshSaveError);
	if (bCopiedToSkeleton)
	{
		Data->SetBoolField(TEXT("skeleton_saved"), bSkeletonSaved);
		if (!bSkeletonSaved)
		{
			Data->SetStringField(TEXT("skeleton_save_error"), SkeletonSaveError);
		}
	}

	const bool bAllSaved = bMeshSaved && bSkeletonSaved;
	return mcp_common::MakeOutcomeReply(Data, bAllSaved, bAllSaved ? FString() : FString(
		TEXT("add_skeletal_mesh_socket: the socket was added in memory but a changed package did NOT reach disk. ")
		TEXT("Read saved/save_error (the mesh) and skeleton_saved/skeleton_save_error (its skeleton).")));
}

// ---------------------------------------------------------------------------
// get_skeletal_mesh_morph_targets
// ---------------------------------------------------------------------------
// MCP:get_skeletal_mesh_morph_targets - List every morph target on a skeletal mesh asset by name, with whether it holds valid data and how many vertex deltas it has on LOD 0; count is the number of morph targets. Read-only: it changes nothing; a mesh that does not load is an error. Use it to find a morph_target_name, then read one morph's per-vertex deltas on any LOD with get_morph_target_deltas or write them with set_morph_target_deltas; create_skeletal_mesh_morph_target, rename_skeletal_mesh_morph_target and remove_skeletal_mesh_morph_target manage the list itself. get_skeletal_mesh_info only counts them.
//   @param skeletal_mesh_path - Content path of the USkeletalMesh asset (object or package path), e.g. '/Game/Characters/SKM_Hero'.
//   @returns { skeletal_mesh_path: string, count: int, morph_targets: [{name, has_valid_data, num_deltas_lod0}] }

TSharedPtr<FJsonObject> HandleGetSkeletalMeshMorphTargets(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MeshPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("skeletal_mesh_path"), MeshPath)) { return Missing; }

	USkeletalMesh* Mesh = nullptr;
	if (TSharedPtr<FJsonObject> Error = SkMesh_Load(MeshPath, Mesh)) { return Error; }

	const TArray<TObjectPtr<UMorphTarget>>& MorphTargets = Mesh->GetMorphTargets();

	TSharedPtr<FJsonObject> Data = SkMesh_NewPayload(MeshPath);
	Data->SetNumberField(TEXT("count"), MorphTargets.Num());

	TArray<TSharedPtr<FJsonValue>> MTArray;
	for (const UMorphTarget* MT : MorphTargets)
	{
		if (!MT)
		{
			continue;
		}
		TSharedPtr<FJsonObject> MTObj = MakeShared<FJsonObject>();
		MTObj->SetStringField(TEXT("name"), MT->GetName());
		MTObj->SetBoolField(TEXT("has_valid_data"), MT->HasValidData());
		MTObj->SetNumberField(TEXT("num_deltas_lod0"), MT->GetNumDeltasForLOD(0));
		MTArray.Add(MakeShared<FJsonValueObject>(MTObj));
	}
	Data->SetArrayField(TEXT("morph_targets"), MTArray);

	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_skeletal_mesh_lod_info
// ---------------------------------------------------------------------------
// MCP:get_skeletal_mesh_lod_info - Read the settings of one LOD of a skeletal mesh asset found by content path: screen size, material map entry count, CPU access, the build settings, the reduction settings and the flat LOD scalars (hysteresis, prioritization weight, morph error tolerance, skin cache usage, mesh deformer). Enums come back by name, e.g. "SMTC_NumOfTriangles" or "Auto", a form the setters accept. Nothing is written. A mesh that does not load, or a lod_index outside its LOD count, is an error. Each block has its setter: set_skeletal_mesh_lod_build_settings, set_skeletal_mesh_lod_reduction_settings, and set_skeletal_mesh_lod_settings (screen_size included). Per-LOD vertex counts: get_skeletal_mesh_info; triangle counts: get_asset_lod_settings; sections: get_skeletal_mesh_section_info.
//   @param skeletal_mesh_path - Content path to the skeletal mesh asset, e.g. "/Game/Characters/SKM_Hero"
//   @param lod_index - 0-based LOD index to read (default 0); it must be below the LOD count of the mesh
//   @returns { lod_index: int, screen_size: float, num_material_map_entries: int, allow_cpu_access: bool, build_settings: {...}, reduction_settings: {...}, lod_hysteresis: float, weight_of_prioritization: float, morph_target_position_error_tolerance: float, skin_cache_usage: string, allow_mesh_deformer: bool }

TSharedPtr<FJsonObject> HandleGetSkeletalMeshLodInfo(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MeshPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("skeletal_mesh_path"), MeshPath)) { return Missing; }

	int32 LodIndex = 0;
	Params->TryGetNumberField(TEXT("lod_index"), LodIndex);

	USkeletalMesh* Mesh = nullptr;
	if (TSharedPtr<FJsonObject> Error = SkMesh_Load(MeshPath, Mesh)) { return Error; }

	FSkeletalMeshLODInfo* LODInfo = nullptr;
	if (TSharedPtr<FJsonObject> Error = SkMesh_FindLODInfo(Mesh, LodIndex, TEXT("LOD index"), LODInfo)) { return Error; }

	const FSkeletalMeshBuildSettings& BS = LODInfo->BuildSettings;

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("lod_index"), LodIndex);
	Data->SetNumberField(TEXT("screen_size"), LODInfo->ScreenSize.Default);
	Data->SetNumberField(TEXT("num_material_map_entries"), LODInfo->LODMaterialMap.Num());
	Data->SetBoolField(TEXT("allow_cpu_access"), LODInfo->bAllowCPUAccess != 0);

	TSharedPtr<FJsonObject> BuildSettingsObj = MakeShared<FJsonObject>();
	BuildSettingsObj->SetBoolField(TEXT("recompute_normals"), BS.bRecomputeNormals != 0);
	BuildSettingsObj->SetBoolField(TEXT("recompute_tangents"), BS.bRecomputeTangents != 0);
	BuildSettingsObj->SetBoolField(TEXT("use_mikk_t_space"), BS.bUseMikkTSpace != 0);
	BuildSettingsObj->SetBoolField(TEXT("compute_weighted_normals"), BS.bComputeWeightedNormals != 0);
	BuildSettingsObj->SetBoolField(TEXT("remove_degenerates"), BS.bRemoveDegenerates != 0);
	BuildSettingsObj->SetBoolField(TEXT("high_precision_tangents"), BS.bUseHighPrecisionTangentBasis != 0);
	BuildSettingsObj->SetBoolField(TEXT("high_precision_weights"), BS.bUseHighPrecisionSkinWeights != 0);
	BuildSettingsObj->SetBoolField(TEXT("full_precision_uvs"), BS.bUseFullPrecisionUVs != 0);
	BuildSettingsObj->SetNumberField(TEXT("bone_influence_limit"), BS.BoneInfluenceLimit);
	Data->SetObjectField(TEXT("build_settings"), BuildSettingsObj);

	// Reduction settings (what bake_skeletal_mesh_lod consumes).
	const FSkeletalMeshOptimizationSettings& RS = LODInfo->ReductionSettings;
	TSharedPtr<FJsonObject> ReductionObj = MakeShared<FJsonObject>();
	ReductionObj->SetStringField(TEXT("termination_criterion"), EnumToName<SkeletalMeshTerminationCriterion>(RS.TerminationCriterion.GetValue()));
	ReductionObj->SetStringField(TEXT("reduction_method"), EnumToName<SkeletalMeshOptimizationType>(RS.ReductionMethod.GetValue()));
	ReductionObj->SetNumberField(TEXT("num_of_triangles_percentage"), RS.NumOfTrianglesPercentage);
	ReductionObj->SetNumberField(TEXT("num_of_vert_percentage"), RS.NumOfVertPercentage);
	ReductionObj->SetNumberField(TEXT("max_deviation_percentage"), RS.MaxDeviationPercentage);
	ReductionObj->SetNumberField(TEXT("max_num_of_triangles"), static_cast<double>(RS.MaxNumOfTriangles));
	ReductionObj->SetNumberField(TEXT("max_num_of_verts"), static_cast<double>(RS.MaxNumOfVerts));
	ReductionObj->SetNumberField(TEXT("max_num_of_triangles_percentage"), static_cast<double>(RS.MaxNumOfTrianglesPercentage));
	ReductionObj->SetNumberField(TEXT("max_num_of_verts_percentage"), static_cast<double>(RS.MaxNumOfVertsPercentage));
	ReductionObj->SetNumberField(TEXT("base_lod"), RS.BaseLOD);
	ReductionObj->SetStringField(TEXT("silhouette_importance"), EnumToName<SkeletalMeshOptimizationImportance>(RS.SilhouetteImportance.GetValue()));
	ReductionObj->SetStringField(TEXT("texture_importance"), EnumToName<SkeletalMeshOptimizationImportance>(RS.TextureImportance.GetValue()));
	ReductionObj->SetStringField(TEXT("shading_importance"), EnumToName<SkeletalMeshOptimizationImportance>(RS.ShadingImportance.GetValue()));
	ReductionObj->SetStringField(TEXT("skinning_importance"), EnumToName<SkeletalMeshOptimizationImportance>(RS.SkinningImportance.GetValue()));
	ReductionObj->SetNumberField(TEXT("max_bones_per_vertex"), RS.MaxBonesPerVertex);
	ReductionObj->SetNumberField(TEXT("welding_threshold"), RS.WeldingThreshold);
	ReductionObj->SetNumberField(TEXT("normals_threshold"), RS.NormalsThreshold);
	ReductionObj->SetBoolField(TEXT("recalc_normals"), RS.bRecalcNormals != 0);
	ReductionObj->SetBoolField(TEXT("remap_morph_targets"), RS.bRemapMorphTargets != 0);
	ReductionObj->SetBoolField(TEXT("enforce_bone_boundaries"), RS.bEnforceBoneBoundaries != 0);
	Data->SetObjectField(TEXT("reduction_settings"), ReductionObj);

	// Flat LODInfo scalars/enums.
	Data->SetNumberField(TEXT("lod_hysteresis"), LODInfo->LODHysteresis);
	Data->SetNumberField(TEXT("weight_of_prioritization"), LODInfo->WeightOfPrioritization);
	Data->SetNumberField(TEXT("morph_target_position_error_tolerance"), LODInfo->MorphTargetPositionErrorTolerance);
	Data->SetStringField(TEXT("skin_cache_usage"), EnumToName<ESkinCacheUsage>(LODInfo->SkinCacheUsage));
	Data->SetBoolField(TEXT("allow_mesh_deformer"), LODInfo->bAllowMeshDeformer != 0);

	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_skeletal_mesh_lod_build_settings
// ---------------------------------------------------------------------------
// MCP:set_skeletal_mesh_lod_build_settings - Change the stored build settings of one LOD of a skeletal mesh asset, only the fields you pass, and save the mesh. The mesh is not rebuilt here, so the settings take effect at its next build or reimport; a failed save returns an error. Returns settings_applied.
//   @param skeletal_mesh_path - Skeletal mesh asset path, e.g. "/Game/Characters/SKM_Hero".
//   @param lod_index - 0-based LOD index (default 0); an index the mesh does not have fails the call.
//   @param recompute_normals - Ignore the imported normals and recompute them (bRecomputeNormals).
//   @param recompute_tangents - Ignore the imported tangents and recompute them (bRecomputeTangents).
//   @param use_mikk_t_space - Use MikkTSpace when computing tangents (bUseMikkTSpace).
//   @param compute_weighted_normals - Weight computed normals by triangle area and corner angle (bComputeWeightedNormals).
//   @param remove_degenerates - Remove degenerate triangles at build time (bRemoveDegenerates).
//   @param high_precision_tangents - Store tangents at 16-bit instead of 8-bit precision (bUseHighPrecisionTangentBasis).
//   @param high_precision_weights - Use 16-bit instead of 8-bit skin weights for rendering (bUseHighPrecisionSkinWeights).
//   @param full_precision_uvs - Store UVs at full floating-point precision (bUseFullPrecisionUVs).
//   @param bone_influence_limit - Maximum bone influences per vertex; 0 uses the DefaultBoneInfluenceLimit project setting, and values above the project limit have no effect.
//   @returns { lod_index: int, settings_applied: {...}, saved: bool }

TSharedPtr<FJsonObject> HandleSetSkeletalMeshLodBuildSettings(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MeshPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("skeletal_mesh_path"), MeshPath)) { return Missing; }

	int32 LodIndex = 0;
	Params->TryGetNumberField(TEXT("lod_index"), LodIndex);

	USkeletalMesh* Mesh = nullptr;
	if (TSharedPtr<FJsonObject> Error = SkMesh_Load(MeshPath, Mesh)) { return Error; }

	FSkeletalMeshLODInfo* LODInfo = nullptr;
	if (TSharedPtr<FJsonObject> Error = SkMesh_FindLODInfo(Mesh, LodIndex, TEXT("LOD index"), LODInfo)) { return Error; }

	FSkeletalMeshBuildSettings& BS = LODInfo->BuildSettings;
	TSharedPtr<FJsonObject> AppliedObj = MakeShared<FJsonObject>();

	// Apply each optional bool setting
	bool bRecomputeNormals;
	if (Params->TryGetBoolField(TEXT("recompute_normals"), bRecomputeNormals))
	{
		BS.bRecomputeNormals = bRecomputeNormals ? 1 : 0;
		AppliedObj->SetBoolField(TEXT("recompute_normals"), bRecomputeNormals);
	}
	bool bRecomputeTangents;
	if (Params->TryGetBoolField(TEXT("recompute_tangents"), bRecomputeTangents))
	{
		BS.bRecomputeTangents = bRecomputeTangents ? 1 : 0;
		AppliedObj->SetBoolField(TEXT("recompute_tangents"), bRecomputeTangents);
	}
	bool bUseMikkTSpace;
	if (Params->TryGetBoolField(TEXT("use_mikk_t_space"), bUseMikkTSpace))
	{
		BS.bUseMikkTSpace = bUseMikkTSpace ? 1 : 0;
		AppliedObj->SetBoolField(TEXT("use_mikk_t_space"), bUseMikkTSpace);
	}
	bool bComputeWeightedNormals;
	if (Params->TryGetBoolField(TEXT("compute_weighted_normals"), bComputeWeightedNormals))
	{
		BS.bComputeWeightedNormals = bComputeWeightedNormals ? 1 : 0;
		AppliedObj->SetBoolField(TEXT("compute_weighted_normals"), bComputeWeightedNormals);
	}
	bool bRemoveDegenerates;
	if (Params->TryGetBoolField(TEXT("remove_degenerates"), bRemoveDegenerates))
	{
		BS.bRemoveDegenerates = bRemoveDegenerates ? 1 : 0;
		AppliedObj->SetBoolField(TEXT("remove_degenerates"), bRemoveDegenerates);
	}
	bool bHighPrecisionTangents;
	if (Params->TryGetBoolField(TEXT("high_precision_tangents"), bHighPrecisionTangents))
	{
		BS.bUseHighPrecisionTangentBasis = bHighPrecisionTangents ? 1 : 0;
		AppliedObj->SetBoolField(TEXT("high_precision_tangents"), bHighPrecisionTangents);
	}
	bool bHighPrecisionWeights;
	if (Params->TryGetBoolField(TEXT("high_precision_weights"), bHighPrecisionWeights))
	{
		BS.bUseHighPrecisionSkinWeights = bHighPrecisionWeights ? 1 : 0;
		AppliedObj->SetBoolField(TEXT("high_precision_weights"), bHighPrecisionWeights);
	}
	bool bFullPrecisionUVs;
	if (Params->TryGetBoolField(TEXT("full_precision_uvs"), bFullPrecisionUVs))
	{
		BS.bUseFullPrecisionUVs = bFullPrecisionUVs ? 1 : 0;
		AppliedObj->SetBoolField(TEXT("full_precision_uvs"), bFullPrecisionUVs);
	}
	int32 BoneInfluenceLimit;
	if (Params->TryGetNumberField(TEXT("bone_influence_limit"), BoneInfluenceLimit))
	{
		BS.BoneInfluenceLimit = BoneInfluenceLimit;
		AppliedObj->SetNumberField(TEXT("bone_influence_limit"), BoneInfluenceLimit);
	}

	Mesh->Modify();

	if (TSharedPtr<FJsonObject> SaveFailed = SkMesh_SaveOrReply(Mesh))
	{
		return SaveFailed;
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("lod_index"), LodIndex);
	Data->SetObjectField(TEXT("settings_applied"), AppliedObj);
	Data->SetBoolField(TEXT("saved"), true);

	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_skeletal_mesh_lod_reduction_settings
// ---------------------------------------------------------------------------
// MCP:set_skeletal_mesh_lod_reduction_settings - Author a LOD's FSkeletalMeshOptimizationSettings (ReductionSettings) — the data bake_skeletal_mesh_lod consumes, making the bake steerable instead of running with defaults.  Partial update (idempotent): only fields present are written. Enums are by name (full "SMTC_NumOfTriangles" or short "NumOfTriangles"). termination_criterion (SMTC_*) and reduction_method (SMOT_*) are DISTINCT.
//   @param skeletal_mesh_path - Content path to the skeletal mesh asset.
//   @param lod_index - LOD index to modify (default 0).
//   @param termination_criterion - SMTC_*: NumOfTriangles|NumOfVerts|TriangleOrVert|AbsNumOfTriangles|AbsNumOfVerts|AbsTriangleOrVert.
//   @param reduction_method - SMOT_*: NumOfTriangles|MaxDeviation|TriangleOrDeviation.
//   @param num_of_triangles_percentage - float ratio of triangles to retain (0.1 = 10%).
//   @param num_of_vert_percentage - float ratio of vertices to retain.
//   @param max_deviation_percentage - float [0,1], accuracy target when reduction_method=MaxDeviation.
//   @param max_num_of_triangles - int absolute triangle cap (ClampMin 4).
//   @param max_num_of_verts - int absolute vertex cap (ClampMin 6).
//   @param max_num_of_triangles_percentage - int (editor-only) cap when using percentage criterion (ClampMin 4).
//   @param max_num_of_verts_percentage - int (editor-only) cap when using percentage criterion (ClampMin 6).
//   @param base_lod - int base LOD index to generate from (default 0).
//   @param silhouette_importance - SMOI_*: Off|Lowest|Low|Normal|High|Highest.
//   @param texture_importance - SMOI_* importance of texture density.
//   @param shading_importance - SMOI_* importance of shading quality.
//   @param skinning_importance - SMOI_* importance of skinning quality.
//   @param max_bones_per_vertex - int max bone influences per vertex (ClampMin 1).
//   @param welding_threshold - float vertex weld distance.
//   @param normals_threshold - float hard-edge angle in degrees (used when recalc_normals).
//   @param recalc_normals - bool recompute normals.
//   @param remap_morph_targets - bool remap morph targets onto the reduced LOD.
//   @param enforce_bone_boundaries - bool penalize collapses across different major bones.
//   @returns { lod_index: int, settings_applied: {...}, saved: bool }

TSharedPtr<FJsonObject> HandleSetSkeletalMeshLodReductionSettings(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MeshPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("skeletal_mesh_path"), MeshPath)) { return Missing; }

	int32 LodIndex = 0;
	Params->TryGetNumberField(TEXT("lod_index"), LodIndex);

	USkeletalMesh* Mesh = nullptr;
	if (TSharedPtr<FJsonObject> Error = SkMesh_Load(MeshPath, Mesh)) { return Error; }

	FSkeletalMeshLODInfo* LODInfo = nullptr;
	if (TSharedPtr<FJsonObject> Error = SkMesh_FindLODInfo(Mesh, LodIndex, TEXT("LOD index"), LODInfo)) { return Error; }

	FSkeletalMeshOptimizationSettings& RS = LODInfo->ReductionSettings;
	TSharedPtr<FJsonObject> AppliedObj = MakeShared<FJsonObject>();
	FString S; double Num; bool B; int32 I;

	if (Params->TryGetStringField(TEXT("termination_criterion"), S))
	{
		SkeletalMeshTerminationCriterion E;
		if (!ParseEnumByName(S, TEXT("SMTC_"), E))
			return mcp_common::MakeErrorReply(FString::Printf(
				TEXT("Unknown termination_criterion: %s (NumOfTriangles/NumOfVerts/TriangleOrVert/AbsNumOfTriangles/AbsNumOfVerts/AbsTriangleOrVert)"), *S));
		RS.TerminationCriterion = E;
		AppliedObj->SetStringField(TEXT("termination_criterion"), EnumToName(E));
	}
	if (Params->TryGetStringField(TEXT("reduction_method"), S))
	{
		SkeletalMeshOptimizationType E;
		if (!ParseEnumByName(S, TEXT("SMOT_"), E))
			return mcp_common::MakeErrorReply(FString::Printf(
				TEXT("Unknown reduction_method: %s (NumOfTriangles/MaxDeviation/TriangleOrDeviation)"), *S));
		RS.ReductionMethod = E;
		AppliedObj->SetStringField(TEXT("reduction_method"), EnumToName(E));
	}
	if (Params->TryGetNumberField(TEXT("num_of_triangles_percentage"), Num)) { RS.NumOfTrianglesPercentage = static_cast<float>(Num); AppliedObj->SetNumberField(TEXT("num_of_triangles_percentage"), Num); }
	if (Params->TryGetNumberField(TEXT("num_of_vert_percentage"), Num)) { RS.NumOfVertPercentage = static_cast<float>(Num); AppliedObj->SetNumberField(TEXT("num_of_vert_percentage"), Num); }
	if (Params->TryGetNumberField(TEXT("max_deviation_percentage"), Num)) { RS.MaxDeviationPercentage = FMath::Clamp(static_cast<float>(Num), 0.f, 1.f); AppliedObj->SetNumberField(TEXT("max_deviation_percentage"), RS.MaxDeviationPercentage); }
	if (Params->TryGetNumberField(TEXT("max_num_of_triangles"), Num)) { RS.MaxNumOfTriangles = static_cast<uint32>(FMath::Max(4.0, Num)); AppliedObj->SetNumberField(TEXT("max_num_of_triangles"), static_cast<double>(RS.MaxNumOfTriangles)); }
	if (Params->TryGetNumberField(TEXT("max_num_of_verts"), Num)) { RS.MaxNumOfVerts = static_cast<uint32>(FMath::Max(6.0, Num)); AppliedObj->SetNumberField(TEXT("max_num_of_verts"), static_cast<double>(RS.MaxNumOfVerts)); }
	if (Params->TryGetNumberField(TEXT("max_num_of_triangles_percentage"), Num)) { RS.MaxNumOfTrianglesPercentage = static_cast<uint32>(FMath::Max(4.0, Num)); AppliedObj->SetNumberField(TEXT("max_num_of_triangles_percentage"), static_cast<double>(RS.MaxNumOfTrianglesPercentage)); }
	if (Params->TryGetNumberField(TEXT("max_num_of_verts_percentage"), Num)) { RS.MaxNumOfVertsPercentage = static_cast<uint32>(FMath::Max(6.0, Num)); AppliedObj->SetNumberField(TEXT("max_num_of_verts_percentage"), static_cast<double>(RS.MaxNumOfVertsPercentage)); }
	if (Params->TryGetNumberField(TEXT("base_lod"), I)) { RS.BaseLOD = I; AppliedObj->SetNumberField(TEXT("base_lod"), I); }
	if (Params->TryGetStringField(TEXT("silhouette_importance"), S)) { SkeletalMeshOptimizationImportance E; if (!ParseEnumByName(S, TEXT("SMOI_"), E)) return mcp_common::MakeErrorReply(FString::Printf(TEXT("Unknown silhouette_importance: %s (Off/Lowest/Low/Normal/High/Highest)"), *S)); RS.SilhouetteImportance = E; AppliedObj->SetStringField(TEXT("silhouette_importance"), EnumToName(E)); }
	if (Params->TryGetStringField(TEXT("texture_importance"), S)) { SkeletalMeshOptimizationImportance E; if (!ParseEnumByName(S, TEXT("SMOI_"), E)) return mcp_common::MakeErrorReply(FString::Printf(TEXT("Unknown texture_importance: %s (Off/Lowest/Low/Normal/High/Highest)"), *S)); RS.TextureImportance = E; AppliedObj->SetStringField(TEXT("texture_importance"), EnumToName(E)); }
	if (Params->TryGetStringField(TEXT("shading_importance"), S)) { SkeletalMeshOptimizationImportance E; if (!ParseEnumByName(S, TEXT("SMOI_"), E)) return mcp_common::MakeErrorReply(FString::Printf(TEXT("Unknown shading_importance: %s (Off/Lowest/Low/Normal/High/Highest)"), *S)); RS.ShadingImportance = E; AppliedObj->SetStringField(TEXT("shading_importance"), EnumToName(E)); }
	if (Params->TryGetStringField(TEXT("skinning_importance"), S)) { SkeletalMeshOptimizationImportance E; if (!ParseEnumByName(S, TEXT("SMOI_"), E)) return mcp_common::MakeErrorReply(FString::Printf(TEXT("Unknown skinning_importance: %s (Off/Lowest/Low/Normal/High/Highest)"), *S)); RS.SkinningImportance = E; AppliedObj->SetStringField(TEXT("skinning_importance"), EnumToName(E)); }
	if (Params->TryGetNumberField(TEXT("max_bones_per_vertex"), I)) { RS.MaxBonesPerVertex = FMath::Max(1, I); AppliedObj->SetNumberField(TEXT("max_bones_per_vertex"), RS.MaxBonesPerVertex); }
	if (Params->TryGetNumberField(TEXT("welding_threshold"), Num)) { RS.WeldingThreshold = static_cast<float>(Num); AppliedObj->SetNumberField(TEXT("welding_threshold"), Num); }
	if (Params->TryGetNumberField(TEXT("normals_threshold"), Num)) { RS.NormalsThreshold = static_cast<float>(Num); AppliedObj->SetNumberField(TEXT("normals_threshold"), Num); }
	if (Params->TryGetBoolField(TEXT("recalc_normals"), B)) { RS.bRecalcNormals = B ? 1 : 0; AppliedObj->SetBoolField(TEXT("recalc_normals"), B); }
	if (Params->TryGetBoolField(TEXT("remap_morph_targets"), B)) { RS.bRemapMorphTargets = B ? 1 : 0; AppliedObj->SetBoolField(TEXT("remap_morph_targets"), B); }
	if (Params->TryGetBoolField(TEXT("enforce_bone_boundaries"), B)) { RS.bEnforceBoneBoundaries = B ? 1 : 0; AppliedObj->SetBoolField(TEXT("enforce_bone_boundaries"), B); }

	Mesh->Modify();
	if (TSharedPtr<FJsonObject> SaveFailed = SkMesh_SaveOrReply(Mesh))
		return SaveFailed;

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("lod_index"), LodIndex);
	Data->SetObjectField(TEXT("settings_applied"), AppliedObj);
	Data->SetBoolField(TEXT("saved"), true);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_skeletal_mesh_lod_settings
// ---------------------------------------------------------------------------
// MCP:set_skeletal_mesh_lod_settings - Set the flat per-LOD scalars/enums of FSkeletalMeshLODInfo that no other setter covers (screen size, hysteresis, CPU access, morph error tolerance, skin cache usage, mesh deformer, prioritization weight) on one LOD or, with lod_index -1 or "all", on every LOD of the mesh, then save the mesh. Partial update (idempotent): only fields present are written; a bad skin_cache_usage or lod_index is refused before anything is written. A failed save is an error that leaves the change in memory. Per-platform screen-size overrides are left as they are.
//   @param skeletal_mesh_path - Content path to the skeletal mesh asset.
//   @param lod_index - Optional. 0-based LOD index to modify (default 0), or -1 / "all" for every LOD. allow_cpu_access with every LOD goes through USkeletalMeshEditorSubsystem::SetAllowCPUAccess.
//   @param screen_size - Optional float, ScreenSize.Default: the screen size at which this LOD switches in (e.g. 0.3).
//   @param lod_hysteresis - float, avoids flickering at the LOD boundary.
//   @param allow_cpu_access - bool, keep this LOD's data on the CPU (e.g. for FX sampling).
//   @param morph_target_position_error_tolerance - float in microns, Clamp 0.01..10000 (default 20).
//   @param skin_cache_usage - ESkinCacheUsage by NAME: Auto|Disabled|Enabled (Disabled=255, parsed by name).
//   @param allow_mesh_deformer - bool, whether a Mesh Deformer is used on this LOD.
//   @param weight_of_prioritization - float >=0, weight given to BonesToPrioritize/SectionsToPrioritize.
//   @returns { skeletal_mesh_path: string, lod_index: int (-1 for every LOD), all_lods: bool, applied_lods: int, settings_applied: {...}, old_screen_size?: float (one LOD), old_screen_sizes?: [float] (every LOD), saved: bool }

TSharedPtr<FJsonObject> HandleSetSkeletalMeshLodSettings(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MeshPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("skeletal_mesh_path"), MeshPath)) { return Missing; }

	// lod_index: a 0-based LOD (a JSON number or a numeric string), or -1 / "all" for every LOD.
	int32 LodIndex = 0;
	FString LodIndexText;
	if (Params->TryGetStringField(TEXT("lod_index"), LodIndexText))
	{
		LodIndexText.TrimStartAndEndInline();
		if (LodIndexText.Equals(TEXT("all"), ESearchCase::IgnoreCase))
		{
			LodIndex = INDEX_NONE;
		}
		else if (LodIndexText.IsNumeric())
		{
			LodIndex = FCString::Atoi(*LodIndexText);
		}
		else
		{
			return mcp_common::MakeErrorReply(FString::Printf(
				TEXT("lod_index must be a 0-based LOD index, -1 or \"all\" (got \"%s\")"), *LodIndexText));
		}
	}
	else
	{
		Params->TryGetNumberField(TEXT("lod_index"), LodIndex);
	}
	const bool bAllLods = LodIndex == INDEX_NONE;

	// Values are read and validated before anything is written.
	double ScreenSize = 0.0, Hysteresis = 0.0, Weight = 0.0, Tolerance = 0.0;
	bool bCpuAccess = false, bMeshDeformer = false;
	const bool bHasScreenSize = Params->TryGetNumberField(TEXT("screen_size"), ScreenSize);
	const bool bHasHysteresis = Params->TryGetNumberField(TEXT("lod_hysteresis"), Hysteresis);
	const bool bHasWeight = Params->TryGetNumberField(TEXT("weight_of_prioritization"), Weight);
	const bool bHasTolerance = Params->TryGetNumberField(TEXT("morph_target_position_error_tolerance"), Tolerance);
	const bool bHasCpuAccess = Params->TryGetBoolField(TEXT("allow_cpu_access"), bCpuAccess);
	const bool bHasMeshDeformer = Params->TryGetBoolField(TEXT("allow_mesh_deformer"), bMeshDeformer);
	FString SkinCacheText;
	ESkinCacheUsage SkinCache = ESkinCacheUsage::Auto;
	const bool bHasSkinCache = Params->TryGetStringField(TEXT("skin_cache_usage"), SkinCacheText);
	if (bHasSkinCache && !ParseEnumByName(SkinCacheText, TEXT("ESkinCacheUsage::"), SkinCache))
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Unknown skin_cache_usage: %s (Auto/Disabled/Enabled)"), *SkinCacheText));
	}

	USkeletalMesh* Mesh = nullptr;
	if (TSharedPtr<FJsonObject> Error = SkMesh_Load(MeshPath, Mesh)) { return Error; }

	// The LODs this call writes: the one asked for (refused when out of range), or every LOD.
	TArray<FSkeletalMeshLODInfo*> Infos;
	if (bAllLods)
	{
		for (int32 Index = 0; Index < Mesh->GetLODNum(); ++Index)
		{
			if (FSkeletalMeshLODInfo* Info = Mesh->GetLODInfo(Index))
			{
				Infos.Add(Info);
			}
		}
	}
	else
	{
		FSkeletalMeshLODInfo* LODInfo = nullptr;
		if (TSharedPtr<FJsonObject> Error = SkMesh_FindLODInfo(Mesh, LodIndex, TEXT("LOD index"), LODInfo)) { return Error; }
		Infos.Add(LODInfo);
	}

	Mesh->Modify();
	TSharedPtr<FJsonObject> AppliedObj = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> OldScreenSizes;
	for (FSkeletalMeshLODInfo* LODInfo : Infos)
	{
		if (bHasScreenSize)
		{
			OldScreenSizes.Add(MakeShared<FJsonValueNumber>(LODInfo->ScreenSize.Default));
			LODInfo->ScreenSize.Default = static_cast<float>(ScreenSize);
			AppliedObj->SetNumberField(TEXT("screen_size"), LODInfo->ScreenSize.Default);
		}
		if (bHasHysteresis) { LODInfo->LODHysteresis = static_cast<float>(Hysteresis); AppliedObj->SetNumberField(TEXT("lod_hysteresis"), Hysteresis); }
		if (bHasWeight) { LODInfo->WeightOfPrioritization = FMath::Max(0.f, static_cast<float>(Weight)); AppliedObj->SetNumberField(TEXT("weight_of_prioritization"), LODInfo->WeightOfPrioritization); }
		if (bHasTolerance) { LODInfo->MorphTargetPositionErrorTolerance = FMath::Clamp(static_cast<float>(Tolerance), 0.01f, 10000.f); AppliedObj->SetNumberField(TEXT("morph_target_position_error_tolerance"), LODInfo->MorphTargetPositionErrorTolerance); }
		// Every LOD's CPU access goes through the editor subsystem below instead.
		if (bHasCpuAccess && !bAllLods) { LODInfo->bAllowCPUAccess = bCpuAccess ? 1 : 0; AppliedObj->SetBoolField(TEXT("allow_cpu_access"), bCpuAccess); }
		if (bHasMeshDeformer) { LODInfo->bAllowMeshDeformer = bMeshDeformer ? 1 : 0; AppliedObj->SetBoolField(TEXT("allow_mesh_deformer"), bMeshDeformer); }
		if (bHasSkinCache) { LODInfo->SkinCacheUsage = SkinCache; AppliedObj->SetStringField(TEXT("skin_cache_usage"), EnumToName(SkinCache)); }
	}
	if (bHasCpuAccess && bAllLods)
	{
#if UE_VERSION_OLDER_THAN(5, 8, 0)
		SkMesh_SetAllowCPUAccess(Mesh, bCpuAccess);
#else
		USkeletalMeshEditorSubsystem::SetAllowCPUAccess(Mesh, bCpuAccess);
#endif
		AppliedObj->SetBoolField(TEXT("allow_cpu_access"), bCpuAccess);
	}

	if (TSharedPtr<FJsonObject> SaveFailed = SkMesh_SaveOrReply(Mesh))
		return SaveFailed;

	TSharedPtr<FJsonObject> Data = SkMesh_NewPayload(MeshPath);
	Data->SetNumberField(TEXT("lod_index"), LodIndex);
	Data->SetBoolField(TEXT("all_lods"), bAllLods);
	Data->SetNumberField(TEXT("applied_lods"), Infos.Num());
	Data->SetObjectField(TEXT("settings_applied"), AppliedObj);
	if (bHasScreenSize)
	{
		if (bAllLods)
		{
			Data->SetArrayField(TEXT("old_screen_sizes"), OldScreenSizes);
		}
		else if (OldScreenSizes.Num() > 0)
		{
			Data->SetField(TEXT("old_screen_size"), OldScreenSizes[0]);
		}
	}
	Data->SetBoolField(TEXT("saved"), true);
	return mcp_common::MakeOkReply(Data);
}

// ===========================================================================
// Morph target + section commands

// ---------------------------------------------------------------------------
// create_skeletal_mesh_morph_target
// ---------------------------------------------------------------------------
// MCP:create_skeletal_mesh_morph_target - Register a new morph target on a skeletal mesh asset in memory only, for the rest of the editor session: it is never saved (persisted_to_disk is always false), and a mesh rebuild or reload drops any morph the mesh's imported source data does not contain. Each LOD gets one tiny seed delta on vertex 0 so the morph counts as valid; write real deltas with set_morph_target_deltas. Marks the mesh dirty; it is not an undo step. Idempotent: an existing name returns created=false and already_exists=true.
//   @param skeletal_mesh_path - Content path to the skeletal mesh asset (e.g. /Game/MyGame/SKM_Example)
//   @param morph_target_name - Name for the new morph target
//   @returns { morph_target_name: string, skeletal_mesh_path: string, created: bool, persisted_to_disk: bool, total_morph_targets: int, note: string }

TSharedPtr<FJsonObject> HandleCreateSkeletalMeshMorphTarget(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MeshPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("skeletal_mesh_path"), MeshPath)) { return Missing; }

	FString MorphName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("morph_target_name"), MorphName)) { return Missing; }

	USkeletalMesh* Mesh = nullptr;
	if (TSharedPtr<FJsonObject> Error = SkMesh_Load(MeshPath, Mesh)) { return Error; }

	const FName MorphFName(*MorphName);

	// Idempotent: if morph target already exists, return success
	UMorphTarget* ExistingMT = Mesh->FindMorphTarget(MorphFName);
	if (ExistingMT)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("morph_target_name"), MorphName);
		Data->SetStringField(TEXT("skeletal_mesh_path"), MeshPath);
		Data->SetBoolField(TEXT("created"), false);
		Data->SetBoolField(TEXT("already_exists"), true);
		return mcp_common::MakeOkReply(Data);
	}

	// Create new morph target with the requested name so FindMorphTarget can locate it.
	// If a stale object with the same name exists in memory (from a prior run), NewObject
	// would auto-rename it. To prevent that, rename the stale object out of the way first.
	if (UObject* StaleObj = StaticFindObject(UMorphTarget::StaticClass(), Mesh, *MorphName))
	{
		mcp_common::RenameToFreeName(StaleObj, FString::Printf(TEXT("%s_OLD"), *MorphName),
			nullptr, REN_DontCreateRedirectors | REN_NonTransactional);
	}
	UMorphTarget* NewMT = NewObject<UMorphTarget>(Mesh, UMorphTarget::StaticClass(), MorphFName);
	NewMT->BaseSkelMesh = Mesh;
	NewMT->SetFlags(RF_Public | RF_Transactional);

	// -------------------------------------------------------------------------
	// WHY THIS DOES NOT (AND CANNOT) PERSIST TO DISK FOR A COOKED-ENGINE-MESH
	// DUPLICATE LIKE /Game/MyGame/SKM_Example, AND WHAT WE DO INSTEAD.
	//
	// Traced against the engine source. A runtime-injected UMorphTarget is destroyed
	// by ANY skeletal-mesh build, because the build regenerates the entire morph
	// set from the IMPORTED SOURCE mesh description, not from the runtime
	// GetMorphTargets() array:
	//
	//   USkeletalMesh::Build()  (SkeletalMesh.cpp)
	//     -> ExecuteBuildInternal -> CacheDerivedData
	//         -> FSkeletalMeshBuilder::Build                     (SkeletalMeshBuilder.cpp)
	//             needs GetMeshDescription(LOD); morphs come ONLY from
	//             MeshAttributes.GetMorphTargetNames()
	//             FinalizeContext -> Context.BuildMorphTargets()
	//             which is a no-op unless ImportData.MorphTargetNames.Num()>0
	//     -> FinishBuildInternal -> ApplyFinishBuildInternalData
	//         -> FFinishBuildMorphTargetData::ApplyEditorData    (MorphTarget.cpp)
	//             EMPTIES SkeletalMesh->GetMorphTargets() and re-adds ONLY the
	//             morphs present in MorphLODModelsPerTargetName (from source).
	//             Every other morph is appended to ToDeleteMorphTargets and
	//             MarkAsGarbage()'d.                             (MorphTarget.cpp)
	//
	// A build is triggered both by RegisterMorphTarget(.., bInvalidateRenderData=true)
	//   -> InitMorphTargetsAndRebuildRenderData (SkeletalMesh.cpp) whose
	//      FScopedSkeletalMeshPostEditChange calls PostEditChange()->Build()
	//      on scope exit,
	// and potentially by SavePackage / PostLoad after a from-disk reload.
	//
	// SKM_Example is a duplicate of /Engine/EngineMeshes/SkeletalCube: it has NO
	// "MT_Test" in its imported source, so a build can never re-create the morph.
	// Injecting MorphLODModels.Vertices is irrelevant —
	// HasValidData() being true keeps the morph through InitMorphTargets(false)
	// (MorphTarget.cpp, SkeletalMesh.cpp) but NOT through
	// ApplyEditorData, which discards by source membership, not HasValidData().
	//
	// Therefore: do NOT invalidate render data (avoid the build), do NOT save
	// (a build-on-save or a from-disk reload would strip it), and be HONEST with
	// persisted_to_disk=false. We still seed one valid LOD so HasValidData() is
	// true and the morph survives InitMorphTargets(true), and we add it to the
	// index map ourselves so FindMorphTarget() — used by every later command
	// in the same session (set_morph_target_deltas / rename / remove) — succeeds.
	// -------------------------------------------------------------------------
	FSkeletalMeshModel* ImportedModel = Mesh->GetImportedModel();
	int32 NumLODs = Mesh->GetLODNum();
	if (ImportedModel && ImportedModel->LODModels.Num() > NumLODs)
	{
		NumLODs = ImportedModel->LODModels.Num();
	}
	if (NumLODs <= 0)
	{
		NumLODs = 1;
	}

	for (int32 LODIdx = 0; LODIdx < NumLODs; ++LODIdx)
	{
		FMorphTargetLODModel LODModel;
		// NumBaseMeshVerts = the LOD's base vertex count when we know it, else 1.
		int32 BaseVerts = 1;
		if (ImportedModel && ImportedModel->LODModels.IsValidIndex(LODIdx))
		{
			BaseVerts = FMath::Max(1, static_cast<int32>(ImportedModel->LODModels[LODIdx].NumVertices));
			if (ImportedModel->LODModels[LODIdx].Sections.Num() > 0)
			{
				LODModel.SectionIndices.Add(0);
			}
		}
		LODModel.NumBaseMeshVerts = BaseVerts;

		// One real delta on vertex 0 so HasValidData() (Vertices.Num()>0) is true,
		// which keeps the morph through InitMorphTargets(/*bKeepEmpty*/true) below.
		FMorphTargetDelta SeedDelta;
		SeedDelta.SourceIdx = 0;
		SeedDelta.PositionDelta = FVector3f(0.01f, 0.0f, 0.0f);
		SeedDelta.TangentZDelta = FVector3f(0.0f, 0.0f, 0.0f);
		LODModel.Vertices.Add(SeedDelta);

		// NumVertices is runtime-derived (not serialized) but set it for consistency.
		LODModel.NumVertices = LODModel.Vertices.Num();

		NewMT->GetMorphLODModels().Add(LODModel);
	}

	// Register WITHOUT invalidating render data: bInvalidateRenderData=false avoids
	// InitMorphTargetsAndRebuildRenderData()'s FScopedSkeletalMeshPostEditChange,
	// which would PostEditChange()->Build()->ApplyEditorData() and immediately
	// MarkAsGarbage() this runtime morph (it is not in the imported source).
	if (!Mesh->RegisterMorphTarget(NewMT, /*bInvalidateRenderData=*/false))
	{
		return mcp_common::MakeErrorReply(TEXT("RegisterMorphTarget failed"));
	}

	// Rebuild the morph index map so FindMorphTarget() locates MT_Test in this
	// session. bInKeepEmptyMorphTargets=true so nothing is pruned, and critically
	// this does NOT trigger a build (unlike InitMorphTargetsAndRebuildRenderData).
	Mesh->InitMorphTargets(/*bInKeepEmptyMorphTargets=*/true);

	Mesh->Modify();

	// Intentionally do NOT save or reload-verify here. Persisting is impossible for
	// a cooked-engine-mesh duplicate whose imported source has no MT_Test: a build
	// during save, or the build on PostLoad after a from-disk reload, would strip
	// it (see trace above). The morph remains valid in-memory for the rest of the
	// session; we report persisted_to_disk=false rather than hard-erroring.
	const bool bInMemory = (Mesh->FindMorphTarget(MorphFName) != nullptr);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("morph_target_name"), MorphName);
	Data->SetStringField(TEXT("skeletal_mesh_path"), MeshPath);
	Data->SetBoolField(TEXT("created"), bInMemory);
	Data->SetBoolField(TEXT("persisted_to_disk"), false);
	Data->SetStringField(TEXT("note"),
		TEXT("Morph target registered in-memory. A skeletal-mesh build regenerates morphs from "
		     "imported source data only, so a runtime-created morph on a mesh without it in its "
		     "imported source cannot survive a build/save. Usable for the rest of this session."));
	Data->SetNumberField(TEXT("total_morph_targets"), Mesh->GetMorphTargets().Num());
	if (!bInMemory)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Morph target '%s' could not be registered in-memory"), *MorphName));
	}
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// remove_skeletal_mesh_morph_target
// ---------------------------------------------------------------------------
// MCP:remove_skeletal_mesh_morph_target - Delete one morph target, by name, from a skeletal mesh asset found by content path, including its data in every LOD's source mesh, in one undo step, then save the mesh. Not idempotent: a name the mesh does not have is an error, so a repeat call fails instead of reporting it already gone. A mesh that does not load is an error, and so is a failed save (refused during PIE or under a read-only root, or a locked file), although the deletion stays in memory. remaining_morph_targets counts the morphs left. List names with get_skeletal_mesh_morph_targets; rename with rename_skeletal_mesh_morph_target; add one with create_skeletal_mesh_morph_target.
//   @param skeletal_mesh_path - Content path to the skeletal mesh asset, e.g. "/Game/Characters/SKM_Hero"
//   @param morph_target_name - Name of the morph target to delete, as get_skeletal_mesh_morph_targets lists it
//   @returns { morph_target_name: string, skeletal_mesh_path: string, removed: bool, remaining_morph_targets: int }

TSharedPtr<FJsonObject> HandleRemoveSkeletalMeshMorphTarget(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MeshPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("skeletal_mesh_path"), MeshPath)) { return Missing; }

	FString MorphName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("morph_target_name"), MorphName)) { return Missing; }

	USkeletalMesh* Mesh = nullptr;
	if (TSharedPtr<FJsonObject> Error = SkMesh_Load(MeshPath, Mesh)) { return Error; }

	const FName MorphFName(*MorphName);
	UMorphTarget* FoundMT = Mesh->FindMorphTarget(MorphFName);
	if (!FoundMT)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Morph target '%s' not found on mesh"), *MorphName));
	}

	Mesh->Modify();

	TArray<FName> NamesToRemove;
	NamesToRemove.Add(MorphFName);
	if (!Mesh->RemoveMorphTargets(MakeArrayView(NamesToRemove)))
	{
		return mcp_common::MakeErrorReply(TEXT("RemoveMorphTargets failed"));
	}

	if (TSharedPtr<FJsonObject> SaveFailed = SkMesh_SaveOrReply(Mesh))
		return SaveFailed;

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("morph_target_name"), MorphName);
	Data->SetStringField(TEXT("skeletal_mesh_path"), MeshPath);
	Data->SetBoolField(TEXT("removed"), true);
	Data->SetNumberField(TEXT("remaining_morph_targets"), Mesh->GetMorphTargets().Num());
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// rename_skeletal_mesh_morph_target
// ---------------------------------------------------------------------------
// MCP:rename_skeletal_mesh_morph_target - Rename a morph target on a skeletal mesh asset and save the mesh. Fails if old_name is not a morph target of the mesh or new_name already exists; a failed save also returns an error although the rename stays in memory. Returns old_name, new_name and renamed.
//   @param skeletal_mesh_path - Skeletal mesh asset path, e.g. "/Game/Characters/SKM_Hero".
//   @param old_name - Current morph target name, as get_skeletal_mesh_morph_targets lists it.
//   @param new_name - New morph target name; must not already exist on the mesh.
//   @returns { old_name: string, new_name: string, skeletal_mesh_path: string, renamed: bool }

TSharedPtr<FJsonObject> HandleRenameSkeletalMeshMorphTarget(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MeshPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("skeletal_mesh_path"), MeshPath)) { return Missing; }

	FString OldName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("old_name"), OldName)) { return Missing; }

	FString NewName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("new_name"), NewName)) { return Missing; }

	USkeletalMesh* Mesh = nullptr;
	if (TSharedPtr<FJsonObject> Error = SkMesh_Load(MeshPath, Mesh)) { return Error; }

	const FName OldFName(*OldName);
	const FName NewFName(*NewName);

	UMorphTarget* FoundMT = Mesh->FindMorphTarget(OldFName);
	if (!FoundMT)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Morph target '%s' not found on mesh"), *OldName));
	}

	// Check new name doesn't already exist
	if (Mesh->FindMorphTarget(NewFName))
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Morph target '%s' already exists"), *NewName));
	}

	Mesh->Modify();
	if (!Mesh->RenameMorphTarget(OldFName, NewFName))
	{
		return mcp_common::MakeErrorReply(TEXT("RenameMorphTarget failed"));
	}

	if (TSharedPtr<FJsonObject> SaveFailed = SkMesh_SaveOrReply(Mesh))
		return SaveFailed;

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("old_name"), OldName);
	Data->SetStringField(TEXT("new_name"), NewName);
	Data->SetStringField(TEXT("skeletal_mesh_path"), MeshPath);
	Data->SetBoolField(TEXT("renamed"), true);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_morph_target_deltas
// ---------------------------------------------------------------------------
// MCP:set_morph_target_deltas - Overwrite all vertex deltas of an existing morph target on one LOD of a skeletal mesh asset with the deltas passed, in one undo step that restores the old deltas, then save the mesh. The undo step records the morph target only, not the mesh: undoing a step that holds the mesh rebuilds it, and the rebuild drops every morph target its imported data does not hold, those of create_skeletal_mesh_morph_target included. The LOD's old deltas are discarded; deltas whose position offset is 0.015 units or less are dropped (normal-only deltas included) and entries that are not objects are skipped, so deltas_set counts what was sent and total_deltas what that LOD kept. When the morph target had no data for a LOD below lod_index, the engine adds an empty one for it (created=true in deltas_per_lod). Every count is read back from the morph target after the save: total_deltas for lod_index, total_deltas_lod0 for LOD 0, deltas_per_lod for each LOD the morph target has, and total_deltas_all_lods. Repeating the same call writes the same data again. A missing morph target or a lod_index outside the mesh's LODs is an error and nothing changes; a failed save makes success false, with saved=false and save_error. Find the morph with get_skeletal_mesh_morph_targets (create_skeletal_mesh_morph_target adds one) and read the deltas themselves back with get_morph_target_deltas.
//   @param skeletal_mesh_path - Content path of the USkeletalMesh asset (object or package path), e.g. '/Game/Characters/SKM_Hero'.
//   @param morph_target_name - Name of an existing morph target, as get_skeletal_mesh_morph_targets lists it, e.g. 'Smile'.
//   @param lod_index - 0-based LOD index whose deltas are overwritten (default 0); it must be below num_lods from get_skeletal_mesh_info.
//   @param deltas - Array of objects {vertex_index, position_delta, normal_delta}: vertex_index is the vertex's index in that LOD's vertex buffer (not range-checked), the two deltas are {x, y, z} offsets in mesh-local units and a missing one is zero, e.g. [{"vertex_index": 0, "position_delta": {"x": 1.0, "y": 0, "z": 0}, "normal_delta": {"x": 0, "y": 0, "z": 0}}]. An empty array leaves the LOD with no deltas.
//   @returns { morph_target_name: string, lod_index: int, deltas_set: int, total_deltas: int, total_deltas_lod0: int, deltas_per_lod: [{ lod_index: int, total_deltas: int, written: bool, created: bool }], total_deltas_all_lods: int, saved: bool, save_error?: string }

TSharedPtr<FJsonObject> HandleSetMorphTargetDeltas(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MeshPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("skeletal_mesh_path"), MeshPath)) { return Missing; }

	FString MorphName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("morph_target_name"), MorphName)) { return Missing; }

	int32 LodIndex = 0;
	Params->TryGetNumberField(TEXT("lod_index"), LodIndex);

	USkeletalMesh* Mesh = nullptr;
	if (TSharedPtr<FJsonObject> Error = SkMesh_Load(MeshPath, Mesh)) { return Error; }

	const FName MorphFName(*MorphName);
	UMorphTarget* MT = Mesh->FindMorphTarget(MorphFName);
	if (!MT)
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Morph target '%s' not found"), *MorphName));

	// Get imported model for sections
	FSkeletalMeshModel* ImportedModel = Mesh->GetImportedModel();
	if (!ImportedModel || LodIndex < 0 || LodIndex >= ImportedModel->LODModels.Num())
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("LOD index %d out of range"), LodIndex));
	}

	const TArray<FSkelMeshSection>& Sections = ImportedModel->LODModels[LodIndex].Sections;

	// Parse deltas array from JSON
	const TArray<TSharedPtr<FJsonValue>>* DeltasArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("deltas"), DeltasArray) || !DeltasArray)
		return mcp_common::MakeErrorReply(TEXT("Missing 'deltas' array"));

	TArray<FMorphTargetDelta> Deltas;
	Deltas.Reserve(DeltasArray->Num());

	for (const auto& Val : *DeltasArray)
	{
		const auto Obj = Val->AsObject();
		if (!Obj.IsValid()) { continue; }

		FMorphTargetDelta Delta;
		Delta.SourceIdx = static_cast<uint32>(Obj->GetNumberField(TEXT("vertex_index")));
		SkMesh_ReadDeltaVector(Obj, TEXT("position_delta"), Delta.PositionDelta);
		SkMesh_ReadDeltaVector(Obj, TEXT("normal_delta"), Delta.TangentZDelta);
		Deltas.Add(Delta);
	}

	// The deltas live on the morph target, not on the mesh, so the morph target is what the undo step must record.
	// The engine creates morph targets without RF_Transactional (import and build), and Modify() records nothing
	// on an object without it. The MESH is deliberately NOT recorded: undoing a step that holds it runs the mesh's
	// PostEditChange, which calls Build() and regenerates the morph targets from the imported source -- dropping
	// every morph target that is not in it, those create_skeletal_mesh_morph_target adds included (measured: undo
	// left "Morph target not found"). UMorphTarget::Serialize writes MorphLODModels, so the morph alone restores.
	const int32 LodModelsBefore = MT->GetMorphLODModels().Num();
	{
		FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetMorphTargetDeltas", "Set Morph Target Deltas"));
		MT->SetFlags(RF_Transactional);
		MT->Modify();
		// Writes LOD lod_index, and adds an empty LOD model for every LOD below it the morph target had none for.
		MT->PopulateDeltas(Deltas, LodIndex, Sections);
	}

	FString SaveError;
	const bool bSaved = mcp_common::SaveAssetPackage(Mesh->GetOutermost(), Mesh, SaveError);

	// Read back from the morph target the mesh holds now, every LOD of it.
	const UMorphTarget* After = Mesh->FindMorphTarget(MorphFName);
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("morph_target_name"), MorphName);
	Data->SetNumberField(TEXT("lod_index"), LodIndex);
	Data->SetNumberField(TEXT("deltas_set"), Deltas.Num());
	Data->SetNumberField(TEXT("total_deltas"), After ? After->GetNumDeltasForLOD(LodIndex) : 0);
	Data->SetNumberField(TEXT("total_deltas_lod0"), After ? After->GetNumDeltasForLOD(0) : 0);
	TArray<TSharedPtr<FJsonValue>> PerLod;
	int32 TotalAllLods = 0;
	const int32 LodModelsAfter = After ? After->GetMorphLODModels().Num() : 0;
	for (int32 Lod = 0; Lod < LodModelsAfter; ++Lod)
	{
		const int32 LodDeltas = After->GetNumDeltasForLOD(Lod);
		TotalAllLods += LodDeltas;
		TSharedPtr<FJsonObject> LodObj = MakeShared<FJsonObject>();
		LodObj->SetNumberField(TEXT("lod_index"), Lod);
		LodObj->SetNumberField(TEXT("total_deltas"), LodDeltas);
		LodObj->SetBoolField(TEXT("written"), Lod == LodIndex);
		LodObj->SetBoolField(TEXT("created"), Lod >= LodModelsBefore);
		PerLod.Add(MakeShared<FJsonValueObject>(LodObj));
	}
	Data->SetArrayField(TEXT("deltas_per_lod"), PerLod);
	Data->SetNumberField(TEXT("total_deltas_all_lods"), TotalAllLods);
	if (!After)
	{
		mcp_common::AddSaveOutcome(Data, bSaved, SaveError);
		return mcp_common::MakeOutcomeReply(Data, false, FString::Printf(
			TEXT("set_morph_target_deltas: morph target '%s' is no longer on the mesh after the write and save"), *MorphName));
	}
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_morph_target_deltas"), SaveError);
}

// ---------------------------------------------------------------------------
// get_morph_target_deltas
// ---------------------------------------------------------------------------
// MCP:get_morph_target_deltas - Read back the vertex deltas of a morph target on a specific LOD (inverse of set_morph_target_deltas).
//   @param skeletal_mesh_path - Content path to the skeletal mesh asset
//   @param morph_target_name - Name of the morph target to read
//   @param lod_index - LOD index to read (default 0)
//   @param max_deltas - Optional cap on how many deltas to return (default 5000); the response is honestly marked truncated if the LOD has more
//   @returns { morph_target_name: string, lod_index: int, total_deltas: int, returned_deltas: int, truncated: bool, deltas: [{vertex_index, position_delta:{x,y,z}, normal_delta:{x,y,z}}] }

TSharedPtr<FJsonObject> HandleGetMorphTargetDeltas(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MeshPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("skeletal_mesh_path"), MeshPath)) { return Missing; }

	FString MorphName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("morph_target_name"), MorphName)) { return Missing; }

	int32 LodIndex = 0;
	Params->TryGetNumberField(TEXT("lod_index"), LodIndex);

	int32 MaxDeltas = 5000;
	Params->TryGetNumberField(TEXT("max_deltas"), MaxDeltas);
	if (MaxDeltas <= 0) { MaxDeltas = 5000; }

	USkeletalMesh* Mesh = nullptr;
	if (TSharedPtr<FJsonObject> Error = SkMesh_Load(MeshPath, Mesh)) { return Error; }

	const FName MorphFName(*MorphName);
	UMorphTarget* MT = Mesh->FindMorphTarget(MorphFName);
	if (!MT)
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Morph target '%s' not found"), *MorphName));

	const TArray<FMorphTargetLODModel>& LODModels = MT->GetMorphLODModels();
	if (LodIndex < 0 || LodIndex >= LODModels.Num())
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("LOD index %d out of range (morph target has %d LOD model(s))"), LodIndex, LODModels.Num()));
	}

	const TArray<FMorphTargetDelta>& Vertices = LODModels[LodIndex].Vertices;
	const int32 TotalDeltas = Vertices.Num();
	const int32 ReturnCount = FMath::Min(TotalDeltas, MaxDeltas);

	TArray<TSharedPtr<FJsonValue>> DeltasArr;
	DeltasArr.Reserve(ReturnCount);
	for (int32 i = 0; i < ReturnCount; ++i)
	{
		const FMorphTargetDelta& Delta = Vertices[i];
		TSharedPtr<FJsonObject> DObj = MakeShared<FJsonObject>();
		DObj->SetNumberField(TEXT("vertex_index"), Delta.SourceIdx);
		DObj->SetObjectField(TEXT("position_delta"), mcp_common::VectorToJsonObject(FVector(Delta.PositionDelta)));
		DObj->SetObjectField(TEXT("normal_delta"), mcp_common::VectorToJsonObject(FVector(Delta.TangentZDelta)));
		DeltasArr.Add(MakeShared<FJsonValueObject>(DObj));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("morph_target_name"), MorphName);
	Data->SetNumberField(TEXT("lod_index"), LodIndex);
	Data->SetNumberField(TEXT("total_deltas"), TotalDeltas);
	Data->SetNumberField(TEXT("returned_deltas"), ReturnCount);
	Data->SetBoolField(TEXT("truncated"), ReturnCount < TotalDeltas);
	Data->SetArrayField(TEXT("deltas"), DeltasArr);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_skeletal_mesh_section_info
// ---------------------------------------------------------------------------
// MCP:get_skeletal_mesh_section_info - Read the render sections of one LOD of a skeletal mesh asset from its imported model: per section its material slot index, triangle and vertex counts, bone map size and the cast-shadow, recompute-tangent, ray-tracing and disabled flags, plus the LOD's total vertex count. Read-only: it changes nothing. A mesh that does not load, or a lod_index outside the mesh's LOD count, is an error. Use it to pick the section_index that set_skeletal_mesh_section_flags writes; for whole-mesh counts use get_skeletal_mesh_info, for one LOD's settings get_skeletal_mesh_lod_info, and for slot names get_skeletal_mesh_material_slots.
//   @param skeletal_mesh_path - Content path of the USkeletalMesh asset (object or package path), e.g. '/Game/Characters/SKM_Hero'.
//   @param lod_index - 0-based LOD index to read (default 0); it must be below num_lods from get_skeletal_mesh_info.
//   @returns { skeletal_mesh_path: string, lod_index: int, num_sections: int, lod_num_vertices: int, sections: [{section_index, material_index, num_triangles, num_vertices, recompute_tangent, cast_shadow, visible_in_ray_tracing, disabled, bone_map_count}] }

TSharedPtr<FJsonObject> HandleGetSkeletalMeshSectionInfo(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MeshPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("skeletal_mesh_path"), MeshPath)) { return Missing; }

	int32 LodIndex = 0;
	Params->TryGetNumberField(TEXT("lod_index"), LodIndex);

	USkeletalMesh* Mesh = nullptr;
	if (TSharedPtr<FJsonObject> Error = SkMesh_Load(MeshPath, Mesh)) { return Error; }

	FSkeletalMeshModel* ImportedModel = Mesh->GetImportedModel();
	if (!ImportedModel || LodIndex < 0 || LodIndex >= ImportedModel->LODModels.Num())
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("LOD index %d out of range"), LodIndex));
	}

	const FSkeletalMeshLODModel& LODModel = ImportedModel->LODModels[LodIndex];
	const TArray<FSkelMeshSection>& Sections = LODModel.Sections;

	TSharedPtr<FJsonObject> Data = SkMesh_NewPayload(MeshPath);
	Data->SetNumberField(TEXT("lod_index"), LodIndex);
	Data->SetNumberField(TEXT("num_sections"), Sections.Num());
	Data->SetNumberField(TEXT("lod_num_vertices"), static_cast<int32>(LODModel.NumVertices));

	TArray<TSharedPtr<FJsonValue>> SectionsArray;
	for (int32 i = 0; i < Sections.Num(); ++i)
	{
		const FSkelMeshSection& Sec = Sections[i];
		TSharedPtr<FJsonObject> SecObj = MakeShared<FJsonObject>();
		SecObj->SetNumberField(TEXT("section_index"), i);
		SecObj->SetNumberField(TEXT("material_index"), Sec.MaterialIndex);
		SecObj->SetNumberField(TEXT("num_triangles"), Sec.NumTriangles);
		SecObj->SetNumberField(TEXT("num_vertices"), Sec.NumVertices);
		SecObj->SetBoolField(TEXT("recompute_tangent"), Sec.bRecomputeTangent);
		SecObj->SetBoolField(TEXT("cast_shadow"), Sec.bCastShadow);
		SecObj->SetBoolField(TEXT("visible_in_ray_tracing"), Sec.bVisibleInRayTracing);
		SecObj->SetBoolField(TEXT("disabled"), Sec.bDisabled);
		SecObj->SetNumberField(TEXT("bone_map_count"), Sec.BoneMap.Num());
		SectionsArray.Add(MakeShared<FJsonValueObject>(SecObj));
	}
	Data->SetArrayField(TEXT("sections"), SectionsArray);

	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_skeletal_mesh_section_flags
// ---------------------------------------------------------------------------
// MCP:set_skeletal_mesh_section_flags - Turn shadow casting, runtime tangent recomputation (a GPU skin cache feature) and/or ray-tracing visibility on or off for one section of one LOD of a skeletal mesh asset found by content path; pass at least one of the three flags, and only the flags passed are written. The SkeletalMeshEditorSubsystem writes the section's user data (one undo step per flag) and rebuilds the render data, then the mesh is saved. A mesh that does not load, a LOD or section index its imported model does not have, or a call during PIE (the subsystem refuses it) is an error, and the flags written before the refused one stay in memory unsaved; so is a failed save, which leaves the change in memory. Setting the current value again re-applies and re-saves it. Read the section flags with get_skeletal_mesh_section_info.
//   @param skeletal_mesh_path - Content path to the skeletal mesh asset, e.g. "/Game/Characters/SKM_Hero"
//   @param lod_index - 0-based LOD index containing the section (default 0)
//   @param section_index - 0-based index of the section within that LOD, as get_skeletal_mesh_section_info lists it (default 0)
//   @param cast_shadow - Optional bool: whether the section casts shadows.
//   @param recompute_tangent - Optional bool: whether the GPU skin cache recomputes the section's tangents at runtime.
//   @param visible_in_ray_tracing - Optional bool: whether the section is visible in ray tracing (bVisibleInRayTracing). b_visible is accepted as an alias.
//   @returns { skeletal_mesh_path: string, lod_index: int, section_index: int, cast_shadow?: bool, recompute_tangent?: bool, visible_in_ray_tracing?: bool, saved: bool }

TSharedPtr<FJsonObject> HandleSetSkeletalMeshSectionCastShadow(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MeshPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("skeletal_mesh_path"), MeshPath)) { return Missing; }

	int32 LodIndex = 0;
	Params->TryGetNumberField(TEXT("lod_index"), LodIndex);

	int32 SectionIndex = 0;
	Params->TryGetNumberField(TEXT("section_index"), SectionIndex);

	// The flags to write: only those passed, at least one.
	bool bCastShadow = true;
	const bool bHasCastShadow = Params->TryGetBoolField(TEXT("cast_shadow"), bCastShadow);
	bool bRecomputeTangent = true;
	const bool bHasRecomputeTangent = Params->TryGetBoolField(TEXT("recompute_tangent"), bRecomputeTangent);
	bool bVisibleInRayTracing = true;
	bool bHasVisibleInRayTracing = true;
	if (!Params->TryGetBoolField(TEXT("visible_in_ray_tracing"), bVisibleInRayTracing))
	{
		bHasVisibleInRayTracing = Params->TryGetBoolField(TEXT("b_visible"), bVisibleInRayTracing);
	}
	if (!bHasCastShadow && !bHasRecomputeTangent && !bHasVisibleInRayTracing)
	{
		return mcp_common::MakeErrorReply(
			TEXT("Pass at least one flag to write: cast_shadow, recompute_tangent or visible_in_ray_tracing (bools)"));
	}

	USkeletalMesh* Mesh = nullptr;
	if (TSharedPtr<FJsonObject> Error = SkMesh_Load(MeshPath, Mesh)) { return Error; }

	USkeletalMeshEditorSubsystem* Subsys = GEditor->GetEditorSubsystem<USkeletalMeshEditorSubsystem>();
	if (!Subsys)
		return mcp_common::MakeErrorReply(TEXT("SkeletalMeshEditorSubsystem unavailable"));

	Mesh->Modify();
	if (bHasCastShadow && !Subsys->SetSectionCastShadow(Mesh, LodIndex, SectionIndex, bCastShadow))
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("SetSectionCastShadow failed for LOD %d section %d"), LodIndex, SectionIndex));
	}
	if (bHasRecomputeTangent && !Subsys->SetSectionRecomputeTangent(Mesh, LodIndex, SectionIndex, bRecomputeTangent))
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("SetSectionRecomputeTangent failed for LOD %d section %d"), LodIndex, SectionIndex));
	}
	if (bHasVisibleInRayTracing && !Subsys->SetSectionVisibleInRayTracing(Mesh, LodIndex, SectionIndex, bVisibleInRayTracing))
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("SetSectionVisibleInRayTracing failed for LOD %d section %d"), LodIndex, SectionIndex));
	}

	if (TSharedPtr<FJsonObject> SaveFailed = SkMesh_SaveOrReply(Mesh))
		return SaveFailed;

	TSharedPtr<FJsonObject> Data = SkMesh_NewPayload(MeshPath);
	Data->SetNumberField(TEXT("lod_index"), LodIndex);
	Data->SetNumberField(TEXT("section_index"), SectionIndex);
	if (bHasCastShadow)
	{
		Data->SetBoolField(TEXT("cast_shadow"), bCastShadow);
	}
	if (bHasRecomputeTangent)
	{
		Data->SetBoolField(TEXT("recompute_tangent"), bRecomputeTangent);
	}
	if (bHasVisibleInRayTracing)
	{
		Data->SetBoolField(TEXT("visible_in_ray_tracing"), bVisibleInRayTracing);
	}
	Data->SetBoolField(TEXT("saved"), true);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_skeletal_mesh_material_slots
// ---------------------------------------------------------------------------
// MCP:get_skeletal_mesh_material_slots - List the material slots of a skeletal mesh asset found by content path, in slot-index order: slot name, assigned material and overlay material as object paths, "(none)" where a slot has none. Nothing is written; a mesh that does not load is an error. Pass a slot_index or slot_name from here to set_skeletal_mesh_material_slot to assign a material; get_skeletal_mesh_section_info gives the material_index each section of a LOD draws with; get_skeletal_mesh_info gives the slot count next to LODs, morph targets, sockets and bones.
//   @param skeletal_mesh_path - Content path to the skeletal mesh asset, e.g. "/Game/Characters/SKM_Hero"
//   @returns { skeletal_mesh_path: string, num_material_slots: int, material_slots: [{slot_index, slot_name, material_path, overlay_material_path}] }

TSharedPtr<FJsonObject> HandleGetSkeletalMeshMaterialSlots(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MeshPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("skeletal_mesh_path"), MeshPath)) { return Missing; }

	USkeletalMesh* Mesh = nullptr;
	if (TSharedPtr<FJsonObject> Error = SkMesh_Load(MeshPath, Mesh)) { return Error; }

	const TArray<FSkeletalMaterial>& Materials = Mesh->GetMaterials();

	TSharedPtr<FJsonObject> Data = SkMesh_NewPayload(MeshPath);
	Data->SetNumberField(TEXT("num_material_slots"), Materials.Num());

	TArray<TSharedPtr<FJsonValue>> SlotsArray;
	for (int32 i = 0; i < Materials.Num(); ++i)
	{
		const FSkeletalMaterial& Mat = Materials[i];
		TSharedPtr<FJsonObject> SlotObj = MakeShared<FJsonObject>();
		SlotObj->SetNumberField(TEXT("slot_index"), i);
		SlotObj->SetStringField(TEXT("slot_name"), Mat.MaterialSlotName.ToString());
		SlotObj->SetStringField(TEXT("material_path"),
			Mat.MaterialInterface ? Mat.MaterialInterface->GetPathName() : TEXT("(none)"));
		SlotObj->SetStringField(TEXT("overlay_material_path"),
			Mat.OverlayMaterialInterface ? Mat.OverlayMaterialInterface->GetPathName() : TEXT("(none)"));
		SlotsArray.Add(MakeShared<FJsonValueObject>(SlotObj));
	}
	Data->SetArrayField(TEXT("material_slots"), SlotsArray);

	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_skeletal_mesh_material_slot
// ---------------------------------------------------------------------------
// MCP:set_skeletal_mesh_material_slot - Assign a material to an existing material slot of a skeletal mesh ASSET (writes FSkeletalMaterial.MaterialInterface in USkeletalMesh::GetMaterials()). Address the slot by slot_index OR slot_name (exactly one). Never auto-grows the slot array.
//   @param skeletal_mesh_path - Content path to the USkeletalMesh asset.
//   @param slot_index - Zero-based index of an existing material slot (mutually exclusive with slot_name).
//   @param slot_name - Name of an existing material slot to resolve to its index (mutually exclusive with slot_index).
//   @param material_path - Content path to the UMaterialInterface to assign.
//   @returns { skeletal_mesh_path: string, slot_index: int, slot_name: string, material_path: string, saved: bool }

TSharedPtr<FJsonObject> HandleSetSkeletalMeshMaterialSlot(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MeshPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("skeletal_mesh_path"), MeshPath)) { return Missing; }

	FString MaterialPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_path"), MaterialPath)) { return Missing; }

	// Slot addressing: exactly one of slot_index / slot_name. One canonical route
	// per thing -- no silent default index, no both-at-once ambiguity.
	const bool bHasIndex = Params->HasField(TEXT("slot_index"));
	const bool bHasName  = Params->HasField(TEXT("slot_name"));
	if (bHasIndex == bHasName)
		return mcp_common::MakeErrorReply(
			TEXT("Provide exactly one of 'slot_index' or 'slot_name'"));

	USkeletalMesh* Mesh = nullptr;
	if (TSharedPtr<FJsonObject> Error = SkMesh_Load(MeshPath, Mesh)) { return Error; }

	// GetMaterials() returns a mutable TArray<FSkeletalMaterial>& (SkeletalMesh.h) —
	// assigning a material to a slot mutates it in place; no geometry rebuild needed.
	TArray<FSkeletalMaterial>& Materials = Mesh->GetMaterials();
	const int32 NumSlots = Materials.Num();

	// Resolve the target slot index. Validate against the existing slots — never
	// auto-grow the array (mirror of set_static_mesh_material_slot).
	int32 SlotIndex = 0;
	if (bHasIndex)
	{
		Params->TryGetNumberField(TEXT("slot_index"), SlotIndex);
		if (SlotIndex < 0 || SlotIndex >= NumSlots)
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("slot_index %d out of range [0, %d)"), SlotIndex, NumSlots));
	}
	else
	{
		FString SlotNameStr;
		Params->TryGetStringField(TEXT("slot_name"), SlotNameStr);
		const FName SlotName(*SlotNameStr);
		SlotIndex = INDEX_NONE;
		for (int32 i = 0; i < NumSlots; ++i)
		{
			if (Materials[i].MaterialSlotName == SlotName)
			{
				SlotIndex = i;
				break;
			}
		}
		if (SlotIndex == INDEX_NONE)
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("slot_name '%s' not found among %d material slots"), *SlotNameStr, NumSlots));
	}

	UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
	if (!Mat)
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Could not load material: %s"), *MaterialPath));

	Mesh->Modify();
	Materials[SlotIndex].MaterialInterface = Mat;
	Mesh->MarkPackageDirty();

	if (TSharedPtr<FJsonObject> SaveFailed = SkMesh_SaveOrReply(Mesh))
		return SaveFailed;

	// Read back the assigned material off the (now-persisted) slot.
	const FSkeletalMaterial& WrittenSlot = Mesh->GetMaterials()[SlotIndex];
	const FString ReadBackPath =
		WrittenSlot.MaterialInterface ? WrittenSlot.MaterialInterface->GetPathName() : FString();

	TSharedPtr<FJsonObject> Data = SkMesh_NewPayload(MeshPath);
	Data->SetNumberField(TEXT("slot_index"), SlotIndex);
	Data->SetStringField(TEXT("slot_name"), WrittenSlot.MaterialSlotName.ToString());
	Data->SetStringField(TEXT("material_path"), ReadBackPath);
	Data->SetBoolField(TEXT("saved"), true);
	return mcp_common::MakeOkReply(Data);
}
// ========================================================================== //
// Skeletal Mesh LOD authoring
//
// LOD authoring on USkeletalMesh:
//   add_skeletal_mesh_lod             append a new LOD entry (clone of LOD 0
//                                     build settings; caller can override
//                                     screen_size). Does NOT regenerate render
//                                     data — that's the user's job (or
//                                     bake_skeletal_mesh_lod after).
//   remove_skeletal_mesh_lod          drop an LOD entry by index via
//                                     USkeletalMesh::RemoveSourceModel.
//                                     LOD 0 is protected.
//   set_skeletal_mesh_min_lod         set MinLodIdx (the forced-min-LOD bias
//                                     applied to this mesh everywhere).
//   bake_skeletal_mesh_lod            call USkeletalMeshEditorSubsystem::
//                                     RegenerateLOD(Mesh, NewLODCount) to
//                                     rebuild LODs from LOD 0 via the
//                                     editor's reduction pipeline.
//
// Each of these calls Mesh->Modify() (which marks the package dirty) and does
// not save: the asset stays in the editor's dirty-set and gets persisted on
// next save.
// ========================================================================== //

// MCP:add_skeletal_mesh_lod - Append one LOD to a skeletal mesh asset found by content path, its LOD info a copy of LOD 0's (build and reduction settings, material map, flags) with ScreenSize.Default set to screen_size, or 0.5 when omitted. Each call appends another LOD (not idempotent), in one undo step; the mesh is marked dirty but not saved, and its render data is not regenerated: bake_skeletal_mesh_lod with regenerate_even_if_imported=true and new_lod_count omitted builds the new LOD (its defaults skip it, as it copies LOD 0's imported flag; note says which applies), then keep it with save_asset. A mesh that does not load is an error. To append reduced LODs in one call, use bake_skeletal_mesh_lod with a higher new_lod_count instead. Tune this LOD with set_skeletal_mesh_lod_reduction_settings or set_skeletal_mesh_lod_settings (screen_size); take one out with remove_skeletal_mesh_lod; inspect it with get_skeletal_mesh_lod_info.
//   @param skeletal_mesh_path - Content path to the skeletal mesh asset, e.g. "/Game/Characters/SKM_Hero"
//   @param screen_size - Optional ScreenSize.Default of the new LOD, the screen size at which it switches in (default 0.5); not clamped
//   @returns { skeletal_mesh_path: string, lod_index: int, lod_count: int, screen_size: float, added: bool, note: string }
TSharedPtr<FJsonObject> HandleAddSkeletalMeshLod(const TSharedPtr<FJsonObject>& Params)
{
    FString MeshPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("skeletal_mesh_path"), MeshPath)) { return Missing; }

    USkeletalMesh* Mesh = nullptr;
    if (TSharedPtr<FJsonObject> Error = SkMesh_Load(MeshPath, Mesh)) { return Error; }

    Mesh->Modify();

    // Clone LOD 0's build settings / material map for the new entry (engine
    // convention: a new LOD starts as a copy of the highest-quality LOD until
    // the user re-tunes it).
    FSkeletalMeshLODInfo NewLOD;
    if (const FSkeletalMeshLODInfo* LOD0 = Mesh->GetLODInfo(0))
    {
        NewLOD = *LOD0;
    }
    // Default screen size for a new LOD: 0.5 (half-screen rule of thumb). If
    // the caller passes screen_size it overrides.
    NewLOD.ScreenSize.Default = 0.5f;
    if (Params->HasField(TEXT("screen_size")))
    {
        NewLOD.ScreenSize.Default = static_cast<float>(Params->GetNumberField(TEXT("screen_size")));
    }

    const int32 NewIndex = Mesh->AddLODInfo(NewLOD);
    const int32 NewLODCount = Mesh->GetLODNum();

    TSharedPtr<FJsonObject> ResultObj = SkMesh_NewPayload(MeshPath);
    ResultObj->SetNumberField(TEXT("lod_index"), NewIndex);
    ResultObj->SetNumberField(TEXT("lod_count"), NewLODCount);
    ResultObj->SetNumberField(TEXT("screen_size"), NewLOD.ScreenSize.Default);
    ResultObj->SetBoolField(TEXT("added"), true);
    // FLODUtilities::RegenerateLOD, keeping the LOD count, reduces a LOD above 0 only when it is flagged
    // bHasBeenSimplified or regenerate_even_if_imported is set; the new LOD copied that flag from LOD 0.
    ResultObj->SetStringField(TEXT("note"), NewLOD.bHasBeenSimplified
        ? TEXT("Render data NOT regenerated. This LOD is flagged generated (copied from LOD 0), so "
               "bake_skeletal_mesh_lod with new_lod_count omitted builds it.")
        : TEXT("Render data NOT regenerated. This LOD copied LOD 0's imported flag, so bake_skeletal_mesh_lod "
               "with its defaults skips it: call it with regenerate_even_if_imported=true and new_lod_count "
               "omitted to build it (that also re-reduces the other LODs above 0)."));
    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:remove_skeletal_mesh_lod - Remove one LOD from a skeletal mesh asset, with its source model and cached reduction data; higher LODs shift down one index. lod_index must be 1 or more and below the LOD count, otherwise an error (LOD 0 is never removed). Marks the mesh dirty without saving or rebuilding render data, so follow with save_asset. Use add_skeletal_mesh_lod to append a LOD, get_skeletal_mesh_lod_info to inspect them, set_skeletal_mesh_min_lod to skip the most detailed LODs while keeping them.
//   @param skeletal_mesh_path - Content path to the skeletal mesh asset
//   @param lod_index - Index of the LOD to remove (must be > 0)
//   @returns { skeletal_mesh_path: string, lod_index: int, lod_count: int, removed: bool }
TSharedPtr<FJsonObject> HandleRemoveSkeletalMeshLod(const TSharedPtr<FJsonObject>& Params)
{
    FString MeshPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("skeletal_mesh_path"), MeshPath)) { return Missing; }

    int32 LodIndex = 0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("lod_index"), LodIndex)) { return Missing; }

    USkeletalMesh* Mesh = nullptr;
    if (TSharedPtr<FJsonObject> Error = SkMesh_Load(MeshPath, Mesh)) { return Error; }

    const int32 NumLODs = Mesh->GetLODNum();
    if (LodIndex <= 0 || LodIndex >= NumLODs)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("lod_index %d out of range [1, %d); LOD 0 is protected"),
                            LodIndex, NumLODs));
    }

Mesh->Modify();
    // RemoveLODInfo is deprecated in favor of RemoveSourceModel. Both
    // signatures drop the entry at the index; RemoveSourceModel additionally
    // re-balances the SourceModels/LODInfo arrays atomically.
    Mesh->RemoveSourceModel(LodIndex);
    const int32 NewLODCount = Mesh->GetLODNum();

    TSharedPtr<FJsonObject> ResultObj = SkMesh_NewPayload(MeshPath);
    ResultObj->SetNumberField(TEXT("lod_index"), LodIndex);
    ResultObj->SetNumberField(TEXT("lod_count"), NewLODCount);
    ResultObj->SetBoolField(TEXT("removed"), true);
    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:set_skeletal_mesh_min_lod - Set the minimum LOD a skeletal mesh asset (found by content path) renders, so the more detailed LODs below it are skipped while they stay in the asset. It replaces the whole Minimum LOD property with this one value, per-platform overrides included, or the per-quality-level one when the project setting UseSkeletalMeshMinLODPerQualityLevels is on. A missing min_lod, one outside 0 to the LOD count minus 1, or a mesh that does not load is an error. One undo step; the mesh is marked dirty but not saved, so follow with save_asset. old_min_lod is the value in effect before the call. To take a LOD out of the asset use remove_skeletal_mesh_lod; to change switch distances, set_skeletal_mesh_lod_settings with screen_size.
//   @param skeletal_mesh_path - Content path to the skeletal mesh asset, e.g. "/Game/Characters/SKM_Hero"
//   @param min_lod - New minimum LOD index, 0-based (must be in [0, NumLODs-1])
//   @returns { skeletal_mesh_path: string, old_min_lod: int, min_lod: int, lod_count: int, applied: bool }
TSharedPtr<FJsonObject> HandleSetSkeletalMeshMinLod(const TSharedPtr<FJsonObject>& Params)
{
    FString MeshPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("skeletal_mesh_path"), MeshPath)) { return Missing; }

    int32 MinLOD = 0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("min_lod"), MinLOD)) { return Missing; }

    USkeletalMesh* Mesh = nullptr;
    if (TSharedPtr<FJsonObject> Error = SkMesh_Load(MeshPath, Mesh)) { return Error; }

    const int32 MaxValidMinLOD = FMath::Max(0, Mesh->GetLODNum() - 1);
    if (MinLOD < 0 || MinLOD > MaxValidMinLOD)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("min_lod %d out of range [0, %d] (mesh has %d LODs)"),
                            MinLOD, MaxValidMinLOD, Mesh->GetLODNum()));
    }

Mesh->Modify();
    // USkeletalMesh::GetMinLodIdx(bool bForceLowestLODIdx = false) reads the
    // current MinLod bias (declared in SkeletalMesh.h, defined in
    // SkeletalMesh.cpp — note the lowercase "Lod", not "LOD").
    const int32 OldMinLOD = Mesh->GetMinLodIdx();
    // USkeletalMesh::SetMinLodIdx(int32) is the public setter on the asset
    // (declared in SkeletalMesh.h) — sets the platform-wide forced-min-LOD
    // bias used by GetMinLOD() at runtime.
    Mesh->SetMinLodIdx(MinLOD);

    TSharedPtr<FJsonObject> ResultObj = SkMesh_NewPayload(MeshPath);
    ResultObj->SetNumberField(TEXT("old_min_lod"), OldMinLOD);
    ResultObj->SetNumberField(TEXT("min_lod"), MinLOD);
    ResultObj->SetNumberField(TEXT("lod_count"), Mesh->GetLODNum());
    ResultObj->SetBoolField(TEXT("applied"), true);
    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:bake_skeletal_mesh_lod - Regenerate the LODs of a skeletal mesh asset (found by content path) with the engine's mesh reduction, driven by each LOD's reduction settings, then rebuild its render data; nothing is saved, so follow with save_asset. A new_lod_count below the current count drops the LODs above it; above it, appends reduced LODs up to that count; 0, omitted or equal re-reduces the LODs that were generated, plus imported LODs above 0 with regenerate_even_if_imported and LOD 0 with generate_base_lod. A mesh that does not load is an error; during PIE, where the engine regenerates nothing, the call is refused before the mesh is touched, and any other run the engine refuses fails with baked=false and bake_warning. Author the settings first with set_skeletal_mesh_lod_reduction_settings; one empty LOD entry: add_skeletal_mesh_lod; one LOD out by index: remove_skeletal_mesh_lod; triangle counts: get_asset_lod_settings.
//   @param skeletal_mesh_path - Content path to the skeletal mesh asset, e.g. "/Game/Characters/SKM_Hero"
//   @param new_lod_count - Target LOD count; fewer than now drops the extra LODs, more appends reduced ones, and <= 0 (the default when omitted) keeps the current count and re-runs reduction
//   @param regenerate_even_if_imported - bool, also re-reduce imported LODs above 0 when the count is kept (default false)
//   @param generate_base_lod - bool, re-derive LOD 0 from its reduction settings when the count is kept (default false)
//   @returns { skeletal_mesh_path: string, old_lod_count: int, new_lod_count: int, lod_count: int, baked: bool, regenerate_even_if_imported: bool, generate_base_lod: bool, bake_warning?: string }
TSharedPtr<FJsonObject> HandleBakeSkeletalMeshLod(const TSharedPtr<FJsonObject>& Params)
{
    FString MeshPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("skeletal_mesh_path"), MeshPath)) { return Missing; }

    // Default: keep current LOD count and regenerate all of them.
    int32 NewLODCount = -1;
    Params->TryGetNumberField(TEXT("new_lod_count"), NewLODCount);

    bool bRegenerateEvenIfImported = false;
    Params->TryGetBoolField(TEXT("regenerate_even_if_imported"), bRegenerateEvenIfImported);

    bool bGenerateBaseLOD = false;
    Params->TryGetBoolField(TEXT("generate_base_lod"), bGenerateBaseLOD);

    USkeletalMesh* Mesh = nullptr;
    if (TSharedPtr<FJsonObject> Error = SkMesh_Load(MeshPath, Mesh)) { return Error; }

    const int32 CurrentLODCount = Mesh->GetLODNum();
    const int32 TargetLODCount = (NewLODCount > 0)
        ? NewLODCount
        : CurrentLODCount;
    if (TargetLODCount < 1)
    {
        return mcp_common::MakeErrorReply(TEXT("new_lod_count must be >= 1"));
    }

    // USkeletalMeshEditorSubsystem::RegenerateLOD returns false without doing anything while a play
    // session runs (EditorScriptingHelpers::CheckIfInEditorAndPIE tests GEditor->PlayWorld and
    // GIsPlayInEditorWorld), so that case is refused here, before the mesh is touched.
    if (mcp_common::IsPieActive() || (GEditor != nullptr && GEditor->PlayWorld != nullptr) || GIsPlayInEditorWorld)
    {
        return mcp_common::MakeErrorReply(TEXT("REFUSED: a Play-in-Editor session is running, and the engine does not ")
            TEXT("regenerate LODs during it, so nothing was changed. Stop PIE (stop_play_in_editor) and retry."));
    }

    Mesh->Modify();
    // USkeletalMeshEditorSubsystem::RegenerateLOD (static UE_API):
    //   static bool RegenerateLOD(USkeletalMesh* SkeletalMesh, int32 NewLODCount = 0,
    //                             bool bRegenerateEvenIfImported = false,
    //                             bool bGenerateBaseLOD = false);
    // When NewLODCount <= 0, keeps the current count and re-runs reduction on
    // every LOD; >0 rebuilds the mesh with that many LODs. bGenerateBaseLOD
    // re-derives LOD 0 from source vertices; off by default (LOD 0 stays as
    // the user-imported mesh).
    const bool bOk = USkeletalMeshEditorSubsystem::RegenerateLOD(
        Mesh, NewLODCount, bRegenerateEvenIfImported, bGenerateBaseLOD);

    TSharedPtr<FJsonObject> ResultObj = SkMesh_NewPayload(MeshPath);
    ResultObj->SetNumberField(TEXT("old_lod_count"), CurrentLODCount);
    ResultObj->SetNumberField(TEXT("new_lod_count"), TargetLODCount);
    ResultObj->SetNumberField(TEXT("lod_count"), Mesh->GetLODNum());
    ResultObj->SetBoolField(TEXT("baked"), bOk);
    ResultObj->SetBoolField(TEXT("regenerate_even_if_imported"), bRegenerateEvenIfImported);
    ResultObj->SetBoolField(TEXT("generate_base_lod"), bGenerateBaseLOD);
    if (!bOk)
    {
        ResultObj->SetStringField(TEXT("bake_warning"),
            TEXT("RegenerateLOD returned false; the reduction pipeline may "
                 "have failed. Inspect the editor log for SkeletalMesh reduction errors."));
    }
    return mcp_common::MakeOutcomeReply(ResultObj, bOk, bOk ? FString() : FString(
        TEXT("bake_skeletal_mesh_lod: the engine did not regenerate the LODs (RegenerateLOD returned false); ")
        TEXT("see bake_warning and the editor log.")));
}

// ========================================================================== //
// SkeletalMesh socket CRUD + section ray tracing
// + physics asset + CPU access + morph targets, all on
// USkeletalMeshEditorSubsystem (static UE_API, except the instance method
// SetSectionVisibleInRayTracing) or USkeletalMeshSocket (ENGINE_API,
// WITH_EDITOR-only for the parent/transform setters).
//
// Source-of-truth notes:
//   RenameSocket:                 SkeletalMeshEditorSubsystem.h
//   RemoveSocket:                 SkeletalMeshEditorSubsystem.h
//   SetMorphTargetsToGeneratedByEngine: SkeletalMeshEditorSubsystem.h
//   GetMorphTargetsGeneratedByEngine:   SkeletalMeshEditorSubsystem.h
//   AssignPhysicsAsset:           SkeletalMeshEditorSubsystem.h
//   SetAllowCPUAccess:            SkeletalMeshEditorSubsystem.h
//   SetSectionVisibleInRayTracing: SkeletalMeshEditorSubsystem.h
//   SetSocketParent:              SkeletalMeshSocket.h  (WITH_EDITOR)
//   SetSocketLocalTransform:      SkeletalMeshSocket.h  (WITH_EDITOR)
//
// The editor-subsystem calls invalidate cooked data when needed; we just
// persist the package through SkMesh_SaveOrReply.
// ========================================================================== //

// ---------------------------------------------------------------------------
// rename_skeletal_mesh_socket
// ---------------------------------------------------------------------------
// MCP:rename_skeletal_mesh_socket - Renames a socket of a skeletal mesh asset; a socket of the same name on the mesh's skeleton is renamed too, and preview assets attached to the old name follow it. One undo step, then every asset that changed is saved: the mesh for its own socket, the skeleton for its socket (saved_assets lists them), so a socket that lives only on the skeleton is written to the skeleton's file; a failed save makes success false with save_error and leaves the rename in memory. Refused with nothing changed when new_name equals old_name, a socket called new_name already exists on the mesh or its skeleton while old_name is still there, old_name is not found, or an asset that would change cannot be saved (engine content, a read-only root). Idempotent: when old_name is gone and new_name is there, it succeeds with already_renamed true and nothing changed or saved. Add a socket with add_skeletal_mesh_socket, re-parent one with set_skeletal_mesh_socket_parent, delete one with remove_skeletal_mesh_socket; Skeleton assets: add_skeleton_socket and remove_skeleton_socket; static meshes: rename_static_mesh_socket.
//   @param skeletal_mesh_path - Package or object path of the skeletal mesh asset (e.g. "/Game/Characters/SK_Hero").
//   @param old_name - Current socket name, matched case-insensitively (e.g. "hand_r_socket").
//   @param new_name - New socket name; must differ from old_name and be free on the mesh and its skeleton (a change of letter case only counts as taken).
//   @returns { old_name: string, new_name: string, skeletal_mesh_path: string, renamed: bool, already_renamed: bool, renamed_on_mesh: bool, renamed_on_skeleton: bool, saved_assets: [string], saved: bool, save_error?: string, socket_count: int } -- renamed is read back (new_name found, old_name gone); socket_count adds the mesh's sockets and its skeleton's

TSharedPtr<FJsonObject> HandleRenameSkeletalMeshSocket(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MeshPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("skeletal_mesh_path"), MeshPath)) { return Missing; }

	FString OldName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("old_name"), OldName)) { return Missing; }

	FString NewName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("new_name"), NewName)) { return Missing; }

	if (OldName.Equals(NewName, ESearchCase::CaseSensitive))
		return mcp_common::MakeErrorReply(
			TEXT("old_name and new_name are identical"));

	USkeletalMesh* Mesh = nullptr;
	if (TSharedPtr<FJsonObject> Error = SkMesh_Load(MeshPath, Mesh)) { return Error; }

	const FName OldFName(*OldName);
	const FName NewFName(*NewName);

	// USkeletalMesh::FindSocket searches the mesh's own sockets, then its skeleton's.
	const bool bOldPresent = Mesh->FindSocket(OldFName) != nullptr;
	const bool bNewPresent = Mesh->FindSocket(NewFName) != nullptr;

	auto NewReply = [&](bool bRenamed, bool bAlreadyRenamed)
	{
		TSharedPtr<FJsonObject> Data = SkMesh_NewPayload(MeshPath);
		Data->SetStringField(TEXT("old_name"), OldName);
		Data->SetStringField(TEXT("new_name"), NewName);
		Data->SetBoolField(TEXT("renamed"), bRenamed);
		Data->SetBoolField(TEXT("already_renamed"), bAlreadyRenamed);
		Data->SetNumberField(TEXT("socket_count"), Mesh->NumSockets());
		return Data;
	};

	// A repeat call: the rename it asks for is already the state, so nothing changes and nothing is saved.
	if (!bOldPresent && bNewPresent)
	{
		TSharedPtr<FJsonObject> Data = NewReply(/*bRenamed=*/false, /*bAlreadyRenamed=*/true);
		Data->SetBoolField(TEXT("renamed_on_mesh"), false);
		Data->SetBoolField(TEXT("renamed_on_skeleton"), false);
		Data->SetArrayField(TEXT("saved_assets"), TArray<TSharedPtr<FJsonValue>>());
		Data->SetBoolField(TEXT("saved"), false);
		return mcp_common::MakeOkReply(Data);
	}

	// Honest dedup: if new_name already exists as a socket, fail loud.
	// We do NOT silently coalesce because that would hide a logic error at the
	// call site (the caller almost certainly did not realize they were clobbering).
	if (bNewPresent)
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Socket '%s' already exists on the mesh or its skeleton; refusing to overwrite"), *NewName));

	if (!bOldPresent)
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Socket '%s' not found on the mesh or its skeleton"), *OldName));

	// The assets the rename writes, refused before anything changes when one of them cannot be saved.
	const FSkMeshSocketOwners Owners = SkMesh_SocketOwners(Mesh, OldFName);
	const FString WriteBlocker = SkMesh_SocketOwnersWriteBlocker(Mesh, Owners);
	if (!WriteBlocker.IsEmpty())
	{
		return mcp_common::MakeErrorReply(FString::Printf(
			TEXT("Renaming socket '%s' would change an asset that %s; nothing was changed"), *OldName, *WriteBlocker));
	}

	UE_LOG(LogTemp, Verbose, TEXT("[MCP] rename_skeletal_mesh_socket: %s -> %s on %s"),
		*OldName, *NewName, *MeshPath);

	// RenameSocket calls Modify() on each socket it renames (and on the mesh or skeleton whose preview assets follow).
	if (!USkeletalMeshEditorSubsystem::RenameSocket(Mesh, OldFName, NewFName))
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("RenameSocket failed for '%s' -> '%s'"), *OldName, *NewName));

	// Read back: the new name is found and the old one is gone, on the mesh and its skeleton.
	const bool bRenamed = Mesh->FindSocket(NewFName) != nullptr && Mesh->FindSocket(OldFName) == nullptr;

	TSharedPtr<FJsonObject> Data = NewReply(bRenamed, /*bAlreadyRenamed=*/false);
	Data->SetBoolField(TEXT("renamed_on_mesh"), Owners.bMeshSocket);
	Data->SetBoolField(TEXT("renamed_on_skeleton"), Owners.bSkeletonSocket);
	FString SaveError;
	const bool bSaved = SkMesh_SaveSocketOwners(Mesh, Owners, Data, SaveError);
	if (!bRenamed)
	{
		Data->SetBoolField(TEXT("saved"), bSaved);
		return mcp_common::MakeOutcomeReply(Data, false,
			mcp_common::MeasuredOutcomeError(TEXT("rename_skeletal_mesh_socket"), TEXT("renamed")));
	}
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("rename_skeletal_mesh_socket"), SaveError);
}

// ---------------------------------------------------------------------------
// remove_skeletal_mesh_socket
// ---------------------------------------------------------------------------
// MCP:remove_skeletal_mesh_socket - Deletes a socket from a skeletal mesh asset, together with the same-named socket on its skeleton and the preview assets attached to that socket name. One undo step, then every asset that changed is saved: the mesh for its own socket, the skeleton for its socket (saved_assets lists them), so a socket that lives only on the skeleton is deleted from the skeleton's file too; a failed save makes success false with save_error and leaves the deletion in memory. An asset that would change but cannot be saved (engine content, a read-only root) is refused before anything changes. Idempotent: a name neither the mesh nor its skeleton has returns already_removed true, with nothing changed or saved. A socket that should go from a Skeleton asset alone can also be deleted with remove_skeleton_socket; static meshes use remove_static_mesh_socket. Re-add with add_skeletal_mesh_socket; get_skeletal_mesh_info reports the socket count.
//   @param skeletal_mesh_path - Package or object path of the skeletal mesh asset (e.g. "/Game/Characters/SK_Hero").
//   @param socket_name - Name of the socket to delete, matched case-insensitively (e.g. "hand_r_socket").
//   @returns { socket_name: string, skeletal_mesh_path: string, removed: bool, already_removed: bool, removed_from_mesh: bool, removed_from_skeleton: bool, saved_assets: [string], saved: bool, save_error?: string, socket_count: int } -- removed is read back (the name is no longer found on the mesh or its skeleton)

TSharedPtr<FJsonObject> HandleRemoveSkeletalMeshSocket(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MeshPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("skeletal_mesh_path"), MeshPath)) { return Missing; }

	FString SocketName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("socket_name"), SocketName)) { return Missing; }

	USkeletalMesh* Mesh = nullptr;
	if (TSharedPtr<FJsonObject> Error = SkMesh_Load(MeshPath, Mesh)) { return Error; }

	const FName SocketFName(*SocketName);
	const int32 SocketsBefore = Mesh->NumSockets();

	// Idempotent: socket already absent -> no save, no mutation, return early.
	if (!Mesh->FindSocket(SocketFName))
	{
		TSharedPtr<FJsonObject> Data = SkMesh_NewPayload(MeshPath);
		Data->SetStringField(TEXT("socket_name"), SocketName);
		Data->SetBoolField(TEXT("removed"), false);
		Data->SetBoolField(TEXT("already_removed"), true);
		Data->SetBoolField(TEXT("removed_from_mesh"), false);
		Data->SetBoolField(TEXT("removed_from_skeleton"), false);
		Data->SetArrayField(TEXT("saved_assets"), TArray<TSharedPtr<FJsonValue>>());
		Data->SetBoolField(TEXT("saved"), false);
		Data->SetNumberField(TEXT("socket_count"), SocketsBefore);
		return mcp_common::MakeOkReply(Data);
	}

	// The assets the removal writes, refused before anything changes when one of them cannot be saved.
	const FSkMeshSocketOwners Owners = SkMesh_SocketOwners(Mesh, SocketFName);
	const FString WriteBlocker = SkMesh_SocketOwnersWriteBlocker(Mesh, Owners);
	if (!WriteBlocker.IsEmpty())
	{
		return mcp_common::MakeErrorReply(FString::Printf(
			TEXT("Removing socket '%s' would change an asset that %s; nothing was changed"), *SocketName, *WriteBlocker));
	}

	UE_LOG(LogTemp, Verbose, TEXT("[MCP] remove_skeletal_mesh_socket: '%s' from %s"),
		*SocketName, *MeshPath);

	// RemoveSocket calls Modify() on each socket it removes and on the mesh or skeleton whose list it changes.
#if UE_VERSION_OLDER_THAN(5, 8, 0)
	if (!SkMesh_RemoveSocket(Mesh, SocketFName))
#else
	if (!USkeletalMeshEditorSubsystem::RemoveSocket(Mesh, SocketFName))
#endif
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("RemoveSocket failed for '%s'"), *SocketName));

	// Read back: the name is no longer found on the mesh or its skeleton.
	const bool bRemoved = Mesh->FindSocket(SocketFName) == nullptr;

	TSharedPtr<FJsonObject> Data = SkMesh_NewPayload(MeshPath);
	Data->SetStringField(TEXT("socket_name"), SocketName);
	Data->SetBoolField(TEXT("removed"), bRemoved);
	Data->SetBoolField(TEXT("already_removed"), false);
	Data->SetBoolField(TEXT("removed_from_mesh"), Owners.bMeshSocket);
	Data->SetBoolField(TEXT("removed_from_skeleton"), Owners.bSkeletonSocket);
	Data->SetNumberField(TEXT("socket_count"), Mesh->NumSockets());
	FString SaveError;
	const bool bSaved = SkMesh_SaveSocketOwners(Mesh, Owners, Data, SaveError);
	if (!bRemoved)
	{
		Data->SetBoolField(TEXT("saved"), bSaved);
		return mcp_common::MakeOutcomeReply(Data, false,
			mcp_common::MeasuredOutcomeError(TEXT("remove_skeletal_mesh_socket"), TEXT("removed")));
	}
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("remove_skeletal_mesh_socket"), SaveError);
}

// ---------------------------------------------------------------------------
// set_skeletal_mesh_socket_parent
// ---------------------------------------------------------------------------
// MCP:set_skeletal_mesh_socket_parent - Re-parent an existing socket on a skeletal mesh to a different bone. Validates the bone exists in the reference skeleton.
//   @param skeletal_mesh_path - Content path to the skeletal mesh asset
//   @param socket_name - Name of the socket to re-parent
//   @param parent_bone_name - Bone name to attach the socket to (must exist in skeleton)
//   @returns { socket_name: string, parent_bone_name: string, skeletal_mesh_path: string, reparented: bool }

TSharedPtr<FJsonObject> HandleSetSkeletalMeshSocketParent(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MeshPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("skeletal_mesh_path"), MeshPath)) { return Missing; }

	FString SocketName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("socket_name"), SocketName)) { return Missing; }

	FString BoneNameStr;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("parent_bone_name"), BoneNameStr)) { return Missing; }

	USkeletalMesh* Mesh = nullptr;
	if (TSharedPtr<FJsonObject> Error = SkMesh_Load(MeshPath, Mesh)) { return Error; }

	const FName SocketFName(*SocketName);
	const FName BoneName(*BoneNameStr);

	USkeletalMeshSocket* Socket = Mesh->FindSocket(SocketFName);
	if (!Socket)
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Socket '%s' not found on mesh"), *SocketName));

	if (!BoneExists(Mesh, BoneName))
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Bone '%s' not found in skeleton"), *BoneNameStr));

	UE_LOG(LogTemp, Verbose, TEXT("[MCP] set_skeletal_mesh_socket_parent: '%s' -> '%s' on %s"),
		*SocketName, *BoneNameStr, *MeshPath);

	// USkeletalMeshSocket::SetSocketParent is WITH_EDITOR (header:
	// SkeletalMeshSocket.h). It validates the bone exists in the skeleton
	// itself, but we pre-check above for a clearer error message.
	Mesh->Modify();
	Socket->SetSocketParent(Mesh, BoneName);

	if (TSharedPtr<FJsonObject> SaveFailed = SkMesh_SaveOrReply(Mesh))
		return SaveFailed;

	TSharedPtr<FJsonObject> Data = SkMesh_NewPayload(MeshPath);
	Data->SetStringField(TEXT("socket_name"), SocketName);
	Data->SetStringField(TEXT("parent_bone_name"), BoneNameStr);
	Data->SetBoolField(TEXT("reparented"), true);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_skeletal_mesh_socket_transform
// ---------------------------------------------------------------------------
// MCP:set_skeletal_mesh_socket_transform - Set the local transform (relative to its parent bone) of an existing socket of a skeletal mesh asset, then save the asset that owns the socket: the mesh for one of its own sockets, else its skeleton for a socket that lives only there (socket_owner and saved_asset say which). location, rotation and scale are each optional but at least one is required; only those passed are written and the rest keep their current values, and the reply reads the transform back from the socket. Not an undo step; repeating the call writes and saves the same transform again. A missing socket, or an owner that cannot be saved (engine content, a read-only root), is an error and nothing changes; a failed save makes success false, with saved=false and save_error. Create the socket with add_skeletal_mesh_socket (or add_skeleton_socket on the skeleton), move it to another bone with set_skeletal_mesh_socket_parent, rename it with rename_skeletal_mesh_socket.
//   @param skeletal_mesh_path - Content path of the USkeletalMesh asset (object or package path), e.g. '/Game/Characters/SKM_Hero'.
//   @param socket_name - Name of an existing socket on the mesh (or its skeleton), as add_skeletal_mesh_socket created it, e.g. 'hand_r_socket'.
//   @param location - Optional [x, y, z] offset from the parent bone, in centimetres.
//   @param rotation - Optional [pitch, yaw, roll] rotation relative to the parent bone, in degrees.
//   @param scale - Optional [x, y, z] scale relative to the parent bone, e.g. [1, 1, 1].
//   @returns { socket_name: string, skeletal_mesh_path: string, socket_owner: "mesh"|"skeleton", saved_asset: string, saved: bool, save_error?: string, applied: {location, rotation, scale}, location_x: float, location_y: float, location_z: float, rotation_pitch: float, rotation_yaw: float, rotation_roll: float, scale_x: float, scale_y: float, scale_z: float }

TSharedPtr<FJsonObject> HandleSetSkeletalMeshSocketTransform(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MeshPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("skeletal_mesh_path"), MeshPath)) { return Missing; }

	FString SocketName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("socket_name"), SocketName)) { return Missing; }

	USkeletalMesh* Mesh = nullptr;
	if (TSharedPtr<FJsonObject> Error = SkMesh_Load(MeshPath, Mesh)) { return Error; }

	// The socket and the asset that OWNS it, searched the way USkeletalMesh::FindSocket searches: the mesh's own
	// sockets first, then its skeleton's. A skeleton socket lives in the skeleton package, so that is the package
	// to dirty and save -- saving the mesh would leave the edit in memory only.
	const FName SocketFName(*SocketName);
	USkeletalMeshSocket* Socket = nullptr;
	UObject* SocketOwner = nullptr;
	for (const TObjectPtr<USkeletalMeshSocket>& MeshSocket : Mesh->GetMeshOnlySocketList())
	{
		if (MeshSocket && MeshSocket->SocketName == SocketFName)
		{
			Socket = MeshSocket;
			SocketOwner = Mesh;
			break;
		}
	}
	if (!Socket)
	{
		if (USkeleton* Skeleton = Mesh->GetSkeleton())
		{
			for (const TObjectPtr<USkeletalMeshSocket>& SkeletonSocket : Skeleton->Sockets)
			{
				if (SkeletonSocket && SkeletonSocket->SocketName == SocketFName)
				{
					Socket = SkeletonSocket;
					SocketOwner = Skeleton;
					break;
				}
			}
		}
	}
	if (!Socket || !SocketOwner)
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Socket '%s' not found on the mesh or its skeleton"), *SocketName));
	const bool bOnSkeleton = SocketOwner != Mesh;

	const bool bHasLoc = Params->HasField(TEXT("location"));
	const bool bHasRot = Params->HasField(TEXT("rotation"));
	const bool bHasScale = Params->HasField(TEXT("scale"));
	if (!bHasLoc && !bHasRot && !bHasScale)
		return mcp_common::MakeErrorReply(
			TEXT("At least one of 'location' / 'rotation' / 'scale' must be provided"));

	// Refuse before anything changes when the owning package may not be written (engine content, a read-only root):
	// the save would fail after the edit and leave it in memory only.
	UPackage* OwnerPackage = SocketOwner->GetOutermost();
	const FString WriteBlocker = SkMesh_PackageWriteBlocker(OwnerPackage->GetName());
	if (!WriteBlocker.IsEmpty())
	{
		return mcp_common::MakeErrorReply(FString::Printf(
			TEXT("Socket '%s' lives on the %s %s, which cannot be saved: %s; nothing was changed"),
			*SocketName, bOnSkeleton ? TEXT("skeleton") : TEXT("mesh"), *SocketOwner->GetPathName(), *WriteBlocker));
	}

	const FTransform OldT = Socket->GetSocketLocalTransform();
	FVector NewLoc = OldT.GetLocation();
	FRotator NewRot = OldT.GetRotation().Rotator();
	FVector NewScale = OldT.GetScale3D();

	if (bHasLoc)
	{
		NewLoc = mcp_common::ReadJsonVector(Params, TEXT("location"));
	}
	if (bHasRot)
	{
		// The rotation field is read with ReadJsonRotator, the standard
		// pitch/yaw/roll triple (not roll/pitch/yaw) -- the same reader
		// add_skeletal_mesh_socket uses, for parity.
		NewRot = mcp_common::ReadJsonRotator(Params, TEXT("rotation"));
	}
	if (bHasScale)
	{
		NewScale = mcp_common::ReadJsonVector(Params, TEXT("scale"));
	}

	const FTransform NewT(NewRot, NewLoc, NewScale);

	UE_LOG(LogTemp, Verbose, TEXT("[MCP] set_skeletal_mesh_socket_transform: '%s' on %s "
		"(loc=%d rot=%d scale=%d)"),
		*SocketName, *MeshPath,
		bHasLoc ? 1 : 0, bHasRot ? 1 : 0, bHasScale ? 1 : 0);

	// Modify the owner (mesh or skeleton) and the socket before the write, so the right package is dirtied.
	SocketOwner->Modify();
	Socket->Modify();
	Socket->SetSocketLocalTransform(NewT);

	FString SaveError;
	const bool bSaved = mcp_common::SaveAssetPackage(OwnerPackage, SocketOwner, SaveError);

	TSharedPtr<FJsonObject> AppliedObj = MakeShared<FJsonObject>();
	AppliedObj->SetBoolField(TEXT("location"), bHasLoc);
	AppliedObj->SetBoolField(TEXT("rotation"), bHasRot);
	AppliedObj->SetBoolField(TEXT("scale"), bHasScale);

	// Read back from the socket, not echoed from the request.
	const FTransform ReadT = Socket->GetSocketLocalTransform();
	const FVector ReadLoc = ReadT.GetLocation();
	const FRotator ReadRot = ReadT.GetRotation().Rotator();
	const FVector ReadScale = ReadT.GetScale3D();

	TSharedPtr<FJsonObject> Data = SkMesh_NewPayload(MeshPath);
	Data->SetStringField(TEXT("socket_name"), SocketName);
	Data->SetStringField(TEXT("socket_owner"), bOnSkeleton ? TEXT("skeleton") : TEXT("mesh"));
	Data->SetStringField(TEXT("saved_asset"), SocketOwner->GetPathName());
	Data->SetObjectField(TEXT("applied"), AppliedObj);
	Data->SetNumberField(TEXT("location_x"), ReadLoc.X);
	Data->SetNumberField(TEXT("location_y"), ReadLoc.Y);
	Data->SetNumberField(TEXT("location_z"), ReadLoc.Z);
	Data->SetNumberField(TEXT("rotation_pitch"), ReadRot.Pitch);
	Data->SetNumberField(TEXT("rotation_yaw"), ReadRot.Yaw);
	Data->SetNumberField(TEXT("rotation_roll"), ReadRot.Roll);
	Data->SetNumberField(TEXT("scale_x"), ReadScale.X);
	Data->SetNumberField(TEXT("scale_y"), ReadScale.Y);
	Data->SetNumberField(TEXT("scale_z"), ReadScale.Z);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_skeletal_mesh_socket_transform"), SaveError);
}

// ---------------------------------------------------------------------------
// assign_skeletal_mesh_physics_asset
// ---------------------------------------------------------------------------
// MCP:assign_skeletal_mesh_physics_asset - Assign a physics asset to a skeletal mesh (or pass null/empty to clear it).
//   @param skeletal_mesh_path - Content path to the skeletal mesh asset
//   @param physics_asset_path - Content path to the physics asset (e.g. /Game/MyPhysAsset); empty string clears the assignment
//   @returns { skeletal_mesh_path: string, physics_asset_path: string, assigned: bool, cleared: bool, physics_asset_name: string }

TSharedPtr<FJsonObject> HandleAssignSkeletalMeshPhysicsAsset(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MeshPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("skeletal_mesh_path"), MeshPath)) { return Missing; }

	// Empty path = clear the assignment (AssignPhysicsAsset accepts
	// nullptr and always succeeds in that case, returning true).
	FString PhysAssetPath;
	Params->TryGetStringField(TEXT("physics_asset_path"), PhysAssetPath);

	USkeletalMesh* Mesh = nullptr;
	if (TSharedPtr<FJsonObject> Error = SkMesh_Load(MeshPath, Mesh)) { return Error; }

	UPhysicsAsset* PhysAsset = nullptr;
	if (!PhysAssetPath.IsEmpty())
	{
		PhysAsset = LoadObject<UPhysicsAsset>(nullptr, *PhysAssetPath);
		if (!PhysAsset)
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("Could not load physics asset: %s"), *PhysAssetPath));
	}

	UE_LOG(LogTemp, Verbose, TEXT("[MCP] assign_skeletal_mesh_physics_asset: %s -> %s"),
		*MeshPath,
		PhysAsset ? *PhysAsset->GetPathName() : TEXT("(null/clear)"));

	Mesh->Modify();
	if (!USkeletalMeshEditorSubsystem::AssignPhysicsAsset(Mesh, PhysAsset))
		return mcp_common::MakeErrorReply(
			TEXT("AssignPhysicsAsset failed"));

	if (TSharedPtr<FJsonObject> SaveFailed = SkMesh_SaveOrReply(Mesh))
		return SaveFailed;

	TSharedPtr<FJsonObject> Data = SkMesh_NewPayload(MeshPath);
	Data->SetStringField(TEXT("physics_asset_path"),
		PhysAsset ? PhysAsset->GetPathName() : FString());
	Data->SetBoolField(TEXT("assigned"), PhysAsset != nullptr);
	Data->SetBoolField(TEXT("cleared"), PhysAsset == nullptr);
	Data->SetStringField(TEXT("physics_asset_name"),
		PhysAsset ? PhysAsset->GetName() : FString());
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_morph_target_generated_by_engine
// ---------------------------------------------------------------------------
// MCP:set_morph_target_generated_by_engine - Flag morph targets of a skeletal mesh asset as generated by engine tools rather than imported from a source file, on every LOD, then save the mesh (a failed save is an error). The flag is only ever set, never cleared; names not on the mesh are ignored, and an empty or missing list flags every morph. When any flag changes the engine runs PreEditChange/PostEditChange on the mesh, which rebuilds it. modified says whether anything changed; flagged lists every morph now marked on LOD 0. Names come from get_skeletal_mesh_morph_targets.
//   @param skeletal_mesh_path - Content path to the skeletal mesh asset
//   @param morph_target_names - List of morph target names to flag; empty list = flag every morph on the mesh
//   @returns { skeletal_mesh_path: string, requested_count: int, named_list: bool, modified: bool, flagged: [string], note: string }

TSharedPtr<FJsonObject> HandleSetMorphTargetGeneratedByEngine(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MeshPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("skeletal_mesh_path"), MeshPath)) { return Missing; }

	const TArray<TSharedPtr<FJsonValue>>* NamesArray = nullptr;
	const bool bHasList = Params->TryGetArrayField(TEXT("morph_target_names"), NamesArray) && NamesArray != nullptr;

	USkeletalMesh* Mesh = nullptr;
	if (TSharedPtr<FJsonObject> Error = SkMesh_Load(MeshPath, Mesh)) { return Error; }

	TArray<FString> OptionalNames;
	if (bHasList)
	{
		OptionalNames.Reserve(NamesArray->Num());
		for (const TSharedPtr<FJsonValue>& V : *NamesArray)
		{
			FString S;
			if (V.IsValid() && V->TryGetString(S) && !S.IsEmpty())
			{
				OptionalNames.Add(S);
			}
		}
	}

	UE_LOG(LogTemp, Verbose, TEXT("[MCP] set_morph_target_generated_by_engine: %s (%d names)"),
		*MeshPath, OptionalNames.Num());

	Mesh->Modify();
	const bool bModified = USkeletalMeshEditorSubsystem::SetMorphTargetsToGeneratedByEngine(Mesh, OptionalNames);

	// Read back the flagged set so the caller can verify which morphs are now
	// marked generated-by-engine. This is the public getter and is the
	// only reliable way to confirm "did my edit take?" since the static setter
	// returns a single bool.
	TArray<FName> FlaggedNames;
	USkeletalMeshEditorSubsystem::GetMorphTargetsGeneratedByEngine(Mesh, FlaggedNames);

	if (TSharedPtr<FJsonObject> SaveFailed = SkMesh_SaveOrReply(Mesh))
		return SaveFailed;

	TArray<TSharedPtr<FJsonValue>> FlaggedJson;
	for (const FName& N : FlaggedNames)
	{
		FlaggedJson.Add(MakeShared<FJsonValueString>(N.ToString()));
	}

	TSharedPtr<FJsonObject> Data = SkMesh_NewPayload(MeshPath);
	Data->SetNumberField(TEXT("requested_count"), OptionalNames.Num());
	Data->SetBoolField(TEXT("named_list"), bHasList);
	Data->SetBoolField(TEXT("modified"), bModified);
	Data->SetArrayField(TEXT("flagged"), FlaggedJson);
	Data->SetStringField(TEXT("note"),
		TEXT("Empty morph_target_names flags every morph on the mesh. "
		     "Read back via 'flagged' to confirm what is currently marked."));
	return mcp_common::MakeOkReply(Data);
}

const McpCommandFamilies::FRegistrar Registration =
{
	{ TEXT("add_skeletal_mesh_lod"), &HandleAddSkeletalMeshLod },
	{ TEXT("add_skeletal_mesh_socket"), &HandleAddSkeletalMeshSocket },
	{ TEXT("assign_skeletal_mesh_physics_asset"), &HandleAssignSkeletalMeshPhysicsAsset },
	{ TEXT("bake_skeletal_mesh_lod"), &HandleBakeSkeletalMeshLod },
	{ TEXT("create_skeletal_mesh_morph_target"), &HandleCreateSkeletalMeshMorphTarget },
	{ TEXT("get_morph_target_deltas"), &HandleGetMorphTargetDeltas },
	{ TEXT("get_skeletal_mesh_info"), &HandleGetSkeletalMeshInfo },
	{ TEXT("get_skeletal_mesh_lod_info"), &HandleGetSkeletalMeshLodInfo },
	{ TEXT("get_skeletal_mesh_material_slots"), &HandleGetSkeletalMeshMaterialSlots },
	{ TEXT("get_skeletal_mesh_morph_targets"), &HandleGetSkeletalMeshMorphTargets },
	{ TEXT("get_skeletal_mesh_section_info"), &HandleGetSkeletalMeshSectionInfo },
	{ TEXT("remove_skeletal_mesh_lod"), &HandleRemoveSkeletalMeshLod },
	{ TEXT("remove_skeletal_mesh_morph_target"), &HandleRemoveSkeletalMeshMorphTarget },
	{ TEXT("remove_skeletal_mesh_socket"), &HandleRemoveSkeletalMeshSocket },
	{ TEXT("rename_skeletal_mesh_morph_target"), &HandleRenameSkeletalMeshMorphTarget },
	{ TEXT("rename_skeletal_mesh_socket"), &HandleRenameSkeletalMeshSocket },
	{ TEXT("set_morph_target_deltas"), &HandleSetMorphTargetDeltas },
	{ TEXT("set_morph_target_generated_by_engine"), &HandleSetMorphTargetGeneratedByEngine },
	{ TEXT("set_skeletal_mesh_lod_build_settings"), &HandleSetSkeletalMeshLodBuildSettings },
	{ TEXT("set_skeletal_mesh_lod_reduction_settings"), &HandleSetSkeletalMeshLodReductionSettings },
	{ TEXT("set_skeletal_mesh_lod_settings"), &HandleSetSkeletalMeshLodSettings },
	{ TEXT("set_skeletal_mesh_material_slot"), &HandleSetSkeletalMeshMaterialSlot },
	{ TEXT("set_skeletal_mesh_min_lod"), &HandleSetSkeletalMeshMinLod },
	{ TEXT("set_skeletal_mesh_section_flags"), &HandleSetSkeletalMeshSectionCastShadow },
	{ TEXT("set_skeletal_mesh_socket_parent"), &HandleSetSkeletalMeshSocketParent },
	{ TEXT("set_skeletal_mesh_socket_transform"), &HandleSetSkeletalMeshSocketTransform },
};
} // namespace UEAgentSkeletalMeshCommands
