// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

#include "Editor.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"

// Engine: UStaticMesh, UStaticMeshSocket, UBodySetup
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSocket.h"
#include "PhysicsEngine/BodySetup.h"
#include "BodySetupEnums.h"

// Material slot assignment (LoadObject<UMaterialInterface>, GetPathName)
#include "Materials/MaterialInterface.h"

// StaticMeshEditor subsystem
#include "StaticMeshEditorSubsystem.h"

// Render thread flush
#include "RenderingThread.h"

// Async mesh compilation
#include "StaticMeshCompiler.h"

/**
 * Static Mesh asset commands.
 *
 * The editing commands work on a /Game copy of a mesh (duplicate_asset), never on /Engine content. Queries and
 * Nanite settings go through
 * UStaticMeshEditorSubsystem (StaticMeshEditor module); sockets, collision, material slots and lightmap
 * settings are edited through the UStaticMesh and UBodySetup APIs directly.
 */
namespace UEAgentStaticMeshCommands
{

// MCP-DOMAIN: Asset

namespace
{
	FString CollisionFlagToString(ECollisionTraceFlag Flag)
	{
		switch (Flag)
		{
			case CTF_UseDefault:          return TEXT("default");
			case CTF_UseSimpleAndComplex: return TEXT("simple_and_complex");
			case CTF_UseSimpleAsComplex:  return TEXT("simple_as_complex");
			case CTF_UseComplexAsSimple:  return TEXT("complex_as_simple");
			default:                      return TEXT("unknown");
		}
	}

	ECollisionTraceFlag StringToCollisionFlag(const FString& In)
	{
		const FString Lower = In.ToLower();
		if (Lower == TEXT("simple_and_complex")) { return CTF_UseSimpleAndComplex; }
		if (Lower == TEXT("simple_as_complex")) { return CTF_UseSimpleAsComplex; }
		if (Lower == TEXT("complex_as_simple")) { return CTF_UseComplexAsSimple; }
		return CTF_UseDefault;
	}

	/** The static mesh at MeshPath; the error reply when it does not load. */
	TSharedPtr<FJsonObject> StaticMesh_Load(const FString& MeshPath, UStaticMesh*& OutMesh)
	{
		OutMesh = mcp_common::LoadAssetOrSoftPath<UStaticMesh>(MeshPath);
		if (!OutMesh)
		{
			return mcp_common::MakeErrorReply(FString::Printf(TEXT("Cannot load static mesh: %s"), *MeshPath));
		}
		return nullptr;
	}

	/** Saves the mesh's package: null when it saved, else the error reply carrying the save error. */
	TSharedPtr<FJsonObject> StaticMesh_SaveOrReply(UStaticMesh* Mesh)
	{
		FString SaveError;
		if (!mcp_common::SaveAssetPackage(Mesh->GetOutermost(), Mesh, SaveError))
		{
			return mcp_common::MakeErrorReply(SaveError);
		}
		return nullptr;
	}

	/** A reply payload that starts with static_mesh_path. */
	TSharedPtr<FJsonObject> StaticMesh_NewPayload(const FString& MeshPath)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("static_mesh_path"), MeshPath);
		return Data;
	}

	/**
	 * Null when the optional location, rotation and scale params are absent or well formed; otherwise the error
	 * reply naming the first bad one. Well formed is a list of at least three numbers, or for rotation also a
	 * {pitch, yaw, roll} object: the readers turn anything else into 0,0,0, which as a scale collapses the socket.
	 */
	TSharedPtr<FJsonObject> StaticMesh_CheckSocketTransform(const TSharedPtr<FJsonObject>& Params)
	{
		static const TCHAR* const TransformFields[] = { TEXT("location"), TEXT("rotation"), TEXT("scale") };
		for (const TCHAR* Field : TransformFields)
		{
			if (!Params->HasField(Field))
			{
				continue;
			}
			const bool bRotation = FCString::Strcmp(Field, TEXT("rotation")) == 0;
			if (bRotation && Params->HasTypedField<EJson::Object>(Field))
			{
				continue;
			}
			const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
			const bool bThreeNumbers = Params->TryGetArrayField(Field, Items) && Items != nullptr && Items->Num() >= 3
				&& (*Items)[0]->Type == EJson::Number && (*Items)[1]->Type == EJson::Number && (*Items)[2]->Type == EJson::Number;
			if (!bThreeNumbers)
			{
				return mcp_common::MakeErrorReply(FString::Printf(TEXT("'%s' must be a list of three numbers %s"), Field,
					bRotation ? TEXT("[pitch, yaw, roll] or a {pitch, yaw, roll} object") : TEXT("[x, y, z]")));
			}
		}
		return nullptr;
	}

	/** Writes the optional location, rotation and scale params onto the socket's relative transform. */
	void StaticMesh_ReadSocketTransform(const TSharedPtr<FJsonObject>& Params, UStaticMeshSocket* Socket)
	{
		if (Params->HasField(TEXT("location")))
		{
			Socket->RelativeLocation = mcp_common::ReadJsonVector(Params, TEXT("location"));
		}
		if (Params->HasField(TEXT("rotation")))
		{
			Socket->RelativeRotation = mcp_common::ReadJsonRotator(Params, TEXT("rotation"));
		}
		if (Params->HasField(TEXT("scale")))
		{
			Socket->RelativeScale = mcp_common::ReadJsonVector(Params, TEXT("scale"));
		}
	}

	/** location {x,y,z}, rotation {pitch,yaw,roll} and scale {x,y,z}: the socket's relative transform. */
	void StaticMesh_AddSocketTransform(const TSharedPtr<FJsonObject>& Target, const UStaticMeshSocket* Socket)
	{
		Target->SetObjectField(TEXT("location"), mcp_common::VectorToJsonObject(Socket->RelativeLocation));
		Target->SetObjectField(TEXT("rotation"), mcp_common::RotatorToJsonObject(Socket->RelativeRotation));
		Target->SetObjectField(TEXT("scale"), mcp_common::VectorToJsonObject(Socket->RelativeScale));
	}
}

// ---------------------------------------------------------------------------
// get_static_mesh_info
// ---------------------------------------------------------------------------
// MCP:get_static_mesh_info - Get detailed info about a static mesh without changing it: LODs, verts, materials (with slots), every socket with its transform and tag, nanite, collision, bounds, and the asset-level lightmap settings (lightmap_resolution, the texel resolution static lighting is baked at, and lightmap_coordinate_index, the UV channel holding the lightmap UVs; an override on a placed component is not read). A mesh that does not load is an error. Per-section flags are get_static_mesh_sections.
//   @param static_mesh_path - Package or object path of the UStaticMesh asset, e.g. "/Game/Meshes/SM_Crate".
//   @returns { static_mesh_path, num_lods, num_verts_lod0, num_materials, num_sockets, materials: [{slot_index, slot_name, material_path}], sockets: [{name, location:{x,y,z}, rotation:{pitch,yaw,roll}, scale:{x,y,z}, tag}], collision_complexity, nanite_enabled, lightmap_resolution, lightmap_coordinate_index, bounds_origin_x, bounds_origin_y, bounds_origin_z, bounds_extent_x, bounds_extent_y, bounds_extent_z, sphere_radius }
TSharedPtr<FJsonObject> HandleGetStaticMeshInfo(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MeshPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("static_mesh_path"), MeshPath)) { return Missing; }

	UStaticMesh* Mesh = nullptr;
	if (TSharedPtr<FJsonObject> Error = StaticMesh_Load(MeshPath, Mesh)) { return Error; }

	UStaticMeshEditorSubsystem* SMES = GEditor->GetEditorSubsystem<UStaticMeshEditorSubsystem>();
	if (!SMES)
		return mcp_common::MakeErrorReply(TEXT("UStaticMeshEditorSubsystem unavailable"));

	const int32 NumLODs = SMES->GetLodCount(Mesh);
	const int32 NumVerts = SMES->GetNumberVerts(Mesh, 0);
	const int32 NumMats = SMES->GetNumberMaterials(Mesh);
	const int32 NumSockets = Mesh->Sockets.Num();
	const bool bNanite = SMES->GetNaniteSettings(Mesh).bEnabled;
	const FString CollisionStr = CollisionFlagToString(SMES->GetCollisionComplexity(Mesh));

	const FBoxSphereBounds Bounds = Mesh->GetBounds();

	// Material slots: index/name/path so callers know which indices set_static_mesh_material_slot accepts.
	const TArray<FStaticMaterial>& StaticMaterials = Mesh->GetStaticMaterials();
	TArray<TSharedPtr<FJsonValue>> MaterialsArray;
	for (int32 SlotIndex = 0; SlotIndex < StaticMaterials.Num(); ++SlotIndex)
	{
		const FStaticMaterial& Slot = StaticMaterials[SlotIndex];
		TSharedPtr<FJsonObject> SlotObj = MakeShared<FJsonObject>();
		SlotObj->SetNumberField(TEXT("slot_index"), SlotIndex);
		SlotObj->SetStringField(TEXT("slot_name"), Slot.MaterialSlotName.ToString());
		SlotObj->SetStringField(TEXT("material_path"),
			Slot.MaterialInterface ? Slot.MaterialInterface->GetPathName() : FString());
		MaterialsArray.Add(MakeShared<FJsonValueObject>(SlotObj));
	}

	// Every socket with its relative transform and tag.
	TArray<TSharedPtr<FJsonValue>> SocketsArray;
	for (const TObjectPtr<UStaticMeshSocket>& SocketPtr : Mesh->Sockets)
	{
		const UStaticMeshSocket* Socket = SocketPtr;
		if (!Socket) { continue; }

		TSharedPtr<FJsonObject> SocketObj = MakeShared<FJsonObject>();
		SocketObj->SetStringField(TEXT("name"), Socket->SocketName.ToString());
		StaticMesh_AddSocketTransform(SocketObj, Socket);
		SocketObj->SetStringField(TEXT("tag"), Socket->Tag);
		SocketsArray.Add(MakeShared<FJsonValueObject>(SocketObj));
	}

	TSharedPtr<FJsonObject> Data = StaticMesh_NewPayload(MeshPath);
	Data->SetNumberField(TEXT("num_lods"), NumLODs);
	Data->SetNumberField(TEXT("num_verts_lod0"), NumVerts);
	Data->SetNumberField(TEXT("num_materials"), NumMats);
	Data->SetNumberField(TEXT("num_sockets"), NumSockets);
	Data->SetArrayField(TEXT("materials"), MaterialsArray);
	Data->SetArrayField(TEXT("sockets"), SocketsArray);
	Data->SetStringField(TEXT("collision_complexity"), CollisionStr);
	Data->SetBoolField(TEXT("nanite_enabled"), bNanite);
	Data->SetNumberField(TEXT("lightmap_resolution"), Mesh->GetLightMapResolution());
	Data->SetNumberField(TEXT("lightmap_coordinate_index"), Mesh->GetLightMapCoordinateIndex());
	Data->SetNumberField(TEXT("bounds_origin_x"), Bounds.Origin.X);
	Data->SetNumberField(TEXT("bounds_origin_y"), Bounds.Origin.Y);
	Data->SetNumberField(TEXT("bounds_origin_z"), Bounds.Origin.Z);
	Data->SetNumberField(TEXT("bounds_extent_x"), Bounds.BoxExtent.X);
	Data->SetNumberField(TEXT("bounds_extent_y"), Bounds.BoxExtent.Y);
	Data->SetNumberField(TEXT("bounds_extent_z"), Bounds.BoxExtent.Z);
	Data->SetNumberField(TEXT("sphere_radius"), Bounds.SphereRadius);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_static_mesh_nanite
// ---------------------------------------------------------------------------
// MCP:set_static_mesh_nanite - Set the Nanite settings of a UStaticMesh asset through the StaticMeshEditor subsystem, which rebuilds the mesh, then save it; a failed save is an error. Both fields are always written: omitting enabled disables Nanite and omitting keep_percent_triangles resets it to 1.0. Returns nanite_enabled read back.
//   @param static_mesh_path - Content path to the UStaticMesh asset.
//   @param enabled - true enables Nanite, false disables it; default false, so leaving it out disables Nanite.
//   @param keep_percent_triangles - Fraction of the source triangles Nanite keeps, 1.0 keeps them all (default 1.0, written even when omitted).
//   @returns { static_mesh_path, nanite_enabled, saved }
TSharedPtr<FJsonObject> HandleSetStaticMeshNanite(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MeshPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("static_mesh_path"), MeshPath)) { return Missing; }

	UStaticMesh* Mesh = nullptr;
	if (TSharedPtr<FJsonObject> Error = StaticMesh_Load(MeshPath, Mesh)) { return Error; }

	UStaticMeshEditorSubsystem* SMES = GEditor->GetEditorSubsystem<UStaticMeshEditorSubsystem>();
	if (!SMES)
		return mcp_common::MakeErrorReply(TEXT("UStaticMeshEditorSubsystem unavailable"));

	bool bEnabled = false;
	if (Params->HasField(TEXT("enabled")))
	{
		// TryGetBoolField returns false if missing; check HasField first
		Params->TryGetBoolField(TEXT("enabled"), bEnabled);
	}

	double KeepPercent = 1.0;
	Params->TryGetNumberField(TEXT("keep_percent_triangles"), KeepPercent);

	// Get current settings and modify
	FMeshNaniteSettings Settings = SMES->GetNaniteSettings(Mesh);
	Settings.bEnabled = bEnabled ? 1 : 0;
	Settings.KeepPercentTriangles = static_cast<float>(KeepPercent);

	Mesh->Modify();

	FlushRenderingCommands();
	SMES->SetNaniteSettings(Mesh, Settings, true);
	FlushRenderingCommands();

	if (TSharedPtr<FJsonObject> SaveFailed = StaticMesh_SaveOrReply(Mesh)) { return SaveFailed; }

	// Read back to confirm
	const bool bConfirm = SMES->GetNaniteSettings(Mesh).bEnabled != 0;

	TSharedPtr<FJsonObject> Data = StaticMesh_NewPayload(MeshPath);
	Data->SetBoolField(TEXT("nanite_enabled"), bConfirm);
	Data->SetBoolField(TEXT("saved"), true);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// add_static_mesh_socket
// ---------------------------------------------------------------------------
// MCP:add_static_mesh_socket - Add a new socket to a UStaticMesh asset (package or object path) with an optional relative location, rotation, scale and tag, then save the mesh; no undo transaction. A socket_name the mesh already has is an error, and so is a location, rotation or scale that is not three numbers; both are checked before the mesh is touched. A failed save is an error, yet the socket stays on the mesh in memory. Edit an existing socket with set_static_mesh_socket_transform or rename_static_mesh_socket; list them with get_static_mesh_info; skeletal meshes use add_skeletal_mesh_socket.
//   @param static_mesh_path - Content path to the UStaticMesh asset.
//   @param socket_name - Name for the new socket.
//   @param location - Socket relative location as [x, y, z] (optional)
//   @param rotation - Socket relative rotation as [pitch, yaw, roll] (optional)
//   @param scale - Socket relative scale as [x, y, z] (optional; defaults to 1,1,1)
//   @param tag - Free-form socket tag string (optional)
//   @returns { socket_name, socket_count, location_x, location_y, location_z, scale_x, scale_y, scale_z, tag }
TSharedPtr<FJsonObject> HandleAddStaticMeshSocket(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MeshPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("static_mesh_path"), MeshPath)) { return Missing; }

	FString SocketName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("socket_name"), SocketName)) { return Missing; }

	// Everything is checked before the mesh is touched: a malformed transform field, and a name another socket
	// already has (FindSocket would only ever return the first of two same-named sockets).
	if (TSharedPtr<FJsonObject> Malformed = StaticMesh_CheckSocketTransform(Params)) { return Malformed; }

	UStaticMesh* Mesh = nullptr;
	if (TSharedPtr<FJsonObject> Error = StaticMesh_Load(MeshPath, Mesh)) { return Error; }

	if (Mesh->FindSocket(FName(*SocketName)) != nullptr)
	{
		return mcp_common::MakeErrorReply(FString::Printf(
			TEXT("A socket named '%s' already exists on %s; move it with set_static_mesh_socket_transform or pick another name"),
			*SocketName, *MeshPath));
	}

	Mesh->Modify();

	UStaticMeshSocket* Socket = NewObject<UStaticMeshSocket>(Mesh);
	Socket->SocketName = FName(*SocketName);

	StaticMesh_ReadSocketTransform(Params, Socket);
	FString SocketTag;
	if (Params->TryGetStringField(TEXT("tag"), SocketTag))
	{
		Socket->Tag = SocketTag;
	}

	Mesh->AddSocket(Socket);

	UE_LOG(LogTemp, Verbose, TEXT("add_static_mesh_socket: '%s' on %s (scale=%s tag='%s')"),
		*SocketName, *MeshPath, *Socket->RelativeScale.ToString(), *Socket->Tag);

	if (TSharedPtr<FJsonObject> SaveFailed = StaticMesh_SaveOrReply(Mesh)) { return SaveFailed; }

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("socket_name"), SocketName);
	Data->SetNumberField(TEXT("socket_count"), Mesh->Sockets.Num());
	Data->SetNumberField(TEXT("location_x"), Socket->RelativeLocation.X);
	Data->SetNumberField(TEXT("location_y"), Socket->RelativeLocation.Y);
	Data->SetNumberField(TEXT("location_z"), Socket->RelativeLocation.Z);
	Data->SetNumberField(TEXT("scale_x"), Socket->RelativeScale.X);
	Data->SetNumberField(TEXT("scale_y"), Socket->RelativeScale.Y);
	Data->SetNumberField(TEXT("scale_z"), Socket->RelativeScale.Z);
	Data->SetStringField(TEXT("tag"), Socket->Tag);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// remove_static_mesh_socket
// ---------------------------------------------------------------------------
// MCP:remove_static_mesh_socket - Remove one socket, found by name, from a UStaticMesh asset loaded from its package or object path, then save the mesh; there is no undo transaction. A name the mesh does not have is a success with removed=false, and nothing is touched or saved (idempotent). A mesh that does not load is an error, and so is a failed save -- refused while PIE runs or under a read-only root -- after which the socket is already gone from the mesh in memory. socket_count is the number of sockets left. List sockets with get_static_mesh_info, add one with add_static_mesh_socket, rename one with rename_static_mesh_socket; skeletal meshes use remove_skeletal_mesh_socket and skeletons remove_skeleton_socket.
//   @param static_mesh_path - Package or object path of the UStaticMesh, e.g. "/Game/Meshes/SM_Crate".
//   @param socket_name - Name of the socket to remove, compared as an FName (case-insensitive).
//   @returns { static_mesh_path, socket_name, removed, socket_count }
TSharedPtr<FJsonObject> HandleRemoveStaticMeshSocket(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MeshPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("static_mesh_path"), MeshPath)) { return Missing; }

	FString SocketName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("socket_name"), SocketName)) { return Missing; }

	UStaticMesh* Mesh = nullptr;
	if (TSharedPtr<FJsonObject> Error = StaticMesh_Load(MeshPath, Mesh)) { return Error; }

	UStaticMeshSocket* Socket = Mesh->FindSocket(FName(*SocketName));
	if (!Socket)
	{
		// Idempotent: socket not present -> report removed:false without mutating/saving.
		UE_LOG(LogTemp, Verbose, TEXT("remove_static_mesh_socket: '%s' not found on %s (no-op)"), *SocketName, *MeshPath);
		TSharedPtr<FJsonObject> Data = StaticMesh_NewPayload(MeshPath);
		Data->SetStringField(TEXT("socket_name"), SocketName);
		Data->SetBoolField(TEXT("removed"), false);
		Data->SetNumberField(TEXT("socket_count"), Mesh->Sockets.Num());
		return mcp_common::MakeOkReply(Data);
	}

	Mesh->Modify();
	Mesh->RemoveSocket(Socket);

	UE_LOG(LogTemp, Verbose, TEXT("remove_static_mesh_socket: removed '%s' from %s"), *SocketName, *MeshPath);

	if (TSharedPtr<FJsonObject> SaveFailed = StaticMesh_SaveOrReply(Mesh)) { return SaveFailed; }

	TSharedPtr<FJsonObject> Data = StaticMesh_NewPayload(MeshPath);
	Data->SetStringField(TEXT("socket_name"), SocketName);
	Data->SetBoolField(TEXT("removed"), true);
	Data->SetNumberField(TEXT("socket_count"), Mesh->Sockets.Num());
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_static_mesh_socket_transform
// ---------------------------------------------------------------------------
// MCP:set_static_mesh_socket_transform - Update the relative transform of an existing static mesh socket (only the provided fields). A location, rotation or scale that is not three numbers is an error.
//   @param static_mesh_path - Content path to the UStaticMesh asset.
//   @param socket_name - Name of the socket to update.
//   @param location - Socket relative location as [x, y, z] (optional).
//   @param rotation - Socket relative rotation as [pitch, yaw, roll] (optional).
//   @param scale - Socket relative scale as [x, y, z] (optional).
//   @returns { static_mesh_path, socket_name, updated, location:{x,y,z}, rotation:{pitch,yaw,roll}, scale:{x,y,z}, saved }
TSharedPtr<FJsonObject> HandleSetStaticMeshSocketTransform(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MeshPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("static_mesh_path"), MeshPath)) { return Missing; }

	FString SocketName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("socket_name"), SocketName)) { return Missing; }

	UStaticMesh* Mesh = nullptr;
	if (TSharedPtr<FJsonObject> Error = StaticMesh_Load(MeshPath, Mesh)) { return Error; }

	// A malformed transform field would be written as 0,0,0; refused before the mesh is touched.
	if (TSharedPtr<FJsonObject> Malformed = StaticMesh_CheckSocketTransform(Params)) { return Malformed; }

	UStaticMeshSocket* Socket = Mesh->FindSocket(FName(*SocketName));
	if (!Socket)
	{
		// Honest: socket not present -> report updated:false without mutating/saving.
		UE_LOG(LogTemp, Verbose, TEXT("set_static_mesh_socket_transform: '%s' not found on %s (no-op)"), *SocketName, *MeshPath);
		TSharedPtr<FJsonObject> Data = StaticMesh_NewPayload(MeshPath);
		Data->SetStringField(TEXT("socket_name"), SocketName);
		Data->SetBoolField(TEXT("updated"), false);
		return mcp_common::MakeOkReply(Data);
	}

	Mesh->Modify();
	StaticMesh_ReadSocketTransform(Params, Socket);

	UE_LOG(LogTemp, Verbose, TEXT("set_static_mesh_socket_transform: '%s' on %s (loc=%s rot=%s scale=%s)"),
		*SocketName, *MeshPath, *Socket->RelativeLocation.ToString(), *Socket->RelativeRotation.ToString(), *Socket->RelativeScale.ToString());

	if (TSharedPtr<FJsonObject> SaveFailed = StaticMesh_SaveOrReply(Mesh)) { return SaveFailed; }

	// Read back the transform fields.
	TSharedPtr<FJsonObject> Data = StaticMesh_NewPayload(MeshPath);
	Data->SetStringField(TEXT("socket_name"), SocketName);
	Data->SetBoolField(TEXT("updated"), true);
	StaticMesh_AddSocketTransform(Data, Socket);
	Data->SetBoolField(TEXT("saved"), true);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// rename_static_mesh_socket
// ---------------------------------------------------------------------------
// MCP:rename_static_mesh_socket - Rename an existing static mesh socket (guards against name collisions; idempotent).
//   @param static_mesh_path - Content path to the UStaticMesh asset.
//   @param socket_name - Current name of the socket to rename.
//   @param new_name - New name for the socket.
//   @returns { static_mesh_path, old_name, new_name, renamed, socket_count, saved }
TSharedPtr<FJsonObject> HandleRenameStaticMeshSocket(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MeshPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("static_mesh_path"), MeshPath)) { return Missing; }

	FString SocketName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("socket_name"), SocketName)) { return Missing; }

	FString NewName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("new_name"), NewName)) { return Missing; }

	UStaticMesh* Mesh = nullptr;
	if (TSharedPtr<FJsonObject> Error = StaticMesh_Load(MeshPath, Mesh)) { return Error; }

	UStaticMeshSocket* Socket = Mesh->FindSocket(FName(*SocketName));
	if (!Socket)
	{
		// Honest: socket not present -> report renamed:false without mutating/saving.
		UE_LOG(LogTemp, Verbose, TEXT("rename_static_mesh_socket: '%s' not found on %s (no-op)"), *SocketName, *MeshPath);
		TSharedPtr<FJsonObject> Data = StaticMesh_NewPayload(MeshPath);
		Data->SetStringField(TEXT("old_name"), SocketName);
		Data->SetStringField(TEXT("new_name"), NewName);
		Data->SetBoolField(TEXT("renamed"), false);
		return mcp_common::MakeOkReply(Data);
	}

	// Idempotent: renaming to the same name is a no-op.
	if (NewName == SocketName)
	{
		UE_LOG(LogTemp, Verbose, TEXT("rename_static_mesh_socket: '%s' already named on %s (no-op)"), *SocketName, *MeshPath);
		TSharedPtr<FJsonObject> Data = StaticMesh_NewPayload(MeshPath);
		Data->SetStringField(TEXT("old_name"), SocketName);
		Data->SetStringField(TEXT("new_name"), NewName);
		Data->SetBoolField(TEXT("renamed"), false);
		Data->SetBoolField(TEXT("already_named"), true);
		return mcp_common::MakeOkReply(Data);
	}

	// Collision guard: refuse to shadow a different existing socket.
	UStaticMeshSocket* Other = Mesh->FindSocket(FName(*NewName));
	if (Other && Other != Socket)
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("A socket named '%s' already exists"), *NewName));

	Mesh->Modify();
	Socket->SocketName = FName(*NewName);

	UE_LOG(LogTemp, Verbose, TEXT("rename_static_mesh_socket: '%s' -> '%s' on %s"), *SocketName, *NewName, *MeshPath);

	if (TSharedPtr<FJsonObject> SaveFailed = StaticMesh_SaveOrReply(Mesh)) { return SaveFailed; }

	// Read back: the socket must now resolve under its new name.
	const bool bRenamed = Mesh->FindSocket(FName(*NewName)) != nullptr;

	TSharedPtr<FJsonObject> Data = StaticMesh_NewPayload(MeshPath);
	Data->SetStringField(TEXT("old_name"), SocketName);
	Data->SetStringField(TEXT("new_name"), NewName);
	Data->SetBoolField(TEXT("renamed"), bRenamed);
	Data->SetNumberField(TEXT("socket_count"), Mesh->Sockets.Num());
	Data->SetBoolField(TEXT("saved"), true);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_static_mesh_material_slot
// ---------------------------------------------------------------------------
// MCP:set_static_mesh_material_slot - Assign a material to one existing material slot of a UStaticMesh asset, picked by zero-based material_index, then save the mesh. This edits the asset, so every component without its own override of that slot shows the new material. The slot array never grows: an index outside the existing slots is an error, as is a mesh or material path that does not load. No undo transaction and no same-material check, so a repeat call writes and saves the slot again (idempotent). A failed save -- refused while PIE runs, under a read-only root, or a read-only file -- is an error, with the material already set in memory. material_path is read back. Slots: get_static_mesh_info; per-actor override: set_actor_material; skeletal meshes: set_skeletal_mesh_material_slot; a Blueprint component template: apply_material_to_blueprint.
//   @param static_mesh_path - Content path to the UStaticMesh asset.
//   @param material_index - Zero-based index of an existing material slot.
//   @param material_path - Content path to the UMaterialInterface to assign.
//   @returns { static_mesh_path, material_index, material_path, saved }
TSharedPtr<FJsonObject> HandleSetStaticMeshMaterialSlot(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MeshPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("static_mesh_path"), MeshPath)) { return Missing; }

	int32 MaterialIndex = 0;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("material_index"), MaterialIndex)) { return Missing; }

	FString MaterialPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_path"), MaterialPath)) { return Missing; }

	UStaticMesh* Mesh = nullptr;
	if (TSharedPtr<FJsonObject> Error = StaticMesh_Load(MeshPath, Mesh)) { return Error; }

	// Validate the slot index against the existing slots — never auto-grow the array.
	const int32 NumSlots = Mesh->GetStaticMaterials().Num();
	if (MaterialIndex < 0 || MaterialIndex >= NumSlots)
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("material_index %d out of range [0, %d)"), MaterialIndex, NumSlots));

	UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
	if (!Mat)
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Cannot load material: %s"), *MaterialPath));

	Mesh->Modify();
	Mesh->SetMaterial(MaterialIndex, Mat);

	// SetMaterial queues an async rebuild — drain it before read-back and save.
	FStaticMeshCompilingManager::Get().FinishCompilation({ Mesh });
	FlushRenderingCommands();

	UE_LOG(LogTemp, Verbose, TEXT("set_static_mesh_material_slot: slot %d -> %s on %s"),
		MaterialIndex, *MaterialPath, *MeshPath);

	if (TSharedPtr<FJsonObject> SaveFailed = StaticMesh_SaveOrReply(Mesh)) { return SaveFailed; }

	// Read back the assigned material path.
	UMaterialInterface* ReadBack = Mesh->GetMaterial(MaterialIndex);
	const FString ReadBackPath = ReadBack ? ReadBack->GetPathName() : FString();

	TSharedPtr<FJsonObject> Data = StaticMesh_NewPayload(MeshPath);
	Data->SetNumberField(TEXT("material_index"), MaterialIndex);
	Data->SetStringField(TEXT("material_path"), ReadBackPath);
	Data->SetBoolField(TEXT("saved"), true);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_static_mesh_collision_complexity
// ---------------------------------------------------------------------------
// MCP:set_static_mesh_collision_complexity - Set the collision complexity (BodySetup CollisionTraceFlag) of a UStaticMesh asset, creating its BodySetup when missing, then save it; a failed save is an error. An unrecognized complexity silently becomes default, so check collision_complexity, which is read back.
//   @param static_mesh_path - Content path to the UStaticMesh asset.
//   @param complexity - One of "default", "simple_and_complex", "simple_as_complex", "complex_as_simple", case-insensitive; any other value is stored as default.
//   @returns { static_mesh_path, collision_complexity, saved }
TSharedPtr<FJsonObject> HandleSetStaticMeshCollisionComplexity(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MeshPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("static_mesh_path"), MeshPath)) { return Missing; }

	FString Complexity;
	if (!Params->TryGetStringField(TEXT("complexity"), Complexity) || Complexity.IsEmpty())
		return mcp_common::MakeErrorReply(TEXT("complexity is required (default|simple_and_complex|simple_as_complex|complex_as_simple)"));

	UStaticMesh* Mesh = nullptr;
	if (TSharedPtr<FJsonObject> Error = StaticMesh_Load(MeshPath, Mesh)) { return Error; }

	// Ensure BodySetup exists
	if (!Mesh->GetBodySetup())
	{
		Mesh->CreateBodySetup();
	}
	UBodySetup* BodySetup = Mesh->GetBodySetup();
	if (!BodySetup)
		return mcp_common::MakeErrorReply(TEXT("Failed to create BodySetup"));

	const ECollisionTraceFlag Flag = StringToCollisionFlag(Complexity);

	Mesh->Modify();
	BodySetup->Modify();
	BodySetup->CollisionTraceFlag = Flag;

	if (TSharedPtr<FJsonObject> SaveFailed = StaticMesh_SaveOrReply(Mesh)) { return SaveFailed; }

	// Read back
	UStaticMeshEditorSubsystem* SMES = GEditor->GetEditorSubsystem<UStaticMeshEditorSubsystem>();
	const FString ReadBack = SMES
		? CollisionFlagToString(SMES->GetCollisionComplexity(Mesh))
		: CollisionFlagToString(Flag);

	TSharedPtr<FJsonObject> Data = StaticMesh_NewPayload(MeshPath);
	Data->SetStringField(TEXT("collision_complexity"), ReadBack);
	Data->SetBoolField(TEXT("saved"), true);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_static_mesh_sections
// ---------------------------------------------------------------------------
// MCP:get_static_mesh_sections - List every section of every render LOD of a UStaticMesh asset, loaded from its package or object path, without changing it: lod_index, section_index and the cast_shadow, collision_enabled and visible_in_ray_tracing flags of each, read through the StaticMeshEditor subsystem. A mesh without render data has no LODs and returns an empty list; a mesh that does not load, or a missing subsystem, is an error. total_sections counts the rows. Material slots, vertex counts, sockets, bounds and lightmap settings are get_static_mesh_info; skeletal meshes use get_skeletal_mesh_section_info.
//   @param static_mesh_path - Package or object path of the UStaticMesh, e.g. "/Game/Meshes/SM_Crate".
//   @returns { static_mesh_path: string, sections: [{ lod_index: number, section_index: number, cast_shadow: bool, collision_enabled: bool, visible_in_ray_tracing: bool }], total_sections: number }
TSharedPtr<FJsonObject> HandleGetStaticMeshSections(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MeshPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("static_mesh_path"), MeshPath)) { return Missing; }

	UStaticMesh* Mesh = nullptr;
	if (TSharedPtr<FJsonObject> Error = StaticMesh_Load(MeshPath, Mesh)) { return Error; }

	UStaticMeshEditorSubsystem* SMES = GEditor->GetEditorSubsystem<UStaticMeshEditorSubsystem>();
	if (!SMES)
		return mcp_common::MakeErrorReply(TEXT("UStaticMeshEditorSubsystem unavailable"));

	const int32 NumLODs = Mesh->GetNumLODs();
	TArray<TSharedPtr<FJsonValue>> SectionsArray;

	for (int32 LOD = 0; LOD < NumLODs; ++LOD)
	{
		const int32 NumSections = Mesh->GetNumSections(LOD);
		for (int32 Sec = 0; Sec < NumSections; ++Sec)
		{
			TSharedPtr<FJsonObject> SecObj = MakeShared<FJsonObject>();
			SecObj->SetNumberField(TEXT("lod_index"), LOD);
			SecObj->SetNumberField(TEXT("section_index"), Sec);
			SecObj->SetBoolField(TEXT("cast_shadow"),
				SMES->IsSectionCastShadowEnabled(Mesh, LOD, Sec));
			SecObj->SetBoolField(TEXT("collision_enabled"),
				SMES->IsSectionCollisionEnabled(Mesh, LOD, Sec));
			SecObj->SetBoolField(TEXT("visible_in_ray_tracing"),
				SMES->IsSectionVisibleInRayTracingEnabled(Mesh, LOD, Sec));
			SectionsArray.Add(MakeShared<FJsonValueObject>(SecObj));
		}
	}

	TSharedPtr<FJsonObject> Data = StaticMesh_NewPayload(MeshPath);
	Data->SetArrayField(TEXT("sections"), SectionsArray);
	Data->SetNumberField(TEXT("total_sections"), SectionsArray.Num());
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_static_mesh_lightmap
// ---------------------------------------------------------------------------
// MCP:set_static_mesh_lightmap - Set lightmap resolution and/or lightmap UV coordinate index on a static mesh (no rebuild).
//   @param static_mesh_path - Content path to the UStaticMesh asset.
//   @param lightmap_resolution - Absolute lightmap texture resolution (optional).
//   @param lightmap_coordinate_index - Absolute lightmap UV channel index (optional).
//   @returns { static_mesh_path, lightmap_resolution, lightmap_coordinate_index, applied_count, saved, save_error? }
TSharedPtr<FJsonObject> HandleSetStaticMeshLightmap(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MeshPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("static_mesh_path"), MeshPath)) { return Missing; }

	UStaticMesh* Mesh = nullptr;
	if (TSharedPtr<FJsonObject> Error = StaticMesh_Load(MeshPath, Mesh)) { return Error; }

	const bool bHasResolution = Params->HasField(TEXT("lightmap_resolution"));
	const bool bHasCoordIndex = Params->HasField(TEXT("lightmap_coordinate_index"));
	if (!bHasResolution && !bHasCoordIndex)
		return mcp_common::MakeErrorReply(
			TEXT("At least one of lightmap_resolution or lightmap_coordinate_index is required"));

	Mesh->Modify();

	int32 AppliedCount = 0;

	if (bHasResolution)
	{
		int32 Resolution = 0;
		Params->TryGetNumberField(TEXT("lightmap_resolution"), Resolution);
		Mesh->SetLightMapResolution(Resolution);
		UE_LOG(LogTemp, Verbose, TEXT("set_static_mesh_lightmap: SetLightMapResolution(%d) on %s"), Resolution, *MeshPath);
		++AppliedCount;
	}

	if (bHasCoordIndex)
	{
		int32 CoordIndex = 0;
		Params->TryGetNumberField(TEXT("lightmap_coordinate_index"), CoordIndex);
		Mesh->SetLightMapCoordinateIndex(CoordIndex);
		UE_LOG(LogTemp, Verbose, TEXT("set_static_mesh_lightmap: SetLightMapCoordinateIndex(%d) on %s"), CoordIndex, *MeshPath);
		++AppliedCount;
	}

	// Persist via SavePackage directly. These are serialized UPROPERTYs, NOT DDC-built;
	// do NOT call PostEditChange()/Build() — that would queue an unnecessary mesh rebuild.
	UPackage* Pkg = Mesh->GetOutermost();
	FString SaveError;
	const bool bSaved = mcp_common::SaveAssetPackage(Pkg, Mesh, SaveError);

	// Read back the actual values on the mesh.
	const int32 ReadResolution = Mesh->GetLightMapResolution();
	const int32 ReadCoordIndex = Mesh->GetLightMapCoordinateIndex();

	TSharedPtr<FJsonObject> Data = StaticMesh_NewPayload(MeshPath);
	Data->SetNumberField(TEXT("lightmap_resolution"), ReadResolution);
	Data->SetNumberField(TEXT("lightmap_coordinate_index"), ReadCoordIndex);
	Data->SetNumberField(TEXT("applied_count"), AppliedCount);
	if (!bSaved)
		Data->SetStringField(TEXT("save_error"), SaveError);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_static_mesh_lightmap"));
}

const McpCommandFamilies::FRegistrar Registration =
{
	{ TEXT("add_static_mesh_socket"), &HandleAddStaticMeshSocket },
	{ TEXT("get_static_mesh_info"), &HandleGetStaticMeshInfo },
	{ TEXT("get_static_mesh_sections"), &HandleGetStaticMeshSections },
	{ TEXT("remove_static_mesh_socket"), &HandleRemoveStaticMeshSocket },
	{ TEXT("rename_static_mesh_socket"), &HandleRenameStaticMeshSocket },
	{ TEXT("set_static_mesh_collision_complexity"), &HandleSetStaticMeshCollisionComplexity },
	{ TEXT("set_static_mesh_lightmap"), &HandleSetStaticMeshLightmap },
	{ TEXT("set_static_mesh_material_slot"), &HandleSetStaticMeshMaterialSlot },
	{ TEXT("set_static_mesh_nanite"), &HandleSetStaticMeshNanite },
	{ TEXT("set_static_mesh_socket_transform"), &HandleSetStaticMeshSocketTransform },
};
} // namespace UEAgentStaticMeshCommands
