#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "KismetProceduralMeshLibrary.h"
#include "ProceduralMeshComponent.h"

/**
 * Procedural Mesh Component commands.
 *
 * Wraps the engine's ProceduralMeshComponent plugin (UProceduralMeshComponent
 * + UKismetProceduralMeshLibrary) so an LLM can author mesh sections at
 * runtime: spawn a procedural mesh component on an actor, generate the
 * canonical box / grid geometry, push it into a section, update, clear or
 * hide sections, read back section info and raw data, slice the mesh with a
 * plane, assign materials and remove the component.
 *
 * All commands are headless-safe: no PIE, no render — they work on the
 * component and its CPU-side section buffers (FProcMeshSection). The
 * procedural mesh does not require a /Game asset; sections are built
 * in-memory and the component renders them when added to the world.
 */
namespace UEAgentProceduralMeshCommands
{

// MCP-DOMAIN: Mesh

namespace
{
	UProceduralMeshComponent* ResolvePMC(UWorld* World, const FString& ActorLabel, int32 ComponentIndex)
	{
		AActor* Actor = mcp_common::FindActorByLabel(World, ActorLabel);
		if (!Actor) { return nullptr; }
		TArray<UProceduralMeshComponent*> PMCs;
		Actor->GetComponents<UProceduralMeshComponent>(PMCs);
		if (!PMCs.IsValidIndex(ComponentIndex)) { return nullptr; }
		return PMCs[ComponentIndex];
	}

	bool ReadVectorArray(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, TArray<FVector>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Params->TryGetArrayField(Field, Arr) || !Arr) { return false; }
		Out.Reset();
		Out.Reserve(Arr->Num());
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			const TArray<TSharedPtr<FJsonValue>>* Triple = nullptr;
			if (V->TryGetArray(Triple) && Triple && Triple->Num() == 3)
			{
				Out.Emplace((*Triple)[0]->AsNumber(), (*Triple)[1]->AsNumber(), (*Triple)[2]->AsNumber());
			}
		}
		return Out.Num() > 0;
	}

	/** The PMC ComponentIndex of the editor-world actor labelled ActorLabel; on failure returns nullptr and fills OutError. */
	UProceduralMeshComponent* ProceduralMesh_FindComponent(const FString& ActorLabel, int32 ComponentIndex,
		TSharedPtr<FJsonObject>& OutError)
	{
		UWorld* World = nullptr;
		if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World))
		{
			OutError = NoWorld;
			return nullptr;
		}
		UProceduralMeshComponent* PMC = ResolvePMC(World, ActorLabel, ComponentIndex);
		if (!PMC)
		{
			OutError = mcp_common::MakeErrorReply(
				FString::Printf(TEXT("No UProceduralMeshComponent on actor '%s' at index %d"),
					*ActorLabel, ComponentIndex));
		}
		return PMC;
	}

	/** A reply payload that starts with actor_label and section_index. */
	TSharedPtr<FJsonObject> ProceduralMesh_NewSectionData(const FString& ActorLabel, int32 SectionIndex)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("actor_label"), ActorLabel);
		Data->SetNumberField(TEXT("section_index"), SectionIndex);
		return Data;
	}
}

// ---------------------------------------------------------------------------
// create_procedural_mesh_component {actor_label, location?=[x,y,z]}
//   Spawns a bare AActor in the editor world and attaches a fresh
//   UProceduralMeshComponent as its root. Returns the component index
//   (0) — useful so the caller can address it from the next commands.
// ---------------------------------------------------------------------------
// MCP:create_procedural_mesh_component - Spawn a bare AActor in the editor world and attach a fresh UProceduralMeshComponent as its root. Idempotent: reuses the actor's existing PMC if found.
//   @param actor_label - Editor label for the new actor
//   @param location - Optional [x, y, z] world location; defaults to (0,0,0)
//   @returns { actor_label: string, actor_name: string, component_index: int, section_count: int, reused: bool }

TSharedPtr<FJsonObject> HandleCreateProceduralMeshComponent(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ActorLabel;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_label"), ActorLabel)) { return Missing; }
	UWorld* World = nullptr;
	if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World)) { return NoWorld; }

	// Idempotency: if an actor with this label already exists AND has a PMC,
	// return it instead of failing. This avoids "actor name already exists"
	// on re-runs.
	if (AActor* Existing = mcp_common::FindActorByLabel(World, ActorLabel))
	{
		TArray<UProceduralMeshComponent*> ExistingPMCs;
		Existing->GetComponents<UProceduralMeshComponent>(ExistingPMCs);
		if (ExistingPMCs.Num() > 0)
		{
			TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("actor_label"), Existing->GetActorLabel());
			Data->SetStringField(TEXT("actor_name"), Existing->GetName());
			Data->SetNumberField(TEXT("component_index"), 0);
			Data->SetNumberField(TEXT("section_count"), ExistingPMCs[0]->GetNumSections());
			Data->SetBoolField(TEXT("reused"), true);
			return mcp_common::MakeOkReply(Data);
		}
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	FVector Location = FVector::ZeroVector;
	const TArray<TSharedPtr<FJsonValue>>* LocArr = nullptr;
	if (Params->TryGetArrayField(TEXT("location"), LocArr) && LocArr && LocArr->Num() == 3)
	{
		Location.X = (*LocArr)[0]->AsNumber();
		Location.Y = (*LocArr)[1]->AsNumber();
		Location.Z = (*LocArr)[2]->AsNumber();
	}

	AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), Location, FRotator::ZeroRotator, SpawnParams);
	if (!Actor)
	{
		return mcp_common::MakeErrorReply(TEXT("Failed to spawn actor"));
	}
	Actor->SetActorLabel(ActorLabel);

	UProceduralMeshComponent* PMC = NewObject<UProceduralMeshComponent>(Actor, UProceduralMeshComponent::StaticClass(),
		TEXT("ProceduralMesh"), RF_Transactional);
	if (!PMC)
	{
		return mcp_common::MakeErrorReply(TEXT("Failed to create UProceduralMeshComponent"));
	}
	PMC->SetupAttachment(Actor->GetRootComponent());
	PMC->RegisterComponent();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
	Data->SetStringField(TEXT("actor_name"), Actor->GetName());
	Data->SetNumberField(TEXT("component_index"), 0);
	Data->SetNumberField(TEXT("section_count"), PMC->GetNumSections());
	Data->SetBoolField(TEXT("reused"), false);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// create_procedural_box_mesh {actor_label, component_index?, section_index?,
//   box_radius?=[x,y,z], create_collision?=false}
//   Generates a box mesh with UKismetProceduralMeshLibrary::GenerateBoxMesh
//   and pushes it into the procedural mesh component as one section.
// ---------------------------------------------------------------------------
// MCP:create_procedural_box_mesh - Generate a box mesh via UKismetProceduralMeshLibrary::GenerateBoxMesh and push it into a procedural mesh component as a single section.
//   @param actor_label - Editor label of the actor carrying the PMC
//   @param component_index - Optional 0-based PMC index on the actor; defaults to 0
//   @param section_index - Optional target section index; defaults to 0
//   @param box_radius - Optional [x, y, z] half-extents; defaults to (50, 50, 50)
//   @param create_collision - Optional bool; defaults to false
//   @returns { actor_label: string, section_index: int, vertex_count: int, triangle_count: int, total_sections: int }

TSharedPtr<FJsonObject> HandleCreateProceduralBoxMesh(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ActorLabel;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_label"), ActorLabel)) { return Missing; }
	int32 ComponentIndex = 0;
	Params->TryGetNumberField(TEXT("component_index"), ComponentIndex);
	int32 SectionIndex = 0;
	Params->TryGetNumberField(TEXT("section_index"), SectionIndex);
	bool bCreateCollision = false;
	Params->TryGetBoolField(TEXT("create_collision"), bCreateCollision);

	FVector BoxRadius(50.f, 50.f, 50.f);
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Params->TryGetArrayField(TEXT("box_radius"), Arr) && Arr && Arr->Num() == 3)
		{
			BoxRadius.X = (*Arr)[0]->AsNumber();
			BoxRadius.Y = (*Arr)[1]->AsNumber();
			BoxRadius.Z = (*Arr)[2]->AsNumber();
		}
	}

	TSharedPtr<FJsonObject> Err;
	UProceduralMeshComponent* PMC = ProceduralMesh_FindComponent(ActorLabel, ComponentIndex, Err);
	if (!PMC) { return Err; }

	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	TArray<FProcMeshTangent> Tangents;
	UKismetProceduralMeshLibrary::GenerateBoxMesh(BoxRadius, Vertices, Triangles, Normals, UVs, Tangents);

	PMC->CreateMeshSection_LinearColor(SectionIndex, Vertices, Triangles, Normals, UVs,
		TArray<FLinearColor>(), Tangents, bCreateCollision);

	TSharedPtr<FJsonObject> Data = ProceduralMesh_NewSectionData(ActorLabel, SectionIndex);
	Data->SetNumberField(TEXT("vertex_count"), Vertices.Num());
	Data->SetNumberField(TEXT("triangle_count"), Triangles.Num() / 3);
	Data->SetNumberField(TEXT("total_sections"), PMC->GetNumSections());
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// create_procedural_grid_mesh {actor_label, component_index?, section_index?,
//   num_x?=10, num_y?=10, grid_spacing?=16.0, create_collision?=false}
// ---------------------------------------------------------------------------
// MCP:create_procedural_grid_mesh - Generate a welded grid mesh via UKismetProceduralMeshLibrary::CreateGridMeshWelded and push it into a procedural mesh component as a single section.
//   @param actor_label - Editor label of the actor carrying the PMC
//   @param component_index - Optional 0-based PMC index on the actor; defaults to 0
//   @param section_index - Optional target section index; defaults to 0
//   @param num_x - Optional X subdivision count; defaults to 10
//   @param num_y - Optional Y subdivision count; defaults to 10
//   @param grid_spacing - Optional grid cell size in world units; defaults to 16.0
//   @param create_collision - Optional bool; defaults to false
//   @returns { actor_label: string, section_index: int, vertex_count: int, triangle_count: int, total_sections: int }

TSharedPtr<FJsonObject> HandleCreateProceduralGridMesh(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ActorLabel;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_label"), ActorLabel)) { return Missing; }
	int32 ComponentIndex = 0;
	Params->TryGetNumberField(TEXT("component_index"), ComponentIndex);
	int32 SectionIndex = 0;
	Params->TryGetNumberField(TEXT("section_index"), SectionIndex);
	int32 NumX = 10;
	Params->TryGetNumberField(TEXT("num_x"), NumX);
	int32 NumY = 10;
	Params->TryGetNumberField(TEXT("num_y"), NumY);
	double GridSpacing = 16.0;
	Params->TryGetNumberField(TEXT("grid_spacing"), GridSpacing);
	bool bCreateCollision = false;
	Params->TryGetBoolField(TEXT("create_collision"), bCreateCollision);

	TSharedPtr<FJsonObject> Err;
	UProceduralMeshComponent* PMC = ProceduralMesh_FindComponent(ActorLabel, ComponentIndex, Err);
	if (!PMC) { return Err; }

	TArray<int32> Triangles;
	TArray<FVector> Vertices;
	TArray<FVector2D> UVs;
	UKismetProceduralMeshLibrary::CreateGridMeshWelded(NumX, NumY, Triangles, Vertices, UVs,
		static_cast<float>(GridSpacing));

	// Welded grid has no per-vertex normals — UKismetProceduralMeshLibrary
	// supplies UVs but not normals/tangents. Pass empty normals; PMC allows
	// that (it will recompute internally as needed).
	TArray<FVector> Normals;
	TArray<FProcMeshTangent> Tangents;
	PMC->CreateMeshSection_LinearColor(SectionIndex, Vertices, Triangles, Normals, UVs,
		TArray<FLinearColor>(), Tangents, bCreateCollision);

	TSharedPtr<FJsonObject> Data = ProceduralMesh_NewSectionData(ActorLabel, SectionIndex);
	Data->SetNumberField(TEXT("vertex_count"), Vertices.Num());
	Data->SetNumberField(TEXT("triangle_count"), Triangles.Num() / 3);
	Data->SetNumberField(TEXT("total_sections"), PMC->GetNumSections());
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// update_procedural_mesh_section {actor_label, component_index?,
//   section_index?, vertices?=[[x,y,z],...]}
//   Updates an existing section in-place (same topology, new positions).
// ---------------------------------------------------------------------------
// MCP:update_procedural_mesh_section - Update an existing procedural mesh section in-place (same topology, new vertex positions).
//   @param actor_label - Editor label of the actor carrying the PMC
//   @param component_index - Optional 0-based PMC index on the actor; defaults to 0
//   @param section_index - Optional target section index; defaults to 0
//   @param vertices - Required [[x, y, z], ...] array with the same vertex count as the existing section
//   @returns { actor_label: string, section_index: int, vertex_count: int }

TSharedPtr<FJsonObject> HandleUpdateProceduralMeshSection(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ActorLabel;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_label"), ActorLabel)) { return Missing; }
	int32 ComponentIndex = 0;
	Params->TryGetNumberField(TEXT("component_index"), ComponentIndex);
	int32 SectionIndex = 0;
	Params->TryGetNumberField(TEXT("section_index"), SectionIndex);

	TArray<FVector> Vertices;
	if (!ReadVectorArray(Params, TEXT("vertices"), Vertices))
	{
		return mcp_common::MakeErrorReply(
			TEXT("vertices array of [x,y,z] triples is required"));
	}

	TSharedPtr<FJsonObject> Err;
	UProceduralMeshComponent* PMC = ProceduralMesh_FindComponent(ActorLabel, ComponentIndex, Err);
	if (!PMC) { return Err; }

	const FProcMeshSection* Existing = PMC->GetProcMeshSection(SectionIndex);
	if (!Existing)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Section %d does not exist on actor '%s'"),
				SectionIndex, *ActorLabel));
	}
	if (Vertices.Num() != Existing->ProcVertexBuffer.Num())
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Vertex count mismatch: supplied %d, section has %d"),
				Vertices.Num(), Existing->ProcVertexBuffer.Num()));
	}

	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	PMC->UpdateMeshSection_LinearColor(SectionIndex, Vertices, Normals, UVs,
		TArray<FLinearColor>(), TArray<FProcMeshTangent>(), false);

	TSharedPtr<FJsonObject> Data = ProceduralMesh_NewSectionData(ActorLabel, SectionIndex);
	Data->SetNumberField(TEXT("vertex_count"), Vertices.Num());
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// clear_procedural_mesh {actor_label, component_index?, section_index?=ALL}
//   If section_index is supplied, clear that section; otherwise clear all.
// ---------------------------------------------------------------------------
// MCP:clear_procedural_mesh - Clear a single section or all sections on a procedural mesh component.
//   @param actor_label - Editor label of the actor carrying the PMC
//   @param component_index - Optional 0-based PMC index on the actor; defaults to 0
//   @param section_index - Optional target section; defaults to INDEX_NONE (clear all)
//   @returns { actor_label: string, section_cleared: int, remaining_sections: int }

TSharedPtr<FJsonObject> HandleClearProceduralMesh(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ActorLabel;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_label"), ActorLabel)) { return Missing; }
	int32 ComponentIndex = 0;
	Params->TryGetNumberField(TEXT("component_index"), ComponentIndex);
	int32 SectionIndex = INDEX_NONE;
	Params->TryGetNumberField(TEXT("section_index"), SectionIndex);

	TSharedPtr<FJsonObject> Err;
	UProceduralMeshComponent* PMC = ProceduralMesh_FindComponent(ActorLabel, ComponentIndex, Err);
	if (!PMC) { return Err; }

	if (SectionIndex == INDEX_NONE)
	{
		PMC->ClearAllMeshSections();
	}
	else
	{
		PMC->ClearMeshSection(SectionIndex);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_label"), ActorLabel);
	Data->SetNumberField(TEXT("section_cleared"), SectionIndex);
	Data->SetNumberField(TEXT("remaining_sections"), PMC->GetNumSections());
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_procedural_mesh_info {actor_label, component_index?}
// ---------------------------------------------------------------------------
// MCP:get_procedural_mesh_info - Read summary info (section count, collision flags, per-section vertex/triangle counts) for a procedural mesh component on a named actor.
//   @param actor_label - Editor label of the actor carrying the PMC
//   @param component_index - Optional 0-based PMC index on the actor; defaults to 0
//   @returns { actor_label: string, num_sections: int, use_complex_as_simple_collision: bool, use_async_cooking: bool, sections: [{ index, vertex_count, triangle_count }] }

TSharedPtr<FJsonObject> HandleGetProceduralMeshInfo(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ActorLabel;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_label"), ActorLabel)) { return Missing; }
	int32 ComponentIndex = 0;
	Params->TryGetNumberField(TEXT("component_index"), ComponentIndex);

	TSharedPtr<FJsonObject> Err;
	UProceduralMeshComponent* PMC = ProceduralMesh_FindComponent(ActorLabel, ComponentIndex, Err);
	if (!PMC) { return Err; }

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_label"), ActorLabel);
	Data->SetNumberField(TEXT("num_sections"), PMC->GetNumSections());
	Data->SetBoolField(TEXT("use_complex_as_simple_collision"), PMC->bUseComplexAsSimpleCollision);
	Data->SetBoolField(TEXT("use_async_cooking"), PMC->bUseAsyncCooking);

	TArray<TSharedPtr<FJsonValue>> SectionArr;
	for (int32 i = 0; i < PMC->GetNumSections(); ++i)
	{
		const FProcMeshSection* Section = PMC->GetProcMeshSection(i);
		TSharedPtr<FJsonObject> SectionObj = MakeShared<FJsonObject>();
		SectionObj->SetNumberField(TEXT("vertex_count"), Section ? Section->ProcVertexBuffer.Num() : 0);
		SectionObj->SetNumberField(TEXT("triangle_count"), Section ? Section->ProcIndexBuffer.Num() / 3 : 0);
		SectionObj->SetBoolField(TEXT("visible"), PMC->IsMeshSectionVisible(i));
		SectionObj->SetBoolField(TEXT("collision_enabled"), Section ? Section->bEnableCollision : false);
		SectionArr.Add(MakeShared<FJsonValueObject>(SectionObj));
	}
	Data->SetArrayField(TEXT("sections"), SectionArr);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// Section visibility, raw section readback, slice.
// ---------------------------------------------------------------------------

// MCP:set_procedural_mesh_section_visible - Toggle the visibility of a single section on a procedural mesh component. Idempotent: sets the section's bSectionVisible flag directly.
//   @param actor_label - Editor label of the actor carrying the PMC
//   @param component_index - Optional 0-based PMC index on the actor; defaults to 0
//   @param section_index - Target section index; defaults to 0
//   @param visible - Required bool; new visibility for the section
//   @returns { actor_label: string, section_index: int, visible: bool, total_sections: int }

TSharedPtr<FJsonObject> HandleSetProceduralMeshSectionVisible(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ActorLabel;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_label"), ActorLabel)) { return Missing; }
	bool bVisible = false;
	if (!Params->TryGetBoolField(TEXT("visible"), bVisible))
	{
		return mcp_common::MakeErrorReply(TEXT("visible (bool) is required"));
	}
	int32 ComponentIndex = 0;
	Params->TryGetNumberField(TEXT("component_index"), ComponentIndex);
	int32 SectionIndex = 0;
	Params->TryGetNumberField(TEXT("section_index"), SectionIndex);

	TSharedPtr<FJsonObject> Err;
	UProceduralMeshComponent* PMC = ProceduralMesh_FindComponent(ActorLabel, ComponentIndex, Err);
	if (!PMC) { return Err; }
	// SetMeshSectionVisible silently no-ops on out-of-range indices; validate up-front
	// so the caller gets an honest error instead of a false success.
	if (SectionIndex < 0 || SectionIndex >= PMC->GetNumSections())
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Section index %d out of range (num_sections=%d) on actor '%s'"),
				SectionIndex, PMC->GetNumSections(), *ActorLabel));
	}

	PMC->SetMeshSectionVisible(SectionIndex, bVisible);

	TSharedPtr<FJsonObject> Data = ProceduralMesh_NewSectionData(ActorLabel, SectionIndex);
	Data->SetBoolField(TEXT("visible"), PMC->IsMeshSectionVisible(SectionIndex));
	Data->SetNumberField(TEXT("total_sections"), PMC->GetNumSections());
	return mcp_common::MakeOkReply(Data);
}

// MCP:get_procedural_mesh_section_visible - Read the visibility flag of a single section on a procedural mesh component.
//   @param actor_label - Editor label of the actor carrying the PMC
//   @param component_index - Optional 0-based PMC index on the actor; defaults to 0
//   @param section_index - Target section index; defaults to 0
//   @returns { actor_label: string, section_index: int, visible: bool }

TSharedPtr<FJsonObject> HandleGetProceduralMeshSectionVisible(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ActorLabel;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_label"), ActorLabel)) { return Missing; }
	int32 ComponentIndex = 0;
	Params->TryGetNumberField(TEXT("component_index"), ComponentIndex);
	int32 SectionIndex = 0;
	Params->TryGetNumberField(TEXT("section_index"), SectionIndex);

	TSharedPtr<FJsonObject> Err;
	UProceduralMeshComponent* PMC = ProceduralMesh_FindComponent(ActorLabel, ComponentIndex, Err);
	if (!PMC) { return Err; }
	// IsMeshSectionVisible returns false for out-of-range indices, which is
	// indistinguishable from a genuinely hidden section — validate explicitly.
	if (SectionIndex < 0 || SectionIndex >= PMC->GetNumSections())
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Section index %d out of range (num_sections=%d) on actor '%s'"),
				SectionIndex, PMC->GetNumSections(), *ActorLabel));
	}

	TSharedPtr<FJsonObject> Data = ProceduralMesh_NewSectionData(ActorLabel, SectionIndex);
	Data->SetBoolField(TEXT("visible"), PMC->IsMeshSectionVisible(SectionIndex));
	return mcp_common::MakeOkReply(Data);
}

// MCP:get_procedural_mesh_section_data - Read back the raw geometry (vertices, triangles, normals, UV0, tangents, colors) of a single procedural mesh section.
//   @param actor_label - Editor label of the actor carrying the PMC
//   @param component_index - Optional 0-based PMC index on the actor; defaults to 0
//   @param section_index - Required target section index
//   @returns { actor_label, section_index, vertex_count, triangle_count, vertices, triangles, normals, uv0, tangents, colors, local_box, collision_enabled, visible }

TSharedPtr<FJsonObject> HandleGetProceduralMeshSectionData(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ActorLabel;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_label"), ActorLabel)) { return Missing; }
	int32 ComponentIndex = 0;
	Params->TryGetNumberField(TEXT("component_index"), ComponentIndex);
	int32 SectionIndex = 0;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("section_index"), SectionIndex)) { return Missing; }

	TSharedPtr<FJsonObject> Err;
	UProceduralMeshComponent* PMC = ProceduralMesh_FindComponent(ActorLabel, ComponentIndex, Err);
	if (!PMC) { return Err; }
	const FProcMeshSection* Section = PMC->GetProcMeshSection(SectionIndex);
	if (!Section)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Section %d does not exist on actor '%s'"),
				SectionIndex, *ActorLabel));
	}

	auto Vec3 = [](const FVector& V)
	{
		TArray<TSharedPtr<FJsonValue>> A = mcp_common::VectorToJsonArray(V);
		return A;
	};

	TArray<TSharedPtr<FJsonValue>> Vertices, Normals, UV0, Tangents, Colors;
	for (const FProcMeshVertex& V : Section->ProcVertexBuffer)
	{
		Vertices.Add(MakeShared<FJsonValueArray>(Vec3(V.Position)));
		Normals.Add(MakeShared<FJsonValueArray>(Vec3(V.Normal)));

		UV0.Add(MakeShared<FJsonValueArray>(mcp_common::Vector2DToJsonArray(V.UV0)));

		// FProcMeshTangent is { FVector TangentX; bool bFlipTangentY } — not a plain vector.
		TArray<TSharedPtr<FJsonValue>> Tan = Vec3(V.Tangent.TangentX);
		Tan.Add(MakeShared<FJsonValueBoolean>(V.Tangent.bFlipTangentY));
		Tangents.Add(MakeShared<FJsonValueArray>(Tan));

		// FColor is 0-255 bytes (not FLinearColor).
		TArray<TSharedPtr<FJsonValue>> Col = mcp_common::ColorToJsonArray(V.Color);
		Colors.Add(MakeShared<FJsonValueArray>(Col));
	}

	TArray<TSharedPtr<FJsonValue>> Triangles;
	for (uint32 Index : Section->ProcIndexBuffer)
	{
		Triangles.Add(MakeShared<FJsonValueNumber>(static_cast<double>(Index)));
	}

	TSharedPtr<FJsonObject> Box = MakeShared<FJsonObject>();
	Box->SetArrayField(TEXT("min"), Vec3(Section->SectionLocalBox.Min));
	Box->SetArrayField(TEXT("max"), Vec3(Section->SectionLocalBox.Max));

	TSharedPtr<FJsonObject> Data = ProceduralMesh_NewSectionData(ActorLabel, SectionIndex);
	Data->SetNumberField(TEXT("vertex_count"), Section->ProcVertexBuffer.Num());
	Data->SetNumberField(TEXT("triangle_count"), Section->ProcIndexBuffer.Num() / 3);
	Data->SetArrayField(TEXT("vertices"), Vertices);
	Data->SetArrayField(TEXT("triangles"), Triangles);
	Data->SetArrayField(TEXT("normals"), Normals);
	Data->SetArrayField(TEXT("uv0"), UV0);
	Data->SetArrayField(TEXT("tangents"), Tangents);
	Data->SetArrayField(TEXT("colors"), Colors);
	Data->SetObjectField(TEXT("local_box"), Box);
	Data->SetBoolField(TEXT("collision_enabled"), Section->bEnableCollision);
	Data->SetBoolField(TEXT("visible"), Section->bSectionVisible);
	return mcp_common::MakeOkReply(Data);
}

// MCP:slice_procedural_mesh - Slice a procedural mesh in place with a plane, optionally spawning the other half as a new PMC on the same actor. NOT idempotent: mutates the mesh destructively; re-running slices the already-sliced result.
//   @param actor_label - Editor label of the actor carrying the PMC
//   @param component_index - Optional 0-based PMC index on the actor; defaults to 0
//   @param plane_position - [x,y,z] world-space point on the slice plane
//   @param plane_normal - [x,y,z] slice plane normal (must be non-zero); geometry on the positive side is kept
//   @param create_other_half - Optional bool; if true, the sliced-off half is spawned as a new PMC on the same actor
//   @param cap_option - Optional "NoCap" | "CreateNewSectionForCap" | "UseLastSectionForCap"; defaults to "NoCap"
//   @param cap_material_path - Optional /Game path to a UMaterialInterface for the cap
//   @returns { actor_label, sliced, total_sections, other_half_created, other_half_component_name?, other_half_sections? }

TSharedPtr<FJsonObject> HandleSliceProceduralMesh(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ActorLabel;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_label"), ActorLabel)) { return Missing; }
	int32 ComponentIndex = 0;
	Params->TryGetNumberField(TEXT("component_index"), ComponentIndex);

	FVector PlanePosition = FVector::ZeroVector;
	FVector PlaneNormal = FVector::ZeroVector;
	const TArray<TSharedPtr<FJsonValue>>* PosArr = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* NormArr = nullptr;
	if (Params->TryGetArrayField(TEXT("plane_position"), PosArr) && PosArr && PosArr->Num() == 3)
	{
		PlanePosition.Set((*PosArr)[0]->AsNumber(), (*PosArr)[1]->AsNumber(), (*PosArr)[2]->AsNumber());
	}
	if (Params->TryGetArrayField(TEXT("plane_normal"), NormArr) && NormArr && NormArr->Num() == 3)
	{
		PlaneNormal.Set((*NormArr)[0]->AsNumber(), (*NormArr)[1]->AsNumber(), (*NormArr)[2]->AsNumber());
	}
	if (PlaneNormal.IsNearlyZero())
	{
		return mcp_common::MakeErrorReply(TEXT("plane_normal must be a non-zero [x,y,z] vector"));
	}

	EProcMeshSliceCapOption CapOption = EProcMeshSliceCapOption::NoCap;
	FString CapOptionStr;
	if (Params->TryGetStringField(TEXT("cap_option"), CapOptionStr))
	{
		if (CapOptionStr == TEXT("CreateNewSectionForCap")) { CapOption = EProcMeshSliceCapOption::CreateNewSectionForCap; }
		else if (CapOptionStr == TEXT("UseLastSectionForCap")) { CapOption = EProcMeshSliceCapOption::UseLastSectionForCap; }
		else if (CapOptionStr == TEXT("NoCap")) { CapOption = EProcMeshSliceCapOption::NoCap; }
		else
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("Invalid cap_option '%s' (expected NoCap|CreateNewSectionForCap|UseLastSectionForCap)"),
					*CapOptionStr));
		}
	}

	bool bCreateOtherHalf = false;
	Params->TryGetBoolField(TEXT("create_other_half"), bCreateOtherHalf);

	TSharedPtr<FJsonObject> Err;
	UProceduralMeshComponent* PMC = ProceduralMesh_FindComponent(ActorLabel, ComponentIndex, Err);
	if (!PMC) { return Err; }

	UMaterialInterface* CapMaterial = nullptr;
	FString CapMaterialPath;
	if (Params->TryGetStringField(TEXT("cap_material_path"), CapMaterialPath) && !CapMaterialPath.IsEmpty())
	{
		CapMaterial = LoadObject<UMaterialInterface>(nullptr, *CapMaterialPath);
		if (!CapMaterial)
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("Failed to load cap material: %s"), *CapMaterialPath));
		}
	}

	UProceduralMeshComponent* OtherHalf = nullptr;
	UKismetProceduralMeshLibrary::SliceProceduralMesh(PMC, PlanePosition, PlaneNormal,
		bCreateOtherHalf, OtherHalf, CapOption, CapMaterial);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_label"), ActorLabel);
	Data->SetBoolField(TEXT("sliced"), true);
	Data->SetNumberField(TEXT("total_sections"), PMC->GetNumSections());
	Data->SetBoolField(TEXT("other_half_created"), OtherHalf != nullptr);
	if (OtherHalf)
	{
		Data->SetStringField(TEXT("other_half_component_name"), OtherHalf->GetName());
		Data->SetNumberField(TEXT("other_half_sections"), OtherHalf->GetNumSections());
	}
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// Typed component removal + material assignment.
// ---------------------------------------------------------------------------

// MCP:remove_procedural_mesh_component - Destroy the UProceduralMeshComponent on a named actor in the editor world (inverse of create_procedural_mesh_component). It removes only the FIRST such component it finds, so an actor carrying several needs one call each; the response says whether one was found at all, and whether it was the actor's root.
//   @param actor_label - Editor label of the actor carrying the component
//   @returns { actor_label, removed, component_name?, was_root? }

TSharedPtr<FJsonObject> HandleRemoveProceduralMeshComponent(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorLabel;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_label"), ActorLabel)) { return Missing; }
	UWorld* World = nullptr;
	if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World)) { return NoWorld; }
	AActor* Actor = mcp_common::FindActorByName(World, ActorLabel);
	if (!Actor)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Actor not found: %s"), *ActorLabel));
	}
	UProceduralMeshComponent* PMC = Actor->FindComponentByClass<UProceduralMeshComponent>();
	if (!PMC)
	{
		TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetStringField(TEXT("actor_name"), Actor->GetName());
		Resp->SetBoolField(TEXT("removed"), false);
		Resp->SetStringField(TEXT("note"), TEXT("Actor has no UProceduralMeshComponent"));
		return mcp_common::MakeOkReply(Resp);
	}
	const bool bWasRoot = (Actor->GetRootComponent() == PMC);
	const FString ComponentName = PMC->GetName();
	PMC->DestroyComponent();
	TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
	Resp->SetStringField(TEXT("actor_name"), Actor->GetName());
	Resp->SetBoolField(TEXT("removed"), true);
	Resp->SetStringField(TEXT("component_name"), ComponentName);
	Resp->SetBoolField(TEXT("was_root"), bWasRoot);
	if (bWasRoot)
	{
		Resp->SetStringField(TEXT("note"),
			TEXT("Component was the actor's root; the actor is left without a transform until another root is assigned"));
	}
	return mcp_common::MakeOkReply(Resp);
}

// MCP:set_procedural_mesh_material - Assign a material to one slot of the first UProceduralMeshComponent on an editor-world actor (found by label or object name) with SetMaterial, marking the actor package dirty without saving. Returns the loaded material_path, element_index and previous_material_path when the slot had a material.
//   @param actor_label - Editor label or object name of the actor; the first actor matching either is used
//   @param material_path - Material or material instance asset path, e.g. "/Game/Materials/M_Rock"
//   @param element_index - Material slot index, usually the section index; default 0 (not range-checked)
//   @returns { actor_label, material_path, element_index, material_set, previous_material_path? }

TSharedPtr<FJsonObject> HandleSetProceduralMeshMaterial(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorLabel;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_label"), ActorLabel)) { return Missing; }
	FString MaterialPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_path"), MaterialPath)) { return Missing; }
	int32 ElementIndex = 0;
	Params->TryGetNumberField(TEXT("element_index"), ElementIndex);

	UWorld* EditorWorld = nullptr;
	if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(EditorWorld)) { return NoWorld; }

	AActor* Owner = mcp_common::FindActorByName(EditorWorld, ActorLabel);
	if (Owner == nullptr)
	{
		return mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor not found: %s"), *ActorLabel));
	}
	UProceduralMeshComponent* MeshComponent = Owner->FindComponentByClass<UProceduralMeshComponent>();
	if (MeshComponent == nullptr)
	{
		return mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor '%s' has no UProceduralMeshComponent"), *ActorLabel));
	}
	UMaterialInterface* NewMaterial = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
	if (NewMaterial == nullptr)
	{
		return mcp_common::MakeErrorReply(FString::Printf(TEXT("Could not load UMaterialInterface at %s"), *MaterialPath));
	}

	// Remember what the slot held before assigning (Modify first), then dirty the owning package.
	const UMaterialInterface* Previous = MeshComponent->GetMaterial(ElementIndex);
	const FString PreviousPath = Previous ? Previous->GetPathName() : FString();
	MeshComponent->Modify();
	MeshComponent->SetMaterial(ElementIndex, NewMaterial);
	Owner->MarkPackageDirty();

	TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
	Resp->SetStringField(TEXT("actor_label"), Owner->GetActorLabel());
	Resp->SetStringField(TEXT("material_path"), NewMaterial->GetPathName());
	Resp->SetNumberField(TEXT("element_index"), ElementIndex);
	Resp->SetBoolField(TEXT("material_set"), true);
	if (!PreviousPath.IsEmpty())
	{
		Resp->SetStringField(TEXT("previous_material_path"), PreviousPath);
	}
	return mcp_common::MakeOkReply(Resp);
}

const McpCommandFamilies::FRegistrar Registration =
{
	{ TEXT("clear_procedural_mesh"), &HandleClearProceduralMesh },
	{ TEXT("create_procedural_box_mesh"), &HandleCreateProceduralBoxMesh },
	{ TEXT("create_procedural_grid_mesh"), &HandleCreateProceduralGridMesh },
	{ TEXT("create_procedural_mesh_component"), &HandleCreateProceduralMeshComponent },
	{ TEXT("get_procedural_mesh_info"), &HandleGetProceduralMeshInfo },
	{ TEXT("get_procedural_mesh_section_data"), &HandleGetProceduralMeshSectionData },
	{ TEXT("get_procedural_mesh_section_visible"), &HandleGetProceduralMeshSectionVisible },
	{ TEXT("remove_procedural_mesh_component"), &HandleRemoveProceduralMeshComponent },
	{ TEXT("set_procedural_mesh_material"), &HandleSetProceduralMeshMaterial },
	{ TEXT("set_procedural_mesh_section_visible"), &HandleSetProceduralMeshSectionVisible },
	{ TEXT("slice_procedural_mesh"), &HandleSliceProceduralMesh },
	{ TEXT("update_procedural_mesh_section"), &HandleUpdateProceduralMeshSection },
};
} // namespace UEAgentProceduralMeshCommands
