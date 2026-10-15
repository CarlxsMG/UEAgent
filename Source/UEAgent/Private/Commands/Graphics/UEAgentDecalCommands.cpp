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
// MCP:spawn_decal_actor - Spawns a DecalActor into the editor world.  If material_path loads as a UMaterialInterface it is assigned to the UDecalComponent (SetDecalMaterial). Size may be a uniform number or an [x, y, z] half-extent applied to DecalSize (read back).
//   @param actor_label - Label for the spawned (or re-used) ADecalActor.
//   @param location - Optional [x, y, z] world location (default 0,0,0).
//   @param rotation - Optional [pitch, yaw, roll] world rotation.
//   @param decal_size - Optional [x, y, z] half-extent for DecalSize (default 128,256,256).
//   @param material_path - Optional content path of a UMaterialInterface to assign to the decal.
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
		Comp->DecalSize = DecalSize;
		FString MaterialPath;
		if (Params->TryGetStringField(TEXT("material_path"), MaterialPath) && !MaterialPath.IsEmpty())
		{
			if (UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, *MaterialPath))
			{
				Comp->SetDecalMaterial(Mat);
			}
			else
			{
				// Don't fail the spawn — just note the material wasn't applied.
				UE_LOG(LogTemp, Warning,
					TEXT("[Decal] spawn_decal_actor: material_path %s did not resolve; decal spawned without it"),
					*MaterialPath);
			}
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
// MCP:set_decal_material - Assign a UMaterialInterface to the UDecalComponent on a named DecalActor.
//   @param actor_label - Label of the ADecalActor in the editor world.
//   @param material_path - Content path of the UMaterialInterface.
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
	Decal->GetDecal()->SetDecalMaterial(Mat);
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_label"), ActorLabel);
	Data->SetStringField(TEXT("material_path"), MaterialPath);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_decal_size {actor_label, decal_size?=[x,y,z], size_x?, size_y?, size_z?}
//   Either a single decal_size array OR three named numbers are accepted.
// ---------------------------------------------------------------------------
// MCP:set_decal_size - Set the DecalSize half-extent on a UDecalComponent parented under a DecalActor.
//   @param actor_label - Label of the ADecalActor in the editor world.
//   @param decal_size - Optional [x, y, z] half-extent (overrides size_x/size_y/size_z).
//   @param size_x - Optional X half-extent.
//   @param size_y - Optional Y half-extent.
//   @param size_z - Optional Z half-extent.
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
	Decal->GetDecal()->DecalSize = Size;
	Decal->GetDecal()->MarkRenderStateDirty();
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
// MCP:set_decal_color - Set DecalColor on an editor-world ADecalActor found by label; it only shows if the decal material uses a Decal Color node. A color that is not a 4-number array is ignored (current color kept). No undo, dirty flag or save; returns the resulting r, g, b, a.
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
	FLinearColor Color = Decal->GetDecal()->DecalColor;
	ReadLinearColor(Params, TEXT("color"), Color);
	Decal->GetDecal()->SetDecalColor(Color);
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
// MCP:set_decal_sort_order - Set the SortOrder int on a UDecalComponent (lower draws first within the same decal bucket).
//   @param actor_label - Label of the ADecalActor in the editor world.
//   @param sort_order - New sort order value.
//   @returns { actor_label: string, sort_order: int }
TSharedPtr<FJsonObject> HandleSetDecalSortOrder(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ActorLabel;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_label"), ActorLabel)) { return Missing; }
	int32 SortOrder = 0;
	Params->TryGetNumberField(TEXT("sort_order"), SortOrder);
	ADecalActor* Decal = nullptr;
	if (TSharedPtr<FJsonObject> Error = Decal_FindExisting(ActorLabel, Decal)) { return Error; }
	Decal->GetDecal()->SetSortOrder(SortOrder);
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
//   fade_in_duration?, destroy_owner_after_fade?}
//   Partial / idempotent: only provided fields change. Direct field writes (not
//   SetFadeOut/SetFadeIn) so authored values round-trip exactly — SetFadeOut applies a
//   global CVarDecalFadeDurationScale and calls SetLifeSpan (a runtime side effect); the
//   details panel writes these EditAnywhere fields directly, which is what we do here.
// ---------------------------------------------------------------------------
// MCP:set_decal_fade - Set a UDecalComponent's fade-out/fade-in timing fields on a named DecalActor.  Writes the EditAnywhere fields directly (mirrors the details panel) so values round-trip exactly, avoiding SetFadeOut's global fade-scale CVar and SetLifeSpan side effect.  Only provided fields change (partial, idempotent).
//   @param actor_label - Label of the ADecalActor in the editor world.
//   @param fade_start_delay - Optional seconds before fade-out begins (FadeStartDelay; 0 + fade_duration 0 = persistent).
//   @param fade_duration - Optional fade-out duration in seconds (FadeDuration).
//   @param fade_in_start_delay - Optional seconds before fade-in begins (FadeInStartDelay).
//   @param fade_in_duration - Optional fade-in duration in seconds (FadeInDuration).
//   @param destroy_owner_after_fade - Optional: destroy the owning actor after fully fading out (bDestroyOwnerAfterFade).
//   @returns { actor_label: string, fade_start_delay: float, fade_duration: float, fade_in_start_delay: float, fade_in_duration: float, destroy_owner_after_fade: bool, applied_count: int }
TSharedPtr<FJsonObject> HandleSetDecalFade(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ActorLabel;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_label"), ActorLabel)) { return Missing; }
	ADecalActor* Decal = nullptr;
	if (TSharedPtr<FJsonObject> Error = Decal_FindExisting(ActorLabel, Decal)) { return Error; }
	UDecalComponent* Comp = Decal->GetDecal();

	int32 Applied = 0;
	double Number = 0.0;
	if (Params->TryGetNumberField(TEXT("fade_start_delay"), Number))    { Comp->FadeStartDelay = static_cast<float>(Number); ++Applied; }
	if (Params->TryGetNumberField(TEXT("fade_duration"), Number))       { Comp->FadeDuration = static_cast<float>(Number); ++Applied; }
	if (Params->TryGetNumberField(TEXT("fade_in_start_delay"), Number)) { Comp->FadeInStartDelay = static_cast<float>(Number); ++Applied; }
	if (Params->TryGetNumberField(TEXT("fade_in_duration"), Number))    { Comp->FadeInDuration = static_cast<float>(Number); ++Applied; }
	bool bDestroy = false;
	if (Params->TryGetBoolField(TEXT("destroy_owner_after_fade"), bDestroy)) { Comp->bDestroyOwnerAfterFade = bDestroy ? 1 : 0; ++Applied; }

	Comp->MarkRenderStateDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_label"), ActorLabel);
	Data->SetNumberField(TEXT("fade_start_delay"), Comp->FadeStartDelay);
	Data->SetNumberField(TEXT("fade_duration"), Comp->FadeDuration);
	Data->SetNumberField(TEXT("fade_in_start_delay"), Comp->FadeInStartDelay);
	Data->SetNumberField(TEXT("fade_in_duration"), Comp->FadeInDuration);
	Data->SetBoolField(TEXT("destroy_owner_after_fade"), Comp->bDestroyOwnerAfterFade != 0);
	Data->SetNumberField(TEXT("applied_count"), Applied);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_decal_fade_screen_size {actor_label, fade_screen_size}
// ---------------------------------------------------------------------------
// MCP:set_decal_fade_screen_size - Set the FadeScreenSize on a UDecalComponent (the screen-size fraction below which the decal fades out).  The writer for the value get_decal_info already reads.
//   @param actor_label - Label of the ADecalActor in the editor world.
//   @param fade_screen_size - New FadeScreenSize (0..1; smaller fades the decal only when it gets very small on screen).
//   @returns { actor_label: string, fade_screen_size: float }
TSharedPtr<FJsonObject> HandleSetDecalFadeScreenSize(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ActorLabel;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_label"), ActorLabel)) { return Missing; }
	double Value = 0.0;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("fade_screen_size"), Value)) { return Missing; }
	ADecalActor* Decal = nullptr;
	if (TSharedPtr<FJsonObject> Error = Decal_FindExisting(ActorLabel, Decal)) { return Error; }
	Decal->GetDecal()->SetFadeScreenSize(static_cast<float>(Value));

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_label"), ActorLabel);
	Data->SetNumberField(TEXT("fade_screen_size"), Decal->GetDecal()->FadeScreenSize);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// Fade-in + destroy-owner flags
// ---------------------------------------------------------------------------

// MCP:set_decal_fade_in - Set the FadeInStartDelay and FadeInDuration on a UDecalComponent via UDecalComponent::SetFadeIn (the existing SetFadeOut cmd only sets the *out* fields; this cmd sets the *in* pair).
//   @param actor_label - Label of the ADecalActor in the editor world.
//   @param start_delay - Time in seconds before the decal begins fading in (default 0).
//   @param duration - Time in seconds for the fade-in to complete (default 0 = no fade-in).
//   @returns { actor_label: string, fade_in_start_delay: float, fade_in_duration: float }
TSharedPtr<FJsonObject> HandleSetDecalFadeIn(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ActorLabel;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_label"), ActorLabel)) { return Missing; }

	double StartDelay = 0.0;
	Params->TryGetNumberField(TEXT("start_delay"), StartDelay);
	double Duration = 0.0;
	Params->TryGetNumberField(TEXT("duration"), Duration);

	ADecalActor* Decal = nullptr;
	if (TSharedPtr<FJsonObject> Error = Decal_FindExisting(ActorLabel, Decal)) { return Error; }

	UDecalComponent* Comp = Decal->GetDecal();
	Comp->SetFadeIn(static_cast<float>(StartDelay), static_cast<float>(Duration));

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_label"), ActorLabel);
	Data->SetNumberField(TEXT("fade_in_start_delay"), Comp->FadeInStartDelay);
	Data->SetNumberField(TEXT("fade_in_duration"), Comp->FadeInDuration);
	return mcp_common::MakeOkReply(Data);
}

// MCP:set_decal_destroy_owner_after_fade - Toggle the bDestroyOwnerAfterFade bit on a UDecalComponent. The existing set_decal_fade cmd always passes DestroyOwnerAfterFade=true to SetFadeOut; this cmd lets callers flip it independently and read it back. Implementation: writes the FadeOut triad through SetFadeOut(start, duration, destroy) using the current FadeStartDelay/FadeDuration so the other two fields are preserved.
//   @param actor_label - Label of the ADecalActor in the editor world.
//   @param destroy_owner_after_fade - New value for the bDestroyOwnerAfterFade bit (true = auto-destroy the actor once fully faded).
//   @returns { actor_label: string, destroy_owner_after_fade: bool, fade_start_delay: float, fade_duration: float }
TSharedPtr<FJsonObject> HandleSetDecalDestroyOwnerAfterFade(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ActorLabel;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_label"), ActorLabel)) { return Missing; }

	bool bDestroy = true;
	if (Params->HasField(TEXT("destroy_owner_after_fade")))
	{
		// TryGetBoolField returns false if missing; check HasField first.
		Params->TryGetBoolField(TEXT("destroy_owner_after_fade"), bDestroy);
	}

	ADecalActor* Decal = nullptr;
	if (TSharedPtr<FJsonObject> Error = Decal_FindExisting(ActorLabel, Decal)) { return Error; }

	UDecalComponent* Comp = Decal->GetDecal();
	// Round-trip through SetFadeOut so the engine UPROPERTY bits stay consistent
	// (DecalComponent has no standalone bDestroyOwnerAfterFade setter).
	Comp->SetFadeOut(Comp->FadeStartDelay, Comp->FadeDuration, bDestroy);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_label"), ActorLabel);
	Data->SetBoolField(TEXT("destroy_owner_after_fade"), Comp->bDestroyOwnerAfterFade != 0);
	Data->SetNumberField(TEXT("fade_start_delay"), Comp->FadeStartDelay);
	Data->SetNumberField(TEXT("fade_duration"), Comp->FadeDuration);
	return mcp_common::MakeOkReply(Data);
}

const McpCommandFamilies::FRegistrar Registration =
{
	{ TEXT("get_decal_info"), &HandleGetDecalInfo },
	{ TEXT("set_decal_color"), &HandleSetDecalColor },
	{ TEXT("set_decal_destroy_owner_after_fade"), &HandleSetDecalDestroyOwnerAfterFade },
	{ TEXT("set_decal_fade"), &HandleSetDecalFade },
	{ TEXT("set_decal_fade_in"), &HandleSetDecalFadeIn },
	{ TEXT("set_decal_fade_screen_size"), &HandleSetDecalFadeScreenSize },
	{ TEXT("set_decal_material"), &HandleSetDecalMaterial },
	{ TEXT("set_decal_size"), &HandleSetDecalSize },
	{ TEXT("set_decal_sort_order"), &HandleSetDecalSortOrder },
	{ TEXT("spawn_decal_actor"), &HandleSpawnDecalActor },
};
} // namespace UEAgentDecalCommands
