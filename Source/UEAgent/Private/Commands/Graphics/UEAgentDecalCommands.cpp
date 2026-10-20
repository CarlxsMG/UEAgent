#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

#include "Components/DecalComponent.h"
#include "Editor.h"
#include "Engine/DecalActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Materials/MaterialInterface.h"

/**
 * Decal commands.
 *
 * They wrap the engine's ADecalActor + UDecalComponent so decals can be authored in the editor: spawn a decal
 * actor, assign its material, set its projection box size, tint color, sort order and fading, and read it
 * back.
 *
 * All commands are headless-safe actor / component manipulation — no PIE, no render. The decal's render state
 * is built when the component registers with the world, which happens automatically when the actor is spawned
 * into the editor world.
 */
namespace UEAgentDecalCommands
{

// MCP-DOMAIN: Graphics

namespace
{
	ADecalActor* FindDecalByLabel(UWorld* World, const FString& Label)
	{
		for (TActorIterator<ADecalActor> It(World); It; ++It)
		{
			if (It->GetActorLabel() == Label) { return *It; }
		}
		return nullptr;
	}

	/**
	 * The decal actor labelled ActorLabel in the editor world. Null on success, else the error reply: no editor world,
	 * or no such decal actor with a decal component.
	 */
	TSharedPtr<FJsonObject> Decal_FindExisting(const FString& ActorLabel, ADecalActor*& OutDecal)
	{
		UWorld* World = nullptr;
		if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World))
		{
			return NoWorld;
		}
		OutDecal = FindDecalByLabel(World, ActorLabel);
		if (!OutDecal || !OutDecal->GetDecal())
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("Decal actor or component not found: %s"), *ActorLabel));
		}
		return nullptr;
	}

	bool ReadVector(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, FVector& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Params->TryGetArrayField(Field, Arr) || !Arr || Arr->Num() != 3) { return false; }
		Out.X = (*Arr)[0]->AsNumber();
		Out.Y = (*Arr)[1]->AsNumber();
		Out.Z = (*Arr)[2]->AsNumber();
		return true;
	}

	bool ReadLinearColor(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, FLinearColor& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Params->TryGetArrayField(Field, Arr) || !Arr || Arr->Num() != 4) { return false; }
		Out = mcp_common::LinearColorFromJsonArray(Arr, Out);
		return true;
	}
}

// ---------------------------------------------------------------------------
// spawn_decal_actor {actor_label, location?=[x,y,z], rotation?=[pitch,yaw,roll],
//   decal_size?=[x,y,z], material_path?}
//   Idempotent: if a decal actor with this label already exists, returns it
//   (after optionally re-applying decal_size/material).
// ---------------------------------------------------------------------------
// MCP:spawn_decal_actor - Spawn a DecalActor labelled actor_label in the editor world with its DecalSize half-extent and optional material in one call; spawn_actor has no decal type. Idempotent by label: an existing one is REUSED, moved to location/rotation (origin/zero when omitted) and DecalSize reset (default 128,256,256). decal_size must be [x, y, z]; a material_path that does not load is an error and nothing is spawned or moved. No undo; not saved (save_level).
//   @param actor_label - Label for the spawned (or re-used) ADecalActor.
//   @param location - Optional [x, y, z] world location (default 0,0,0).
//   @param rotation - Optional [pitch, yaw, roll] world rotation.
//   @param decal_size - Optional [x, y, z] half-extent for DecalSize (default 128,256,256).
//   @param material_path - Optional content path of a UMaterialInterface to assign to the decal; a path that does not load is an error and nothing is spawned or changed.
//   @returns { actor_label: string, actor_name: string, reused: bool, decal_size_x: float, decal_size_y: float, decal_size_z: float, decal_material: string }
TSharedPtr<FJsonObject> HandleSpawnDecalActor(
    const TSharedPtr<FJsonObject>& Params)
{
	FString ActorLabel;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_label"), ActorLabel)) { return Missing; }

	UWorld* World = nullptr;
	if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World)) { return NoWorld; }

	FVector Location = FVector::ZeroVector;
	ReadVector(Params, TEXT("location"), Location);
	FRotator Rotation = FRotator::ZeroRotator;
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Params->TryGetArrayField(TEXT("rotation"), Arr) && Arr && Arr->Num() == 3)
		{
			Rotation.Pitch = (*Arr)[0]->AsNumber();
			Rotation.Yaw   = (*Arr)[1]->AsNumber();
			Rotation.Roll  = (*Arr)[2]->AsNumber();
		}
	}
	FVector DecalSize(128.f, 256.f, 256.f);
	ReadVector(Params, TEXT("decal_size"), DecalSize);

	// The material is loaded before anything is spawned or moved, so a path that does not load refuses the call
	// instead of leaving a decal without the material it was asked for.
	UMaterialInterface* Material = nullptr;
	FString MaterialPath;
	if (Params->TryGetStringField(TEXT("material_path"), MaterialPath) && !MaterialPath.IsEmpty())
	{
		Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
		if (!Material)
		{
			return mcp_common::MakeErrorReply(FString::Printf(
				TEXT("material_path '%s' does not load as a UMaterialInterface; nothing was spawned or changed"), *MaterialPath));
		}
	}

	bool bReused = false;
	ADecalActor* Decal = FindDecalByLabel(World, ActorLabel);
	if (Decal)
	{
		bReused = true;
		// Modify() before mutating an existing actor's transform: neither SetActorLocation
		// nor SetActorRotation marks the actor's (OFPA) package dirty on their own, so
		// without this the reposition looks live-correct but is silently dropped by the
		// next save_level.
		Decal->Modify();
		Decal->SetActorLocation(Location);
		Decal->SetActorRotation(Rotation);
	}
	else
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Decal = World->SpawnActor<ADecalActor>(ADecalActor::StaticClass(), Location, Rotation, SpawnParams);
		if (!Decal)
		{
			return mcp_common::MakeErrorReply(TEXT("Failed to spawn ADecalActor"));
		}
		Decal->SetActorLabel(ActorLabel);
	}

	if (UDecalComponent* Comp = Decal->GetDecal())
	{
		// A reused actor's component is a separate object from the actor recorded above, so it is recorded too before
		// its size and material are written; a new actor's spawn already covers it.
		if (bReused)
		{
			Comp->Modify();
		}
		Comp->DecalSize = DecalSize;
		if (Material)
		{
			Comp->SetDecalMaterial(Material);
		}
		Comp->MarkRenderStateDirty();
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_label"), Decal->GetActorLabel());
	Data->SetStringField(TEXT("actor_name"), Decal->GetName());
	Data->SetBoolField(TEXT("reused"), bReused);
	if (UDecalComponent* Comp = Decal->GetDecal())
	{
		Data->SetNumberField(TEXT("decal_size_x"), Comp->DecalSize.X);
		Data->SetNumberField(TEXT("decal_size_y"), Comp->DecalSize.Y);
		Data->SetNumberField(TEXT("decal_size_z"), Comp->DecalSize.Z);
		Data->SetStringField(TEXT("decal_material"),
			Comp->GetDecalMaterial() ? Comp->GetDecalMaterial()->GetPathName() : TEXT(""));
	}
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_decal_material {actor_label, material_path}
// ---------------------------------------------------------------------------
// MCP:set_decal_material - Assign a material to the UDecalComponent of an ADecalActor in the editor world, found by actor label (not object name), in place of its current one. material_path is loaded as a UMaterialInterface (it renders only with the Deferred Decal material domain) and echoed back as given, not read back. The change is one undo step and dirties the level; nothing is saved. Assigning the material it already has changes nothing and adds no undo step. An unknown label, an actor without a decal component, or a path that does not load as a material is an error. Read back with get_decal_info; spawn_decal_actor can set one at creation; tint: set_decal_color; projection box: set_decal_size.
//   @param actor_label - Actor label of the ADecalActor in the editor world (case-insensitive; the object name does not match).
//   @param material_path - Asset path of the UMaterialInterface, e.g. "/Game/Decals/M_Decal_Crack" (the object path form also works); echoed back as given.
//   @returns { actor_label: string, material_path: string }
TSharedPtr<FJsonObject> HandleSetDecalMaterial(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ActorLabel, MaterialPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_label"), ActorLabel)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_path"), MaterialPath)) { return Missing; }
	ADecalActor* Decal = nullptr;
	if (TSharedPtr<FJsonObject> Error = Decal_FindExisting(ActorLabel, Decal)) { return Error; }
	UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
	if (!Mat)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Material not found: %s"), *MaterialPath));
	}
	// The component is recorded before the write, so the call is one undo step and dirties the actor's package; the
	// material it already has is not written again, so that call dirties nothing.
	UDecalComponent* Comp = Decal->GetDecal();
	if (Comp->GetDecalMaterial() != Mat)
	{
		Comp->Modify();
		Comp->SetDecalMaterial(Mat);
	}
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_label"), ActorLabel);
	Data->SetStringField(TEXT("material_path"), MaterialPath);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_decal_size {actor_label, decal_size?=[x,y,z], size_x?, size_y?, size_z?}
//   Either a single decal_size array OR three named numbers are accepted.
// ---------------------------------------------------------------------------
// MCP:set_decal_size - Set the DecalSize half-extents (cm, in the decal's local space, before actor scale) of an ADecalActor's UDecalComponent in the editor world, found by actor label, and refresh its render state. A valid decal_size [x, y, z] wins over size_x, size_y and size_z; without it each given axis changes and the rest keep their value, and a call with none changes nothing. X is the projection depth, Y and Z the footprint. Values are not validated; a change is one undo step that dirties the level, and nothing is saved. An unknown label or an actor without a decal component is an error. Read back with get_decal_info; set it at spawn with spawn_decal_actor; draw order: set_decal_sort_order.
//   @param actor_label - Actor label of the ADecalActor in the editor world (case-insensitive; the object name does not match).
//   @param decal_size - Optional array of exactly 3 numbers [x, y, z], the half-extents in cm; when valid it overrides size_x, size_y and size_z, and any other length is ignored.
//   @param size_x - Optional X half-extent in cm (the projection depth); used only without a valid decal_size.
//   @param size_y - Optional Y half-extent in cm; used only without a valid decal_size.
//   @param size_z - Optional Z half-extent in cm; used only without a valid decal_size.
//   @returns { actor_label: string, size_x: float, size_y: float, size_z: float }
TSharedPtr<FJsonObject> HandleSetDecalSize(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ActorLabel;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_label"), ActorLabel)) { return Missing; }
	ADecalActor* Decal = nullptr;
	if (TSharedPtr<FJsonObject> Error = Decal_FindExisting(ActorLabel, Decal)) { return Error; }
	FVector Size = Decal->GetDecal()->DecalSize;
	if (!ReadVector(Params, TEXT("decal_size"), Size))
	{
		double X = 0, Y = 0, Z = 0;
		if (Params->TryGetNumberField(TEXT("size_x"), X)) { Size.X = static_cast<float>(X); }
		if (Params->TryGetNumberField(TEXT("size_y"), Y)) { Size.Y = static_cast<float>(Y); }
		if (Params->TryGetNumberField(TEXT("size_z"), Z)) { Size.Z = static_cast<float>(Z); }
	}
	// The component is recorded before the write, so a change is one undo step and dirties the actor's package; the
	// size it already has is not written again, so that call dirties nothing.
	UDecalComponent* Comp = Decal->GetDecal();
	if (Comp->DecalSize != Size)
	{
		Comp->Modify();
		Comp->DecalSize = Size;
		Comp->MarkRenderStateDirty();
	}
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_label"), ActorLabel);
	Data->SetNumberField(TEXT("size_x"), Size.X);
	Data->SetNumberField(TEXT("size_y"), Size.Y);
	Data->SetNumberField(TEXT("size_z"), Size.Z);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_decal_color {actor_label, color?=[r,g,b,a]}
// ---------------------------------------------------------------------------
// MCP:set_decal_color - Set DecalColor on an editor-world ADecalActor found by label; it only shows if the decal material uses a Decal Color node. A color that is not a 4-number array is ignored (current color kept). A change is one undo step that dirties the level; nothing is saved. Returns the resulting r, g, b, a. An unknown label is an error; read it back with get_decal_info.
//   @param actor_label - Label of the ADecalActor in the editor world.
//   @param color [array] - Array of exactly 4 numbers [r, g, b, a], a linear color such as [1, 0.5, 0, 1]; missing or another length keeps the current DecalColor.
//   @returns { actor_label: string, r: float, g: float, b: float, a: float }
TSharedPtr<FJsonObject> HandleSetDecalColor(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ActorLabel;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_label"), ActorLabel)) { return Missing; }
	ADecalActor* Decal = nullptr;
	if (TSharedPtr<FJsonObject> Error = Decal_FindExisting(ActorLabel, Decal)) { return Error; }
	UDecalComponent* Comp = Decal->GetDecal();
	FLinearColor Color = Comp->DecalColor;
	ReadLinearColor(Params, TEXT("color"), Color);
	// The component is recorded before the write, so a change is one undo step and dirties the actor's package; the
	// color it already has is not written again, so that call dirties nothing.
	if (Comp->DecalColor != Color)
	{
		Comp->Modify();
		Comp->SetDecalColor(Color);
	}
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_label"), ActorLabel);
	Data->SetNumberField(TEXT("r"), Color.R);
	Data->SetNumberField(TEXT("g"), Color.G);
	Data->SetNumberField(TEXT("b"), Color.B);
	Data->SetNumberField(TEXT("a"), Color.A);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_decal_sort_order {actor_label, sort_order}
// ---------------------------------------------------------------------------
// MCP:set_decal_sort_order - Set the SortOrder of the UDecalComponent of an ADecalActor in the editor world, found by actor label: where decals overlap, a higher sort order draws later, on top of a lower one. sort_order is required; a missing one is an error and nothing is written. A change is one undo step that dirties the level; the order the decal already has changes nothing, and nothing is saved. An unknown label or an actor without a decal component is an error. Read it back with get_decal_info; set the material with set_decal_material and the projection box with set_decal_size.
//   @param actor_label - Label of the ADecalActor in the editor world.
//   @param sort_order - New sort order value, a whole number (a fraction is rounded); required.
//   @returns { actor_label: string, sort_order: int }
TSharedPtr<FJsonObject> HandleSetDecalSortOrder(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ActorLabel;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_label"), ActorLabel)) { return Missing; }
	// The order is the whole point of the call: without it nothing is written, rather than an order of 0. A negated read
	// into the int32, not ReadRequiredNumber, so the catalog also publishes the int32 range the precondition enforces.
	int32 SortOrder = 0;
	if (!Params->TryGetNumberField(TEXT("sort_order"), SortOrder))
	{
		return mcp_common::MissingParameterReply(TEXT("sort_order"));
	}
	ADecalActor* Decal = nullptr;
	if (TSharedPtr<FJsonObject> Error = Decal_FindExisting(ActorLabel, Decal)) { return Error; }
	// The component is recorded before the write, so a change is one undo step and dirties the actor's package; the
	// order it already has is not written again, so that call dirties nothing.
	UDecalComponent* Comp = Decal->GetDecal();
	if (Comp->SortOrder != SortOrder)
	{
		Comp->Modify();
		Comp->SetSortOrder(SortOrder);
	}
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_label"), ActorLabel);
	Data->SetNumberField(TEXT("sort_order"), SortOrder);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_decal_info {actor_label}
// ---------------------------------------------------------------------------
// MCP:get_decal_info - Read the full UDecalComponent state (size, sort order, fade screen size, fade-out/in timing, color, material) of a named DecalActor.
//   @param actor_label - Label of the ADecalActor in the editor world.
//   @returns { actor_label: string, actor_name: string, decal_size_x: float, decal_size_y: float, decal_size_z: float, sort_order: int, fade_screen_size: float, fade_start_delay: float, fade_duration: float, fade_in_start_delay: float, fade_in_duration: float, destroy_owner_after_fade: bool, r: float, g: float, b: float, a: float, decal_material: string }
TSharedPtr<FJsonObject> HandleGetDecalInfo(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ActorLabel;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_label"), ActorLabel)) { return Missing; }
	ADecalActor* Decal = nullptr;
	if (TSharedPtr<FJsonObject> Error = Decal_FindExisting(ActorLabel, Decal)) { return Error; }
	UDecalComponent* Comp = Decal->GetDecal();
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_label"), Decal->GetActorLabel());
	Data->SetStringField(TEXT("actor_name"), Decal->GetName());
	Data->SetNumberField(TEXT("decal_size_x"), Comp->DecalSize.X);
	Data->SetNumberField(TEXT("decal_size_y"), Comp->DecalSize.Y);
	Data->SetNumberField(TEXT("decal_size_z"), Comp->DecalSize.Z);
	Data->SetNumberField(TEXT("sort_order"), Comp->SortOrder);
	Data->SetNumberField(TEXT("fade_screen_size"), Comp->FadeScreenSize);
	Data->SetNumberField(TEXT("fade_start_delay"), Comp->FadeStartDelay);
	Data->SetNumberField(TEXT("fade_duration"), Comp->FadeDuration);
	Data->SetNumberField(TEXT("fade_in_start_delay"), Comp->FadeInStartDelay);
	Data->SetNumberField(TEXT("fade_in_duration"), Comp->FadeInDuration);
	Data->SetBoolField(TEXT("destroy_owner_after_fade"), Comp->bDestroyOwnerAfterFade != 0);
	Data->SetNumberField(TEXT("r"), Comp->DecalColor.R);
	Data->SetNumberField(TEXT("g"), Comp->DecalColor.G);
	Data->SetNumberField(TEXT("b"), Comp->DecalColor.B);
	Data->SetNumberField(TEXT("a"), Comp->DecalColor.A);
	Data->SetStringField(TEXT("decal_material"),
		Comp->GetDecalMaterial() ? Comp->GetDecalMaterial()->GetPathName() : TEXT(""));
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_decal_fade {actor_label, fade_start_delay?, fade_duration?, fade_in_start_delay?,
//   fade_in_duration?, destroy_owner_after_fade?, fade_screen_size?}
//   Partial / idempotent: only provided fields change. Direct field writes (not
//   SetFadeOut/SetFadeIn) so authored values round-trip exactly — SetFadeOut applies a
//   global CVarDecalFadeDurationScale and calls SetLifeSpan (a runtime side effect); the
//   details panel writes these EditAnywhere fields directly, which is what we do here.
// ---------------------------------------------------------------------------
// MCP:set_decal_fade - Set a UDecalComponent's fade fields on a named DecalActor: fade-out timing, fade-in timing, the destroy-owner-after-fade bit and the fade screen size (the screen-size fraction below which the decal fades out).  Writes the EditAnywhere fields directly (mirrors the details panel) so values round-trip exactly, avoiding SetFadeOut's global fade-scale CVar and SetLifeSpan side effect, so no lifespan timer starts in the editor.  Only provided fields change (partial, idempotent); a change is one undo step that dirties the level, nothing is saved. An unknown label, or a fade_screen_size that is not a number of at least 0, is an error before anything is written. Read the values back with get_decal_info.
//   @param actor_label - Label of the ADecalActor in the editor world.
//   @param fade_start_delay - Optional seconds before fade-out begins (FadeStartDelay; 0 + fade_duration 0 = persistent).
//   @param fade_duration - Optional fade-out duration in seconds (FadeDuration).
//   @param fade_in_start_delay - Optional seconds before fade-in begins (FadeInStartDelay).
//   @param fade_in_duration - Optional fade-in duration in seconds (FadeInDuration).
//   @param destroy_owner_after_fade - Optional: destroy the owning actor after fully fading out (bDestroyOwnerAfterFade).
//   @param fade_screen_size - Optional FadeScreenSize, a number >= 0 (typically 0..1; smaller fades the decal only when it gets very small on screen).
//   @returns { actor_label: string, fade_start_delay: float, fade_duration: float, fade_in_start_delay: float, fade_in_duration: float, destroy_owner_after_fade: bool, fade_screen_size: float, applied_count: int }
TSharedPtr<FJsonObject> HandleSetDecalFade(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ActorLabel;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_label"), ActorLabel)) { return Missing; }

	// fade_screen_size is checked before the actor is touched: a wrong one writes nothing.
	double FadeScreenSize = 0.0;
	const bool bFadeScreenSizeIsNumber = Params->TryGetNumberField(TEXT("fade_screen_size"), FadeScreenSize);
	const bool bHasFadeScreenSize = Params->HasField(TEXT("fade_screen_size"));
	if (bHasFadeScreenSize && (!bFadeScreenSizeIsNumber || !FMath::IsFinite(FadeScreenSize) || FadeScreenSize < 0.0))
	{
		return mcp_common::MakeErrorReply(TEXT("fade_screen_size must be a number >= 0 (typically 0..1); nothing was changed"));
	}

	ADecalActor* Decal = nullptr;
	if (TSharedPtr<FJsonObject> Error = Decal_FindExisting(ActorLabel, Decal)) { return Error; }
	UDecalComponent* Comp = Decal->GetDecal();

	// The component is recorded once, before its first write that changes a value, so a change is one undo step and
	// dirties the actor's package; fields given their current value are not written and dirty nothing.
	bool bRecorded = false;
	auto WriteField = [Comp, &bRecorded](float& Member, double Value)
	{
		const float NewValue = static_cast<float>(Value);
		if (Member != NewValue)
		{
			if (!bRecorded)
			{
				Comp->Modify();
				bRecorded = true;
			}
			Member = NewValue;
		}
	};

	int32 Applied = 0;
	double Number = 0.0;
	if (Params->TryGetNumberField(TEXT("fade_start_delay"), Number))    { WriteField(Comp->FadeStartDelay, Number); ++Applied; }
	if (Params->TryGetNumberField(TEXT("fade_duration"), Number))       { WriteField(Comp->FadeDuration, Number); ++Applied; }
	if (Params->TryGetNumberField(TEXT("fade_in_start_delay"), Number)) { WriteField(Comp->FadeInStartDelay, Number); ++Applied; }
	if (Params->TryGetNumberField(TEXT("fade_in_duration"), Number))    { WriteField(Comp->FadeInDuration, Number); ++Applied; }
	bool bDestroy = false;
	if (Params->TryGetBoolField(TEXT("destroy_owner_after_fade"), bDestroy))
	{
		if ((Comp->bDestroyOwnerAfterFade != 0) != bDestroy)
		{
			if (!bRecorded)
			{
				Comp->Modify();
				bRecorded = true;
			}
			Comp->bDestroyOwnerAfterFade = bDestroy ? 1 : 0;
		}
		++Applied;
	}
	if (bHasFadeScreenSize) { WriteField(Comp->FadeScreenSize, FadeScreenSize); ++Applied; }

	if (bRecorded)
	{
		Comp->MarkRenderStateDirty();
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_label"), ActorLabel);
	Data->SetNumberField(TEXT("fade_start_delay"), Comp->FadeStartDelay);
	Data->SetNumberField(TEXT("fade_duration"), Comp->FadeDuration);
	Data->SetNumberField(TEXT("fade_in_start_delay"), Comp->FadeInStartDelay);
	Data->SetNumberField(TEXT("fade_in_duration"), Comp->FadeInDuration);
	Data->SetBoolField(TEXT("destroy_owner_after_fade"), Comp->bDestroyOwnerAfterFade != 0);
	Data->SetNumberField(TEXT("fade_screen_size"), Comp->FadeScreenSize);
	Data->SetNumberField(TEXT("applied_count"), Applied);
	return mcp_common::MakeOkReply(Data);
}

const McpCommandFamilies::FRegistrar Registration =
{
	{ TEXT("get_decal_info"), &HandleGetDecalInfo },
	{ TEXT("set_decal_color"), &HandleSetDecalColor },
	{ TEXT("set_decal_fade"), &HandleSetDecalFade },
	{ TEXT("set_decal_material"), &HandleSetDecalMaterial },
	{ TEXT("set_decal_size"), &HandleSetDecalSize },
	{ TEXT("set_decal_sort_order"), &HandleSetDecalSortOrder },
	{ TEXT("spawn_decal_actor"), &HandleSpawnDecalActor },
};
} // namespace UEAgentDecalCommands
