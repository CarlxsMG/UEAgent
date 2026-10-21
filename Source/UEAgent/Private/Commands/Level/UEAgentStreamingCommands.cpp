// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

#include "Editor.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "Engine/LevelStreamingDynamic.h"
#include "Engine/LevelStreamingVolume.h"
#include "GameFramework/Volume.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "ScopedTransaction.h"

// Editor-only sub-level detach (UnrealEd module — already a private dependency).
#include "EditorLevelUtils.h"

#define LOCTEXT_NAMESPACE "UEAgentStreaming"

/**
 * Streaming-level commands: the streaming sub-levels of the editor world's persistent map
 * (UWorld::GetStreamingLevels) and the level streaming volumes bound to them.
 *
 * Engine APIs: UWorld::AddStreamingLevel / RemoveStreamingLevel; ULevelStreaming::SetShouldBeLoaded /
 * SetShouldBeVisible / GetWorldAssetPackageName / IsLevelLoaded / IsLevelVisible; ULevelStreamingDynamic
 * for the sub-levels added here. A loaded sub-level is removed with UEditorLevelUtils::RemoveLevelFromWorld
 * (UnrealEd, editor only).
 */
namespace UEAgentStreamingCommands
{

// MCP-DOMAIN: Level

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/** Normalise a user-supplied level path to a bare package name ("/Game/Foo"). */
FString NormalizePackageName(const FString& InPath)
{
    FString Path = InPath;
    Path.TrimStartAndEndInline();
    if (Path.IsEmpty())
    {
        return Path;
    }

    // If the caller passed an object path ("/Game/Foo.Foo"), reduce it to the
    // package name ("/Game/Foo"). ObjectPathToPackageName is a no-op for a bare
    // package name.
    Path = FPackageName::ObjectPathToPackageName(Path);

    // Strip a trailing slash if any.
    while (Path.Len() > 1 && Path.EndsWith(TEXT("/")))
    {
        Path.LeftChopInline(1);
    }
    return Path;
}

/** Find a streaming level in World by package name (case-insensitive, tolerant of
 *  ObjectPath / short-name input). Returns nullptr if not present. */
ULevelStreaming* FindStreamingLevelByPackageName(UWorld* World, const FString& PackageName)
{
    if (!World)
    {
        return nullptr;
    }

    const FString Target = NormalizePackageName(PackageName);
    const FString TargetLeaf = FPackageName::GetLongPackageAssetName(Target);

    for (ULevelStreaming* StreamingLevel : World->GetStreamingLevels())
    {
        if (!StreamingLevel)
        {
            continue;
        }

        const FString PkgName = StreamingLevel->GetWorldAssetPackageName();

        // Full package-name match (most common), or, when the caller passed a
        // short name, match on the leaf asset name.
        if (PkgName.Equals(Target, ESearchCase::IgnoreCase))
        {
            return StreamingLevel;
        }
        if (!TargetLeaf.IsEmpty() &&
            FPackageName::GetLongPackageAssetName(PkgName).Equals(TargetLeaf, ESearchCase::IgnoreCase))
        {
            return StreamingLevel;
        }
    }
    return nullptr;
}

/** The editor world and its streaming level LevelPackagePath; the error reply when either is missing. */
TSharedPtr<FJsonObject> FindEditorStreamingLevel(const FString& LevelPackagePath, UWorld*& OutWorld,
    ULevelStreaming*& OutStreamingLevel)
{
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(OutWorld))
    {
        return NoWorld;
    }

    OutStreamingLevel = FindStreamingLevelByPackageName(OutWorld, LevelPackagePath);
    if (!OutStreamingLevel)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Streaming level not found in world: %s"), *NormalizePackageName(LevelPackagePath)));
    }
    return nullptr;
}

/** Serialise one ULevelStreaming to a JSON object. */
TSharedPtr<FJsonObject> StreamingLevelToJson(ULevelStreaming* StreamingLevel)
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    if (!StreamingLevel)
    {
        return Obj;
    }

    const FString PackageName = StreamingLevel->GetWorldAssetPackageName();
    Obj->SetStringField(TEXT("package_name"), PackageName);
    Obj->SetStringField(TEXT("name"), StreamingLevel->GetName());
    Obj->SetStringField(TEXT("streaming_class"), StreamingLevel->GetClass()->GetName());

    // ShouldBeLoaded() is the virtual that reflects the requested loaded state.
    Obj->SetBoolField(TEXT("should_be_loaded"), StreamingLevel->ShouldBeLoaded());
    // GetShouldBeVisibleFlag() returns the raw requested-visible flag (works even
    // when the level is not yet loaded). ShouldBeVisible() additionally requires
    // ShouldBeLoaded() so we expose the raw flag for a stable read.
    Obj->SetBoolField(TEXT("should_be_visible"), StreamingLevel->GetShouldBeVisibleFlag());
    Obj->SetBoolField(TEXT("is_level_loaded"), StreamingLevel->IsLevelLoaded());
    Obj->SetBoolField(TEXT("is_level_visible"), StreamingLevel->IsLevelVisible());
    return Obj;
}

// ---------------------------------------------------------------------------
// add_streaming_level
// ---------------------------------------------------------------------------

// MCP:add_streaming_level - Adds a saved level package to the editor world as a streaming sub-level.  Creates a ULevelStreamingDynamic wrapper referencing the given level package and attaches it to the current editor world's StreamingLevels array. The package must already exist on disk (a streaming level references a saved .umap). Idempotent: if the level is already a streaming level, returns its current state with added=false.          (an object path "/Game/Maps/SubLevel.SubLevel" is also accepted).
//   @param level_package_path - Package path of the level, e.g. "/Game/Maps/SubLevel"
//   @param should_be_loaded - Desired initial ShouldBeLoaded state (optional, default true)
//   @param should_be_visible - Desired initial ShouldBeVisible state (optional, default true)
//   @returns { package_name: string, name: string, streaming_class: string, should_be_loaded: bool, should_be_visible: bool, is_level_loaded: bool, is_level_visible: bool, added: bool, already_present: bool, streaming_level_count: number }

TSharedPtr<FJsonObject> HandleAddStreamingLevel(const TSharedPtr<FJsonObject>& Params)
{
    FString LevelPackagePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("level_package_path"), LevelPackagePath)) { return Missing; }

    bool bShouldBeLoaded = true;
    Params->TryGetBoolField(TEXT("should_be_loaded"), bShouldBeLoaded);
    bool bShouldBeVisible = true;
    Params->TryGetBoolField(TEXT("should_be_visible"), bShouldBeVisible);

    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World)) { return NoWorld; }
    if (!World->PersistentLevel)
    {
        return mcp_common::MakeErrorReply(TEXT("Editor world has no persistent level"));
    }

    const FString PackageName = NormalizePackageName(LevelPackagePath);

    // The package must exist on disk: a streaming level references a saved .umap.
    if (!FPackageName::DoesPackageExist(PackageName))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Level package does not exist on disk: %s (save a level there first)"), *PackageName));
    }

    // The persistent level itself cannot be added as a streaming sub-level.
    if (World->PersistentLevel->GetOutermost() &&
        World->PersistentLevel->GetOutermost()->GetName().Equals(PackageName, ESearchCase::IgnoreCase))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Cannot add the persistent level (%s) as a streaming sub-level"), *PackageName));
    }

    // Idempotent: if it's already a streaming level just return its current state.
    if (ULevelStreaming* Existing = FindStreamingLevelByPackageName(World, PackageName))
    {
        TSharedPtr<FJsonObject> Data = StreamingLevelToJson(Existing);
        Data->SetBoolField(TEXT("added"), false);
        Data->SetBoolField(TEXT("already_present"), true);
        Data->SetNumberField(TEXT("streaming_level_count"), World->GetStreamingLevels().Num());
        return mcp_common::MakeOkReply(Data);
    }

    const FScopedTransaction Transaction(LOCTEXT("AddStreamingLevel", "Add Streaming Level"));
    World->Modify();

    // Construct the streaming-level wrapper directly (mirrors the body of
    // UEditorLevelUtils::AddLevelToWorld_Internal) so we avoid that function's
    // headless hazards (FScopedSlowTask UI + FSuppressableWarningDialog::ShowModal
    // on an already-present level).
    ULevelStreaming* StreamingLevel = NewObject<ULevelStreaming>(
        World, ULevelStreamingDynamic::StaticClass(), NAME_None, RF_Transactional, nullptr);
    if (!StreamingLevel)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to create ULevelStreamingDynamic object"));
    }

    StreamingLevel->SetWorldAssetByPackageName(FName(*PackageName));
    StreamingLevel->LevelTransform = FTransform::Identity;
    StreamingLevel->SetShouldBeLoaded(bShouldBeLoaded);
    StreamingLevel->SetShouldBeVisible(bShouldBeVisible);
    StreamingLevel->LevelColor = FLinearColor::MakeRandomColor();

    World->AddStreamingLevel(StreamingLevel);

    // Refresh just the new level so the editor re-associates it (editor-only).
    {
        TArray<ULevelStreaming*> LevelsForRefresh;
        LevelsForRefresh.Add(StreamingLevel);
        World->RefreshStreamingLevels(LevelsForRefresh);
    }

    if (!StreamingLevel->HasAnyFlags(RF_Transient))
    {
        World->MarkPackageDirty();
    }
    World->BroadcastLevelsChanged();

    TSharedPtr<FJsonObject> Data = StreamingLevelToJson(StreamingLevel);
    Data->SetBoolField(TEXT("added"), true);
    Data->SetBoolField(TEXT("already_present"), false);
    Data->SetNumberField(TEXT("streaming_level_count"), World->GetStreamingLevels().Num());
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_streaming_levels
// ---------------------------------------------------------------------------

// MCP:get_streaming_levels - Lists every streaming sub-level of the current editor world.  Reads UWorld::GetStreamingLevels(); works even when the world has zero streaming levels (returns an empty array).
//   @returns { streaming_levels: [{ package_name: string, name: string, streaming_class: string, should_be_loaded: bool, should_be_visible: bool, is_level_loaded: bool, is_level_visible: bool }], streaming_level_count: number, world_package: string }

TSharedPtr<FJsonObject> HandleGetStreamingLevels(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World)) { return NoWorld; }

    TArray<TSharedPtr<FJsonValue>> LevelsArray;
    for (ULevelStreaming* StreamingLevel : World->GetStreamingLevels())
    {
        if (!StreamingLevel)
        {
            continue;
        }
        LevelsArray.Add(MakeShared<FJsonValueObject>(StreamingLevelToJson(StreamingLevel)));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("streaming_levels"), LevelsArray);
    Data->SetNumberField(TEXT("streaming_level_count"), LevelsArray.Num());

    FString WorldPackage;
    if (World->GetOutermost())
    {
        WorldPackage = World->GetOutermost()->GetName();
    }
    Data->SetStringField(TEXT("world_package"), WorldPackage);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_streaming_level_loaded
// ---------------------------------------------------------------------------

// MCP:set_streaming_level_loaded - Set the should-be-loaded request flag of a streaming sub-level of the editor world (ULevelStreaming::SetShouldBeLoaded), found by package path, object path or short name (case-insensitive), inside an undo transaction that marks the map dirty without saving. It is a request: this call streams nothing in or out itself, so is_level_loaded may not change. Only a sub-level class that stores the flag honors it, such as ULevelStreamingDynamic (what add_streaming_level creates); an always-loaded sub-level keeps reporting should_be_loaded true, so asking to unload it gives applied false (applied compares ShouldBeLoaded with loaded). Repeating it leaves the same state, though each call still adds an undo step. An unknown level is an error. Visibility: set_streaming_level_visible; current state: get_streaming_levels; detaching a sub-level: remove_streaming_level.
//   @param level_package_path - Package path of an existing streaming sub-level, e.g. "/Game/Maps/SubLevel"; an object path or the bare level name also matches.
//   @param loaded - Desired should-be-loaded state; required.
//   @returns { package_name: string, name: string, streaming_class: string, should_be_loaded: bool, should_be_visible: bool, is_level_loaded: bool, is_level_visible: bool, requested_loaded: bool, applied: bool }

TSharedPtr<FJsonObject> HandleSetStreamingLevelLoaded(const TSharedPtr<FJsonObject>& Params)
{
    FString LevelPackagePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("level_package_path"), LevelPackagePath)) { return Missing; }

    bool bLoaded = true;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredBool(Params, TEXT("loaded"), bLoaded)) { return Missing; }

    UWorld* World = nullptr;
    ULevelStreaming* StreamingLevel = nullptr;
    if (TSharedPtr<FJsonObject> Failure = FindEditorStreamingLevel(LevelPackagePath, World, StreamingLevel))
    {
        return Failure;
    }

    const FScopedTransaction Transaction(LOCTEXT("SetStreamingLevelLoaded", "Set Streaming Level Loaded"));
    StreamingLevel->Modify();
    StreamingLevel->SetShouldBeLoaded(bLoaded);
    World->BroadcastLevelsChanged();

    // Read the applied value back (ShouldBeLoaded() is the authoritative virtual).
    TSharedPtr<FJsonObject> Data = StreamingLevelToJson(StreamingLevel);
    Data->SetBoolField(TEXT("requested_loaded"), bLoaded);
    Data->SetBoolField(TEXT("applied"), StreamingLevel->ShouldBeLoaded() == bLoaded);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_streaming_level_visible
// ---------------------------------------------------------------------------

// MCP:set_streaming_level_visible - Set the game-time should-be-visible request flag (ULevelStreaming bShouldBeVisible) of a streaming sub-level of the editor world, found by package path or short name, inside an undo transaction. It is a request, not an immediate change, and the editor viewport follows the level's separate editor-visibility flag, so is_level_visible may not change. Marks the map dirty without saving; an unknown level is an error. applied compares the flag with visible. Loading: set_streaming_level_loaded; current state: get_streaming_levels; adding a sub-level: add_streaming_level.
//   @param level_package_path - Package path of an existing streaming sub-level.
//   @param visible - Desired should-be-visible state.
//   @returns { package_name: string, name: string, streaming_class: string, should_be_loaded: bool, should_be_visible: bool, is_level_loaded: bool, is_level_visible: bool, requested_visible: bool, applied: bool }

TSharedPtr<FJsonObject> HandleSetStreamingLevelVisible(const TSharedPtr<FJsonObject>& Params)
{
    FString LevelPackagePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("level_package_path"), LevelPackagePath)) { return Missing; }

    bool bVisible = true;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredBool(Params, TEXT("visible"), bVisible)) { return Missing; }

    UWorld* World = nullptr;
    ULevelStreaming* StreamingLevel = nullptr;
    if (TSharedPtr<FJsonObject> Failure = FindEditorStreamingLevel(LevelPackagePath, World, StreamingLevel))
    {
        return Failure;
    }

    const FScopedTransaction Transaction(LOCTEXT("SetStreamingLevelVisible", "Set Streaming Level Visible"));
    StreamingLevel->Modify();
    StreamingLevel->SetShouldBeVisible(bVisible);
    World->BroadcastLevelsChanged();

    // GetShouldBeVisibleFlag() reads the raw requested-visible flag we just set.
    TSharedPtr<FJsonObject> Data = StreamingLevelToJson(StreamingLevel);
    Data->SetBoolField(TEXT("requested_visible"), bVisible);
    Data->SetBoolField(TEXT("applied"), StreamingLevel->GetShouldBeVisibleFlag() == bVisible);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// remove_streaming_level
// ---------------------------------------------------------------------------

// MCP:remove_streaming_level - Removes a streaming sub-level from the current editor world.  If the sub-level is loaded, uses UEditorLevelUtils::RemoveLevelFromWorld; otherwise detaches the streaming wrapper via UWorld::RemoveStreamingLevel. The result is verified by re-querying the world's streaming levels.
//   @param level_package_path - Package path of an existing streaming sub-level.
//   @returns { package_name: string, removed: bool, method: string, streaming_level_count: number }

TSharedPtr<FJsonObject> HandleRemoveStreamingLevel(const TSharedPtr<FJsonObject>& Params)
{
    FString LevelPackagePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("level_package_path"), LevelPackagePath)) { return Missing; }

    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World)) { return NoWorld; }

    const FString PackageName = NormalizePackageName(LevelPackagePath);
    ULevelStreaming* StreamingLevel = FindStreamingLevelByPackageName(World, PackageName);
    if (!StreamingLevel)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Streaming level not found in world: %s"), *PackageName));
    }

    const FScopedTransaction Transaction(LOCTEXT("RemoveStreamingLevel", "Remove Streaming Level"));
    World->Modify();

    bool bRemoved = false;
    FString Method;

    // If the sub-level is actually loaded into the world, the editor utility is the
    // clean path (it tears down the ULevel and unloads the package). Otherwise we
    // just drop the streaming wrapper from the world's array.
    ULevel* LoadedLevel = StreamingLevel->GetLoadedLevel();
    if (LoadedLevel && !LoadedLevel->IsPersistentLevel())
    {
        bRemoved = UEditorLevelUtils::RemoveLevelFromWorld(LoadedLevel, /*bClearSelection*/ true, /*bResetTransBuffer*/ false);
        Method = TEXT("RemoveLevelFromWorld");
    }

    if (!bRemoved)
    {
        // Fallback (and the normal path for an unloaded streaming level): request
        // unload and detach the streaming wrapper from the world.
        StreamingLevel->SetShouldBeLoaded(false);
        StreamingLevel->SetShouldBeVisible(false);
        bRemoved = World->RemoveStreamingLevel(StreamingLevel);
        if (Method.IsEmpty())
        {
            Method = TEXT("RemoveStreamingLevel");
        }
        World->RefreshStreamingLevels();
    }

    World->MarkPackageDirty();
    World->BroadcastLevelsChanged();

    // Verify by re-querying the world's streaming levels.
    const bool bStillPresent = (FindStreamingLevelByPackageName(World, PackageName) != nullptr);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("package_name"), PackageName);
    Data->SetBoolField(TEXT("removed"), bRemoved && !bStillPresent);
    Data->SetStringField(TEXT("method"), Method);
    Data->SetNumberField(TEXT("streaming_level_count"), World->GetStreamingLevels().Num());
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// spawn_level_streaming_volume
// ALevelStreamingVolume is an AVolume (Runtime/Engine/Classes/Engine/LevelStreamingVolume.h),
// so like every brush-based volume it must be given a box brush
// (mcp_common::BuildCubeBrushForVolume, as spawn_nav_modifier_volume does). The volume
// is then bound to an existing streaming sub-level by appending it to that level's
// ULevelStreaming::EditorStreamingVolumes array (public UPROPERTY, no #if guard). The
// derived Volume->StreamingLevelNames array and Volume->UpdateStreamingLevelsRefs() are
// intentionally NOT touched: the former is rebuilt from the binding, and the latter has no
// export macro (calling it would be an LNK2019). The load/unload effect itself is a
// runtime/PIE behaviour driven by a viewpoint entering the volume, so headless this is an
// honest data-only creation (see the "note" field).
// ---------------------------------------------------------------------------
// MCP:spawn_level_streaming_volume - Spawn an ALevelStreamingVolume in the editor world with a real box brush (half-extent extent) and bind it to an existing streaming sub-level, setting its usage, editor_pre_vis_only and disabled flags, in one undo step. Over spawn_box_volume(volume_class='LevelStreamingVolume') it adds the binding: the volume is appended to the sub-level's EditorStreamingVolumes, which no other tool writes. Each call spawns a NEW volume; an extent axis of 0 or less becomes 100; an unknown usage or sub-level is an error before anything spawns. Not saved (save_level). The load/unload itself happens only at PIE/runtime, when a viewpoint enters the volume.
//   @param streaming_level_name - Package path (or short name) of an existing streaming sub-level to bind the volume to.
//   @param location - [x, y, z] world location for the volume (default [0,0,0]).
//   @param extent - Optional [x, y, z] box half-extent (default [100,100,100]); each axis is clamped > 0.
//   @param usage - Optional EStreamingVolumeUsage: one of Loading, LoadingAndVisibility (default), VisibilityBlockingOnLoad, BlockingOnLoad, LoadingNotVisible.
//   @param editor_pre_vis_only - Optional bEditorPreVisOnly flag (default false).
//   @param disabled - Optional bDisabled flag (default false).
//   @param actor_label - Optional editor label for the spawned volume.
//   @returns { actor_name, actor_label, actor_class, brush_built, streaming_level, usage, bound, already_bound, streaming_level_names: [string], success, note }
TSharedPtr<FJsonObject> HandleSpawnLevelStreamingVolume(const TSharedPtr<FJsonObject>& Params)
{
    FString StreamingLevelName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("streaming_level_name"), StreamingLevelName)) { return Missing; }

    // The volume is useless without a level to bind to: resolve it up-front and fail
    // honestly if the caller named a level the world does not stream.
    UWorld* World = nullptr;
    ULevelStreaming* StreamingLevel = nullptr;
    if (TSharedPtr<FJsonObject> Failure = FindEditorStreamingLevel(StreamingLevelName, World, StreamingLevel))
    {
        return Failure;
    }

    const FVector Location = mcp_common::ReadJsonVector(Params, TEXT("location"));

    // Optional half-extent; defaults to 100uu half-extent -> 200uu full box. Clamp each
    // axis strictly > 0 so the cube builder always gets a valid, non-degenerate size.
    FVector Extent(100.0, 100.0, 100.0);
    if (Params->HasField(TEXT("extent")))
    {
        Extent = mcp_common::ReadJsonVector(Params, TEXT("extent"));
        if (Extent.X <= 0.0) Extent.X = 100.0;
        if (Extent.Y <= 0.0) Extent.Y = 100.0;
        if (Extent.Z <= 0.0) Extent.Z = 100.0;
    }

    // Optional usage; defaults to SVB_LoadingAndVisibility. Reject SVB_MAX (a sentinel,
    // not a real usage).
    EStreamingVolumeUsage Usage = SVB_LoadingAndVisibility;
    FString UsageStr;
    if (Params->TryGetStringField(TEXT("usage"), UsageStr) && !UsageStr.IsEmpty())
    {
        const FString U = UsageStr.TrimStartAndEnd();
        if      (U.Equals(TEXT("Loading"), ESearchCase::IgnoreCase))                   { Usage = SVB_Loading; }
        else if (U.Equals(TEXT("LoadingAndVisibility"), ESearchCase::IgnoreCase))      { Usage = SVB_LoadingAndVisibility; }
        else if (U.Equals(TEXT("VisibilityBlockingOnLoad"), ESearchCase::IgnoreCase))  { Usage = SVB_VisibilityBlockingOnLoad; }
        else if (U.Equals(TEXT("BlockingOnLoad"), ESearchCase::IgnoreCase))            { Usage = SVB_BlockingOnLoad; }
        else if (U.Equals(TEXT("LoadingNotVisible"), ESearchCase::IgnoreCase))         { Usage = SVB_LoadingNotVisible; }
        else
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Invalid usage '%s' (expected Loading | LoadingAndVisibility | VisibilityBlockingOnLoad | BlockingOnLoad | LoadingNotVisible)"), *UsageStr));
        }
    }

    bool bEditorPreVisOnly = false;
    Params->TryGetBoolField(TEXT("editor_pre_vis_only"), bEditorPreVisOnly);
    bool bDisabled = false;
    Params->TryGetBoolField(TEXT("disabled"), bDisabled);

    const FScopedTransaction Transaction(LOCTEXT("SpawnLevelStreamingVolume", "Spawn Level Streaming Volume"));

    AActor* NewActor = World->SpawnActor<ALevelStreamingVolume>(Location, FRotator::ZeroRotator);
    if (!NewActor)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to spawn ALevelStreamingVolume actor"));
    }

    ALevelStreamingVolume* Volume = Cast<ALevelStreamingVolume>(NewActor);
    if (!Volume)
    {
        return mcp_common::MakeErrorReply(TEXT("Spawned actor is not an ALevelStreamingVolume"));
    }

    // Build a default box brush so the volume has real geometry/bounds (a brush-based
    // volume spawns with no brush).
    const bool bBrushBuilt = mcp_common::BuildCubeBrushForVolume(NewActor, Extent);

    // Apply the public configuration fields directly (all public UPROPERTYs).
    Volume->Modify();
    Volume->StreamingUsage = Usage;
    Volume->bEditorPreVisOnly = bEditorPreVisOnly ? 1 : 0;
    Volume->bDisabled = bDisabled ? 1 : 0;

    FString ActorLabel;
    if (Params->TryGetStringField(TEXT("actor_label"), ActorLabel) && !ActorLabel.IsEmpty())
    {
        Volume->SetActorLabel(ActorLabel);
    }

    // Bind the volume to the streaming level (idempotent). EditorStreamingVolumes is the
    // editor-side source of truth; the volume's own StreamingLevelNames is derived from
    // this and is NOT written here.
    StreamingLevel->Modify();
    bool bAlreadyBound = false;
    bool bBound = false;
    if (StreamingLevel->EditorStreamingVolumes.Contains(Volume))
    {
        bAlreadyBound = true;
        bBound = true;
    }
    else
    {
        StreamingLevel->EditorStreamingVolumes.Add(Volume);
        bBound = StreamingLevel->EditorStreamingVolumes.Contains(Volume);
    }

    World->BroadcastLevelsChanged();

    // Read back the (engine-maintained) derived StreamingLevelNames for honest reporting.
    TArray<TSharedPtr<FJsonValue>> NamesArray;
    for (const FName& LevelName : Volume->StreamingLevelNames)
    {
        NamesArray.Add(MakeShared<FJsonValueString>(LevelName.ToString()));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("actor_name"), Volume->GetName());
    ResultObj->SetStringField(TEXT("actor_label"), Volume->GetActorLabel());
    ResultObj->SetStringField(TEXT("actor_class"), Volume->GetClass()->GetName());
    ResultObj->SetBoolField(TEXT("brush_built"), bBrushBuilt);
    ResultObj->SetStringField(TEXT("streaming_level"), StreamingLevel->GetWorldAssetPackageName());
    ResultObj->SetStringField(TEXT("usage"), UsageStr.IsEmpty() ? TEXT("LoadingAndVisibility") : UsageStr.TrimStartAndEnd());
    ResultObj->SetBoolField(TEXT("bound"), bBound);
    ResultObj->SetBoolField(TEXT("already_bound"), bAlreadyBound);
    ResultObj->SetArrayField(TEXT("streaming_level_names"), NamesArray);
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetStringField(TEXT("note"),
        TEXT("Volume + binding created (data-only). The actual load/unload trigger runs at PIE/runtime when a viewpoint enters the volume; not exercised headless."));

    return mcp_common::MakeOkReply(ResultObj);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_streaming_level"), &HandleAddStreamingLevel },
    { TEXT("get_streaming_levels"), &HandleGetStreamingLevels },
    { TEXT("remove_streaming_level"), &HandleRemoveStreamingLevel },
    { TEXT("set_streaming_level_loaded"), &HandleSetStreamingLevelLoaded },
    { TEXT("set_streaming_level_visible"), &HandleSetStreamingLevelVisible },
    { TEXT("spawn_level_streaming_volume"), &HandleSpawnLevelStreamingVolume },
};
} // namespace UEAgentStreamingCommands

#undef LOCTEXT_NAMESPACE
