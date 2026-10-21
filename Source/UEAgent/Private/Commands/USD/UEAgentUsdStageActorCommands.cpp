// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

// GEditor + GetEditorWorldContext (editor-only). UnrealEd module is in
// PrivateDependencyModuleNames so this is safe in the .cpp; UEAgent itself
// is an Editor plugin so GEditor is always defined in this translation unit.
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"               // TActorIterator (for ResolveStageActor fallback)
#include "GameFramework/Actor.h"

// USDStage module — AUsdStageActor, IUsdStageModule, EUsdStageState.
// IMPORTANT: USDStage must be in UEAgent.Build.cs (PublicDependencyModuleNames or
// PrivateDependencyModuleNames) for the linker to resolve the USDSTAGE_API symbols.
// If USDStage is absent from Build.cs the linker will fail with
// unresolved USDSTAGE_API symbols.
#include "USDStageActor.h"
#include "USDStageModule.h"

// USD wrappers. USDUtilities / UnrealUSDWrapper are already linked in Build.cs.
#include "UnrealUSDWrapper.h"
#include "UsdWrappers/UsdStage.h"
#include "UsdWrappers/SdfLayer.h"
#include "Commands/USD/UEAgentUsdRegistry.h"  // helpers shared by the USD handlers (USD error capture)

/**
 * USD stage-actor commands.
 *
 * Wraps the actor-side USD workflow around `AUsdStageActor` (the USDStage module's
 * primary UObject bridge between UE and a live USD stage).
 *
 * Engine APIs:
 *   - AUsdStageActor::SetRootLayer(const FString&) / SetStageState(EUsdStageState) /
 *     GetUsdStage() const / SetUsdStage(const UE::FUsdStage&) /
 *     IsolateLayer(const UE::FSdfLayer&, bool) / GetLevelSequence() const /
 *     GetGeneratedComponent(const FString&) const / GetGeneratedAssets(const FString&) const
 *       (USDStage module, USDStageActor.h; all BlueprintCallable + CallInEditor)
 *   - IUsdStageModule::Get().GetUsdStageActor(UWorld*) / FindUsdStageActor(UWorld*)
 *       (USDStageModule.h)
 *   - EUsdStageState enum { Closed, Opened, OpenedAndLoaded } (USDStageActor.h)
 *   - UE::FSdfLayer::GetIdentifier() const (UnrealUSDWrapper, UsdWrappers/SdfLayer.h)
 *
 * Modules: USDStage (Runtime; requires the USDCore plugin enabled), UnrealUSDWrapper + USDUtilities.
 *
 * IMPORTANT — honest-gated, NOT headless: every command requires an editor world
 * (AUsdStageActor lives in a UWorld). Without one it returns a clean error with note
 * "no editor world", which the cov tests accept through allow_errors=["no editor world"].
 */

// ---- helpers ---------------------------------------------------------
namespace UEAgentUsdStageActorCommands
{

// MCP-DOMAIN: USD

namespace
{
	// Build the standard "no editor world" honest-gate payload. Used by every
	// handler — these commands are NOT headless. The note is a soft marker the
	// cov harness recognizes via allow_errors=["no editor world"].
	TSharedPtr<FJsonObject> NoEditorWorldPayload()
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("success"), false);
		Data->SetStringField(TEXT("error"), TEXT("no editor world"));
		Data->SetStringField(TEXT("note"), TEXT("USD stage-actor commands require a live editor world (AUsdStageActor lives in UWorld); headless mode is not supported"));
		return Data;
	}

	// The required actor_path param: null, or the error reply when it is missing or empty.
	TSharedPtr<FJsonObject> UsdStageActor_ReadActorPath(const TSharedPtr<FJsonObject>& Params, FString& OutActorPath)
	{
		if (!mcp_common::ExtractStringPath(Params, TEXT("actor_path"), OutActorPath))
		{
			return mcp_common::MakeErrorReply(TEXT("Missing required 'actor_path' parameter"));
		}
		return nullptr;
	}

	// Map an EUsdStageState to its JSON token. Used by get_usd_stage_actor_info
	// so the LLM sees "OpenedAndLoaded" rather than the integer value.
	FString StageStateToString(EUsdStageState State)
	{
		switch (State)
		{
			case EUsdStageState::Closed:            return TEXT("Closed");
			case EUsdStageState::Opened:            return TEXT("Opened");
			case EUsdStageState::OpenedAndLoaded:   return TEXT("OpenedAndLoaded");
			default:                               return TEXT("Unknown");
		}
	}
}

// ---------------------------------------------------------------------------
// Static helper implementations
// ---------------------------------------------------------------------------

// Map a token to EUsdStageState. Unknown / empty defaults to OpenedAndLoaded.
// Case-insensitive.
EUsdStageState ParseUsdStageState(const FString& InValue)
{
	const FString V = InValue.ToLower();
	if (V == TEXT("closed"))            { return EUsdStageState::Closed; }
	if (V == TEXT("opened"))            { return EUsdStageState::Opened; }
	if (V == TEXT("openedandloaded"))   { return EUsdStageState::OpenedAndLoaded; }
	// Spec default — same default the spec calls out in the YAML.
	return EUsdStageState::OpenedAndLoaded;
}

// Read a 3-element JSON number array from Params into an FVector / FRotator.
// Missing / short arrays fill with the supplied defaults.

FVector ParseVector3Param(
	const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, const FVector& Default)
{
	if (!Params.IsValid()) { return Default; }
	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	if (!Params->TryGetArrayField(FieldName, Arr) || !Arr || Arr->Num() == 0) { return Default; }
	double X = Default.X, Y = Default.Y, Z = Default.Z;
	if (Arr->Num() >= 1) { (*Arr)[0]->TryGetNumber(X); }
	if (Arr->Num() >= 2) { (*Arr)[1]->TryGetNumber(Y); }
	if (Arr->Num() >= 3) { (*Arr)[2]->TryGetNumber(Z); }
	return FVector(X, Y, Z);
}

FRotator ParseRotator3Param(
	const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, const FRotator& Default)
{
	if (!Params.IsValid()) { return Default; }
	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	if (!Params->TryGetArrayField(FieldName, Arr) || !Arr || Arr->Num() == 0) { return Default; }
	double P = Default.Pitch, Y = Default.Yaw, R = Default.Roll;
	if (Arr->Num() >= 1) { (*Arr)[0]->TryGetNumber(P); }
	if (Arr->Num() >= 2) { (*Arr)[1]->TryGetNumber(Y); }
	if (Arr->Num() >= 3) { (*Arr)[2]->TryGetNumber(R); }
	return FRotator(P, Y, R);
}

// Resolve an AUsdStageActor by its actor_path (the FString form of
// AActor::GetPathName()). Supports both "/Game/Maps/MyMap.MyMap:PersistentLevel.ActorName"
// and the simpler short-name. Returns nullptr on miss and fills OutError.
AUsdStageActor* ResolveStageActor(
	const FString& ActorPath, FString& OutError)
{
	OutError.Empty();

	if (ActorPath.IsEmpty())
	{
		OutError = TEXT("Missing required 'actor_path' parameter");
		return nullptr;
	}

	UWorld* World = mcp_common::GetEditorWorld();
	if (!World)
	{
		OutError = TEXT("no editor world");
		return nullptr;
	}

	// Fast path: full path name via FindObject<>. The bridge stores actor_path
	// in the canonical "/Game/Maps/MyMap.MyMap:PersistentLevel.Actor_2" format.
	// FindObject<UObject>(Outer, Name) requires walking the outer chain; the
	// simpler form FindObject<AUsdStageActor>(nullptr, *Path) works for the
	// full path string form. We also try the short-name path as a fallback
	// for callers that pass just "MyStageActor" after create_usd_stage_actor.
	AUsdStageActor* Found = FindObject<AUsdStageActor>(nullptr, *ActorPath);
	if (Found) { return Found; }

	// Fallback: scan the world with TActorIterator, matching either the FName
	// or the editor label. This is the path exercised by the coverage test
	// flow (create_usd_stage_actor returns short_name from a user param).
	for (TActorIterator<AUsdStageActor> It(World); It; ++It)
	{
		AUsdStageActor* Candidate = *It;
		if (!Candidate) { continue; }
		if (Candidate->GetName() == ActorPath || Candidate->GetActorLabel() == ActorPath)
		{
			return Candidate;
		}
	}

	OutError = FString::Printf(TEXT("AUsdStageActor not found at actor_path '%s'"), *ActorPath);
	return nullptr;
}

// ---------------------------------------------------------------------------
// create_usd_stage_actor
// ---------------------------------------------------------------------------
// MCP:create_usd_stage_actor - Spawn an AUsdStageActor into the editor's current world (or return the existing one).  Each editor world has at most one AUsdStageActor (USDStage is single-instance per world); when one already exists the tool returns already_existed:true with its current actor_path instead of failing. Spawn uses a uniquely-named FName based on the user-supplied name parameter so re-runs with the same name pick up the existing actor cleanly. Requires a live editor world — honest-gated.
//   @param name [string] - Desired actor name (default "UsdStageActor"). Used only on first spawn; ignored on subsequent calls that pick up the existing actor.
//   @param location [array] - World [x, y, z] location for the new actor (optional, default [0, 0, 0]).
//   @param rotation [array] - World [pitch, yaw, roll] in degrees for the new actor (optional, default [0, 0, 0]).
//   @returns { actor_path: string, already_existed: bool, name: string }

TSharedPtr<FJsonObject> HandleCreateUsdStageActor(
	const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = mcp_common::GetEditorWorld();
	if (!World)
	{
		return NoEditorWorldPayload();
	}

	// Idempotency: if an AUsdStageActor is already in this world, return it
	// (idempotent authoring — re-running must not duplicate or corrupt).
	// IUsdStageModule::Get() is the canonical accessor; if the module is not
	// loaded (USDStage module unavailable), the helper returns a null ptr.
	IUsdStageModule& UsdModule = FModuleManager::Get().LoadModuleChecked<IUsdStageModule>(TEXT("USDStage"));
	AUsdStageActor* Existing = UsdModule.FindUsdStageActor(World);
	if (Existing)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("already_existed"), true);
		Data->SetStringField(TEXT("actor_path"), Existing->GetPathName());
		Data->SetStringField(TEXT("name"), Existing->GetName());
		return mcp_common::MakeOkReply(Data);
	}

	// Spawn a new AUsdStageActor.
	FString Name = TEXT("UsdStageActor");
	Params->TryGetStringField(TEXT("name"), Name);
	if (Name.IsEmpty()) { Name = TEXT("UsdStageActor"); }

	const FVector Location = ParseVector3Param(Params, TEXT("location"), FVector::ZeroVector);
	const FRotator Rotation = ParseRotator3Param(Params, TEXT("rotation"), FRotator::ZeroRotator);

	FActorSpawnParameters SpawnParams;
	SpawnParams.Name = MakeUniqueObjectName(
		World->GetCurrentLevel(),
		AUsdStageActor::StaticClass(),
		FName(*Name));
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	const FTransform SpawnXform(Rotation, Location, FVector::ZeroVector);

	AUsdStageActor* Actor = World->SpawnActor<AUsdStageActor>(
		AUsdStageActor::StaticClass(), SpawnXform, SpawnParams);

	if (!Actor)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Failed to spawn AUsdStageActor '%s' in current editor world"), *Name));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("already_existed"), false);
	Data->SetStringField(TEXT("actor_path"), Actor->GetPathName());
	Data->SetStringField(TEXT("name"), Actor->GetName());
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_usd_stage_actor_root_layer
// ---------------------------------------------------------------------------
// MCP:set_usd_stage_actor_root_layer - Assign a USD file as the actor's root layer and transition the stage state.  Wraps AUsdStageActor::SetRootLayer(path) followed by AUsdStageActor::SetStageState(state). SetStageState(OpenedAndLoaded) triggers level-sequence regeneration — this can take a noticeable amount of time on large stages, so the cov harness may observe a transient 'rebuilding' state via follow-up get_usd_stage_actor_info calls. The tool does NOT verify the file exists on disk — SetRootLayer stores the path; the actual load happens during SetStageState(OpenedAndLoaded). When root_layer_path is empty, the existing root layer is preserved.
//   @param actor_path [string!] - Full actor path (or short name) of the AUsdStageActor.
//   @param root_layer_path [string!] - Absolute file path of the USD file (.usd/.usda/.usdc/.usdz).
//   @param stage_state [string] - "OpenedAndLoaded" (default), "Opened", or "Closed".
//   @returns { actor_path: string, root_layer_path: string, stage_state: string }

TSharedPtr<FJsonObject> HandleSetUsdStageActorRootLayer(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ActorPath;
	if (TSharedPtr<FJsonObject> Missing = UsdStageActor_ReadActorPath(Params, ActorPath))
	{
		return Missing;
	}

	FString RootLayerPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("root_layer_path"), RootLayerPath)) { return Missing; }

	FString StageStateStr = TEXT("OpenedAndLoaded");
	Params->TryGetStringField(TEXT("stage_state"), StageStateStr);
	const EUsdStageState State = ParseUsdStageState(StageStateStr);

	FString ResolveError;
	AUsdStageActor* Actor = ResolveStageActor(ActorPath, ResolveError);
	if (!Actor)
	{
		return mcp_common::MakeErrorReply(ResolveError);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_path"), Actor->GetPathName());
	Data->SetStringField(TEXT("root_layer_path"), RootLayerPath);

	// SetRootLayer writes to the UPROPERTY and triggers PostEditChangeProperty
	// (WITH_EDITOR path) which calls back into OpenUsdStage on the editor side.
	// We capture USD errors but do not gate on them — invalid file paths surface
	// as pxr:: warnings rather than hard failures.
	UEAgent::UsdCommon::CaptureUsdErrors(
		[Actor, &RootLayerPath]()
		{
			Actor->SetRootLayer(RootLayerPath);
		},
		Data);

	// SetStageState triggers the level sequence regeneration. On a missing or
	// malformed USD file, this emits pxr:: errors and leaves the stage in a
	// partially-loaded state — reported honestly via the captured errors.
	UEAgent::UsdCommon::CaptureUsdErrors(
		[Actor, State]()
		{
			Actor->SetStageState(State);
		},
		Data);

	Data->SetStringField(TEXT("stage_state"), StageStateToString(State));
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_usd_stage_actor_info
// ---------------------------------------------------------------------------
// MCP:get_usd_stage_actor_info - Read live metadata from an AUsdStageActor.  Returns the root layer identifier + real path from the actor's currently-open UE::FUsdStage (via GetUsdStage()), the integer EUsdStageState (0=Closed, 1=Opened, 2=OpenedAndLoaded), the presence of a generated ULevelSequence (a non-null GetLevelSequence()), and the count of USceneComponents the actor has spawned (via GetComponents()). All read-only.
//   @param actor_path [string!] - Full actor path (or short name) of the AUsdStageActor.
//   @returns { actor_path: string, root_layer: string, real_path: string, stage_state: int, stage_state_name: string, has_level_sequence: bool, generated_component_count: int, has_isolated_stage: bool, errors: [string]? }

TSharedPtr<FJsonObject> HandleGetUsdStageActorInfo(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ActorPath;
	if (TSharedPtr<FJsonObject> Missing = UsdStageActor_ReadActorPath(Params, ActorPath))
	{
		return Missing;
	}

	FString ResolveError;
	AUsdStageActor* Actor = ResolveStageActor(ActorPath, ResolveError);
	if (!Actor)
	{
		return mcp_common::MakeErrorReply(ResolveError);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_path"), Actor->GetPathName());

	// GetUsdStage returns the isolated stage when one is active, else the base
	// stage. Always const&; may be invalid if the actor has no stage loaded.
	const UE::FUsdStage& Stage = Actor->GetUsdStage();

	if (Stage)
	{
		const UE::FSdfLayer RootLayer = Stage.GetRootLayer();
		Data->SetStringField(TEXT("root_layer"), RootLayer.GetIdentifier());
		Data->SetStringField(TEXT("real_path"), RootLayer.GetRealPath());
	}
	else
	{
		Data->SetStringField(TEXT("root_layer"), FString());
		Data->SetStringField(TEXT("real_path"), FString());
	}

	// EUsdStageState is uint8 enum; we cast to int for JSON.
	const EUsdStageState State = Actor->StageState;
	Data->SetNumberField(TEXT("stage_state"), static_cast<int32>(State));
	Data->SetStringField(TEXT("stage_state_name"), StageStateToString(State));

	// GetLevelSequence is the UPROPERTY accessor for the level sequence the
	// actor populates with animated tracks. nullptr means no sequence has been
	// generated yet (e.g. before SetStageState(OpenedAndLoaded) has run).
	const ULevelSequence* LevelSeq = Actor->GetLevelSequence();
	Data->SetBoolField(TEXT("has_level_sequence"), LevelSeq != nullptr);

	// generated_component_count: count of USceneComponents spawned under this
	// actor — the runtime footprint of the opened USD stage. > 0 means the
	// stage is at least partially loaded. Uses GetComponents() (AActor API),
	// not AUsdStageActor::GetGeneratedComponent (which takes a prim path).
	int32 CompCount = 0;
	for (UActorComponent* Comp : Actor->GetComponents())
	{
		if (Comp && Comp->IsA<USceneComponent>())
		{
			++CompCount;
		}
	}
	Data->SetNumberField(TEXT("generated_component_count"), CompCount);

	// has_isolated_stage: GetIsolatedUsdStage() returns the isolated stage only
	// when IsolateLayer() has been called. operator bool() reads as a valid
	// UE::FUsdStage iff an isolated stage is active.
	const UE::FUsdStage& Isolated = Actor->GetIsolatedUsdStage();
	Data->SetBoolField(TEXT("has_isolated_stage"), static_cast<bool>(Isolated));

	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_usd_generated_component
// ---------------------------------------------------------------------------
// MCP:get_usd_generated_component - Resolve a transient USceneComponent generated by the stage actor for a given prim path.  Wraps AUsdStageActor::GetGeneratedComponent(prim_path). Returns present:false (NOT an error) when the path is invalid or the prim has no generated component — the LLM can pivot on this cleanly. When present, also returns component_class (the runtime UClass name).
//   @param actor_path [string!] - Full actor path (or short name) of the AUsdStageActor.
//   @param prim_path [string!] - Full prim path, e.g. "/root_prim/my_prim".
//   @returns { actor_path: string, prim_path: string, present: bool, component_path?: string, component_class?: string }

TSharedPtr<FJsonObject> HandleGetUsdGeneratedComponent(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ActorPath;
	if (TSharedPtr<FJsonObject> Missing = UsdStageActor_ReadActorPath(Params, ActorPath))
	{
		return Missing;
	}

	FString PrimPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("prim_path"), PrimPath)) { return Missing; }

	FString ResolveError;
	AUsdStageActor* Actor = ResolveStageActor(ActorPath, ResolveError);
	if (!Actor)
	{
		return mcp_common::MakeErrorReply(ResolveError);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_path"), Actor->GetPathName());
	Data->SetStringField(TEXT("prim_path"), PrimPath);

	// AUsdStageActor::GetGeneratedComponent returns nullptr for invalid paths
	// and for paths that were collapsed into a parent prim. We treat both as
	// "not present" rather than as errors — the LLM can re-query with the
	// parent path or accept that the prim is collapsed.
	USceneComponent* Comp = Actor->GetGeneratedComponent(PrimPath);
	if (!Comp)
	{
		Data->SetBoolField(TEXT("present"), false);
		return mcp_common::MakeOkReply(Data);
	}

	Data->SetBoolField(TEXT("present"), true);
	Data->SetStringField(TEXT("component_path"), Comp->GetPathName());
	Data->SetStringField(TEXT("component_class"),
		Comp->GetClass() ? Comp->GetClass()->GetName() : FString());
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_usd_generated_assets
// ---------------------------------------------------------------------------
// MCP:get_usd_generated_assets - Resolve the transient assets (UStaticMesh, USkeletalMesh, UMaterial, etc.) generated by the stage actor for a given prim path.  Wraps AUsdStageActor::GetGeneratedAssets(prim_path). Returns an empty assets array (count:0) when the prim has no generated assets — NOT an error. A prim may produce multiple assets (e.g. a skinned mesh produces USkeletalMesh + USkeleton + UPhysicsAsset).
//   @param actor_path [string!] - Full actor path (or short name) of the AUsdStageActor.
//   @param prim_path [string!] - Full prim path, e.g. "/root_prim/my_mesh".
//   @returns { actor_path: string, prim_path: string, assets: [string], count: int }

TSharedPtr<FJsonObject> HandleGetUsdGeneratedAssets(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ActorPath;
	if (TSharedPtr<FJsonObject> Missing = UsdStageActor_ReadActorPath(Params, ActorPath))
	{
		return Missing;
	}

	FString PrimPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("prim_path"), PrimPath)) { return Missing; }

	FString ResolveError;
	AUsdStageActor* Actor = ResolveStageActor(ActorPath, ResolveError);
	if (!Actor)
	{
		return mcp_common::MakeErrorReply(ResolveError);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_path"), Actor->GetPathName());
	Data->SetStringField(TEXT("prim_path"), PrimPath);

	const TArray<UObject*> Assets = Actor->GetGeneratedAssets(PrimPath);

	TArray<TSharedPtr<FJsonValue>> Arr;
	Arr.Reserve(Assets.Num());
	for (UObject* Asset : Assets)
	{
		if (Asset)
		{
			Arr.Add(MakeShared<FJsonValueString>(Asset->GetPathName()));
		}
	}
	Data->SetArrayField(TEXT("assets"), Arr);
	Data->SetNumberField(TEXT("count"), Arr.Num());
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// isolate_usd_layer
// ---------------------------------------------------------------------------
// MCP:isolate_usd_layer - Enter isolated mode by replacing the actor's displayed stage with a new stage rooted at one of the sublayers of the current layer stack.  Wraps AUsdStageActor::IsolateLayer(Layer, b_load_usd_stage). The layer_index selects into Stage.GetLayerStack(true) — index 0 is the root layer, >0 are sublayers. When b_load_usd_stage is true (default) assets, actors and components are regenerated from the isolated stage; when false the isolation is structural only.
//   @param actor_path [string!] - Full actor path (or short name) of the AUsdStageActor.
//   @param layer_index [integer] - Index into the actor's stage layer stack (default 0 = root layer).
//   @param b_load_usd_stage [boolean] - Load assets/components from the isolated stage (default true).
//   @returns { actor_path: string, layer_index: int, isolated_layer: string, b_load_usd_stage: bool }

TSharedPtr<FJsonObject> HandleIsolateUsdLayer(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ActorPath;
	if (TSharedPtr<FJsonObject> Missing = UsdStageActor_ReadActorPath(Params, ActorPath))
	{
		return Missing;
	}

	int32 LayerIndex = 0;
	Params->TryGetNumberField(TEXT("layer_index"), LayerIndex);

	bool bLoadUsdStage = true;
	Params->TryGetBoolField(TEXT("b_load_usd_stage"), bLoadUsdStage);

	FString ResolveError;
	AUsdStageActor* Actor = ResolveStageActor(ActorPath, ResolveError);
	if (!Actor)
	{
		return mcp_common::MakeErrorReply(ResolveError);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_path"), Actor->GetPathName());
	Data->SetNumberField(TEXT("layer_index"), LayerIndex);
	Data->SetBoolField(TEXT("b_load_usd_stage"), bLoadUsdStage);

	// The stage must be open with a valid layer stack before we can index
	// into it. StageState must be Opened or OpenedAndLoaded — the actor's
	// GetUsdStage() is otherwise an invalid stage and the stack is empty.
	const UE::FUsdStage& Stage = Actor->GetUsdStage();
	if (!Stage)
	{
		return UEAgent::UsdCommon::FailReply(Data,
			TEXT("AUsdStageActor has no valid stage -- call set_usd_stage_actor_root_layer first to open a stage"));
	}

	const TArray<UE::FSdfLayer> Stack = Stage.GetLayerStack(/*bIncludeSessionLayers*/ true);
	if (!Stack.IsValidIndex(LayerIndex))
	{
		return UEAgent::UsdCommon::FailReply(Data,
			FString::Printf(TEXT("layer_index %d out of range (layer stack has %d entries)"),
				LayerIndex, Stack.Num()));
	}

	const UE::FSdfLayer& Target = Stack[LayerIndex];
	const FString IsolatedLayerId = Target.GetIdentifier();

	// IsolateLayer creates a brand-new UE::FUsdStage rooted at Target and
	// swaps it into Actor->IsolatedStage. With b_load_usd_stage=true (default)
	// this triggers level sequence + component regen — expensive on large
	// stages but no way around it.
	UEAgent::UsdCommon::CaptureUsdErrors(
		[Actor, &Target, bLoadUsdStage]()
		{
			Actor->IsolateLayer(Target, bLoadUsdStage);
		},
		Data);

	Data->SetStringField(TEXT("isolated_layer"), IsolatedLayerId);
	return mcp_common::MakeOkReply(Data);
}

const McpCommandFamilies::FRegistrar Registration =
{
	{ TEXT("create_usd_stage_actor"), &HandleCreateUsdStageActor },
	{ TEXT("get_usd_generated_assets"), &HandleGetUsdGeneratedAssets },
	{ TEXT("get_usd_generated_component"), &HandleGetUsdGeneratedComponent },
	{ TEXT("get_usd_stage_actor_info"), &HandleGetUsdStageActorInfo },
	{ TEXT("isolate_usd_layer"), &HandleIsolateUsdLayer },
	{ TEXT("set_usd_stage_actor_root_layer"), &HandleSetUsdStageActorRootLayer },
};
} // namespace UEAgentUsdStageActorCommands
