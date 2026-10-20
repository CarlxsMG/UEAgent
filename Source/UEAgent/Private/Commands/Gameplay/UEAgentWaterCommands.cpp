#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Materials/MaterialInterface.h"
#include "UObject/UObjectGlobals.h"
#include "WaterBodyActor.h"
#include "WaterBodyComponent.h"
#include "WaterBodyRiverComponent.h"
#include "WaterBodyOceanComponent.h"
#include "WaterSplineComponent.h"
#include "WaterSplineMetadata.h"
#include "Components/SplineComponent.h"
#include "WaterBodyLakeActor.h"
#include "WaterBodyOceanActor.h"
#include "WaterBodyRiverActor.h"
#include "WaterBodyTypes.h"
#include "WaterZoneActor.h"
// Gerstner waves + exclusion volumes.
#include "GerstnerWaterWaves.h"
#include "WaterWaves.h"
#include "WaterBodyExclusionVolume.h"
#include "GameFramework/Volume.h"
#include "ActorFactories/ActorFactory.h"
#include "Builders/CubeBuilder.h"
// MCP:configure_water_zone reads AWaterZone::RenderTargetResolution by reflection --
// its getter (AWaterZone::GetRenderTargetResolution) is NOT exported with UE_API, so
// calling it from this module would not link. FStructProperty lives here.
#include "UObject/UnrealType.h"

/**
 * Water System commands.
 *
 * Wraps the engine's Water plugin (AWaterZone and the AWaterBodyLake/River/Ocean actors) so an agent can
 * author a water scene: create a zone, spawn water bodies and exclusion volumes, link bodies to the zone,
 * shape rivers and waves, and read it all back. Everything is editor-world authoring: no PIE, no rendering.
 */
namespace UEAgentWaterCommands
{

// MCP-DOMAIN: Water

namespace
{
	/** Extract a location array (length 3) or default to zero. */
	FVector ReadLocationOrZero(const TSharedPtr<FJsonObject>& Params)
	{
		FVector Loc = FVector::ZeroVector;
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Params->TryGetArrayField(TEXT("location"), Arr) && Arr && Arr->Num() == 3)
		{
			Loc.X = (*Arr)[0]->AsNumber();
			Loc.Y = (*Arr)[1]->AsNumber();
			Loc.Z = (*Arr)[2]->AsNumber();
		}
		return Loc;
	}

	FString WaterBodyTypeToString(const EWaterBodyType Type)
	{
		switch (Type)
		{
			case EWaterBodyType::River:      return TEXT("River");
			case EWaterBodyType::Lake:       return TEXT("Lake");
			case EWaterBodyType::Ocean:      return TEXT("Ocean");
			case EWaterBodyType::Transition: return TEXT("Custom");
			default:                         return TEXT("Unknown");
		}
	}

	/** The editor world's AWaterBody labelled BodyLabel; on failure returns nullptr and fills OutError. */
	AWaterBody* Water_FindBody(const FString& BodyLabel, TSharedPtr<FJsonObject>& OutError)
	{
		UWorld* World = mcp_common::GetEditorWorld();
		if (!World)
		{
			OutError = mcp_common::MakeErrorReply(TEXT("No editor world available"));
			return nullptr;
		}
		AWaterBody* Body = Cast<AWaterBody>(mcp_common::FindActorByLabel(World, BodyLabel));
		if (!Body)
		{
			OutError = mcp_common::MakeErrorReply(FString::Printf(TEXT("Water body actor not found: %s"), *BodyLabel));
		}
		return Body;
	}

	/** Water_FindBody for the required water_body_label param, read into OutBodyLabel. */
	AWaterBody* Water_FindBodyParam(const TSharedPtr<FJsonObject>& Params, FString& OutBodyLabel,
		TSharedPtr<FJsonObject>& OutError)
	{
		if (!Params->TryGetStringField(TEXT("water_body_label"), OutBodyLabel) || OutBodyLabel.IsEmpty())
		{
			OutError = mcp_common::MakeErrorReply(TEXT("water_body_label is required"));
			return nullptr;
		}
		return Water_FindBody(OutBodyLabel, OutError);
	}

	/**
	 * The editor world's AWaterZone labelled ZoneLabel (OutWorld, when given, receives that world); on failure returns
	 * nullptr and fills OutError.
	 */
	AWaterZone* Water_FindZone(const FString& ZoneLabel, TSharedPtr<FJsonObject>& OutError, UWorld** OutWorld = nullptr)
	{
		UWorld* World = mcp_common::GetEditorWorld();
		if (!World)
		{
			OutError = mcp_common::MakeErrorReply(TEXT("No editor world available"));
			return nullptr;
		}
		if (OutWorld)
		{
			*OutWorld = World;
		}
		AWaterZone* Zone = Cast<AWaterZone>(mcp_common::FindActorByLabel(World, ZoneLabel));
		if (!Zone)
		{
			OutError = mcp_common::MakeErrorReply(FString::Printf(TEXT("Water zone actor not found: %s"), *ZoneLabel));
		}
		return Zone;
	}

	/**
	 * The zone's raw struct UPROPERTY PropertyName, read by reflection (GetZoneExtent applies the actor scale and
	 * GetRenderTargetResolution is not exported), or null when the property is missing or is not the struct named
	 * StructName.
	 */
	template <typename T>
	const T* Water_ZoneStructProperty(const AWaterZone* Zone, const TCHAR* PropertyName, const TCHAR* StructName)
	{
		const FStructProperty* Prop = CastField<FStructProperty>(AWaterZone::StaticClass()->FindPropertyByName(PropertyName));
		if (Prop && Prop->Struct && Prop->Struct->GetFName() == FName(StructName))
		{
			return Prop->ContainerPtrToValuePtr<T>(Zone);
		}
		return nullptr;
	}

	/**
	 * Empty when the engine's setters can write Curve at InputKey, else why not. They write the point
	 * FInterpCurve::GetPointIndexForInputValue returns, which is -1 for a key before the first point and asserts
	 * on an empty curve: either would kill the editor, so both are refused before anything is written.
	 */
	FString Water_CurveKeyError(const FInterpCurveFloat& Curve, float InputKey, const TCHAR* CurveName)
	{
		if (Curve.Points.Num() == 0)
		{
			return FString::Printf(TEXT("The water spline metadata has no %s points; nothing was changed"), CurveName);
		}
		if (InputKey < Curve.Points[0].InVal)
		{
			return FString::Printf(TEXT("input_key %g is before the first spline point (key %g); nothing was changed"),
				InputKey, Curve.Points[0].InVal);
		}
		return FString();
	}

	/** Whether the engine's setter would change the value it writes for InputKey (Curve already validated). */
	bool Water_CurveKeyDiffers(const FInterpCurveFloat& Curve, float InputKey, float Value)
	{
		return Curve.Points[Curve.GetPointIndexForInputValue(InputKey)].OutVal != Value;
	}

	/** The index of the point the engine's setters write for InputKey, or INDEX_NONE when Water_CurveKeyError refuses it. */
	int32 Water_CurvePointIndex(const FInterpCurveFloat& Curve, float InputKey)
	{
		return (Curve.Points.Num() > 0 && InputKey >= Curve.Points[0].InVal)
			? Curve.GetPointIndexForInputValue(InputKey) : INDEX_NONE;
	}

	/**
	 * The value Curve holds at point PointIndex, which is what the engine's setters wrote; the getters evaluate the
	 * curve at the key instead, which interpolates between points. A curve without that point is evaluated at InputKey.
	 */
	float Water_CurvePointValue(const FInterpCurveFloat& Curve, int32 PointIndex, float InputKey)
	{
		return Curve.Points.IsValidIndex(PointIndex) ? Curve.Points[PointIndex].OutVal : Curve.Eval(InputKey, 0.0f);
	}

	/**
	 * The component of the body the water_body_label param names, with the zone the water_zone_label param names in
	 * OutZone (both params required); on failure returns nullptr and fills OutError.
	 */
	UWaterBodyComponent* Water_ResolveBodyAndZone(const TSharedPtr<FJsonObject>& Params, FString& OutBodyLabel,
		FString& OutZoneLabel, AWaterZone*& OutZone, TSharedPtr<FJsonObject>& OutError)
	{
		if (!Params->TryGetStringField(TEXT("water_body_label"), OutBodyLabel) || OutBodyLabel.IsEmpty())
		{
			OutError = mcp_common::MakeErrorReply(TEXT("water_body_label is required"));
			return nullptr;
		}
		if (!Params->TryGetStringField(TEXT("water_zone_label"), OutZoneLabel) || OutZoneLabel.IsEmpty())
		{
			OutError = mcp_common::MakeErrorReply(TEXT("water_zone_label is required"));
			return nullptr;
		}
		AWaterBody* Body = Water_FindBody(OutBodyLabel, OutError);
		if (!Body)
		{
			return nullptr;
		}
		OutZone = Water_FindZone(OutZoneLabel, OutError);
		if (!OutZone)
		{
			return nullptr;
		}
		UWaterBodyComponent* BodyComp = Body->GetWaterBodyComponent();
		if (!BodyComp)
		{
			OutError = mcp_common::MakeErrorReply(TEXT("Body has no UWaterBodyComponent"));
		}
		return BodyComp;
	}

	/**
	 * The water_body_label param's body (in OutBody) and its component, which must support waves when
	 * bRequireWaveSupport; on failure returns nullptr and fills OutError.
	 */
	UWaterBodyComponent* Water_ResolveBodyComponent(const TSharedPtr<FJsonObject>& Params, bool bRequireWaveSupport,
		AWaterBody*& OutBody, TSharedPtr<FJsonObject>& OutError)
	{
		OutError.Reset();
		FString BodyLabel;
		OutBody = Water_FindBodyParam(Params, BodyLabel, OutError);
		if (!OutBody)
		{
			return nullptr;
		}
		UWaterBodyComponent* Comp = OutBody->GetWaterBodyComponent();
		if (!Comp)
		{
			OutError = mcp_common::MakeErrorReply(TEXT("Body has no UWaterBodyComponent"));
			return nullptr;
		}
		if (bRequireWaveSupport && !Comp->IsWaveSupported())
		{
			OutError = mcp_common::MakeErrorReply(
				FString::Printf(TEXT("%s does not support waves (only Lake/Ocean bodies do)"), *BodyLabel));
			return nullptr;
		}
		return Comp;
	}

	// --- Gerstner-wave helpers ----------------------------------
	/** Wire a generator into a fresh waves object, recompute, and apply to the body. */
	void ApplyGerstnerWaves(AWaterBody* Body, UGerstnerWaterWaves* Waves, UGerstnerWaterWaveGeneratorBase* Generator)
	{
		Waves->GerstnerWaveGenerator = Generator;
		// Recompute the cached wave array + max height before wiring to the body
		// (headless path: no BP script).
		Waves->RecomputeWaves(/*bAllowBPScript*/ false);
		// The waves object is a property of the actor, which is recorded before the write so the call is one undo
		// step and dirties the actor's package. The component is recorded too: its PreEditUndo/PostEditUndo are what
		// re-register the restored waves and rebuild the body on undo.
		Body->Modify();
		if (UWaterBodyComponent* Comp = Body->GetWaterBodyComponent())
		{
			Comp->Modify();
		}
		Body->SetWaterWaves(Waves);
	}

	/** Fill the common wave-state response fields from a wave-bearing component. */
	void FillWaveState(const TSharedPtr<FJsonObject>& Data, UWaterBodyComponent* Comp, const UGerstnerWaterWaves* Waves)
	{
		Data->SetBoolField(TEXT("wave_supported"), Comp->IsWaveSupported());
		Data->SetBoolField(TEXT("has_waves"), Comp->HasWaves());
		Data->SetNumberField(TEXT("num_waves"), Waves->GetGerstnerWaves().Num());
		Data->SetNumberField(TEXT("max_wave_height"), Comp->GetMaxWaveHeight());
		// "applied" = the waves object was set on the body (the authoring action
		// succeeded), independent of whether the generator yielded any waves.
		Data->SetBoolField(TEXT("applied"), Comp->GetWaterWaves() != nullptr);

		// Sum of per-wave Steepness. GerstnerWaveEvaluation.cpp confirms
		// Q_i*k_i == Steepness_i for this parametrization, so Sigma(steepness) <= 1 is
		// the exact no-self-intersection criterion. Surfaced here so callers can verify
		// a wave set is fold-free from the same payload that created/reports it, instead
		// of a second get_water_body_gerstner_wave_detail call + manual summation.
		float TotalSteepness = 0.0f;
		for (const FGerstnerWave& W : Waves->GetGerstnerWaves())
		{
			TotalSteepness += W.Steepness;
		}
		Data->SetNumberField(TEXT("total_steepness"), TotalSteepness);
	}
}

// ---------------------------------------------------------------------------
// create_water_zone
// ---------------------------------------------------------------------------
// MCP:create_water_zone - Spawn a new AWaterZone actor in the editor world at location, unrotated, and label it. Not idempotent: every call adds another zone, even with a label already in use. Nothing is saved. Returns actor_label, actor_name and has_water_mesh; link bodies via water_zone_label of create_lake_water_body and its siblings.
//   @param actor_label - Label to assign to the new AWaterZone actor
//   @param location - World location [X, Y, Z] in cm; anything but a 3-number array means the origin.
//   @returns { actor_label: string, actor_name: string, has_water_mesh: bool }
TSharedPtr<FJsonObject> HandleCreateWaterZone(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ActorLabel;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_label"), ActorLabel)) { return Missing; }
	UWorld* World = nullptr;
	if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World)) { return NoWorld; }
	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWaterZone* Zone = World->SpawnActor<AWaterZone>(
		AWaterZone::StaticClass(), ReadLocationOrZero(Params), FRotator::ZeroRotator, SpawnParams);
	if (!Zone)
	{
		return mcp_common::MakeErrorReply(
			TEXT("Failed to spawn AWaterZone"));
	}
	Zone->SetActorLabel(ActorLabel);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_label"), Zone->GetActorLabel());
	Data->SetStringField(TEXT("actor_name"), Zone->GetName());
	Data->SetBoolField(TEXT("has_water_mesh"), Zone->GetWaterMeshComponent() != nullptr);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// create_lake_water_body / create_river_water_body / create_ocean_water_body
//   All three follow the same pattern: spawn the AWaterBody base class
//   (the simplest entry point — the engine derives Lake/River/Ocean from
//   it), set the actor label, and (if a water zone label is supplied)
//   auto-link to that zone via AddWaterBodyComponent.
// ---------------------------------------------------------------------------
//
// WAVES ARE READ BACK HERE, AND ON A LAKE THAT READING IS THE WHOLE POINT (a MUTE
// failure otherwise). The engine's Lake actor factory attaches a WaterWavesAssetReference
// whose asset reports GetMaxWaveHeight() = 508.125 uu (5.08 m): the SAME waves as the
// OCEAN. On a pond a few meters deep that puts the crest above the lip of the basin and the
// trough below the bed, and NOTHING in the lake's own properties says so -- `water_waves`
// is an object whose class exposes no `waves`/`asset` field, so every property reads
// green. (A river, by contrast, is born at 0.000, which is what makes the lake case look
// normal.) So: the component's GetMaxWaveHeight() is read after the spawn and reported,
// a `warning` is emitted when a body that should not have waves has them, and
// `disable_waves` (OPT-IN, default false) sets water_waves = None and RE-READS the
// height to prove it went to 0.
static TSharedPtr<FJsonObject> SpawnWaterBodyAndLink(
	const TSharedPtr<FJsonObject>& Params,
	UClass* BodyClass,
	const TCHAR* BodyTypeLabel,
	bool bWavesAreUnexpected = false)
{
	FString ActorLabel, ZoneLabel;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_label"), ActorLabel)) { return Missing; }
	Params->TryGetStringField(TEXT("water_zone_label"), ZoneLabel);

	UWorld* World = nullptr;
	if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World)) { return NoWorld; }

	// affects_landscape defaults to FALSE (non-destructive). A freshly constructed
	// UWaterBodyComponent defaults bAffectsLandscape=true; FWaterEditorModule reacts
	// to GEngine->OnLevelActorAdded -- broadcast SYNCHRONOUSLY INSIDE
	// UWorld::SpawnActor, before any code here would get to run -- by spawning an
	// AWaterBrushManager and registering it into the landscape's "Water" edit layer.
	// A post-spawn flag flip is too late: the brush manager already exists by then,
	// and the next edit-layer merge (e.g. on save_level) can flatten the terrain.
	// The flag has to be set from CustomPreSpawnInitialization, which SpawnActor
	// runs before that broadcast.
	bool bAffectsLandscape = false;
	Params->TryGetBoolField(TEXT("affects_landscape"), bAffectsLandscape);

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.CustomPreSpawnInitialization = [bAffectsLandscape](AActor* NewActor)
	{
		if (AWaterBody* NewBody = Cast<AWaterBody>(NewActor))
		{
			if (UWaterBodyComponent* NewComp = NewBody->GetWaterBodyComponent())
			{
				NewComp->bAffectsLandscape = bAffectsLandscape;
			}
		}
	};

	// Spawn the CONCRETE water-body class (AWaterBodyLake/River/Ocean) THROUGH the
	// engine's own actor factory -- the same path the editor's Water placement UI
	// uses -- so e.g. UWaterBodyOceanActorFactory::PostSpawnActor applies the same
	// defaults a manual placement gets (surface/static-mesh/underwater-PP/water-info
	// materials, Gerstner waves, curve/heightmap/weightmap settings). A raw
	// SpawnActor skips all of that and produces an invisible, materialless body.
	// The base AWaterBody is UCLASS(Abstract) -- SpawnActor on it always fails --
	// so each create_* command must still resolve its concrete subclass.
	const FTransform SpawnTransform(FRotator::ZeroRotator, ReadLocationOrZero(Params));
	AWaterBody* Body = nullptr;
	if (UActorFactory* Factory = GEditor->FindActorFactoryForActorClass(BodyClass))
	{
		// CreateActor logs Asset->GetFullName() internally, so InAsset must be
		// non-null; a UClass* is a valid stand-in here (GetDefaultActorClass
		// ignores AssetData and returns the factory's own NewActorClass) -- the
		// same pattern FWaterEditorModule itself uses to spawn its brush manager.
		Body = Cast<AWaterBody>(Factory->CreateActor(BodyClass, World->GetCurrentLevel(), SpawnTransform, SpawnParams));
	}
	if (!Body)
	{
		// Defensive fallback (the Water module registers these factories at
		// startup, so FindActorFactoryForActorClass should not fail in practice):
		// still spawns correctly and is still safe from the brush-manager bug via
		// SpawnParams, just without the factory's material/wave defaults.
		Body = World->SpawnActor<AWaterBody>(
			BodyClass, ReadLocationOrZero(Params), FRotator::ZeroRotator, SpawnParams);
	}
	if (!Body)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Failed to spawn %s"), *BodyClass->GetName()));
	}
	Body->SetActorLabel(ActorLabel);
	UWaterBodyComponent* WaterComp = Body->GetWaterBodyComponent();

	bool bLinkedToZone = false;
	if (!ZoneLabel.IsEmpty())
	{
		if (AWaterZone* Zone = Cast<AWaterZone>(mcp_common::FindActorByLabel(World, ZoneLabel)))
		{
			if (UWaterBodyComponent* BodyComp = Body->GetWaterBodyComponent())
			{
				Zone->AddWaterBodyComponent(BodyComp);
				bLinkedToZone = true;
			}
		}
	}

	// --- waves: read what the factory actually attached, never assume -------
	const float SpawnMaxWaveHeight = WaterComp ? WaterComp->GetMaxWaveHeight() : 0.0f;

	bool bDisableWaves = false;
	Params->TryGetBoolField(TEXT("disable_waves"), bDisableWaves);
	bool bWavesDisabled = false;
	if (bDisableWaves)
	{
		Body->Modify();
		Body->SetWaterWaves(nullptr);
		bWavesDisabled = true;
	}
	// RE-READ after the (optional) mutation -- this is the number that proves it.
	const float MaxWaveHeight = WaterComp ? WaterComp->GetMaxWaveHeight() : 0.0f;

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_label"), Body->GetActorLabel());
	Data->SetStringField(TEXT("actor_name"), Body->GetName());
	Data->SetStringField(TEXT("body_type"), BodyTypeLabel);
	Data->SetStringField(TEXT("water_zone_label"), ZoneLabel);
	Data->SetBoolField(TEXT("linked_to_zone"), bLinkedToZone);
	Data->SetBoolField(TEXT("affects_landscape"), WaterComp ? WaterComp->AffectsLandscape() : bAffectsLandscape);
	Data->SetNumberField(TEXT("max_wave_height"), MaxWaveHeight);
	Data->SetNumberField(TEXT("max_wave_height_on_spawn"), SpawnMaxWaveHeight);
	Data->SetBoolField(TEXT("has_waves"), WaterComp ? WaterComp->HasWaves() : false);
	Data->SetBoolField(TEXT("waves_disabled"), bWavesDisabled);

	// At most one `warning`, and the more specific one wins: if the caller ASKED for the
	// waves to go away and something still reads back, that is the fact worth reporting.
	if (bDisableWaves && MaxWaveHeight > 0.0f)
	{
		// MaxWaveHeightOffset is added by GetMaxWaveHeight() on top of the wave asset,
		// so clearing the waves does not necessarily reach exactly 0.
		Data->SetStringField(TEXT("warning"), FString::Printf(
			TEXT("disable_waves cleared the wave asset but max_wave_height still reads %.3f uu -- UWaterBodyComponent::GetMaxWaveHeight() adds MaxWaveHeightOffset on top of the asset, so that residue comes from the component, not from the waves."),
			MaxWaveHeight));
	}
	else if (bWavesAreUnexpected && MaxWaveHeight > 0.0f)
	{
		Data->SetStringField(TEXT("warning"), FString::Printf(
			TEXT("This %s was born with waves: max_wave_height = %.3f uu. The engine's actor factory attaches the OCEAN's wave asset to a lake (measured 508.125 uu = 5.08 m). On a shallow basin that puts the crest above the lip and the trough below the bed, and no property of the actor reveals it -- water_waves is an object whose class exposes no wave fields. Re-run with disable_waves=true (or set water_waves = None) to take it to 0.000, and check max_wave_height in the response."),
			BodyTypeLabel, MaxWaveHeight));
	}

	// `success` is DERIVED: asking for a zone that does not exist is a failure, not
	// success:true with linked_to_zone:false buried in the payload.
	const bool bZoneOk = ZoneLabel.IsEmpty() || bLinkedToZone;
	FString Error;
	if (!bZoneOk)
	{
		Error = FString::Printf(
			TEXT("Water body '%s' was spawned but NOT linked: water zone '%s' was not found in the editor world (or the body has no UWaterBodyComponent)."),
			*ActorLabel, *ZoneLabel);
	}

	mcp_common::AddRequestedEcho(Data, Params,
		{ TEXT("actor_label"), TEXT("location"), TEXT("water_zone_label"),
		  TEXT("affects_landscape"), TEXT("disable_waves") });

	return mcp_common::MakeOutcomeReply(Data, bZoneOk, Error);
}

// MCP:create_lake_water_body - Spawn a Lake AWaterBody actor, optionally link to a WaterZone, and READ BACK the wave height the factory attached (a lake is born with the OCEAN's waves and nothing else reveals it).
//   @param actor_label - Label to assign to the new water body actor
//   @param location - Optional [X, Y, Z] world location (defaults to origin)
//   @param water_zone_label - Optional label of an existing AWaterZone to auto-link the body to. If given and the zone is not found, `success` is FALSE (the body is still spawned; `linked_to_zone` says so).
//   @param affects_landscape - When true the body carves the existing landscape via the water landscape brush; defaults to FALSE (non-destructive)
//   @param disable_waves - OPT-IN, default FALSE. Sets water_waves = None right after the spawn and RE-READS GetMaxWaveHeight() to prove it went to 0. Use it on any basin shallower than ~5 m: the engine's Lake factory attaches the ocean's wave asset (measured 508.125 uu = 5.08 m of crest), which on a pond a few meters deep breaks the water surface -- crest above the lip of the basin, trough below the bed -- with every property of the actor reading green.
//   @returns { actor_label: string, actor_name: string, body_type: string, water_zone_label: string, linked_to_zone: bool, affects_landscape: bool, max_wave_height: number (RE-READ from UWaterBodyComponent::GetMaxWaveHeight() AFTER any disable_waves), max_wave_height_on_spawn: number (RE-READ as the factory left it), has_waves: bool, waves_disabled: bool, warning?: string (emitted when a lake was born with waves), requested: { actor_label, location, water_zone_label, affects_landscape, disable_waves } }
//   Note: spawns through the engine's own Water actor factory, so it gets the same surface/static-mesh/underwater-PP/water-info materials and (Lake/Ocean) Gerstner waves the editor's placement UI applies -- not a bare, materialless actor.
TSharedPtr<FJsonObject> HandleCreateLakeWaterBody(
	const TSharedPtr<FJsonObject>& Params)
{
	// bWavesAreUnexpected=true: on a LAKE the factory's waves are a defect worth a
	// warning (they are the ocean's). On an ocean they are correct, on a river there
	// are none.
	return SpawnWaterBodyAndLink(Params, AWaterBodyLake::StaticClass(), TEXT("Lake"), /*bWavesAreUnexpected*/ true);
}

// MCP:create_river_water_body - Spawn a River AWaterBody actor, optionally link to a WaterZone, and read back its wave height (a river is born at 0.000 -- that is the control case for the lake defect).
//   @param actor_label - Label to assign to the new water body actor
//   @param location - Optional [X, Y, Z] world location (defaults to origin)
//   @param water_zone_label - Optional label of an existing AWaterZone to auto-link the body to. If given and the zone is not found, `success` is FALSE (the body is still spawned; `linked_to_zone` says so).
//   @param affects_landscape - When true the body carves the existing landscape via the water landscape brush; defaults to FALSE (non-destructive)
//   @param disable_waves - OPT-IN, default FALSE. Sets water_waves = None right after the spawn and RE-READS GetMaxWaveHeight(). A river has no waves to begin with, so this is normally a no-op here.
//   @returns { actor_label: string, actor_name: string, body_type: string, water_zone_label: string, linked_to_zone: bool, affects_landscape: bool, max_wave_height: number (RE-READ), max_wave_height_on_spawn: number (RE-READ), has_waves: bool, waves_disabled: bool, requested: { actor_label, location, water_zone_label, affects_landscape, disable_waves } }
//   Note: spawns through the engine's own Water actor factory, so it gets the same surface/static-mesh/underwater-PP/water-info materials and (Lake/Ocean) Gerstner waves the editor's placement UI applies -- not a bare, materialless actor.
TSharedPtr<FJsonObject> HandleCreateRiverWaterBody(
	const TSharedPtr<FJsonObject>& Params)
{
	return SpawnWaterBodyAndLink(Params, AWaterBodyRiver::StaticClass(), TEXT("River"));
}

// MCP:create_ocean_water_body - Spawn an Ocean AWaterBody actor, optionally link to a WaterZone, and read back its wave height (on an ocean the factory's waves are CORRECT -- ~508 uu -- so no warning is emitted here).
//   @param actor_label - Label to assign to the new water body actor
//   @param location - Optional [X, Y, Z] world location (defaults to origin)
//   @param water_zone_label - Optional label of an existing AWaterZone to auto-link the body to. If given and the zone is not found, `success` is FALSE (the body is still spawned; `linked_to_zone` says so).
//   @param affects_landscape - When true the body carves the existing landscape via the water landscape brush; defaults to FALSE (non-destructive)
//   @param disable_waves - OPT-IN, default FALSE. Sets water_waves = None right after the spawn and RE-READS GetMaxWaveHeight(). On an ocean you almost certainly do NOT want this.
//   @returns { actor_label: string, actor_name: string, body_type: string, water_zone_label: string, linked_to_zone: bool, affects_landscape: bool, max_wave_height: number (RE-READ), max_wave_height_on_spawn: number (RE-READ), has_waves: bool, waves_disabled: bool, requested: { actor_label, location, water_zone_label, affects_landscape, disable_waves } }
//   Note: spawns through the engine's own Water actor factory, so it gets the same surface/static-mesh/underwater-PP/water-info materials and (Lake/Ocean) Gerstner waves the editor's placement UI applies -- not a bare, materialless actor.
TSharedPtr<FJsonObject> HandleCreateOceanWaterBody(
	const TSharedPtr<FJsonObject>& Params)
{
	return SpawnWaterBodyAndLink(Params, AWaterBodyOcean::StaticClass(), TEXT("Ocean"));
}

// ---------------------------------------------------------------------------
// add_water_body_to_zone  — link an existing water body to an existing zone
// ---------------------------------------------------------------------------
// MCP:add_water_body_to_zone - Link an existing water body actor to an existing AWaterZone.
//   @param water_body_label - Label of the existing AWaterBody actor to link
//   @param water_zone_label - Label of the existing AWaterZone actor to link into
//   @returns { water_body_label: string, water_zone_label: string, linked: bool }
TSharedPtr<FJsonObject> HandleAddWaterBodyToZone(
	const TSharedPtr<FJsonObject>& Params)
{
	FString BodyLabel;
	FString ZoneLabel;
	AWaterZone* Zone = nullptr;
	TSharedPtr<FJsonObject> Error;
	UWaterBodyComponent* BodyComp = Water_ResolveBodyAndZone(Params, BodyLabel, ZoneLabel, Zone, Error);
	if (!BodyComp)
	{
		return Error;
	}

	Zone->AddWaterBodyComponent(BodyComp);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("water_body_label"), BodyLabel);
	Data->SetStringField(TEXT("water_zone_label"), ZoneLabel);
	Data->SetBoolField(TEXT("linked"), true);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// remove_water_body_from_zone  — inverse of add_water_body_to_zone
// ---------------------------------------------------------------------------
// MCP:remove_water_body_from_zone - Unlink an existing water body actor from an existing AWaterZone (inverse of add_water_body_to_zone).
//   @param water_body_label - Label of the existing AWaterBody actor to unlink
//   @param water_zone_label - Label of the existing AWaterZone actor to unlink from
//   @returns { water_body_label: string, water_zone_label: string, removed: bool, note: string }
TSharedPtr<FJsonObject> HandleRemoveWaterBodyFromZone(
	const TSharedPtr<FJsonObject>& Params)
{
	FString BodyLabel;
	FString ZoneLabel;
	AWaterZone* Zone = nullptr;
	TSharedPtr<FJsonObject> Error;
	UWaterBodyComponent* BodyComp = Water_ResolveBodyAndZone(Params, BodyLabel, ZoneLabel, Zone, Error);
	if (!BodyComp)
	{
		return Error;
	}

	// AWaterZone::RemoveWaterBodyComponent removes the component from the zone's private
	// OwnedWaterBodies list; it silently no-ops when the body is not in the list, so the
	// call is idempotent by construction (already_absent needs no explicit pre-check, and
	// there is no public getter for OwnedWaterBodies to read it back — the add twin has the
	// same limitation and reports linked=true unconditionally). This detaches only the
	// zone->body ownership entry; the body's OwningWaterZone pointer is managed separately by
	// UWaterBodyComponent::UpdateWaterZones (spatial overlap / WaterZoneOverride), so a body
	// still spatially inside the zone may be re-added to the list on the next zone update.
	Zone->RemoveWaterBodyComponent(BodyComp);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("water_body_label"), BodyLabel);
	Data->SetStringField(TEXT("water_zone_label"), ZoneLabel);
	Data->SetBoolField(TEXT("removed"), true);
	Data->SetStringField(TEXT("note"),
		TEXT("Removed from the zone's owned-bodies list (idempotent; no-ops if it was not linked). ")
		TEXT("OwnedWaterBodies has no public getter so this is not read-back-verified (the add twin has the same limitation). ")
		TEXT("This detaches only the zone->body ownership entry; the body's OwningWaterZone is managed by UpdateWaterZones (spatial overlap), so a body still inside the zone may be re-added on the next zone update."));
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_water_zone_info
// ---------------------------------------------------------------------------
// MCP:get_water_zone_info - Read info about an existing AWaterZone and count its linked water bodies.
//   @param water_zone_label - Label of the existing AWaterZone actor
//   @returns { actor_label: string, actor_name: string, has_water_mesh: bool, linked_body_count: number, zone_extent: [number, number], overlap_priority: int }
TSharedPtr<FJsonObject> HandleGetWaterZoneInfo(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ZoneLabel;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("water_zone_label"), ZoneLabel)) { return Missing; }
	TSharedPtr<FJsonObject> Error;
	UWorld* World = nullptr;
	AWaterZone* Zone = Water_FindZone(ZoneLabel, Error, &World);
	if (!Zone)
	{
		return Error;
	}

	int32 BodyCount = 0;
	for (TActorIterator<AWaterBody> It(World); It; ++It)
	{
		if (It->GetWaterBodyComponent() && It->GetWaterBodyComponent()->GetWaterZone() == Zone)
		{
			++BodyCount;
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_label"), Zone->GetActorLabel());
	Data->SetStringField(TEXT("actor_name"), Zone->GetName());
	Data->SetBoolField(TEXT("has_water_mesh"), Zone->GetWaterMeshComponent() != nullptr);
	Data->SetNumberField(TEXT("linked_body_count"), BodyCount);
	Data->SetArrayField(TEXT("zone_extent"), mcp_common::Vector2DToJsonArray(Zone->GetZoneExtent()));
	Data->SetNumberField(TEXT("overlap_priority"), Zone->GetOverlapPriority());
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// Water body read-back + material + zone configuration
// ---------------------------------------------------------------------------

// MCP:get_water_body_info - Read back a water body's type, linked zone, surface material and wave state: whether waves are supported and present, the max wave height, the active waves class with its wave count and, for Gerstner waves, the generator class. Read-only; an unknown label is an error. Per-wave detail is get_water_body_gerstner_wave_detail.
//   @param water_body_label - Label of the AWaterBody actor
//   @returns { actor_label: string, actor_name: string, body_type: string, water_zone_label: string, material: string, max_wave_height: number, wave_supported: bool, has_waves: bool, waves_class: string, num_waves: int, generator_class: string }
TSharedPtr<FJsonObject> HandleGetWaterBodyInfo(
	const TSharedPtr<FJsonObject>& Params)
{
	FString BodyLabel;
	TSharedPtr<FJsonObject> Error;
	AWaterBody* Body = Water_FindBodyParam(Params, BodyLabel, Error);
	if (!Body)
	{
		return Error;
	}
	UWaterBodyComponent* Comp = Body->GetWaterBodyComponent();
	AWaterZone* Zone = Comp ? Comp->GetWaterZone() : nullptr;
	UMaterialInterface* Mat = Comp ? Comp->GetWaterMaterial() : nullptr;

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_label"), Body->GetActorLabel());
	Data->SetStringField(TEXT("actor_name"), Body->GetName());
	Data->SetStringField(TEXT("body_type"), WaterBodyTypeToString(Body->GetWaterBodyType()));
	Data->SetStringField(TEXT("water_zone_label"), Zone ? Zone->GetActorLabel() : FString());
	Data->SetStringField(TEXT("material"), Mat ? Mat->GetName() : FString(TEXT("None")));
	Data->SetNumberField(TEXT("max_wave_height"), Comp ? Comp->GetMaxWaveHeight() : 0.0);

	// Wave state: the waves object and, for Gerstner waves, its cached wave count and generator.
	UWaterWavesBase* WavesBase = Comp ? Comp->GetWaterWaves() : nullptr;
	int32 WaveCount = 0;
	FString GeneratorClass;
	if (const UGerstnerWaterWaves* Gerstner = Cast<UGerstnerWaterWaves>(WavesBase))
	{
		WaveCount = Gerstner->GetGerstnerWaves().Num();
		GeneratorClass = Gerstner->GerstnerWaveGenerator ? Gerstner->GerstnerWaveGenerator->GetClass()->GetName() : FString();
	}
	Data->SetBoolField(TEXT("wave_supported"), Comp ? Comp->IsWaveSupported() : false);
	Data->SetBoolField(TEXT("has_waves"), Comp ? Comp->HasWaves() : false);
	Data->SetStringField(TEXT("waves_class"), WavesBase ? WavesBase->GetClass()->GetName() : FString(TEXT("None")));
	Data->SetNumberField(TEXT("num_waves"), WaveCount);
	Data->SetStringField(TEXT("generator_class"), GeneratorClass);
	return mcp_common::MakeOkReply(Data);
}

// MCP:set_water_body_material - Set the surface water material on a water body's UWaterBodyComponent (body found by actor label in the editor world, material loaded by asset path) and rebuild its material instances. The change is one undo step and dirties the level; nothing is saved. The material it already has changes nothing, adds no undo step and still answers set=true; set confirms the component now holds the requested material, and material is that material's short name, not its path. An unknown label, a body without a water body component, or a path that does not load as a UMaterialInterface is an error. Read it back with get_water_body_info; other body settings: set_water_body_properties; waves: set_water_body_gerstner_waves.
//   @param water_body_label - Actor label of the AWaterBody in the editor world (case-insensitive; the object name does not match).
//   @param material_path - Asset path of the UMaterialInterface (a material or material instance), e.g. "/Game/Water/M_Water" (the object path form also works).
//   @returns { water_body_label: string, material: string, set: bool }
TSharedPtr<FJsonObject> HandleSetWaterBodyMaterial(
	const TSharedPtr<FJsonObject>& Params)
{
	FString BodyLabel, MaterialPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("water_body_label"), BodyLabel)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_path"), MaterialPath)) { return Missing; }
	TSharedPtr<FJsonObject> Error;
	AWaterBody* Body = Water_FindBody(BodyLabel, Error);
	if (!Body)
	{
		return Error;
	}
	UWaterBodyComponent* Comp = Body->GetWaterBodyComponent();
	if (!Comp) { return mcp_common::MakeErrorReply(TEXT("Body has no UWaterBodyComponent")); }
	UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
	if (!Mat)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Material not found: %s"), *MaterialPath));
	}
	// The material is a property of the component: recording it before the write makes the call one undo step and
	// dirties the actor's package. The material it already has is left alone, so that call dirties nothing.
	if (Comp->GetWaterMaterial() != Mat)
	{
		Comp->Modify();
		Comp->SetWaterMaterial(Mat);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("water_body_label"), BodyLabel);
	Data->SetStringField(TEXT("material"), Comp->GetWaterMaterial() ? Comp->GetWaterMaterial()->GetName() : FString(TEXT("None")));
	Data->SetBoolField(TEXT("set"), Comp->GetWaterMaterial() == Mat);
	return mcp_common::MakeOkReply(Data);
}

// MCP:configure_water_zone - Set an AWaterZone's planar extent and/or water-info render-target resolution, then RE-READ both from the actor (extent, world bounds, texel density) instead of echoing the request back.
//   @param water_zone_label - Label of the AWaterZone actor
//   @param extent - Optional [X, Y] planar zone extent. !!! THIS IS THE **COMPLETE** EXTENT, NOT THE HALF-EXTENT / SEMI-AXIS !!! The engine halves it itself (AWaterZone::GetZoneBounds2D: "GetZoneExtents returns the full extent of the zone but BoxSphereBounds expects a half-extent"), so passing 4000000 gives world bounds of [-2000000, +2000000] around the actor -- a 40 km sea, not 80 km. Reading it as a semi-axis gives a sea of DOUBLE the intended size with bounds off the map; the `world_bounds_extent` field in the response is what disambiguates it (half of `zone_extent_actual`).
//   @param render_target_resolution - Optional [X, Y] water-info render target resolution. Read back by reflection as `render_target_resolution_actual` (the getter is not exported, so this is the raw UPROPERTY, i.e. BEFORE the r.Water.WaterInfo.RenderTargetResolutionMax clamp).
//   @returns { water_zone_label: string, zone_extent: [x,y] (RE-READ, kept for compatibility), zone_extent_actual: [x,y] (RE-READ, COMPLETE extent), world_bounds_extent: [x,y,z] (HALF-extent of the real actor bounds), world_bounds_origin: [x,y,z], world_bounds_min: [x,y,z], world_bounds_max: [x,y,z], actor_scale: [x,y,z], render_target_resolution_actual: [x,y] (omitted if unreadable), render_target_resolution_verified: bool, water_info_texel_size_uu: number (zone_extent / render_target_resolution; omitted if the resolution is unreadable or zero), applied: bool (DERIVED from read-back == request), requested: { extent, render_target_resolution } }
//
// WHY. A hardcoded `applied` and an extent that agrees with the request by construction
// can never expose a mistake. The mistake this tool invites is a SEMANTIC one and the field
// name is what invites it: `ZoneExtent` is the COMPLETE extent. The oracle that settles it
// is the contrast between the two numbers in the payload: for a requested 4,000,000 the
// real half-extent is 2,000,000. `water_info_texel_size_uu` is emitted for the same
// reason: zone_extent / render_target_resolution is what decides whether a shoreline can
// resolve at all (40 km / 512 = 78 m per texel = a flat blue sea no material can correct),
// and nothing in the actor's properties says so.
TSharedPtr<FJsonObject> HandleConfigureWaterZone(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ZoneLabel;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("water_zone_label"), ZoneLabel)) { return Missing; }
	TSharedPtr<FJsonObject> LookupError;
	AWaterZone* Zone = Water_FindZone(ZoneLabel, LookupError);
	if (!Zone)
	{
		return LookupError;
	}
	bool bExtentRequested = false;
	FVector2D RequestedExtent(0.0, 0.0);
	const TArray<TSharedPtr<FJsonValue>>* ExtArr = nullptr;
	if (Params->TryGetArrayField(TEXT("extent"), ExtArr) && ExtArr && ExtArr->Num() >= 2)
	{
		RequestedExtent = FVector2D((*ExtArr)[0]->AsNumber(), (*ExtArr)[1]->AsNumber());
		bExtentRequested = true;
	}
	bool bResolutionRequested = false;
	FIntPoint RequestedResolution(0, 0);
	const TArray<TSharedPtr<FJsonValue>>* ResArr = nullptr;
	if (Params->TryGetArrayField(TEXT("render_target_resolution"), ResArr) && ResArr && ResArr->Num() >= 2)
	{
		RequestedResolution = FIntPoint(
			static_cast<int32>((*ResArr)[0]->AsNumber()), static_cast<int32>((*ResArr)[1]->AsNumber()));
		bResolutionRequested = true;
	}

	// Both values are UPROPERTYs of the zone actor, recorded before the first write so the call is one undo step
	// and dirties the actor's package. A value the zone already holds (its raw property, before actor scale) is not
	// written, so a call that changes nothing dirties nothing.
	const FVector2D* StoredExtent = Water_ZoneStructProperty<FVector2D>(Zone, TEXT("ZoneExtent"), TEXT("Vector2D"));
	const FIntPoint* StoredResolution = Water_ZoneStructProperty<FIntPoint>(Zone, TEXT("RenderTargetResolution"), TEXT("IntPoint"));
	const bool bWriteExtent = bExtentRequested && !(StoredExtent && *StoredExtent == RequestedExtent);
	const bool bWriteResolution = bResolutionRequested && !(StoredResolution && *StoredResolution == RequestedResolution);
	if (bWriteExtent || bWriteResolution)
	{
		Zone->Modify();
	}
	if (bWriteExtent)
	{
		Zone->SetZoneExtent(RequestedExtent);
	}
	if (bWriteResolution)
	{
		Zone->SetRenderTargetResolution(RequestedResolution);
	}

	// --- everything below is a RE-READ of the actor ------------------------
	// NOTE: GetZoneExtent() is `ZoneExtent * actor scale`, so a scaled zone actor
	// legitimately reads back something other than what was set -- which is exactly
	// the kind of mismatch this response surfaces (see `actor_scale`).
	const FVector2D Extent = Zone->GetZoneExtent();
	const FBox ZoneBounds = Zone->GetZoneBounds();
	FVector BoundsOrigin = FVector::ZeroVector;
	FVector BoundsExtent = FVector::ZeroVector;
	ZoneBounds.GetCenterAndExtents(BoundsOrigin, BoundsExtent);
	const FVector ActorScale = Zone->GetActorScale3D();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("water_zone_label"), ZoneLabel);
	Data->SetArrayField(TEXT("zone_extent"), mcp_common::Vector2DToJsonArray(Extent));
	Data->SetArrayField(TEXT("zone_extent_actual"), mcp_common::Vector2DToJsonArray(Extent));
	Data->SetArrayField(TEXT("world_bounds_extent"), mcp_common::VectorToJsonArray(BoundsExtent));
	Data->SetArrayField(TEXT("world_bounds_origin"), mcp_common::VectorToJsonArray(BoundsOrigin));
	Data->SetArrayField(TEXT("world_bounds_min"), mcp_common::VectorToJsonArray(ZoneBounds.Min));
	Data->SetArrayField(TEXT("world_bounds_max"), mcp_common::VectorToJsonArray(ZoneBounds.Max));
	Data->SetArrayField(TEXT("actor_scale"), mcp_common::VectorToJsonArray(ActorScale));

	// Reflection read of the RenderTargetResolution UPROPERTY. The getter
	// (AWaterZone::GetRenderTargetResolution) is declared WITHOUT UE_API, so calling it
	// from this module does not link; the property itself is reachable. This is the raw
	// authored value, i.e. before the r.Water.WaterInfo.RenderTargetResolutionMax clamp
	// the getter would apply.
	const FIntPoint* ResolutionValue = Water_ZoneStructProperty<FIntPoint>(Zone, TEXT("RenderTargetResolution"), TEXT("IntPoint"));
	const bool bResolutionRead = (ResolutionValue != nullptr);
	const FIntPoint ActualResolution = bResolutionRead ? *ResolutionValue : FIntPoint(0, 0);
	if (bResolutionRead)
	{
		Data->SetArrayField(TEXT("render_target_resolution_actual"), mcp_common::Vector2DToJsonArray(ActualResolution));
		if (ActualResolution.X > 0 && ActualResolution.Y > 0)
		{
			// The number that decides whether a shoreline can resolve. One texel of the
			// Water Info Texture covers this many unreal units.
			Data->SetNumberField(TEXT("water_info_texel_size_uu"),
				FMath::Max(Extent.X / ActualResolution.X, Extent.Y / ActualResolution.Y));
		}
	}

	// `applied` is DERIVED: read-back vs request, per field that was actually requested.
	// 1 uu of tolerance on the extent (it round-trips through a double multiply by the
	// actor scale); the resolution is integer and compared exactly.
	const bool bExtentOk = !bExtentRequested || Extent.Equals(RequestedExtent, 1.0);
	const bool bResolutionOk = !bResolutionRequested || !bResolutionRead || (ActualResolution == RequestedResolution);
	const bool bApplied = bExtentOk && bResolutionOk;
	Data->SetBoolField(TEXT("applied"), bApplied);
	// Honest about what could NOT be checked, rather than folding it into `applied`.
	Data->SetBoolField(TEXT("render_target_resolution_verified"), bResolutionRead);

	mcp_common::AddRequestedEcho(Data, Params,
		{ TEXT("extent"), TEXT("render_target_resolution") });

	FString Error;
	if (!bExtentOk)
	{
		Error = FString::Printf(
			TEXT("zone_extent read back from the actor (%.1f, %.1f) does not match the requested (%.1f, %.1f). GetZoneExtent() multiplies by the actor scale (%s)."),
			Extent.X, Extent.Y, RequestedExtent.X, RequestedExtent.Y, *ActorScale.ToString());
	}
	else if (!bResolutionOk)
	{
		Error = FString::Printf(
			TEXT("render_target_resolution read back from the actor (%d, %d) does not match the requested (%d, %d)."),
			ActualResolution.X, ActualResolution.Y, RequestedResolution.X, RequestedResolution.Y);
	}
	return mcp_common::MakeOutcomeReply(Data, bApplied, Error);
}

// ---------------------------------------------------------------------------
// River spline path + per-point width/depth
// ---------------------------------------------------------------------------

// MCP:set_water_body_spline_points - Set ANY water body's spline path from world points, then synchronize the water spline metadata and RE-READ the spline (point count + closed-loop) to prove it landed.
//   @param water_body_label - Label of the AWaterBody actor. DESPITE THE COMMAND NAME THIS IS NOT RIVER-ONLY: it works on any water body that has a UWaterSplineComponent -- lake, ocean, river, custom -- and it is the only way to give a LAKE its real shoreline, because the spline the actor factory creates is an arbitrary CLOSED triangle (3 points) that has nothing to do with the basin. `closed_loop` is a property of the spline and is NOT touched here: set 12 points on a factory-made lake and you get 12 points still closed.
//   @param points - Array of [X, Y, Z] WORLD points (>= 2) defining the spline. Entries that are not a >= 3-element numeric array are skipped silently; `requested_point_count` reports how many survived parsing.
//   @returns { water_body_label: string, point_count: int (RE-READ), spline_point_count_actual: int (RE-READ, same value, unambiguously named), closed_loop_actual: bool (RE-READ), requested_point_count: int, applied: bool (DERIVED: read-back count == requested count) }
//
// WHY. The write works; the re-read is what proves it. A hardcoded `applied: true`
// would read the same whether the spline had taken the points or not. The re-read is
// cheap (GetNumberOfSplinePoints/IsClosedLoop) and turns "applied" into a measurement.
// The whole `points` array is deliberately NOT echoed back under "requested": on a real
// river that doubles the payload for no information. Use get_water_body_spline_points if you
// need the resulting points one by one.
TSharedPtr<FJsonObject> HandleSetRiverSplinePoints(
	const TSharedPtr<FJsonObject>& Params)
{
	FString BodyLabel;
	TSharedPtr<FJsonObject> LookupError;
	AWaterBody* Body = Water_FindBodyParam(Params, BodyLabel, LookupError);
	if (!Body)
	{
		return LookupError;
	}
	UWaterSplineComponent* Spline = Body->GetWaterSpline();
	if (!Spline) { return mcp_common::MakeErrorReply(TEXT("Body has no water spline")); }

	const TArray<TSharedPtr<FJsonValue>>* PtsArr = nullptr;
	if (!Params->TryGetArrayField(TEXT("points"), PtsArr) || !PtsArr)
	{
		return mcp_common::MakeErrorReply(TEXT("points is required (array of [X,Y,Z])"));
	}
	TArray<FVector> Pts;
	for (const TSharedPtr<FJsonValue>& V : *PtsArr)
	{
		const TArray<TSharedPtr<FJsonValue>>* P = nullptr;
		if (V.IsValid() && V->TryGetArray(P) && P && P->Num() >= 3)
		{
			Pts.Add(FVector((*P)[0]->AsNumber(), (*P)[1]->AsNumber(), (*P)[2]->AsNumber()));
		}
	}
	if (Pts.Num() < 2)
	{
		return mcp_common::MakeErrorReply(TEXT("Need at least 2 valid [X,Y,Z] points"));
	}

	const int32 RequestedCount = Pts.Num();
	// The points live on the spline component and the per-point water values on its metadata, which SetSplinePoints
	// resets and the synchronization below fixes up: both are recorded before the write, as the editor's spline
	// visualizer does, so the call is one undo step and dirties the actor's package.
	Spline->Modify();
	if (USplineMetadata* Metadata = Spline->GetSplinePointsMetadata())
	{
		Metadata->Modify();
	}
	Spline->SetSplinePoints(Pts, ESplineCoordinateSpace::World, /*bUpdateSpline*/ true);
	// Propagate to the water spline metadata (required when editing points
	// outside the editor UI; verified WATER_API).
	Spline->K2_SynchronizeAndBroadcastDataChange();

	// --- RE-READ of the spline, not a copy of the input --------------------
	const int32 ActualCount = Spline->GetNumberOfSplinePoints();
	const bool  bClosedLoop = Spline->IsClosedLoop();
	const bool  bApplied = (ActualCount == RequestedCount);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("water_body_label"), BodyLabel);
	Data->SetNumberField(TEXT("point_count"), ActualCount);
	Data->SetNumberField(TEXT("spline_point_count_actual"), ActualCount);
	Data->SetBoolField(TEXT("closed_loop_actual"), bClosedLoop);
	Data->SetNumberField(TEXT("requested_point_count"), RequestedCount);
	Data->SetBoolField(TEXT("applied"), bApplied);

	FString Error;
	if (!bApplied)
	{
		Error = FString::Printf(
			TEXT("Spline read back with %d points after setting %d."), ActualCount, RequestedCount);
	}
	return mcp_common::MakeOutcomeReply(Data, bApplied, Error);
}

// MCP:get_water_body_spline_points - Read the spline control points of any water body -- river, lake, ocean or custom, found by actor label in the editor world -- as world-space [x, y, z] positions in cm, in spline order. Only point locations come back: no tangents, widths, depths or closed-loop flag. Read-only. An unknown label, or a body without a water spline, is an error. Write them with set_water_body_spline_points; per-point river width and depth: set_river_width_depth_at_point; a generic actor's USplineComponent in local space, with its closed-loop flag: get_spline_points; body type, zone and material: get_water_body_info.
//   @param water_body_label - Actor label of the AWaterBody in the editor world (case-insensitive; the object name does not match).
//   @returns { water_body_label: string, point_count: int, points: [[number, number, number]] }
TSharedPtr<FJsonObject> HandleGetRiverSplinePoints(
	const TSharedPtr<FJsonObject>& Params)
{
	FString BodyLabel;
	TSharedPtr<FJsonObject> Error;
	AWaterBody* Body = Water_FindBodyParam(Params, BodyLabel, Error);
	if (!Body)
	{
		return Error;
	}
	UWaterSplineComponent* Spline = Body->GetWaterSpline();
	if (!Spline) { return mcp_common::MakeErrorReply(TEXT("Body has no water spline")); }

	const int32 Count = Spline->GetNumberOfSplinePoints();
	TArray<TSharedPtr<FJsonValue>> PointsJson;
	for (int32 i = 0; i < Count; ++i)
	{
		const FVector Loc = Spline->GetLocationAtSplinePoint(i, ESplineCoordinateSpace::World);
		TArray<TSharedPtr<FJsonValue>> P = mcp_common::VectorToJsonArray(Loc);
		PointsJson.Add(MakeShared<FJsonValueArray>(P));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("water_body_label"), BodyLabel);
	Data->SetNumberField(TEXT("point_count"), Count);
	Data->SetArrayField(TEXT("points"), PointsJson);
	return mcp_common::MakeOkReply(Data);
}

// MCP:set_river_width_depth_at_point - Set the width and/or depth of a river water body, found by actor label in the editor world, at the spline point at or before input_key (point i sits at key i; past the last point, the last one), then synchronize the spline so the river rebuilds; nothing is saved. A value below 0.0001 is stored as 0.0001, as the engine's sync does. width and depth are read back from that point, named by point_index. A value already held is not written: applied is false with a note and nothing changes; otherwise the call is one undo step that dirties the actor's package. Neither width nor depth, a non-river body, an unknown label, a spline without points or a key before the first point is an error and nothing changes. All points at once: set_river_widths_depths; point positions: get_water_body_spline_points.
//   @param water_body_label - Label of the AWaterBodyRiver actor
//   @param input_key - Spline input key at which to set width/depth; a key before the first spline point (below 0) is an error and nothing is changed
//   @param width - Optional new river width at the point at or before the key (give width, depth or both)
//   @param depth - Optional new river depth at the point at or before the key (give width, depth or both)
//   @returns { water_body_label: string, input_key: number, point_index: int, width: number, depth: number, applied: bool, note: string? }
TSharedPtr<FJsonObject> HandleSetRiverWidthDepthAtPoint(
	const TSharedPtr<FJsonObject>& Params)
{
	FString BodyLabel;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("water_body_label"), BodyLabel)) { return Missing; }
	double InputKeyD = 0.0;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("input_key"), InputKeyD)) { return Missing; }
	const float InputKey = static_cast<float>(InputKeyD);
	TSharedPtr<FJsonObject> Error;
	AWaterBody* Body = Water_FindBody(BodyLabel, Error);
	if (!Body)
	{
		return Error;
	}
	UWaterBodyRiverComponent* River = Cast<UWaterBodyRiverComponent>(Body->GetWaterBodyComponent());
	if (!River)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("%s is not a river water body"), *BodyLabel));
	}
	// The engine's setters write the per-point curves of the component's water spline metadata.
	UWaterSplineMetadata* Metadata = River->GetWaterSplineMetadata();
	if (!Metadata)
	{
		return mcp_common::MakeErrorReply(FString::Printf(TEXT("%s has no water spline metadata"), *BodyLabel));
	}

	// The spline sync below pushes the point values into the spline, which a spline without points cannot hold.
	UWaterSplineComponent* Spline = Body->GetWaterSpline();
	if (!Spline || Spline->GetNumberOfSplinePoints() == 0)
	{
		return mcp_common::MakeErrorReply(FString::Printf(TEXT("%s has no spline points; nothing was changed"), *BodyLabel));
	}

	double WidthD = 0.0;
	const bool bHasWidth = Params->TryGetNumberField(TEXT("width"), WidthD);
	double DepthD = 0.0;
	const bool bHasDepth = Params->TryGetNumberField(TEXT("depth"), DepthD);
	if (!bHasWidth && !bHasDepth)
	{
		return mcp_common::MakeErrorReply(TEXT("Provide width, depth or both: there is nothing to set; nothing was changed"));
	}
	// The spline sync raises a width or depth below UE_KINDA_SMALL_NUMBER to it (the spline scale must stay positive
	// for collision), so that is the value the point ends up holding and the one compared and written.
	const float Width = FMath::Max(static_cast<float>(WidthD), UE_KINDA_SMALL_NUMBER);
	const float Depth = FMath::Max(static_cast<float>(DepthD), UE_KINDA_SMALL_NUMBER);

	// Both curves are checked before anything is written.
	FString KeyError = bHasWidth ? Water_CurveKeyError(Metadata->RiverWidth, InputKey, TEXT("river width")) : FString();
	if (KeyError.IsEmpty() && bHasDepth)
	{
		KeyError = Water_CurveKeyError(Metadata->Depth, InputKey, TEXT("depth"));
	}
	if (!KeyError.IsEmpty())
	{
		return mcp_common::MakeErrorReply(KeyError);
	}

	// Only the metadata is recorded, before the first write, as the editor's spline point details do: the call is one
	// undo step that dirties the actor's package, and undoing it re-synchronizes the spline from the restored values.
	// A value the point already holds is not written, so a call that changes nothing dirties nothing.
	const bool bWriteWidth = bHasWidth && Water_CurveKeyDiffers(Metadata->RiverWidth, InputKey, Width);
	const bool bWriteDepth = bHasDepth && Water_CurveKeyDiffers(Metadata->Depth, InputKey, Depth);
	const bool bApplied = bWriteWidth || bWriteDepth;
	if (bApplied)
	{
		Metadata->Modify();
		if (bWriteWidth)
		{
			River->SetRiverWidthAtSplineInputKey(InputKey, Width);
		}
		if (bWriteDepth)
		{
			River->SetRiverDepthAtSplineInputKey(InputKey, Depth);
		}
		// Push the new values into the spline scale and rebuild the river (same idiom as set_river_widths_depths).
		Spline->K2_SynchronizeAndBroadcastDataChange();
	}

	// The read-back is the point the setters write, not the curve evaluated at the key, which interpolates between
	// points; after the sync every curve has one point per spline point, at key i for point i.
	const int32 PointIndex = Water_CurvePointIndex(bHasWidth ? Metadata->RiverWidth : Metadata->Depth, InputKey);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("water_body_label"), BodyLabel);
	Data->SetNumberField(TEXT("input_key"), InputKey);
	Data->SetNumberField(TEXT("point_index"), PointIndex);
	Data->SetNumberField(TEXT("width"), Water_CurvePointValue(Metadata->RiverWidth, PointIndex, InputKey));
	Data->SetNumberField(TEXT("depth"), Water_CurvePointValue(Metadata->Depth, PointIndex, InputKey));
	Data->SetBoolField(TEXT("applied"), bApplied);
	if (!bApplied)
	{
		Data->SetStringField(TEXT("note"), TEXT("The spline point already holds these values; nothing was changed"));
	}
	return mcp_common::MakeOkReply(Data);
}

// MCP:set_river_widths_depths - Set the width and/or depth of every spline point of a river water body, found by actor label: the arrays are indexed by point (point i at key i), each given one with point_count entries. A value below 0.0001 is stored as 0.0001, as the spline sync does. The points are written in one undo step dirtying the actor's package, then the spline is synchronized once to rebuild the river; nothing is saved. widths and depths are read back from the points; applied_widths or applied_depths is true when that array changed a point and every point holds it, and success is false when one does not. Arrays already held change nothing: both false, a note, no undo step. Neither array, a wrong length, a non-river body or an unknown label is an error. One point: set_river_width_depth_at_point.
//   @param water_body_label - Label of the AWaterBodyRiver actor
//   @param widths - Optional array of per-point river widths (length must equal point_count)
//   @param depths - Optional array of per-point river depths (length must equal point_count)
//   @returns { water_body_label: string, point_count: int, applied_widths: bool, applied_depths: bool, widths: [number], depths: [number], note: string? }
TSharedPtr<FJsonObject> HandleSetRiverWidthsDepths(
	const TSharedPtr<FJsonObject>& Params)
{
	FString BodyLabel;
	TSharedPtr<FJsonObject> Error;
	AWaterBody* Body = Water_FindBodyParam(Params, BodyLabel, Error);
	if (!Body)
	{
		return Error;
	}
	UWaterBodyRiverComponent* River = Cast<UWaterBodyRiverComponent>(Body->GetWaterBodyComponent());
	if (!River)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("%s is not a river water body"), *BodyLabel));
	}
	UWaterSplineComponent* Spline = Body->GetWaterSpline();
	if (!Spline) { return mcp_common::MakeErrorReply(TEXT("Body has no water spline")); }
	const int32 PointCount = Spline->GetNumberOfSplinePoints();

	const TArray<TSharedPtr<FJsonValue>>* WidthsArr = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* DepthsArr = nullptr;
	const bool bHasWidths = Params->TryGetArrayField(TEXT("widths"), WidthsArr) && WidthsArr;
	const bool bHasDepths = Params->TryGetArrayField(TEXT("depths"), DepthsArr) && DepthsArr;
	if (!bHasWidths && !bHasDepths)
	{
		return mcp_common::MakeErrorReply(TEXT("Provide at least one of 'widths' or 'depths' (arrays of length point_count)"));
	}
	if (bHasWidths && WidthsArr->Num() != PointCount)
	{
		return mcp_common::MakeErrorReply(FString::Printf(
			TEXT("'widths' has %d entries but the river has %d spline points"), WidthsArr->Num(), PointCount));
	}
	if (bHasDepths && DepthsArr->Num() != PointCount)
	{
		return mcp_common::MakeErrorReply(FString::Printf(
			TEXT("'depths' has %d entries but the river has %d spline points"), DepthsArr->Num(), PointCount));
	}
	// The engine's setters write the per-point curves of the component's water spline metadata; an empty curve, or
	// one whose first point is past key 0, would make them write out of range, so both are refused up front.
	UWaterSplineMetadata* Metadata = River->GetWaterSplineMetadata();
	if (!Metadata)
	{
		return mcp_common::MakeErrorReply(FString::Printf(TEXT("%s has no water spline metadata"), *BodyLabel));
	}
	if (PointCount > 0)
	{
		FString KeyError = bHasWidths ? Water_CurveKeyError(Metadata->RiverWidth, 0.0f, TEXT("river width")) : FString();
		if (KeyError.IsEmpty() && bHasDepths)
		{
			KeyError = Water_CurveKeyError(Metadata->Depth, 0.0f, TEXT("depth"));
		}
		if (!KeyError.IsEmpty())
		{
			return mcp_common::MakeErrorReply(KeyError);
		}
	}

	// The spline sync raises a width or depth below UE_KINDA_SMALL_NUMBER to it (the spline scale must stay positive
	// for collision), so that is the value each point ends up holding and the one compared, written and checked.
	auto Wanted = [](const TArray<TSharedPtr<FJsonValue>>* Values, int32 Index)
	{
		return FMath::Max(static_cast<float>((*Values)[Index]->AsNumber()), UE_KINDA_SMALL_NUMBER);
	};

	// Only the metadata is recorded, before the first write, as the editor's spline point details do: the call is one
	// undo step dirtying the actor's package, and undoing it re-synchronizes the spline from the restored values.
	// Arrays equal to what the points already hold write nothing, so that call dirties nothing and adds no undo step.
	bool bWidthsChange = false;
	bool bDepthsChange = false;
	for (int32 i = 0; i < PointCount; ++i)
	{
		const float InputKey = static_cast<float>(i);
		bWidthsChange |= bHasWidths && Water_CurveKeyDiffers(Metadata->RiverWidth, InputKey, Wanted(WidthsArr, i));
		bDepthsChange |= bHasDepths && Water_CurveKeyDiffers(Metadata->Depth, InputKey, Wanted(DepthsArr, i));
	}
	if (bWidthsChange || bDepthsChange)
	{
		Metadata->Modify();
		for (int32 i = 0; i < PointCount; ++i)
		{
			const float InputKey = static_cast<float>(i);
			if (bWidthsChange)
			{
				River->SetRiverWidthAtSplineInputKey(InputKey, Wanted(WidthsArr, i));
			}
			if (bDepthsChange)
			{
				River->SetRiverDepthAtSplineInputKey(InputKey, Wanted(DepthsArr, i));
			}
		}
		// One metadata sync after the whole batch (same propagate idiom as set_water_body_spline_points).
		Spline->K2_SynchronizeAndBroadcastDataChange();
	}

	// The read-back is the value each point holds after the sync, not the request: an array counts as applied only
	// when it changed a point and every point now holds it.
	TArray<TSharedPtr<FJsonValue>> WidthsOut;
	TArray<TSharedPtr<FJsonValue>> DepthsOut;
	FString Mismatch;
	for (int32 i = 0; i < PointCount; ++i)
	{
		const float InputKey = static_cast<float>(i);
		const float WidthHeld = Water_CurvePointValue(Metadata->RiverWidth, Water_CurvePointIndex(Metadata->RiverWidth, InputKey), InputKey);
		const float DepthHeld = Water_CurvePointValue(Metadata->Depth, Water_CurvePointIndex(Metadata->Depth, InputKey), InputKey);
		WidthsOut.Add(MakeShared<FJsonValueNumber>(WidthHeld));
		DepthsOut.Add(MakeShared<FJsonValueNumber>(DepthHeld));
		if (Mismatch.IsEmpty() && bHasWidths && WidthHeld != Wanted(WidthsArr, i))
		{
			Mismatch = FString::Printf(TEXT("point %d holds width %g, not %g"), i, WidthHeld, Wanted(WidthsArr, i));
		}
		if (Mismatch.IsEmpty() && bHasDepths && DepthHeld != Wanted(DepthsArr, i))
		{
			Mismatch = FString::Printf(TEXT("point %d holds depth %g, not %g"), i, DepthHeld, Wanted(DepthsArr, i));
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("water_body_label"), BodyLabel);
	Data->SetNumberField(TEXT("point_count"), PointCount);
	Data->SetBoolField(TEXT("applied_widths"), bWidthsChange && Mismatch.IsEmpty());
	Data->SetBoolField(TEXT("applied_depths"), bDepthsChange && Mismatch.IsEmpty());
	Data->SetArrayField(TEXT("widths"), WidthsOut);
	Data->SetArrayField(TEXT("depths"), DepthsOut);
	if (!Mismatch.IsEmpty())
	{
		return mcp_common::MakeOutcomeReply(Data, false, FString::Printf(
			TEXT("set_river_widths_depths: after the write and the spline sync %s; widths and depths carry what the points hold"),
			*Mismatch));
	}
	if (!bWidthsChange && !bDepthsChange)
	{
		Data->SetStringField(TEXT("note"), TEXT("The spline points already hold these values; nothing was changed"));
	}
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// Gerstner waves authoring + wave info read-back + exclusion volumes
// ---------------------------------------------------------------------------

// MCP:set_water_body_gerstner_waves - Build a Gerstner wave set (UGerstnerWaterWaves + simple parametric generator) and apply it to a wave-supporting water body (Lake/Ocean; rivers are rejected). The generator ramps per-wave Steepness from large_wave_steepness to small_wave_steepness; the no-self-intersection (no fold, no black-crest) criterion is total_steepness (returned) <= 1 — lower the two steepness params and/or num_waves if it exceeds 1.
//   @param water_body_label - Label of the AWaterBody actor (must support waves: Lake or Ocean)
//   @param num_waves - Optional wave count for the generator (>= 1; default 16)
//   @param min_wavelength - Optional minimum wavelength in uu
//   @param max_wavelength - Optional maximum wavelength in uu
//   @param min_amplitude - Optional minimum wave amplitude in uu
//   @param max_amplitude - Optional maximum wave amplitude in uu
//   @param wind_angle - Optional dominant wind angle in degrees
//   @param small_wave_steepness - Optional steepness of the smallest wave (generator default 0.4)
//   @param large_wave_steepness - Optional steepness of the largest wave (generator default 0.2)
//   @param steepness_falloff - Optional exponent shaping the small->large steepness ramp (generator default 1.0)
//   @returns { water_body_label: string, wave_supported: bool, has_waves: bool, num_waves: int, max_wave_height: number, total_steepness: number, applied: bool }
TSharedPtr<FJsonObject> HandleSetWaterBodyGerstnerWaves(
	const TSharedPtr<FJsonObject>& Params)
{
	AWaterBody* Body = nullptr; TSharedPtr<FJsonObject> Err;
	UWaterBodyComponent* Comp = Water_ResolveBodyComponent(Params, /*bRequireWaveSupport*/ true, Body, Err);
	if (!Comp) { return Err; }

	// Build the waves object + its simple parametric generator as instanced
	// subobjects, mirroring the editor's "Gerstner Water Waves" source.
	UGerstnerWaterWaves* Waves = NewObject<UGerstnerWaterWaves>(Body);
	UGerstnerWaterWaveGeneratorSimple* Generator = NewObject<UGerstnerWaterWaveGeneratorSimple>(Waves);

	double Tmp = 0.0;
	if (Params->TryGetNumberField(TEXT("num_waves"), Tmp) && Tmp >= 1.0) { Generator->NumWaves = static_cast<int32>(Tmp); }
	if (Params->TryGetNumberField(TEXT("min_wavelength"), Tmp)) { Generator->MinWavelength = static_cast<float>(Tmp); }
	if (Params->TryGetNumberField(TEXT("max_wavelength"), Tmp)) { Generator->MaxWavelength = static_cast<float>(Tmp); }
	if (Params->TryGetNumberField(TEXT("min_amplitude"), Tmp)) { Generator->MinAmplitude = static_cast<float>(Tmp); }
	if (Params->TryGetNumberField(TEXT("max_amplitude"), Tmp)) { Generator->MaxAmplitude = static_cast<float>(Tmp); }
	if (Params->TryGetNumberField(TEXT("wind_angle"), Tmp)) { Generator->WindAngleDeg = static_cast<float>(Tmp); }
	// The generator ramps Steepness from LargeWaveSteepness to SmallWaveSteepness
	// (GerstnerWaterWaves.cpp: Lerp(Large, Small, Pow(i/NumWaves, Falloff))); at the
	// engine defaults (Large=0.2, Small=0.4) that sum blows well past the Sigma<=1
	// no-fold limit once NumWaves is more than a handful. Expose the 3 UPROPERTY the
	// generator already has for this — no engine-side clamp exists, it's the caller's
	// responsibility, verified in GerstnerWaterWaves.h/.cpp.
	if (Params->TryGetNumberField(TEXT("small_wave_steepness"), Tmp)) { Generator->SmallWaveSteepness = static_cast<float>(Tmp); }
	if (Params->TryGetNumberField(TEXT("large_wave_steepness"), Tmp)) { Generator->LargeWaveSteepness = static_cast<float>(Tmp); }
	if (Params->TryGetNumberField(TEXT("steepness_falloff"), Tmp)) { Generator->SteepnessFalloff = static_cast<float>(Tmp); }

	ApplyGerstnerWaves(Body, Waves, Generator);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("water_body_label"), Body->GetActorLabel());
	FillWaveState(Data, Comp, Waves);
	return mcp_common::MakeOkReply(Data);
}

// MCP:create_water_body_exclusion_volume - Spawn an AWaterBodyExclusionVolume (a brush volume that excludes water bodies), build its cube brush, set the exclusion mode, and optionally bind specific water bodies by label.
//   @param actor_label - Label to assign to the new exclusion volume actor
//   @param location - Optional [X, Y, Z] world location (defaults to origin)
//   @param size - Optional cube brush size in uu (defaults to 200)
//   @param exclusion_mode - Optional "remove" (default) or "add": whether water_body_labels are removed from or added to the exclusion
//   @param water_body_labels - Optional array of AWaterBody labels to bind into the volume's WaterBodies list
//   @returns { actor_label: string, actor_name: string, exclusion_mode: string, water_body_count: int, brush_built: bool }
TSharedPtr<FJsonObject> HandleCreateWaterBodyExclusionVolume(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ActorLabel;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_label"), ActorLabel)) { return Missing; }
	UWorld* World = nullptr;
	if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World)) { return NoWorld; }

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWaterBodyExclusionVolume* Volume = World->SpawnActor<AWaterBodyExclusionVolume>(
		AWaterBodyExclusionVolume::StaticClass(), ReadLocationOrZero(Params), FRotator::ZeroRotator, SpawnParams);
	if (!Volume)
	{
		return mcp_common::MakeErrorReply(TEXT("Failed to spawn AWaterBodyExclusionVolume"));
	}
	Volume->SetActorLabel(ActorLabel);

	// A freshly spawned brush volume has no geometry (zero bounds, no collision).
	// Build a default cube brush the same way the editor's volume factory does.
	bool bBrushBuilt = false;
	if (UCubeBuilder* Builder = NewObject<UCubeBuilder>())
	{
		double Size = 0.0;
		if (Params->TryGetNumberField(TEXT("size"), Size) && Size > 0.0)
		{
			Builder->X = static_cast<float>(Size);
			Builder->Y = static_cast<float>(Size);
			Builder->Z = static_cast<float>(Size);
		}
		UActorFactory::CreateBrushForVolumeActor(Volume, Builder);
		bBrushBuilt = true;
	}

	// Exclusion mode (engine default = remove the listed bodies from exclusion).
	FString ModeStr;
	Params->TryGetStringField(TEXT("exclusion_mode"), ModeStr);
	Volume->ExclusionMode = ModeStr.Equals(TEXT("add"), ESearchCase::IgnoreCase)
		? EWaterExclusionMode::AddWaterBodiesListToExclusion
		: EWaterExclusionMode::RemoveWaterBodiesListFromExclusion;

	// Bind explicit water bodies into the volume's list (resolved by actor label).
	int32 BoundCount = 0;
	const TArray<TSharedPtr<FJsonValue>>* LabelsArr = nullptr;
	if (Params->TryGetArrayField(TEXT("water_body_labels"), LabelsArr) && LabelsArr)
	{
		for (const TSharedPtr<FJsonValue>& V : *LabelsArr)
		{
			FString Label;
			if (V.IsValid() && V->TryGetString(Label) && !Label.IsEmpty())
			{
				if (AWaterBody* Bound = Cast<AWaterBody>(mcp_common::FindActorByLabel(World, Label)))
				{
					Volume->WaterBodies.Add(Bound);
					++BoundCount;
				}
			}
		}
	}

	// Finalize: recompute which water bodies this volume affects.
	Volume->UpdateOverlappingWaterBodies(FWaterExclusionVolumeChangedParams());

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_label"), Volume->GetActorLabel());
	Data->SetStringField(TEXT("actor_name"), Volume->GetName());
	Data->SetStringField(TEXT("exclusion_mode"),
		Volume->ExclusionMode == EWaterExclusionMode::AddWaterBodiesListToExclusion ? TEXT("add") : TEXT("remove"));
	Data->SetNumberField(TEXT("water_body_count"), BoundCount);
	Data->SetBoolField(TEXT("brush_built"), bBrushBuilt);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// Per-wave detail read-back
// ---------------------------------------------------------------------------

// MCP:get_water_body_gerstner_wave_detail - Read the individual Gerstner waves a water body holds (found by actor label in the editor world), as last generated and cached on its waves object: per wave the wavelength and amplitude in cm, the steepness, and the horizontal direction vector. A body without Gerstner waves -- a river, a body with another waves class, or one whose generator produced none -- is a success with wave_count 0 and an empty list. Read-only; an unknown label is an error. For the summary (waves class, count, max height) use get_water_body_info; author waves with set_water_body_gerstner_waves.
//   @param water_body_label - Actor label of the AWaterBody in the editor world (case-insensitive; the object name does not match).
//   @returns { water_body_label: string, wave_count: int, waves: [{ wavelength: number, amplitude: number, steepness: number, direction: [number, number, number] }] }
TSharedPtr<FJsonObject> HandleGetWaterBodyGerstnerWaveDetail(
	const TSharedPtr<FJsonObject>& Params)
{
	AWaterBody* Body = nullptr; TSharedPtr<FJsonObject> Err;
	UWaterBodyComponent* Comp = Water_ResolveBodyComponent(Params, /*bRequireWaveSupport*/ false, Body, Err);
	if (!Comp) { return Err; }

	TArray<TSharedPtr<FJsonValue>> WavesJson;
	if (const UGerstnerWaterWaves* Gerstner = Cast<UGerstnerWaterWaves>(Comp->GetWaterWaves()))
	{
		for (const FGerstnerWave& W : Gerstner->GetGerstnerWaves())
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetNumberField(TEXT("wavelength"), W.WaveLength);
			Entry->SetNumberField(TEXT("amplitude"), W.Amplitude);
			Entry->SetNumberField(TEXT("steepness"), W.Steepness);
			Entry->SetArrayField(TEXT("direction"), mcp_common::VectorToJsonArray(W.Direction));
			WavesJson.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("water_body_label"), Body->GetActorLabel());
	Data->SetNumberField(TEXT("wave_count"), WavesJson.Num());
	Data->SetArrayField(TEXT("waves"), WavesJson);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// Multi-property partial-update with integrated read-back
// ---------------------------------------------------------------------------
namespace
{
	/** Per-field result: echo of the written value + whether it already matched. */
	void EmitFloatField(const TSharedPtr<FJsonObject>& Out, const TCHAR* Key, float Value, bool bAlreadySet)
	{
		TSharedPtr<FJsonObject> Field = MakeShared<FJsonObject>();
		Field->SetNumberField(TEXT("value"), Value);
		Field->SetBoolField(TEXT("already_set"), bAlreadySet);
		Out->SetObjectField(Key, Field);
	}

	void EmitBoolField(const TSharedPtr<FJsonObject>& Out, const TCHAR* Key, bool Value, bool bAlreadySet)
	{
		TSharedPtr<FJsonObject> Field = MakeShared<FJsonObject>();
		Field->SetBoolField(TEXT("value"), Value);
		Field->SetBoolField(TEXT("already_set"), bAlreadySet);
		Out->SetObjectField(Key, Field);
	}

	/**
	 * Send Comp the change notification the editor's details panel sends after it sets the UWaterBodyComponent field
	 * MemberName (or the InnerName field of that struct member): the component's PostEditChangeProperty, which applies
	 * the property's own follow-up and rebroadcasts the body change that rebuilds the water mesh and the landscape
	 * brush. PreEditChange is not sent: on a component it re-registers it and reruns the actor's construction script.
	 */
	void Water_NotifyComponentChange(UWaterBodyComponent* Comp, FName MemberName, FName InnerName = NAME_None)
	{
		FProperty* MemberProperty = FindFProperty<FProperty>(UWaterBodyComponent::StaticClass(), MemberName);
		FProperty* ChangedProperty = MemberProperty;
		if (!InnerName.IsNone())
		{
			const FStructProperty* StructProperty = CastField<FStructProperty>(MemberProperty);
			ChangedProperty = StructProperty ? FindFProperty<FProperty>(StructProperty->Struct, InnerName) : nullptr;
		}
		FPropertyChangedEvent Event(ChangedProperty, EPropertyChangeType::ValueSet);
		Event.SetActiveMemberProperty(MemberProperty);
		// UWaterBodyComponent declares its override protected; the public UObject declaration dispatches to it.
		static_cast<UObject*>(Comp)->PostEditChangeProperty(Event);
	}

	/**
	 * Write Value into Member, calling BeforeWrite first and AfterWrite after, unless Member already holds it exactly;
	 * then report it in Fields under Key (with whether Member already held it) and in Applied.
	 */
	void Water_ApplyFloat(float& Member, float Value, const TCHAR* Key, const TSharedPtr<FJsonObject>& Fields,
		TArray<TSharedPtr<FJsonValue>>& Applied, TFunctionRef<void()> BeforeWrite, TFunctionRef<void()> AfterWrite)
	{
		// The same exact comparison decides already_set and the write, so already_set is true only when nothing was written.
		const bool bAlready = (Member == Value);
		if (!bAlready)
		{
			BeforeWrite();
			Member = Value;
			AfterWrite();
		}
		EmitFloatField(Fields, Key, Member, bAlready);
		Applied.Add(MakeShared<FJsonValueString>(Key));
	}
}

// MCP:set_water_body_properties - Set only the given settings of a lake, ocean or river water body, found by actor label in the editor world, reading each back with already_set; nothing is saved. Use it over set_properties for water_velocity/audio_intensity, which live per spline point: they go to the point at or before input_key, are read back from it and synchronize the spline. water_height_offset is ocean-only; the ocean's setter clamps it to >= 0 and rebuilds the body. Every other field that changes gets the editor's change notification, updating the water mesh and landscape. All checks run before any write: a spline value without input_key, a key before the first spline point, water_height_offset on a non-ocean body or an unknown label is an error that changes nothing. A value held exactly is not written (already_set true); otherwise the call is one undo step dirtying the actor's package. River width/depth: set_river_width_depth_at_point; waves: set_water_body_gerstner_waves.
//   @param water_body_label [!] - Label of the AWaterBody actor (Lake/Ocean/River)
//   @param water_velocity [number] - River/spline water velocity to set at input_key (requires input_key)
//   @param audio_intensity [number] - Spline audio intensity to set at input_key (requires input_key)
//   @param input_key [number] - Spline input key for water_velocity/audio_intensity (required only when either is set); a key before the first spline point (below 0) is an error and nothing is changed
//   @param target_wave_mask_depth [number] - Water depth at which waves start being attenuated
//   @param max_wave_height_offset [number] - Offset added to the automatically calculated max wave height bounds
//   @param collision_height_offset [number] - Distance above the water surface where collision checks still occur
//   @param shape_dilation [number] - Shape dilation in uu
//   @param affects_landscape [bool] - Whether the landscape is deformed by this water body
//   @param post_process_blend_weight [number] - Underwater post-process blend weight (0..1)
//   @param water_height_offset [number] - Ocean-only additional water height (errors on non-ocean bodies)
//   @returns { water_body_label: string, body_type: string, applied: [string], fields: { <name>: { value, already_set } } }
TSharedPtr<FJsonObject> HandleSetWaterBodyProperties(
	const TSharedPtr<FJsonObject>& Params)
{
	// Resolver: label -> AWaterBody -> concrete UWaterBodyComponent (no spawn; the
	// base AWaterBody is Abstract but the actor is already a Lake/Ocean/River).
	AWaterBody* Body = nullptr;
	TSharedPtr<FJsonObject> Error;
	UWaterBodyComponent* Comp = Water_ResolveBodyComponent(Params, /*bRequireWaveSupport*/ false, Body, Error);
	if (!Comp)
	{
		return Error;
	}

	// --- Validation: every check runs before the first write, so a refused call changes nothing ---------
	// water_velocity / audio_intensity are written into the component's water spline metadata at input_key; the
	// engine's setters write the point at or before the key, which a key before the first point puts out of range.
	const bool bWantsVelocity = Params->HasField(TEXT("water_velocity"));
	const bool bWantsAudio    = Params->HasField(TEXT("audio_intensity"));
	UWaterSplineMetadata* Metadata = Comp->GetWaterSplineMetadata();
	float InputKey = 0.0f;
	// input_key is read unconditionally so that only the two spline fields need it: the other fields work without it.
	double InputKeyD = 0.0;
	const bool bHasInputKey = Params->TryGetNumberField(TEXT("input_key"), InputKeyD);
	if (bWantsVelocity || bWantsAudio)
	{
		if (!bHasInputKey)
		{
			return mcp_common::MakeErrorReply(
				TEXT("input_key is required when setting water_velocity or audio_intensity"));
		}
		InputKey = static_cast<float>(InputKeyD);
		if (!Metadata)
		{
			return mcp_common::MakeErrorReply(FString::Printf(
				TEXT("%s has no water spline metadata to hold water_velocity or audio_intensity"), *Body->GetActorLabel()));
		}
		FString KeyError = bWantsVelocity
			? Water_CurveKeyError(Metadata->WaterVelocityScalar, InputKey, TEXT("water velocity")) : FString();
		if (KeyError.IsEmpty() && bWantsAudio)
		{
			KeyError = Water_CurveKeyError(Metadata->AudioIntensity, InputKey, TEXT("audio intensity"));
		}
		if (!KeyError.IsEmpty())
		{
			return mcp_common::MakeErrorReply(KeyError);
		}
	}
	// The base UWaterBodyComponent::SetHeightOffset is a stub `{ check(false); }`;
	// only UWaterBodyOceanComponent overrides it (clamps >= 0, fires OnWaterBodyChanged).
	UWaterBodyOceanComponent* Ocean = nullptr;
	if (Params->HasField(TEXT("water_height_offset")))
	{
		Ocean = Cast<UWaterBodyOceanComponent>(Comp);
		if (!Ocean)
		{
			return mcp_common::MakeErrorReply(
				TEXT("water_height_offset only applies to ocean water bodies"));
		}
	}

	TSharedPtr<FJsonObject> Fields = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Applied;
	auto MarkApplied = [&Applied](const TCHAR* Key) { Applied.Add(MakeShared<FJsonValueString>(Key)); };

	// Each object is recorded once, right before its first write that changes a value: the call is one undo step
	// and dirties the actor's package, and a call that only repeats the current values dirties nothing.
	bool bCompRecorded = false;
	auto RecordComp = [Comp, &bCompRecorded]()
	{
		if (!bCompRecorded)
		{
			Comp->Modify();
			bCompRecorded = true;
		}
	};
	bool bMetadataRecorded = false;
	auto RecordMetadata = [Metadata, &bMetadataRecorded]()
	{
		if (!bMetadataRecorded)
		{
			Metadata->Modify();
			bMetadataRecorded = true;
		}
	};

	// --- Group A: spline values (water_velocity / audio_intensity) ---------
	// The engine's setters write the spline point at or before input_key, so that point is what is compared, written
	// and read back: the getters evaluate the curve at the key, which interpolates between points.
	double VelocityD = 0.0;
	Params->TryGetNumberField(TEXT("water_velocity"), VelocityD);
	double AudioD = 0.0;
	Params->TryGetNumberField(TEXT("audio_intensity"), AudioD);
	const float WantVelocity = static_cast<float>(VelocityD);
	const float WantAudio = static_cast<float>(AudioD);
	const bool bVelocityAlready = bWantsVelocity && !Water_CurveKeyDiffers(Metadata->WaterVelocityScalar, InputKey, WantVelocity);
	const bool bAudioAlready = bWantsAudio && !Water_CurveKeyDiffers(Metadata->AudioIntensity, InputKey, WantAudio);
	if (bWantsVelocity && !bVelocityAlready)
	{
		RecordMetadata();
		Comp->SetWaterVelocityAtSplineInputKey(InputKey, WantVelocity);
	}
	if (bWantsAudio && !bAudioAlready)
	{
		RecordMetadata();
		Comp->SetAudioIntensityAtSplineInputKey(InputKey, WantAudio);
	}
	if (bMetadataRecorded)
	{
		// Push the change through the spline, as the editor's spline point details do, so the body rebuilds.
		if (UWaterSplineComponent* Spline = Body->GetWaterSpline())
		{
			Spline->K2_SynchronizeAndBroadcastDataChange();
		}
	}
	if (bWantsVelocity)
	{
		const int32 PointIndex = Water_CurvePointIndex(Metadata->WaterVelocityScalar, InputKey);
		EmitFloatField(Fields, TEXT("water_velocity"),
			Water_CurvePointValue(Metadata->WaterVelocityScalar, PointIndex, InputKey), bVelocityAlready);
		MarkApplied(TEXT("water_velocity"));
	}
	if (bWantsAudio)
	{
		const int32 PointIndex = Water_CurvePointIndex(Metadata->AudioIntensity, InputKey);
		EmitFloatField(Fields, TEXT("audio_intensity"),
			Water_CurvePointValue(Metadata->AudioIntensity, PointIndex, InputKey), bAudioAlready);
		MarkApplied(TEXT("audio_intensity"));
	}

	// --- Group B: public UPROPERTY data members (last-write-wins) ------------
	// Each field that changes gets the component's change notification right after its write, as the details panel
	// sends it, so the water mesh, the zone and the landscape brush update.
	{
		double D = 0.0;
		if (Params->TryGetNumberField(TEXT("target_wave_mask_depth"), D))
		{
			Water_ApplyFloat(Comp->TargetWaveMaskDepth, static_cast<float>(D), TEXT("target_wave_mask_depth"), Fields, Applied,
				RecordComp, [Comp]() { Water_NotifyComponentChange(Comp, GET_MEMBER_NAME_CHECKED(UWaterBodyComponent, TargetWaveMaskDepth)); });
		}
		if (Params->TryGetNumberField(TEXT("max_wave_height_offset"), D))
		{
			Water_ApplyFloat(Comp->MaxWaveHeightOffset, static_cast<float>(D), TEXT("max_wave_height_offset"), Fields, Applied,
				RecordComp, [Comp]() { Water_NotifyComponentChange(Comp, GET_MEMBER_NAME_CHECKED(UWaterBodyComponent, MaxWaveHeightOffset)); });
		}
		if (Params->TryGetNumberField(TEXT("collision_height_offset"), D))
		{
			Water_ApplyFloat(Comp->CollisionHeightOffset, static_cast<float>(D), TEXT("collision_height_offset"), Fields, Applied,
				RecordComp, [Comp]() { Water_NotifyComponentChange(Comp, GET_MEMBER_NAME_CHECKED(UWaterBodyComponent, CollisionHeightOffset)); });
		}
		if (Params->TryGetNumberField(TEXT("shape_dilation"), D))
		{
			Water_ApplyFloat(Comp->ShapeDilation, static_cast<float>(D), TEXT("shape_dilation"), Fields, Applied,
				RecordComp, [Comp]() { Water_NotifyComponentChange(Comp, GET_MEMBER_NAME_CHECKED(UWaterBodyComponent, ShapeDilation)); });
		}
		if (Params->TryGetNumberField(TEXT("post_process_blend_weight"), D))
		{
			Water_ApplyFloat(Comp->UnderwaterPostProcessSettings.BlendWeight, static_cast<float>(D),
				TEXT("post_process_blend_weight"), Fields, Applied, RecordComp, [Comp]()
				{
					Water_NotifyComponentChange(Comp, GET_MEMBER_NAME_CHECKED(UWaterBodyComponent, UnderwaterPostProcessSettings),
						GET_MEMBER_NAME_CHECKED(FUnderwaterPostProcessSettings, BlendWeight));
				});
		}
		bool B = false;
		if (Params->TryGetBoolField(TEXT("affects_landscape"), B))
		{
			const bool bAlready = (Comp->bAffectsLandscape == B);
			if (!bAlready)
			{
				RecordComp();
				Comp->bAffectsLandscape = B;
				Water_NotifyComponentChange(Comp, GET_MEMBER_NAME_CHECKED(UWaterBodyComponent, bAffectsLandscape));
			}
			EmitBoolField(Fields, TEXT("affects_landscape"), Comp->bAffectsLandscape, bAlready);
			MarkApplied(TEXT("affects_landscape"));
		}
	}

	// --- Ocean-only: water_height_offset (HONEST gate, checked above) -------
	bool bOceanNote = false;
	if (Ocean)
	{
		double D = 0.0; Params->TryGetNumberField(TEXT("water_height_offset"), D);
		const float Want = FMath::Max(0.0f, static_cast<float>(D)); // mirror the engine clamp for the already_set check
		// The setter itself rebuilds the body; the same exact comparison decides already_set and the write.
		const bool bAlready = (Ocean->GetHeightOffset() == Want);
		if (!bAlready)
		{
			RecordComp();
			Ocean->SetHeightOffset(static_cast<float>(D));
		}
		EmitFloatField(Fields, TEXT("water_height_offset"), Ocean->GetHeightOffset(), bAlready);
		MarkApplied(TEXT("water_height_offset"));
		bOceanNote = true;
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("water_body_label"), Body->GetActorLabel());
	Data->SetStringField(TEXT("body_type"), WaterBodyTypeToString(Body->GetWaterBodyType()));
	Data->SetArrayField(TEXT("applied"), Applied);
	Data->SetObjectField(TEXT("fields"), Fields);
	if (bOceanNote)
	{
		Data->SetStringField(TEXT("note"),
			TEXT("height_offset regenerates the ocean dynamic mesh; without an active WaterZone ")
			TEXT("the visual effect is not materialized, the value is persisted"));
	}
	return mcp_common::MakeOkReply(Data);
}

const McpCommandFamilies::FRegistrar Registration =
{
	{ TEXT("add_water_body_to_zone"), &HandleAddWaterBodyToZone },
	{ TEXT("configure_water_zone"), &HandleConfigureWaterZone },
	{ TEXT("create_lake_water_body"), &HandleCreateLakeWaterBody },
	{ TEXT("create_ocean_water_body"), &HandleCreateOceanWaterBody },
	{ TEXT("create_river_water_body"), &HandleCreateRiverWaterBody },
	{ TEXT("create_water_body_exclusion_volume"), &HandleCreateWaterBodyExclusionVolume },
	{ TEXT("create_water_zone"), &HandleCreateWaterZone },
	{ TEXT("get_water_body_spline_points"), &HandleGetRiverSplinePoints },
	{ TEXT("get_water_body_gerstner_wave_detail"), &HandleGetWaterBodyGerstnerWaveDetail },
	{ TEXT("get_water_body_info"), &HandleGetWaterBodyInfo },
	{ TEXT("get_water_zone_info"), &HandleGetWaterZoneInfo },
	{ TEXT("remove_water_body_from_zone"), &HandleRemoveWaterBodyFromZone },
	{ TEXT("set_water_body_spline_points"), &HandleSetRiverSplinePoints },
	{ TEXT("set_river_width_depth_at_point"), &HandleSetRiverWidthDepthAtPoint },
	{ TEXT("set_river_widths_depths"), &HandleSetRiverWidthsDepths },
	{ TEXT("set_water_body_gerstner_waves"), &HandleSetWaterBodyGerstnerWaves },
	{ TEXT("set_water_body_material"), &HandleSetWaterBodyMaterial },
	{ TEXT("set_water_body_properties"), &HandleSetWaterBodyProperties },
};
} // namespace UEAgentWaterCommands
