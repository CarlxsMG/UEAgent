// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

// Interchange framework + editor reset APIs.
#include "InterchangeEditorScriptLibrary.h"                 // UInterchangeEditorScriptLibrary::Reset*/CanReset* (INTERCHANGEEDITOR_API)
#include "InterchangeSceneImportAsset.h"                    // UInterchangeSceneImportAsset (INTERCHANGEENGINE_API, MinimalAPI)
#include "Engine/World.h"                                   // UWorld
#include "GameFramework/Actor.h"                            // AActor
#include "UObject/UObjectGlobals.h"                         // LoadObject<UObject>, FindObject<>
#include "UObject/Package.h"                                // UPackage (LoadObject outer resolution)
#include "Editor.h"                                          // GEditor
#include "HAL/IConsoleManager.h"                            // IConsoleManager (honest CVar gate for Reset* silent no-ops)

/**
 * Interchange Editor commands: the reset side of the Interchange Editor framework (UE 5.8).
 *
 * Engine APIs used:
 *
 *   - UInterchangeEditorScriptLibrary
 *       Engine\Plugins\Interchange\Editor\Source\InterchangeEditor\Public\
 *       InterchangeEditorScriptLibrary.h
 *
 *     Signatures (all static UE_API UFUNCTION(BlueprintCallable),
 *     INTERCHANGEEDITOR_API):
 *       * ResetLevelAsset(UWorld* World)
 *         — Performs Interchange Reset on a Level Asset. Internally gated by
 *           the CVar "Interchange.FeatureFlags.InterchangeReset"
 *           (InterchangeEditorScriptLibrary.cpp); no-op if disabled or
 *           if the world has no UInterchangeSceneImportAsset bound.
 *       * ResetSceneImportAsset(UInterchangeSceneImportAsset* SceneImportAsset)
 *         — Performs Interchange Reset on an Interchange Scene Import Asset.
 *           Silently no-ops if the CVar is off
 *           (InterchangeEditorScriptLibrary.cpp).
 *       * ResetActors(TArray<AActor*> Actors)
 *         — Performs Interchange Reset on qualifying actors. Filters
 *           internally by CanResetActor (no-op for non-resettable actors;
 *           InterchangeEditorScriptLibrary.cpp). Idempotent.
 *       * CanResetActor(const AActor* Actor) -> bool
 *         — Read-only; checks if a given actor can be reset.
 *       * CanResetWorld(const UWorld* World) -> bool
 *         — Read-only; checks if a given world can be reset.
 *
 *   - UInterchangeSceneImportAsset
 *       Engine\Source\Runtime\Interchange\Engine\Public\InterchangeSceneImportAsset.h
 *       (UCLASS(MinimalAPI), INTERCHANGEENGINE_API). This is the asset that
 *       backs a level created from an Interchange scene import; the level
 *       reset calls back into it via the LevelFactoryNode in
 *       AssetImportData->GetNodeContainer().
 *
 * Gating contract (gotcha): all Reset* methods silently no-op when
 * "Interchange.FeatureFlags.InterchangeReset" == 0 (default in 5.8). We
 * gate honestly via CanResetWorld / CanResetActor when available, and via
 * a direct CVar lookup for the scene/level cases (the public API only
 * exposes Can* for Actor and World).
 */
namespace UEAgentInterchangeEditorCommands
{

// MCP-DOMAIN: Interchange

namespace
{
    // Resolve an actor by an editor path. In editor contexts actor paths
    // look like:
    //     /Game/Maps/MyMap.MyMap:PersistentLevel.ActorName
    //     /Game/Maps/MyMap.MyMap:PersistentLevel.ActorName_0
    // or simpler short forms like:
    //     ActorName_0     (level-scoped name, outer = PersistentLevel)
    // We try the full path first via FindObject<AActor>, then fall through
    // to StaticFindObject by name in the editor world's PersistentLevel
    // (the conventional MCP-reachable form).
    //
    // Returns nullptr + fills OutError on failure.
    AActor* ResolveActorByPath(const FString& InPath, FString& OutError)
    {
        if (InPath.IsEmpty())
        {
            OutError = TEXT("Missing required 'actor_path' parameter");
            return nullptr;
        }

        // Try direct path resolution first (covers fully-qualified paths).
        if (AActor* Actor = FindObject<AActor>(nullptr, *InPath))
        {
            return Actor;
        }

        // Try the editor world's persistent level as outer (label or short
        // name form). Falls back gracefully — if no editor world exists
        // (e.g. -game), the outer-less StaticFindObject below still gets
        // a shot.
        if (UWorld* EditorWorld = mcp_common::GetEditorWorld())
        {
            if (ULevel* PersistentLevel = EditorWorld->PersistentLevel)
            {
                if (AActor* Actor = FindObject<AActor>(PersistentLevel, *InPath))
                {
                    return Actor;
                }
            }
        }

        OutError = FString::Printf(
            TEXT("Actor not found at '%s' (tried full path + persistent-level short name)"),
            *InPath);
        return nullptr;
    }

    // Reads "world_path" and resolves that world: the asset it names ("/Game/Maps/MyMap"), or the current editor
    // world when it is absent or empty. Null on success, else the error reply.
    TSharedPtr<FJsonObject> Interchange_ReadWorld(const TSharedPtr<FJsonObject>& Params, FString& OutWorldPath,
        UWorld*& OutWorld)
    {
        Params->TryGetStringField(TEXT("world_path"), OutWorldPath);
        if (OutWorldPath.IsEmpty())
        {
            if (!GEditor)
            {
                return mcp_common::MakeErrorReply(TEXT("GEditor not available; cannot resolve current editor world"));
            }
            OutWorld = mcp_common::GetEditorWorld();
            if (!OutWorld)
            {
                return mcp_common::MakeErrorReply(TEXT("Editor world not available (no current map?)"));
            }
            return nullptr;
        }

        // LoadObject<UWorld>(nullptr, *Path) handles both "/Game/Maps/MyMap"
        // (package path -> loads the world) and the fully-qualified object
        // path "/Game/Maps/MyMap.MyMap".
        OutWorld = LoadObject<UWorld>(nullptr, *OutWorldPath);
        if (!OutWorld)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("World not found at '%s'"), *OutWorldPath));
        }
        return nullptr;
    }

    // Honest gate for the CVar
    // "Interchange.FeatureFlags.InterchangeReset". The Reset* methods
    // return void and silently no-op when this CVar is 0 (default in 5.8
    // — InterchangeEditorScriptLibrary.cpp). The CVar is
    // private to InterchangeEditor module init but the lookup itself is
    // via the public IConsoleManager API.
    bool IsInterchangeResetCVarEnabled()
    {
        IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(
            TEXT("Interchange.FeatureFlags.InterchangeReset"));
        return CVar ? CVar->GetBool() : false;
    }
}

// ---------------------------------------------------------------------------
// MCP:can_reset_interchange_actor - Check, without changing anything, whether an Interchange reset (restoring an imported actor to what its Interchange scene import produced) can run on one actor of the editor world (UInterchangeEditorScriptLibrary::CanResetActor). can_reset is true only when the Interchange.FeatureFlags.InterchangeReset console variable is on and a component of the actor carries Interchange asset user data (for a Level Instance, its map's world settings do; that map is loaded to check). false is a normal answer, not an error. Use it before reset_interchange_actors, which skips the actors that fail this check without saying which; for a whole map use can_reset_interchange_world and reset_interchange_level_asset; to see what an asset's import recorded, get_interchange_asset_import_data. An actor that is not found is an error.
//   @param actor_path [string!] - Full object path of the actor (e.g. "/Game/Maps/MyMap.MyMap:PersistentLevel.ActorName_0") or its object name in the persistent level of the editor world (e.g. "StaticMeshActor_3"); labels are not matched.
//   @returns { actor_path: string, can_reset: bool }
TSharedPtr<FJsonObject> HandleInterchangeCanResetActor(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorPath;
    if (!mcp_common::ExtractStringPath(Params, TEXT("actor_path"), ActorPath))
    {
        return mcp_common::MakeErrorReply(
            TEXT("Missing required 'actor_path' parameter"));
    }

    FString ResolveError;
    AActor* Actor = ResolveActorByPath(ActorPath, ResolveError);
    if (!Actor)
    {
        return mcp_common::MakeErrorReply(ResolveError);
    }

    // Read-only. CanResetActor is a static UFUNCTION on the script library
    // (InterchangeEditorScriptLibrary.h). It walks the actor's owning
    // level + checks if there's a UInterchangeSceneImportAsset bound
    // (InterchangeEditorScriptLibrary.cpp).
    const bool bCanReset = UInterchangeEditorScriptLibrary::CanResetActor(Actor);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("actor_path"), ActorPath);
    Data->SetBoolField(TEXT("can_reset"), bCanReset);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// MCP:can_reset_interchange_world - Report, read-only, whether Interchange can reset a world (UInterchangeEditorScriptLibrary::CanResetWorld): the map asset world_path names, loaded if needed, or the current editor world. Returns can_reset, is_editor_world and resolved_world_path. Run the reset with reset_interchange_level_asset.
//   @param world_path [string] - World asset path (e.g. "/Game/Maps/MyMap") or fully-qualified object path. Empty = current editor world.
//   @returns { world_path: string, can_reset: bool, is_editor_world: bool }
TSharedPtr<FJsonObject> HandleInterchangeCanResetWorld(const TSharedPtr<FJsonObject>& Params)
{
    FString WorldPath;
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> Error = Interchange_ReadWorld(Params, WorldPath, World)) { return Error; }

    const bool bIsEditorWorld = (World == mcp_common::GetEditorWorld());
    const bool bCanReset = UInterchangeEditorScriptLibrary::CanResetWorld(World);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("world_path"), bIsEditorWorld ? FString() : WorldPath);
    Data->SetStringField(TEXT("resolved_world_path"), World->GetPathName());
    Data->SetBoolField(TEXT("can_reset"), bCanReset);
    Data->SetBoolField(TEXT("is_editor_world"), bIsEditorWorld);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// MCP:reset_interchange_actors - Run an Interchange reset (UInterchangeEditorScriptLibrary::ResetActors) on the editor-world actors listed by object path or persistent-level object name; the library skips actors that fail CanResetActor, and the reply does not say which were reset. Returns count, resolved and unresolved_paths. Check first with can_reset_interchange_actor.
//   @param actor_paths [array!] - Actors to reset, each a full object path (e.g. "/Game/Maps/MyMap.MyMap:PersistentLevel.ActorName_0") or an object name in the persistent level; labels are not matched, and empty or unknown entries land in unresolved_paths.
//   @returns { count: number, resolved: number, unresolved_paths: [string], note: string }
TSharedPtr<FJsonObject> HandleInterchangeResetActors(const TSharedPtr<FJsonObject>& Params)
{
    const TArray<TSharedPtr<FJsonValue>>* ActorPathsArr = nullptr;
    if (!Params->TryGetArrayField(TEXT("actor_paths"), ActorPathsArr) || !ActorPathsArr)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Missing required 'actor_paths' parameter (array of actor paths)"));
    }

    // Empty array is legal and is a no-op (matches library semantics —
    // ResetActors iterates the empty TArray and does nothing). Report
    // count:0 honestly rather than erroring out.
    TArray<AActor*> Actors;
    TArray<TSharedPtr<FJsonValue>> UnresolvedArr;
    Actors.Reserve(ActorPathsArr->Num());
    UnresolvedArr.Reserve(ActorPathsArr->Num());

    for (const TSharedPtr<FJsonValue>& V : *ActorPathsArr)
    {
        FString Path;
        if (!V->TryGetString(Path) || Path.IsEmpty())
        {
            UnresolvedArr.Add(MakeShared<FJsonValueString>(TEXT("<empty or non-string>")));
            continue;
        }
        FString ResolveError;
        if (AActor* Actor = ResolveActorByPath(Path, ResolveError))
        {
            Actors.Add(Actor);
        }
        else
        {
            UnresolvedArr.Add(MakeShared<FJsonValueString>(Path));
        }
    }

    // The library silently filters by CanResetActor (no-op for actors
    // that don't qualify, see InterchangeEditorScriptLibrary.cpp).
    // We don't pre-filter — we pass everything we resolved and let the
    // library do the work, which is the most honest "this is what I asked
    // for, library decided" contract. Idempotent.
    UInterchangeEditorScriptLibrary::ResetActors(Actors);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetNumberField(TEXT("count"), ActorPathsArr->Num());
    Data->SetNumberField(TEXT("resolved"), Actors.Num());
    Data->SetArrayField(TEXT("unresolved_paths"), UnresolvedArr);
    Data->SetStringField(TEXT("note"),
        TEXT("ResetActors filters internally via CanResetActor; non-resettable actors are silent no-ops"));
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// MCP:reset_interchange_scene_import_asset - Call the Interchange reset of a UInterchangeSceneImportAsset (UInterchangeEditorScriptLibrary::ResetSceneImportAsset); other asset classes are refused. The engine only resets while the Interchange.FeatureFlags.InterchangeReset CVar is 1: with it at 0 the call does nothing and fails (reset false). The reset itself returns nothing, so reset true means it ran, not what it changed. Use reset_interchange_level_asset for a level.
//   @param asset_path [string!] - Asset path of the UInterchangeSceneImportAsset (e.g. "/Game/Imports/MyScene.MyScene")
//   @returns { asset_path: string, reset: bool, cvar_enabled: bool, note: string }
TSharedPtr<FJsonObject> HandleInterchangeResetSceneImportAsset(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_path"), AssetPath)) { return Missing; }

    UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
    if (!Asset)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Asset not found at '%s'"), *AssetPath));
    }

    UInterchangeSceneImportAsset* SceneAsset = Cast<UInterchangeSceneImportAsset>(Asset);
    if (!SceneAsset)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(
                TEXT("Asset at '%s' is not a UInterchangeSceneImportAsset (got class '%s')"),
                *AssetPath,
                Asset->GetClass() ? *Asset->GetClass()->GetName() : TEXT("<null>")));
    }

    // ResetSceneImportAsset returns nothing and silently does nothing while the CVar is off, so
    // the CVar decides whether the reset runs at all: with it off the call changed nothing and fails.
    const bool bCVarEnabled = IsInterchangeResetCVarEnabled();
    if (bCVarEnabled)
    {
        UInterchangeEditorScriptLibrary::ResetSceneImportAsset(SceneAsset);
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), AssetPath);
    Data->SetBoolField(TEXT("reset"), bCVarEnabled);
    Data->SetBoolField(TEXT("cvar_enabled"), bCVarEnabled);
    Data->SetStringField(TEXT("note"),
        bCVarEnabled
            ? TEXT("ResetSceneImportAsset ran; it returns nothing, so what it changed is not reported")
            : TEXT("CVar 'Interchange.FeatureFlags.InterchangeReset' is 0, so the engine resets nothing; set it to 1 and call again."));
    return mcp_common::MakeOutcomeReply(Data, bCVarEnabled, bCVarEnabled ? FString() :
        FString(TEXT("reset_interchange_scene_import_asset did nothing: the Interchange.FeatureFlags.InterchangeReset CVar is 0. Set it to 1 (execute_console_command) and call again.")));
}

// ---------------------------------------------------------------------------
// MCP:reset_interchange_level_asset - Run an Interchange reset on a level (UInterchangeEditorScriptLibrary::ResetLevelAsset), the map world_path names or the current editor world, only when the Interchange.FeatureFlags.InterchangeReset CVar is on and CanResetWorld is true; otherwise it does nothing and the call fails (reset false, with the reason in note). It does not save. Returns reset, can_reset, cvar_enabled and a note.
//   @param world_path [string] - World asset path (e.g. "/Game/Maps/MyMap") or fully-qualified object path. Empty = current editor world.
//   @returns { world_path: string, resolved_world_path: string, reset: bool, can_reset: bool, cvar_enabled: bool, note: string }
TSharedPtr<FJsonObject> HandleInterchangeResetLevelAsset(const TSharedPtr<FJsonObject>& Params)
{
    FString WorldPath;
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> Error = Interchange_ReadWorld(Params, WorldPath, World)) { return Error; }

    // Same honest gate as the scene variant — ResetLevelAsset returns
    // void and silently no-ops when the CVar is off or when the world
    // has no UInterchangeSceneImportAsset bound (InterchangeEditorScriptLibrary.cpp).
    const bool bCVarEnabled = IsInterchangeResetCVarEnabled();
    const bool bCanReset = UInterchangeEditorScriptLibrary::CanResetWorld(World);

    if (bCVarEnabled && bCanReset)
    {
        UInterchangeEditorScriptLibrary::ResetLevelAsset(World);
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("world_path"),
        (World == mcp_common::GetEditorWorld()) ? FString() : WorldPath);
    Data->SetStringField(TEXT("resolved_world_path"), World->GetPathName());
    Data->SetBoolField(TEXT("reset"), bCVarEnabled && bCanReset);
    Data->SetBoolField(TEXT("can_reset"), bCanReset);
    Data->SetBoolField(TEXT("cvar_enabled"), bCVarEnabled);
    Data->SetStringField(TEXT("note"),
        !bCVarEnabled
            ? TEXT("CVar 'Interchange.FeatureFlags.InterchangeReset' is 0; library silently no-ops. Set the CVar to 1 to enable.")
            : !bCanReset
                ? TEXT("World is not bound to a UInterchangeSceneImportAsset; ResetLevelAsset would be a no-op. Skipped.")
                : TEXT("ResetLevelAsset invoked on bound world"));
    const bool bReset = bCVarEnabled && bCanReset;
    return mcp_common::MakeOutcomeReply(Data, bReset, bReset ? FString() : FString::Printf(
        TEXT("reset_interchange_level_asset did nothing: %s"), *Data->GetStringField(TEXT("note"))));
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("can_reset_interchange_actor"), &HandleInterchangeCanResetActor },
    { TEXT("can_reset_interchange_world"), &HandleInterchangeCanResetWorld },
    { TEXT("reset_interchange_actors"), &HandleInterchangeResetActors },
    { TEXT("reset_interchange_level_asset"), &HandleInterchangeResetLevelAsset },
    { TEXT("reset_interchange_scene_import_asset"), &HandleInterchangeResetSceneImportAsset },
};
} // namespace UEAgentInterchangeEditorCommands
